// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/display_hdr.h"

namespace Service {

class Nx2HdrNotifications {
public:
    struct Status {
        bool enabled;
        u16 peak_luminance;

        bool operator==(const Status&) const = default;
    };

    Status RecordStatus(Common::HdrOutput output) {
        return status.emplace(output.enabled, output.peak_luminance);
    }

    u16 RecordPaperWhite(Common::HdrOutput output) {
        return paper_white.emplace(output.reference_white);
    }

    bool ToldHdr() const {
        return status && status->enabled;
    }

    u32 ChangedSlots(Common::HdrOutput output, u32 status_slots, u32 paper_white_slots) const {
        const bool status_changed =
            status && *status != Status{output.enabled, output.peak_luminance};
        const bool paper_white_changed = paper_white && *paper_white != output.reference_white;
        return (status_changed ? status_slots : 0) | (paper_white_changed ? paper_white_slots : 0);
    }

private:
    std::optional<Status> status;
    std::optional<u16> paper_white;
};

} // namespace Service
