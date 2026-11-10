// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>

class DarwinDbCService;

class DarwinDbC final : public IOService {
    OSDeclareDefaultStructors(DarwinDbC)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    IOLock *discoveryLock{};
    IONotifier *controllerNotifier{};
    DarwinDbCService *transport{};
    uint32_t controllersSeen{};

    static bool controllerMatched(
        void *target, void *reference, IOService *provider, IONotifier *notifier);
    void releaseDiscovery();
};
