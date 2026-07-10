/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *  Copyright (c) 2007 The FFmpeg Project
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include "connectivity/ip_utils.h" // MUST BE INCLUDED FIRST
#include "libav_deps.h"            // THEN THIS ONE AFTER

#include "socket_pair.h"
#include "libav_utils.h"
#include "logger.h"
#include "connectivity/security/memory.h"

#include <iostream>
#include <string>
#include <algorithm>
#include <iterator>
#include <random>
#include <sstream>

extern "C" {
#include "srtp.h"
}

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <sys/types.h>
#include <ciso646> // fix windows compiler bug

#ifdef _WIN32
#define SOCK_NONBLOCK FIONBIO
#define poll          WSAPoll
#define close(x)      closesocket(x)
#endif

#ifdef __ANDROID__
#include <asm-generic/fcntl.h>
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#ifdef __APPLE__
#include <fcntl.h>
#endif

// Swap 2 byte, 16 bit values:
#define Swap2Bytes(val) ((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))

// Swap 4 byte, 32 bit values:
#define Swap4Bytes(val) \
    ((((val) >> 24) & 0x000000FF) | (((val) >> 8) & 0x0000FF00) | (((val) << 8) & 0x00FF0000) \
     | (((val) << 24) & 0xFF000000))

// Swap 8 byte, 64 bit values:
#define Swap8Bytes(val) \
    ((((val) >> 56) & 0x00000000000000FF) | (((val) >> 40) & 0x000000000000FF00) \
     | (((val) >> 24) & 0x0000000000FF0000) | (((val) >> 8) & 0x00000000FF000000) \
     | (((val) << 8) & 0x000000FF00000000) | (((val) << 24) & 0x0000FF0000000000) \
     | (((val) << 40) & 0x00FF000000000000) | (((val) << 56) & 0xFF00000000000000))

namespace sip_core {

static constexpr int NET_POLL_TIMEOUT = 100; /* poll() timeout in ms */
static constexpr int RTP_MAX_PACKET_LENGTH = 2048;
static constexpr auto UDP_HEADER_SIZE = 8;
static constexpr auto SRTP_OVERHEAD = 10;
static constexpr uint32_t RTCP_RR_FRACTION_MASK = 0xFF000000;
static constexpr unsigned MINIMUM_RTP_HEADER_SIZE = 16;

enum class DataType : unsigned { RTP = 1 << 0, RTCP = 1 << 1 };

class SRTPProtoContext
{
public:
    SRTPProtoContext(const char* out_suite,
                     const char* out_key,
                     const char* in_suite,
                     const char* in_key)
    {
        ring_secure_memzero(&srtp_out, sizeof(srtp_out));
        ring_secure_memzero(&srtp_in, sizeof(srtp_in));
        if (out_suite && out_key) {
            // XXX: see srtp_open from libavformat/srtpproto.c
            if (ff_srtp_set_crypto(&srtp_out, out_suite, out_key) < 0) {
                srtp_close();
                throw std::runtime_error("Could not set crypto on output");
            }
        }

        if (in_suite && in_key) {
            if (ff_srtp_set_crypto(&srtp_in, in_suite, in_key) < 0) {
                srtp_close();
                throw std::runtime_error("Could not set crypto on input");
            }
        }
    }

    ~SRTPProtoContext() { srtp_close(); }

