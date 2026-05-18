/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
 *  Author: Eloi Bail <Eloi.Bail@savoirfairelinux.com>
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

#include <atomic>
#include <thread>
#include <functional>
#include <stdexcept>
#include <condition_variable>
#include <mutex>

namespace sip_core {

struct ThreadLoopException : public std::runtime_error
{
    ThreadLoopException()
        : std::runtime_error("ThreadLoopException")
    {}
};

class ThreadLoop
{
public:
    enum class ThreadState { READY, RUNNING, STOPPING };
    enum class ThreadPriority { LOW, NORMAL, HIGH };

    ThreadLoop(const std::function<bool()>& setup,
               const std::function<void()>& process,
               const std::function<void()>& cleanup,
               const ThreadPriority priority = ThreadPriority::NORMAL);
    virtual ~ThreadLoop();

    void start();
    void exit();
    virtual void stop();
    void join();
    void waitForCompletion(); // thread will stop itself

    // Wait up to `timeout` for the thread to finish. If it does, join and
    // return true. If not, detach the thread and return false: the thread
    // and its captures live on (mainloop holds copies of the user callbacks
    // and a shared_ptr to State, so reading state_ remains safe). Callers
    // that opt into joinFor are responsible for keeping any `this` referenced
    // by the user callbacks alive past the timeout, typically by capturing a
    // shared_from_this()/weak_ptr in those callbacks.
    bool joinFor(std::chrono::milliseconds timeout);

    bool isRunning() const noexcept;
    bool isJoinable() const noexcept;
    bool isStopping() const noexcept { return state_->state == ThreadState::STOPPING; }
    std::thread::id get_id() const noexcept { return threadId_; }

private:
    // set threads priority only for linux/windows
    // this is usefull when the system doesn not have 
    // enough resources to decode/encode audio and video at the same time.
    // MacOS doesnt need such optimization as it usualy have enough resources.
    void setPriority(std::thread& thread, const ThreadPriority& priority);

    ThreadLoop(const ThreadLoop&) = delete;
    ThreadLoop(ThreadLoop&&) noexcept = delete;
    ThreadLoop& operator=(const ThreadLoop&) = delete;
    ThreadLoop& operator=(ThreadLoop&&) noexcept = delete;

    // These must be provided by users of ThreadLoop
    const ThreadPriority priority_;
    std::function<bool()> setup_;
    std::function<void()> process_;
    std::function<void()> cleanup_;

    // Shared between the ThreadLoop instance and the worker thread's
    // captured copy. Putting state on the heap (via shared_ptr) lets the
    // worker keep reading it even if the owning ThreadLoop is destroyed
    // after a joinFor() timeout + detach.
    struct State
    {
        std::atomic<ThreadState> state {ThreadState::READY};
        std::mutex doneMutex;
        std::condition_variable doneCv;
        std::atomic_bool done {true};
    };

    static void mainloop(std::shared_ptr<State> state,
                         std::function<bool()> setup,
                         std::function<void()> process,
                         std::function<void()> cleanup,
                         const ThreadLoop* ownerForLogging);

    std::shared_ptr<State> state_ {std::make_shared<State>()};
    std::thread::id threadId_;
    std::thread thread_;
    mutable std::mutex threadMutex_;
};

class InterruptedThreadLoop : public ThreadLoop
{
public:
    InterruptedThreadLoop(const std::function<bool()>& setup,
                          const std::function<void()>& process,
                          const std::function<void()>& cleanup)
        : ThreadLoop::ThreadLoop(setup, process, cleanup)
    {}

    void stop() override;

    void interrupt() noexcept { cv_.notify_one(); }

    template<typename Rep, typename Period>
    void wait_for(const std::chrono::duration<Rep, Period>& rel_time)
    {
        if (std::this_thread::get_id() != get_id())
            throw std::runtime_error("can not call wait_for outside thread context");

        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, rel_time, [this]() { return isStopping(); });
    }

    template<typename Rep, typename Period, typename Pred>
    bool wait_for(const std::chrono::duration<Rep, Period>& rel_time, Pred&& pred)
    {
        if (std::this_thread::get_id() != get_id())
            throw std::runtime_error("can not call wait_for outside thread context");

        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, rel_time, [this, pred] { return isStopping() || pred(); });
    }

    template<typename Pred>
    void wait(Pred&& pred)
    {
        if (std::this_thread::get_id() != get_id())
            throw std::runtime_error("Can not call wait outside thread context");

        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this, p = std::forward<Pred>(pred)] { return isStopping() || p(); });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace sip_core
