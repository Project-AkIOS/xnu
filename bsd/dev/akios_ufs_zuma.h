/*
 * zuma's UFS host controller driven by this kernel (akios_ufs_zuma.c), as hakios.ufs.c uses it when no loader serves
 * the storage (the AkIOS chain: firmware -> UniXNU -> DarwinBoot -> XNU).
 */
#ifndef _AKIOS_UFS_ZUMA_H_
#define _AKIOS_UFS_ZUMA_H_

#include <stdbool.h>
#include <stdint.h>

#define AKIOS_ZUFS_MAX_LUNS     8
#define AKIOS_ZUFS_CMD_BYTES    (1u << 20)      /* the most one command moves; longer requests are split */

bool akios_zufs_probe(void);
uint32_t akios_zufs_lun_mask(void);                     /* the logical units that answered */
uint64_t akios_zufs_lun_blocks(uint8_t lun);
uint32_t akios_zufs_lun_bsize(uint8_t lun);
/* blocks of the unit's own size; buf: kernel memory, 4-byte aligned (64-byte for reads into memory others share) */
int akios_zufs_read(uint8_t lun, uint64_t lba, uint64_t blocks, void *buf);
int akios_zufs_write(uint8_t lun, uint64_t lba, uint64_t blocks, const void *buf, bool fua);
int akios_zufs_sync(uint8_t lun);
/* the ranges writes may reach (inclusive, in the unit's blocks); nothing else is ever written */
bool akios_zufs_allow_write(uint8_t lun, uint64_t first, uint64_t last);
void akios_zufs_report(const char *when);

#endif
