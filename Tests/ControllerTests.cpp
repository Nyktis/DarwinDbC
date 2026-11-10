// SPDX-License-Identifier: BSD-2-Clause
#include "DbCCapability.hpp"
#include "DbCConsole.hpp"
#include "DbCPanic.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace DbC;

static void require(bool condition, const char *expression, unsigned line)
{
    if (condition) {
        return;
    }

    std::string message = "line " + std::to_string(line) + ": " + expression;
    throw std::runtime_error(message);
}

#define REQUIRE(value) require((value), #value, __LINE__)

class DeviceModel final : public Platform {
public:
    DeviceModel()
    {
        if (posix_memalign(&memory, Layout::kAlignment, Layout::kLength)) {
            throw std::bad_alloc();
        }

        std::memset(memory, 0xA5, Layout::kLength);
        registers[0] = 10;
        registers[8] = (3U << 16) | 0x220;
        registers[1] = 0x00800000;
    }

    ~DeviceModel()
    {
        std::free(memory);
    }

    DeviceModel(const DeviceModel &) = delete;
    DeviceModel &operator=(const DeviceModel &) = delete;

    static constexpr uint64_t kDMA = 0x123400000ULL;

    void *memory{};
    std::array<uint32_t, 16> registers{};
    std::vector<std::pair<uint32_t, uint32_t>> writes;

    bool ready{true};
    bool connect{true};
    bool completes{true};
    bool refusesDisable{};
    bool frozenClock{};
    bool intelEndpoints{};

    uint64_t clock{};
    size_t pauses{};
    size_t doorbells{};
    size_t registerReads{};
    uint32_t ignoredWriteOffset{UINT32_MAX};

    std::function<void()> onPause;
    std::string incoming{"commands"};
    std::string outgoing;

    std::array<uint16_t, 2> consumers{};
    std::array<bool, 2> cycles{true, true};
    uint16_t eventProducer{};
    bool eventCycle{true};
    unsigned selectedDirection{};
    uint32_t eventResidual{};
    uint8_t completionCode{1};
    bool badPointer{};
    bool eventData{};

    bool isReady() override
    {
        return ready;
    }

    uint32_t read32(uint32_t offset) override
    {
        REQUIRE(ready);
        REQUIRE(offset < 64 && offset % 4 == 0);
        ++registerReads;

        return registers[offset / 4];
    }

    void write32(uint32_t offset, uint32_t value) override
    {
        REQUIRE(ready);
        REQUIRE(offset < 64 && offset % 4 == 0);
        writes.emplace_back(offset, value);

        if (offset == ignoredWriteOffset) {
            return;
        }

        if (offset == 0x20) {
            REQUIRE(!(value & 0xC));
            uint32_t old = registers[8];
            uint32_t control = (old & 0x7FFF0011) | (value & 0x8000FFE2);

            if (value & 16) {
                control &= ~16U;
            }

            if (refusesDisable && !(value & 0x80000000)) {
                control |= 0x80000000;
            }

            if (control & 0x80000000) {
                if (!(old & 0x80000000)) {
                    consumers = {};
                    cycles = {true, true};
                    eventProducer = 0;
                    eventCycle = true;
                    if (connect) {
                        control |= 1;
                    }
                }
            } else {
                control &= ~1U;
            }

            if ((old & 1) && !(control & 1)) {
                control |= 16;
            }

            registers[8] = control;
        } else if (offset == 0x28) {
            REQUIRE(!(value & 0xFF1DC20C));
            registers[10] = (registers[10] & ~(value & 0xE20000) & ~2U) | (value & 2);
        } else if (offset == 4) {
            ++doorbells;
            selectedDirection = (value >> 8) & 0xFF;
            REQUIRE(selectedDirection <= 1);
            if (completes) {
                completeTransfer(selectedDirection);
            }
        } else {
            registers[offset / 4] = value;
        }
    }

    void synchronizeDMA(DMADirection) override
    {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    uint64_t timeMicroseconds() override
    {
        return clock;
    }

    void pause() override
    {
        ++pauses;

        if (!frozenClock) {
            ++clock;
        }

        if (onPause) {
            onPause();
        }
    }

    Configuration configuration()
    {
        return {
            this,
            {memory, kDMA, Layout::kLength},
            {0xFFFF, 1, 0x100, "DarwinDbC", "Debug Console", "0001"},
            64,
        };
    }

    Wait wait()
    {
        return Wait::forDuration(*this, 10);
    }

    TRB *ring(size_t offset)
    {
        return reinterpret_cast<TRB *>(static_cast<uint8_t *>(memory) + offset);
    }

    void appendEvent(TRB event)
    {
        event.control = (event.control & ~1U) | (eventCycle ? 1 : 0);
        ring(0)[eventProducer] = event;
        if (++eventProducer == 256) {
            eventProducer = 0;
            eventCycle = !eventCycle;
        }
    }

    void completeTransfer(unsigned direction)
    {
        size_t ringOffset = direction ? 0x2000 : 0x1000;
        auto &index = consumers[direction];
        auto &cycle = cycles[direction];
        auto *entries = ring(ringOffset);

        if (index == 255) {
            REQUIRE(((entries[index].control >> 10) & 63) == 6);
            REQUIRE(entries[index].parameter == kDMA + ringOffset);
            REQUIRE((entries[index].control & 3) == (cycle ? 3U : 2U));
            index = 0;
            cycle = !cycle;
        }

        const auto &trb = entries[index];
        REQUIRE(((trb.control >> 10) & 63) == 1);
        REQUIRE(bool(trb.control & 1) == cycle);
        REQUIRE(trb.control & 32);

        uint32_t length = trb.status & 0x1FFFF;
        auto *buffer = static_cast<char *>(memory) + (trb.parameter - kDMA);
        REQUIRE(trb.parameter >= kDMA && trb.parameter + length <= kDMA + Layout::kLength);
        REQUIRE((trb.parameter & 0xFFFF) + length <= 0x10000);

        uint32_t residual = eventResidual;
        if (direction) {
            size_t amount = incoming.size() < length ? incoming.size() : length;
            std::memcpy(buffer, incoming.data(), amount);
            residual = length - uint32_t(amount);
        } else if (eventResidual <= length) {
            outgoing.append(buffer, length - eventResidual);
        }

        uint32_t endpoint = direction + (intelEndpoints ? 2 : 0);
        TRB event{
            kDMA + ringOffset + index * 16 + (badPointer ? 8 : 0),
            residual | (uint32_t(completionCode) << 24),
            (32U << 10) | (endpoint << 16) | (eventData ? 4U : 0U),
        };

        appendEvent(event);
        ++index;
    }
};

class Session {
public:
    DeviceModel device;
    Controller controller;