    SRTPContext srtp_out {};
    SRTPContext srtp_in {};
    uint8_t encryptbuf[RTP_MAX_PACKET_LENGTH];

private:
    void srtp_close() noexcept
    {
        ff_srtp_free(&srtp_out);
        ff_srtp_free(&srtp_in);
    }
};

static int
ff_network_wait_fd(int fd)
{
    struct pollfd p = {fd, POLLOUT, 0};
    auto ret = poll(&p, 1, NET_POLL_TIMEOUT);
    return ret < 0 ? errno : p.revents & (POLLOUT | POLLERR | POLLHUP) ? 0 : -EAGAIN;
}

static int
create_nonblocking_udp_socket(int family)
{
    int udp_fd = -1;

#ifdef __APPLE__
    udp_fd = socket(family, SOCK_DGRAM, 0);
    if (udp_fd >= 0 && fcntl(udp_fd, F_SETFL, O_NONBLOCK) < 0) {
        close(udp_fd);
        udp_fd = -1;
    }
#elif defined _WIN32
    udp_fd = socket(family, SOCK_DGRAM, 0);
    u_long block = 1;
    if (udp_fd >= 0 && ioctlsocket(udp_fd, FIONBIO, &block) < 0) {
        close(udp_fd);
        udp_fd = -1;
    }
#else
    udp_fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
#endif

    if (udp_fd < 0) {
        SIP_CORE_ERR("socket() failed");
        strErr();
    }

    return udp_fd;
}

static void
close_socket_handle(int& handle) noexcept
{
    if (handle >= 0) {
        if (close(handle))
            strErr();
        handle = -1;
    }
}

static int
last_socket_error_code()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static std::string
last_socket_error_message(int err)
{
#ifdef _WIN32
    return "winsock error";
#else
    return std::strerror(err);
#endif
}

static bool
bind_udp_socket(int handle, int family, uint16_t port, const char* mediaKind, const char* component)
{
    auto bind_addr = ip_utils::getAnyHostAddr(family);
    if (not bind_addr.isIpv4() and not bind_addr.isIpv6()) {
        SIP_CORE_ERR("[%s] No IPv4/IPv6 wildcard host found for family %u", mediaKind, family);
        return false;
    }

    bind_addr.setPort(port);
    SIP_CORE_DBG("[%s] trying %s socket on local address %s",
                 mediaKind,
                 component,
                 bind_addr.toString(true, true).c_str());

    if (::bind(handle, bind_addr, bind_addr.getLength()) < 0) {
        const auto err = last_socket_error_code();
        SIP_CORE_WARN("[%s] bind failed for %s socket %s: (%d) %s",
                      mediaKind,
                      component,
                      bind_addr.toString(true, true).c_str(),
                      err,
                      last_socket_error_message(err).c_str());
        return false;
    }

    return true;
}

static std::mutex&
reservation_mutex()
{
    static std::mutex mutex;
    return mutex;
}

static std::mt19937&
reservation_rng()
{
    static std::mt19937 rng {std::random_device {}()};
    return rng;
}

ReservedSocketPair::ReservedSocketPair(
    uint16_t family, int rtpHandle, int rtcpHandle, uint16_t rtpPort, uint16_t rtcpPort) noexcept
    : family_(family)
    , rtpHandle_(rtpHandle)
    , rtcpHandle_(rtcpHandle)
    , rtpPort_(rtpPort)
    , rtcpPort_(rtcpPort)
{}

ReservedSocketPair::~ReservedSocketPair()
{
    reset();
}

ReservedSocketPair::ReservedSocketPair(ReservedSocketPair&& other) noexcept
{
    *this = std::move(other);
}

ReservedSocketPair&
ReservedSocketPair::operator=(ReservedSocketPair&& other) noexcept
{
    if (this == &other)
        return *this;

    reset();
    family_ = other.family_;
    rtpHandle_ = other.rtpHandle_;
    rtcpHandle_ = other.rtcpHandle_;
    rtpPort_ = other.rtpPort_;
    rtcpPort_ = other.rtcpPort_;

    other.family_ = AF_UNSPEC;
    other.rtpHandle_ = -1;
    other.rtcpHandle_ = -1;
    other.rtpPort_ = 0;
    other.rtcpPort_ = 0;
    return *this;
}

bool
ReservedSocketPair::valid() const noexcept
{
    return (family_ == AF_INET || family_ == AF_INET6) && rtpHandle_ >= 0 && rtcpHandle_ >= 0
           && rtpPort_ != 0 && rtcpPort_ != 0;
}

void
ReservedSocketPair::reset() noexcept
{
    close_socket_handle(rtpHandle_);
    close_socket_handle(rtcpHandle_);
    family_ = AF_UNSPEC;
    rtpPort_ = 0;
    rtcpPort_ = 0;
}

int
ReservedSocketPair::releaseRtpHandle() noexcept
{
    auto handle = rtpHandle_;
    rtpHandle_ = -1;
    return handle;
}

int
ReservedSocketPair::releaseRtcpHandle() noexcept
{
    auto handle = rtcpHandle_;
    rtcpHandle_ = -1;
    return handle;
}

ReservedSocketPair
reserveSocketPairInRange(uint16_t family,
                         const std::pair<uint16_t, uint16_t>& range,
                         const char* mediaKind)
{
    if (family != AF_INET && family != AF_INET6) {
        throw std::runtime_error("Unsupported RTP socket family");
    }

    if (range.first == 0 || range.second <= range.first) {
        throw std::runtime_error("Invalid RTP port range");
    }

    const uint16_t firstCandidate = (range.first % 2 == 0) ? range.first : range.first + 1;
    const uint16_t upperBound = static_cast<uint16_t>(range.second - 1);
    const uint16_t lastCandidate = (upperBound % 2 == 0) ? upperBound
                                                         : static_cast<uint16_t>(upperBound - 1);

    if (firstCandidate > lastCandidate) {
        std::ostringstream oss;
        oss << "No free " << mediaKind << " RTP/RTCP pair available in configured range ["
            << range.first << "-" << range.second << "]";
        SIP_CORE_ERR("%s", oss.str().c_str());
        throw std::runtime_error(oss.str());
    }
    const auto pairCount = static_cast<uint32_t>(((lastCandidate - firstCandidate) / 2u) + 1u);

    std::lock_guard<std::mutex> lk(reservation_mutex());
    std::uniform_int_distribution<uint32_t> startDist(0, pairCount - 1u);
    const auto startIndex = startDist(reservation_rng());

    for (uint32_t attempt = 0; attempt < pairCount; ++attempt) {
        const auto candidateIndex = (startIndex + attempt) % pairCount;
        const auto rtpPort = static_cast<uint16_t>(firstCandidate + candidateIndex * 2u);
        const auto rtcpPort = static_cast<uint16_t>(rtpPort + 1);

        int rtpHandle = create_nonblocking_udp_socket(family);
        if (rtpHandle < 0) {
            throw std::runtime_error("Failed to create RTP socket");
        }

        if (!bind_udp_socket(rtpHandle, family, rtpPort, mediaKind, "RTP")) {
            close_socket_handle(rtpHandle);
            continue;
        }

        int rtcpHandle = create_nonblocking_udp_socket(family);
        if (rtcpHandle < 0) {
            close_socket_handle(rtpHandle);
            throw std::runtime_error("Failed to create RTCP socket");
        }

        if (!bind_udp_socket(rtcpHandle, family, rtcpPort, mediaKind, "RTCP")) {
            close_socket_handle(rtpHandle);
            close_socket_handle(rtcpHandle);
            continue;
        }

        SIP_CORE_WARN("[%s] reserved local RTP/RTCP ports %u/%u", mediaKind, rtpPort, rtcpPort);
        return ReservedSocketPair {family, rtpHandle, rtcpHandle, rtpPort, rtcpPort};
    }

    std::ostringstream oss;
    oss << "No free " << mediaKind << " RTP/RTCP pair available in configured range ["
        << range.first << "-" << range.second << "]";
    SIP_CORE_ERR("%s", oss.str().c_str());
    throw std::runtime_error(oss.str());
}

SocketPair::SocketPair(const char* uri, ReservedSocketPair&& reserved)
{
    openSockets(uri, std::move(reserved));
}

SocketPair::~SocketPair()
{
    interrupt();
    closeSockets();
    SIP_CORE_DBG("[%p] Instance destroyed", this);
}

bool
SocketPair::waitForRTCP(std::chrono::seconds interval)
{
    std::unique_lock<std::mutex> lock(rtcpInfo_mutex_);
    return cvRtcpPacketReadyToRead_.wait_for(lock, interval, [this] {
        return interrupted_ or not listRtcpRRHeader_.empty() or not listRtcpREMBHeader_.empty()
               or not readBlockingMode_;
    });
}

void
SocketPair::saveRtcpRRPacket(uint8_t* buf, size_t len)
{
    if (len < sizeof(rtcpRRHeader))
        return;

    auto header = reinterpret_cast<rtcpRRHeader*>(buf);
    if (header->pt != 201) // 201 = RR PT
        return;

    storeValidatedRR(*header);
}

void
SocketPair::storeValidatedRR(const rtcpRRHeader& header)
{
    // Only accept report blocks that are about OUR outgoing stream. This
    // filters SRTCP ciphertext (we cannot decrypt SRTCP, so a compliant
    // secure peer's reports would parse as garbage) and reports meant for
    // other SSRCs. Skipped while our SSRC is still unknown (receive-only).
    auto ourSsrc = ourOutSsrc_.load(std::memory_order_relaxed);
    if (ourSsrc != 0 and Swap4Bytes(header.id) != ourSsrc) {
        SIP_CORE_DBG("Dropping RTCP report block about SSRC %x (ours is %x)",
                     Swap4Bytes(header.id),
                     ourSsrc);
        return;
    }

    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);

