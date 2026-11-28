// SPDX-License-Identifier: GPL-3.0-or-later
// Emitting one IR::Program twice must give the same SPIR-V: Nx2Perf::Verify() emits a deferred
// pipeline's reference module and then the pipeline's module from the same program.

#include <array>
#include <optional>
#include <utility>
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

class StubEnvironment final : public Environment {
public:
    StubEnvironment() {
        stage = Stage::Compute;
    }
    u64 ReadInstruction(u32) override {
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    TextureType ReadTextureType(u32) override {
        return {};
    }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 0;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

size_t Count(const std::vector<u32>& code, u32 opcode) {
    size_t n = 0;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        REQUIRE(count > 0);
        n += (code[i] & 0xffff) == opcode;
        i += count;
    }
    return n;
}

// SPIR-V opcodes the cases look for in the first module, to show the flag was emitted from
// its parent rather than skipped.
constexpr u32 OpIEqual = 170;
constexpr u32 OpSGreaterThanEqual = 175;
constexpr u32 OpSLessThan = 177;
constexpr u32 OpSLessThanEqual = 179;
constexpr u32 OpIAddCarry = 149;
constexpr u32 OpImageSparseFetch = 313;
constexpr u32 OpImageSparseTexelsResident = 316;

struct Emitted {
    std::vector<u32> first;
    std::vector<u32> second;
    bool ir_kept; ///< Each emission left every instruction's opcode, arguments, flags and use
                  ///< count as it found them
};

struct InstState {
    IR::Opcode opcode;
    std::vector<IR::Value> args;
    u32 flags;
    int uses;
    bool operator==(const InstState&) const = default;
};

std::vector<InstState> Snapshot(IR::Block& block) {
    std::vector<InstState> insts;
    for (const IR::Inst& inst : block.Instructions()) {
        InstState& state{insts.emplace_back()};
        state.opcode = inst.GetOpcode();
        for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
            state.args.push_back(inst.Arg(arg));
        }
        state.flags = inst.Flags<u32>();
        state.uses = inst.UseCount();
    }
    return insts;
}

// One compute block: a and b read from a constant buffer, `build` makes a value from them, and
// the value is written to global memory. The program is emitted twice.
template <typename Build>
Emitted EmitTwice(Build&& build, bool texture = false) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const IR::U32 a{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x110))};
    const IR::U32 b{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x114))};
    const IR::U32 value{build(ir, a, b)};
    const IR::U64 address{ir.PackUint2x32(ir.CompositeConstruct(a, b))};
    ir.WriteGlobal32(address, value);

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    StubEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    Optimization::CollectShaderInfoPass(env, program);
    program.info.native_global_bounds = true;
    program.info.constant_buffer_descriptors = {{0, 1}};
    if (texture) {
        program.info.texture_descriptors = {{.type = TextureType::Color2D, .count = 1}};
    }

    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    Emitted emitted;
    const auto before{Snapshot(block)};
    emitted.first = Backend::SPIRV::EmitSPIRV(profile, program);
    const auto after_first{Snapshot(block)};
    emitted.second = Backend::SPIRV::EmitSPIRV(profile, program);
    emitted.ir_kept = after_first == before && Snapshot(block) == before;
    return emitted;
}

} // Anonymous namespace

TEST_CASE("Emitting one program twice gives the same SPIR-V", "[spirv_emission]") {
    // An add whose carry is read (a 64-bit address's low half, IADD.CC): the carry is a
    // pseudo-operation the emission of the add defines.
    const Emitted emitted{EmitTwice([](IR::IREmitter& ir, const IR::U32& a, const IR::U32& b) {
        const IR::U32 sum{ir.IAdd(a, b)};
        const IR::U1 carry{ir.GetCarryFromOp(sum)};
        return IR::U32{ir.Select(carry, ir.Imm32(1), sum)};
    })};
    INFO("first: " << emitted.first.size() << " words, " << Count(emitted.first, OpIAddCarry)
                   << " OpIAddCarry; second: " << emitted.second.size() << " words, "
                   << Count(emitted.second, OpIAddCarry) << " OpIAddCarry");
    CHECK(Count(emitted.first, OpIAddCarry) == 1);
    CHECK(emitted.ir_kept);
    REQUIRE(emitted.first == emitted.second);
}

