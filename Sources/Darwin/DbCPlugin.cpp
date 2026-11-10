// SPDX-License-Identifier: BSD-2-Clause
#include "DbCPlugin.hpp"
#include "DbCService.hpp"
#include "DbCPanic.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <Headers/kern_version.hpp>
#include <IOKit/IONotifier.h>
#include <pexpert/pexpert.h>

namespace {

const char *disableArguments[]{"-dbcoff"};
const char *debugArguments[]{"-dbcdbg"};
const char *betaArguments[]{"-dbcbeta"};

void pluginStart()
{
    char selector[16]{};

    if (!PE_parse_boot_argn("dbc-pci", selector, sizeof(selector))) {
        return;
    }

    auto error = lilu.onPatcherLoad([](void *, KernelPatcher &patcher) {
        if (!DbC::Console::route(patcher)) {
            IOLog("DarwinDbC: console routing failed\n");
        } else {
            IOLog("[DarwinDbC] capture=ready\n");
        }
        if (!DbC::PanicConsole::route(patcher)) {
            IOLog("DarwinDbC: panic routing failed\n");
        }
    });

    if (error != LiluAPI::Error::NoError) {
        IOLog("DarwinDbC: patcher registration failed (%u)\n", static_cast<unsigned>(error));
    }
}
}

OSDefineMetaClassAndStructors(DarwinDbC, IOService)

IOService *DarwinDbC::probe(IOService *provider, SInt32 *score)
{
    setProperty("VersionInfo", kextVersion);
    return ADDPR(startSuccess) ? IOService::probe(provider, score) : nullptr;
}

bool DarwinDbC::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }

    setProperty("ControllerNotification", false);
    setProperty("ControllersSeen", uint64_t{0}, 32);
    setProperty("TransportAttached", false);
    char selector[16]{};

    if (!PE_parse_boot_argn("dbc-pci", selector, sizeof(selector))) {
        return true;
    }

    discoveryLock = IOLockAlloc();

    if (!discoveryLock) {
        return false;
    }

    // Controller configuration personalities use platform identity matching.
    // Class-only notifications discover registered xHCI controllers across platforms.
    // https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOService.cpp
    auto *matching = serviceMatching("AppleUSBXHCI");

    if (!matching) {
        return false;
    }

    controllerNotifier =
        addMatchingNotification(gIOFirstMatchNotification, matching, controllerMatched, this);
    matching->release();
    setProperty("ControllerNotification", controllerNotifier != nullptr);

    return controllerNotifier != nullptr;
}

bool DarwinDbC::controllerMatched(void *target, void *, IOService *provider, IONotifier *)
{
    auto *service = static_cast<DarwinDbC *>(target);
    IOLockLock(service->discoveryLock);
    service->setProperty("ControllersSeen", ++service->controllersSeen, 32);

    if (service->transport && service->transport->isInactive()) {
        service->transport->release();
        service->transport = nullptr;
    }

    if (!service->transport) {
        service->transport = DarwinDbCService::withProvider(provider);
    }

    service->setProperty("TransportAttached", service->transport != nullptr);
    IOLockUnlock(service->discoveryLock);

    return true;
}

void DarwinDbC::releaseDiscovery()
{
    // Removing the notification waits for controller callbacks before releasing the child.
    if (controllerNotifier) {
        controllerNotifier->remove();
        controllerNotifier = nullptr;
    }

    if (transport) {
        transport->terminate(kIOServiceSynchronous);
        transport->release();
        transport = nullptr;
    }
}

void DarwinDbC::stop(IOService *provider)
{
    releaseDiscovery();
    IOService::stop(provider);
}

void DarwinDbC::free()
{
    releaseDiscovery();

    if (discoveryLock) {
        IOLockFree(discoveryLock);
        discoveryLock = nullptr;
    }

    IOService::free();
}

PluginConfiguration ADDPR(config){
    "DarwinDbC",
    parseModuleVersion("0.1.11"),
    LiluAPI::AllowNormal,
    disableArguments,
    arrsize(disableArguments),
    debugArguments,
    arrsize(debugArguments),
    betaArguments,
    arrsize(betaArguments),
    KernelVersion::Ventura,
    KernelVersion::Tahoe,
    pluginStart,
};