    if (listRtcpRRHeader_.size() >= MAX_LIST_SIZE) {
        listRtcpRRHeader_.pop_front();
    }

    lastRtcpRRHeader_ = header;
    listRtcpRRHeader_.emplace_back(header);

    cvRtcpPacketReadyToRead_.notify_one();
}

void
SocketPair::handleIncomingSR(uint8_t* buf, size_t len)
{
    if (len < sizeof(rtcpSRHeader))
        return;

    auto sr = reinterpret_cast<rtcpSRHeader*>(buf);

    // Middle 32 bits of the peer's NTP timestamp -> LSR of our next RR;
    // arrival time -> DLSR (RFC 3550 §6.4.1).
    uint32_t msb = Swap4Bytes(sr->timestampMSB);
    uint32_t lsb = Swap4Bytes(sr->timestampLSB);
    peerSrNtpMid_ = (msb << 16) | (lsb >> 16);
    peerSrArrival_ = clock::now();

    // A compound SR from a standards-compliant peer may carry report blocks
    // about our own sending; treat the first block like an RR so the
    // loss-based bitrate adaptation can consume it.
    constexpr size_t reportBlockSize = 24;
    if (sr->rc > 0 and len >= sizeof(rtcpSRHeader) + reportBlockSize) {
        rtcpRRHeader rr {};
        rr.version = sr->version;
        rr.rc = sr->rc;
        rr.pt = 201;
        rr.len = sr->len;
        rr.ssrc = sr->ssrc;
        // Wire layout of a report block matches rtcpRRHeader from `id` on.
        memcpy(&rr.id, buf + sizeof(rtcpSRHeader), reportBlockSize);
        storeValidatedRR(rr);
    }
}

void
SocketPair::saveRtcpREMBPacket(uint8_t* buf, size_t len)
{
    if (len < sizeof(rtcpREMBHeader))
        return;

    auto header = reinterpret_cast<rtcpREMBHeader*>(buf);
    if (header->pt != 206) // 206 = REMB PT
        return;

    if (header->uid != 0x424D4552) // uid must be "REMB"
        return;

    // Decode the carried estimate directly from the wire bytes — the struct
    // bitfields are byte-order-scrambled on little-endian hosts.
    // Byte 16 = NumSSRC, byte 17 = BRExp(6) | mantissa hi 2 bits, 18-19 = mantissa.
    if (len >= 20) {
        uint8_t expo = buf[17] >> 2;
        uint32_t mant = (uint32_t(buf[17] & 0x3) << 16) | (uint32_t(buf[18]) << 8) | buf[19];
        lastRembBps_.store(uint64_t(mant) << expo, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);

    if (listRtcpREMBHeader_.size() >= MAX_LIST_SIZE) {
        listRtcpREMBHeader_.pop_front();
    }

    lastRtcpREMBHeader_ = *header;
    listRtcpREMBHeader_.push_back(*header);

    cvRtcpPacketReadyToRead_.notify_one();
}

std::list<rtcpRRHeader>
SocketPair::getRtcpRR()
{
    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);
    return std::move(listRtcpRRHeader_);
}

std::list<rtcpREMBHeader>
SocketPair::getRtcpREMB()
{
    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);
    return std::move(listRtcpREMBHeader_);
}

