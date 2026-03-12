/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *  Author: Philippe Gorley <philippe.gorley@savoirfairelinux.com>
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

#pragma once

#include "socket_pair.h"
#include "connectivity/sip_utils.h"
#include "media/media_codec.h"
#include "media/media_codec.h"
#include "sip/sipaccount.h"

#include <functional>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace sip_core {

class MediaRecorder;

class RtpSession
{
public:
    // Media direction
    enum class Direction { SEND, RECV };

    // Note: callId is used for ring buffers and smarttools
    // Note: account is used for getting reference to voip link for nat ping
    RtpSession(const std::string& callId,
               const std::string& streamId,
               MediaType type,
               std::shared_ptr<SIPAccountBase> account)
        : callId_(callId)
        , streamId_(streamId)
        , mediaType_(type)
        // we are sure that account will be SIPAccount
        , account_(std::static_pointer_cast<SIPAccount>(account))
    {}
    virtual ~RtpSession() {};

    virtual void start() = 0;
    virtual void restartSender() = 0;
    virtual void stop() = 0;
    void setMediaSource(const std::string& resource) { input_ = resource; }
    const std::string& getInput() const { return input_; }
    std::shared_ptr<SIPAccountBase> getAccount() const { return account_; }
    MediaType getMediaType() const { return mediaType_; };
    virtual void setMuted(bool mute, Direction dir = Direction::SEND) = 0;

    virtual void updateMedia(const MediaDescription& send, const MediaDescription& receive)
    {
        send_ = send;
        receive_ = receive;
    }

    void setMtu(uint16_t mtu) { mtu_ = mtu; }
    void setReservedSocketPair(ReservedSocketPair&& reserved)
    {
        reservedSocketPair_.emplace(std::move(reserved));
    }
    bool hasReservedSocketPair() const { return reservedSocketPair_.has_value(); }

    void setSuccessfulSetupCb(const std::function<void(MediaType, bool)>& cb)
    {
        onSuccessfulSetup_ = cb;
    }

    virtual void initRecorder() = 0;
    virtual void deinitRecorder() = 0;
    std::shared_ptr<AccountCodecInfo> getCodec() const { return send_.codec; }
    const IpAddr& getSendAddr() const { return send_.addr; };
    const IpAddr& getRecvAddr() const { return receive_.addr; };
    virtual rtcpRRHeader getRtcpRR() = 0;
    virtual rtcpREMBHeader getRtcpREMB() = 0;
    virtual rtcpSRHeader getRtcpSR() = 0;

    inline std::string streamId() const { return streamId_; }

    inline std::string callId() const { return callId_; }

    std::string getRemoteRtpUri() const { return "rtp://" + send_.addr.toString(true); }

protected:
    std::recursive_mutex mutex_;
    const std::string callId_;
    const std::string streamId_;
    MediaType mediaType_;
    const std::shared_ptr<SIPAccountBase> account_;
    std::unique_ptr<SocketPair> socketPair_;
    std::string input_ {};
    MediaDescription send_;
    MediaDescription receive_;
    uint16_t mtu_;
    std::shared_ptr<MediaRecorder> recorder_;
    std::function<void(MediaType, bool)> onSuccessfulSetup_;
    std::optional<ReservedSocketPair> reservedSocketPair_ {};

    ReservedSocketPair takeReservedSocketPair()
    {
        if (!reservedSocketPair_) {
            throw std::runtime_error("Missing reserved socket pair");
        }

        auto reserved = std::move(*reservedSocketPair_);
        reservedSocketPair_.reset();
        return reserved;
    }
};

} // namespace sip_core
