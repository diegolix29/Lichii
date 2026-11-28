// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Exceptions the C++ library throws from inside itself must reach a handler for their type.
// With -fno-rtti on Apple, every TU had its own hidden std typeinfo and libc++abi compares
// typeinfo by address, so these fell through to catch (...).

#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {

enum class Caught { ByType, Other, Nothing };

Caught StoiOfText() {
    try {
        static_cast<void>(std::stoi("xyz"));
    } catch (const std::logic_error&) {
        return Caught::ByType;
    } catch (...) {
        return Caught::Other;
    }
    return Caught::Nothing;
}

Caught StoiOfTextExactly() {
    try {
        static_cast<void>(std::stoi("xyz"));
    } catch (const std::invalid_argument&) {
        return Caught::ByType;
    } catch (...) {
        return Caught::Other;
    }
    return Caught::Nothing;
}

Caught StreamFailure() {
    try {
        std::istringstream stream{"xyz"};
        stream.exceptions(std::ios::failbit);
        int value{};
        stream >> value;
    } catch (const std::ios_base::failure&) {
        return Caught::ByType;
    } catch (...) {
        return Caught::Other;
    }
    return Caught::Nothing;
}

} // Anonymous namespace

TEST_CASE("Exceptions thrown inside the C++ library are caught by their type", "[common]") {
    CHECK(StoiOfText() == Caught::ByType);
    CHECK(StoiOfTextExactly() == Caught::ByType);
    CHECK(StreamFailure() == Caught::ByType);
}
