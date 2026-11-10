// SPDX-License-Identifier: BSD-2-Clause
#include "DbCController.hpp"
#if defined(KERNEL) && DEBUG
#include <libkern/libkern.h>
#endif

#include <string.h>

namespace DbC {

const char *statusName(Status status)
{
    switch (status) {
    case Status::Success:
        return "success";
    case Status::InvalidArgument:
        return "invalid argument";
    case Status::InvalidState:
        return "invalid state";
    case Status::Busy:
        return "busy";
    case Status::Timeout:
        return "timeout";
    case Status::Disconnected:
        return "disconnected";
    case Status::ControllerReset:
        return "controller reset";
    case Status::ConfigurationError:
        return "configuration error";
    case Status::Unavailable:
        return "controller unavailable";
    case Status::TransferError:
        return "transfer error";
    case Status::InvalidEvent:
        return "invalid event";
    }

    return "unknown status";
}

const char *stateName(State state)
{
    switch (state) {
    case State::Released:
        return "released";
    case State::Prepared:
        return "prepared";
    case State::Waiting:
        return "waiting";
    case State::Running:
        return "running";
    case State::Faulted:
        return "faulted";
    }

    return "unknown state";
}

Wait Wait::forDuration(Platform &platform, uint32_t microseconds)
{
    uint64_t now = platform.timeMicroseconds();
    uint64_t end = now + microseconds;

    if (end < now) {
        end = UINT64_MAX;
    }

    return {end, microseconds < 100000 ? microseconds + 1 : 100000};
}

Controller::Guard::Guard(Controller &controller)
    : owner(controller),
      acquired(!__atomic_test_and_set(&controller.locked, __ATOMIC_ACQUIRE))
{
}

Controller::Guard::~Guard()
{
    if (acquired) {
        __atomic_clear(&owner.locked, __ATOMIC_RELEASE);
    }
}

bool Controller::Budget::advance()
{
    if (!wait.pollLimit || platform.timeMicroseconds() >= wait.deadline) {
        return false;
    }

    --wait.pollLimit;
    platform.pause();
    return true;
}

uint64_t Controller::deviceAddress(size_t offset) const
{
    return configuration.memory.deviceAddress + offset;
}

uint8_t *Controller::bytes(size_t offset) const
{
    return static_cast<uint8_t *>(configuration.memory.bytes) + offset;
}

uint64_t Controller::read64(uint32_t offset)
{
    uint64_t low = configuration.platform->read32(offset);
    uint64_t high = configuration.platform->read32(offset + 4);

    return low | (high << 32);
}

void Controller::write64(uint32_t offset, uint64_t value)
{
    configuration.platform->write32(offset, uint32_t(value));
    configuration.platform->write32(offset + 4, uint32_t(value >> 32));
}

static bool validString(const char *string)
{
    if (!string) {
        return false;
    }

    for (size_t i = 0; i <= 126; ++i) {
        if (!string[i]) {
            return true;
        }

        if (uint8_t(string[i]) < 0x20 || uint8_t(string[i]) > 0x7E) {
            return false;
        }
    }

    return false;
}

Status Controller::initialize(const Configuration &value)
{
    Guard guard(*this);

    if (!guard) {
        return Status::Busy;
    }

    if (snapshot.state != State::Released) {
        return Status::InvalidState;
    }

    if (!value.platform || (value.addressBits != 32 && value.addressBits != 64)) {
        return Status::InvalidArgument;
    }

    const auto &memory = value.memory;
    uintptr_t hostAddress = reinterpret_cast<uintptr_t>(memory.bytes);
    constexpr size_t alignmentMask = Layout::kAlignment - 1;

    if (!memory.bytes || memory.length < Layout::kLength || (hostAddress & alignmentMask) ||
        (memory.deviceAddress & alignmentMask)) {
        return Status::InvalidArgument;
    }

    if (!memory.deviceAddress || memory.deviceAddress > UINT64_MAX - Layout::kLength) {
        return Status::InvalidArgument;
    }

    uint64_t memoryEnd = memory.deviceAddress + Layout::kLength;

    if (value.addressBits == 32 && memoryEnd > (1ULL << 32)) {
        return Status::InvalidArgument;
    }

    const auto &identity = value.identity;

    if (!identity.vendorID || !identity.productID || !validString(identity.manufacturer) ||
        !validString(identity.product) || !validString(identity.serialNumber)) {
        return Status::InvalidArgument;
    }

    configuration = value;
    snapshot = {};
    snapshot.state = State::Prepared;
    return Status::Success;
}

Status Controller::release()
{
    Guard guard(*this);

    if (!guard) {
        return Status::Busy;
    }

    if (ownsRegisters) {
        return Status::Busy;
    }

    configuration = {};
    snapshot = {};
    return Status::Success;
}

static uint8_t encodeString(uint8_t *destination, const char *string)
{
    size_t length = strlen(string);
    destination[0] = uint8_t(length * 2 + 2);
    destination[1] = 3;

    for (size_t i = 0; i < length; ++i) {
        destination[2 + i * 2] = uint8_t(string[i]);
        destination[3 + i * 2] = 0;
    }

    return destination[0];
}

void Controller::prepareMemory()
{
    memset(bytes(0), 0, Layout::kLength);
    out = {};
    in = {};
    eventIndex = 0;
    eventCycle = true;

    auto &context = *reinterpret_cast<Context *>(bytes(Layout::kContext));
    auto &table = *reinterpret_cast<EventRingSegment *>(bytes(Layout::kSegmentTable));
    table.address = deviceAddress(Layout::kEventRing);
    table.size = Layout::kTRBCount;

    auto *strings = bytes(Layout::kStrings);
    strings[0] = 4;
    strings[1] = 3;
    strings[2] = 9;
    strings[3] = 4;

    context.information.language = deviceAddress(Layout::kStrings);
    context.information.manufacturer = deviceAddress(Layout::kStrings + 0x100);
    context.information.product = deviceAddress(Layout::kStrings + 0x200);
    context.information.serialNumber = deviceAddress(Layout::kStrings + 0x300);

    const auto &identity = configuration.identity;
    uint8_t manufacturerLength = encodeString(strings + 0x100, identity.manufacturer);
    uint8_t productLength = encodeString(strings + 0x200, identity.product);
    uint8_t serialNumberLength = encodeString(strings + 0x300, identity.serialNumber);

    context.information.stringLengths = InfoField::LanguageLength::encode(4) |
        InfoField::ManufacturerLength::encode(manufacturerLength) |
        InfoField::ProductLength::encode(productLength) |
        InfoField::SerialNumberLength::encode(serialNumberLength);

    uint32_t burst = DCCTRL::MaxBurst::decode(snapshot.control);
    uint32_t endpointParameters =
        EndpointField::MaxBurst::encode(burst) | EndpointField::MaxPacketSize::encode(1024);

    context.out.information2 =
        endpointParameters | EndpointField::Type::encode(EndpointField::kBulkOut);
    context.in.information2 =
        endpointParameters | EndpointField::Type::encode(EndpointField::kBulkIn);

    context.out.dequeue = deviceAddress(Layout::kOutRing) | EndpointField::kDequeueCycle;
    context.in.dequeue = deviceAddress(Layout::kInRing) | EndpointField::kDequeueCycle;

    context.out.transferInformation =
        EndpointField::AverageTRBLength::encode(Layout::kBufferLength);
    context.in.transferInformation = context.out.transferInformation;
}

Status Controller::open(Wait wait)
{
    Guard guard(*this);

    if (!guard) {
        return Status::Busy;
    }

    if (snapshot.state == State::Released || snapshot.state == State::Faulted) {
        return Status::InvalidState;
    }

    auto &platform = *configuration.platform;

    if (!platform.isReady()) {
        return Status::Unavailable;
    }

    if (snapshot.state == State::Prepared) {
        snapshot.control = platform.read32(Register::kDCCTRL);

        if (snapshot.control & DCCTRL::kEnable) {
            return Status::Busy;
        }

        uint32_t capabilityID = DCID::CapabilityID::decode(platform.read32(Register::kDCID));

        if (capabilityID != DCID::kDebugCapabilityID) {
            return Status::InvalidArgument;
        }

        if (DCCTRL::MaxBurst::decode(snapshot.control) > 15) {
            return Status::InvalidArgument;
        }

        preservedControl = snapshot.control & DCCTRL::kReservedPreserve;
        preservedDoorbell = platform.read32(Register::kDCDB) & DCDB::kReservedPreserve;

        prepareMemory();
        platform.synchronizeDMA(DMADirection::ToDevice);

        uint32_t segmentCount =
            (platform.read32(Register::kDCERSTSZ) & DCERSTSZ::kReservedPreserve) | 1;
        platform.write32(Register::kDCERSTSZ, segmentCount);

        uint64_t segmentTable = deviceAddress(Layout::kSegmentTable) |
            (read64(Register::kDCERSTBA) & DCERSTBA::kReservedPreserve);
        write64(Register::kDCERSTBA, segmentTable);

        uint64_t eventDequeue = deviceAddress(Layout::kEventRing) |
            (read64(Register::kDCERDP) & DCERDP::kReservedPreserve);
        write64(Register::kDCERDP, eventDequeue);

        uint64_t contextPointer =
            deviceAddress(Layout::kContext) | (read64(Register::kDCCP) & DCCP::kReservedPreserve);
        write64(Register::kDCCP, contextPointer);

        const auto &identity = configuration.identity;
        uint32_t descriptorInfo1 = DCDDI1::VendorID::encode(identity.vendorID);
        uint32_t descriptorInfo2 = DCDDI2::ProductID::encode(identity.productID) |
            DCDDI2::Revision::encode(identity.revision);

        platform.write32(Register::kDCDDI1, descriptorInfo1);
        platform.write32(Register::kDCDDI2, descriptorInfo2);

        ownsRegisters = true;
        captureRegisters(snapshot);
        snapshot.configurationVerified = verifyConfiguration(snapshot);

        // Validate device-address registers before allowing the controller to start DMA.
        if (!snapshot.configurationVerified) {
            return fault(Status::ConfigurationError);
        }

        snapshot.state = State::Waiting;

        // DRC disables the doorbell until software acknowledges the previous Run change.
        uint32_t control = preservedControl | DCCTRL::kEnable | DCCTRL::kLinkStatusEventEnable |
            DCCTRL::kRunChange;
        platform.write32(Register::kDCCTRL, control);
    }

    Budget budget{platform, wait};

    do {
        auto status = pollLocked();

        if (status != Status::Success && status != Status::Disconnected) {
            return status;
        }

        if (snapshot.state == State::Running) {
            return Status::Success;
        }
    } while (budget.advance());

    ++snapshot.timeouts;
    snapshot.lastStatus = Status::Timeout;

    return Status::Timeout;
}

Status Controller::close(Wait wait)
{
    Guard guard(*this);

    if (!guard) {
        return Status::Busy;
    }

    if (!ownsRegisters) {
        return Status::Success;
    }

    auto &platform = *configuration.platform;

    if (!platform.isReady()) {
        return Status::Unavailable;
    }

    platform.write32(Register::kDCCTRL, preservedControl);
    Budget budget{platform, wait};

    do {
        if (!platform.isReady()) {
            return Status::Unavailable;
        }

        uint32_t control = platform.read32(Register::kDCCTRL);
        if (!(control & DCCTRL::kEnable)) {
            platform.synchronizeDMA(DMADirection::FromDevice);

            // DCE clear terminates transfer and event DMA (§7.6.8.4).
            write64(Register::kDCCP, read64(Register::kDCCP) & DCCP::kReservedPreserve);
            write64(Register::kDCERSTBA, read64(Register::kDCERSTBA) & DCERSTBA::kReservedPreserve);
            write64(Register::kDCERDP, read64(Register::kDCERDP) & DCERDP::kReservedPreserve);
            platform.write32(Register::kDCERSTSZ,
                platform.read32(Register::kDCERSTSZ) & DCERSTSZ::kReservedPreserve);

            ownsRegisters = false;
            out = {};
            in = {};
            snapshot.state = State::Prepared;
            snapshot.lastStatus = Status::Success;
            snapshot.configurationVerified = false;
            snapshot.registersValid = false;

            return Status::Success;
        }
    } while (budget.advance());

    ++snapshot.timeouts;
    snapshot.lastStatus = Status::Timeout;

    return Status::Timeout;
}

Status Controller::fault(Status status)
{
    snapshot.state = State::Faulted;
    snapshot.lastStatus = status;

    return status;
}

Status Controller::poll()
{
    Guard guard(*this);

    if (!guard) {
        return Status::Busy;
    }

    return pollLocked();
}

Status Controller::enterPanic(Wait wait)
{
    if (!__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) {
        __atomic_clear(&locked, __ATOMIC_RELEASE);
        return Status::Success;
    }

    // The interrupted owner will remain stopped throughout the fatal panic.
    // Stop hardware DMA before replacing its in-progress software state.
    if (!configuration.platform || !configuration.platform->isReady()) {
        return Status::Unavailable;
    }

    auto &platform = *configuration.platform;
    platform.write32(
        Register::kDCCTRL, platform.read32(Register::kDCCTRL) & DCCTRL::kReservedPreserve);
    Budget budget{platform, wait};
    do {
        if (!platform.isReady()) {
            return Status::Unavailable;
        }
        if (!(platform.read32(Register::kDCCTRL) & DCCTRL::kEnable)) {
            platform.synchronizeDMA(DMADirection::FromDevice);
            __atomic_clear(&locked, __ATOMIC_RELEASE);
            const auto status = close(wait);
            if (status == Status::Success) {
                ++snapshot.panicTakeovers;
            }
            return status;
        }
    } while (budget.advance());

    return Status::Timeout;
}

Status Controller::pollLocked()
{
    if (!ownsRegisters) {
        return Status::InvalidState;
    }

    if (snapshot.state == State::Faulted) {
        return snapshot.lastStatus;
    }

    auto &platform = *configuration.platform;

    if (!platform.isReady()) {
        return fault(Status::Unavailable);
    }

    snapshot.control = platform.read32(Register::kDCCTRL);
    if (!(snapshot.control & DCCTRL::kEnable)) {
        return fault(Status::ControllerReset);
    }

    platform.synchronizeDMA(DMADirection::FromDevice);
    auto *ring = reinterpret_cast<volatile TRB *>(bytes(Layout::kEventRing));

    for (uint16_t count = 0; count < Layout::kTRBCount; ++count) {
        uint32_t control = ring[eventIndex].control;
        if (bool(control & TRBField::kCycle) != eventCycle) {
            break;
        }

        platform.synchronizeDMA(DMADirection::FromDevice);
        TRB event{ring[eventIndex].parameter, ring[eventIndex].status, control};
        Status status = Status::Success;
        auto type = TRBField::Type::decode(control);

        // ADL can report a nonzero Port ID. This dedicated ring has one debug
        // port, whose current state is read from DCPORTSC below.
        if (type == uint8_t(TRBField::TypeValue::TransferEvent)) {
            status = complete(event);
        } else if (type != uint8_t(TRBField::TypeValue::PortStatusChangeEvent)) {
            status = Status::InvalidEvent;
        }

        if (status == Status::InvalidEvent && !snapshot.invalidEvents) {
            snapshot.firstInvalidEvent = event;
            snapshot.firstInvalidEventAddress =
                deviceAddress(Layout::kEventRing + eventIndex * sizeof(TRB));
        }

        if (++eventIndex == Layout::kTRBCount) {
            eventIndex = 0;
            eventCycle = !eventCycle;
        }

        uint64_t eventDequeue = deviceAddress(Layout::kEventRing + eventIndex * sizeof(TRB)) |
            (read64(Register::kDCERDP) & DCERDP::kReservedPreserve);
        write64(Register::kDCERDP, eventDequeue);

        if (status == Status::InvalidEvent) {
            ++snapshot.invalidEvents;
            return fault(status);
        }

        if (status == Status::TransferError) {
            return fault(status);
        }
    }

    snapshot.portStatus = platform.read32(Register::kDCPORTSC);

    if (snapshot.portStatus & DCPORTSC::kChanges) {
        platform.write32(
            Register::kDCPORTSC, snapshot.portStatus & (DCPORTSC::kEnabled | DCPORTSC::kChanges));
    }

    if (snapshot.control & DCCTRL::kRunChange) {
        if (out.pending || in.pending) {
            return fault(Status::Disconnected);
        }

        // With both rings idle, acknowledge the host's configuration change and
        // keep DCE set. DRC disables doorbells until cleared (§7.6.8.4).
        platform.write32(Register::kDCCTRL,
            preservedControl | DCCTRL::kEnable | DCCTRL::kRunChange |
                (snapshot.control & DCCTRL::kLinkStatusEventEnable));
        snapshot.control = platform.read32(Register::kDCCTRL);
        if (!(snapshot.control & DCCTRL::kEnable)) {
            return fault(Status::ControllerReset);
        }
        if (snapshot.control & DCCTRL::kRunChange) {
            return fault(Status::Disconnected);
        }
    }

    if (snapshot.control & (DCCTRL::kHaltIn | DCCTRL::kHaltOut)) {
        return fault(Status::TransferError);
    }

    bool running = snapshot.control & DCCTRL::kRun;
    snapshot.state = running ? State::Running : State::Waiting;
    snapshot.lastStatus = running ? Status::Success : Status::Disconnected;

    return snapshot.lastStatus;
}

Status Controller::complete(const TRB &event)
{
    if (event.control & TRBField::TransferEvent::kEventData) {
        return Status::InvalidEvent;
    }

    auto endpoint = TRBField::TransferEvent::EndpointID::decode(event.control);
    Transfer *transfer = nullptr;

    // DbC indices 0/1 and controller encodings 2/3 occur in deployed hardware.
    // The pending TRB address identifies the DMA buffer whose ownership returns.
    if (out.pending && event.parameter == out.trbAddress && (endpoint == 0 || endpoint == 2)) {
        transfer = &out;
    }

    if (in.pending && event.parameter == in.trbAddress && (endpoint == 1 || endpoint == 3)) {
        transfer = &in;
    }

    if (!transfer) {
        return Status::InvalidEvent;
    }

    uint32_t residual = TRBField::TransferEvent::Residual::decode(event.status);

    if (residual > transfer->requested) {
        return Status::InvalidEvent;
    }

    auto completion = TRBField::TransferEvent::Completion::decode(event.status);
    if (completion != uint8_t(TRBField::CompletionValue::Success) &&
        completion != uint8_t(TRBField::CompletionValue::ShortPacket)) {
        transfer->pending = false;
        transfer->status = Status::TransferError;
        return Status::TransferError;
    }

    transfer->completed = transfer->requested - residual;
    transfer->pending = false;
    transfer->status = Status::Success;

    if (transfer == &out) {
        snapshot.bytesWritten += transfer->completed;
        if (transfer->timedOut) {
            ++snapshot.lateWrites;
        }
    }

    return Status::Success;
}

void Controller::enqueueTransfer(Transfer &transfer, bool output, uint32_t length)
{
    size_t ringOffset = output ? Layout::kOutRing : Layout::kInRing;
    auto *ring = reinterpret_cast<TRB *>(bytes(ringOffset));
    auto &trb = ring[transfer.enqueue];

    transfer.trbAddress = deviceAddress(ringOffset + transfer.enqueue * sizeof(TRB));
    transfer.requested = length;
    transfer.completed = 0;
    transfer.consumed = 0;
    transfer.pending = true;
    transfer.timedOut = false;
    transfer.status = Status::Success;

    trb.parameter = deviceAddress(output ? Layout::kOutBuffer : Layout::kInBuffer);
    trb.status = TRBField::Normal::Length::encode(length);
    configuration.platform->synchronizeDMA(DMADirection::ToDevice);

    trb.control = TRBField::Type::encode(uint8_t(TRBField::TypeValue::Normal)) |
        TRBField::Normal::kInterruptOnCompletion | TRBField::Normal::kInterruptOnShortPacket |
        (transfer.cycle ? TRBField::kCycle : 0);

    if (++transfer.enqueue == Layout::kTRBCount - 1) {
        auto &link = ring[transfer.enqueue];
        link.parameter = deviceAddress(ringOffset);
        link.status = 0;
        link.control = TRBField::Type::encode(uint8_t(TRBField::TypeValue::Link)) |
            TRBField::Link::kToggleCycle | (transfer.cycle ? TRBField::kCycle : 0);
        transfer.enqueue = 0;
        transfer.cycle = !transfer.cycle;
    }

    configuration.platform->synchronizeDMA(DMADirection::ToDevice);
    configuration.platform->write32(
        Register::kDCDB, preservedDoorbell | DCDB::Target::encode(output ? DCDB::kOut : DCDB::kIn));
}

TransferResult Controller::waitForTransfer(Transfer &transfer, Wait wait)
{
    Budget budget{*configuration.platform, wait};

    do {
        Status status = pollLocked();

        if (status != Status::Success) {
            return {status, 0, transfer.pending};
        }

        if (!transfer.pending) {
            return {transfer.status, transfer.completed, false};
        }
    } while (budget.advance());

    transfer.timedOut = true;
    ++snapshot.timeouts;
    snapshot.lastStatus = Status::Timeout;

    return {Status::Timeout, 0, true};
}

TransferResult Controller::write(const WriteRequest &request)
{
    Guard guard(*this);

    if (!guard) {
        return {Status::Busy, 0, false};
    }

    if ((!request.bytes && request.length) || request.length > Layout::kBufferLength) {
        return {Status::InvalidArgument, 0, false};
    }

    auto status = pollLocked();

    if (status != Status::Success) {
        return {status, 0, out.pending};
    }

    if (out.pending) {
        return {Status::Busy, 0, true};
    }

    if (!request.length) {
        return {Status::Success, 0, false};
    }

    memcpy(bytes(Layout::kOutBuffer), request.bytes, request.length);
    enqueueTransfer(out, true, uint32_t(request.length));

    return waitForTransfer(out, request.wait);
}

TransferResult Controller::read(const ReadRequest &request)
{
    Guard guard(*this);

    if (!guard) {
        return {Status::Busy, 0, false};
    }

    if (!request.bytes && request.length) {
        return {Status::InvalidArgument, 0, false};
    }

    auto status = pollLocked();

    if (status != Status::Success) {
        return {status, 0, in.pending};
    }

    if (!request.length) {
        return {Status::Success, 0, false};
    }

    if (!in.pending && in.consumed == in.completed) {
        enqueueTransfer(in, false, Layout::kBufferLength);
    }

    if (in.pending) {
        auto result = waitForTransfer(in, request.wait);

        if (result.status != Status::Success) {
            return result;
        }
    }

    size_t amount = in.completed - in.consumed;
    if (amount > request.length) {
        amount = request.length;
    }

    memcpy(request.bytes, bytes(Layout::kInBuffer) + in.consumed, amount);
    in.consumed += uint32_t(amount);
    snapshot.bytesRead += amount;

    return {Status::Success, amount, false};
}

TransferResult Controller::flush(Wait wait)
{
    Guard guard(*this);
    if (!guard) {
        return {Status::Busy, 0, false};
    }
    auto status = pollLocked();
    if (status != Status::Success) {
        return {status, 0, out.pending};
    }
    if (!out.pending) {
        return {out.status, out.completed, false};
    }
    return waitForTransfer(out, wait);
}

#if defined(KERNEL) && DEBUG
Status Controller::testPanic()
{
    Guard guard(*this);
    if (!guard) {
        return Status::Busy;
    }
    const auto status = pollLocked();
    if (status != Status::Success) {
        return status;
    }
    if (snapshot.state != State::Running || out.pending) {
        return Status::Busy;
    }
    constexpr char marker[] = "\n[DarwinDbC] test.held-writer DMA submitted\n";
    memcpy(bytes(Layout::kOutBuffer), marker, sizeof(marker) - 1);
    enqueueTransfer(out, true, sizeof(marker) - 1);
    // The panic abandons both console and controller guards after a real TD submission.
    panic("DarwinDbC controlled held-writer panic: out.pending=%u trb=0x%llx bytes=%u",
        static_cast<unsigned>(out.pending),
        static_cast<unsigned long long>(out.trbAddress),
        out.requested);
}
#endif

Status Controller::copySnapshot(Snapshot &value)
{
    Guard guard(*this);

    if (!guard) {
        return Status::Busy;
    }

    value = snapshot;
    value.registersValid = false;

    if (ownsRegisters && configuration.platform->isReady()) {
        captureRegisters(value);
    }

    return Status::Success;
}

void Controller::captureRegisters(Snapshot &value)
{
    auto &platform = *configuration.platform;
    value.capabilityID = platform.read32(Register::kDCID);
    value.doorbell = platform.read32(Register::kDCDB);
    value.segmentTableSize = platform.read32(Register::kDCERSTSZ);
    value.segmentTableBase = read64(Register::kDCERSTBA);
    value.eventDequeue = read64(Register::kDCERDP);
    value.control = platform.read32(Register::kDCCTRL);
    value.debugStatus = platform.read32(Register::kDCST);
    value.portStatus = platform.read32(Register::kDCPORTSC);
    value.contextPointer = read64(Register::kDCCP);
    value.descriptorInfo1 = platform.read32(Register::kDCDDI1);
    value.descriptorInfo2 = platform.read32(Register::kDCDDI2);
    value.registersValid = true;
}

bool Controller::verifyConfiguration(const Snapshot &value) const
{
    const auto &identity = configuration.identity;
    uint32_t descriptorInfo1 = DCDDI1::VendorID::encode(identity.vendorID);
    uint32_t descriptorInfo2 =
        DCDDI2::ProductID::encode(identity.productID) | DCDDI2::Revision::encode(identity.revision);
    uint64_t segmentTableAddress = value.segmentTableBase & DCERSTBA::Address::kMask;
    uint64_t eventDequeue =
        value.eventDequeue & (DCERDP::Address::kMask | DCERDP::SegmentIndex::kMask);
    uint64_t contextAddress = value.contextPointer & DCCP::Address::kMask;
    uint32_t descriptorFields =
        value.descriptorInfo1 & (DCDDI1::Protocol::kMask | DCDDI1::VendorID::kMask);

    bool validCapability =
        DCID::CapabilityID::decode(value.capabilityID) == DCID::kDebugCapabilityID;
    bool validTable = DCERSTSZ::Size::decode(value.segmentTableSize) == 1 &&
        segmentTableAddress == deviceAddress(Layout::kSegmentTable);
    bool validPointers = eventDequeue == deviceAddress(Layout::kEventRing) &&
        contextAddress == deviceAddress(Layout::kContext);
    bool validIdentity =
        descriptorFields == descriptorInfo1 && value.descriptorInfo2 == descriptorInfo2;

    return validCapability && validTable && validPointers && validIdentity;
}
}
