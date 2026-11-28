// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <memory_resource>
#include <new>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_view_slots.h"

namespace {

class GenerationMemory final : public std::pmr::memory_resource {
public:
    bool fail_allocations{};

private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        if (fail_allocations) {
            throw std::bad_alloc{};
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* memory, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using FaultInjectingViewSlots =
    VideoCommon::ImageViewSlots<std::unique_ptr<u32>, std::pmr::polymorphic_allocator<u32>>;

} // namespace

TEST_CASE("An erased image view stays stale when its slot and address are reused",
          "[video_core][image_view_slots]") {
    struct View {
        GPUVAddr gpu_addr;
        u32 handle;
    };
    constexpr GPUVAddr address = 0x1000;
    VideoCommon::ImageViewSlots<View> views;
    const auto saved = views.insert(View{address, 1});
    const auto generation = views.Generation(saved);
    const auto other = views.insert(View{address + 0x1000, 2});
    const auto other_generation = views.Generation(other);
    const auto* const original = views.GetIfCurrent(saved, generation);
    REQUIRE(original != nullptr);
    CHECK(original->handle == 1);

    views.erase(saved);
    CHECK(views.GetIfCurrent(saved, generation) == nullptr);
    CHECK(views.GetIfCurrent(other, other_generation) != nullptr);

    const auto replacement = views.insert(View{address, 3});
    REQUIRE(replacement == saved);
    REQUIRE(views[replacement].gpu_addr == address);
    CHECK(views.GetIfCurrent(saved, generation) == nullptr);
    const auto* const current = views.GetIfCurrent(replacement, views.Generation(replacement));
    REQUIRE(current != nullptr);
    CHECK(current->handle == 3);
}

TEST_CASE("A moved view can be retired when generation allocation fails",
          "[video_core][image_view_slots]") {
    GenerationMemory memory;
    FaultInjectingViewSlots views{std::pmr::polymorphic_allocator<u32>{&memory}};
    const auto id = views.insert(std::make_unique<u32>(42));
    const auto generation = views.Generation(id);

    memory.fail_allocations = true;
    const auto retired = std::move(views[id]);
    REQUIRE_NOTHROW(views.erase(id));
    CHECK(*retired == 42);
    CHECK(views.GetIfCurrent(id, generation) == nullptr);

    const auto reused = views.insert(std::make_unique<u32>(43));
    REQUIRE(reused == id);
    CHECK(views.GetIfCurrent(id, generation) == nullptr);
}

TEST_CASE("A failed generation allocation rolls back a new view",
          "[video_core][image_view_slots]") {
    GenerationMemory memory;
    FaultInjectingViewSlots views{std::pmr::polymorphic_allocator<u32>{&memory}};
    const VideoCommon::ImageViewId first_slot{0};

    memory.fail_allocations = true;
    REQUIRE_THROWS_AS((void)views.insert(std::make_unique<u32>(42)), std::bad_alloc);
    CHECK(views.GetIfCurrent(first_slot, 0) == nullptr);

    memory.fail_allocations = false;
    const auto id = views.insert(std::make_unique<u32>(43));
    REQUIRE(id == first_slot);
    const auto* const live = views.GetIfCurrent(id, views.Generation(id));
    REQUIRE(live != nullptr);
    CHECK(**live == 43);
}
