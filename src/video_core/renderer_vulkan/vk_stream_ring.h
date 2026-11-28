// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

#include "common/alignment.h"
#include "common/common_types.h"

namespace Vulkan {

/// The stream buffer's cursor. The buffer is a ring of NumRegions equal regions, each tagged with
/// the last tick that wrote to it; a range is handed out only when every region it needs that was
/// not already checked in this lap is free (its tick reached by the GPU). Ticks are marked lazily:
/// the regions the cursor has left get the tick of the next request.
template <size_t NumRegions>
class StreamRing {
public:
    StreamRing(size_t size_, size_t alignment_)
        : size{size_}, region_size{size_ / NumRegions}, alignment{alignment_} {}

    [[nodiscard]] size_t RegionSize() const noexcept {
        return region_size;
    }

    /// The offset of `bytes` for writes in `tick`, or none when a region it needs may still be
    /// read: `is_free(region_tick)` says whether the GPU is past a tick. Then the caller takes a
    /// separate buffer and the cursor stays where it is, so the next request asks again. Requests
    /// larger than a region always get none.
    template <typename IsFree>
    [[nodiscard]] std::optional<size_t> Allocate(size_t bytes, u64 tick, IsFree&& is_free) {
        if (bytes > region_size) {
            return std::nullopt;
        }
        // The regions the cursor has left since the last mark were written in this tick or
        // before; marking them now, as a request handed out would, keeps the check at the wrap
        // from reading an older tick there (TakeAtCursor moves the cursor without marking).
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
                  tick);
        used_iterator = iterator;
        const bool wraps = iterator + bytes >= size;
        // Asked before the cursor moves: once it is back at the start, the region it is in counts
        // as checked, and a later request would be handed space in it unchecked.
        if (wraps && AreRegionsActive(0, Region(bytes) + 1, is_free)) {
            return std::nullopt;
        }
        if (AreRegionsActive(Region(free_iterator) + 1,
                             (std::min)(Region(iterator + bytes) + 1, NumRegions), is_free)) {
            return std::nullopt;
        }
        free_iterator = (std::max)(free_iterator, iterator + bytes);
        if (wraps) {
            std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.end(), tick);
            used_iterator = 0;
            iterator = 0;
            free_iterator = bytes;
        }
        const size_t offset = iterator;
        iterator = Common::AlignUp(iterator + bytes, alignment);
        return offset;
    }

    /// Out of memory for a separate buffer: `bytes` (any size) at the cursor, without wrapping, if
    /// they fit before the end and the regions they cover are free. The submitted ticks are done,
    /// so the ring after its last range is free unless the open tick wrapped into it; the ranges
    /// before it may hold data for commands not recorded yet.
    template <typename IsFree>
    [[nodiscard]] std::optional<size_t> TakeAtCursor(size_t bytes, IsFree&& is_free) {
        if (iterator + bytes >= size ||
            AreRegionsActive(Region(iterator), Region(iterator + bytes) + 1, is_free)) {
            return std::nullopt;
        }
        const size_t offset = iterator;
        free_iterator = (std::max)(free_iterator, offset + bytes);
        iterator = Common::AlignUp(offset + bytes, alignment);
        return offset;
    }

private:
    [[nodiscard]] size_t Region(size_t iter) const noexcept {
        return iter / region_size;
    }

    template <typename IsFree>
    [[nodiscard]] bool AreRegionsActive(size_t region_begin, size_t region_end,
                                        IsFree& is_free) const {
        return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                           [&is_free](u64 sync_tick) { return !is_free(sync_tick); });
    }

    size_t size;
    size_t region_size;
    size_t alignment;
    size_t iterator = 0;      ///< Where the next range starts
    size_t used_iterator = 0; ///< Ranges from here to the cursor are not marked yet
    size_t free_iterator = 0; ///< The regions up to the one holding this were checked in this lap
    std::array<u64, NumRegions> sync_ticks{};
};

} // namespace Vulkan
