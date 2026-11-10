// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "DbCController.hpp"

#include <Headers/kern_patcher.hpp>

namespace DbC {

class Console final {
public:
    struct Configuration {
        Controller *controller;
        Platform *platform;
        uint32_t timeoutMicroseconds;
    };

    struct Counters {
        uint64_t confirmedBytes;
        uint64_t unconfirmedBytes;
        uint64_t discardedBytes;
        uint64_t queuedBytes;
        uint64_t historyBytes;
    };

    static bool route(KernelPatcher &patcher);
    static bool isReady();
    static const char *routingStatus();

    static Status initialize(const Configuration &configuration);
    static Status release();

    // Returns true when this call changes the console from detached to attached.
    static bool attach();
    static void detach();
    static void flush();
#if defined(KERNEL) && DEBUG
    static Status testPanic();
#endif
    static Counters copyCounters();

private:
    enum class RoutingState : uint8_t {
        Pending,
        Ready,
        Failed
    };

    static Configuration configuration;
    static Counters counters;
    static uint8_t routingState;
    static bool attached;
    static uint8_t writer;
    static void (*originalWrite)(char *bytes, int length);
    static void (*originalWriteUnbuffered)(char character);

    static void write(char *bytes, int length);
    static void writeUnbuffered(char character);
    static void send(const void *bytes, size_t length);
    static void captureHistory();
};
}
