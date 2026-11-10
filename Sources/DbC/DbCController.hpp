// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "DbCRegisters.hpp"

namespace DbC {

enum class Status : uint8_t {
    Success,
    InvalidArgument,
    InvalidState,
    Busy,
    Timeout,
    Disconnected,
    ControllerReset,
    Unavailable,
    TransferError,
    InvalidEvent,
    ConfigurationError
};

const char *statusName(Status status);

enum class State : uint8_t {
    Released,
    Prepared,
    Waiting,
    Running,
    Faulted
};

const char *stateName(State state);

enum class DMADirection : uint8_t {
    ToDevice,
    FromDevice
};

// Implementations provide coherent, permanently mapped DMA memory and ordered MMIO.
// isReady() includes power state, mapping lifetime and the xHC USBSTS.CNR check.
class Platform {
public:
    virtual bool isReady() = 0;
    virtual uint32_t read32(uint32_t offset) = 0;
    virtual void write32(uint32_t offset, uint32_t value) = 0;
    virtual void synchronizeDMA(DMADirection direction) = 0;
    virtual uint64_t timeMicroseconds() = 0;
    virtual void pause() = 0;

protected:
    ~Platform() = default;
};

struct Wait {
    uint64_t deadline;
    uint32_t pollLimit;

    static Wait forDuration(Platform &platform, uint32_t microseconds);
};

struct Memory {
    void *bytes;
    uint64_t deviceAddress;
    size_t length;
};

struct Identity {
    uint16_t vendorID;
    uint16_t productID;
    uint16_t revision;
    const char *manufacturer;
    const char *product;
    const char *serialNumber;
};

struct Configuration {
    Platform *platform;
    Memory memory;
    Identity identity;
    uint8_t addressBits;
};

struct ReadRequest {
    void *bytes;
    size_t length;
    Wait wait;
};

struct WriteRequest {
    const void *bytes;
    size_t length;
    Wait wait;
};

struct TransferResult {
    Status status;
    size_t transferred;
    bool pending;
};

struct Snapshot {
    State state;
    Status lastStatus;
    uint32_t control;
    uint32_t portStatus;
    uint64_t bytesRead;
    uint64_t bytesWritten;
    uint64_t timeouts;
    uint64_t invalidEvents;
    uint64_t lateWrites;
    uint64_t panicTakeovers;
    // Preserve the first rejected event through close/open for hardware diagnosis.
    TRB firstInvalidEvent;
    uint64_t firstInvalidEventAddress;
    // copySnapshot() samples MMIO while Controller owns registers and Platform is ready.
    bool registersValid;
    // Set by the pre-DCE readback check; cleared by successful close().
    bool configurationVerified;
    uint32_t capabilityID;
    uint32_t doorbell;
    uint32_t segmentTableSize;
    uint64_t segmentTableBase;
    uint64_t eventDequeue;
    uint32_t debugStatus;
    uint64_t contextPointer;
    uint32_t descriptorInfo1;
    uint32_t descriptorInfo2;
};

// A contiguous device-address range keeps each TRB within a 64 KiB boundary.
namespace Layout {
constexpr size_t kAlignment = 0x10000;
constexpr size_t kLength = 0xC000;
constexpr size_t kEventRing = 0x0000;
constexpr size_t kOutRing = 0x1000;
constexpr size_t kInRing = 0x2000;
constexpr size_t kContext = 0x3000;
constexpr size_t kSegmentTable = 0x30C0;
constexpr size_t kStrings = 0x3100;
constexpr size_t kOutBuffer = 0x4000;
constexpr size_t kInBuffer = 0x8000;
constexpr size_t kBufferLength = 0x4000;
constexpr uint16_t kTRBCount = 256;
}

// Retain Platform, Memory and identity strings through successful close()/release().
// Recursive and concurrent calls return Busy through a single try-lock attempt.
class Controller final {
public:
    Controller() = default;
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    Status initialize(const Configuration &configuration);
    Status release();

    Status open(Wait wait);
    Status close(Wait wait);

    TransferResult read(const ReadRequest &request);
    TransferResult write(const WriteRequest &request);
    TransferResult flush(Wait wait);
#if defined(KERNEL) && DEBUG
    Status testPanic();
#endif

    // The fatal debugger callback calls this after the other CPUs are quiesced.
    Status enterPanic(Wait wait);

    Status poll();
    Status copySnapshot(Snapshot &snapshot);

private:
    struct Transfer {
        uint64_t trbAddress{};
        uint32_t requested{};
        uint32_t completed{};
        uint32_t consumed{};
        uint16_t enqueue{};
        bool cycle{true};
        bool pending{};
        bool timedOut{};
        Status status{Status::Success};
    };

    class Guard {
    public:
        explicit Guard(Controller &controller);
        ~Guard();

        explicit operator bool() const
        {
            return acquired;
        }

    private:
        Controller &owner;
        bool acquired;
    };

    struct Budget {
        Platform &platform;
        Wait wait;

        bool advance();
    };

    uint8_t locked{};
    Configuration configuration{};
    Snapshot snapshot{};
    Transfer out{};
    Transfer in{};
    uint16_t eventIndex{};
    bool eventCycle{true};
    bool ownsRegisters{};
    uint32_t preservedControl{};
    uint32_t preservedDoorbell{};

    uint64_t deviceAddress(size_t offset) const;
    uint8_t *bytes(size_t offset) const;
    uint64_t read64(uint32_t offset);
    void write64(uint32_t offset, uint64_t value);
    void prepareMemory();
    Status pollLocked();
    Status fault(Status status);
    // The transfer-specific name keeps this header compatible with XNU queue macros.
    void enqueueTransfer(Transfer &transfer, bool output, uint32_t length);
    Status complete(const TRB &event);
    TransferResult waitForTransfer(Transfer &transfer, Wait wait);
    void captureRegisters(Snapshot &value);
    bool verifyConfiguration(const Snapshot &value) const;
};
}
