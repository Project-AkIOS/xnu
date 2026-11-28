/*
 * Copyright (c) 2026 AkIOS contributors. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/* The kernel console in the phone's persistent RAM (osfmk/arm64/akios_pstore.c). */
#ifndef _ARM64_AKIOS_PSTORE_H_
#define _ARM64_AKIOS_PSTORE_H_

#include <pexpert/arm64/boot.h>

void akios_pstore_early(boot_args *args);       /* top of arm_init(): through start.s's identity map */
void akios_pstore_suspend(void);                /* arm_vm_init(), before it leaves start.s's tables */
void akios_pstore_init(void);                   /* after arm_vm_init(): mapped for good */
void akios_pstore_putc(char c);                 /* each console character (loader_log_put_locked()) */
size_t akios_pstore_read(uint32_t at, char *out, size_t max);  /* the zone oldest first (usbmux port 11) */

#endif /* _ARM64_AKIOS_PSTORE_H_ */
