// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "tests/core/system_test_context.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"

TEST_CASE("Completing a closed channel's syncpoint runs its dropped increments",
          "[core][nvdrv][syncpoint_complete]") {
    ScopedSystemTestContext context;
    Core::System system;
    SCOPE_EXIT {
        system.CoreTiming().Reset();
    };
    system.Initialize();
    auto host = std::make_unique<Tegra::Host1x::Host1x>(system);
    Service::Nvidia::NvCore::SyncpointManager allocator{*host};
    auto& host1x_syncpoints = host->GetSyncpointManager();

    // A channel submits three increments; one runs before the channel is closed and its list
    // (with the other two) dropped.
    const u32 id = allocator.AllocateSyncpoint(false);
    const u32 base = host1x_syncpoints.GetHostSyncpointValue(id);
    REQUIRE(host1x_syncpoints.GetGuestSyncpointValue(id) == base);
    const u32 fence = allocator.IncrementSyncpointMaxExt(id, 3);
    REQUIRE(fence == base + 3);
    host1x_syncpoints.IncrementGuest(id);
    host1x_syncpoints.IncrementHost(id);

    // What waits on the submit's fence, and a wait past it that must stay waiting.
    u32 host_reached{};
    u32 guest_reached{};
    u32 past_reached{};
    host1x_syncpoints.RegisterHostAction(id, fence, [&] { ++host_reached; });
    host1x_syncpoints.RegisterGuestAction(id, fence, [&] { ++guest_reached; });
    const auto past = host1x_syncpoints.RegisterHostAction(id, fence + 1, [&] { ++past_reached; });
    REQUIRE(host_reached == 0);

    REQUIRE(allocator.CompleteSyncpoint(id) == fence);
    CHECK(host1x_syncpoints.GetHostSyncpointValue(id) == fence);
    CHECK(host1x_syncpoints.GetGuestSyncpointValue(id) == fence);
    CHECK(host_reached == 1);
    CHECK(guest_reached == 1);
    CHECK(past_reached == 0);
    CHECK(allocator.HasSyncpointExpired(id, fence));

    // Nothing short: nothing happens.
    REQUIRE(allocator.CompleteSyncpoint(id) == fence);
    CHECK(host1x_syncpoints.GetHostSyncpointValue(id) == fence);

    // The syncpoint is reused without a deficit: the next channel's first submit resolves with
    // its own increment, not one submit late.
    const u32 next = allocator.IncrementSyncpointMaxExt(id, 1);
    REQUIRE(next == fence + 1);
    host1x_syncpoints.IncrementGuest(id);
    host1x_syncpoints.IncrementHost(id);
    CHECK(host1x_syncpoints.IsReadyHost(id, next));
    CHECK(past_reached == 1);
    host1x_syncpoints.DeregisterHostAction(id, past);
}

TEST_CASE("Completing a syncpoint leaves values it cannot be short of alone",
          "[core][nvdrv][syncpoint_complete]") {
    ScopedSystemTestContext context;
    Core::System system;
    SCOPE_EXIT {
        system.CoreTiming().Reset();
    };
    system.Initialize();
    auto host = std::make_unique<Tegra::Host1x::Host1x>(system);
    Service::Nvidia::NvCore::SyncpointManager allocator{*host};
    auto& host1x_syncpoints = host->GetSyncpointManager();

    // Already at its maximum, and then ahead of it: nothing to complete.
    const u32 id = allocator.AllocateSyncpoint(false);
    const u32 max = allocator.IncrementSyncpointMaxExt(id, 1);
    host1x_syncpoints.IncrementGuest(id);
    host1x_syncpoints.IncrementHost(id);
    REQUIRE(allocator.CompleteSyncpoint(id) == max);
    CHECK(host1x_syncpoints.GetHostSyncpointValue(id) == max);
    host1x_syncpoints.IncrementGuest(id);
    host1x_syncpoints.IncrementHost(id);
    CHECK(allocator.CompleteSyncpoint(id) == max + 1);
    CHECK(host1x_syncpoints.GetHostSyncpointValue(id) == max + 1);
    CHECK(host1x_syncpoints.GetGuestSyncpointValue(id) == max + 1);

    // Short by more than 2^16: not this channel's increments, so left as they are.
    const u32 other = allocator.AllocateSyncpoint(false);
    const u32 start = host1x_syncpoints.GetHostSyncpointValue(other);
    const u32 far = allocator.IncrementSyncpointMaxExt(other, (1U << 16) + 1);
    u32 reached{};
    const auto handle = host1x_syncpoints.RegisterHostAction(other, far, [&] { ++reached; });
    CHECK(allocator.CompleteSyncpoint(other) == start);
    CHECK(host1x_syncpoints.GetHostSyncpointValue(other) == start);
    CHECK(host1x_syncpoints.GetGuestSyncpointValue(other) == start);
    CHECK(reached == 0);
    host1x_syncpoints.DeregisterHostAction(other, handle);
}