rtcpSRHeader
SocketPair::getLastRtcpSR()
{
    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);
    return lastRtcpSRHeader_;
}

rtcpREMBHeader
SocketPair::getLastRtcpREMB()
{
    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);
    return lastRtcpREMBHeader_;
}

rtcpRRHeader
SocketPair::getLastRtcpRR()
{
    std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);
    return lastRtcpRRHeader_;
}

void
SocketPair::createSRTP(const char* out_suite,
                       const char* out_key,
                       const char* in_suite,
                       const char* in_key)
{
    srtpContext_.reset(new SRTPProtoContext(out_suite, out_key, in_suite, in_key));
}

void
SocketPair::interrupt()
{
    if (interrupted_.exchange(true))
        return;
    SIP_CORE_WARN("[%p] Interrupting RTP sockets", this);
    cv_.notify_all();
    cvRtcpPacketReadyToRead_.notify_all();
}

void
SocketPair::setReadBlockingMode(bool block)
{
    SIP_CORE_DBG("[%p] Read operations in blocking mode [%s]", this, block ? "YES" : "NO");
    readBlockingMode_ = block;
    cv_.notify_all();
    cvRtcpPacketReadyToRead_.notify_all();
}

void
SocketPair::stopSendOp(bool state)
{
    noWrite_ = state;
}

void
SocketPair::flushReadQueue()
{
    // System-socket path: drain the kernel UDP receive buffers for both
    // RTP and RTCP. The sockets are already created in non-blocking mode
    // (SOCK_NONBLOCK on Linux/Android, F_SETFL O_NONBLOCK on Apple,
    // FIONBIO on Windows), so recvfrom() will return -1/EAGAIN once the
    // buffer is empty. We discard everything we read.
    if (rtpHandle_ >= 0 || rtcpHandle_ >= 0) {
        char drainBuf[RTP_MAX_PACKET_LENGTH];
        struct sockaddr_storage from;
        socklen_t from_len;
        unsigned drainedRtp = 0;
        unsigned drainedRtcp = 0;
        // Cap the drain loops so a flooded socket cannot stall the caller.
        constexpr unsigned MAX_DRAIN_PACKETS = 4096;
        if (rtpHandle_ >= 0) {
            for (; drainedRtp < MAX_DRAIN_PACKETS; ++drainedRtp) {
                from_len = sizeof(from);
                int n = recvfrom(rtpHandle_,
                                 drainBuf,
                                 sizeof(drainBuf),
                                 0,
                                 reinterpret_cast<struct sockaddr*>(&from),
                                 &from_len);
                if (n <= 0) {
                    break;
                }
            }
        }
        if (rtcpHandle_ >= 0) {
            for (; drainedRtcp < MAX_DRAIN_PACKETS; ++drainedRtcp) {
                from_len = sizeof(from);
                int n = recvfrom(rtcpHandle_,
                                 drainBuf,
                                 sizeof(drainBuf),
                                 0,
                                 reinterpret_cast<struct sockaddr*>(&from),
                                 &from_len);
                if (n <= 0) {
                    break;
                }
            }
        }
        if (drainedRtp || drainedRtcp) {
            SIP_CORE_DBG("[%p] flushReadQueue drained %u RTP + %u RTCP datagrams",
                         this,
                         drainedRtp,
                         drainedRtcp);
        }
        return;
    }

    // ICE / non-system-socket path: clear the in-memory queues.
    std::unique_lock<std::mutex> lk(dataBuffMutex_);
    const auto rtpCount = rtpDataBuff_.size();
    const auto rtcpCount = rtcpDataBuff_.size();
    rtpDataBuff_.clear();
    rtcpDataBuff_.clear();
    if (rtpCount || rtcpCount) {
        SIP_CORE_DBG("[%p] flushReadQueue cleared %zu RTP + %zu RTCP queued ICE datagrams",
                     this,
                     rtpCount,
                     rtcpCount);
    }
}

void
SocketPair::closeSockets()
{
    close_socket_handle(rtcpHandle_);
    close_socket_handle(rtpHandle_);
    localFamily_ = AF_UNSPEC;
    localRtpPort_ = 0;
    localRtcpPort_ = 0;
}

void
SocketPair::openSockets(const char* uri, ReservedSocketPair&& reserved)
{
    if (!reserved) {
        throw std::runtime_error("Reserved socket pair is invalid");
    }

    SIP_CORE_DBG("Creating rtp socket for uri %s using reserved local ports %u/%u",
                 uri,
                 reserved.rtpPort(),
                 reserved.rtcpPort());

    char hostname[256];
    char path[1024];
    int dst_rtp_port;

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &dst_rtp_port, path, sizeof(path), uri);

    const auto local_rtp_port = reserved.rtpPort();
    const auto local_rtcp_port = reserved.rtcpPort();
    const int dst_rtcp_port = dst_rtp_port + 1;

    rtpDestAddr_ = IpAddr {hostname};
    rtpDestAddr_.setPort(dst_rtp_port);
    rtcpDestAddr_ = IpAddr {hostname};
    rtcpDestAddr_.setPort(dst_rtcp_port);

    if (rtpDestAddr_.getFamily() != reserved.family()) {
        SIP_CORE_ERR("[%p] Reserved socket family %u does not match remote RTP family %u",
                     this,
                     reserved.family(),
                     rtpDestAddr_.getFamily());
        throw std::runtime_error("Reserved socket family mismatch");
    }

    rtpHandle_ = reserved.releaseRtpHandle();
    rtcpHandle_ = reserved.releaseRtcpHandle();
    localFamily_ = reserved.family();
    localRtpPort_ = local_rtp_port;
    localRtcpPort_ = local_rtcp_port;

    SIP_CORE_WARN("SocketPair: local{%d,%d} / %s{%d,%d}",
                  local_rtp_port,
                  local_rtcp_port,
                  hostname,
                  dst_rtp_port,
                  dst_rtcp_port);
}

