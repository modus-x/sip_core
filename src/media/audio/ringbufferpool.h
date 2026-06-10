/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
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

#pragma once

#include "audiobuffer.h"
#include "noncopyable.h"

#include <map>
#include <set>
#include <string>
#include <mutex>
#include <memory>

namespace sip_core {

class RingBuffer;

class RingBufferPool
{
public:
    static const char* const DEFAULT_ID;

    RingBufferPool();
    ~RingBufferPool();

    int getInternalSamplingRate() const { return internalAudioFormat_.sample_rate; }

    AudioFormat getInternalAudioFormat() const { return internalAudioFormat_; }

    void setInternalSamplingRate(unsigned sr);

    void setInternalAudioFormat(AudioFormat format);

    /**
     * Bind together two audio streams so that a client will be able
     * to put and get data specifying its callid only.
     */
    void bindCallID(const std::string& call_id1, const std::string& call_id2);

    /**
     * Add a new call_id to unidirectional outgoing stream
     * \param call_id New call id to be added for this stream
     * \param process_id Process that require this stream
     */
    void bindHalfDuplexOut(const std::string& process_id, const std::string& call_id);

    /**
     * Unbind two calls
     */
    void unBindCallID(const std::string& call_id1, const std::string& call_id2);

    /**
     * Unbind a unidirectional stream
     */
    void unBindHalfDuplexOut(const std::string& process_id, const std::string& call_id);

    void unBindAllHalfDuplexOut(const std::string& call_id);

    void unBindAll(const std::string& call_id);

    bool waitForDataAvailable(const std::string& call_id,
                              const std::chrono::microseconds& max_wait) const;

    std::shared_ptr<AudioFrame> getData(const std::string& call_id);

    std::shared_ptr<AudioFrame> getAvailableData(const std::string& call_id);

    size_t availableForGet(const std::string& call_id) const;

    size_t discard(size_t toDiscard, const std::string& call_id);

    void flush(const std::string& call_id);

    void flushAllBuffers();

    /**
     * Create a new ringbuffer with a default readoffset.
     * This class keeps a weak reference on returned pointer,
     * so the caller is responsible of the referred instance.
     */
    std::shared_ptr<RingBuffer> createRingBuffer(const std::string& id);

    /**
     * Obtain a shared pointer on a RingBuffer given by its ID.
     * If the ID doesn't match to any RingBuffer, the shared pointer
     * is empty. This non-const version flushes internal weak pointers
     * if the ID was used and the associated RingBuffer has been deleted.
     */
    std::shared_ptr<RingBuffer> getRingBuffer(const std::string& id);

    /**
     * Works as non-const getRingBuffer, without the weak reference flush.
     */
    std::shared_ptr<RingBuffer> getRingBuffer(const std::string& id) const;

    bool isAudioMeterActive(const std::string& id);
    void setAudioMeterState(const std::string& id, bool state);

    /**
     * Mark a ring buffer ID so that getData(DEFAULT_ID) skips it.
     * Used by Conference::muteLocalPlayback to silence participants
     * in the local speaker output without affecting inter-participant audio.
     */
    void setLocalPlaybackMuted(const std::string& id, bool muted);

    /**
     * Mark a reader ID so that getData(id) / getAvailableData(id) skip the
     * local capture buffer (DEFAULT_ID) when mixing.  Used by
     * Conference::muteLocalHost / muteHost to silence the host microphone
     * toward conference participants in a race-proof way: even if an async
     * re-bind re-attaches the capture buffer to this reader, its mix will
     * not contain the microphone.
     */
    void setMicMuted(const std::string& id, bool muted);

private:
    NON_COPYABLE(RingBufferPool);

    // A set of RingBuffers readable by a call
    using ReadBindings
        = std::set<std::shared_ptr<RingBuffer>, std::owner_less<std::shared_ptr<RingBuffer>>>;

    const ReadBindings* getReadBindings(const std::string& call_id) const;
    ReadBindings* getReadBindings(const std::string& call_id);

    void removeReadBindings(const std::string& call_id);

    void addReaderToRingBuffer(const std::shared_ptr<RingBuffer>& rbuf, const std::string& call_id);

    void removeReaderFromRingBuffer(const std::shared_ptr<RingBuffer>& rbuf,
                                    const std::string& call_id);

    /**
     * Returns true if @rbuf must be excluded from the mix computed for
     * @call_id (local playback mute or host mic mute).  When the excluded
     * buffer is the capture buffer (DEFAULT_ID), the pending frame is
     * consumed and dropped so the read offset keeps advancing and no stale
     * microphone audio bursts out on un-mute.  Caller must hold stateLock_.
     * Shared by getData() / getAvailableData() so the two paths cannot drift.
     */
    bool skipMutedBuffer(const std::shared_ptr<RingBuffer>& rbuf,
                         const std::string& call_id,
                         bool filterPlayback,
                         bool filterMic);

    // A cache of created RingBuffers listed by IDs.
    std::map<std::string, std::weak_ptr<RingBuffer>> ringBufferMap_ {};

    // A map of which RingBuffers a call has some ReadOffsets
    std::map<std::string, ReadBindings> readBindingsMap_ {};

    mutable std::recursive_mutex stateLock_ {};

    AudioFormat internalAudioFormat_ {AudioFormat::DEFAULT()};

    std::shared_ptr<RingBuffer> defaultRingBuffer_;

    // Ring buffer IDs whose audio should be skipped when mixing for
    // local playback (DEFAULT_ID).  Protected by stateLock_.
    std::set<std::string> localPlaybackMutedIds_;

    // Reader IDs whose mixed frames must not include the local capture
    // buffer (DEFAULT_ID).  Protected by stateLock_.
    std::set<std::string> micMutedIds_;
};

} // namespace sip_core
