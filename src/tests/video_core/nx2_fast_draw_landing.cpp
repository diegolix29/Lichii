// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_fast_draw.h"

namespace FastDraw = VideoCommon::Nx2FastDraw;

namespace {

// `writers` threads write as a guest CPU write does (BeginMemoryChange, the bytes,
// EndMemoryChange), pausing `idle` iterations between writes. The thread that draws does a full
// resolve's read, a comparison's read and HandleMemoryUnchanged, `rounds` times. Returns how
// often the two reads differed while memory was reported unchanged, and how often they differed.
std::pair<int, int> Race(int writer_count, int idle, int rounds) {
    std::atomic<u64> memory{};
    std::atomic<bool> stop{};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < writer_count; ++writer) {
        writers.emplace_back([&, writer] {
            u64 value = static_cast<u64>(writer) << 48;
            while (!stop.load(std::memory_order_relaxed)) {
                FastDraw::BeginMemoryChange();
                memory.store(++value, std::memory_order_relaxed);
                FastDraw::EndMemoryChange();
                for (int i = 0; i < idle; ++i) {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                }
            }
        });
    }
    int missed = 0;
    int changed_seen = 0;
    for (int round = 0; round < rounds; ++round) {
        FastDraw::NoteHandlesRead();
        const u64 resolved = memory.load(std::memory_order_relaxed);
        const u64 compared = memory.load(std::memory_order_relaxed);
        const bool unchanged = FastDraw::HandleMemoryUnchanged();
        if (compared != resolved) {
            ++changed_seen;
            missed += unchanged ? 1 : 0;
        }
    }
    stop.store(true);
    for (auto& thread : writers) {
        thread.join();
    }
    return {missed, changed_seen};
}

// Whether the writers ran between the reader's two reads at all: on one core, or two quiet ones,
// they may not, and then the round proves nothing either way.
static void NoteInterleaving(int changed_seen) {
    if (changed_seen == 0) {
        WARN("No writer ran between the reader's reads; the property was not exercised");
    }
}

} // Anonymous namespace

TEST_CASE("A write that ends just after a resolve's clear still counts as a change",
          "[nx2_fast_draw]") {
    // The race this guards: a writer that read the flag still set from before a resolve's clear
    // skipped its store. Few writers, pausing between writes, so the landing count is often zero
    // while a write ends. Evidence, not a guaranteed detector: on four idle cores it fails on most
    // runs of the unfixed code, less often on loaded or fewer cores; it cannot fail on the fixed
    // code. The warning is per configuration, when no change was seen over a whole run.
    for (const int idle : {200, 0}) {
        for (const int writers : {3, 6}) {
            const auto [missed, changed_seen] = Race(writers, idle, 1'000'000);
            CAPTURE(idle, writers);
            REQUIRE(missed == 0);
            NoteInterleaving(changed_seen);
        }
    }
    REQUIRE(FastDraw::g_changes_landing.load() == 0);
}

// The next two pin the parts of the contract this fix leaves as they were (both pass on the code
// before it): the landing count, and a write ending after a clear.
TEST_CASE("A change still landing on another thread counts as a change", "[nx2_fast_draw]") {
    FastDraw::NoteHandlesRead();
    std::atomic<int> stage{};
    std::thread writer([&] {
        FastDraw::BeginMemoryChange();
        stage.store(1);
        while (stage.load() != 2) {
            std::this_thread::yield();
        }
        FastDraw::EndMemoryChange();
    });
    while (stage.load() != 1) {
        std::this_thread::yield();
    }
    // CHECK, not REQUIRE: a REQUIRE that throws here would leave `writer` joinable.
    CHECK_FALSE(FastDraw::HandleMemoryUnchanged());
    stage.store(2);
    writer.join();
    // It ended changing memory: still a change until the next resolve.
    REQUIRE_FALSE(FastDraw::HandleMemoryUnchanged());
    FastDraw::NoteHandlesRead();
    REQUIRE(FastDraw::HandleMemoryUnchanged());
}

TEST_CASE("A write that ends after a resolve's clear is a change, whatever the flag held",
          "[nx2_fast_draw]") {
    // On one thread, so in order: the flag set from an earlier write, a resolve, then a write.
    // Not the race above, which needs another thread's stale read.
    FastDraw::BeginMemoryChange();
    FastDraw::EndMemoryChange();
    FastDraw::NoteHandlesRead();
    FastDraw::BeginMemoryChange();
    FastDraw::EndMemoryChange();
    REQUIRE_FALSE(FastDraw::HandleMemoryUnchanged());
    FastDraw::NoteHandlesRead();
    REQUIRE(FastDraw::HandleMemoryUnchanged());
}
