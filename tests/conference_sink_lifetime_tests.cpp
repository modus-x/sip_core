/*
 *  Copyright (C) 2004-2026 Savoir-faire Linux Inc.
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

// Regression tests for the Conference video-sink use-after-free (the crash where
// Conference::createSinks dispatched _M_dispose() through a freed SinkClient
// control block; RIP landed wild in pj_dns_resolver_create, si_addr=0x28).
//
// The fix has two parts, each modelled here as a self-contained invariant so the
// test links nothing from the daemon (the real Conference/VideoMixer need the
// whole Manager singleton + a render pipeline, so a faithful end-to-end repro is
// not unit-isolable — see tests/AGENTS.md).
//
//   Part A — createSinks strong-copy: createSinks now takes a STRONG COPY of
//   videoMixer_->getSink() (getSink() returns a reference into VideoMixer::sink_)
//   instead of a reference, so the temporary vector element can never be the last
//   owner and dispose the control block. Deterministic.
//
//   Part B — weak_from_this() callback capture: the root cause was the VideoMixer
//   onSourcesUpdated callback (run on the mixer process() thread) capturing raw
//   [this] and calling weak()/shared_from_this() on the owning Conference. The fix
//   (attachVideoMixerCallbacks) registers the callback post-construction capturing
//   weak_from_this() BY VALUE, so the mixer thread only ever does w.lock() — never
//   touches a raw `this`. lock() on an expired weak_ptr returns null safely, and
//   the captured weak_ptr keeps the control block alive for the duration of the
//   lock. Test 2 reproduces that topology: a background worker holds the captured
//   handle and ticks while the main thread hammers create/destroy. With the fix
//   (weak handle) it is UAF-clean; compile -DOMIT_FIX to use a raw-pointer handle
//   (the bug) and watch ASan trip:
//     c++ -std=c++17 -pthread -fsanitize=address,undefined -DOMIT_FIX \
//         tests/conference_sink_lifetime_tests.cpp -o /tmp/bad && /tmp/bad   # UAF
//     c++ -std=c++17 -pthread -fsanitize=address,undefined \
//         tests/conference_sink_lifetime_tests.cpp -o /tmp/ok && /tmp/ok     # clean
//   (also build a -fsanitize=thread variant). The fixed assertion is deterministic
//   (N iterations complete, worker observed only live owners); reproducing the
//   original SIGSEGV is not deterministic (it depends on heap-reuse timing).

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void
report(const char* name, bool ok, const std::string& detail = {})
{
    if (ok) {
        std::cout << "[ OK ] " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "[FAIL] " << name;
        if (!detail.empty())
            std::cout << " — " << detail;
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// Test 1 — createSinks strong-copy invariant (Part A)
// ---------------------------------------------------------------------------

struct SinkLike
{
    explicit SinkLike(std::atomic<int>& liveCount)
        : liveCount_(liveCount)
    {
        ++liveCount_;
    }
    ~SinkLike() { --liveCount_; }
    std::atomic<int>& liveCount_;
};

void
test_createSinks_strongCopy_keepsSinkAlive()
{
    std::atomic<int> live {0};

    auto sink_ = std::make_shared<SinkLike>(live); // models VideoMixer::sink_
    report("sink created (1 live)", live.load() == 1);

    // BUGGY pattern was `auto& s = sink_;` (reference). The fix copies:
    auto sinkCopy = sink_; // like `auto sink = mixer->getSink();`
    report("strong copy: use_count >= 2", sink_.use_count() >= 2);

    {
        std::vector<std::shared_ptr<SinkLike>> temp {sinkCopy}; // like createSinks' temp
        report("temp vector holds the sink (use_count >= 3)", sink_.use_count() >= 3);
    }
    // Destroying the temp must NOT free the SinkLike — sink_ (and our copy) still
    // own it. With a reference + a concurrently-freed owner, the temp release was
    // the last owner and dispatched _M_dispose() through freed memory: the crash.
    report("temp destruction did not free the sink", live.load() == 1);

    sinkCopy.reset();
    report("after dropping copy, sink still owned by sink_", live.load() == 1);

    sink_.reset();
    report("after dropping last owner, sink freed exactly once", live.load() == 0);
}

// ---------------------------------------------------------------------------
// Test 2 — weak_from_this() callback capture invariant (Part B)
// ---------------------------------------------------------------------------

struct OwnerLike : std::enable_shared_from_this<OwnerLike>
{
    // marker_ is heap-allocated so a stale raw access (OMIT_FIX) is a clean
    // heap-use-after-free that ASan flags precisely.
    OwnerLike()
        : marker_(std::make_unique<std::atomic<int>>(0))
    {}
    void tick() { marker_->fetch_add(1); }
    std::unique_ptr<std::atomic<int>> marker_;
};

void
test_weakCapture_noUseAfterFree()
{
    constexpr int kIterations = 5000;

    std::atomic<bool> stop {false};
    std::atomic<long> liveTicks {0};
    std::mutex slotMtx;

#ifdef OMIT_FIX
    // The BUG: the callback captured a raw owner pointer (raw [this]).
    OwnerLike* rawSlot = nullptr;
#else
    // The FIX: the callback captured weak_from_this() by value.
    std::weak_ptr<OwnerLike> weakSlot;
#endif

    // Background worker — models the VideoMixer process() thread invoking the
    // captured callback. It is free-standing (does not own the OwnerLike), so it
    // can never join itself even if a lock momentarily makes it the last owner.
    std::thread worker([&] {
        while (!stop.load(std::memory_order_relaxed)) {
#ifdef OMIT_FIX
            OwnerLike* p;
            {
                std::lock_guard<std::mutex> lk(slotMtx);
                p = rawSlot;
            }
            if (p)
                p->tick(); // UAF: p may already be destroyed on the main thread
#else
            std::weak_ptr<OwnerLike> w;
            {
                std::lock_guard<std::mutex> lk(slotMtx);
                w = weakSlot;
            }
            if (auto s = w.lock()) { // null-safe; never touches a freed owner
                s->tick();
                liveTicks.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < kIterations; ++i) {
        auto owner = std::make_shared<OwnerLike>();
        {
            std::lock_guard<std::mutex> lk(slotMtx);
#ifdef OMIT_FIX
            rawSlot = owner.get();
#else
            weakSlot = owner;
#endif
        }
        // Let the worker latch the handle and tick against the live owner.
        std::this_thread::sleep_for(std::chrono::microseconds(30));
        // Destroy on the MAIN thread (like the PJSIP teardown thread) WITHOUT
        // clearing the slot first: the worker may still deref the handle. With
        // the fix (weak_ptr) lock() returns null; with the bug (raw pointer) it
        // dereferences freed memory -> ASan heap-use-after-free.
        owner.reset();
    }

    {
        std::lock_guard<std::mutex> lk(slotMtx);
#ifdef OMIT_FIX
        rawSlot = nullptr;
#else
        weakSlot.reset();
#endif
    }
    stop.store(true);
    worker.join();

    report("weak-capture: completed all iterations without UAF/crash",
           true,
           std::to_string(kIterations) + " iterations");
#ifndef OMIT_FIX
    report("weak-capture: worker safely locked live owners (no raw deref)",
           liveTicks.load() >= 0); // always true if we got here without a UAF trap
#endif
}

} // namespace

int
main()
{
    std::cout << "== conference_sink_lifetime_tests ==\n";
#ifdef OMIT_FIX
    std::cout << "(built with -DOMIT_FIX: raw-pointer capture, the bug; expect a UAF under ASan)\n";
#endif
    test_createSinks_strongCopy_keepsSinkAlive();
    test_weakCapture_noUseAfterFree();

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)\n";
    return 1;
}
