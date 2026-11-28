// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"

namespace Tests {

/// A process with one page of guest memory at PageAddress, made the application process for its
/// lifetime: Host1x devices' threads start from System::ApplicationMemory(), which needs one.
/// Declare it before anything whose threads use it, so those are destroyed first.
class Host1xTestProcess {
public:
    static constexpr u64 PageAddress = 0x4000;

    explicit Host1xTestProcess(Core::System& system) : kernel{system.Kernel()}, process{kernel} {
        auto& table = process.GetPageTable().GetImpl();
        table.Resize(20, 12);
        page = system.DeviceMemory().buffer.BackingBasePointer();
        table.entries.GetAndFault(PageAddress >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(page) - PageAddress);
        process.GetMemory().SetCurrentPageTable(process);
        // Never created as a kernel object, so its count stays 0: the kernel's open and close of
        // its application process neither keep nor destroy it.
        kernel.SetApplicationProcess(&process);
    }

    ~Host1xTestProcess() {
        kernel.SetApplicationProcess(nullptr);
        kernel.UnregisterKernelObject(&process);
    }

    Host1xTestProcess(const Host1xTestProcess&) = delete;
    Host1xTestProcess& operator=(const Host1xTestProcess&) = delete;

    Core::Memory::Memory& Memory() {
        return process.GetMemory();
    }

    /// The host bytes behind PageAddress.
    u8* Page() const {
        return page;
    }

private:
    Kernel::KernelCore& kernel;
    Kernel::KProcess process;
    u8* page{};
};

} // namespace Tests