    Session()
    {
        REQUIRE(controller.initialize(device.configuration()) == Status::Success);
    }

    ~Session()
    {
        device.ready = true;
        device.refusesDisable = false;
        controller.close(device.wait());
        controller.release();
    }

    void open()
    {
        REQUIRE(controller.open(device.wait()) == Status::Success);
    }
};

static void lifecycle()
{
    Session session;
    REQUIRE(session.device.writes.empty());

    session.open();
    auto *context =
        reinterpret_cast<Context *>(static_cast<uint8_t *>(session.device.memory) + 0x3000);

    REQUIRE(context->out.dequeue == DeviceModel::kDMA + 0x1001);
    REQUIRE(context->in.dequeue == DeviceModel::kDMA + 0x2001);
    REQUIRE(context->out.information2 == 0x04000310);
    REQUIRE(context->in.information2 == 0x04000330);
    REQUIRE(session.device.registers[14] == 0xFFFF0000);
    REQUIRE(session.device.registers[15] == 0x01000001);
    REQUIRE(session.controller.release() == Status::Busy);

    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
    REQUIRE(session.device.registers[12] == 0);
    REQUIRE(session.device.registers[4] == 0);
    REQUIRE(session.device.registers[8] & 16);

    REQUIRE(session.controller.open(session.device.wait()) == Status::Success);
    REQUIRE(!(session.device.registers[8] & 16));
    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
    REQUIRE(session.controller.release() == Status::Success);
    REQUIRE(session.controller.release() == Status::Success);
    REQUIRE(session.controller.poll() == Status::InvalidState);
}

static void transfers()
{
    for (bool intel : {false, true}) {
        Session session;
        session.device.intelEndpoints = intel;
        session.open();

        for (unsigned i = 0; i < 800; ++i) {
            char data[] = {'A', '\0', 'B', '\n'};
            auto result = session.controller.write({data, sizeof(data), session.device.wait()});
            REQUIRE(result.status == Status::Success && result.transferred == 4 && !result.pending);
        }

        REQUIRE(session.device.outgoing.size() == 3200);
        char buffer[4]{};
        auto result = session.controller.read({buffer, 3, session.device.wait()});

        REQUIRE(result.status == Status::Success && result.transferred == 3);
        REQUIRE(std::memcmp(buffer, "com", 3) == 0);

        auto bells = session.device.doorbells;
        result = session.controller.read({buffer, 4, session.device.wait()});
        REQUIRE(result.transferred == 4 && std::memcmp(buffer, "mand", 4) == 0);
        REQUIRE(session.device.doorbells == bells);

        result = session.controller.read({buffer, 4, session.device.wait()});
        REQUIRE(result.transferred == 1 && buffer[0] == 's');

        Snapshot snapshot{};
        REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
        REQUIRE(snapshot.bytesWritten == 3200 && snapshot.bytesRead == 8);
    }
}

static void pendingOwnership()
{
    Session session;
    session.open();
    session.device.completes = false;

    char data[] = "old";
    auto result = session.controller.write({data, 3, session.device.wait()});
    REQUIRE(result.status == Status::Timeout && result.pending);

    std::memcpy(data, "new", 3);
    result = session.controller.write({data, 3, session.device.wait()});
    REQUIRE(result.status == Status::Busy && result.pending);
    REQUIRE(std::memcmp(static_cast<char *>(session.device.memory) + 0x4000, "old", 3) == 0);

    char readBuffer[8];
    result = session.controller.read({readBuffer, sizeof(readBuffer), session.device.wait()});
    REQUIRE(result.status == Status::Timeout && result.pending);

    session.device.completeTransfer(0);
    session.device.completeTransfer(1);
    REQUIRE(session.controller.poll() == Status::Success);
    result = session.controller.read({readBuffer, sizeof(readBuffer), session.device.wait()});
    REQUIRE(result.transferred == 8 && std::memcmp(readBuffer, "commands", 8) == 0);

    session.device.completes = true;
    result = session.controller.write({data, 3, session.device.wait()});
    REQUIRE(result.transferred == 3);
    REQUIRE(session.device.outgoing == "oldnew");

    Snapshot snapshot{};
    session.controller.copySnapshot(snapshot);
    REQUIRE(snapshot.lateWrites == 1 && snapshot.timeouts == 2);
}

static void boundsAndFaults()
{
    const std::array<Status, 5> expectedStatuses{
        Status::InvalidEvent,
        Status::InvalidEvent,
        Status::InvalidEvent,
        Status::TransferError,
        Status::ControllerReset,
    };

    for (unsigned mode = 0; mode < 5; ++mode) {
        Session session;
        session.open();

        session.device.badPointer = mode == 0;
        session.device.eventResidual = mode == 1 ? 20 : 0;
        session.device.eventData = mode == 2;
        session.device.completionCode = mode == 3 ? 6 : 1;
        session.device.completes = mode != 4;
        if (mode == 4) {
            session.device.onPause = [&] {
                session.device.registers[8] &= ~0x80000000U;
            };
        }

        auto result = session.controller.write({"hello", 5, session.device.wait()});

        REQUIRE(result.status == expectedStatuses[mode]);
        REQUIRE(session.controller.release() == Status::Busy);
        REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
        if (mode < 3) {
            Snapshot snapshot{};
            REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
            REQUIRE(snapshot.invalidEvents == 1);
            REQUIRE(snapshot.firstInvalidEventAddress == DeviceModel::kDMA);
            REQUIRE(snapshot.firstInvalidEvent.parameter ==
                DeviceModel::kDMA + 0x1000 + (mode == 0 ? 8 : 0));
            REQUIRE(
                snapshot.firstInvalidEvent.control == ((32U << 10) | (mode == 2 ? 4U : 0U) | 1U));
        }
        REQUIRE(session.controller.release() == Status::Success);
    }

    Session session;
    session.device.connect = false;
    session.device.frozenClock = true;

    REQUIRE(session.controller.open({UINT64_MAX, 3}) == Status::Timeout);
    REQUIRE(session.device.pauses == 3);

    session.device.refusesDisable = true;
    REQUIRE(session.controller.close({UINT64_MAX, 2}) == Status::Timeout);
    REQUIRE(session.controller.release() == Status::Busy);

    session.device.ready = false;
    auto writes = session.device.writes.size();
    REQUIRE(session.controller.close(session.device.wait()) == Status::Unavailable);
    REQUIRE(session.device.writes.size() == writes);
}

static void validationAndReentry()
{
    Session session;
    session.device.registers[8] |= 0x80000000;
    REQUIRE(session.controller.open(session.device.wait()) == Status::Busy);
    REQUIRE(session.device.writes.empty());

    session.device.registers[8] &= ~0x80000000U;
    session.open();

    auto result = session.controller.write({nullptr, 1, session.device.wait()});
    REQUIRE(result.status == Status::InvalidArgument);

    result = session.controller.write({"x", Layout::kBufferLength + 1, session.device.wait()});
    REQUIRE(result.status == Status::InvalidArgument);

    session.device.completes = false;
    session.device.onPause = [&] {
        REQUIRE(session.controller.poll() == Status::Busy);
        REQUIRE(session.controller.close(session.device.wait()) == Status::Busy);
        session.device.completeTransfer(0);
    };
    result = session.controller.write({"x", 1, session.device.wait()});
    REQUIRE(result.status == Status::Success);
    session.device.onPause = {};

    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
    REQUIRE(session.controller.release() == Status::Success);

    auto config = session.device.configuration();
    config.addressBits = 32;
    REQUIRE(session.controller.initialize(config) == Status::InvalidArgument);

    config = session.device.configuration();
    ++config.memory.deviceAddress;
    REQUIRE(session.controller.initialize(config) == Status::InvalidArgument);

    config = session.device.configuration();
    config.identity.product = "\xFF";
    REQUIRE(session.controller.initialize(config) == Status::InvalidArgument);
}

static void portChanges()
{
    Session session;
    session.open();

    session.device.registers[10] = 0xE20003;
    session.device.appendEvent({0, 1U << 24, 34U << 10});
    REQUIRE(session.controller.poll() == Status::Success);
    REQUIRE(session.device.registers[10] == 3);

    session.device.registers[8] = (session.device.registers[8] & ~1U) | 16;
    REQUIRE(session.controller.poll() == Status::Disconnected);
    Snapshot snapshot{};
    session.controller.copySnapshot(snapshot);
    REQUIRE(snapshot.state == State::Waiting);
    REQUIRE(!(session.device.registers[8] & DCCTRL::kRunChange));
    REQUIRE(session.device.registers[8] & DCCTRL::kEnable);
}

static void hostReconfiguration()
{
    Session session;
    session.device.connect = false;
    REQUIRE(session.controller.open(session.device.wait()) == Status::Timeout);

    // SetConfiguration(0) leaves idle rings waiting for the host to configure them.
    session.device.registers[8] |= DCCTRL::kRunChange;
    REQUIRE(session.controller.poll() == Status::Disconnected);
    REQUIRE(!(session.device.registers[8] & DCCTRL::kRunChange));
    REQUIRE(session.device.registers[8] & DCCTRL::kEnable);

    // The host can reset and configure the device between two target polls.
    session.device.registers[8] |= DCCTRL::kRun | DCCTRL::kRunChange;
    session.device.registers[10] = DCPORTSC::kConnected | DCPORTSC::kEnabled;
    REQUIRE(session.controller.poll() == Status::Success);
    REQUIRE(!(session.device.registers[8] & DCCTRL::kRunChange));
    REQUIRE(session.device.registers[8] & DCCTRL::kEnable);
    auto result = session.controller.write({"ready", 5, session.device.wait()});
    REQUIRE(result.status == Status::Success && result.transferred == 5);

    // Reconfiguration during an outstanding TD retains its DMA ownership.
    session.device.completes = false;
    result = session.controller.write({"pending", 7, session.device.wait()});
    REQUIRE(result.status == Status::Timeout && result.pending);
    session.device.registers[8] |= DCCTRL::kRunChange;
    REQUIRE(session.controller.poll() == Status::Disconnected);
    REQUIRE(session.controller.release() == Status::Busy);
    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
}

static void capabilityDiscovery()
{
    std::array<uint32_t, 256> words{};
    words[0] = 0x40;
    words[4] = (0x20U << 16) | 1;
    words[0x80 / 4] = 1 | (8U << 8);
    words[0xA0 / 4] = 10 | (4U << 16);

    RegisterSpace space{
        &words,
        [](void *context, uint32_t offset) {
            auto &data = *static_cast<std::array<uint32_t, 256> *>(context);
            REQUIRE(offset / 4 < data.size());
            return data[offset / 4];
        },
        sizeof(words),
    };

    Capability capability{};
    REQUIRE(findCapability(space, capability) == Status::Success);
    REQUIRE(capability.offset == 0xA0 && capability.operationalOffset == 0x40);
    REQUIRE(capability.addressBits == 64 && capability.erstMax == 4);

    words[0x44 / 4] = 1U << 11;
    REQUIRE(findCapability(space, capability) == Status::Unavailable);

    words[0x44 / 4] = 0;
    words[4] = 0xFFU << 16;
    words[255] = 10;
    REQUIRE(findCapability(space, capability) == Status::InvalidArgument);

    words[4] = 0;
    REQUIRE(findCapability(space, capability) == Status::Unavailable);
}

static void waitingForHost()
{
    Session session;
    session.device.connect = false;
    REQUIRE(session.controller.open(Wait::forDuration(session.device, 0)) == Status::Timeout);

    Snapshot snapshot{};
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(snapshot.state == State::Waiting);
    REQUIRE(snapshot.configurationVerified && snapshot.registersValid);
    REQUIRE(snapshot.control & 0x80000000);
    REQUIRE(!(snapshot.control & 1));
    REQUIRE(snapshot.segmentTableBase == DeviceModel::kDMA + 0x30C0);
    REQUIRE(snapshot.eventDequeue == DeviceModel::kDMA);
    REQUIRE(snapshot.contextPointer == DeviceModel::kDMA + 0x3000);
    REQUIRE(snapshot.segmentTableSize == 1);
    REQUIRE(snapshot.descriptorInfo1 == 0xFFFF0000);
    REQUIRE(snapshot.descriptorInfo2 == 0x01000001);

    // ADL's connection notification carries Port ID 14 on the dedicated DbC ring.
    session.device.registers[10] = 0x00021003;
    session.device.appendEvent({0x0E000000, 0x01000000, 0x00008800});
    REQUIRE(session.controller.poll() == Status::Disconnected);
    REQUIRE(session.device.registers[10] == 0x00001003);
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(snapshot.state == State::Waiting && snapshot.invalidEvents == 0);
    REQUIRE(snapshot.eventDequeue == DeviceModel::kDMA + sizeof(TRB));

    size_t writes = session.device.writes.size();

    for (unsigned i = 0; i < 1000; ++i) {
        REQUIRE(session.controller.poll() == Status::Disconnected);
    }

    REQUIRE(session.device.writes.size() == writes);
    REQUIRE(session.device.doorbells == 0);
    REQUIRE(session.device.pauses == 0);
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(snapshot.state == State::Waiting);
    REQUIRE(snapshot.timeouts == 1 && snapshot.invalidEvents == 0);
    REQUIRE(snapshot.bytesRead == 0 && snapshot.bytesWritten == 0);

    session.device.registers[8] |= 1;
    session.device.registers[10] = 3;
    REQUIRE(session.controller.poll() == Status::Success);
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(snapshot.state == State::Running);
    REQUIRE(snapshot.configurationVerified);
}

static void registerSnapshots()
{
    Session session;
    Snapshot snapshot{};
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(!snapshot.registersValid && !snapshot.configurationVerified);
    REQUIRE(session.device.registerReads == 0);
    session.open();

    session.device.registers[9] = 0x02000001;
    session.device.registers[10] = 0x002002A3;
    size_t writes = session.device.writes.size();
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(snapshot.registersValid && snapshot.configurationVerified);
    REQUIRE(snapshot.capabilityID == 10);
    REQUIRE(snapshot.doorbell == 0x00800000);
    REQUIRE(snapshot.debugStatus == 0x02000001);
    REQUIRE(snapshot.portStatus == 0x002002A3);
    REQUIRE(session.device.writes.size() == writes);
    REQUIRE(session.device.registers[10] == 0x002002A3);

    session.device.ready = false;
    size_t reads = session.device.registerReads;
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(!snapshot.registersValid);
    REQUIRE(session.device.registerReads == reads);

    session.device.ready = true;
    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
    reads = session.device.registerReads;
    REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
    REQUIRE(snapshot.state == State::Prepared);
    REQUIRE(!snapshot.registersValid && !snapshot.configurationVerified);
    REQUIRE(session.device.registerReads == reads);
    REQUIRE(std::strcmp(stateName(snapshot.state), "prepared") == 0);
}

static void rejectedConfiguration()
{
    const std::array<uint32_t, 9> offsets{
        0x08,
        0x10,
        0x14,
        0x18,
        0x1C,
        0x30,
        0x34,
        0x38,
        0x3C,
    };

    for (uint32_t offset : offsets) {
        Session session;
        session.device.ignoredWriteOffset = offset;
        REQUIRE(session.controller.open(session.device.wait()) == Status::ConfigurationError);
        REQUIRE(!(session.device.registers[8] & 0x80000000));
        REQUIRE(session.device.doorbells == 0);

        for (const auto &write : session.device.writes) {
            REQUIRE(write.first != 0x20);
        }

        Snapshot snapshot{};
        REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
        REQUIRE(snapshot.state == State::Faulted);
        REQUIRE(snapshot.registersValid && !snapshot.configurationVerified);
        REQUIRE(snapshot.lastStatus == Status::ConfigurationError);
        REQUIRE(session.controller.release() == Status::Busy);

        session.device.ignoredWriteOffset = UINT32_MAX;
        REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
        REQUIRE(session.device.registers[2] == 0);
        REQUIRE(session.device.registers[4] == 0 && session.device.registers[5] == 0);
        REQUIRE(session.device.registers[6] == 0 && session.device.registers[7] == 0);
        REQUIRE(session.device.registers[12] == 0 && session.device.registers[13] == 0);
        session.open();
    }

    Session session;
    session.device.registers[2] = 0xABCD0000;
    session.device.registers[4] = 15;
    session.device.registers[6] = 8;
    session.device.registers[12] = 15;
    session.open();
    REQUIRE(session.device.registers[2] == 0xABCD0001);
    REQUIRE(session.device.registers[4] == 0x234030CF);
    REQUIRE(session.device.registers[6] == 0x23400008);
    REQUIRE(session.device.registers[12] == 0x2340300F);
}

static void powerAvailability()
{
    Session session;
    session.device.connect = false;

    for (unsigned i = 0; i < 64; ++i) {
        REQUIRE(session.controller.open(Wait::forDuration(session.device, 0)) == Status::Timeout);
        REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
        session.device.ready = false;
        size_t reads = session.device.registerReads;
        size_t writes = session.device.writes.size();

        REQUIRE(session.controller.open(session.device.wait()) == Status::Unavailable);
        Snapshot snapshot{};
        REQUIRE(session.controller.copySnapshot(snapshot) == Status::Success);
        REQUIRE(!snapshot.registersValid);
        REQUIRE(snapshot.state == State::Prepared);
        REQUIRE(session.device.registerReads == reads);
        REQUIRE(session.device.writes.size() == writes);
        session.device.ready = true;
    }

    REQUIRE(session.controller.open(session.device.wait()) == Status::Timeout);
    session.device.ready = false;
    REQUIRE(session.controller.poll() == Status::Unavailable);
    REQUIRE(session.controller.close(session.device.wait()) == Status::Unavailable);
    REQUIRE(session.controller.release() == Status::Busy);
    session.device.ready = true;
    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
    REQUIRE(session.controller.open(session.device.wait()) == Status::Timeout);
}

static std::string originalOutput;

static void originalWrite(char *bytes, int length)
{
    if (length > 0) {
        originalOutput.append(bytes, static_cast<size_t>(length));
    }
}

static void originalWriteUnbuffered(char character)
{
    originalOutput += character;
}

static void discardOriginal(char *, int)
{
}

static void discardOriginal(char)
{
}

static void consoleLifecycle()
{
    REQUIRE(std::strcmp(Console::routingStatus(), "pending") == 0);
    Session session;
    session.open();
    KernelPatcher patcher(originalWrite, originalWriteUnbuffered);
    patcher.routeLimit = 1;
    REQUIRE(!Console::route(patcher));
    REQUIRE(!Console::isReady() && patcher.errorCleared);
    REQUIRE(std::strcmp(Console::routingStatus(), "failed") == 0);

    originalOutput.clear();
    char message[] = {'A', '\0', 'B', '\n'};
    patcher.write(message, sizeof(message));
    REQUIRE(originalOutput.size() == sizeof(message));
    REQUIRE(session.device.outgoing.empty());

    patcher.routeLimit = 2;
    REQUIRE(Console::route(patcher) && Console::isReady());
    REQUIRE(std::strcmp(Console::routingStatus(), "ready") == 0);
    REQUIRE(Console::initialize({nullptr, &session.device, 10}) == Status::InvalidState);
    REQUIRE(Console::initialize({&session.controller, &session.device, 10}) == Status::Success);
    REQUIRE(
        Console::initialize({&session.controller, &session.device, 10}) == Status::InvalidState);

    auto before = Console::copyCounters();
    originalOutput.clear();
    Console::attach();
    patcher.write(message, sizeof(message));
    patcher.writeUnbuffered('Z');
    REQUIRE(originalOutput == std::string(message, sizeof(message)) + 'Z');
    REQUIRE(session.device.outgoing == std::string(message, sizeof(message)) + originalOutput);
    auto after = Console::copyCounters();
    REQUIRE(after.confirmedBytes - before.confirmedBytes == 9);
    REQUIRE(after.unconfirmedBytes == before.unconfirmedBytes);
    REQUIRE(after.discardedBytes == before.discardedBytes);

    Console::detach();
    patcher.writeUnbuffered('X');
    REQUIRE(session.device.outgoing.size() == 9);
    REQUIRE(originalOutput.size() == 6);
    REQUIRE(Console::copyCounters().queuedBytes == 1);
    Console::attach();
    Console::flush();
    REQUIRE(session.device.outgoing.back() == 'X');
    REQUIRE(Console::copyCounters().queuedBytes == 0);
    REQUIRE(Console::release() == Status::Success);
    REQUIRE(Console::release() == Status::Success);
}

static void consoleBudget()
{
    Session session;
    session.open();
    KernelPatcher patcher(originalWrite, originalWriteUnbuffered);
    REQUIRE(Console::route(patcher));
    REQUIRE(Console::initialize({&session.controller, &session.device, 5}) == Status::Success);
    session.device.completes = false;

    auto before = Console::copyCounters();
    std::string message(Layout::kBufferLength + 8, 'T');
    originalOutput.clear();
    Console::attach();
    patcher.write(message.data(), static_cast<int>(message.size()));
    auto after = Console::copyCounters();
    REQUIRE(after.confirmedBytes == before.confirmedBytes);
    REQUIRE(after.unconfirmedBytes - before.unconfirmedBytes == Layout::kBufferLength);
    REQUIRE(after.discardedBytes == before.discardedBytes);
    REQUIRE(after.queuedBytes == message.size());
    REQUIRE(session.device.pauses == 5 && session.device.doorbells == 1);
    REQUIRE(originalOutput == message);
    session.device.completeTransfer(0);
    REQUIRE(session.controller.poll() == Status::Success);
    session.device.completes = true;
    Console::flush();
    REQUIRE(Console::copyCounters().queuedBytes == 0);
    REQUIRE(session.device.outgoing == message);
    REQUIRE(Console::release() == Status::Success);

    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
    session.open();
    REQUIRE(Console::initialize({&session.controller, &session.device, 1}) == Status::Success);
    before = Console::copyCounters();
    message.assign(Layout::kBufferLength * 3 + 1, 'B');
    originalOutput.clear();
    Console::attach();
    patcher.write(message.data(), static_cast<int>(message.size()));
    after = Console::copyCounters();
    REQUIRE(after.confirmedBytes - before.confirmedBytes == Layout::kBufferLength * 2);
    REQUIRE(after.discardedBytes == before.discardedBytes);
    REQUIRE(after.queuedBytes == Layout::kBufferLength + 1);
    REQUIRE(originalOutput == message);
    Console::flush();
    REQUIRE(Console::copyCounters().queuedBytes == 0);
    REQUIRE(Console::release() == Status::Success);
}

static void consoleReentry()
{
    Session session;
    session.open();
    session.device.completes = false;
    KernelPatcher patcher(originalWrite, originalWriteUnbuffered);
    REQUIRE(Console::route(patcher));
    REQUIRE(Console::initialize({&session.controller, &session.device, 10}) == Status::Success);
    auto before = Console::copyCounters();
    bool attempted = false;
    char nested[] = "nested";

    session.device.onPause = [&] {
        if (!attempted) {
            attempted = true;
            patcher.write(nested, 6);
            REQUIRE(Console::release() == Status::Busy);
            session.device.completeTransfer(0);
        }
    };

    originalOutput.clear();
    Console::attach();
    char outer[] = "outer";
    patcher.write(outer, 5);
    session.device.completes = true;
    session.device.completeTransfer(0);
    Console::attach();
    Console::flush();
    auto after = Console::copyCounters();
    REQUIRE(attempted);
    REQUIRE(after.confirmedBytes - before.confirmedBytes == 11);
    REQUIRE(after.discardedBytes == before.discardedBytes);
    REQUIRE(originalOutput == "nestedouter");
    REQUIRE(session.device.outgoing == "outernested");
    REQUIRE(Console::release() == Status::Success);
}

static void consoleConcurrentOutput()
{
    Session session;
    session.open();
    session.device.completes = false;
    KernelPatcher patcher(originalWrite, originalWriteUnbuffered);
    REQUIRE(Console::route(patcher));
    REQUIRE(Console::initialize({&session.controller, &session.device, 10}) == Status::Success);
    std::atomic<bool> waiting{};
    std::atomic<bool> completed{};
    bool observedWriter = false;
    auto waitFor = [](const std::atomic<bool> &flag) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        while (!flag.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }

            std::this_thread::yield();
        }

