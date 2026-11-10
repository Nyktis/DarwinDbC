// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <stddef.h>
#include <stdint.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error DarwinDbC requires a little-endian target
#endif

namespace DbC {

// xHCI 1.2c §§7.6.8–7.6.9, 6.2.3, 6.4.1.1, 6.4.2.1, 6.4.4.1.
// https://cdrdv2.intel.com/v1/dl/getContent/868295
namespace Register {
constexpr uint32_t kDCID = 0x00;
constexpr uint32_t kDCDB = 0x04;
constexpr uint32_t kDCERSTSZ = 0x08;
constexpr uint32_t kDCERSTBA = 0x10;
constexpr uint32_t kDCERDP = 0x18;
constexpr uint32_t kDCCTRL = 0x20;
constexpr uint32_t kDCST = 0x24;
constexpr uint32_t kDCPORTSC = 0x28;
constexpr uint32_t kDCCP = 0x30;
constexpr uint32_t kDCDDI1 = 0x38;
constexpr uint32_t kDCDDI2 = 0x3C;
constexpr uint32_t kSize = 0x40;
}

template <unsigned Shift, unsigned Width, typename T = uint32_t> struct Field {
    static_assert(Width && Width <= sizeof(T) * 8 - Shift, "field range");
    static constexpr T kValueMask = static_cast<T>(~T(0)) >> (sizeof(T) * 8 - Width);
    static constexpr T kMask = kValueMask << Shift;

    static constexpr T encode(T value)
    {
        return (value & kValueMask) << Shift;
    }

    static constexpr T decode(T value)
    {
        return (value >> Shift) & kValueMask;
    }
};

namespace DCID {
using CapabilityID = Field<0, 8>;
using NextCapability = Field<8, 8>;
using ERSTMax = Field<16, 5>;
constexpr uint32_t kReservedPreserve = 0xFFE00000;
constexpr uint32_t kDebugCapabilityID = 10;
}

namespace DCDB {
using Target = Field<8, 8>;
constexpr uint32_t kReservedPreserve = 0xFFFF00FF;
constexpr uint32_t kOut = 0;
constexpr uint32_t kIn = 1;
}

namespace DCERSTSZ {
using Size = Field<0, 16>;
constexpr uint32_t kReservedPreserve = 0xFFFF0000;
}

namespace DCERSTBA {
using Address = Field<4, 60, uint64_t>;
constexpr uint64_t kReservedPreserve = 0xF;
}

namespace DCERDP {
using SegmentIndex = Field<0, 3, uint64_t>;
using Address = Field<4, 60, uint64_t>;
constexpr uint64_t kReservedPreserve = 8;
}

namespace DCCTRL {
constexpr uint32_t kRun = 1U << 0;
constexpr uint32_t kLinkStatusEventEnable = 1U << 1;
constexpr uint32_t kHaltOut = 1U << 2;
constexpr uint32_t kHaltIn = 1U << 3;
constexpr uint32_t kRunChange = 1U << 4;
constexpr uint32_t kReservedPreserve = 0xFFE0;
using MaxBurst = Field<16, 8>;
using DeviceAddress = Field<24, 7>;
constexpr uint32_t kEnable = 1U << 31;
}

namespace DCST {
constexpr uint32_t kEventRingNotEmpty = 1U << 0;
constexpr uint32_t kSystemBusReset = 1U << 1;
constexpr uint32_t kReservedPreserve = 0x00FFFFFC;
using PortNumber = Field<24, 8>;
}

namespace DCPORTSC {
constexpr uint32_t kConnected = 1U << 0;
constexpr uint32_t kEnabled = 1U << 1;
constexpr uint32_t kReset = 1U << 4;
using LinkState = Field<5, 4>;
using Speed = Field<10, 4>;
constexpr uint32_t kConnectChange = 1U << 17;
constexpr uint32_t kResetChange = 1U << 21;
constexpr uint32_t kLinkChange = 1U << 22;
constexpr uint32_t kConfigurationErrorChange = 1U << 23;
constexpr uint32_t kChanges =
    kConnectChange | kResetChange | kLinkChange | kConfigurationErrorChange;

// CSC occupies bit 17; preserve it in the writable change set.
constexpr uint32_t kReservedZero = 0xFF1DC20C;

static_assert((kReservedZero | kConnected | kEnabled | kReset | LinkState::kMask | Speed::kMask |
                  kChanges) == UINT32_MAX,
    "port field coverage");

enum class LinkStateValue : uint8_t {
    U0 = 0,
    U1 = 1,
    U2 = 2,
    U3 = 3,
    Disabled = 4,
    RxDetect = 5,
    Inactive = 6,
    Polling = 7,
    Recovery = 8,
    HotReset = 9
};
}

namespace DCCP {
using Address = Field<4, 60, uint64_t>;
constexpr uint64_t kReservedPreserve = 0xF;
}

namespace DCDDI1 {
using Protocol = Field<0, 8>;
using VendorID = Field<16, 16>;
constexpr uint32_t kReservedZero = 0xFF00;

enum class ProtocolValue : uint8_t {
    VendorDefined = 0,
    GNU = 1
};
}

namespace DCDDI2 {
using ProductID = Field<0, 16>;
using Revision = Field<16, 16>;
}

struct TRB {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

namespace TRBField {
constexpr uint32_t kCycle = 1U << 0;
using Type = Field<10, 6>;

namespace Normal {
using Length = Field<0, 17>;
using TDSize = Field<17, 5>;
using Interrupter = Field<22, 10>;
constexpr uint32_t kEvaluateNext = 1U << 1;
constexpr uint32_t kInterruptOnShortPacket = 1U << 2;
constexpr uint32_t kNoSnoop = 1U << 3;
constexpr uint32_t kChain = 1U << 4;
constexpr uint32_t kInterruptOnCompletion = 1U << 5;
constexpr uint32_t kImmediateData = 1U << 6;
constexpr uint32_t kBlockEventInterrupt = 1U << 9;
constexpr uint32_t kReservedZero = 0xFFFF0180;
}

namespace Link {
using Address = Field<4, 60, uint64_t>;
using Interrupter = Field<22, 10>;
constexpr uint32_t kToggleCycle = 1U << 1;
constexpr uint32_t kChain = 1U << 4;
constexpr uint32_t kInterruptOnCompletion = 1U << 5;
constexpr uint32_t kReservedStatus = 0x003FFFFF;
constexpr uint32_t kReservedControl = 0xFFFF03CC;
}

namespace TransferEvent {
using Address = Field<4, 60, uint64_t>;
using Residual = Field<0, 24>;
using Completion = Field<24, 8>;
constexpr uint32_t kEventData = 1U << 2;
using EndpointID = Field<16, 5>;
using SlotID = Field<24, 8>;
constexpr uint32_t kReservedControl = 0x00E003FA;
}

namespace PortStatusChangeEvent {
using PortID = Field<24, 8, uint64_t>;
using Completion = Field<24, 8>;
constexpr uint64_t kReservedParameter = 0xFFFFFFFF00FFFFFFULL;
constexpr uint32_t kReservedStatus = 0x00FFFFFF;
constexpr uint32_t kReservedControl = 0xFFFF03FE;
}

enum class TypeValue : uint8_t {
    Normal = 1,
    Link = 6,
    TransferEvent = 32,
    PortStatusChangeEvent = 34
};

// Completion values emitted by DbC, xHCI 1.2c §7.6.4.2 and Table 6-90.
enum class CompletionValue : uint8_t {
    Invalid = 0,
    Success = 1,
    DataBufferError = 2,
    Babble = 3,
    USBTransactionError = 4,
    TRBError = 5,
    Stall = 6,
    ShortPacket = 13,
    EventRingFull = 21,
    Undefined = 33,
    VendorErrorFirst = 192,
    VendorErrorLast = 223,
    VendorInfoFirst = 224,
    VendorInfoLast = 255
};
}

struct alignas(64) InfoContext {
    uint64_t language;
    uint64_t manufacturer;
    uint64_t product;
    uint64_t serialNumber;
    uint32_t stringLengths;
    uint32_t reserved[7];
};

namespace InfoField {
using StringAddress = Field<1, 63, uint64_t>;
using LanguageLength = Field<0, 8>;
using ManufacturerLength = Field<8, 8>;
using ProductLength = Field<16, 8>;
using SerialNumberLength = Field<24, 8>;
}

struct alignas(64) EndpointContext {
    uint32_t information1;
    uint32_t information2;
    uint64_t dequeue;
    uint32_t transferInformation;
    uint32_t opaque[11];
};

namespace EndpointField {
using State = Field<0, 3>;
constexpr uint32_t kReservedInformation1 = 0xFFFFFFF8;
using Type = Field<3, 3>;
using MaxBurst = Field<8, 8>;
using MaxPacketSize = Field<16, 16>;
constexpr uint32_t kReservedInformation2 = 0xC7;
constexpr uint64_t kDequeueCycle = 1;
using DequeueAddress = Field<4, 60, uint64_t>;
constexpr uint64_t kReservedDequeue = 0xE;
using AverageTRBLength = Field<0, 16>;
constexpr uint32_t kReservedTransferInformation = 0xFFFF0000;
constexpr uint32_t kBulkOut = 2;
constexpr uint32_t kBulkIn = 6;
}

struct alignas(64) Context {
    InfoContext information; // 0x00
    EndpointContext out;     // 0x40
    EndpointContext in;      // 0x80
};

struct EventRingSegment {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
};

namespace SegmentField {
using Address = Field<6, 58, uint64_t>;
using Size = Field<0, 16>;
constexpr uint32_t kReservedSize = 0xFFFF0000;
}

static_assert(sizeof(TRB) == 16 && offsetof(TRB, control) == 12, "TRB layout");
static_assert(
    sizeof(InfoContext) == 64 && offsetof(InfoContext, stringLengths) == 32, "info layout");
static_assert(sizeof(EndpointContext) == 64 && offsetof(EndpointContext, transferInformation) == 16,
    "endpoint layout");
static_assert(sizeof(Context) == 192 && offsetof(Context, in) == 128, "context layout");
static_assert(sizeof(EventRingSegment) == 16, "segment layout");
}
