// SPDX-License-Identifier: BSD-2-Clause
#include <mach/mach_types.h>

extern kern_return_t DarwinDbC_kern_start(kmod_info_t *, void *);
extern kern_return_t DarwinDbC_kern_stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL(
    org.darwindbc.driver.DarwinDbC, "0.1.11", DarwinDbC_kern_start, DarwinDbC_kern_stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
