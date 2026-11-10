// SPDX-License-Identifier: BSD-2-Clause
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--trigger") || geteuid() != 0) {
        fprintf(stderr, "Usage: sudo %s --trigger\n", argv[0]);
        return 2;
    }
    auto service =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("DarwinDbCService"));
    if (!service) {
        fprintf(stderr, "DarwinDbCService is unavailable.\n");
        return 1;
    }
    auto ready =
        IORegistryEntryCreateCFProperty(service, CFSTR("PanicBufferReady"), kCFAllocatorDefault, 0);
    if (!ready || !CFEqual(ready, kCFBooleanTrue)) {
        if (ready) {
            CFRelease(ready);
        }
        IOObjectRelease(service);
        fprintf(stderr, "PanicBufferReady must be Yes.\n");
        return 1;
    }
    CFRelease(ready);
    printf("Save your work and keep the receiver connected.\n"
           "A held-writer kernel panic will be triggered in 10 seconds. Ctrl-C cancels.\n");
    fflush(stdout);
    sleep(10);
    sync();
    const void *keys[] = {CFSTR("DarwinDbCPanicTest")};
    const void *values[] = {CFSTR("held-writer")};
    auto request = CFDictionaryCreate(kCFAllocatorDefault,
        keys,
        values,
        1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!request) {
        IOObjectRelease(service);
        fprintf(stderr, "Could not create the panic request.\n");
        return 1;
    }
    const auto result = IORegistryEntrySetCFProperties(service, request);
    CFRelease(request);
    IOObjectRelease(service);
    fprintf(stderr, "Panic request returned 0x%08x.\n", result);
    if (result == kIOReturnBusy || result == kIOReturnNotReady) {
        fprintf(stderr, "Wait for QueuedBytes=0 and retry.\n");
    }
    return 1;
}
