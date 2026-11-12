// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "common/literals.h"
#include "common/memory_detect.h"

using namespace Common::Literals;
using Common::HostRam;
using Common::IsBelowSwitch2RamMinimum;

TEST_CASE("Switch 2 RAM minimum: installed RAM below 16 GiB", "[common]") {
    CHECK(IsBelowSwitch2RamMinimum(HostRam{8_GiB, true}));
    CHECK(IsBelowSwitch2RamMinimum(HostRam{12_GiB, true}));
    CHECK(IsBelowSwitch2RamMinimum(HostRam{16_GiB - 1_MiB, true}));
    CHECK_FALSE(IsBelowSwitch2RamMinimum(HostRam{16_GiB, true}));
    CHECK_FALSE(IsBelowSwitch2RamMinimum(HostRam{32_GiB, true}));
}

TEST_CASE("Switch 2 RAM minimum: managed memory below 12 GiB", "[common]") {
    CHECK(IsBelowSwitch2RamMinimum(HostRam{7_GiB + 800_MiB, false}));
    CHECK(IsBelowSwitch2RamMinimum(HostRam{11_GiB + 600_MiB, false}));
    CHECK_FALSE(IsBelowSwitch2RamMinimum(HostRam{12_GiB, false}));
    CHECK_FALSE(IsBelowSwitch2RamMinimum(HostRam{14_GiB + 500_MiB, false}));
}

TEST_CASE("Switch 2 RAM minimum: unknown RAM gives no warning", "[common]") {
    CHECK_FALSE(IsBelowSwitch2RamMinimum(HostRam{0, true}));
    CHECK_FALSE(IsBelowSwitch2RamMinimum(HostRam{0, false}));
}
