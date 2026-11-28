// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stop_token>

#include "common/common_types.h"

namespace Tegra::Host1x {

class Host1x;
class Nvdec;

class Control {
public:
    enum class Method : u32 {
        WaitSyncpt = 0x8,
        LoadSyncptPayload32 = 0x4e,
        WaitSyncpt32 = 0x50,
    };

    /// Writes the method into the state, Invoke Execute() if encountered
    /// A wait ends early once stop_token is stopped (the device is being destroyed).
    void ProcessMethod(Host1x& host1x, Method method, u32 argument,
                       const std::stop_token& stop_token);
    /// For Host1x, execute is waiting on a syncpoint previously written into the state
    void Execute(Host1x& host1x, u32 data, const std::stop_token& stop_token);

    u32 syncpoint_value{};
};

} // namespace Tegra::Host1x
