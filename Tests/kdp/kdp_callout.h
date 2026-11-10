// SPDX-License-Identifier: BSD-2-Clause
#pragma once

// Callback ABI from XNU osfmk/kdp/kdp_callout.h.
typedef enum {
    KDP_EVENT_ENTER,
    KDP_EVENT_EXIT,
    KDP_EVENT_PANICLOG
} kdp_event_t;
typedef void (*kdp_callout_fn_t)(void *, kdp_event_t);
extern "C" void kdp_register_callout(kdp_callout_fn_t, void *);
