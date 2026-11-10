// SPDX-License-Identifier: BSD-2-Clause
#include "DbCPanic.hpp"

#include <kdp/kdp_callout.h>
#include <string.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

namespace DbC {
namespace {

// Covers XNU's 0x13ff80-byte extended x86 panic region and subsequent diagnostics.
constexpr size_t PanicCapacity = 2 * 1024 * 1024;
constexpr uint32_t StopMicroseconds = 50000;
constexpr uint32_t ConnectMicroseconds = 2000000;
constexpr uint32_t TransferMicroseconds = 20000;
constexpr uint32_t FlushIntervalMicroseconds = 1000;
constexpr uint32_t ReportMicroseconds = 100000;
// Charge the transport's active waiting time across fatal entries.
constexpr uint32_t TotalMicroseconds = 10000000;
constexpr uint32_t PollLimit = 1000000;
constexpr size_t MaximumSourceCapacity = PanicCapacity;

struct PanicState {
    Controller *controller{};
    Platform *platform{};
    bool initialized{};
    bool ready{};
    bool active{};
    bool draining{};
    bool waiting{};
    bool reportAttempted{};
    bool stopRecorded{};
    uint64_t deadline{};
    uint64_t waitStarted{};
    uint64_t waited{};
    uint64_t lastFlush{};
    uint32_t polls{};
    unsigned failures{};
    unsigned entries{};
    uintptr_t sourceBase{};
    size_t sourceCursor{};
    size_t sourceEnd{};
    bool mirrorPending{};
    char mirrorByte{};
    size_t captured{};
    size_t submitted{};
    size_t confirmed{};
    size_t pending{};
    size_t discarded{};
    Status lastStatus{Status::Success};
    const char *stopReason{"ready"};
    char bytes[PanicCapacity + 1024]{};
};

PanicState state;
unsigned (*panicActive)(){};
int (*allCPUsHalted)(){};
void (*debugPutc)(char){};
void (*originalRaiseException)(unsigned, unsigned, unsigned, void *){};
char **debugBase;
char **debugEnd;
uint32_t *debugSize;
uintptr_t bufferedCaller;
uintptr_t unbufferedCaller;
uintptr_t logCaller;

void endWait()
{
    if (state.waiting) {
        const auto elapsed = state.platform->timeMicroseconds() - state.waitStarted;
        const auto remaining = TotalMicroseconds - state.waited;
        state.waited += elapsed < remaining ? elapsed : remaining;
        state.waiting = false;
    }
}

bool beginWait(uint32_t duration)
{
    endWait();
    if (state.waited == TotalMicroseconds || !state.polls) {
        return false;
    }
    const auto remaining = TotalMicroseconds - state.waited;
    state.waitStarted = state.platform->timeMicroseconds();
    state.deadline =
        Wait::forDuration(*state.platform, duration < remaining ? duration : remaining).deadline;
    state.waiting = true;
    return true;
}

Wait budget(uint32_t duration)
{
    auto wait = Wait::forDuration(*state.platform, duration);
    if (wait.deadline > state.deadline) {
        wait.deadline = state.deadline;
    }
    wait.pollLimit = state.polls < PollLimit ? state.polls : PollLimit;
    return wait;
}

void append(const void *bytes, size_t length, size_t capacity = PanicCapacity)
{
    const size_t available = state.captured < capacity ? capacity - state.captured : 0;
    const size_t amount = length < available ? length : available;
    memcpy(state.bytes + state.captured, bytes, amount);
    state.captured += amount;
    state.discarded += length - amount;
}

bool captureBuffer()
{
    if (!PanicConsole::hasBuffer()) {
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(__atomic_load_n(debugBase, __ATOMIC_ACQUIRE));
    const auto end = reinterpret_cast<uintptr_t>(__atomic_load_n(debugEnd, __ATOMIC_ACQUIRE));
    const auto size = __atomic_load_n(debugSize, __ATOMIC_ACQUIRE);
    if (!base || !size || size > MaximumSourceCapacity || end < base || end - base > size) {
        return false;
    }
    const size_t used = end - base;
    if (state.sourceBase != base || used < state.sourceEnd) {
        if (state.sourceBase) {
            constexpr char marker[] = "\n[DarwinDbC] panic.buffer-reset\n";
            append(marker, sizeof(marker) - 1);
        }
        state.sourceBase = base;
        state.sourceCursor = 0;
        state.mirrorPending = false;
    }
    const auto *source = reinterpret_cast<const char *>(base);
    // consdebug_putc() writes its console character before updating debug_buf_ptr.
    // Consume the confirmed buffer copy of that character exactly once.
    if (state.mirrorPending && used > state.sourceCursor) {
        if (source[state.sourceCursor] == state.mirrorByte) {
            ++state.sourceCursor;
        }
        state.mirrorPending = false;
    }
    if (used > state.sourceCursor) {
        append(source + state.sourceCursor, used - state.sourceCursor);
        state.sourceCursor = used;
    }
    state.sourceEnd = used;
    return true;
}

void completePending(const TransferResult &result)
{
    if (state.pending && result.status == Status::Success && !result.pending) {
        state.submitted -= state.pending;
        state.submitted += result.transferred;
        state.confirmed = state.submitted;
        state.pending = 0;
    }
}

int formatState(char *marker, size_t capacity, const char *stage)
{
    return snprintf(marker,
        capacity,
        "\n[DarwinDbC] %s reason=%s last_status=%s captured=%llu confirmed=%llu pending=%llu "
        "remaining=%llu waited_us=%llu failures=%u\n",
        stage,
        state.stopReason,
        statusName(state.lastStatus),
        static_cast<unsigned long long>(state.captured),
        static_cast<unsigned long long>(state.confirmed),
        static_cast<unsigned long long>(state.pending),
        static_cast<unsigned long long>(state.captured - state.confirmed),
        static_cast<unsigned long long>(state.waited),
        state.failures);
}

void reportStop()
{
    if (state.reportAttempted) {
        return;
    }
    state.reportAttempted = true;
    char marker[384];
    const int length = formatState(marker, sizeof(marker), "panic.stop");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(marker)) {
        return;
    }
    if (!state.stopRecorded) {
        append(marker, static_cast<size_t>(length), sizeof(state.bytes));
        state.stopRecorded = true;
    }
    // Reserve one bounded status transfer after the payload waiting budget.
    auto wait = Wait::forDuration(*state.platform, ReportMicroseconds);
    wait.pollLimit = 10000;
    const auto completion = state.controller->flush(wait);
    completePending(completion);
    if (completion.status == Status::Success && !completion.pending) {
        const int updatedLength = formatState(marker, sizeof(marker), "panic.stop");
        state.controller->write({marker, static_cast<size_t>(updatedLength), wait});
    }
}

bool retryTimeout(Status status, uint64_t started)
{
    // A stationary clock ends this bounded drain; a later boundary can resume it.
    return status == Status::Timeout && state.platform->timeMicroseconds() > started;
}

void drain(uint32_t duration = TransferMicroseconds)
{
    if (state.draining || !__atomic_load_n(&state.initialized, __ATOMIC_ACQUIRE) ||
        !state.platform->isReady()) {
        return;
    }

    state.draining = true;
    auto &controller = *state.controller;
    const bool started = beginWait(duration);
    while (started && state.polls && state.platform->timeMicroseconds() < state.deadline) {
        --state.polls;
        const auto attemptStarted = state.platform->timeMicroseconds();
        Snapshot snapshot{};
        state.lastStatus = controller.copySnapshot(snapshot);
        if (state.lastStatus != Status::Success) {
            ++state.failures;
            break;
        }
        if (snapshot.state == State::Released) {
            state.lastStatus = Status::InvalidState;
            ++state.failures;
            break;
        }
        if (snapshot.state == State::Faulted) {
            state.lastStatus = controller.close(budget(StopMicroseconds));
            if (state.lastStatus != Status::Success) {
                ++state.failures;
                if (retryTimeout(state.lastStatus, attemptStarted)) {
                    continue;
                }
                break;
            }
            // Replay retained panic text after a failed transfer resets the rings.
            state.submitted = 0;
            state.confirmed = 0;
            state.pending = 0;
            continue;
        }
        if (snapshot.state != State::Running) {
            auto status = controller.open(budget(ConnectMicroseconds));
            state.lastStatus = status;
            if (status != Status::Success) {
                ++state.failures;
                if (retryTimeout(status, attemptStarted)) {
                    continue;
                }
                break;
            }
        }

        if (state.pending) {
            auto result = controller.flush(budget(TransferMicroseconds));
            state.lastStatus = result.status;
            completePending(result);
            state.failures = result.status == Status::Success ? 0 : state.failures + 1;
            if (result.status == Status::Success) {
                continue;
            }
            if (retryTimeout(result.status, attemptStarted)) {
                continue;
            }
            break;
        }
        if (state.submitted == state.captured) {
            state.stopReason = "drained";
            break;
        }

        size_t amount = state.captured - state.submitted;
        if (amount > Layout::kBufferLength) {
            amount = Layout::kBufferLength;
        }
        auto result =
            controller.write({state.bytes + state.submitted, amount, budget(TransferMicroseconds)});
        state.lastStatus = result.status;
        if (result.status == Status::Success && result.transferred) {
            state.submitted += result.transferred;
            state.confirmed = state.submitted;
            state.failures = 0;
        } else if (result.status == Status::Timeout && result.pending) {
            // Controller retains its DMA copy until completion or close().
            state.submitted += amount;
            state.pending = amount;
            ++state.failures;
            if (retryTimeout(result.status, attemptStarted)) {
                continue;
            }
            break;
        } else if (result.status == Status::Busy) {
            auto completion = controller.flush(budget(TransferMicroseconds));
            state.lastStatus = completion.status;
            if (completion.status == Status::Success) {
                state.failures = 0;
                continue;
            }
            ++state.failures;
            if (retryTimeout(completion.status, attemptStarted)) {
                continue;
            }
            break;
        } else {
            ++state.failures;
            if (retryTimeout(result.status, attemptStarted)) {
                continue;
            }
            break;
        }
    }
    endWait();
    state.lastFlush = state.platform->timeMicroseconds();
    if (state.waited == TotalMicroseconds) {
        state.stopReason = "wait-budget";
    } else if (!state.polls) {
        state.stopReason = "poll-budget";
    } else if (state.confirmed < state.captured) {
        state.stopReason = "pending";
    }
    if (state.waited == TotalMicroseconds || !state.polls) {
        reportStop();
    }
    state.draining = false;
}

void flushOnBoundary(const void *bytes, size_t length)
{
    if (state.captured - state.submitted >= Layout::kBufferLength || memchr(bytes, '\n', length) ||
        (__atomic_load_n(&state.initialized, __ATOMIC_ACQUIRE) &&
            state.platform->timeMicroseconds() - state.lastFlush >= FlushIntervalMicroseconds)) {
        drain();
    }
}

void debuggerEvent(void *, kdp_event_t event)
{
    if (!panicActive() || !allCPUsHalted()) {
        return;
    }
    if (event == KDP_EVENT_ENTER || event == KDP_EVENT_PANICLOG) {
        state.reportAttempted = false;
    }
    if (event == KDP_EVENT_ENTER) {
        // XNU invokes this after mp_kdp_enter(), with the other CPUs quiesced.
        // Ordinary debugger and stackshot entries retain their normal lifecycle.
        const bool nested = __atomic_exchange_n(&state.active, true, __ATOMIC_ACQ_REL);
        ++state.entries;
        endWait();
        state.draining = false;
        if (!nested) {
            constexpr char marker[] = "\n[DarwinDbC] panic.begin\n";
            append(marker, sizeof(marker) - 1);
        } else {
            state.mirrorPending = false;
            char marker[80];
            const int count = snprintf(
                marker, sizeof(marker), "\n[DarwinDbC] panic.nested entry=%u\n", state.entries);
            append(marker, static_cast<size_t>(count));
        }
        if (__atomic_load_n(&state.initialized, __ATOMIC_ACQUIRE)) {
            if (!nested) {
                state.polls = PollLimit;
            }
            state.failures = 0;
            state.draining = true;
            auto result = beginWait(StopMicroseconds)
                ? state.controller->enterPanic(budget(StopMicroseconds))
                : Status::Timeout;
            endWait();
            state.lastStatus = result;
            state.draining = false;
            if (result != Status::Success) {
                captureBuffer();
                return;
            }
            Snapshot snapshot{};
            if (state.controller->copySnapshot(snapshot) == Status::Success) {
                if (snapshot.state == State::Prepared) {
                    state.submitted = 0;
                    state.confirmed = 0;
                    state.pending = 0;
                }
                char marker[96];
                const int count = snprintf(marker,
                    sizeof(marker),
                    "\n[DarwinDbC] panic.transport takeovers=%llu\n",
                    static_cast<unsigned long long>(snapshot.panicTakeovers));
                append(marker, static_cast<size_t>(count));
            }
        }
    } else if (event == KDP_EVENT_PANICLOG) {
        state.failures = 0;
    }
    captureBuffer();
    if (event == KDP_EVENT_PANICLOG) {
        char marker[384];
        const int length = formatState(marker, sizeof(marker), "panic.buffer-checkpoint");
        append(marker, static_cast<size_t>(length));
    }
    drain(TotalMicroseconds);
    if (event == KDP_EVENT_ENTER) {
        char marker[384];
        const int length = formatState(marker, sizeof(marker), "panic.entry.return");
        append(marker, static_cast<size_t>(length));
        drain();
    }
}

void logOnly(char character)
{
    const auto previous =
        PanicConsole::hasBuffer() ? __atomic_load_n(debugEnd, __ATOMIC_ACQUIRE) : nullptr;
    debugPutc(character);
    if (PanicConsole::isActive()) {
        const bool captured = captureBuffer();
        // A full XNU buffer keeps its cursor fixed; continue streaming new characters.
        if (!captured || __atomic_load_n(debugEnd, __ATOMIC_ACQUIRE) == previous) {
            append(&character, sizeof(character));
        }
        flushOnBoundary(&character, sizeof(character));
    }
}

void spin()
{
    if (PanicConsole::isActive()) {
        captureBuffer();
        state.failures = 0;
        state.reportAttempted = false;
        drain(TotalMicroseconds);
        char marker[384];
        const int length = snprintf(marker,
            sizeof(marker),
            "\n[DarwinDbC] panic.end captured=%llu submitted=%llu confirmed=%llu "
            "remaining=%llu discarded=%llu waited_us=%llu reason=%s\n",
            static_cast<unsigned long long>(state.captured),
            static_cast<unsigned long long>(state.submitted),
            static_cast<unsigned long long>(state.confirmed),
            static_cast<unsigned long long>(state.captured - state.confirmed),
            static_cast<unsigned long long>(state.discarded),
            static_cast<unsigned long long>(state.waited),
            state.stopReason);
        if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
            append(marker, static_cast<size_t>(length), sizeof(state.bytes));
        }
        state.failures = 0;
        drain(TotalMicroseconds);
    }
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
    }
}

