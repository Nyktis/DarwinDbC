// SPDX-License-Identifier: BSD-2-Clause
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_error.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace {

using Device = IOUSBDeviceInterface500;
using Interface = IOUSBInterfaceInterface550;
constexpr int32_t VendorID = 0xFFFF;
constexpr int32_t ProductID = 0x0001;
volatile sig_atomic_t stopping = 0;

void stop(int)
{
    stopping = 1;
}

void report(const char *operation, IOReturn result)
{
    std::fprintf(stderr, "%s: %s (0x%08x)\n", operation, mach_error_string(result), result);
}

template <typename T> T **interfaceFor(io_service_t service, CFUUIDRef client, CFUUIDRef type)
{
    IOCFPlugInInterface **plugin = nullptr;
    SInt32 score = 0;
    IOReturn result =
        IOCreatePlugInInterfaceForService(service, client, kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != kIOReturnSuccess) {
        report("Create USB interface", result);
        return nullptr;
    }

    T **interface = nullptr;
    HRESULT queried = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(type), reinterpret_cast<LPVOID *>(&interface));
    IODestroyPlugInInterface(plugin);
    if (queried != S_OK) {
        std::fprintf(stderr, "Query USB interface: 0x%08x\n", static_cast<unsigned>(queried));
        return nullptr;
    }
    return interface;
}

class Connection final {
public:
    ~Connection()
    {
        close();
    }

    IOReturn open(io_service_t service)
    {
        device =
            interfaceFor<Device>(service, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID500);
        if (!device) {
            return kIOReturnUnsupported;
        }

        IOReturn result = (*device)->USBDeviceOpen(device);
        if (result != kIOReturnSuccess) {
            return result;
        }
        deviceOpen = true;

        UInt8 configuration = 0;
        result = (*device)->GetConfiguration(device, &configuration);
        if (result != kIOReturnSuccess) {
            return result;
        }
        if (!configuration) {
            IOUSBConfigurationDescriptorPtr descriptor = nullptr;
            result = (*device)->GetConfigurationDescriptorPtr(device, 0, &descriptor);
            if (result != kIOReturnSuccess) {
                return result;
            }
            result = (*device)->SetConfiguration(device, descriptor->bConfigurationValue);
            if (result != kIOReturnSuccess) {
                return result;
            }
        }

        IOUSBFindInterfaceRequest request{kIOUSBFindInterfaceDontCare,
            kIOUSBFindInterfaceDontCare,
            kIOUSBFindInterfaceDontCare,
            kIOUSBFindInterfaceDontCare};
        io_iterator_t iterator = IO_OBJECT_NULL;
        result = (*device)->CreateInterfaceIterator(device, &request, &iterator);
        if (result != kIOReturnSuccess) {
            return result;
        }

        result = kIOReturnNotFound;
        while (io_service_t candidate = IOIteratorNext(iterator)) {
            interface = interfaceFor<Interface>(
                candidate, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID550);
            IOObjectRelease(candidate);
            if (!interface) {
                result = kIOReturnUnsupported;
                break;
            }

            result = (*interface)->USBInterfaceOpen(interface);
            if (result != kIOReturnSuccess) {
                break;
            }
            interfaceOpen = true;
            UInt8 count = 0;
            result = (*interface)->GetNumEndpoints(interface, &count);
            if (result != kIOReturnSuccess) {
                break;
            }
            for (UInt8 pipe = 1; pipe <= count; ++pipe) {
                UInt8 direction = 0, number = 0, transfer = 0, interval = 0;
                UInt16 packetBytes = 0;
                result = (*interface)
                             ->GetPipeProperties(interface,
                                 pipe,
                                 &direction,
                                 &number,
                                 &transfer,
                                 &packetBytes,
                                 &interval);
                if (result != kIOReturnSuccess) {
                    break;
                }
                if (direction == kUSBIn && transfer == kUSBBulk && packetBytes) {
                    inputPipe = pipe;
                    readBytes = packetBytes;
                    break;
                }
            }
            if (inputPipe || result != kIOReturnSuccess) {
                break;
            }
            (*interface)->USBInterfaceClose(interface);
            interfaceOpen = false;
            (*interface)->Release(interface);
            interface = nullptr;
            result = kIOReturnNotFound;
        }
        IOObjectRelease(iterator);
        return inputPipe ? kIOReturnSuccess : result;
    }

