// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <kdp/kdp_callout.h>
#include "DbCXNU.hpp"

// Captures console and debugger routes with in-process original functions.
// Lilu's real declarations are used by the kext build.
// https://github.com/acidanthera/Lilu/blob/e4748cc081bf060302c7d3c44a643ce1d11b7e1d/Lilu/Headers/kern_patcher.hpp
class KernelPatcher final {
public:
    struct RouteRequest {
        const char *symbol;
        uintptr_t to;
        std::function<void(uintptr_t)> setOriginal;

        template <typename T>
        RouteRequest(const char *name, T target, T &original)
            : symbol(name),
              to(reinterpret_cast<uintptr_t>(target)),
              setOriginal([&original](uintptr_t address) {
                  original = reinterpret_cast<T>(address);
              })
        {
        }

        template <typename T>
        RouteRequest(const char *name, T target)
            : symbol(name),
              to(reinterpret_cast<uintptr_t>(target))
        {
        }
    };

    static constexpr size_t KernelID = 0;
    unsigned routeLimit{2};
    bool errorCleared{};
    inline static bool panic{};
    inline static bool halted{true};
    inline static kdp_callout_fn_t debuggerCallback{};
    inline static void *debuggerArgument{};
    inline static void (*panicLogRoute)(char){};
    inline static void (*panicSpinRoute)(){};
    inline static void (*exceptionRoute)(unsigned, unsigned, unsigned, void *){};
    inline static unsigned exceptionCalls{};
    inline static unsigned exceptionArguments[3]{};
    inline static void *exceptionState{};
    inline static DbC::XNU::MsgBuffer *messageBuffer{};
    inline static bool messageBusy{};
    inline static bool messageHeld{};
    inline static bool providePanicBuffer{};
    inline static char *debugBase{};
    inline static char *debugEnd{};
    inline static uint32_t debugSize{};
    inline static uint8_t debugEntries[48]{};
    void (*write)(char *, int){};
    void (*writeUnbuffered)(char){};

    KernelPatcher(void (*buffered)(char *, int), void (*unbuffered)(char))
        : originalWrite(buffered),
          originalWriteUnbuffered(unbuffered)
    {
    }

    bool routeMultipleLong(size_t image, RouteRequest (&requests)[2])
    {
        if (image != KernelID || std::strcmp(requests[0].symbol, "_console_write") ||
            std::strcmp(requests[1].symbol, "_console_write_unbuffered")) {
            return false;
        }

        if (routeLimit >= 1) {
            requests[0].setOriginal(reinterpret_cast<uintptr_t>(originalWrite));
            write = reinterpret_cast<decltype(write)>(requests[0].to);
        }

        if (routeLimit >= 2) {
            requests[1].setOriginal(reinterpret_cast<uintptr_t>(originalWriteUnbuffered));
            writeUnbuffered = reinterpret_cast<decltype(writeUnbuffered)>(requests[1].to);
        }

        return routeLimit == 2;
    }

    bool routeMultipleLong(size_t image, RouteRequest (&requests)[3])
    {
        if (image != KernelID || std::strcmp(requests[0].symbol, "_consdebug_log") ||
            std::strcmp(requests[1].symbol, "_panic_spin_forever") ||
            std::strcmp(requests[2].symbol, "_kdp_raise_exception")) {
            return false;
        }
        panicLogRoute = reinterpret_cast<decltype(panicLogRoute)>(requests[0].to);
        panicSpinRoute = reinterpret_cast<decltype(panicSpinRoute)>(requests[1].to);
        requests[2].setOriginal(reinterpret_cast<uintptr_t>(raiseException));
        exceptionRoute = reinterpret_cast<decltype(exceptionRoute)>(requests[2].to);
        return true;
    }

    void clearError()
    {
        errorCleared = true;
    }

    uintptr_t solveSymbol(size_t image, const char *symbol)
    {
        if (image != KernelID) {
            return 0;
        }
        if (!std::strcmp(symbol, "_kdp_register_callout")) {
            return reinterpret_cast<uintptr_t>(registerCallout);
        }
        if (!std::strcmp(symbol, "_panic_active")) {
            return reinterpret_cast<uintptr_t>(panicActive);
        }
        if (!std::strcmp(symbol, "_mp_kdp_all_cpus_halted")) {
            return reinterpret_cast<uintptr_t>(allHalted);
        }
        if (!std::strcmp(symbol, "_debug_putc")) {
            return reinterpret_cast<uintptr_t>(logOnly);
        }
        if (!std::strcmp(symbol, "_consdebug_log")) {
            if (providePanicBuffer) {
                return reinterpret_cast<uintptr_t>(debugEntries + 32);
            }
            return reinterpret_cast<uintptr_t>(spin);
        }
        if (providePanicBuffer) {
            if (!std::strcmp(symbol, "_debug_buf_base")) {
                return reinterpret_cast<uintptr_t>(&debugBase);
            }
            if (!std::strcmp(symbol, "_debug_buf_ptr")) {
                return reinterpret_cast<uintptr_t>(&debugEnd);
            }
            if (!std::strcmp(symbol, "_debug_buf_size")) {
                return reinterpret_cast<uintptr_t>(&debugSize);
            }
            if (!std::strcmp(symbol, "_consdebug_putc")) {
                return reinterpret_cast<uintptr_t>(debugEntries);
            }
            if (!std::strcmp(symbol, "_consdebug_putc_unbuffered")) {
                return reinterpret_cast<uintptr_t>(debugEntries + 16);
            }
        }
        if (messageBuffer) {
            if (!std::strcmp(symbol, "_msgbufp")) {
                return reinterpret_cast<uintptr_t>(&messageBuffer);
            }
            if (!std::strcmp(symbol, "_bsd_log_lock")) {
                return reinterpret_cast<uintptr_t>(lockMessage);
            }
            if (!std::strcmp(symbol, "_bsd_log_unlock")) {
                return reinterpret_cast<uintptr_t>(unlockMessage);
            }
        }
        return 0;
    }

private:
    static void raiseException(unsigned exception, unsigned code, unsigned subcode, void *state)
    {
        ++exceptionCalls;
        exceptionArguments[0] = exception;
        exceptionArguments[1] = code;
        exceptionArguments[2] = subcode;
        exceptionState = state;
    }

    static void registerCallout(kdp_callout_fn_t callback, void *argument)
    {
        debuggerCallback = callback;
        debuggerArgument = argument;
    }

    static unsigned panicActive()
    {
        return panic;
    }

    static int allHalted()
    {
        return halted;
    }

    static void logOnly(char c)
    {
        if (providePanicBuffer && debugBase && debugEnd && debugEnd >= debugBase &&
            static_cast<size_t>(debugEnd - debugBase) < debugSize) {
            *debugEnd++ = c;
        }
    }

    static void spin()
    {
    }

    static bool lockMessage(bool)
    {
        if (messageBusy) {
            return false;
        }
        messageHeld = true;
        return true;
    }

    static void unlockMessage()
    {
        messageHeld = false;
    }

    void (*originalWrite)(char *, int);
    void (*originalWriteUnbuffered)(char);
};
