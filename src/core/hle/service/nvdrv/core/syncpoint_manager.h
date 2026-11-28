// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "common/common_types.h"
#include "core/hle/service/nvdrv/nvdata.h"

namespace Tegra::Host1x {
class Host1x;
} // namespace Tegra::Host1x

namespace Service::Nvidia::NvCore {

enum class ChannelType : u32 {
    MsEnc = 0,
    VIC = 1,
    GPU = 2,
    NvDec = 3,
    Display = 4,
    NvJpg = 5,
    TSec = 6,
    Max = 7
};

/**
 * @brief SyncpointManager handles allocating and accessing host1x syncpoints, these are cached
 * versions of the HW syncpoints which are intermittently synced
 * @note Refer to Chapter 14 of the Tegra X1 TRM for an exhaustive overview of them
 * @url https://http.download.nvidia.com/tegra-public-appnotes/host1x.html
 * @url
 * https://github.com/Jetson-TX1-AndroidTV/android_kernel_jetson_tx1_hdmi_primary/blob/jetson-tx1/drivers/video/tegra/host/nvhost_syncpt.c
 */
class SyncpointManager final {
public:
    explicit SyncpointManager(Tegra::Host1x::Host1x& host1x);
    ~SyncpointManager();

    /**
     * @brief Checks if the given syncpoint is both allocated and below the number of HW syncpoints
     */
    bool IsSyncpointAllocated(u32 id) const;

    /**
     * @brief Finds a free syncpoint and reserves it
     * @return The ID of the reserved syncpoint
     */
    u32 AllocateSyncpoint(bool client_managed);

    /// Reserves a free nonzero ID, or reports exhaustion without changing reservations.
    std::optional<u32> TryAllocateSyncpoint(bool client_managed);

    /**
     * @url
     * https://github.com/Jetson-TX1-AndroidTV/android_kernel_jetson_tx1_hdmi_primary/blob/8f74a72394efb871cb3f886a3de2998cd7ff2990/drivers/gpu/host1x/syncpt.c#L259
     */
    bool HasSyncpointExpired(u32 id, u32 threshold) const;

    bool IsFenceSignalled(NvFence fence) const {
        return HasSyncpointExpired(fence.id, fence.value);
    }

    /**
     * @brief Atomically increments the maximum value of a syncpoint by the given amount
     * @return The new max value of the syncpoint
     */
    u32 IncrementSyncpointMaxExt(u32 id, u32 amount);

    /**
     * @return The minimum value of the syncpoint
     */
    u32 ReadSyncpointMinValue(u32 id);

    /**
     * @return The maximum value of the syncpoint, without checking that it is allocated
     * (IsSyncpointAllocated)
     */
    u32 PeekSyncpointMaxValue(u32 id) const {
        return syncpoints[id].counter_max.load(std::memory_order_relaxed);
    }

    /**
     * @brief Synchronises the minimum value of the syncpoint to with the GPU
     * @return The new minimum value of the syncpoint
     */
    u32 UpdateMin(u32 id);

    /**
     * @brief Brings host1x's guest and host values of the syncpoint up to its maximum, as if
     * every increment submitted to it had run, running the actions those increments meet; then
     * synchronises its minimum. For a channel closed while work submitted to it was dropped, so
     * that the fences of those increments are signalled and the syncpoint is reused without a
     * deficit. Nothing else may increment it meanwhile.
     *
     * Each bank (guest, host) is completed only when it is at most 1 << 16 increments short of
     * the maximum. A bank further behind is taken as values that are not this channel's
     * increments: it is logged as a warning ("... short of its max ...: left") and left as it
     * is. Its fences are then not signalled by the close. Event waits on them stay pending until
     * something else reaches their value or the guest gives up on them. A wait that nvhost_ctrl
     * escalates after repeated failures (WaitHost with the application stalled) blocks until
     * then, as it would have before completion existed.
     * @return The new minimum value of the syncpoint
     */
    u32 CompleteSyncpoint(u32 id);

    /**
     * @brief Frees the usage of a syncpoint.
     */
    void FreeSyncpoint(u32 id);

    /**
     * @return A fence that will be signalled once this syncpoint hits its maximum value
     */
    NvFence GetSyncpointFence(u32 id);

    static constexpr std::array<u32, static_cast<u32>(ChannelType::Max)> channel_syncpoints{
        0x0,  // `MsEnc` is unimplemented
        0xC,  // `VIC`
        0x0,  // `GPU` syncpoints are allocated per-channel instead
        0x36, // `NvDec`
        0x0,  // `Display` is unimplemented
        0x37, // `NvJpg`
        0x0,  // `TSec` is unimplemented
    };        //!< Maps each channel ID to a constant syncpoint

private:
    /**
     * @note reservation_lock should be locked when calling this
     */
    u32 ReserveSyncpoint(u32 id, bool client_managed);

    /**
     * @return The ID of the first free syncpoint
     */
    u32 FindFreeSyncpoint();

    struct SyncpointInfo {
        std::atomic<u32> counter_min; //!< The least value the syncpoint can be (The value it was
                                      //!< when it was last synchronized with host1x)
        std::atomic<u32> counter_max; //!< The maximum value the syncpoint can reach according to
                                      //!< the current usage
        bool interface_managed; //!< If the syncpoint is managed by a host1x client interface, a
                                //!< client interface is a HW block that can handle host1x
                                //!< transactions on behalf of a host1x client (Which would
                                //!< otherwise need to be manually synced using PIO which is
                                //!< synchronous and requires direct cooperation of the CPU)
        bool reserved; //!< If the syncpoint is reserved or not, not to be confused with a reserved
                       //!< value
    };

    static constexpr std::size_t SyncpointCount{192};
    std::array<SyncpointInfo, SyncpointCount> syncpoints{};
    std::mutex reservation_lock;

    Tegra::Host1x::Host1x& host1x;
};

} // namespace Service::Nvidia::NvCore
