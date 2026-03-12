#include "media/socket_pair.h"
#include "connectivity/ip_utils.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <winsock2.h>
#endif

using namespace sip_core;

namespace {

void
fail(const std::string& message)
{
    std::cerr << "TEST FAILURE: " << message << "\n";
    std::exit(1);
}

void
expect_true(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

class BoundUdpSocket
{
public:
    BoundUdpSocket() = default;
    ~BoundUdpSocket() { reset(); }

    BoundUdpSocket(const BoundUdpSocket&) = delete;
    BoundUdpSocket& operator=(const BoundUdpSocket&) = delete;

    BoundUdpSocket(BoundUdpSocket&& other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = -1;
    }

    BoundUdpSocket& operator=(BoundUdpSocket&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        handle_ = other.handle_;
        other.handle_ = -1;
        return *this;
    }

    bool bind(uint16_t family, uint16_t port)
    {
        reset();
        handle_ = socket(family, SOCK_DGRAM, 0);
        if (handle_ < 0)
            return false;

        auto bindAddr = ip_utils::getAnyHostAddr(family);
        bindAddr.setPort(port);
        if (::bind(handle_, bindAddr, bindAddr.getLength()) < 0) {
            reset();
            return false;
        }

        return true;
    }

    void reset()
    {
        if (handle_ >= 0) {
#ifndef _WIN32
            close(handle_);
#else
            closesocket(handle_);
#endif
            handle_ = -1;
        }
    }

private:
    int handle_ {-1};
};

std::pair<uint16_t, uint16_t>
find_free_range(size_t pairCount)
{
    constexpr uint16_t startPort = 62000;
    constexpr uint16_t endPort = 64999;
    const auto span = static_cast<uint16_t>(pairCount * 2);

    for (uint32_t candidate = startPort; candidate + span - 1 <= endPort; candidate += 2) {
        std::vector<BoundUdpSocket> held;
        held.reserve(span);
        bool ok = true;

        for (size_t i = 0; i < span; ++i) {
            BoundUdpSocket socket;
            if (!socket.bind(AF_INET, static_cast<uint16_t>(candidate + i))) {
                ok = false;
                break;
            }
            held.emplace_back(std::move(socket));
        }

        if (ok) {
            return {static_cast<uint16_t>(candidate), static_cast<uint16_t>(candidate + span - 1)};
        }
    }

    fail("Unable to locate a free UDP test range");
    return {0, 0};
}

BoundUdpSocket
occupy(uint16_t port)
{
    BoundUdpSocket socket;
    expect_true(socket.bind(AF_INET, port),
                "Failed to occupy test UDP port " + std::to_string(port));
    return socket;
}

bool
can_bind(uint16_t port)
{
    BoundUdpSocket socket;
    return socket.bind(AF_INET, port);
}

void
test_reservation_skips_busy_pair()
{
    const auto range = find_free_range(2);
    auto busyRtp = occupy(range.first);
    auto busyRtcp = occupy(static_cast<uint16_t>(range.first + 1));

    auto reserved = reserveSocketPairInRange(AF_INET, range, "test");
    expect_true(reserved.rtpPort() == range.first + 2,
                "Allocator should skip the occupied RTP pair");
    expect_true(reserved.rtcpPort() == range.first + 3,
                "Allocator should skip the occupied RTCP pair");
}

void
test_reservation_exhaustion()
{
    const auto range = find_free_range(2);
    auto busyA = occupy(range.first);
    auto busyB = occupy(static_cast<uint16_t>(range.first + 1));
    auto busyC = occupy(static_cast<uint16_t>(range.first + 2));
    auto busyD = occupy(range.second);

    bool threw = false;
    try {
        auto reserved = reserveSocketPairInRange(AF_INET, range, "test");
        (void) reserved;
    } catch (const std::runtime_error&) {
        threw = true;
    }

    expect_true(threw, "Allocator must fail when every RTP/RTCP pair in range is occupied");
}

void
test_reservation_keeps_ports_busy_until_release()
{
    const auto range = find_free_range(1);
    auto reserved = reserveSocketPairInRange(AF_INET, range, "test");

    expect_true(!can_bind(reserved.rtpPort()),
                "Reserved RTP port should remain busy until the lease is released");
    expect_true(!can_bind(reserved.rtcpPort()),
                "Reserved RTCP port should remain busy until the lease is released");

    reserved.reset();

    expect_true(can_bind(range.first), "Released RTP port should be reusable");
    expect_true(can_bind(static_cast<uint16_t>(range.first + 1)),
                "Released RTCP port should be reusable");
}

} // namespace

int
main()
{
    test_reservation_skips_busy_pair();
    test_reservation_exhaustion();
    test_reservation_keeps_ports_busy_until_release();

    std::cout << "All socket pair reservation tests passed.\n";
    return 0;
}
