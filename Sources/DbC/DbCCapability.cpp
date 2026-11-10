// SPDX-License-Identifier: BSD-2-Clause
#include "DbCCapability.hpp"

namespace DbC {

Status findCapability(const RegisterSpace &registers, Capability &capability)
{
    capability = {};

    if (!registers.read32 || registers.length < 0x20) {
        return Status::InvalidArgument;
    }

    auto read = [&](uint32_t offset) {
        return registers.read32(registers.context, offset);
    };

    uint32_t operational = read(0) & 0xFF;
    if (operational < 0x20 || operational > registers.length - 8) {
        return Status::InvalidArgument;
    }

    if (read(operational + 4) & (1U << 11)) {
        return Status::Unavailable;
    }

    uint32_t parameters = read(0x10);
    uint32_t offset = (parameters >> 16) * 4;

    while (offset) {
        if (offset < 0x20 || offset > registers.length - 4) {
            return Status::InvalidArgument;
        }

        uint32_t header = read(offset);
        if (DCID::CapabilityID::decode(header) == DCID::kDebugCapabilityID) {
            if (offset > registers.length - Register::kSize || DCID::ERSTMax::decode(header) > 15) {
                return Status::InvalidArgument;
            }

            capability = {
                offset,
                operational,
                uint8_t(parameters & 1 ? 64 : 32),
                uint8_t(DCID::ERSTMax::decode(header)),
            };

            return Status::Success;
        }

        uint32_t next = DCID::NextCapability::decode(header) * 4;
        if (!next) {
            break;
        }

        if (offset > UINT32_MAX - next) {
            return Status::InvalidArgument;
        }

        offset += next;
    }

    return Status::Unavailable;
}
}
