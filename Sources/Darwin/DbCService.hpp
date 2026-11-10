// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "DbCConsole.hpp"
#include "DbCDarwinPlatform.hpp"

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

class DarwinDbCService final : public IOService {
    OSDeclareDefaultStructors(DarwinDbCService)

public:
    static DarwinDbCService *withProvider(IOService *provider);
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
#if DEBUG
    IOReturn setProperties(OSObject *properties) override;
#endif

    IOReturn powerStateWillChangeTo(
        IOPMPowerFlags capabilities, unsigned long stateNumber, IOService *device) override;

    IOReturn powerStateDidChangeTo(
        IOPMPowerFlags capabilities, unsigned long stateNumber, IOService *device) override;

private:
    IOWorkLoop *workLoop{};
    IOTimerEventSource *timer{};
    bool ownsTransport{};
    bool powerAvailable{};
    bool timerAttached{};
    bool powerTracked{};
    bool faultRecorded{};
    uint64_t nextPropertyUpdate{};
    DbC::Status reportedStatus{DbC::Status::InvalidState};
    uint64_t powerTransitions{};
    uint64_t controllerResets{};
    uint64_t disconnects{};
    uint64_t configurationErrors{};

    static IOPCIDevice *findPCIDevice(IOService *provider);
    static bool matchesSelector(IOPCIDevice *device);
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);

    DbC::Status closeTransport();
    bool releaseTransport();
    void releaseTimer();
    void recordStart(IOService *provider, const char *stage, DbC::Status status);
    void recordHardware(IOPCIDevice *device);
    void updateProperties(DbC::Status status);
};
