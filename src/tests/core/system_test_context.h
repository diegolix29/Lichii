// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "tests/common/restore_eden_path.h"
#include "tests/common/temp_dir.h"

class ScopedSystemTestContext {
public:
    ScopedSystemTestContext() {
        // The SD card folder is made before either path is redirected: a constructor that throws
        // does not run the destructor, so nothing may be left redirected if it fails (the folder
        // itself goes with `directory`).
        std::error_code error;
        if (!std::filesystem::create_directory(directory.Path() / "sdmc", error)) {
            throw std::runtime_error("Cannot create isolated System test directory");
        }
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, directory.Path());
        // Core::System's Reporter empties sdmc/FsAccessLog.txt when it is made: keep that out of
        // the user's SD card folder too.
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, directory.Path() / "sdmc");
        Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
        Settings::values.use_multi_core.SetValue(false);
    }

    ~ScopedSystemTestContext() {
        Tests::RestoreEdenPath(Common::FS::EdenPath::NANDDir, previous_nand);
        Tests::RestoreEdenPath(Common::FS::EdenPath::SDMCDir, previous_sdmc);
        Settings::values.current_user = previous_user;
        Settings::values.memory_layout_mode.SetValue(previous_memory);
        Settings::values.use_multi_core.SetValue(previous_multicore);
    }

    ScopedSystemTestContext(const ScopedSystemTestContext&) = delete;
    ScopedSystemTestContext& operator=(const ScopedSystemTestContext&) = delete;

    const std::filesystem::path& Directory() const {
        return directory.Path();
    }

private:
    const std::filesystem::path previous_nand =
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    const std::filesystem::path previous_sdmc =
        Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    const s32 previous_user = Settings::values.current_user.GetValue();
    const Settings::MemoryLayout previous_memory = Settings::values.memory_layout_mode.GetValue();
    const bool previous_multicore = Settings::values.use_multi_core.GetValue();
    // A random name made with an exclusive create, retried while taken (Tests::TempDir). A name
    // from steady_clock was taken twice where the clock moves in steps (about 42 ns with macOS's
    // libc++): by contexts made back to back, and by tests processes started together, and the
    // second context failed. Declared last, so it is removed after the destructor has restored
    // the paths.
    const Tests::TempDir directory{"hwacod-system-test"};
};