ReservedSocketPair
SocketPair::releaseLocalReservation() noexcept
{
    if ((localFamily_ != AF_INET && localFamily_ != AF_INET6) || rtpHandle_ < 0 || rtcpHandle_ < 0
        || localRtpPort_ == 0 || localRtcpPort_ == 0) {
        return {};
    }

    auto reserved
        = ReservedSocketPair {localFamily_, rtpHandle_, rtcpHandle_, localRtpPort_, localRtcpPort_};
    rtpHandle_ = -1;
    rtcpHandle_ = -1;
    localFamily_ = AF_UNSPEC;
    localRtpPort_ = 0;
    localRtcpPort_ = 0;
    return reserved;
}

MediaIOHandle*
SocketPair::createIOContext(const uint16_t mtu)
{
    unsigned ip_header_size;
    if (rtpDestAddr_.getFamily() == AF_INET6)
        ip_header_size = 40;
    else
        ip_header_size = 20;
    return new MediaIOHandle(
        mtu - (srtpContext_ ? SRTP_OVERHEAD : 0) - UDP_HEADER_SIZE - ip_header_size,
        true,
        [](void* sp, uint8_t* buf, int len) {
            return static_cast<SocketPair*>(sp)->readCallback(buf, len);
        },
        [](void* sp, const uint8_t* buf, int len) {
            return static_cast<SocketPair*>(sp)->writeCallback(buf, len);
        },
        0,
        reinterpret_cast<void*>(this));
}

int
SocketPair::waitForData()
{
    // System sockets
    if (rtpHandle_ >= 0) {
        int ret;
        do {
            if (interrupted_) {
                errno = EINTR;
                return -1;
            }

            if (not readBlockingMode_) {
                return 0;
            }

            // work with system socket
            struct pollfd p[2] = {{rtpHandle_, POLLIN, 0}, {rtcpHandle_, POLLIN, 0}};
            ret = poll(p, 2, NET_POLL_TIMEOUT);
            if (ret > 0) {
                ret = 0;
                if (p[0].revents & POLLIN)
                    ret |= static_cast<int>(DataType::RTP);
                if (p[1].revents & POLLIN)
                    ret |= static_cast<int>(DataType::RTCP);
            }
        } while (!ret or (ret < 0 and errno == EAGAIN));

        return ret;
    }

    {
        std::unique_lock<std::mutex> lk(dataBuffMutex_);
        cv_.wait(lk, [this] {
            return interrupted_ or not rtpDataBuff_.empty() or not rtcpDataBuff_.empty()
                   or not readBlockingMode_;
        });
    }

    if (interrupted_) {
        errno = EINTR;
        return -1;
    }

    return static_cast<int>(DataType::RTP) | static_cast<int>(DataType::RTCP);
}

int
SocketPair::readRtpData(void* buf, int buf_size)
{
    // handle system socket
    if (rtpHandle_ >= 0) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        return recvfrom(rtpHandle_,
                        static_cast<char*>(buf),
                        buf_size,
                        0,
                        reinterpret_cast<struct sockaddr*>(&from),
                        &from_len);
    }

    // handle ICE
    std::unique_lock<std::mutex> lk(dataBuffMutex_);
    if (not rtpDataBuff_.empty()) {
        auto pkt = std::move(rtpDataBuff_.front());
        rtpDataBuff_.pop_front();
        lk.unlock(); // to not block our ICE callbacks
        int pkt_size = pkt.size();
        int len = std::min(pkt_size, buf_size);
        std::copy_n(pkt.begin(), len, static_cast<char*>(buf));
        return len;
    }

    return 0;
}

int
SocketPair::readRtcpData(void* buf, int buf_size)
{
    // handle system socket
    if (rtcpHandle_ >= 0) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        return recvfrom(rtcpHandle_,
                        static_cast<char*>(buf),
                        buf_size,
                        0,
                        reinterpret_cast<struct sockaddr*>(&from),
                        &from_len);
    }

    // handle ICE
    std::unique_lock<std::mutex> lk(dataBuffMutex_);
    if (not rtcpDataBuff_.empty()) {
        auto pkt = std::move(rtcpDataBuff_.front());
        rtcpDataBuff_.pop_front();
        lk.unlock();
        int pkt_size = pkt.size();
        int len = std::min(pkt_size, buf_size);
        std::copy_n(pkt.begin(), len, static_cast<char*>(buf));
        return len;
    }

    return 0;
}

