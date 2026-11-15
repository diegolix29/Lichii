// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/profile.h"

namespace {
using namespace Shader;

class NativeEnvironment final : public Environment {
public:
    NativeEnvironment() {
        stage = Stage::Compute;
    }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32, u32 offset) override { return offset < 0x200; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return {}; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// OpSelectionMerge count of each function in a SPIR-V module, in order
std::vector<size_t> SelectionsPerFunction(const std::vector<u32>& code) {
    static constexpr u32 op_function{54};
    static constexpr u32 op_selection_merge{247};
    std::vector<size_t> result;
    for (size_t i = 5; i < code.size();) {
        const u32 count{code[i] >> 16};
        REQUIRE(count > 0);
        const u32 opcode{code[i] & 0xffff};
        if (opcode == op_function) {
            result.push_back(0);
        } else if (opcode == op_selection_merge && !result.empty()) {
            ++result.back();
        }
        i += count;
    }
    return result;
}

// Three pointers in constant buffer 0. "routed" is built from the first or the second one,
// "fallback" from the first, the second or a value the pointer search does not follow, and
// "tracked" from the third alone.
struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    IR::Inst* routed{};
    IR::Inst* fallback{};

    Fixture() {
        IR::IREmitter ir{block};
        const auto word = [&](u32 offset) { return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)); };
        const auto address = [&](const IR::U32& low, const IR::U32& high) {
            return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
        };
        const IR::U1 first{ir.INotEqual(word(0x200), ir.Imm32(0))};
        const IR::U1 second{ir.INotEqual(word(0x204), ir.Imm32(0))};
        const IR::U32 low{ir.Select(first, word(0x100), word(0x108))};
        const IR::U32 high{ir.Select(first, word(0x104), word(0x10c))};
        const IR::U32 value{ir.LoadGlobal32(address(low, high))};
        routed = value.InstRecursive();
        const IR::U32 other{ir.IMul(word(0x208), ir.Imm32(3))};
        const IR::U32 any_low{ir.Select(second, low, other)};
        ir.WriteGlobal32(address(any_low, high), value);
        fallback = &block.Instructions().back();
        ir.WriteGlobal32(address(word(0x110), word(0x114)), value);

        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        host.support_int64 = true;
        NativeEnvironment env;
        Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
        Optimization::CollectShaderInfoPass(env, program);
        program.info.constant_buffer_descriptors = {{0, 1}};
    }
};
} // namespace

TEST_CASE("A native access built from its own candidates is routed among them alone",
          "[shader][global_memory_routes]") {
    Fixture fixture;
    const auto& info{fixture.program.info};
    REQUIRE(info.native_global_bounds);
    REQUIRE(info.storage_buffers_descriptors.size() == 3);
    REQUIRE(fixture.routed->GetOpcode() == IR::Opcode::LoadGlobal32);
    REQUIRE(fixture.fallback->GetOpcode() == IR::Opcode::WriteGlobal32);

    const auto offset_of = [&](u32 descriptor) {
        return info.storage_buffers_descriptors[descriptor].cbuf_offset;
    };
    REQUIRE(fixture.program.global_memory_routes.size() == 1);
    std::vector<u32> offsets;
    for (const u32 descriptor : fixture.program.global_memory_routes.front()) {
        offsets.push_back(offset_of(descriptor));
    }
    CHECK(offsets == std::vector<u32>{0x100, 0x108});
    CHECK(fixture.program.global_memory_route.at(fixture.routed) == 0);
    // A path through a multiplication can carry any pointer: every bound buffer
    CHECK_FALSE(fixture.program.global_memory_route.contains(fixture.fallback));

    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const auto selections{
        SelectionsPerFunction(Backend::SPIRV::EmitSPIRV(profile, fixture.program))};
    // The functions through every buffer branch once per buffer, the routed 32-bit load and store
    // once per candidate
    CHECK(std::ranges::count(selections, 3) >= 2);
    CHECK(std::ranges::count(selections, 2) == 2);
}

TEST_CASE("A route through a pointer array keeps the bound slots past those searched",
          "[shader][global_memory_routes]") {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto address = [&](const IR::U32& low, const IR::U32& high) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
    };
    // c0[0x100 + (i << 3)]: the search takes slots 0x100 to 0x178, another access binds 0x180
    const IR::U32 index{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x300))};
    const IR::U32 slot{ir.IAdd(ir.ShiftLeftLogical(index, ir.Imm32(3)), ir.Imm32(0x100))};
    const IR::U32 high{ir.GetCbuf(ir.Imm32(0), ir.IAdd(slot, ir.Imm32(4)))};
    const IR::U32 value{ir.LoadGlobal32(address(ir.GetCbuf(ir.Imm32(0), slot), high))};
    IR::Inst* const dynamic{value.InstRecursive()};
    ir.WriteGlobal32(address(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x180)),
                             ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x184))),
                     value);
    ir.WriteGlobal32(
        address(ir.GetCbuf(ir.Imm32(1), ir.Imm32(0)), ir.GetCbuf(ir.Imm32(1), ir.Imm32(4))), value);

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    NativeEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    REQUIRE(dynamic->GetOpcode() == IR::Opcode::LoadGlobal32);
    const auto& descriptors{program.info.storage_buffers_descriptors};
    REQUIRE(descriptors.size() == 18);
    REQUIRE(program.global_memory_route.contains(dynamic));
    std::vector<std::pair<u32, u32>> route;
    const u32 route_index{program.global_memory_route.at(dynamic)};
    for (const u32 descriptor : program.global_memory_routes.at(route_index)) {
        route.emplace_back(descriptors[descriptor].cbuf_index, descriptors[descriptor].cbuf_offset);
    }
    CHECK(route.size() == 17);
    CHECK(std::ranges::find(route, std::pair{0U, 0x180U}) != route.end());
    CHECK(std::ranges::find(route, std::pair{1U, 0U}) == route.end());
}