void raiseException(unsigned exception, unsigned code, unsigned subcode, void *savedState)
{
    // XNU enters interactive KDP after collecting panic diagnostics. Finish the
    // resident output here before KDP begins its Ethernet connection wait.
    if (PanicConsole::isActive() && panicActive() && allCPUsHalted()) {
        constexpr char marker[] = "\n[DarwinDbC] panic.debugger-skip\n";
        append(marker, sizeof(marker) - 1);
        spin();
    }
    originalRaiseException(exception, code, subcode, savedState);
}
}

bool PanicConsole::route(KernelPatcher &patcher)
{
    if (isReady()) {
        return true;
    }
    auto registerCallout = reinterpret_cast<decltype(&kdp_register_callout)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_kdp_register_callout"));
    patcher.clearError();
    panicActive = reinterpret_cast<decltype(panicActive)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_panic_active"));
    patcher.clearError();
    allCPUsHalted = reinterpret_cast<decltype(allCPUsHalted)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_mp_kdp_all_cpus_halted"));
    patcher.clearError();
    debugPutc = reinterpret_cast<decltype(debugPutc)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_debug_putc"));
    patcher.clearError();
    const auto log = patcher.solveSymbol(KernelPatcher::KernelID, "_consdebug_log");
    patcher.clearError();
    if (!registerCallout || !panicActive || !allCPUsHalted || !debugPutc || !log ||
        log == reinterpret_cast<uintptr_t>(debugPutc)) {
        return false;
    }
    // Buffer discovery extends the existing streaming route independently.
    debugBase = reinterpret_cast<decltype(debugBase)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_debug_buf_base"));
    patcher.clearError();
    debugEnd = reinterpret_cast<decltype(debugEnd)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_debug_buf_ptr"));
    patcher.clearError();
    debugSize = reinterpret_cast<decltype(debugSize)>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_debug_buf_size"));
    patcher.clearError();
    bufferedCaller = patcher.solveSymbol(KernelPatcher::KernelID, "_consdebug_putc");
    patcher.clearError();
    unbufferedCaller = patcher.solveSymbol(KernelPatcher::KernelID, "_consdebug_putc_unbuffered");
    patcher.clearError();
    logCaller = log;
    // consdebug_log() forwards to debug_putc(); call that entry directly to retain
    // the panic buffer update. The final spin keeps its original infinite pause loop.
    KernelPatcher::RouteRequest routes[]{
        {"_consdebug_log", logOnly},
        {"_panic_spin_forever", spin},
        {"_kdp_raise_exception", raiseException, originalRaiseException},
    };
    const bool success = patcher.routeMultipleLong(KernelPatcher::KernelID, routes);
    patcher.clearError();
    if (!success) {
        return false;
    }
    registerCallout(debuggerEvent, nullptr);
    __atomic_store_n(&state.ready, true, __ATOMIC_RELEASE);
    return true;
}

