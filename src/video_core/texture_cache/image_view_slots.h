// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "video_core/texture_cache/types.h"

namespace VideoCommon {

template <typename View, typename GenerationAllocator = std::allocator<u32>>
class ImageViewSlots : private Common::SlotVector<View> {
    using Base = Common::SlotVector<View>;

public:
    ImageViewSlots() = default;
    explicit ImageViewSlots(const GenerationAllocator& allocator) : generations(allocator) {}

    using Base::operator[];

    template <typename... Args>
    [[nodiscard]] ImageViewId insert(Args&&... args) {
        const ImageViewId id = Base::insert(std::forward<Args>(args)...);
        if (id.Value() >= generations.size()) {
            try {
                generations.resize(id.Value() + 1);
            } catch (...) {
                Base::erase(id);
                throw;
            }
        }
        return id;
    }

    void erase(ImageViewId id) noexcept {
        ++generations[id.Value()];
        Base::erase(id);
    }

    [[nodiscard]] u32 Generation(ImageViewId id) const noexcept {
        return id.Value() < generations.size() ? generations[id.Value()] : 0;
    }

    [[nodiscard]] View* GetIfCurrent(ImageViewId id, u32 generation) noexcept {
        return Base::contains(id) && Generation(id) == generation ? &(*this)[id] : nullptr;
    }

private:
    std::vector<u32, GenerationAllocator> generations;
};

} // namespace VideoCommon
