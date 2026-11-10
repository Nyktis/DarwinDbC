// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace DbC {
namespace XNU {

// XNU bsd/sys/msgbuf.h: retained BSD message ring and its independent reader.
constexpr int32_t MSG_MAGIC = 0x063061;
constexpr size_t MAX_MSG_BSIZE = 1024 * 1024;

struct MsgBuffer {
    int32_t msg_magic;
    int32_t msg_size;
    int32_t msg_bufx;
    int32_t msg_bufr;
    char *msg_bufc;
};

static_assert(sizeof(MsgBuffer) == 24, "XNU msgbuf size");
static_assert(offsetof(MsgBuffer, msg_bufx) == 8, "XNU msgbuf write position");
static_assert(offsetof(MsgBuffer, msg_bufc) == 16, "XNU msgbuf storage");
}
}
