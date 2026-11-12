// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_list_macro_trace.h"

namespace {

std::string ReadDump(const std::filesystem::path& directory) {
    for (const auto& file : std::filesystem::directory_iterator{directory}) {
        std::ifstream stream{file.path()};
        std::stringstream text;
        text << stream.rdbuf();
        return text.str();
    }
    return {};
}

} // Anonymous namespace

TEST_CASE("ListMacroTrace: a macro call is noted only when a GPU-written entry fed it",
          "[video_core]") {
    namespace Trace = VideoCommon::ListMacroTrace;
    const auto directory = std::filesystem::temp_directory_path() / "lichii-list-macro-trace";
    std::filesystem::remove_all(directory);
#ifdef _WIN32
    _putenv_s("EDEN_NX2_DEBUG_DIR", directory.string().c_str());
#else
    setenv("EDEN_NX2_DEBUG_DIR", directory.string().c_str(), 1);
#endif
    const std::vector<u32> code(9, 0);
    const std::vector<u32> parameters{7, 8, 9};
    const std::vector<u32> data_ram(0xc00);
    int engine{};
    int other_engine{};
    const auto begin_call = [&](const void* object) {
        return Trace::BeginCall(Trace::Engine::Maxwell3D, object, 0xe62, code, 1, 0, parameters,
                                data_ram);
    };
    CHECK(begin_call(&engine) == nullptr);

    // The entry after the macro's first word holds its other two, read once the host GPU copied it.
    Trace::BeginEntry(Trace::Path::Waited, 1000, Trace::Engine::Maxwell3D, &engine, 0xe62, 2, 2);
    Trace::Call* const call = begin_call(&engine);
    REQUIRE(call != nullptr);
    CHECK(call->capture);
    Trace::NoteRead(call, 0x35e, 5);
    Trace::NoteWrite(call, 0x5f8, 8);
    Trace::EndCall(*call, false);
    Trace::EndEntry(nullptr, 0);
    const std::string dump = ReadDump(directory);
    CHECK(dump.find("3D macro") == 0);
    CHECK(dump.find("call 1: method 0xe62, 3 parameters 00000007 00000008 00000009, GPU-written: "
                    "1,2\n") != std::string::npos);
    CHECK(dump.find("registers read: 0x35e=0x5") != std::string::npos);
    CHECK(dump.find("methods written: 0x5f8=0x8") != std::string::npos);
    CHECK(begin_call(&engine) == nullptr);

    // A call begun inside an entry that ended with two of its words due: it takes the entry when
    // it runs later, on its engine only.
    Trace::BeginEntry(Trace::Path::Copied, 0, Trace::Engine::Maxwell3D, &engine, 0, 0, 4);
    Trace::EndEntry(&engine, 2);
    CHECK(begin_call(&other_engine) == nullptr);
    Trace::Call* const carried = begin_call(&engine);
    REQUIRE(carried != nullptr);
    Trace::EndCall(*carried, true);
    CHECK(begin_call(&engine) == nullptr);

    // Only the middle word from a GPU-written entry: due 2 at its start, 1 at its end.
    Trace::BeginEntry(Trace::Path::Copied, 0, Trace::Engine::Maxwell3D, &engine, 0xe62, 2, 1);
    Trace::EndEntry(&engine, 1);
    Trace::Call* const middle = begin_call(&engine);
    REQUIRE(middle != nullptr);
    Trace::EndCall(*middle, false);
    const std::string samples = ReadDump(directory);
    CHECK(samples.find("call 2: method 0xe62, 3 parameters 00000007 00000008 00000009, "
                       "GPU-written: 0\n") != std::string::npos);
    CHECK(samples.find("call 3: method 0xe62, 3 parameters 00000007 00000008 00000009, "
                       "GPU-written: 1\n") != std::string::npos);
    std::filesystem::remove_all(directory);
}
