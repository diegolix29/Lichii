// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "tests/core/system_test_context.h"
#include "video_core/fence_manager.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"

// A named namespace, not an anonymous one: with the classes local to this file, GCC calls the
// manager's functions directly, so a call after the derived destructor would not go through the
// base's vtable and abort as it does for the renderers' managers.
namespace SplitFenceWaitTest {

struct Cache {
    std::mutex mutex;
    bool HasUncommittedFlushes() const { return true; }
    bool HasAsyncFlushWork() const { return true; }
    bool ShouldWaitAsyncFlushes() const { return true; }
    void AccumulateFlushes() {}
    void CommitAsyncFlushes() {}
    void PopAsyncFlushes() {}
};

struct Fence : VideoCommon::FenceBase {
    Fence(bool stubbed, u64 tick_) : FenceBase{stubbed}, tick{tick_} {}
    u64 tick;
};

struct Traits {
    using FenceType = std::shared_ptr<Fence>;
    using TextureCacheType = Cache;
    using BufferCacheType = Cache;
    using QueryCacheType = Cache;
    static constexpr bool HAS_ASYNC_CHECK = true;
};

struct Submission {
    std::mutex mutex;
    std::condition_variable cv;
    u64 submitted{};
    std::atomic<bool> timed_out{};
    bool wrong_owner{};
    std::atomic<bool> destroyed{};            ///< The derived destructor's body has ended
    std::atomic<bool> called_after_destroy{}; ///< A call came after that
    const std::thread::id owner = std::this_thread::get_id();
};

class FenceManager final : public VideoCommon::FenceManager<Traits> {
public:
    FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_, Cache& texture,
                 Cache& buffer, Cache& query, Submission& submission_)
        : VideoCommon::FenceManager<Traits>{rasterizer_, gpu_, texture, buffer, query},
          submission{submission_} {}

    ~FenceManager() override {
        if (on_destroy) {
            on_destroy();
        }
        StopFenceThread();
        submission.destroyed.store(true);
    }

    u64 queued{};
    u32 submissions{};
    std::function<void()> on_destroy; ///< Runs first in the destructor, before the base's
    std::function<void()> on_wait;    ///< Runs first in WaitFence, on the fencing thread

protected:
    std::shared_ptr<Fence> CreateFence(bool stubbed) override {
        return std::make_shared<Fence>(stubbed, queued + 1);
    }

    void QueueFence(std::shared_ptr<Fence>& fence) override {
        queued = fence->tick;
    }

    bool IsFenceSignaled(std::shared_ptr<Fence>& fence) const override {
        if (submission.destroyed.load()) {
            submission.called_after_destroy.store(true);
        }
        std::scoped_lock lock{submission.mutex};
        return submission.submitted >= fence->tick;
    }

    void WaitFence(std::shared_ptr<Fence>& fence) override {
        if (on_wait) {
            on_wait();
        }
        if (submission.destroyed.load()) {
            submission.called_after_destroy.store(true);
        }
        std::unique_lock lock{submission.mutex};
        if (!submission.cv.wait_for(lock, std::chrono::seconds{2},
                                    [&] { return submission.submitted >= fence->tick; })) {
            submission.timed_out.store(true);
        }
    }

    void SubmitDeferredWork() override {
        std::scoped_lock lock{submission.mutex};
        submission.wrong_owner |= std::this_thread::get_id() != submission.owner;
        submission.submitted = queued;
        ++submissions;
        submission.cv.notify_all();
    }

private:
    Submission& submission;
};

} // namespace SplitFenceWaitTest

using namespace SplitFenceWaitTest;

TEST_CASE("Delayed fence callbacks submit before the owner waits", "[video_core][split_fence_wait]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Cache texture, buffer, query;
    Submission submission;
    const auto previous = Settings::values.gpu_fence_behavior.GetValue();
    SCOPE_EXIT { Settings::values.gpu_fence_behavior.SetValue(previous); };
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Accurate);
    std::vector<u32> callbacks;
    {
        FenceManager manager{rasterizer, gpu, texture, buffer, query, submission};
        for (u32 i = 0; i < 128; ++i) {
            manager.SyncOperation([&, i] { callbacks.push_back(i * 2); });
            manager.SignalFence([&, i] { callbacks.push_back(i * 2 + 1); });
            REQUIRE(manager.submissions == i + 1);
        }
        manager.WaitPendingFences(true);
        REQUIRE(manager.submissions == 130);
        REQUIRE_FALSE(submission.timed_out.load());
        REQUIRE_FALSE(submission.wrong_owner);
    }
    REQUIRE(callbacks.size() == 256);
    for (u32 i = 0; i < callbacks.size(); ++i) {
        REQUIRE(callbacks[i] == i);
    }
}

