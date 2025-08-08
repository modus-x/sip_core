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

void
ThreadLoop::mainloop(std::thread::id& tid,
                     const std::function<bool()> setup,
                     const std::function<void()> process,
                     const std::function<void()> cleanup)
{
    tid = std::this_thread::get_id();
    try {
        if (setup()) {
            while (state_ == ThreadState::RUNNING)
                process();
            cleanup();
        } else {
            SIP_CORE_ERR("setup failed");
        }
    } catch (const ThreadLoopException& e) {
        SIP_CORE_ERR("[threadloop:%p] ThreadLoopException: %s", this, e.what());
    } catch (const std::exception& e) {
        SIP_CORE_ERR("[threadloop:%p] Unwaited exception: %s", this, e.what());
    }
    stop();
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
    if (isRunning()) {
        SIP_CORE_ERR("join() should be explicitly called in owner's destructor");
        join();
    }
}

void
ThreadLoop::start()
{
    const auto s = state_.load();

    if (s == ThreadState::RUNNING) {
        SIP_CORE_ERR("already started");
        return;
    }

    // stop pending but not processed by thread yet?
    if (s == ThreadState::STOPPING and thread_.joinable()) {
        SIP_CORE_DBG("stop pending");
        thread_.join();
    }

    state_ = ThreadState::RUNNING;
    thread_ = std::thread(&ThreadLoop::mainloop, this, std::ref(threadId_), setup_, process_, cleanup_);
    threadId_ = thread_.get_id();

    // set priority if not default
    if(priority_ != ThreadPriority::NORMAL)
        setPriority(thread_, priority_);
}

void
ThreadLoop::stop()
{
    if (state_ == ThreadState::RUNNING)
        state_ = ThreadState::STOPPING;
}

void
ThreadLoop::join()
{
    stop();
    if (thread_.joinable())
        thread_.join();
}

void
ThreadLoop::waitForCompletion()
{
    if (thread_.joinable())
        thread_.join();
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
    return state_ == ThreadState::RUNNING;
#else
    return thread_.joinable() and state_ == ThreadState::RUNNING;
#endif
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