        return true;
    };

    session.device.onPause = [&] {
        waiting.store(true, std::memory_order_release);

        if (waitFor(completed)) {
            session.device.completeTransfer(0);
        }
    };

    auto before = Console::copyCounters();
    originalOutput.clear();
    Console::attach();
    std::thread other([&] {
        observedWriter = waitFor(waiting);

        if (observedWriter) {
            patcher.writeUnbuffered('C');
        }

        completed.store(true, std::memory_order_release);
    });
    char message[] = "main";
    patcher.write(message, 4);
    other.join();

    auto after = Console::copyCounters();
    REQUIRE(observedWriter);
    REQUIRE(after.confirmedBytes - before.confirmedBytes == 5);
    REQUIRE(after.discardedBytes == before.discardedBytes);
    REQUIRE(originalOutput == "Cmain");
    REQUIRE(session.device.outgoing == "mainC");
    REQUIRE(Console::release() == Status::Success);
}

static void consoleBufferedOutput()
{
    Session session;
    KernelPatcher patcher(discardOriginal, discardOriginal);
    REQUIRE(Console::route(patcher));
    constexpr unsigned writers = 8;
    constexpr unsigned records = 1500;
    for (bool connected : {false, true}) {
        session.device.outgoing.clear();
        std::vector<std::thread> producers;
        for (unsigned cpu = 0; cpu < writers; ++cpu) {
            producers.emplace_back([&, cpu] {
                for (unsigned line = 0; line < records; ++line) {
                    auto text = std::to_string(cpu) + ":" + std::to_string(line) + "\n";
                    patcher.write(text.data(), static_cast<int>(text.size()));
                }
            });
        }
        for (auto &producer : producers) {
            producer.join();
        }
        if (!connected) {
            REQUIRE(session.device.outgoing.empty());
            REQUIRE(Console::copyCounters().queuedBytes > 0);
            REQUIRE(Console::initialize({&session.controller, &session.device, 1000}) ==
                Status::Success);
            session.open();
            Console::attach();
        }
        while (Console::copyCounters().queuedBytes) {
            Console::flush();
        }
        std::array<unsigned, writers> next{};
        size_t offset = 0;
        while (offset < session.device.outgoing.size()) {
            const size_t end = session.device.outgoing.find('\n', offset);
            REQUIRE(end != std::string::npos);
            const std::string line = session.device.outgoing.substr(offset, end - offset);
            const auto colon = line.find(':');
            REQUIRE(colon != std::string::npos);
            const auto cpu = std::stoul(line.substr(0, colon));
            const auto number = std::stoul(line.substr(colon + 1));
            REQUIRE(cpu < writers && number == next[cpu]++);
            offset = end + 1;
        }
        for (auto count : next) {
            REQUIRE(count == records);
        }
    }

    // Change record sizes across several physical ring wraps.
    for (unsigned pass = 0; pass < 8; ++pass) {
        Console::detach();
        const size_t start = session.device.outgoing.size();
        std::string expected;
        for (unsigned record = 0; record < 170; ++record) {
            std::string text(4089 + (record % 13), static_cast<char>(record + pass));
            expected += text;
            patcher.write(text.data(), static_cast<int>(text.size()));
        }
        Console::attach();
        while (Console::copyCounters().queuedBytes) {
            Console::flush();
        }
        REQUIRE(session.device.outgoing.substr(start) == expected);
    }
    REQUIRE(Console::copyCounters().discardedBytes == 0);
    REQUIRE(Console::release() == Status::Success);
}

