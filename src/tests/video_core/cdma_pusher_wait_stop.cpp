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
#include "video_core/host1x/control.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace CdmaPusherWaitStopTest {

class Pusher final : public Tegra::CDmaPusher {
public:
    Pusher(Tegra::Host1x::Host1x& host1x_, std::promise<void>& entered_, std::atomic<int>& calls_)
        : CDmaPusher{host1x_, static_cast<s32>(Tegra::ChClassId::NvDec)}, entered{entered_},
          calls{calls_} {}

    ~Pusher() override {
        StopThread();
    }

protected:
    void ProcessMethod(u32, u32) override {
        if (calls.fetch_add(1) == 0) {
            entered.set_value();
        }
    }

private:
    std::promise<void>& entered;
    std::atomic<int>& calls;
};

constexpr u32 SetClass(Tegra::ChClassId class_id) {
    return (static_cast<u32>(Tegra::ChSubmissionMode::SetClass) << 28) |
           (static_cast<u32>(class_id) << 6);
}

constexpr u32 Immediate(u32 method, u32 value) {
    return (static_cast<u32>(Tegra::ChSubmissionMode::Immediate) << 28) | (method << 16) | value;
}

/// Ends the run if `done` is not ready in 5 s: a join that never returns would otherwise leave
/// REQUIRE to throw and a jthread's destructor to wait for it forever.
static void AbortPast5s(std::future<void>& done, const char* what) {
    if (done.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
        std::fprintf(stderr, "%s did not return in 5 s\n", what);
        std::abort();
    }
}

} // namespace CdmaPusherWaitStopTest

using namespace CdmaPusherWaitStopTest;

TEST_CASE("A host syncpoint wait with a stop token ends when it is stopped",
          "[video_core][syncpoint_actions]") {
    Tegra::Host1x::SyncpointManager manager;
    std::stop_source stop;
    std::promise<bool> result;
    auto waited = result.get_future();
    std::jthread waiter{[&] { result.set_value(manager.WaitHost(3, 1, stop.get_token())); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    stop.request_stop();
    if (waited.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
        std::fputs("A stopped host syncpoint wait did not return in 5 s\n", stderr);
        std::abort();
    }
    REQUIRE_FALSE(waited.get());
    REQUIRE(manager.WaitHost(3, 0, stop.get_token()));

    // A waiter already asleep is woken by the increment that reaches its value.
    std::stop_source fresh;
    std::promise<bool> woken;
    auto increment_seen = woken.get_future();
    std::jthread reacher{[&] { woken.set_value(manager.WaitHost(4, 1, fresh.get_token())); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    manager.IncrementHost(4);
    if (increment_seen.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
        std::fputs("A host syncpoint wait did not return after its increment in 5 s\n", stderr);
        std::abort();
    }
    REQUIRE(increment_seen.get());
}

TEST_CASE("Destroying a CDmaPusher ends a Control wait nothing will increment",
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
    // One method write to the device (so the test knows the list is running), then, in class
    // Control, a wait for host syncpoint 7 to reach 1, which nothing here increments: as when
    // the GPU or the device that would have incremented it is gone at shutdown. Then a second
    // device method, the usual shape of a list after its wait, which must not run once stopped.
    using Control = Tegra::Host1x::Control;
    const std::array<u32, 8> words{
        SetClass(Tegra::ChClassId::NvDec),
        Immediate(static_cast<u32>(Tegra::ThiMethod::SetMethod0), 0),
        Immediate(static_cast<u32>(Tegra::ThiMethod::SetMethod1), 0),
        SetClass(Tegra::ChClassId::Control),
        Immediate(static_cast<u32>(Control::Method::LoadSyncptPayload32), 1),
        Immediate(static_cast<u32>(Control::Method::WaitSyncpt), 7),
        SetClass(Tegra::ChClassId::NvDec),
        Immediate(static_cast<u32>(Tegra::ThiMethod::SetMethod1), 0),
    };
    std::memcpy(backing, words.data(), sizeof(words));

    std::promise<void> entered;
    std::atomic<int> calls{};
    auto running = entered.get_future();
    auto pusher = std::make_unique<Pusher>(*host1x, entered, calls);
    pusher->PushEntries(Tegra::ChCommandHeaderList{memory, Tests::Host1xTestProcess::PageAddress, words.size()});
    REQUIRE(running.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    // Long enough for the thread to reach the wait (it has two words left to read).
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    std::promise<void> destroyed;
    auto done = destroyed.get_future();
    std::jthread teardown{[&pusher, &destroyed] {
        pusher.reset();
        destroyed.set_value();
    }};
    AbortPast5s(done, "Destroying a CDmaPusher waiting on a syncpoint");
    REQUIRE(host1x->GetSyncpointManager().GetHostSyncpointValue(7) == 0);
    REQUIRE(calls.load() == 1);
}