int
SocketPair::readCallback(uint8_t* buf, int buf_size)
{
    auto datatype = waitForData();
    if (datatype < 0)
        return datatype;

    int len = 0;
    bool fromRTCP = false;

    if (datatype & static_cast<int>(DataType::RTCP)) {
        len = readRtcpData(buf, buf_size);
        if (len > 0) {
            auto header = reinterpret_cast<rtcpRRHeader*>(buf);
            // 201 = RR PT
            if (header->pt == 201 && static_cast<size_t>(len) >= sizeof(rtcpRRHeader)) {
                lastDLSR_.store(Swap4Bytes(header->dlsr), std::memory_order_relaxed);
                lastRR_time = std::chrono::steady_clock::now();
                saveRtcpRRPacket(buf, len);
            }
            // 206 = REMB PT
            else if (header->pt == 206)
                saveRtcpREMBPacket(buf, len);
            // 200 = SR PT
            else if (header->pt == 200) {
                handleIncomingSR(buf, len);
            } else {
                SIP_CORE_DBG("Can't read RTCP: unknown packet type %u", header->pt);
            }
            fromRTCP = true;
        }
    }

    // No RTCP... try RTP
    if (!len and (datatype & static_cast<int>(DataType::RTP))) {
        len = readRtpData(buf, buf_size);
        fromRTCP = false;
    }

    if (len <= 0)
        return len;

    if (not fromRTCP && (buf_size < static_cast<int>(MINIMUM_RTP_HEADER_SIZE)))
        return len;

    // RFC 3550 receive statistics + periodic Receiver Reports. RTP headers
    // are cleartext even under SRTP, so this runs before decryption and for
    // plain RTP alike. Enabled per media type via enableRtcpReports().
    if (not fromRTCP and rtcpReportClockRate_.load(std::memory_order_relaxed) != 0)
        processIncomingRtpStats(buf, len);

    // SRTP decrypt
    if (not fromRTCP and srtpContext_ and srtpContext_->srtp_in.aes) {
        int32_t gradient = 0;
        int32_t deltaT = 0;
        float abs = 0.0f;
        bool res_parse = false;
        bool res_delay = false;

        res_parse = parse_RTP_ext(buf, &abs);
        bool marker = (buf[1] & 0x80) >> 7;

        if (res_parse)
            res_delay = getOneWayDelayGradient(abs, marker, &gradient, &deltaT);

        // rtpDelayCallback_ is not set for audio
        if (rtpDelayCallback_ and res_delay)
            rtpDelayCallback_(gradient, deltaT);

        auto err = ff_srtp_decrypt(&srtpContext_->srtp_in, buf, &len);
        if (packetLossCallback_ and (buf[2] << 8 | buf[3]) != lastSeqNumIn_ + 1)
            packetLossCallback_();
        lastSeqNumIn_ = buf[2] << 8 | buf[3];
        if (err < 0)
            SIP_CORE_WARN("decrypt error %d", err);
    }

    if (len != 0)
        return len;
    else
        return AVERROR_EOF;
}

int
SocketPair::writeData(const uint8_t* buf, int buf_size)
{
    bool isRTCP = RTP_PT_IS_RTCP(buf[1]);

    // System sockets?
    if (rtpHandle_ >= 0) {
        int fd;
        IpAddr* dest_addr;

        if (isRTCP) {
            fd = rtcpHandle_;
            dest_addr = &rtcpDestAddr_;
        } else {
            fd = rtpHandle_;
            dest_addr = &rtpDestAddr_;
        }

        auto ret = ff_network_wait_fd(fd);
        if (ret < 0)
            return ret;

        if (noWrite_)
            return buf_size;
        return ::sendto(fd,
                        reinterpret_cast<const char*>(buf),
                        buf_size,
                        0,
                        *dest_addr,
                        dest_addr->getLength());
    }

    if (noWrite_)
        return buf_size;

    return 0;
}

int 
SocketPair::readData(uint8_t* buf, int buf_size)
{
    auto datatype = waitForData();
    if (datatype < 0)
        return datatype;

    if (datatype & static_cast<int>(DataType::RTP))
        return readDataNoBlock(buf, buf_size);
    
    return 0;
}


int
SocketPair::readDataNoBlock(uint8_t* buf, int buf_size)
{
    int len = readRtpData(buf, buf_size);
    if (len <= 0)
        return len;

    if (buf_size < static_cast<int>(MINIMUM_RTP_HEADER_SIZE))
        return len;

    // SRTP decrypt
    if (srtpContext_ and srtpContext_->srtp_in.aes) {
        int32_t gradient = 0;
        int32_t deltaT = 0;
        float abs = 0.0f;
        bool res_parse = false;
        bool res_delay = false;

        res_parse = parse_RTP_ext(buf, &abs);
        bool marker = (buf[1] & 0x80) >> 7;

        if (res_parse)
            res_delay = getOneWayDelayGradient(abs, marker, &gradient, &deltaT);

        // rtpDelayCallback_ is not set for audio
        if (rtpDelayCallback_ and res_delay)
            rtpDelayCallback_(gradient, deltaT);

        auto err = ff_srtp_decrypt(&srtpContext_->srtp_in, buf, &len);
        if (packetLossCallback_ and (buf[2] << 8 | buf[3]) != lastSeqNumIn_ + 1)
            packetLossCallback_();
        lastSeqNumIn_ = buf[2] << 8 | buf[3];
        if (err < 0)
            SIP_CORE_WARN("decrypt error %d", err);
    }

    if (len != 0)
        return len;
    else
        return AVERROR_EOF;
}