TEST_CASE("Immediate fence waits still submit outstanding work", "[video_core][split_fence_wait]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Cache texture, buffer, query;
    Submission submission;
    const auto previous = Settings::values.gpu_fence_behavior.GetValue();
    const auto previous_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.gpu_fence_behavior.SetValue(previous);
        Settings::values.current_gpu_accuracy = previous_accuracy;
    };
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Default);
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    std::atomic<bool> callback{};
    {
        FenceManager manager{rasterizer, gpu, texture, buffer, query, submission};
        manager.SyncOperation([&] { callback.store(true); });
        manager.SignalFence([] {});
        REQUIRE(manager.submissions == 0);
        manager.WaitPendingFences(false);
        REQUIRE(manager.submissions == 0);
        manager.WaitPendingFences(true);
        REQUIRE(manager.submissions == 1);
        Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
        manager.WaitPendingFences(true);
        REQUIRE(manager.submissions == 3);
        REQUIRE_FALSE(submission.timed_out.load());
        REQUIRE_FALSE(submission.wrong_owner);
    }
    REQUIRE(callback.load());
}

TEST_CASE("Destroying the fence manager wakes a fencing thread about to sleep",
          "[video_core][split_fence_wait]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Cache texture, buffer, query;
    Submission submission;
    const auto previous = Settings::values.gpu_fence_behavior.GetValue();
    SCOPE_EXIT { Settings::values.gpu_fence_behavior.SetValue(previous); };
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Accurate);
    auto manager =
        std::make_unique<FenceManager>(rasterizer, gpu, texture, buffer, query, submission);
    // The fencing thread runs the marker's callback, finds no fences and, with guard held, is
    // about to sleep: there it waits until the destructor has begun, so the stop is requested
    // while the thread is between its look at the stop flag and its sleep.
    bool marker_ran{}; // Fencing thread only
    bool held{};       // Fencing thread only
    std::latch in_window{1};
    std::latch destroying{1};
    manager->SetFenceThreadSleepHookForTest([&] {
        if (marker_ran && !held) {
            held = true;
            in_window.count_down();
            destroying.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    });
    manager->on_destroy = [&] { destroying.count_down(); };
    manager->SignalFence([&] { marker_ran = true; });
    in_window.wait();
    std::promise<void> destroyed;
    auto done = destroyed.get_future();
    std::jthread teardown{[&manager, &destroyed] {
        manager.reset();
        destroyed.set_value();
    }};
    REQUIRE(done.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    REQUIRE(held);
}

TEST_CASE("Destroying the fence manager stops the fencing thread before its functions are gone",
          "[video_core][split_fence_wait]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Cache texture, buffer, query;
    Submission submission;
    const auto previous = Settings::values.gpu_fence_behavior.GetValue();
    SCOPE_EXIT { Settings::values.gpu_fence_behavior.SetValue(previous); };
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Accurate);
    auto manager =
        std::make_unique<FenceManager>(rasterizer, gpu, texture, buffer, query, submission);
    // The fencing thread waits for the first fence until the destructor has begun, then finds a
    // second fence queued and asks whether it is signaled. Unless the derived destructor stopped
    // the thread, that call goes through the base's vtable and the tests process aborts on a pure
    // virtual call. A compiler that calls the derived function directly runs it on a destroyed
    // object instead, which called_after_destroy catches. A regression thus aborts the whole run:
    // a death test in a forked child could deadlock on a lock another thread held at the fork.
    bool held{}; // Fencing thread only
    std::promise<void> waiting;
    std::promise<void> destroying;
    auto waited = waiting.get_future();
    auto destroy_begun = destroying.get_future();
    manager->on_wait = [&] {
        if (!held) {
            held = true;
            waiting.set_value();
            destroy_begun.wait_for(std::chrono::seconds{5});
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    };
    manager->on_destroy = [&] { destroying.set_value(); };
    manager->SignalFence([] {});
    // The hooks capture state made after the manager: a REQUIRE unwinding here would destroy it
    // before the manager, whose destructor then runs on_destroy on it. End the run instead.
    if (waited.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
        std::fputs("The fencing thread did not reach the first fence's wait in 5 s\n", stderr);
        std::abort();
    }
    manager->SignalFence([] {});
    std::promise<void> destroyed;
    auto done = destroyed.get_future();
    std::jthread teardown{[&manager, &destroyed] {
        manager.reset();
        destroyed.set_value();
    }};
    // A join that never returns would leave REQUIRE to throw and the teardown jthread's destructor
    // to wait for it forever: end the run instead, so that regression is a failure, not a hang.
    if (done.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
        std::fputs("Destroying the fence manager did not return in 5 s\n", stderr);
        std::abort();
    }
    REQUIRE_FALSE(submission.called_after_destroy.load());
}
