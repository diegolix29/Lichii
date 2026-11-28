// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <climits>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/buffer_cache/command_arena.h"
#include "video_core/renderer_vulkan/vk_stream_ring.h"

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct StagingBufferRef {
    VkBuffer buffer;
    VkDeviceAddress device_address;
    VkDeviceSize offset;
    std::span<u8> mapped_span;
    MemoryUsage usage;
    u32 log2_level;
    u64 index;
};

class StagingBufferPool {
public:
    static constexpr size_t NUM_SYNCS = 16;

    /// clock: the buffer side's pool (EDEN_NX2_BUFFER_ARENA) tags its memory with the arena's
    /// stamps instead of scheduler ticks. Two pools share one stream buffer's size (divisor 2).
    explicit StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                               Scheduler& scheduler,
                               const VideoCommon::ArenaClock* clock = nullptr,
                               u32 stream_divisor = 1);
    ~StagingBufferPool();

    StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] VkBuffer StreamBuf() const noexcept {
        return *stream_buffer;
    }

    void TickFrame();

    /// Out of device memory: waits for the submitted work without submitting, then frees every
    /// staging buffer the GPU is done with. Those handed out since the last submit stay.
    void ReleaseMemory();

    /// Bytes of the cached staging buffers of a kind, idle or in use (EDEN_NX2_MEM_TRACE).
    [[nodiscard]] u64 CachedBytes(MemoryUsage usage);

private:
    struct StreamBufferCommit {
        size_t upper_bound;
        u64 tick;
    };

    struct StagingBuffer {
        vk::Buffer buffer;
        VkDeviceAddress device_address;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 log2_level;
        u64 index;
        u64 tick = 0;

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = *buffer,
                .device_address = device_address,
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        /// Handed out deferred, by index, until FreeDeferred: lookups don't scan them. Tens of
        /// thousands of copies of GPU-written command list pages stay held in Hyrule Warriors:
        /// Age of Imprisonment fights, which made each dispatch's copies scan them all.
        std::unordered_map<u64, StagingBuffer> deferred;
        size_t delete_index = 0;
        size_t iterate_index = 0;
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    StagingBufferRef GetStreamBuffer(size_t size);

    /// Calls `f` with a predicate saying whether the GPU is past a stream buffer region's tick.
    /// Without an arena clock it reads the GPU's tick once for the whole scan; with one it asks
    /// the clock per region.
    template <typename F>
    auto WithRegionFree(F&& f) const;

    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred = false);

    std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size, MemoryUsage usage,
                                                         bool deferred);

    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred);

    StagingBuffersCache& GetCache(MemoryUsage usage);

    void ReleaseCache(MemoryUsage usage);

    /// Frees every staging buffer the GPU is done with, of every kind (out of memory).
    void ReleaseIdleBuffers();

    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);
    [[nodiscard]] u64 Now() const noexcept;
    [[nodiscard]] bool IsFree(u64 tick) const noexcept;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    const VideoCommon::ArenaClock* clock;

    vk::Buffer stream_buffer;
    VkDeviceAddress stream_buffer_address{};
    std::span<u8> stream_pointer;
    VkDeviceSize stream_buffer_size;

    StreamRing<NUM_SYNCS> stream_ring;

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 buffer_index = 0;
    u64 unique_ids{};
};

} // namespace Vulkan
