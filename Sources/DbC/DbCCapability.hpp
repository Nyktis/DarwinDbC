// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "DbCController.hpp"

namespace DbC {

struct RegisterSpace {
    void *context;
    uint32_t (*read32)(void *context, uint32_t offset);
    size_t length;
};

struct Capability {
    uint32_t offset;
    uint32_t operationalOffset;
    uint8_t addressBits;
    uint8_t erstMax;
};

// Reads the xHCI capability chain inside the mapped BAR, xHCI 1.2c §§5.3.6, 7.1.
Status findCapability(const RegisterSpace &registers, Capability &capability);
}