    IOReturn read(void *bytes, UInt32 &length)
    {
        // One maximum-size packet completes even when the sender supplies no short packet.
        length = readBytes;
        return (*interface)->ReadPipeTO(interface, inputPipe, bytes, &length, 250, 1000);
    }

    IOReturn clearStall()
    {
        return (*interface)->ClearPipeStallBothEnds(interface, inputPipe);
    }

    void close()
    {
        if (interface) {
            if (interfaceOpen) {
                (*interface)->USBInterfaceClose(interface);
            }
            (*interface)->Release(interface);
            interface = nullptr;
            interfaceOpen = false;
        }
        if (device) {
            if (deviceOpen) {
                (*device)->USBDeviceClose(device);
            }
            (*device)->Release(device);
            device = nullptr;
            deviceOpen = false;
        }
    }

private:
    Device **device = nullptr;
    Interface **interface = nullptr;
    bool deviceOpen = false;
    bool interfaceOpen = false;
    UInt8 inputPipe = 0;
    UInt16 readBytes = 0;
};

io_service_t findDevice()
{
    auto matching = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matching) {
        return IO_OBJECT_NULL;
    }
    auto vendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &VendorID);
    auto product = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &ProductID);
    CFDictionarySetValue(matching, CFSTR(kUSBVendorID), vendor);
    CFDictionarySetValue(matching, CFSTR(kUSBProductID), product);
    CFRelease(vendor);
    CFRelease(product);
    return IOServiceGetMatchingService(kIOMainPortDefault, matching);
}

bool writeAll(int file, const void *bytes, size_t length)
{
    auto cursor = static_cast<const char *>(bytes);
    while (length) {
        ssize_t written = ::write(file, cursor, length);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            std::perror("Write capture");
            return false;
        }
        cursor += written;
        length -= static_cast<size_t>(written);
    }
    return true;
}

}

int main(int argc, char **argv)
{
    char defaultPath[64]{};
    std::time_t now = std::time(nullptr);
    std::strftime(
        defaultPath, sizeof(defaultPath), "DarwinDbC-%Y%m%d-%H%M%S.log", std::localtime(&now));
    const char *path = defaultPath;
    if (argc == 3 && std::strcmp(argv[1], "--output") == 0) {
        path = argv[2];
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--output capture.log]\n", argv[0]);
        return argc == 2 && std::strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }

    int output = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output < 0) {
        std::perror(path);
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);
    std::fprintf(stderr, "Capture: %s\nWaiting for DarwinDbC (ffff:0001); Ctrl-C stops.\n", path);

    uint64_t received = 0;
    IOReturn lastError = kIOReturnSuccess;
    int exitStatus = 0;
    while (!stopping) {
        io_service_t service = findDevice();
        if (!service) {
            usleep(200000);
            continue;
        }

        Connection connection;
        IOReturn result = connection.open(service);
        IOObjectRelease(service);
        if (result != kIOReturnSuccess) {
            if (result != lastError) {
                report("Open DarwinDbC", result);
                lastError = result;
            }
            connection.close();
            usleep(500000);
            continue;
        }
        lastError = kIOReturnSuccess;
        std::fprintf(stderr, "Connected. Reading kernel console.\n");
        while (!stopping) {
            // USB wMaxPacketSize is 16 bits; the interface chooses the requested size.
            unsigned char bytes[UINT16_MAX];
            UInt32 length = 0;
            result = connection.read(bytes, length);
            if (result == kIOReturnSuccess) {
                if (!writeAll(output, bytes, length) || !writeAll(STDOUT_FILENO, bytes, length)) {
                    exitStatus = 1;
                    stopping = 1;
                    break;
                }
                received += length;
            } else if (result == kIOUSBTransactionTimeout || result == kIOReturnTimeout) {
                continue;
            } else if (result == kIOUSBPipeStalled && connection.clearStall() == kIOReturnSuccess) {
                continue;
            } else {
                report("USB input ended", result);
                std::fprintf(stderr, "Waiting for reconnection.\n");
                break;
            }
        }
    }
    if (fsync(output) != 0) {
        std::perror("Flush capture");
        exitStatus = 1;
    }
    if (::close(output) != 0) {
        std::perror("Close capture");
        exitStatus = 1;
    }
    std::fprintf(
        stderr, "Saved %llu bytes to %s\n", static_cast<unsigned long long>(received), path);
    return exitStatus;
}