static void consoleOverflow()
{
    Session session;
    session.open();
    KernelPatcher patcher(discardOriginal, discardOriginal);
    REQUIRE(Console::route(patcher));
    REQUIRE(Console::initialize({&session.controller, &session.device, 1000}) == Status::Success);
    Console::detach();
    std::string text(4 * 1024 * 1024, 'Q');
    patcher.write(text.data(), static_cast<int>(text.size()));
    const auto queued = Console::copyCounters();
    REQUIRE(queued.queuedBytes && queued.discardedBytes);
    REQUIRE(queued.queuedBytes + queued.discardedBytes == text.size());
    Console::attach();
    while (Console::copyCounters().queuedBytes) {
        Console::flush();
    }
    REQUIRE(session.device.outgoing.substr(0, queued.queuedBytes) ==
        text.substr(0, queued.queuedBytes));
    REQUIRE(session.device.outgoing.find("queue.overflow total_lost=" +
                std::to_string(queued.discardedBytes)) != std::string::npos);
    REQUIRE(session.device.outgoing.find("input_window=") != std::string::npos);
    REQUIRE(Console::release() == Status::Success);
}

static void consoleRetry()
{
    for (bool reset : {false, true}) {
        Session session;
        session.open();
        KernelPatcher patcher(discardOriginal, discardOriginal);
        REQUIRE(Console::route(patcher));
        REQUIRE(Console::initialize({&session.controller, &session.device, 10}) == Status::Success);
        session.device.completes = false;
        Console::attach();
        char message[] = "retained-output";
        patcher.write(message, sizeof(message) - 1);
        REQUIRE(Console::copyCounters().queuedBytes == sizeof(message) - 1);
        if (reset) {
            Console::detach();
            session.device.registers[8] &= ~0x80000000U;
            REQUIRE(session.controller.poll() == Status::ControllerReset);
            REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
            session.open();
            Console::attach();
        } else {
            session.device.eventResidual = 3;
            session.device.completeTransfer(0);
            REQUIRE(session.controller.poll() == Status::Success);
            session.device.eventResidual = 0;
        }
        session.device.completes = true;
        Console::flush();
        REQUIRE(Console::copyCounters().queuedBytes == 0);
        REQUIRE(session.device.outgoing == message);
        REQUIRE(Console::release() == Status::Success);
    }
}

