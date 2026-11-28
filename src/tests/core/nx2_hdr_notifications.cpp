// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_hdr_notifications.h"

namespace {

constexpr u32 StatusSlot = 1U << 2;
constexpr u32 PaperWhiteSlot = 1U << 5;

} // namespace

TEST_CASE("A paper-white query cannot hide an HDR status change", "[nx2][display_hdr]") {
    Common::DisplayHdrMonitor monitor;
    Service::Nx2HdrNotifications notifications;
    const auto output = [&] {
        return Common::ResolveGuestHdrOutput(monitor.Current(), Settings::HdrMode::On, 1000, 10,
                                             monitor.PresentingHdr());
    };

    CHECK_FALSE(notifications.RecordStatus(output()).enabled);
    monitor.SetPresentingHdr(true);
    CHECK(notifications.RecordPaperWhite(output()) == 10);
    CHECK(notifications.ChangedSlots(output(), StatusSlot, PaperWhiteSlot) == StatusSlot);
    CHECK_FALSE(notifications.ToldHdr());

    CHECK(notifications.RecordStatus(output()).enabled);
    CHECK(notifications.ToldHdr());
    CHECK(notifications.ChangedSlots(output(), StatusSlot, PaperWhiteSlot) == 0);
}

TEST_CASE("An HDR status query cannot hide a paper-white change", "[nx2][display_hdr]") {
    Common::DisplayHdrMonitor monitor;
    Service::Nx2HdrNotifications notifications;
    const auto output = [&] {
        return Common::ResolveGuestHdrOutput(monitor.Current(), Settings::HdrMode::On, 1000, 0,
                                             monitor.PresentingHdr());
    };

    CHECK(notifications.RecordPaperWhite(output()) == 300);
    REQUIRE(monitor.Update(Common::DisplayHdrInfo{.sdr_white_level = 160.0f}));
    monitor.SetPresentingHdr(true);
    CHECK(notifications.RecordStatus(output()).enabled);
    CHECK(notifications.ChangedSlots(output(), StatusSlot, PaperWhiteSlot) == PaperWhiteSlot);

    CHECK(notifications.RecordPaperWhite(output()) == 160);
    CHECK(notifications.ChangedSlots(output(), StatusSlot, PaperWhiteSlot) == 0);
}

TEST_CASE("HDR events only report changes to values already returned", "[nx2][display_hdr]") {
    Service::Nx2HdrNotifications notifications;
    Common::HdrOutput output{.enabled = true, .peak_luminance = 1000, .reference_white = 10};

    CHECK(notifications.ChangedSlots(output, StatusSlot, PaperWhiteSlot) == 0);
    notifications.RecordStatus(output);
    notifications.RecordPaperWhite(output);
    CHECK(notifications.ChangedSlots(output, StatusSlot, PaperWhiteSlot) == 0);

    output.peak_luminance = 800;
    CHECK(notifications.ChangedSlots(output, StatusSlot, PaperWhiteSlot) == StatusSlot);
    output.reference_white = 20;
    CHECK(notifications.ChangedSlots(output, StatusSlot, PaperWhiteSlot) ==
          (StatusSlot | PaperWhiteSlot));
    CHECK(notifications.ChangedSlots(output, 0, PaperWhiteSlot) == PaperWhiteSlot);
}
