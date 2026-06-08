/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Guillaume Roguez <Guillaume.Roguez@savoirfairelinux.com>
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

#include "threadloop.h"
#include "logger.h"

#include <ciso646> // fix windows compiler bug

#include <thread>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif _WIN32
#include <windows.h>
#endif


namespace sip_core {

static void
joinThread(std::thread& thread, const ThreadLoop* owner, const char* action)
{
    if (!thread.joinable()) {
        return;
    }

    if (std::this_thread::get_id() == thread.get_id()) {
        SIP_CORE_WARN("[threadloop:%p] %s called from worker thread; detaching to avoid deadlock",
                      owner,
                      action);
        thread.detach();
        return;
    }

    thread.join();
}

void
ThreadLoop::mainloop(std::shared_ptr<State> state,
                     std::function<bool()> setup,
                     std::function<void()> process,
                     std::function<void()> cleanup,
                     const ThreadLoop* ownerForLogging)
{
    // We do NOT write to a ThreadLoop member here. The owning ThreadLoop
    // captures thread_.get_id() right after spawning us; that avoids a UAF
    // race where joinFor() detaches before the worker runs and ThreadLoop
    // is later destroyed while we still hold a pointer into it.
    try {
        if (setup()) {
            while (state->state == ThreadState::RUNNING)
                process();
            cleanup();
        } else {
            SIP_CORE_ERR("setup failed");
        }
    } catch (const ThreadLoopException& e) {
        SIP_CORE_ERR("[threadloop:%p] ThreadLoopException: %s", ownerForLogging, e.what());
    } catch (const std::exception& e) {
        SIP_CORE_ERR("[threadloop:%p] Unwaited exception: %s", ownerForLogging, e.what());
    }
    if (state->state == ThreadState::RUNNING)
        state->state = ThreadState::STOPPING;
    {
        std::lock_guard<std::mutex> lock(state->doneMutex);
        state->done = true;
    }
    state->doneCv.notify_all();
}

ThreadLoop::ThreadLoop(const std::function<bool()>& setup,
                       const std::function<void()>& process,
                       const std::function<void()>& cleanup,
                       const ThreadPriority priority /* = ThreadPriority::NORMAL */)
    : priority_(priority)
    , setup_(setup)
    , process_(process)
    , cleanup_(cleanup)
    , thread_()
{}

ThreadLoop::~ThreadLoop()
{
    if (isJoinable()) {
        SIP_CORE_ERR("join() should be explicitly called in owner's destructor");
        join();
    }
}

void
ThreadLoop::start()
{
    std::lock_guard<std::mutex> lock(threadMutex_);
    const auto s = state_->state.load();

    if (s == ThreadState::RUNNING) {
        SIP_CORE_ERR("already started");
        return;
    }

    // stop pending but not processed by thread yet?
    if (s == ThreadState::STOPPING and thread_.joinable()) {
        SIP_CORE_DBG("stop pending");
        joinThread(thread_, this, "start");
    }

    state_->state = ThreadState::RUNNING;
    state_->done.store(false);
    thread_ = std::thread(&ThreadLoop::mainloop,
                          state_,
                          setup_,
                          process_,
                          cleanup_,
                          this);
    threadId_ = thread_.get_id();

    // set priority if not default
    if(priority_ != ThreadPriority::NORMAL)
        setPriority(thread_, priority_);
}

void
ThreadLoop::stop()
{
    if (state_->state == ThreadState::RUNNING)
        state_->state = ThreadState::STOPPING;
}

void
ThreadLoop::join()
{
    std::lock_guard<std::mutex> lock(threadMutex_);
    stop();
    joinThread(thread_, this, "join");
}

bool
ThreadLoop::joinFor(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(threadMutex_);
    stop();

    if (!thread_.joinable())
        return true;

    if (std::this_thread::get_id() == thread_.get_id()) {
        SIP_CORE_WARN("[threadloop:%p] joinFor called from worker thread; detaching", this);
        thread_.detach();
        return false;
    }

    // Wait for the worker to signal completion. The worker doesn't take
    // threadMutex_, only signals via state_->doneCv, so holding it here
    // is safe.
    auto state = state_;
    bool joined;
    {
        std::unique_lock<std::mutex> doneLock(state->doneMutex);
        joined = state->doneCv.wait_for(doneLock, timeout, [&state]() {
            return state->done.load();
        });
    }

    if (joined) {
        thread_.join();
        return true;
    }

    SIP_CORE_WARN(
        "[threadloop:%p] joinFor timed out after %lld ms; detaching worker thread",
        this,
        static_cast<long long>(timeout.count()));
    thread_.detach();
    return false;
}

void
ThreadLoop::waitForCompletion()
{
    std::lock_guard<std::mutex> lock(threadMutex_);
    joinThread(thread_, this, "waitForCompletion");
}

void
ThreadLoop::exit()
{
    stop();
    throw ThreadLoopException();
}

bool
ThreadLoop::isRunning() const noexcept
{
#ifdef _WIN32
    return state_->state == ThreadState::RUNNING;
#else
    if (state_->state != ThreadState::RUNNING)
        return false;

    std::lock_guard<std::mutex> lock(threadMutex_);
    return thread_.joinable();
#endif
}

bool
ThreadLoop::isJoinable() const noexcept
{
    std::lock_guard<std::mutex> lock(threadMutex_);
    return thread_.joinable();
}

void
ThreadLoop::setPriority(std::thread& thread, const ThreadLoop::ThreadPriority& priority)
{
    auto handle = thread.native_handle();

#ifdef __linux__
    struct sched_param param;
    int policy;

    pthread_getschedparam(handle, &policy, &param);

    switch(priority) {
        case ThreadPriority::HIGH:
            // Real-time FIFO scheduling
            // policy = SCHED_FIFO;
            param.sched_priority = sched_get_priority_max(policy);
            break;
        case ThreadPriority::LOW:
            // Background scheduling
            // policy = SCHED_OTHER;
            param.sched_priority = sched_get_priority_min(policy);
            break;
        case ThreadPriority::NORMAL:
            // Keep default policy but adjust priority
            param.sched_priority = (sched_get_priority_max(policy) + 
                                 sched_get_priority_min(policy)) / 2;
            break;
    }

    if (pthread_setschedparam(handle, policy, &param) != 0) {
        SIP_CORE_ERR("Failed to set ThreadLoop thread priority.");
        return;
    }
#elif _WIN32
    switch(priority) {
        case ThreadPriority::HIGH:
            if(!SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST)) {
                SIP_CORE_ERR("Failed to set ThreadLoop thread priority.");
                return;
            }
            break;
        case ThreadPriority::LOW:
            if(!SetThreadPriority(handle, THREAD_PRIORITY_IDLE)) {
                SIP_CORE_ERR("Failed to set ThreadLoop thread priority.");
                return;
            }
            break;
        case ThreadPriority::NORMAL:
        default:
            if(!SetThreadPriority(handle, THREAD_PRIORITY_NORMAL)) {
                SIP_CORE_ERR("Failed to set ThreadLoop thread priority.");
                return;
            }
            break;
    }
#endif
    return;
}

void
InterruptedThreadLoop::stop()
{
    ThreadLoop::stop();
    cv_.notify_one();
}
} // namespace sip_core