static void panicCapture()
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        pid_t child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            try {
                Session session;
                session.open();
                KernelPatcher patcher(originalWrite, originalWriteUnbuffered);
                REQUIRE(Console::route(patcher));
                REQUIRE(PanicConsole::route(patcher) && PanicConsole::isReady());
                REQUIRE(Console::initialize({&session.controller, &session.device, 10}) ==
                    Status::Success);
                KernelPatcher::panic = false;
                KernelPatcher::exceptionRoute(6, 7, 8, &session);
                REQUIRE(KernelPatcher::exceptionCalls == 1);
                REQUIRE(KernelPatcher::exceptionArguments[0] == 6 &&
                    KernelPatcher::exceptionArguments[1] == 7 &&
                    KernelPatcher::exceptionArguments[2] == 8 &&
                    KernelPatcher::exceptionState == &session);
                KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
                REQUIRE(!PanicConsole::isActive());
                KernelPatcher::panic = true;
                KernelPatcher::halted = false;
                KernelPatcher::exceptionRoute(9, 10, 11, nullptr);
                REQUIRE(KernelPatcher::exceptionCalls == 2 && !KernelPatcher::exceptionState);
                KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
                REQUIRE(!PanicConsole::isActive());
                KernelPatcher::halted = true;

                if (mode == 1) {
                    session.device.completes = false;
                    session.device.frozenClock = true;
                } else if (mode == 2) {
                    REQUIRE(session.controller.close(session.device.wait()) == Status::Success);
                    session.device.connect = false;
                    session.device.frozenClock = true;
                }
                KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
                REQUIRE(PanicConsole::isActive());
                REQUIRE(Console::release() == Status::Busy);
                char message[] = "panic-text\n";
                patcher.write(message, sizeof(message) - 1);
                KernelPatcher::panicLogRoute('L');
                patcher.writeUnbuffered('\n');
                if (mode == 0) {
                    REQUIRE(session.device.outgoing.find("panic.begin") != std::string::npos);
                    REQUIRE(session.device.outgoing.find("panic-text\nL\n") != std::string::npos);
                    session.device.completes = false;
                    session.device.onPause = [&] {
                        session.device.completeTransfer(0);
                        if (session.device.outgoing.find("panic.end captured=") !=
                            std::string::npos) {
                            _exit(0);
                        }
                    };
                    KernelPatcher::panicSpinRoute();
                } else {
                    const size_t pauses = session.device.pauses;
                    for (unsigned i = 0; i < 100; ++i) {
                        KernelPatcher::panicLogRoute('x');
                    }
                    REQUIRE(session.device.pauses == pauses);
                    if (mode == 1) {
                        std::string excess(2 * 1024 * 1024 + 512, 'X');
                        patcher.write(excess.data(), static_cast<int>(excess.size()));
                        session.device.completeTransfer(0);
                        session.device.completes = true;
                        KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
                        REQUIRE(session.device.outgoing.size() == 2 * 1024 * 1024);
                        const auto text = session.device.outgoing.find("panic-text\n");
                        const auto tail = session.device.outgoing.find("L\n", text);
                        REQUIRE(
                            text != std::string::npos && tail != std::string::npos && text < tail);
                    }
                }
                _exit(0);
            } catch (const std::exception &error) {
                std::cerr << "panic mode " << mode << ": " << error.what() << '\n';
                _exit(1);
            }
        }
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

