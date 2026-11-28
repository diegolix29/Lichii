// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
using IR::Opcode;

class R2PEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::logic_error("Unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { throw std::logic_error("Unexpected cbuf read"); }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// R2P PR, R<src>, <mask>: register to predicates, byte 0.
constexpr u64 R2PImm(u32 src, u32 mask) {
    return 0x38f0000000000000ULL | (u64{src} << 8) | (u64{mask} << 20);
}
constexpr u64 R2PReg(u32 src, u32 mask_reg) {
    return 0x5cf0000000000000ULL | (u64{src} << 8) | (u64{mask_reg} << 20);
}

struct R2PProgram {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    R2PEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};

    void Optimize() {
        IR::Program program;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        Shader::Optimization::ConstantPropagationPass(env, program);
    }

    std::vector<IR::Pred> WrittenPredicates() const {
        std::vector<IR::Pred> preds;
        for (const IR::Inst& inst : block.Instructions()) {
            if (inst.GetOpcode() == Opcode::SetPred) {
                preds.push_back(inst.Arg(0).Pred());
            }
        }
        return preds;
    }

    size_t Count(Opcode opcode) const {
        return static_cast<size_t>(std::ranges::count_if(
            block.Instructions(), [opcode](const IR::Inst& inst) {
                return inst.GetOpcode() == opcode && inst.HasUses();
            }));
    }
};
} // Anonymous namespace

TEST_CASE("R2P with an immediate mask writes only the predicates it selects",
          "[shader][maxwell][r2p]") {
    for (const u32 mask : {0x40U, 0x04U, 0x05U, 0x7fU, 0x00U}) {
        INFO("mask " << mask);
        R2PProgram program;
        program.visitor.R2P_imm(R2PImm(1, mask));
        program.Optimize();
        std::vector<IR::Pred> expected;
        for (u32 index = 0; index < 7; ++index) {
            if ((mask >> index) & 1) {
                expected.push_back(IR::Pred{index});
            }
        }
        REQUIRE(program.WrittenPredicates() == expected);
        // No select keeps an unselected predicate, and nothing reads bits of the mask.
        REQUIRE(program.Count(Opcode::SelectU1) == 0);
        REQUIRE(program.Count(Opcode::BitFieldUExtract) == expected.size());
    }
}

TEST_CASE("R2P with a register mask keeps the predicates the mask leaves out",
          "[shader][maxwell][r2p]") {
    R2PProgram program;
    program.visitor.R2P_reg(R2PReg(1, 2));
    program.Optimize();
    REQUIRE(program.WrittenPredicates().size() == 7);
    REQUIRE(program.Count(Opcode::SelectU1) == 7);
}
