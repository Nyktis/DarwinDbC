// SPDX-License-Identifier: BSD-2-Clause
#include "DbCConsole.hpp"
#include "DbCPanic.hpp"
#include "DbCXNU.hpp"

#include <string.h>
#ifdef KERNEL
#include <Headers/kern_mach.hpp>
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

namespace DbC {
namespace {

constexpr size_t RecordHeader = sizeof(uint64_t);
constexpr size_t RecordAlignment = sizeof(uint64_t);
constexpr size_t HistoryPrefixSize = 128;
// Admit two maximum history records, including framing and alignment at a wrap.
constexpr size_t BufferCapacity =
    2 * (XNU::MAX_MSG_BSIZE + 2 * HistoryPrefixSize + RecordHeader + RecordAlignment);

struct ConsoleBuffer {
    // Monotonic byte positions include record headers, alignment and wrap padding.
    uint64_t head{};
    uint64_t tail{};
    // Input byte ordinals identify the window containing rejected output.
    uint64_t inputBytes{};
    uint64_t firstDroppedByte{};
    uint64_t lastDroppedByte{};
    uint64_t reportedDrops{};
    uint64_t queuedBytes{};
    // Only the single consumer updates record and DMA progress under writer.
    size_t consumed{};
    size_t pending{};
    uint8_t transfer[Layout::kBufferLength]{};
    // Commit tickets live separately so payload bytes retain their full value range.
    uint64_t committed[BufferCapacity / sizeof(uint64_t)]{};
    alignas(RecordAlignment) uint64_t words[BufferCapacity / sizeof(uint64_t)]{};
};

ConsoleBuffer buffer;
XNU::MsgBuffer **messageBuffer;
bool (*messageLock)(bool);
void (*messageUnlock)();
bool historyPending;
bool historyReader;
char history[XNU::MAX_MSG_BSIZE + 2 * HistoryPrefixSize];

size_t recordSize(size_t length)
{
    return (RecordHeader + length + RecordAlignment - 1) & ~(RecordAlignment - 1);
}

// Producers reserve disjoint records. A release-store commits each completed copy;
// the consumer stops at an interrupted producer and resumes on a later flush().
bool append(const void *bytes, size_t length)
{
    const size_t span = recordSize(length);
    uint64_t head = __atomic_load_n(&buffer.head, __ATOMIC_RELAXED);
    size_t padding;
    for (;;) {
        const size_t offset = head % BufferCapacity;
        padding = offset + span > BufferCapacity ? BufferCapacity - offset : 0;
        const uint64_t tail = __atomic_load_n(&buffer.tail, __ATOMIC_ACQUIRE);
        if (head < tail) {
            head = __atomic_load_n(&buffer.head, __ATOMIC_RELAXED);
            continue;
        }
        if (head - tail > BufferCapacity - span - padding) {
            return false;
        }
        if (__atomic_compare_exchange_n(&buffer.head,
                &head,
                head + padding + span,
                true,
                __ATOMIC_RELAXED,
                __ATOMIC_RELAXED)) {
            break;
        }
    }
    if (padding >= RecordHeader) {
        const size_t index = (head % BufferCapacity) / sizeof(uint64_t);
        buffer.words[index] = 0;
        __atomic_store_n(buffer.committed + index, head + 1, __ATOMIC_RELEASE);
    }
    head += padding;
    const size_t index = (head % BufferCapacity) / sizeof(uint64_t);
    auto *record = buffer.words + index;
    record[0] = length;
    memcpy(record + 1, bytes, length);
    __atomic_fetch_add(&buffer.queuedBytes, length, __ATOMIC_RELAXED);
    __atomic_store_n(buffer.committed + index, head + 1, __ATOMIC_RELEASE);
    return true;
}

size_t copyQueued()
{
    uint64_t tail = __atomic_load_n(&buffer.tail, __ATOMIC_RELAXED);
    size_t consumed = buffer.consumed;
    size_t copied = 0;
    while (copied < sizeof(buffer.transfer) &&
        tail != __atomic_load_n(&buffer.head, __ATOMIC_RELAXED)) {
        const size_t index = (tail % BufferCapacity) / sizeof(uint64_t);
        if (__atomic_load_n(buffer.committed + index, __ATOMIC_ACQUIRE) != tail + 1) {
            break;
        }
        const auto *record = buffer.words + index;
        const size_t length = record[0];
        if (!length) {
            tail += BufferCapacity - tail % BufferCapacity;
            continue;
        }
        const size_t available = sizeof(buffer.transfer) - copied;
        const size_t amount = length - consumed < available ? length - consumed : available;
        memcpy(buffer.transfer + copied,
            reinterpret_cast<const uint8_t *>(record + 1) + consumed,
            amount);
        copied += amount;
        if (amount != length - consumed) {
            break;
        }
        tail += recordSize(length);
        consumed = 0;
    }
    return copied;
}

// Release only bytes acknowledged by the host. A timed-out TD keeps every source
// record resident so a controller reset can replay its outstanding contents.
void consume(size_t amount)
{
    while (amount) {
        const uint64_t tail = __atomic_load_n(&buffer.tail, __ATOMIC_RELAXED);
        const auto *record = buffer.words + (tail % BufferCapacity) / sizeof(uint64_t);
        const size_t length = record[0];
        if (!length) {
            __atomic_store_n(
                &buffer.tail, tail + BufferCapacity - tail % BufferCapacity, __ATOMIC_RELEASE);
            continue;
        }
        const size_t available = length - buffer.consumed;
        const size_t used = amount < available ? amount : available;
        buffer.consumed += used;
        amount -= used;
        if (buffer.consumed == length) {
            buffer.consumed = 0;
            __atomic_store_n(&buffer.tail, tail + recordSize(length), __ATOMIC_RELEASE);
        }
    }
}

void recordLoss(uint64_t first, uint64_t last)
{
    auto old = __atomic_load_n(&buffer.firstDroppedByte, __ATOMIC_RELAXED);
    while ((!old || first < old) &&
        !__atomic_compare_exchange_n(
            &buffer.firstDroppedByte, &old, first, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    old = __atomic_load_n(&buffer.lastDroppedByte, __ATOMIC_RELAXED);
    while (last > old &&
        !__atomic_compare_exchange_n(
            &buffer.lastDroppedByte, &old, last, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}
}

Console::Configuration Console::configuration{};
Console::Counters Console::counters{};
uint8_t Console::routingState{static_cast<uint8_t>(RoutingState::Pending)};
bool Console::attached{};
uint8_t Console::writer{};
void (*Console::originalWrite)(char *, int){};
void (*Console::originalWriteUnbuffered)(char){};

bool Console::route(KernelPatcher &patcher)
{
    // XNU routes buffered console output and debugger characters through these entry points.
    // https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/console/serial_console.c
    KernelPatcher::RouteRequest routes[]{
        {"_console_write", write, originalWrite},
        {"_console_write_unbuffered", writeUnbuffered, originalWriteUnbuffered},
    };

    bool success = patcher.routeMultipleLong(KernelPatcher::KernelID, routes);
    patcher.clearError();
    auto state = success ? RoutingState::Ready : RoutingState::Failed;
    __atomic_store_n(&routingState, static_cast<uint8_t>(state), __ATOMIC_RELEASE);

    if (success && !messageBuffer) {
        messageBuffer = reinterpret_cast<decltype(messageBuffer)>(
            patcher.solveSymbol(KernelPatcher::KernelID, "_msgbufp"));
        patcher.clearError();
        messageLock = reinterpret_cast<decltype(messageLock)>(
            patcher.solveSymbol(KernelPatcher::KernelID, "_bsd_log_lock"));
        patcher.clearError();
        messageUnlock = reinterpret_cast<decltype(messageUnlock)>(
            patcher.solveSymbol(KernelPatcher::KernelID, "_bsd_log_unlock"));
        patcher.clearError();
        __atomic_store_n(
            &historyPending, messageBuffer && messageLock && messageUnlock, __ATOMIC_RELEASE);
        captureHistory();
    }

    return success;
}

bool Console::isReady()
{
    return __atomic_load_n(&routingState, __ATOMIC_ACQUIRE) ==
        static_cast<uint8_t>(RoutingState::Ready);
}

const char *Console::routingStatus()
{
    auto state = static_cast<RoutingState>(__atomic_load_n(&routingState, __ATOMIC_ACQUIRE));

    switch (state) {
    case RoutingState::Pending:
        return "pending";
    case RoutingState::Ready:
        return "ready";
    case RoutingState::Failed:
        return "failed";
    }

    return "unknown";
}

Status Console::initialize(const Configuration &value)
{
    if (configuration.controller || !value.controller || !value.platform ||
        !value.timeoutMicroseconds) {
        return Status::InvalidState;
    }

    configuration = value;

    auto status = PanicConsole::initialize(value.controller, value.platform);
    if (status != Status::Success) {
        configuration = {};
        return status;
    }

    return Status::Success;
}

Status Console::release()
{
    detach();

    if (__atomic_test_and_set(&writer, __ATOMIC_ACQUIRE)) {
        return Status::Busy;
    }

    auto status = PanicConsole::release();
    if (status != Status::Success) {
        __atomic_clear(&writer, __ATOMIC_RELEASE);
        return status;
    }
    configuration = {};
    // close() ends the old DMA ownership. Retain queued bytes for the next transport.
    buffer.pending = 0;
    __atomic_clear(&writer, __ATOMIC_RELEASE);

    return Status::Success;
}

bool Console::attach()
{
    return !__atomic_exchange_n(&attached, true, __ATOMIC_ACQ_REL);
}

void Console::detach()
{
    __atomic_store_n(&attached, false, __ATOMIC_RELEASE);
}

Console::Counters Console::copyCounters()
{
    return {
        __atomic_load_n(&counters.confirmedBytes, __ATOMIC_RELAXED),
        __atomic_load_n(&counters.unconfirmedBytes, __ATOMIC_RELAXED),
        __atomic_load_n(&counters.discardedBytes, __ATOMIC_RELAXED),
        __atomic_load_n(&buffer.queuedBytes, __ATOMIC_RELAXED),
        __atomic_load_n(&counters.historyBytes, __ATOMIC_RELAXED),
    };
}

void Console::write(char *bytes, int length)
{
    if (length > 0) {
        if (PanicConsole::isActive()) {
            PanicConsole::write(bytes,
                static_cast<size_t>(length),
                reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        } else {
            send(bytes, static_cast<size_t>(length));
        }
    }

    // XNU's panic callers retain the panic-buffer write after this console call.
    // Keep the fatal output independent of the video console and its locks.
    if (!PanicConsole::isActive()) {
        originalWrite(bytes, length);
    }
}

void Console::writeUnbuffered(char character)
{
    if (PanicConsole::isActive()) {
        PanicConsole::write(&character,
            sizeof(character),
            reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    } else {
        send(&character, sizeof(character));
    }
    if (!PanicConsole::isActive()) {
        originalWriteUnbuffered(character);
    }
}

void Console::send(const void *bytes, size_t length)
{
    if (PanicConsole::isActive()) {
        PanicConsole::write(bytes, length);
        return;
    }

    auto *cursor = static_cast<const uint8_t *>(bytes);
    captureHistory();
    uint64_t position = __atomic_fetch_add(&buffer.inputBytes, length, __ATOMIC_RELAXED) + 1;
    while (length) {
        const size_t amount = length < Layout::kBufferLength ? length : Layout::kBufferLength;
        if (!append(cursor, amount)) {
            recordLoss(position, position + length - 1);
            __atomic_fetch_add(&counters.discardedBytes, length, __ATOMIC_RELEASE);
            break;
        }
        cursor += amount;
        position += amount;
        length -= amount;
    }
    flush();
}

void Console::flush()
{
    if (PanicConsole::isActive() || !__atomic_load_n(&attached, __ATOMIC_ACQUIRE) ||
        __atomic_test_and_set(&writer, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (!__atomic_load_n(&attached, __ATOMIC_ACQUIRE)) {
        __atomic_clear(&writer, __ATOMIC_RELEASE);
        return;
    }
    captureHistory();

    const auto dropped = __atomic_load_n(&counters.discardedBytes, __ATOMIC_ACQUIRE);
    if (dropped != buffer.reportedDrops) {
        char notice[192];
        const int length = snprintf(notice,
            sizeof(notice),
            "\n[DarwinDbC] queue.overflow total_lost=%llu input_window=%llu..%llu\n",
            static_cast<unsigned long long>(dropped),
            static_cast<unsigned long long>(
                __atomic_load_n(&buffer.firstDroppedByte, __ATOMIC_RELAXED)),
            static_cast<unsigned long long>(
                __atomic_load_n(&buffer.lastDroppedByte, __ATOMIC_RELAXED)));
        if (length > 0 && static_cast<size_t>(length) < sizeof(notice) &&
            append(notice, static_cast<size_t>(length))) {
            buffer.reportedDrops = dropped;
        }
    }

    Wait wait = Wait::forDuration(*configuration.platform, configuration.timeoutMicroseconds);
    uint32_t attempts = wait.pollLimit;
    while (attempts && configuration.platform->timeMicroseconds() < wait.deadline) {
        --attempts;
        const size_t amount = buffer.pending ? buffer.pending : copyQueued();
        if (!amount) {
            break;
        }
        TransferResult result;
        if (buffer.pending) {
            result = configuration.controller->flush(wait);
        } else {
            result = configuration.controller->write({buffer.transfer, amount, wait});
            if (result.status == Status::Timeout && result.pending) {
                buffer.pending = amount;
                __atomic_fetch_add(&counters.unconfirmedBytes, buffer.pending, __ATOMIC_RELAXED);
            }
        }
        if (result.status != Status::Success) {
            if (result.status != Status::Timeout && result.status != Status::Busy) {
                buffer.pending = 0;
            }
            break;
        }
        buffer.pending = 0;
        if (result.transferred > amount) {
            break;
        }
        consume(result.transferred);
        __atomic_fetch_add(&counters.confirmedBytes, result.transferred, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&buffer.queuedBytes, result.transferred, __ATOMIC_RELAXED);
    }
    __atomic_clear(&writer, __ATOMIC_RELEASE);
}

void Console::captureHistory()
{
    if (!__atomic_load_n(&historyPending, __ATOMIC_ACQUIRE) ||
        __atomic_test_and_set(&historyReader, __ATOMIC_ACQUIRE)) {
        return;
    }
#ifdef KERNEL
    const bool interrupts = MachInfo::setInterrupts(false);
#endif
    if (!messageLock(false)) {
#ifdef KERNEL
        MachInfo::setInterrupts(interrupts);
#endif
        __atomic_clear(&historyReader, __ATOMIC_RELEASE);
        return;
    }
    size_t size = 0;
    int position = 0;
    const auto *source = *messageBuffer;
    if (source && source->msg_magic == XNU::MSG_MAGIC && source->msg_size > 0 &&
        static_cast<size_t>(source->msg_size) <= XNU::MAX_MSG_BSIZE && source->msg_bufc &&
        source->msg_bufx >= 0 && source->msg_bufx < source->msg_size) {
        size = static_cast<size_t>(source->msg_size);
        position = source->msg_bufx;
        // Snapshot the retained ring while preserving XNU's reader position.
        memcpy(history + HistoryPrefixSize, source->msg_bufc + position, size - position);
        memcpy(history + HistoryPrefixSize + size - position, source->msg_bufc, position);
    }
    messageUnlock();
#ifdef KERNEL
    MachInfo::setInterrupts(interrupts);
#endif
    if (!size) {
        __atomic_clear(&historyReader, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&historyPending, false, __ATOMIC_RELEASE);
    size_t length = 0;
    for (size_t i = 0; i < size; ++i) {
        if (history[HistoryPrefixSize + i]) {
            history[HistoryPrefixSize + length++] = history[HistoryPrefixSize + i];
        }
    }
    char marker[HistoryPrefixSize];
    const int count = snprintf(marker,
        sizeof(marker),
        "\n[DarwinDbC] xnu.msgbuf.begin retained=%lu capacity=%lu write=%d\n",
        static_cast<unsigned long>(length),
        static_cast<unsigned long>(size),
        position);
    constexpr char end[] = "\n[DarwinDbC] xnu.msgbuf.end\n";
    const size_t start = HistoryPrefixSize - static_cast<size_t>(count);
    const size_t amount = count + length + sizeof(end) - 1;
    memcpy(history + start, marker, count);
    memcpy(history + HistoryPrefixSize + length, end, sizeof(end) - 1);
    const uint64_t positionInStream =
        __atomic_fetch_add(&buffer.inputBytes, amount, __ATOMIC_RELAXED) + 1;
    // One record keeps the historical frame intact among concurrent live writers.
    if (!append(history + start, amount)) {
        recordLoss(positionInStream, positionInStream + amount - 1);
        __atomic_fetch_add(&counters.discardedBytes, amount, __ATOMIC_RELEASE);
    }
    flush();
    __atomic_store_n(&counters.historyBytes, length, __ATOMIC_RELAXED);
    __atomic_clear(&historyReader, __ATOMIC_RELEASE);
}

#if defined(KERNEL) && DEBUG
Status Console::testPanic()
{
    if (__atomic_test_and_set(&writer, __ATOMIC_ACQUIRE)) {
        return Status::Busy;
    }
    Status status = Status::InvalidState;
    if (__atomic_load_n(&attached, __ATOMIC_ACQUIRE) && configuration.controller &&
        PanicConsole::isReady() && !PanicConsole::isActive() &&
        !__atomic_load_n(&buffer.queuedBytes, __ATOMIC_RELAXED)) {
        status = configuration.controller->testPanic();
    }
    __atomic_clear(&writer, __ATOMIC_RELEASE);
    return status;
}
#endif
}
