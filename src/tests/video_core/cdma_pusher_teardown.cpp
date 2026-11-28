// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <stop_token>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "tests/video_core/host1x_test_process.h"
#include "video_core/cdma_pusher.h"
#include "video_core/host1x/host1x.h"

// A named namespace, not an anonymous one: with the class local to this file, GCC would call its
// ProcessMethod directly, not through the base's vtable as for Nvdec and Vic.
namespace CdmaPusherTeardownTest {

/// Outside the pusher, which keeps no members of its own: what the pusher's thread reads stays
/// valid even if it runs on after the pusher is destroyed (the case this test catches).
struct Probe {
    std::promise<void> entered;
    std::stop_token stop; ///< The pusher thread's, so the first method can wait for the stop
    std::atomic<bool> destroyed{};            ///< The derived destructor's body has ended
    std::atomic<bool> ran_after_destroy{};    ///< ProcessMethod ran, or still ran, after that
    std::atomic<int> calls{};
};

static Probe* active_probe{}; ///< The running test's

class Pusher final : public Tegra::CDmaPusher {
public:
    explicit Pusher(Tegra::Host1x::Host1x& host1x_)
        : CDmaPusher{host1x_, static_cast<s32>(Tegra::ChClassId::NvDec)} {
        active_probe->stop = thread.get_stop_token();
    }

    ~Pusher() override {
        StopThread();
        active_probe->destroyed.store(true);
    }

protected:
    void ProcessMethod(u32, u32) override {
        if (active_probe->calls.fetch_add(1) == 0) {
            active_probe->entered.set_value();
            // Held until the stop has been requested, whoever requests it. Past the bound the
            // test itself stalled: going on would make correct code fail, so end the run.
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!active_probe->stop.stop_requested()) {
                if (std::chrono::steady_clock::now() >= limit) {
                    std::fputs("The test's own 5 s bound for the stop expired\n", stderr);
                    std::abort();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        if (active_probe->destroyed.load()) {
            active_probe->ran_after_destroy.store(true);
        }
    }
};

constexpr u32 Immediate(Tegra::ThiMethod method, u32 value) {
    return (static_cast<u32>(Tegra::ChSubmissionMode::Immediate) << 28) |
           (static_cast<u32>(method) << 16) | value;
}

} // namespace CdmaPusherTeardownTest

using namespace CdmaPusherTeardownTest;

TEST_CASE("Destroying a CDmaPusher stops its thread and its list before its derived part is gone",
          "[video_core][cdma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    SCOPE_EXIT {
        system.CoreTiming().Reset();
    };
    system.Initialize();
    auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);

    // One page of guest memory, in a process made the application process: a pusher's thread
    // starts from System::ApplicationMemory().
    Tests::Host1xTestProcess process{system};
    auto& memory = process.Memory();
    u8* const backing = process.Page();
    // The command list: two writes to the class's method 0.
    const std::array<u32, 3> words{
        Immediate(Tegra::ThiMethod::SetMethod0, 0),
        Immediate(Tegra::ThiMethod::SetMethod1, 1),
        Immediate(Tegra::ThiMethod::SetMethod1, 2),
    };
    std::memcpy(backing, words.data(), sizeof(words));

    // The thread runs the list's first device method until the stop has been requested. Unless
    // the derived destructor requested it and joined the thread, that is the base's destructor,
    // so the method is still running once the derived part is gone (Nvdec's and Vic's members,
    // their decoder among them, are destroyed by then), which ran_after_destroy catches. The stop
    // ends the list there: the second device method, after the stop point, must not run.
    Probe probe;
    active_probe = &probe;
    SCOPE_EXIT {
        active_probe = nullptr;
    };
    auto entered = probe.entered.get_future();
    auto pusher = std::make_unique<Pusher>(*host1x);
    pusher->PushEntries(Tegra::ChCommandHeaderList{memory, Tests::Host1xTestProcess::PageAddress, words.size()});
    REQUIRE(entered.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    std::promise<void> destroyed;
    auto done = destroyed.get_future();
    std::jthread teardown{[&pusher, &destroyed] {
        pusher.reset();
        destroyed.set_value();
    }};
    // A join that never returns would leave REQUIRE to throw and the teardown jthread's destructor
    // to wait for it forever: end the run instead, so that regression is a failure, not a hang.
    if (done.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
        std::fputs("Destroying the CDmaPusher did not return in 5 s\n", stderr);
        std::abort();
    }
    REQUIRE(probe.calls.load() == 1);
    REQUIRE_FALSE(probe.ran_after_destroy.load());
}
