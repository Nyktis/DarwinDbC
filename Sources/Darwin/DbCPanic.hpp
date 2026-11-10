// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "DbCController.hpp"
#include <Headers/kern_patcher.hpp>

namespace DbC {

// Fatal debugger output uses resident memory and synchronous hardware polling.
class PanicConsole final {
public:
    static bool route(KernelPatcher &patcher);
    static bool isReady();
    static bool isActive();
    static bool hasBuffer();
    static Status initialize(Controller *controller, Platform *platform);
    static Status release();
    static void write(const void *bytes, size_t length, uintptr_t caller = 0);
};
}
