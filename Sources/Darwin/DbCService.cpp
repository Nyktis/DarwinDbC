// SPDX-License-Identifier: BSD-2-Clause
#include "DbCService.hpp"
#include "DbCPanic.hpp"

#include <Headers/plugin_start.hpp>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>

using DbC::Status;

namespace {

// Lilu keeps this module resident. These objects retain DMA state across provider teardown.
DbC::DarwinPlatform platform;
DbC::Controller controller;
bool transportClaimed{};

constexpr DbC::Identity kIdentity{
    0xFFFF,
    0x0001,
    0x0100,
    "DarwinDbC",
    "Kernel Console",
    "0001",
};

int decodeHex(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }

    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

void setNumber(OSDictionary *dictionary, const char *key, uint64_t value)
{
    auto *number = OSNumber::withNumber(value, 64);

    if (number) {
        dictionary->setObject(key, number);
        number->release();
    }
}

void setString(OSDictionary *dictionary, const char *key, const char *value)
{
    auto *string = OSString::withCString(value);

    if (string) {
        dictionary->setObject(key, string);
        string->release();
    }
}
}

OSDefineMetaClassAndStructors(DarwinDbCService, IOService)

#if DEBUG
IOReturn DarwinDbCService::setProperties(OSObject *properties)
{
    auto *dictionary = OSDynamicCast(OSDictionary, properties);
    auto *request = dictionary ? dictionary->getObject("DarwinDbCPanicTest") : nullptr;
    if (!request) {
        return IOService::setProperties(properties);
    }
    const auto privilege =
        IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator);
    if (privilege != kIOReturnSuccess) {
        return privilege;
    }
    auto *mode = OSDynamicCast(OSString, request);
    if (!mode || !mode->isEqualTo("held-writer")) {
        return kIOReturnBadArgument;
    }
    const auto status = DbC::Console::testPanic();
    return status == Status::Busy ? kIOReturnBusy : kIOReturnNotReady;
}
#endif

IOPCIDevice *DarwinDbCService::findPCIDevice(IOService *provider)
{
    for (unsigned depth = 0; provider && depth < 32; ++depth) {
        auto *device = OSDynamicCast(IOPCIDevice, provider);

        if (device) {
            return device;
        }

        provider = provider->getProvider();
    }

    return nullptr;
}

bool DarwinDbCService::matchesSelector(IOPCIDevice *device)
{
    char selector[16]{};

    if (!device || !PE_parse_boot_argn("dbc-pci", selector, sizeof(selector))) {
        return false;
    }

    if (selector[2] != ':' || selector[5] != '.' || selector[7] != '\0') {
        return false;
    }

    int busHigh = decodeHex(selector[0]);
    int busLow = decodeHex(selector[1]);
    int deviceHigh = decodeHex(selector[3]);
    int deviceLow = decodeHex(selector[4]);
    int function = decodeHex(selector[6]);

    if (busHigh < 0 || busLow < 0 || deviceHigh < 0 || deviceLow < 0 || function < 0 ||
        function > 7) {
        return false;
    }

    unsigned bus = static_cast<unsigned>(busHigh * 16 + busLow);
    unsigned slot = static_cast<unsigned>(deviceHigh * 16 + deviceLow);

    return slot <= 31 && device->getBusNumber() == bus && device->getDeviceNumber() == slot &&
        device->getFunctionNumber() == function;
}

DarwinDbCService *DarwinDbCService::withProvider(IOService *provider)
{
    bool selected = matchesSelector(findPCIDevice(provider));
    auto *result = OSDictionary::withCapacity(3);

    if (result) {
        result->setObject("BootstrapReady", ADDPR(startSuccess) ? kOSBooleanTrue : kOSBooleanFalse);
        result->setObject("Selected", selected ? kOSBooleanTrue : kOSBooleanFalse);
        setString(result, "ConsoleRouting", DbC::Console::routingStatus());
        provider->setProperty("DarwinDbCProbe", result);
        result->release();
    }

    if (!selected || !ADDPR(startSuccess)) {
        return nullptr;
    }

    auto *service = new DarwinDbCService;

    if (!service) {
        return nullptr;
    }

    if (!service->init()) {
        service->release();
        return nullptr;
    }

    // IOService identifies client match categories through interned symbols.
    auto *category = const_cast<OSSymbol *>(gMetaClass.getClassNameSymbol());
    service->setProperty(gIOMatchCategoryKey, category);

    if (!service->attach(provider)) {
        service->release();
        return nullptr;
    }

    if (!service->start(provider)) {
        service->detach(provider);
        service->release();
        return nullptr;
    }

    service->registerService();
    return service;
}

