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

/*
 * The kernel console in the phone's persistent RAM (pstore).
 *
 * A phone has no serial port, and once the boot loader has left nothing reads the screen. The Pixel 8a keeps a
 * region of RAM across a warm restart for Linux's ramoops; its "pmsg" zone is what Android shows after the next start
 * as /sys/fs/pstore/pmsg-ramoops-0. Every console character (loader_log_putc(), osfmk/arm64/loader_return.c: printf,
 * kprintf and the panic text all pass there) is also written into that zone, so the log of a kernel that hung, or
 * whose watchdog restarted the phone, can be read from Android afterwards.
 *
 * The zone is a Linux persistent RAM zone: a header { u32 signature "DBGC", u32 start (where the next byte goes, an
 * offset into the data), u32 size (bytes held) } and the data after it as a ring. The firmware writes its own log
 * the same way into the same zone.
 *
 * When: the kernel's first page tables (start.s) map only the kernel's own memory, so the zone is mapped twice. At
 * the top of arm_init() it gets a block of its own in the identity-mapped half of those tables (TTBR0, which
 * arm_vm_init() keeps until it switches to the kernel's final tables; it calls akios_pstore_suspend() just before).
 * After arm_vm_init() it is mapped for good with ml_io_map(). Each time it is mapped, what the console log holds and
 * the zone does not yet is copied first, so nothing written in between is lost.
 *
 * boot-arg pstore: 0 = off; 1 (the default) = append to what is there (the firmware's and the boot loader's log)
 * after a separator line; 2 = start the zone empty.
 */
#include <mach/mach_types.h>
#include <kern/startup.h>
#include <kern/misc_protos.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <machine/atomic.h>
#include <arm64/akios_pstore.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_BCM2837)

#define AKIOS_PSTORE_BASE       0xFD5FF000ULL           /* ramoops_mem@fd3ff000: console 2 MiB, then pmsg 2 MiB */
#define AKIOS_PSTORE_SIZE       0x200000ULL
#define AKIOS_PSTORE_SIGNATURE  0x43474244u             /* "DBGC" */
#define AKIOS_PSTORE_HEADER     12u
#define AKIOS_PSTORE_DATA       ((uint32_t)(AKIOS_PSTORE_SIZE - AKIOS_PSTORE_HEADER))

/* (osfmk/arm64/loader_return.c) the console log from character *cursor on; moves *cursor */
extern size_t loader_log_copy(uint64_t *cursor, char *out, size_t max);

static volatile uint8_t *akios_pstore_zone;     /* NULL: not mapped (now) */
static bool akios_pstore_live;                  /* the zone takes each character as it comes */
static bool akios_pstore_set_up;                /* the header has been read (or reset) and the separator written */
static bool akios_pstore_early_mapped;          /* akios_pstore_early() found the identity map's slot free */
static int akios_pstore_mode = 1;
static uint32_t akios_pstore_start, akios_pstore_held;  /* the header's start and size, kept here too */
static uint64_t akios_pstore_cursor;            /* characters of the console log the zone has */

static void
akios_pstore_raw(char c)
{
	volatile uint32_t *header = (volatile uint32_t *)akios_pstore_zone;

	akios_pstore_zone[AKIOS_PSTORE_HEADER + akios_pstore_start] = (uint8_t)c;
	if (++akios_pstore_start == AKIOS_PSTORE_DATA) {
		akios_pstore_start = 0;
	}
	if (akios_pstore_held < AKIOS_PSTORE_DATA) {
		akios_pstore_held++;
	}
	header[1] = akios_pstore_start;
	header[2] = akios_pstore_held;
}

static void
akios_pstore_raw_str(const char *s)
{
	for (; *s != 0; s++) {
		akios_pstore_raw(*s);
	}
}

/* (Called by loader_log_put_locked(), under the console log's lock, for each character.) */
void
akios_pstore_putc(char c)
{
	if (!akios_pstore_live) {
		return;
	}
	akios_pstore_raw(c);
	akios_pstore_cursor++;
}

/* The zone is mapped at zone: take it over the first time, then copy what it does not have yet, then go live. */
static void
akios_pstore_attach(volatile uint8_t *zone)
{
	volatile uint32_t *header = (volatile uint32_t *)zone;
	char chunk[128];
	size_t n;

	akios_pstore_zone = zone;
	if (!akios_pstore_set_up) {
		akios_pstore_set_up = true;
		if (akios_pstore_mode == 2 || header[0] != AKIOS_PSTORE_SIGNATURE || header[1] >= AKIOS_PSTORE_DATA ||
		    header[2] > AKIOS_PSTORE_DATA) {
			header[1] = 0;
			header[2] = 0;
			header[0] = AKIOS_PSTORE_SIGNATURE;
		}
		akios_pstore_start = header[1];
		akios_pstore_held = header[2];
		akios_pstore_raw_str(akios_pstore_mode == 2 ? "=== AkIOS kernel console (pstore=2: the zone was reset) ===\n" :
		    "\n=== AkIOS kernel console ===\n");
	}
	while ((n = loader_log_copy(&akios_pstore_cursor, chunk, sizeof(chunk))) > 0) {
		for (size_t i = 0; i < n; i++) {
			akios_pstore_raw(chunk[i]);
		}
	}
	akios_pstore_live = true;
}

