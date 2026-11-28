// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

TEST_CASE("SPIR-V phi operands stay defined after dead code elimination",
          "[video_core][spirv][phi]") {
    using namespace Shader;
    using NodeType = IR::AbstractSyntaxNode::Type;
    enum class Operand { Added, Replaced, Missing };
    const auto operand = GENERATE(Operand::Added, Operand::Replaced, Operand::Missing);
    CAPTURE(static_cast<int>(operand));

    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, body{pool}, merge{pool};
    entry.AddBranch(&body);
    entry.AddBranch(&merge);
    body.AddBranch(&merge);
    IR::IREmitter start{entry}, branch{body};
    entry.AppendNewInst(IR::Opcode::Identity, {start.Imm1(false)});
    const IR::Value identity{&entry.back()};
    const auto predicate = branch.IEqual(branch.Imm32(1U), branch.Imm32(1U));

    merge.AppendNewInst(IR::Opcode::Phi, {});
    auto& phi = merge.back();
    phi.SetFlags(IR::Type::U1);
    if (operand == Operand::Replaced) {
        phi.AddPhiOperand(&entry, start.Imm1(true));
        phi.SetArg(0, identity);
    } else {
        phi.AddPhiOperand(&entry, operand == Operand::Missing ? IR::Value{} : identity);
    }
    phi.AddPhiOperand(&body, predicate);
    merge.AppendNewInst(IR::Opcode::SelectF32, {IR::Value{&phi}, IR::Value{1.0f}, IR::Value{0.0f}});
    merge.AppendNewInst(IR::Opcode::SetFragDepth, {IR::Value{&merge.back()}});

    IR::Program program;
    program.stage = Stage::Fragment;
    program.info.stores_frag_depth = true;
    program.blocks = {&entry, &body, &merge};
    program.post_order_blocks = {&merge, &body, &entry};
    program.syntax_list = {
        {{.block = &entry}, NodeType::Block},
        {{.if_node{start.Imm1(true), &body, &merge}}, NodeType::If},
        {{.block = &body}, NodeType::Block},
        {{.end_if{&merge}}, NodeType::EndIf},
        {{.block = &merge}, NodeType::Block},
        {{}, NodeType::Return},
    };
    Optimization::DeadCodeEliminationPass(program);

    Profile profile{};
    profile.supported_spirv = 0x00010300;
    if (operand == Operand::Missing) {
        REQUIRE_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, program), LogicError);
        return;
    }
    const auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    u32 false_id{};
    size_t phis{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        const auto opcode = static_cast<spv::Op>(code[offset] & 0xffffU);
        if (opcode == spv::Op::OpConstantFalse) {
            REQUIRE(size == 3);
            false_id = code[offset + 2];
        } else if (opcode == spv::Op::OpPhi) {
            REQUIRE(size == 7);
            REQUIRE(false_id != 0);
            CHECK(code[offset + 3] == false_id);
            CHECK(code[offset + 4] == entry.Definition<u32>());
            CHECK(code[offset + 5] == predicate.Inst()->Definition<u32>());
            CHECK(code[offset + 6] == body.Definition<u32>());
            ++phis;
        }
        offset += size;
    }
    CHECK(phis == 1);
}