static void panicStoppedWriter()
{
    Session session;
    session.open();
    KernelPatcher patcher(originalWrite, originalWriteUnbuffered);
    REQUIRE(Console::route(patcher));
    REQUIRE(PanicConsole::route(patcher));
    REQUIRE(Console::initialize({&session.controller, &session.device, 10}) == Status::Success);
    Console::attach();
    session.device.completes = false;
    std::atomic<bool> waiting{};
    std::atomic<bool> resume{};
    session.device.onPause = [&] {
        waiting.store(true, std::memory_order_release);
        while (!resume.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        session.device.completeTransfer(0);
    };
    std::thread writer([&] {
        char message[] = "interrupted";
        patcher.write(message, sizeof(message) - 1);
    });
    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // fork() retains the locked state while the interrupted writer stays in the parent.
    bool passed = true;
    for (bool refusesDisable : {false, true}) {
        pid_t child = fork();
        if (child == 0) {
            session.device.onPause = {};
            session.device.completes = true;
            session.device.refusesDisable = refusesDisable;
            KernelPatcher::panic = true;
            KernelPatcher::halted = true;
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
            if (refusesDisable) {
                Snapshot snapshot{};
                const bool held = session.controller.copySnapshot(snapshot) == Status::Busy;
                const bool retained =
                    std::memcmp(static_cast<char *>(session.device.memory) + 0x4000,
                        "interrupted",
                        11) == 0;
                _exit(held && retained ? 0 : 1);
            }
            char message[] = "panic-after-stopped-writer\n";
            Snapshot snapshot{};
            if (session.controller.copySnapshot(snapshot) != Status::Success ||
                snapshot.panicTakeovers != 1) {
                _exit(1);
            }
            patcher.write(message, sizeof(message) - 1);
            const bool sent = session.device.outgoing.find(message) != std::string::npos;
            _exit(sent ? 0 : 1);
        }
        int status = 0;
        const bool exited = child >= 0 && waitpid(child, &status, 0) == child;
        passed = exited && WIFEXITED(status) && WEXITSTATUS(status) == 0 && passed;
    }
    resume.store(true, std::memory_order_release);
    writer.join();
    REQUIRE(passed);
    REQUIRE(Console::release() == Status::Success);
}

static void retainedHistory()
{
    for (bool large : {false, true}) {
        const auto child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            try {
                Session session;
                session.open();
                std::string storage =
                    large ? std::string(XNU::MAX_MSG_BSIZE, 'H') : std::string("def\0abc\n", 8);
                const std::string expected = large ? storage : "abc\ndef";
                XNU::MsgBuffer message{
                    XNU::MSG_MAGIC, static_cast<int32_t>(storage.size()), 3, 5, storage.data()};
                KernelPatcher::messageBuffer = &message;
                KernelPatcher::messageBusy = true;
                KernelPatcher patcher(discardOriginal, discardOriginal);
                REQUIRE(Console::route(patcher));
                REQUIRE(Console::initialize({&session.controller, &session.device, 1000}) ==
                    Status::Success);
                Console::attach();
                Console::flush();
                REQUIRE(Console::copyCounters().historyBytes == 0);
                KernelPatcher::messageBusy = false;
                Console::flush();
                REQUIRE(Console::copyCounters().historyBytes == expected.size());
                REQUIRE(!KernelPatcher::messageHeld && message.msg_bufr == 5);
                REQUIRE(session.device.outgoing.find(expected) != std::string::npos);
                const auto bytes = session.device.outgoing.size();
                Console::flush();
                REQUIRE(session.device.outgoing.size() == bytes);
                REQUIRE(Console::release() == Status::Success);
                _exit(0);
            } catch (...) {
                _exit(1);
            }
        }
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

static void retainedPanic()
{
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        try {
            Session session;
            session.open();
            char storage[256] = "first-only\n";
            KernelPatcher::providePanicBuffer = true;
            KernelPatcher::debugBase = storage;
            KernelPatcher::debugEnd = storage + strlen(storage);
            KernelPatcher::debugSize = sizeof(storage);
            KernelPatcher patcher(discardOriginal, discardOriginal);
            REQUIRE(Console::route(patcher));
            REQUIRE(PanicConsole::route(patcher) && PanicConsole::hasBuffer());
            REQUIRE(Console::initialize({&session.controller, &session.device, 1000}) ==
                Status::Success);
            KernelPatcher::panic = true;
            KernelPatcher::halted = true;
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
            constexpr char direct[] = "direct-only\n";
            memcpy(KernelPatcher::debugEnd, direct, sizeof(direct) - 1);
            KernelPatcher::debugEnd += sizeof(direct) - 1;
            const auto putc = reinterpret_cast<void (*)(char)>(
                patcher.solveSymbol(KernelPatcher::KernelID, "_debug_putc"));
            PanicConsole::write(
                "C", 1, reinterpret_cast<uintptr_t>(KernelPatcher::debugEntries + 8));
            putc('C');
            KernelPatcher::panicLogRoute('L');
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
            REQUIRE(session.device.outgoing.find("direct-only\nCL") != std::string::npos);
            const auto countText = [&](const char *text) {
                size_t count = 0;
                size_t at = 0;
                while ((at = session.device.outgoing.find(text, at)) != std::string::npos) {
                    ++count;
                    at += strlen(text);
                }
                return count;
            };
            REQUIRE(countText("first-only") == 1 && countText("direct-only") == 1);
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
            REQUIRE(countText("first-only") == 1 && countText("direct-only") == 1);
            char nested[256] = "nested-only\n";
            KernelPatcher::debugBase = nested;
            KernelPatcher::debugEnd = nested + strlen(nested);
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
            REQUIRE(countText("panic.begin") == 1 && countText("panic.nested") == 1);
            REQUIRE(countText("first-only") == 1 && countText("nested-only") == 1);
            REQUIRE(countText("panic.buffer-reset") == 1);
            char full[] = {'A', 'B', 'C', 'D'};
            KernelPatcher::debugBase = full;
            KernelPatcher::debugEnd = full + sizeof(full);
            KernelPatcher::debugSize = sizeof(full);
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
            KernelPatcher::panicLogRoute('F');
            patcher.writeUnbuffered('\n');
            REQUIRE(session.device.outgoing.find("F\n") != std::string::npos);
            REQUIRE(countText("ABCD") == 1);
            KernelPatcher::debugBase = nested;
            KernelPatcher::debugEnd = nested + sizeof(nested);
            KernelPatcher::debugSize = sizeof(nested) - 1;
            KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
            REQUIRE(countText("first-only") == 1 && countText("nested-only") == 1);
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void panicBatching()
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        const auto child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            try {
                Session session;
                session.open();
                KernelPatcher patcher(discardOriginal, discardOriginal);
                REQUIRE(Console::route(patcher) && PanicConsole::route(patcher));
                REQUIRE(Console::initialize({&session.controller, &session.device, 1000}) ==
                    Status::Success);
                KernelPatcher::panic = KernelPatcher::halted = true;
                if (mode == 2) {
                    session.device.completes = false;
                    session.device.onPause = [&] {
                        session.device.clock += 11000000;
                    };
                }
                KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
                if (mode == 0) {
                    REQUIRE(
                        session.device.outgoing.find("panic.entry.return") != std::string::npos);
                    const auto bells = session.device.doorbells;
                    const auto before = session.device.outgoing.size();
                    for (unsigned i = 0; i < 500; ++i) {
                        patcher.writeUnbuffered('b');
                    }
                    REQUIRE(session.device.outgoing.size() == before);
                    patcher.writeUnbuffered('\n');
                    REQUIRE(session.device.outgoing.substr(before) == std::string(500, 'b') + '\n');
                    REQUIRE(session.device.doorbells == bells + 1);
                    session.device.clock += 20000000;
                    char late[] = "after-kernel-work\n";
                    patcher.write(late, sizeof(late) - 1);
                    REQUIRE(session.device.outgoing.find(late) != std::string::npos);
                    KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_ENTER);
                    REQUIRE(
                        session.device.outgoing.find("panic.nested entry=2") != std::string::npos);
                } else if (mode == 1) {
                    session.device.completes = false;
                    char shortText[] = "short-result\n";
                    patcher.write(shortText, sizeof(shortText) - 1);
                    session.device.eventResidual = 5;
                    session.device.completeTransfer(0);
                    session.device.eventResidual = 0;
                    session.device.completes = true;
                    KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
                    const auto text = session.device.outgoing.find(shortText);
                    REQUIRE(text != std::string::npos);
                    REQUIRE(session.device.outgoing.find(shortText, text + 1) == std::string::npos);
                    KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
                    REQUIRE(
                        session.device.outgoing.find("pending=0 remaining=0") != std::string::npos);
                } else if (mode == 2) {
                    session.device.onPause = {};
                    session.device.completeTransfer(0);
                    session.device.completes = true;
                    KernelPatcher::debuggerCallback(nullptr, KDP_EVENT_PANICLOG);
                    REQUIRE(session.device.outgoing.find("panic.stop reason=wait-budget") !=
                        std::string::npos);
                    REQUIRE(
                        session.device.outgoing.find("waited_us=10000000") != std::string::npos);
                    const auto bells = session.device.doorbells;
                    for (unsigned i = 0; i < 100; ++i) {
                        patcher.writeUnbuffered('\n');
                    }
                    REQUIRE(session.device.doorbells == bells);
                } else {
                    session.device.completes = false;
                    char delayed[] = "delayed-completion\n";
                    patcher.write(delayed, sizeof(delayed) - 1);
                    const auto bells = session.device.doorbells;
                    const auto resumeAt = session.device.clock + 100000;
                    bool resumed = false;
                    session.device.onPause = [&] {
                        if (!resumed) {
                            REQUIRE(session.device.doorbells == bells);
                            REQUIRE(!memcmp(
                                static_cast<char *>(session.device.memory) + Layout::kOutBuffer,
                                delayed,
                                sizeof(delayed) - 1));
                            if (session.device.clock >= resumeAt) {
                                resumed = true;
                            }
                        }
                        if (resumed) {
                            session.device.completeTransfer(0);
                            session.device.completes = mode == 3;
                        }
                        if (mode == 4 &&
                            session.device.outgoing.find("panic.end captured=") !=
                                std::string::npos) {
                            REQUIRE(session.device.outgoing.find(delayed) != std::string::npos);
                            REQUIRE(session.device.outgoing.find("panic.debugger-skip") !=
                                std::string::npos);
                            REQUIRE(KernelPatcher::exceptionCalls == 0);
                            _exit(0);
                        }
                    };
                    if (mode == 4) {
                        KernelPatcher::exceptionRoute(6, 0, 0, nullptr);
                        _exit(1);
                    }
                    for (unsigned retry = 0; retry < 8; ++retry) {
                        patcher.writeUnbuffered('\n');
                    }
                    REQUIRE(session.device.completes);
                    const auto text = session.device.outgoing.find(delayed);
                    REQUIRE(text != std::string::npos);
                    REQUIRE(session.device.outgoing.find(delayed, text + 1) == std::string::npos);
                    REQUIRE(session.device.outgoing.find("panic.stop") == std::string::npos);
                }
                _exit(0);
            } catch (const std::exception &error) {
                std::cerr << "panic batching mode " << mode << ": " << error.what() << '\n';
                _exit(1);
            }
        }
        int status = 0;
        REQUIRE(waitpid(child, &status, 0) == child);
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

int main()
{
    try {
        lifecycle();
        transfers();
        pendingOwnership();
        boundsAndFaults();
        validationAndReentry();
        portChanges();
        hostReconfiguration();
        capabilityDiscovery();
        waitingForHost();
        registerSnapshots();
        rejectedConfiguration();
        powerAvailability();
        consoleLifecycle();
        consoleBudget();
        consoleReentry();
        consoleConcurrentOutput();
        consoleBufferedOutput();
        consoleRetry();
        // Overflow counters remain cumulative for the driver lifetime.
        consoleOverflow();
        retainedHistory();
        retainedPanic();
        panicBatching();
        panicCapture();
        panicStoppedWriter();
        std::cout << "Controller and console tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