/* pstore=N on the command line, before the platform expert parses it (the first argument of that name wins). */
static int
akios_pstore_arg(const char *line)
{
	static const char name[] = "pstore=";

	for (size_t i = 0; i < BOOT_LINE_LENGTH && line[i] != 0; i++) {
		size_t k = 0;

		if (i != 0 && line[i - 1] != ' ') {
			continue;
		}
		while (k < sizeof(name) - 1 && i + k < BOOT_LINE_LENGTH && line[i + k] == name[k]) {
			k++;
		}
		if (k == sizeof(name) - 1 && i + k < BOOT_LINE_LENGTH && line[i + k] >= '0' && line[i + k] <= '9') {
			return line[i + k] - '0';
		}
	}
	return 1;
}

/*
 * The top of arm_init(): the zone through a block of its own in the identity map of start.s's tables (TTBR0), if
 * the slot that would cover it is free. The tables lie in the kernel image, where the boot arguments say.
 */
void
akios_pstore_early(boot_args *args)
{
	extern void loader_log_line(const char *line);
	const unsigned int index_bits = ARM_PGSHIFT - 3;        /* a table's entries: 2^index_bits */
	uint64_t tcr, bits, table_pa, entry, *l2;
	unsigned int shift, l2_index;

	akios_pstore_mode = akios_pstore_arg(args->CommandLine);
	if (akios_pstore_mode == 0) {
		return;
	}
	tcr = __builtin_arm_rsr64("TCR_EL1");
	bits = 64 - ((tcr >> TCR_T0SZ_SHIFT) & TCR_TSZ_MASK);
	if ((tcr & (1ULL << 7)) != 0 || bits <= ARM_TT_L2_SHIFT + index_bits || bits > 48 ||
	    (AKIOS_PSTORE_BASE >> bits) != 0) {
		loader_log_line("akios_pstore: no early mapping (TCR_EL1)\n");
		return;
	}
	/* down the identity map from TTBR0's table to the level 2 table that covers the zone */
	table_pa = __builtin_arm_rsr64("TTBR0_EL1") & ARM_TTE_TABLE_MASK;
	for (shift = ARM_TT_L2_SHIFT + index_bits; shift + index_bits < bits; shift += index_bits) {
	}
	for (; shift > ARM_TT_L2_SHIFT; shift -= index_bits) {
		if (table_pa < args->physBase || table_pa >= args->topOfKernelData) {
			loader_log_line("akios_pstore: no early mapping (a table outside the kernel)\n");
			return;
		}
		entry = ((uint64_t *)(table_pa - args->physBase + args->virtBase))[(AKIOS_PSTORE_BASE >> shift) &
		    ((1ULL << index_bits) - 1)];
		if ((entry & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) != (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)) {
			loader_log_line("akios_pstore: no early mapping (no table on the way)\n");
			return;
		}
		table_pa = entry & ARM_TTE_TABLE_MASK;
	}
	if (table_pa < args->physBase || table_pa >= args->topOfKernelData) {
		loader_log_line("akios_pstore: no early mapping (the level 2 table outside the kernel)\n");
		return;
	}
	l2 = (uint64_t *)(table_pa - args->physBase + args->virtBase);
	l2_index = (unsigned int)((AKIOS_PSTORE_BASE & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT);
	if (l2[l2_index] != 0) {
		loader_log_line("akios_pstore: no early mapping (its slot is taken)\n");
		return;
	}
	l2[l2_index] = (AKIOS_PSTORE_BASE & ~ARM_TT_L2_OFFMASK) | ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_AF |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE) | ARM_TTE_BLOCK_AP(AP_RWNA) | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX;
	__asm__ volatile ("dc civac, %0" : : "r" (&l2[l2_index]) : "memory");
	__builtin_arm_dsb(DSB_ISH);
	__builtin_arm_isb(ISB_SY);
	akios_pstore_early_mapped = true;
	akios_pstore_attach((volatile uint8_t *)AKIOS_PSTORE_BASE);
}

/* arm_vm_init(), just before it leaves start.s's tables: the identity map goes. */
void
akios_pstore_suspend(void)
{
	akios_pstore_live = false;
	akios_pstore_zone = NULL;
}

/* After arm_vm_init(): the zone for good, uncached. */
void
akios_pstore_init(void)
{
	if (akios_pstore_mode == 0) {
		return;
	}
	akios_pstore_attach((volatile uint8_t *)ml_io_map(AKIOS_PSTORE_BASE, AKIOS_PSTORE_SIZE));
	printf("akios_pstore: the console goes into pstore (pmsg zone at 0x%llx, %u bytes held; %s)\n", AKIOS_PSTORE_BASE,
	    akios_pstore_held, akios_pstore_early_mapped ? "from the top of arm_init" : "from arm_vm_init on");
}

#else /* !ARM64_BOARD_CONFIG_BCM2837 */

void
akios_pstore_putc(__unused char c)
{
}

void
akios_pstore_early(__unused boot_args *args)
{
}

void
akios_pstore_suspend(void)
{
}

void
akios_pstore_init(void)
{
}

#endif /* ARM64_BOARD_CONFIG_BCM2837 */