bool PanicConsole::isReady()
{
    return __atomic_load_n(&state.ready, __ATOMIC_ACQUIRE);
}

bool PanicConsole::isActive()
{
    return __atomic_load_n(&state.active, __ATOMIC_ACQUIRE);
}

bool PanicConsole::hasBuffer()
{
    return isReady() && debugBase && debugEnd && debugSize && bufferedCaller &&
        bufferedCaller < unbufferedCaller && unbufferedCaller < logCaller;
}

Status PanicConsole::initialize(Controller *controller, Platform *platform)
{
    if (!controller || !platform || state.initialized || isActive()) {
        return Status::InvalidState;
    }
    state.controller = controller;
    state.platform = platform;
    __atomic_store_n(&state.initialized, true, __ATOMIC_RELEASE);
    return Status::Success;
}

Status PanicConsole::release()
{
    if (isActive()) {
        return Status::Busy;
    }
    __atomic_store_n(&state.initialized, false, __ATOMIC_RELEASE);
    state.controller = nullptr;
    state.platform = nullptr;
    return Status::Success;
}

void PanicConsole::write(const void *bytes, size_t length, uintptr_t caller)
{
    const bool captured = captureBuffer();
    append(bytes, length);
    if (captured && length == 1 && caller >= bufferedCaller && caller < logCaller &&
        state.sourceCursor < __atomic_load_n(debugSize, __ATOMIC_ACQUIRE)) {
        state.mirrorByte = *static_cast<const char *>(bytes);
        state.mirrorPending = true;
    }
    flushOnBoundary(bytes, length);
}
}