bool DarwinDbCService::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }

    bool expected = false;

    if (!__atomic_compare_exchange_n(
            &transportClaimed, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        recordStart(provider, "claim", Status::Busy);
        return false;
    }

    ownsTransport = true;
    auto fail = [&](const char *stage, Status failure) {
        nextPropertyUpdate = 0;
        updateProperties(failure);
        releaseTimer();
        releaseTransport();
        recordStart(provider, stage, failure);

        return false;
    };

    Status status = platform.initialize(findPCIDevice(provider));

    if (status != Status::Success) {
        return fail("platform", status);
    }

    recordHardware(findPCIDevice(provider));
    DbC::Configuration configuration{
        &platform,
        platform.memory(),
        kIdentity,
        platform.addressBits(),
    };
    status = controller.initialize(configuration);

    if (status != Status::Success) {
        return fail("controller", status);
    }

    status = DbC::Console::initialize({&controller, &platform, 1000});

    if (status != Status::Success) {
        return fail("console", status);
    }

    workLoop = IOWorkLoop::workLoop();
    timer = IOTimerEventSource::timerEventSource(this, timerFired);

    if (!workLoop || !timer || workLoop->addEventSource(timer) != kIOReturnSuccess) {
        return fail("work loop", Status::Unavailable);
    }

    timerAttached = true;
    status = controller.open(DbC::Wait::forDuration(platform, 0));

    if (status != Status::Success && status != Status::Timeout) {
        return fail("enable", status);
    }

    __atomic_store_n(&powerAvailable, true, __ATOMIC_RELEASE);
    provider->registerInterestedDriver(this);
    powerTracked = true;

    recordStart(provider, "ready", Status::Success);
    updateProperties(status);
    timer->setTimeoutMS(10);

    return true;
}

Status DarwinDbCService::closeTransport()
{
    DbC::Console::detach();
    DbC::Wait wait = DbC::Wait::forDuration(platform, 50000);
    Status status = Status::Busy;

    do {
        status = controller.close(wait);

        if (status != Status::Busy) {
            return status;
        }

        platform.pause();
    } while (wait.pollLimit-- && platform.timeMicroseconds() < wait.deadline);

    return status;
}

bool DarwinDbCService::releaseTransport()
{
    if (!ownsTransport) {
        return true;
    }

    if (closeTransport() != Status::Success || DbC::Console::release() != Status::Success ||
        controller.release() != Status::Success) {
        setProperty("DMAState", "retained");
        return false;
    }

    platform.release();
    ownsTransport = false;
    __atomic_store_n(&transportClaimed, false, __ATOMIC_RELEASE);

    return true;
}

void DarwinDbCService::stop(IOService *provider)
{
    if (powerTracked) {
        provider->deRegisterInterestedDriver(this);
        powerTracked = false;
    }

    __atomic_store_n(&powerAvailable, false, __ATOMIC_RELEASE);
    releaseTimer();

    releaseTransport();
    platform.setPowerAvailable(false);
    IOService::stop(provider);
}

void DarwinDbCService::free()
{
    releaseTimer();
    IOService::free();
}

void DarwinDbCService::releaseTimer()
{
    if (timer) {
        timer->cancelTimeout();
        timer->disable();

        if (timerAttached) {
            workLoop->removeEventSource(timer);
            timerAttached = false;
        }

        timer->release();
        timer = nullptr;
    }

    if (workLoop) {
        workLoop->release();
        workLoop = nullptr;
    }
}

IOReturn DarwinDbCService::powerStateWillChangeTo(
    IOPMPowerFlags capabilities, unsigned long, IOService *)
{
    if (!(capabilities & kIOPMDeviceUsable) &&
        __atomic_exchange_n(&powerAvailable, false, __ATOMIC_ACQ_REL)) {

        workLoop->runActionBlock(^IOReturn {
            Status status = closeTransport();

            if (status == Status::Success) {
                faultRecorded = false;
            }

            platform.setPowerAvailable(false);
            ++powerTransitions;
            nextPropertyUpdate = 0;
            updateProperties(status);

            return kIOReturnSuccess;
        });
    }

    return kIOPMAckImplied;
}

