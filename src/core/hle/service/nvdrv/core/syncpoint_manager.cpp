// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/assert.h"
#include "common/logging.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "video_core/host1x/host1x.h"

namespace Service::Nvidia::NvCore {

SyncpointManager::SyncpointManager(Tegra::Host1x::Host1x& host1x_) : host1x{host1x_} {
    constexpr u32 VBlank0SyncpointId{26};
    constexpr u32 VBlank1SyncpointId{27};

    // Reserve both vblank syncpoints as client managed as they use Continuous Mode
    // Refer to section 14.3.5.3 of the TRM for more information on Continuous Mode
    // https://github.com/Jetson-TX1-AndroidTV/android_kernel_jetson_tx1_hdmi_primary/blob/8f74a72394efb871cb3f886a3de2998cd7ff2990/drivers/gpu/host1x/drm/dc.c#L660
    ReserveSyncpoint(VBlank0SyncpointId, true);
    ReserveSyncpoint(VBlank1SyncpointId, true);

    for (u32 syncpoint_id : channel_syncpoints) {
        if (syncpoint_id) {
            ReserveSyncpoint(syncpoint_id, false);
        }
    }
}

SyncpointManager::~SyncpointManager() = default;

u32 SyncpointManager::ReserveSyncpoint(u32 id, bool client_managed) {
    auto& syncpoint = syncpoints.at(id);

    if (syncpoint.reserved) {
        ASSERT_MSG(false, "Requested syncpoint is in use");
        return 0;
    }

    syncpoint.reserved = true;
    syncpoint.interface_managed = client_managed;

    return id;
}

u32 SyncpointManager::FindFreeSyncpoint() {
    for (u32 i{1}; i < syncpoints.size(); i++) {
        if (!syncpoints[i].reserved) {
            return i;
        }
    }
    return 0;
}

u32 SyncpointManager::AllocateSyncpoint(bool client_managed) {
    const auto id = TryAllocateSyncpoint(client_managed);
    ASSERT_MSG(id.has_value(), "Failed to find a free syncpoint!");
    return id.value_or(0);
}

std::optional<u32> SyncpointManager::TryAllocateSyncpoint(bool client_managed) {
    std::lock_guard lock(reservation_lock);
    const u32 id = FindFreeSyncpoint();
    if (id == 0) {
        return std::nullopt;
    }
    return ReserveSyncpoint(id, client_managed);
}

void SyncpointManager::FreeSyncpoint(u32 id) {
    std::lock_guard lock(reservation_lock);
    auto& syncpoint = syncpoints.at(id);
    ASSERT(syncpoint.reserved);
    syncpoint.reserved = false;
}

bool SyncpointManager::IsSyncpointAllocated(u32 id) const {
    return (id < SyncpointCount) && syncpoints[id].reserved;
}

bool SyncpointManager::HasSyncpointExpired(u32 id, u32 threshold) const {
    const SyncpointInfo& syncpoint{syncpoints.at(id)};

    if (!syncpoint.reserved) {
        ASSERT(false);
        return false;
    }

    // If the interface manages counters then we don't keep track of the maximum value as it handles
    // sanity checking the values then
    if (syncpoint.interface_managed) {
        return static_cast<s32>(syncpoint.counter_min - threshold) >= 0;
    } else {
        return (syncpoint.counter_max - threshold) >= (syncpoint.counter_min - threshold);
    }
}

u32 SyncpointManager::IncrementSyncpointMaxExt(u32 id, u32 amount) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    return syncpoint.counter_max += amount;
}

u32 SyncpointManager::ReadSyncpointMinValue(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    return syncpoint.counter_min;
}

u32 SyncpointManager::UpdateMin(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    syncpoint.counter_min = host1x.GetSyncpointManager().GetHostSyncpointValue(id);
    return syncpoint.counter_min;
}

u32 SyncpointManager::CompleteSyncpoint(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    // Values wrap: one behind the maximum by less than half the range is short of it. A long
    // shortfall means the values are not this channel's increments: leave them as they are.
    constexpr u32 MaxShortfall = 1U << 16;
    const u32 max = syncpoint.counter_max.load(std::memory_order_relaxed);
    auto& host1x_syncpoints = host1x.GetSyncpointManager();
    const auto complete = [&](u32 value, auto&& increment, const char* bank) {
        const u32 shortfall = max - value;
        if (static_cast<s32>(shortfall) <= 0) {
            return;
        }
        if (shortfall > MaxShortfall) {
            LOG_WARNING(Service_NVDRV, "Syncpoint {} {} value {} is {} short of its max {}: left",
                        id, bank, value, shortfall, max);
            return;
        }
        for (u32 i = 0; i < shortfall; ++i) {
            increment();
        }
    };
    // All guest increments, then all host ones. A list's IncSyncpts interleave the two banks
    // per increment, but nothing compares the banks against each other.
    complete(host1x_syncpoints.GetGuestSyncpointValue(id),
             [&] { host1x_syncpoints.IncrementGuest(id); }, "guest");
    complete(host1x_syncpoints.GetHostSyncpointValue(id),
             [&] { host1x_syncpoints.IncrementHost(id); }, "host");
    return UpdateMin(id);
}

NvFence SyncpointManager::GetSyncpointFence(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return NvFence{};
    }

    return {
        .id = static_cast<s32>(id),
        .value = syncpoint.counter_max,
    };
}

} // namespace Service::Nvidia::NvCore