TEST_CASE("Emitting twice keeps an add's zero, sign and overflow flags", "[spirv_emission]") {
    const auto flag = [](IR::Opcode opcode) {
        return [opcode](IR::IREmitter& ir, const IR::U32& a, const IR::U32& b) {
            const IR::U32 sum{ir.IAdd(a, b)};
            const IR::U1 set{opcode == IR::Opcode::GetZeroFromOp   ? ir.GetZeroFromOp(sum)
                             : opcode == IR::Opcode::GetSignFromOp ? ir.GetSignFromOp(sum)
                                                                   : ir.GetOverflowFromOp(sum)};
            return IR::U32{ir.Select(set, ir.Imm32(1), sum)};
        };
    };
    SECTION("zero") {
        const Emitted emitted{EmitTwice(flag(IR::Opcode::GetZeroFromOp))};
        CHECK(Count(emitted.first, OpIEqual) >= 1);
        CHECK(emitted.ir_kept);
        REQUIRE(emitted.first == emitted.second);
    }
    SECTION("sign") {
        const Emitted emitted{EmitTwice(flag(IR::Opcode::GetSignFromOp))};
        CHECK(Count(emitted.first, OpSLessThan) >= 1);
        CHECK(emitted.ir_kept);
        REQUIRE(emitted.first == emitted.second);
    }
    SECTION("overflow") {
        const Emitted emitted{EmitTwice(flag(IR::Opcode::GetOverflowFromOp))};
        CHECK(Count(emitted.first, OpSGreaterThanEqual) >= 1);
        CHECK(emitted.ir_kept);
        REQUIRE(emitted.first == emitted.second);
    }
}

TEST_CASE("Emitting twice keeps every flag of one add", "[spirv_emission]") {
    const Emitted emitted{EmitTwice([](IR::IREmitter& ir, const IR::U32& a, const IR::U32& b) {
        const IR::U32 sum{ir.IAdd(a, b)};
        const IR::U1 zero{ir.GetZeroFromOp(sum)};
        const IR::U1 sign{ir.GetSignFromOp(sum)};
        const IR::U1 carry{ir.GetCarryFromOp(sum)};
        const IR::U1 overflow{ir.GetOverflowFromOp(sum)};
        const IR::U1 any{ir.LogicalOr(ir.LogicalOr(zero, sign), ir.LogicalOr(carry, overflow))};
        return IR::U32{ir.Select(any, ir.Imm32(1), sum)};
    })};
    CHECK(Count(emitted.first, OpIAddCarry) == 1);
    CHECK(emitted.ir_kept);
    REQUIRE(emitted.first == emitted.second);
}

TEST_CASE("Emitting twice keeps a fetch's residency", "[spirv_emission]") {
    const Emitted emitted{EmitTwice(
        [](IR::IREmitter& ir, const IR::U32& a, const IR::U32& b) {
            IR::TextureInstInfo info{};
            info.type.Assign(TextureType::Color2D);
            info.descriptor_index.Assign(0);
            const IR::Value color{ir.ImageFetch(ir.Imm32(0), ir.CompositeConstruct(a, b),
                                                IR::Value{}, ir.Imm32(0), IR::U32{}, info)};
            // As the texture pass leaves it, bound to descriptor 0.
            color.InstRecursive()->ReplaceOpcode(IR::Opcode::ImageFetch);
            const IR::U1 resident{ir.GetSparseFromOp(color)};
            const IR::U32 x{ir.BitCast<IR::U32>(IR::F32{ir.CompositeExtract(color, 0)})};
            return IR::U32{ir.Select(resident, x, ir.Imm32(0))};
        },
        true)};
    CHECK(Count(emitted.first, OpImageSparseFetch) == 1);
    CHECK(Count(emitted.first, OpImageSparseTexelsResident) == 1);
    CHECK(emitted.ir_kept);
    REQUIRE(emitted.first == emitted.second);
}

TEST_CASE("Emitting twice keeps a shuffle's in-bounds flag", "[spirv_emission]") {
    const Emitted emitted{EmitTwice([](IR::IREmitter& ir, const IR::U32& a, const IR::U32& b) {
        const IR::U32 shuffled{ir.ShuffleIndex(b, a, ir.Imm32(31), ir.Imm32(0))};
        const IR::U1 in_bounds{ir.GetInBoundsFromOp(shuffled)};
        return IR::U32{ir.Select(in_bounds, shuffled, ir.Imm32(0))};
    })};
    CHECK(Count(emitted.first, OpSLessThanEqual) >= 1);
    CHECK(emitted.ir_kept);
    REQUIRE(emitted.first == emitted.second);
}