IOReturn DarwinDbCService::powerStateDidChangeTo(
    IOPMPowerFlags capabilities, unsigned long, IOService *)
{
    if ((capabilities & kIOPMDeviceUsable) && !__atomic_load_n(&powerAvailable, __ATOMIC_ACQUIRE)) {
        workLoop->runActionBlock(^IOReturn {
            platform.setPowerAvailable(true);
            __atomic_store_n(&powerAvailable, true, __ATOMIC_RELEASE);
            ++powerTransitions;
            nextPropertyUpdate = 0;
            updateProperties(Status::Success);

            return kIOReturnSuccess;
        });
    }

    return kIOPMAckImplied;
}

void DarwinDbCService::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *service = OSDynamicCast(DarwinDbCService, owner);

    if (!service || !__atomic_load_n(&service->powerAvailable, __ATOMIC_ACQUIRE)) {
        sender->setTimeoutMS(100);
        return;
    }

    Status status = controller.poll();
    DbC::Snapshot snapshot{};

    if (controller.copySnapshot(snapshot) == Status::Success) {
        if (snapshot.state == DbC::State::Waiting) {
            DbC::Console::detach();
        }

        if (snapshot.state == DbC::State::Faulted && !service->faultRecorded) {
            service->faultRecorded = true;
            service->nextPropertyUpdate = 0;
            service->setProperty("LastError", DbC::statusName(snapshot.lastStatus));
            service->setProperty("LastErrorTimeMicroseconds", platform.timeMicroseconds(), 64);
            service->setProperty("LastErrorDCCTRL", uint64_t{snapshot.control}, 64);
            service->setProperty("LastErrorDCPORTSC", uint64_t{snapshot.portStatus}, 64);

            if (snapshot.lastStatus == Status::ControllerReset) {
                ++service->controllerResets;
            } else if (snapshot.lastStatus == Status::Disconnected) {
                ++service->disconnects;
            } else if (snapshot.lastStatus == Status::ConfigurationError) {
                ++service->configurationErrors;
            }
        }

        if (snapshot.state == DbC::State::Prepared) {
            status = controller.open(DbC::Wait::forDuration(platform, 0));
        } else if (snapshot.state == DbC::State::Faulted && platform.isReady()) {
            Status closeStatus = service->closeTransport();

            if (closeStatus == Status::Success) {
                service->faultRecorded = false;
                status = controller.open(DbC::Wait::forDuration(platform, 0));
            } else {
                status = closeStatus;
            }
        }
    }

    if (status == Status::Success && DbC::Console::isReady() &&
        __atomic_load_n(&service->powerAvailable, __ATOMIC_ACQUIRE)) {
        if (DbC::Console::attach()) {
            IOLog("[DarwinDbC] link=ready uptime_us=%llu\n",
                static_cast<unsigned long long>(platform.timeMicroseconds()));
        }
        DbC::Console::flush();
    }

    service->updateProperties(status);
    sender->setTimeoutMS(10);
}

void DarwinDbCService::recordStart(IOService *provider, const char *stage, Status status)
{
    auto *result = OSDictionary::withCapacity(5);

    if (result) {
        setString(result, "Stage", stage);
        setString(result, "Status", DbC::statusName(status));
        const char *dmaState = "released";

        if (ownsTransport) {
            dmaState = status == Status::Success ? "mapped" : "retained";
        }

        setString(result, "DMAState", dmaState);

        if (auto *hardware = getProperty("Hardware")) {
            result->setObject("Hardware", hardware);
        }

        if (auto *registers = getProperty("Registers")) {
            result->setObject("Registers", registers);
        }

        provider->setProperty("DarwinDbCStart", result);
        result->release();
    }

    if (status != Status::Success) {
        IOLog("DarwinDbC: %s: %s\n", stage, DbC::statusName(status));
    }
}

