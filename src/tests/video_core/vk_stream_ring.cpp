// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <optional>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_stream_ring.h"

namespace {

constexpr size_t Regions = 16;
constexpr size_t RegionBytes = 64 * 1024;
constexpr size_t RingBytes = Regions * RegionBytes;
constexpr size_t Alignment = 256;

using Ring = Vulkan::StreamRing<Regions>;

/// A ring and a GPU that has finished every tick up to `known`, with the ranges handed out for
/// ticks it has not finished: none of them may be handed out again.
struct Model {
    struct Range {
        size_t begin;
        size_t end;
        u64 tick;
    };

    Ring ring{RingBytes, Alignment};
    u64 tick = 1;
    u64 known = 0;
    std::vector<Range> pending;
    size_t fallbacks = 0;

    /// A stream request: the offset handed out, or none for a fallback.
    std::optional<size_t> Request(size_t bytes) {
        return Check(bytes, ring.Allocate(bytes, tick, [this](u64 t) { return IsFree(t); }));
    }

    /// The out-of-memory path's request at the cursor, of any size.
    std::optional<size_t> Take(size_t bytes) {
        return Check(bytes, ring.TakeAtCursor(bytes, [this](u64 t) { return IsFree(t); }));
    }

private:
    bool IsFree(u64 region_tick) const {
        return region_tick <= known;
    }

    std::optional<size_t> Check(size_t bytes, std::optional<size_t> offset) {
        if (!offset) {
            ++fallbacks;
            return std::nullopt;
        }
        std::erase_if(pending, [this](const Range& range) { return range.tick <= known; });
        const size_t begin = *offset;
        const size_t end = begin + bytes;
        const auto overlap = std::ranges::find_if(pending, [&](const Range& range) {
            return begin < range.end && range.begin < end;
        });
        if (end > RingBytes || overlap != pending.end()) {
            const Range other = overlap != pending.end() ? *overlap : Range{};
            CAPTURE(begin, end, other.begin, other.end, other.tick, tick, known);
            FAIL("A range past the ring's end or over one still pending");
        }
        pending.push_back({begin, end, tick});
        return offset;
    }
};

/// Sizes and rolls straight from the engine, which is the same on every standard library (the
/// distributions are not).
struct Draw {
    std::mt19937 rng;
    size_t Below(size_t n) {
        return static_cast<size_t>(rng() % n);
    }
    size_t Size(size_t max) {
        return 1 + Below(max);
    }
};

} // namespace

TEST_CASE("The stream ring hands out no range twice while one tick stays open",
          "[video_core][stream_ring]") {
    // Uploads of mixed sizes, all in one tick the GPU has not finished: past the first lap the
    // ring is busy everywhere, so every request falls back and none gets a range again.
    for (u32 seed = 1; seed <= 64; ++seed) {
        CAPTURE(seed);
        Model model;
        Draw draw{std::mt19937{seed}};
        size_t handed = 0;
        for (int i = 0; i < 400; ++i) {
            const size_t bytes = draw.Size(RegionBytes);
            if (model.Request(bytes)) {
                handed += bytes;
            }
        }
        REQUIRE(handed <= RingBytes);
        REQUIRE(model.fallbacks > 0);
    }
}

TEST_CASE("The stream ring hands out no range twice while the GPU lags behind",
          "[video_core][stream_ring]") {
    // Ticks close at random and the GPU finishes them at random, some far behind. Between 1 and
    // 60 % of the requests (a rate per seed) go to the out-of-memory path, which takes up to a
    // region or up to the whole ring at the cursor: it moves the cursor without marking ticks.
    for (u32 seed = 1; seed <= 64; ++seed) {
        CAPTURE(seed);
        Model model;
        Draw draw{std::mt19937{seed}};
        const size_t take_rate = 1 + draw.Below(60);
        size_t reused = 0;
        size_t taken = 0;
        for (int i = 0; i < 4000; ++i) {
            if (draw.Below(100) < take_rate) {
                const size_t most = draw.Below(2) == 0 ? RegionBytes : RingBytes;
                taken += model.Take(draw.Size(most)).has_value() ? 1 : 0;
            } else if (const auto offset = model.Request(draw.Size(RegionBytes));
                       offset && *offset == 0 && i != 0) {
                ++reused;
            }
            if (draw.Below(100) < 10) {
                ++model.tick;
            }
            if (draw.Below(100) < 5) {
                const u64 behind = std::min<u64>(model.tick - 1, draw.Below(3));
                model.known = std::max(model.known, model.tick - 1 - behind);
            }
        }
        // The GPU catches up often enough for the ring to come round again, and the cursor
        // is free for the out-of-memory path now and then.
        REQUIRE(reused > 0);
        REQUIRE(taken > 0);
    }
}

