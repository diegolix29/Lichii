// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/assert.h"
#include "video_core/host1x/control.h"
#include "video_core/host1x/host1x.h"

namespace Tegra::Host1x {

void Control::ProcessMethod(Host1x& host1x, Method method, u32 argument,
                            const std::stop_token& stop_token) {
    switch (method) {
    case Method::LoadSyncptPayload32:
        syncpoint_value = argument;
        break;
    case Method::WaitSyncpt:
    case Method::WaitSyncpt32:
        Execute(host1x, argument, stop_token);
        break;
    default:
        UNIMPLEMENTED_MSG("Control method {:#x}", u32(method));
        break;
    }
}

void Control::Execute(Host1x& host1x, u32 data, const std::stop_token& stop_token) {
    LOG_TRACE(Service_NVDRV, "Control wait syncpt {} value {}", data, syncpoint_value);
    // Stop-aware: what would increment the syncpoint (the GPU, another device's dropped lists)
    // may be gone when the device is destroyed, and the destructor joins this thread.
    host1x.GetSyncpointManager().WaitHost(data, syncpoint_value, stop_token);
}

} // namespace Tegra::Host1x