void DarwinDbCService::recordHardware(IOPCIDevice *device)
{
    DbC::DarwinPlatform::Information information{};

    if (platform.copyInformation(information) != Status::Success) {
        return;
    }

    auto *hardware = OSDictionary::withCapacity(10);

    if (!hardware) {
        return;
    }

    char address[8]{};
    snprintf(address,
        sizeof(address),
        "%02x:%02x.%x",
        device->getBusNumber(),
        device->getDeviceNumber(),
        device->getFunctionNumber());
    setString(hardware, "PCIAddress", address);
    setNumber(hardware, "BARLength", information.registerLength);
    setNumber(hardware, "DbCOffset", information.capability.offset);
    setNumber(hardware, "OperationalOffset", information.capability.operationalOffset);
    setNumber(hardware, "AddressBits", information.capability.addressBits);
    setNumber(hardware, "ERSTMax", information.capability.erstMax);
    setNumber(hardware, "DMABase", information.dmaAddress);
    setNumber(hardware, "DMALength", information.dmaLength);
    setNumber(hardware, "DMAAlignment", DbC::Layout::kAlignment);
    hardware->setObject(
        "UsesIOMapper", information.usesIOMapper ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Hardware", hardware);
    hardware->release();
}

void DarwinDbCService::updateProperties(Status status)
{
    uint64_t now = platform.timeMicroseconds();

    if (now < nextPropertyUpdate && status == reportedStatus) {
        return;
    }

    DbC::Snapshot snapshot{};

    if (controller.copySnapshot(snapshot) != Status::Success) {
        return;
    }

    reportedStatus = status;
    nextPropertyUpdate = now + 1000000;
    bool powered = __atomic_load_n(&powerAvailable, __ATOMIC_ACQUIRE);
    setProperty("TransportStatus", DbC::statusName(status));
    setProperty("ConsoleRouting", DbC::Console::routingStatus());
    setProperty("PanicReady", DbC::PanicConsole::isReady() && DbC::Console::isReady());
    setProperty("PanicBufferReady", DbC::PanicConsole::hasBuffer());
    setProperty("PanicTakeovers", snapshot.panicTakeovers, 64);
    setProperty("TransportState", powered ? DbC::stateName(snapshot.state) : "suspended");
    setProperty("ConfigurationVerified", snapshot.configurationVerified);
    setProperty("RegistersValid", snapshot.registersValid);
    setProperty("SampleTimeMicroseconds", now, 64);
    setProperty("PowerTransitions", powerTransitions, 64);
    setProperty("ControllerResets", controllerResets, 64);
    setProperty("Disconnects", disconnects, 64);
    setProperty("ConfigurationErrors", configurationErrors, 64);
    setProperty("BytesRead", snapshot.bytesRead, 64);
    setProperty("BytesWritten", snapshot.bytesWritten, 64);
    setProperty("Timeouts", snapshot.timeouts, 64);
    setProperty("InvalidEvents", snapshot.invalidEvents, 64);
    setProperty("LateWrites", snapshot.lateWrites, 64);

    if (snapshot.invalidEvents) {
        char event[128];
        snprintf(event,
            sizeof(event),
            "address=%016llx parameter=%016llx status=%08x control=%08x",
            static_cast<unsigned long long>(snapshot.firstInvalidEventAddress),
            static_cast<unsigned long long>(snapshot.firstInvalidEvent.parameter),
            snapshot.firstInvalidEvent.status,
            snapshot.firstInvalidEvent.control);
        setProperty("FirstInvalidEvent", event);
    }

    if (snapshot.registersValid) {
        auto *registers = OSDictionary::withCapacity(11);

        if (registers) {
            setNumber(registers, "DCID", snapshot.capabilityID);
            setNumber(registers, "DCDB", snapshot.doorbell);
            setNumber(registers, "DCERSTSZ", snapshot.segmentTableSize);
            setNumber(registers, "DCERSTBA", snapshot.segmentTableBase);
            setNumber(registers, "DCERDP", snapshot.eventDequeue);
            setNumber(registers, "DCCTRL", snapshot.control);
            setNumber(registers, "DCST", snapshot.debugStatus);
            setNumber(registers, "DCPORTSC", snapshot.portStatus);
            setNumber(registers, "DCCP", snapshot.contextPointer);
            setNumber(registers, "DCDDI1", snapshot.descriptorInfo1);
            setNumber(registers, "DCDDI2", snapshot.descriptorInfo2);
            setProperty("Registers", registers);
            registers->release();
        }
    } else {
        removeProperty("Registers");
    }

    DbC::Console::Counters counters = DbC::Console::copyCounters();
    setProperty("ConfirmedBytes", counters.confirmedBytes, 64);
    setProperty("UnconfirmedBytes", counters.unconfirmedBytes, 64);
    setProperty("DiscardedBytes", counters.discardedBytes, 64);
    setProperty("QueuedBytes", counters.queuedBytes, 64);
    setProperty("HistoryBytes", counters.historyBytes, 64);
}