int
SocketPair::writeCallback(const uint8_t* buf, int buf_size)
{
    if (noWrite_)
        return 0;

    int ret;
    bool isRTCP = RTP_PT_IS_RTCP(buf[1]);

    // Track our outgoing RTP SSRC (it changes whenever the FFmpeg muxer is
    // recreated, e.g. on a sender restart): it is the reporter identity of
    // our Receiver Reports and the validation key for inbound report blocks.
    if (not isRTCP and buf_size >= 12) {
        uint32_t ssrc = (uint32_t(buf[8]) << 24) | (uint32_t(buf[9]) << 16)
                        | (uint32_t(buf[10]) << 8) | uint32_t(buf[11]);
        ourOutSsrc_.store(ssrc, std::memory_order_relaxed);
    }
    unsigned int ts_LSB, ts_MSB;
    double currentSRTS, currentLatency;

    // Encrypt?
    if (not isRTCP and srtpContext_ and srtpContext_->srtp_out.aes) {
        buf_size = ff_srtp_encrypt(&srtpContext_->srtp_out,
                                   buf,
                                   buf_size,
                                   srtpContext_->encryptbuf,
                                   sizeof(srtpContext_->encryptbuf));
        if (buf_size < 0) {
            SIP_CORE_WARN("encrypt error %d", buf_size);
            return buf_size;
        }

        buf = srtpContext_->encryptbuf;
    }

    // check if we're sending an RR, if so, detect packet loss
    // buf_size gives length of buffer, not just header
    if (isRTCP && static_cast<unsigned>(buf_size) >= sizeof(rtcpRRHeader)) {
        auto header = reinterpret_cast<const rtcpRRHeader*>(buf);
        rtcpPacketLoss_ = (header->pt == 201
                           && ntohl(header->fraction_lost) & RTCP_RR_FRACTION_MASK);
    }

    do {
        if (interrupted_)
            return -EINTR;
        ret = writeData(buf, buf_size);
    } while (ret < 0 and errno == EAGAIN);

    if (buf[1] == 200) // Sender Report
    {
        auto header = reinterpret_cast<const rtcpSRHeader*>(buf);
        ts_LSB = Swap4Bytes(header->timestampLSB);
        ts_MSB = Swap4Bytes(header->timestampMSB);

        currentSRTS = ts_MSB + (ts_LSB / pow(2, 32));

        {
            std::lock_guard<std::mutex> lock(latencyMutex_);
            if (lastSRTS_ != 0 && lastDLSR_.load(std::memory_order_relaxed) != 0) {
                if (histoLatency_.size() >= MAX_LIST_SIZE)
                    histoLatency_.pop_front();

                currentLatency = (currentSRTS - lastSRTS_) / 2;
                histoLatency_.push_back(currentLatency);
            }

            lastSRTS_ = currentSRTS;
        }

        std::lock_guard<std::mutex> lock(rtcpInfo_mutex_);
        lastRtcpSRHeader_ = *header;

        // SIP_CORE_WARN("SENDING NEW RTCP SR !! ");

    } else if (buf[1] == 201) // Receiver Report
    {
        // auto header = reinterpret_cast<rtcpRRHeader*>(buf);
        // SIP_CORE_WARN("SENDING NEW RTCP RR !! ");
    }

    return ret < 0 ? -errno : ret;
}

double
SocketPair::getLastLatency()
{
    std::lock_guard<std::mutex> lock(latencyMutex_);
    if (not histoLatency_.empty())
        return histoLatency_.back();
    else
        return -1;
}

void
SocketPair::enableRtcpReports(uint32_t rtpClockRate)
{
    rtcpReportClockRate_.store(rtpClockRate, std::memory_order_relaxed);
}

void
SocketPair::processIncomingRtpStats(uint8_t* buf, int len)
{
    // Cleartext RTP header: V(2) P X CC | M PT | seq(16) | ts(32) | ssrc(32)
    if (len < 12 or (buf[0] >> 6) != 2)
        return;

    const uint16_t seq = uint16_t(buf[2]) << 8 | buf[3];
    const uint32_t rtpTs = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16)
                           | (uint32_t(buf[6]) << 8) | uint32_t(buf[7]);
    const uint32_t ssrc = (uint32_t(buf[8]) << 24) | (uint32_t(buf[9]) << 16)
                          | (uint32_t(buf[10]) << 8) | uint32_t(buf[11]);
    const auto now = clock::now();

    if (not seqInit_ or ssrc != remoteSsrc_) {
        // First packet, or the peer restarted its sender (fresh SSRC):
        // (re)base the whole statistics block (RFC 3550 A.1 init).
        remoteSsrc_ = ssrc;
        seqInit_ = true;
        maxSeq_ = seq;
        seqCycles_ = 0;
        baseSeqExt_ = seq;
        receivedPkts_ = 1;
        expectedPrior_ = 0;
        receivedPrior_ = 0;
        jitterQ4_ = 0;
        transitInit_ = false;
        rateWindowBytes_ = static_cast<uint64_t>(len);
        rateWindowStart_ = now;
        lastRRSent_ = now;
        return;
    }

    receivedPkts_++;
    rateWindowBytes_ += static_cast<uint64_t>(len);

    const uint16_t udelta = static_cast<uint16_t>(seq - maxSeq_);
    if (udelta < 0x8000) {
        if (seq < maxSeq_) // wrapped
            seqCycles_ += 0x10000;
        maxSeq_ = seq;
    } // else: duplicate or reordered — counts as received only

    // Interarrival jitter, RFC 3550 A.8, in RTP timestamp units.
    const uint32_t clockRate = rtcpReportClockRate_.load(std::memory_order_relaxed);
    const int64_t arrivalTicks
        = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()
          * int64_t(clockRate) / 1000000;
    const int64_t transit = arrivalTicks - int64_t(rtpTs);
    if (transitInit_) {
        int64_t d = transit - lastTransit_;
        if (d < 0)
            d = -d;
        // Ignore absurd samples (RTP timestamp wrap once per ~13 h at 90 kHz).
        if (d < int64_t(clockRate)) {
            int64_t j = int64_t(jitterQ4_) + d - ((int64_t(jitterQ4_) + 8) >> 4);
            jitterQ4_ = j > 0 ? uint32_t(j) : 0;
        }
    }
    lastTransit_ = transit;
    transitInit_ = true;

    // Incoming media rate over a ~500 ms sliding window.
    const auto winMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(now - rateWindowStart_).count();
    if (winMs >= 500) {
        lastRateBps_.store(rateWindowBytes_ * 8000 / uint64_t(winMs), std::memory_order_relaxed);
        rateWindowBytes_ = 0;
        rateWindowStart_ = now;
    }

    if (now - lastRRSent_ >= std::chrono::seconds(1)) {
        lastRRSent_ = now;
        sendReceiverReport();
    }
}

