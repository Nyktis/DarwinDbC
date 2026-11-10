// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "DbCCapability.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace DbC {

class DarwinPlatform final : public Platform {
public:
    struct Information {
        Capability capability;
        uint64_t registerLength;
        uint64_t dmaAddress;
        uint64_t dmaLength;
        bool usesIOMapper;
    };

    DarwinPlatform() = default;
    DarwinPlatform(const DarwinPlatform &) = delete;
    DarwinPlatform &operator=(const DarwinPlatform &) = delete;

    Status initialize(IOPCIDevice *device);
    void release();

    Memory memory() const;
    uint8_t addressBits() const;
    Status copyInformation(Information &information) const;
    void setPowerAvailable(bool available);

    bool isReady() override;
    uint32_t read32(uint32_t offset) override;
    void write32(uint32_t offset, uint32_t value) override;
    void synchronizeDMA(DMADirection direction) override;
    uint64_t timeMicroseconds() override;
    void pause() override;

private:
    IOPCIDevice *device{};
    IOMemoryMap *registerMapping{};
    IOBufferMemoryDescriptor *descriptor{};
    IODMACommand *command{};
    IOMapper *mapper{};
    volatile uint32_t *registers{};
    Capability capability{};
    uint64_t dmaAddress{};
    bool powerAvailable{};

    static uint32_t readBAR32(void *context, uint32_t offset);
};
}
