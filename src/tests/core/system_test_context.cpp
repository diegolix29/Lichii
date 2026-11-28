// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <memory>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "tests/core/system_test_context.h"

#ifndef _WIN32
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

TEST_CASE("System test contexts alive together each have a folder of their own",
          "[core][system_test_context]") {
    const auto nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    std::vector<std::unique_ptr<ScopedSystemTestContext>> contexts;
    std::set<std::filesystem::path> directories;
    for (int i = 0; i < 64; ++i) {
        contexts.push_back(std::make_unique<ScopedSystemTestContext>());
        const auto& directory = contexts.back()->Directory();
        CHECK(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) == directory);
        directories.insert(directory);
    }
    CHECK(directories.size() == contexts.size());
    for (const auto& context : contexts) {
        CHECK(std::filesystem::is_directory(context->Directory()));
    }
    while (!contexts.empty()) {
        contexts.pop_back();
    }
    for (const auto& directory : directories) {
        CHECK_FALSE(std::filesystem::exists(directory));
    }
    CHECK(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) == nand);
}

#ifndef _WIN32
TEST_CASE("System test contexts of tests processes started together do not collide",
          "[core][system_test_context]") {
    // Each child makes and drops contexts as fast as it can once all are released at once. A
    // context throws if its folder cannot be made, and exits 2 if the folder is not there once
    // made. The children use no Catch2 macro of their own; the context's destructor still runs
    // RestoreEdenPath's CHECK there, whose failure a child cannot report.
    constexpr int Children = 16;
    constexpr int Rounds = 2000;
    int release[2]{};
    REQUIRE(pipe(release) == 0);
    std::vector<pid_t> children;
    bool forked = true;
    for (int i = 0; i < Children; ++i) {
        const pid_t child = fork();
        if (child == 0) {
            close(release[1]);
            char received{};
            ssize_t got;
            do {
                got = read(release[0], &received, 1);
            } while (got < 0 && errno == EINTR);
            if (got != 0) {
                _exit(3);
            }
            int result = 0;
            try {
                for (int round = 0; round < Rounds && result == 0; ++round) {
                    ScopedSystemTestContext context;
                    if (!std::filesystem::is_directory(context.Directory())) {
                        result = 2; // Leaves the loop after the context has removed its folder
                    }
                }
            } catch (...) {
                result = 1;
            }
            _exit(result);
        }
        if (child < 0) {
            forked = false;
            break;
        }
        children.push_back(child);
    }
    // Closing the write end releases the children, also those of a fork that failed partway, so
    // that they are reaped here rather than left to run on.
    close(release[0]);
    close(release[1]);
    int failed = 0;
    for (const pid_t child : children) {
        int status{};
        pid_t reaped;
        do {
            reaped = waitpid(child, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        if (reaped != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            ++failed;
        }
    }
    REQUIRE(forked);
    CHECK(failed == 0);
}
#endif