TEST_CASE("The stream ring wraps to its start once the GPU is past it",
          "[video_core][stream_ring]") {
    Model model;
    // A lap of whole regions but the last in tick 1, which the GPU has not finished.
    for (size_t i = 0; i < Regions - 1; ++i) {
        REQUIRE(model.Request(RegionBytes) == i * RegionBytes);
    }
    // This one would wrap onto the first region, still busy: it falls back and the cursor stays
    // at the last region, where a smaller request still fits.
    REQUIRE_FALSE(model.Request(RegionBytes).has_value());
    REQUIRE(model.Request(1024) == (Regions - 1) * RegionBytes);
    // One ending exactly at the ring's end wraps too, and falls back the same way.
    REQUIRE_FALSE(model.Request(RegionBytes - 1024).has_value());
    // Once the GPU is past tick 1 the ring wraps to its start.
    model.tick = 2;
    model.known = 1;
    REQUIRE(model.Request(RegionBytes) == 0U);
    REQUIRE(model.Request(4096) == RegionBytes);
}

TEST_CASE("The stream ring falls back before a busy region and keeps its cursor",
          "[video_core][stream_ring]") {
    Model model;
    // The first lap: half the ring in tick 1, the rest in tick 2.
    for (size_t i = 0; i < Regions / 2; ++i) {
        REQUIRE(model.Request(RegionBytes) == i * RegionBytes);
    }
    model.tick = 2;
    for (size_t i = Regions / 2; i < Regions - 1; ++i) {
        REQUIRE(model.Request(RegionBytes) == i * RegionBytes);
    }
    // The GPU is past tick 1 only: the ring wraps and refills what tick 1 used. The cursor
    // leaves a region marked with the tick of the next request, so the region where tick 1's
    // last range ended is marked with tick 2.
    model.tick = 3;
    model.known = 1;
    for (size_t i = 0; i < Regions / 2 - 2; ++i) {
        REQUIRE(model.Request(RegionBytes) == i * RegionBytes);
    }
    const size_t cursor = (Regions / 2 - 2) * RegionBytes;
    // A range ending exactly where the tick-2 region starts falls back without moving the cursor:
    // the check covers the region the cursor would end in, as the region holding free_iterator
    // counts as checked afterwards.
    REQUIRE_FALSE(model.Request(RegionBytes).has_value());
    REQUIRE(model.Request(RegionBytes / 2) == cursor);
    // Once tick 2 is done, the next range goes on from there.
    model.known = 2;
    REQUIRE(model.Request(RegionBytes) == cursor + RegionBytes / 2);
}

TEST_CASE("The stream ring checks its start after the out-of-memory path moved the cursor",
          "[video_core][stream_ring]") {
    // That path moves the cursor from the first regions to near the end without marking the
    // regions it leaves: the wrap must still see that the first region was written in tick 3.
    Model model;
    for (size_t i = 0; i < Regions - 1; ++i) {
        REQUIRE(model.Request(RegionBytes) == i * RegionBytes);
    }
    REQUIRE(model.Request(RegionBytes - Alignment) == (Regions - 1) * RegionBytes);
    model.known = 1;
    model.tick = 2;
    REQUIRE(model.Request(8192) == 0U);
    model.known = 2;
    model.tick = 3;
    REQUIRE(model.Request(4096) == 8192U);
    REQUIRE(model.Take(15 * RegionBytes) == 12288U);
    REQUIRE_FALSE(model.Request(RegionBytes).has_value());
    // Past tick 3, it wraps.
    model.known = 3;
    model.tick = 4;
    REQUIRE(model.Request(RegionBytes) == 0U);
}

TEST_CASE("The stream ring refuses a request larger than a region", "[video_core][stream_ring]") {
    Model model;
    REQUIRE(model.ring.RegionSize() == RegionBytes);
    REQUIRE_FALSE(model.Request(RegionBytes + 1).has_value());
    REQUIRE(model.Request(RegionBytes) == 0U);
}
