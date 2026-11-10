// SPDX-License-Identifier: BSD-2-Clause
#include "DbCDarwinPlatform.hpp"

#include <kern/clock.h>
#include <mach/mach_time.h>
#include <IOKit/IOLib.h>

#if !defined(__x86_64__)
#error The Darwin platform currently targets x86_64
#endif

namespace DbC {

Status DarwinPlatform::initialize(IOPCIDevice *provider)
{
    if (device || !provider) {
        return Status::InvalidState;
    }

    if ((provider->configRead32(kIOPCIConfigRevisionID) >> 8) != 0x0C0330 ||
        !(provider->configRead16(kIOPCIConfigCommand) & kIOPCICommandBusMaster)) {
        return Status::Unavailable;
    }

    device = provider;
    device->retain();

    registerMapping = device->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);

    if (!registerMapping) {
        release();
        return Status::Unavailable;
    }

    registers = reinterpret_cast<volatile uint32_t *>(registerMapping->getVirtualAddress());

    RegisterSpace space{
        this,
        readBAR32,
        static_cast<size_t>(registerMapping->getLength()),
    };
    Status status = findCapability(space, capability);

    if (status != Status::Success) {
        release();
        return status;
    }

    if (readBAR32(this, capability.offset + Register::kDCCTRL) & DCCTRL::kEnable) {
        release();
        return Status::Busy;
    }

    descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryHostPhysicallyContiguous,
        Layout::kLength,
        Layout::kAlignment);

    if (!descriptor) {
        release();
        return Status::Unavailable;
    }

    mapper = IOMapper::copyMapperForDevice(device);
    command = IODMACommand::withSpecification(IODMACommand::OutputHost64,
        capability.addressBits,
        Layout::kLength,
        IODMACommand::kMapped,
        Layout::kLength,
        Layout::kAlignment,
        mapper);

    if (!command || command->setMemoryDescriptor(descriptor) != kIOReturnSuccess) {
        release();
        return Status::Unavailable;
    }

    IODMACommand::Segment64 segment{};
    UInt64 offset = 0;
    UInt32 count = 1;
    IOReturn result = command->gen64IOVMSegments(&offset, &segment, &count);

    // Polling accesses the wired descriptor directly throughout fault handling.
    // A secondary DMA buffer would require synchronization that can allocate or block.
    if (result != kIOReturnSuccess || count != 1 || offset != Layout::kLength ||
        segment.fLength != Layout::kLength || (segment.fIOVMAddr & (Layout::kAlignment - 1)) ||
        command->getIOMemoryDescriptor() != descriptor) {
        release();
        return Status::Unavailable;
    }

    dmaAddress = segment.fIOVMAddr;
    setPowerAvailable(true);

    return Status::Success;
}

void DarwinPlatform::release()
{
    // The owner closes Controller before releasing its device-address mapping.
    setPowerAvailable(false);

    if (command) {
        command->clearMemoryDescriptor();
        command->release();
        command = nullptr;
    }

    if (mapper) {
        mapper->release();
        mapper = nullptr;
    }

    if (descriptor) {
        descriptor->release();
        descriptor = nullptr;
    }

    if (registerMapping) {
        registerMapping->release();
        registerMapping = nullptr;
    }

    if (device) {
        device->release();
        device = nullptr;
    }

    registers = nullptr;
    capability = {};
    dmaAddress = 0;
}

Memory DarwinPlatform::memory() const
{
    return {descriptor->getBytesNoCopy(), dmaAddress, Layout::kLength};
}

uint8_t DarwinPlatform::addressBits() const
{
    return capability.addressBits;
}

Status DarwinPlatform::copyInformation(Information &information) const
{
    information = {};

    if (!descriptor || !command || !registerMapping || !dmaAddress) {
        return Status::InvalidState;
    }

    information = {
        capability,
        registerMapping->getLength(),
        dmaAddress,
        descriptor->getLength(),
        mapper != nullptr,
    };

    return Status::Success;
}

void DarwinPlatform::setPowerAvailable(bool available)
{
    __atomic_store_n(&powerAvailable, available, __ATOMIC_RELEASE);
}

bool DarwinPlatform::isReady()
{
    if (!__atomic_load_n(&powerAvailable, __ATOMIC_ACQUIRE)) {
        return false;
    }

    uint32_t status = readBAR32(this, capability.operationalOffset + 4);
    return !(status & (1U << 11));
}

uint32_t DarwinPlatform::readBAR32(void *context, uint32_t offset)
{
    auto &platform = *static_cast<DarwinPlatform *>(context);
    uint32_t value = platform.registers[offset / sizeof(uint32_t)];

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return value;
}

uint32_t DarwinPlatform::read32(uint32_t offset)
{
    return readBAR32(this, capability.offset + offset);
}

void DarwinPlatform::write32(uint32_t offset, uint32_t value)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    registers[(capability.offset + offset) / sizeof(uint32_t)] = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void DarwinPlatform::synchronizeDMA(DMADirection)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint64_t DarwinPlatform::timeMicroseconds()
{
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);

    return nanoseconds / 1000;
}

void DarwinPlatform::pause()
{
    __asm__ volatile("pause" ::: "memory");
}
}