void
SocketPair::sendReceiverReport()
{
    // Reporter identity: reuse our outgoing RTP SSRC (unknown until we have
    // sent at least one packet — video calls are bidirectional in practice).
    const uint32_t ourSsrc = ourOutSsrc_.load(std::memory_order_relaxed);
    if (ourSsrc == 0 or not seqInit_)
        return;

    // RFC 3550 A.3 interval statistics.
    const uint32_t extMax = seqCycles_ + maxSeq_;
    const uint32_t expected = extMax - baseSeqExt_ + 1;
    const uint32_t expectedInterval = expected - expectedPrior_;
    const uint32_t receivedInterval = receivedPkts_ - receivedPrior_;
    expectedPrior_ = expected;
    receivedPrior_ = receivedPkts_;

    const int32_t lostInterval = int32_t(expectedInterval) - int32_t(receivedInterval);
    uint8_t fraction = 0;
    if (expectedInterval > 0 and lostInterval > 0) {
        uint32_t f = (uint32_t(lostInterval) << 8) / expectedInterval;
        fraction = f > 255 ? 255 : uint8_t(f);
    }

    int32_t cumLost = int32_t(expected) - int32_t(receivedPkts_);
    if (cumLost > 0x7fffff)
        cumLost = 0x7fffff;
    else if (cumLost < 0)
        cumLost = 0;

    const uint32_t jitter = jitterQ4_ >> 4;

    uint32_t dlsr = 0;
    const uint32_t lsr = peerSrNtpMid_;
    if (lsr != 0) {
        const auto sinceSr = std::chrono::duration_cast<std::chrono::microseconds>(clock::now()
                                                                                   - peerSrArrival_)
                                 .count();
        dlsr = uint32_t(sinceSr * 65536 / 1000000);
    }

    uint8_t pkt[32];
    auto be32 = [](uint8_t* p, uint32_t v) {
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);
        p[3] = uint8_t(v);
    };
    pkt[0] = 0x81; // V=2, P=0, RC=1
    pkt[1] = 201;  // RR
    pkt[2] = 0;
    pkt[3] = 7; // length in 32-bit words minus one
    be32(pkt + 4, ourSsrc);
    be32(pkt + 8, remoteSsrc_);
    pkt[12] = fraction;
    pkt[13] = uint8_t(cumLost >> 16);
    pkt[14] = uint8_t(cumLost >> 8);
    pkt[15] = uint8_t(cumLost);
    be32(pkt + 16, extMax);
    be32(pkt + 20, jitter);
    be32(pkt + 24, lsr);
    be32(pkt + 28, dlsr);

    writeData(pkt, sizeof(pkt));
}

void
SocketPair::setRtpDelayCallback(std::function<void(int, int)> cb)
{
    rtpDelayCallback_ = std::move(cb);
}

bool
SocketPair::getOneWayDelayGradient(float sendTS, bool marker, int32_t* gradient, int32_t* deltaT)
{
    // Keep only last packet of each frame
    if (not marker) {
        return 0;
    }

    // 1st frame
    if (not lastSendTS_) {
        lastSendTS_ = sendTS;
        lastReceiveTS_ = std::chrono::steady_clock::now();
        return 0;
    }

    int32_t deltaS = (sendTS - lastSendTS_) * 1000; // milliseconds
    if (deltaS < 0)
        deltaS += 64000;
    lastSendTS_ = sendTS;

    std::chrono::steady_clock::time_point arrival_TS = std::chrono::steady_clock::now();
    auto deltaR = std::chrono::duration_cast<std::chrono::milliseconds>(arrival_TS - lastReceiveTS_)
                      .count();
    lastReceiveTS_ = arrival_TS;

    *gradient = deltaR - deltaS;
    *deltaT = deltaR;

    return true;
}

bool
SocketPair::parse_RTP_ext(uint8_t* buf, float* abs)
{
    if (not(buf[0] & 0x10))
        return false;

    uint16_t magic_word = (buf[12] << 8) + buf[13];
    if (magic_word != 0xBEDE)
        return false;

    uint8_t sec = buf[17] >> 2;
    uint32_t fract = ((buf[17] & 0x3) << 16 | (buf[18] << 8) | buf[19]) << 14;
    float milli = fract / pow(2, 32);

    *abs = sec + (milli);
    return true;
}

uint16_t
SocketPair::lastSeqValOut()
{
    if (srtpContext_)
        return srtpContext_->srtp_out.seq_largest;
    SIP_CORE_ERR("SRTP context not found.");
    return 0;
}

} // namespace sip_core
