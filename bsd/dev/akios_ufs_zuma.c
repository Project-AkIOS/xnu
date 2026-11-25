/*
 * zuma's UFS host controller (ufs@13200000), driven by this kernel: the storage of the Pixel 8a when no loader serves
 * it (the AkIOS chain: our firmware -> UniXNU -> DarwinBoot -> XNU). hakios.ufs.c is the front end -- the devices
 * /dev/ufs_NAME, the partitions, the write rules -- and calls this for blocks of a logical unit.
 *
 * The firmware (firmware/Silicon/Google/ZumaPkg/Drivers/ZumaUfsDxe) has done everything that needs the vendor's
 * calibration: the device's power and reset, the host's enable, the PHY, the link startup, the device's
 * initialisation and the power mode change to HS gear 4 on both lanes. It leaves the link up and the controller
 * running when it exits boot services, with nothing in flight. Its transfer list was in boot-services memory, gone
 * now; so the first thing here, with the doorbell clear, is a list of this kernel's own: UTRLRSR 0, UTRLBA/UTRLBAU,
 * UTRLRSR 1. Nothing else of the link is touched: no reset, no UIC command, no power mode, no clock.
 *
 * What zuma's controller wants besides the standard (JESD223, UFSHCI 3.0): a slot that carries a SCSI command has its
 * bit set in the vendor HCI's UTRL_NEXUS_TYPE (vs_hci + 0x40) -- every slot here carries one, so all of this list's
 * bits are set once; a PRDT entry describes 4 KiB at most (vs_hci TXPRDT/RXPRDT_ENTRY_SIZE = 12, as the bootloader
 * leaves them); the descriptor's units are the standard ones (the tree's fixed-prdt-req_list-ocs). The controller is
 * IO coherent (dma-coherent in the tree, its sysreg's IOCC bits set); the data buffers are cleaned and invalidated
 * around a transfer all the same, as the firmware does (ufs_zuma_coherent=1: not).
 *
 * Every slot of the controller is used: a request takes a free slot, fills its command descriptor, rings its bit of the
 * doorbell and sleeps until the bit clears -- woken by the controller's interrupt (SPI 0x1d2, level; IS.UTRCS is the
 * only source enabled), or by a short timeout that looks again, so that a lost interrupt costs time and nothing
 * else. Requests from many threads are in flight together, which the loader's one-at-a-time page never allowed.
 *
 * Writes only where the front end has said (akios_zufs_allow_write: whole ranges of a unit, set once at start);
 * anything else is refused here, whatever asks for it. The commands are READ(10/16), WRITE(10/16), SYNCHRONIZE
 * CACHE(10), TEST UNIT READY and READ CAPACITY(10/16); no query, no task management, no UIC command.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#if __has_include(<vm/vm_kern_xnu.h>)
#include <vm/vm_kern_xnu.h>
#endif
#include "akios_ufs_zuma.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);

#define ZUFS_PHYS           0x13200000ULL       /* ufs@13200000: the standard registers, then the vendor HCI at +0x1100 */
#define ZUFS_MAP            0x4000
#define ZUFS_VS_HCI         0x1100
#define ZUFS_INTID          (32 + 0x1d2)        /* the tree's interrupts = <0 0x1d2 4>: an SPI, level */

/* UFSHCI registers */
#define R_CAP               0x00
#define R_VER               0x08
#define R_AHIT              0x18
#define R_IS                0x20
#define R_IE                0x24
#define R_HCS               0x30
#define R_HCE               0x34
#define R_UTRIACR           0x4c
#define R_UTRLBA            0x50
#define R_UTRLBAU           0x54
#define R_UTRLDBR           0x58
#define R_UTRLCLR           0x5c
#define R_UTRLRSR           0x60
#define R_UTMRLDBR          0x78
#define R_VS_NEXUS          (ZUFS_VS_HCI + 0x40)
#define R_VS_TXPRDT         (ZUFS_VS_HCI + 0x00)
#define R_VS_RXPRDT         (ZUFS_VS_HCI + 0x04)

#define CAP_NUTRS(c)        (((c) & 0x1f) + 1)
#define CAP_64AS            (1u << 24)
#define HCS_DP              (1u << 0)
#define HCS_UTRLRDY         (1u << 1)
#define IS_UTRCS            (1u << 0)
#define IS_FATAL            ((1u << 2) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))  /* UE DFES UTPES HCFES SBFES */

#define UTRD_CT_UFS         (1u << 28)
#define UTRD_DD_OUT         (1u << 25)
#define UTRD_DD_IN          (2u << 25)
#define UTRD_INTERRUPT      (1u << 24)
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf

#define UPIU_COMMAND        0x01
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20

#define SLOTS_MAX           32
#define UCD_STRIDE          8192                /* a slot's command descriptor: command, response, PRDT */
#define UCD_RESPONSE        0x20
#define UCD_PRDT            0x80
#define PRDT_ENTRIES        ((UCD_STRIDE - UCD_PRDT) / 16)
#define PRD_MAX             4096u
#define AREA_UCD            PAGE_SIZE           /* the list in the first page, the descriptors after it */
#define AREA_SIZE           (AREA_UCD + SLOTS_MAX * UCD_STRIDE)

#define DIR_NONE            0
#define DIR_IN              1
#define DIR_OUT             2

#define TIMEOUT_MS          30000               /* a SYNCHRONIZE CACHE may take its time */

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10         0x28
#define SCSI_WRITE10        0x2a
#define SCSI_SYNC_CACHE10   0x35
#define SCSI_READ16         0x88
#define SCSI_WRITE16        0x8a
#define SCSI_SAI16          0x9e                /* SERVICE ACTION IN(16): READ CAPACITY(16) */
#define SK_UNIT_ATTENTION   0x6

static volatile uint8_t *zr;
static volatile uint8_t *zarea;
static uint64_t zarea_pa;
static uint32_t zslots, zslot_mask;
static bool zup, zirq_on, zcoherent, z64;
static uint32_t zspin_us = 30;

static uint32_t zlun_mask;
static uint64_t zlun_blocks[AKIOS_ZUFS_MAX_LUNS];
static uint32_t zlun_bsize[AKIOS_ZUFS_MAX_LUNS];

#define WRITE_RANGES        32
static struct { uint8_t lun; uint64_t first, last; } zwrite_ok[WRITE_RANGES];
static uint32_t zwrite_ranges;

static LCK_GRP_DECLARE(zufs_grp, "akios-ufs-zuma");
static LCK_MTX_DECLARE(zufs_slot_mtx, &zufs_grp);
static uint32_t zslots_free, zwaiters, zbusy_now, zbusy_most;
static int zufs_event;                          /* what a waiter sleeps on; the interrupt wakes them */

static uint64_t zcmds, zread_bytes, zwrite_bytes, zerrors, zrefused, zretries, ztimeouts, zwait_ns, zirqs, zslot_waits;
static uint32_t zirq_fatal;

static inline uint32_t
zrd(uint32_t off)
{
	return *(volatile uint32_t *)(zr + off);
}

static inline void
zwr(uint32_t off, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");      /* what was written to the descriptors is out before the register */
	*(volatile uint32_t *)(zr + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline volatile uint32_t *
zutrd(uint32_t slot)
{
	return (volatile uint32_t *)(zarea + 32 * slot);
}

static inline volatile uint8_t *
zucd(uint32_t slot)
{
	return zarea + AREA_UCD + (vm_size_t)slot * UCD_STRIDE;
}

static uint64_t
znow_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static void
zufs_irq(__unused void *context, __unused uint32_t intid)
{
	uint32_t is = zrd(R_IS);

	if (is != 0) {
		zwr(R_IS, is);
	}
	zirq_fatal |= is & IS_FATAL;
	zirqs++;
	thread_wakeup((event_t)&zufs_event);
}

static uint32_t
zslot_get(void)
{
	uint32_t slot;

	lck_mtx_lock(&zufs_slot_mtx);
	while (zslots_free == 0) {
		zwaiters++;
		zslot_waits++;
		(void)msleep(&zslots_free, &zufs_slot_mtx, PRIBIO, "akios-ufs", NULL);
		zwaiters--;
	}
	slot = (uint32_t)__builtin_ctz(zslots_free);
	zslots_free &= ~(1u << slot);
	if (++zbusy_now > zbusy_most) {
		zbusy_most = zbusy_now;
	}
	lck_mtx_unlock(&zufs_slot_mtx);
	return slot;
}

static void
zslot_put(uint32_t slot)
{
	lck_mtx_lock(&zufs_slot_mtx);
	zslots_free |= 1u << slot;
	zbusy_now--;
	if (zwaiters != 0) {
		wakeup_one((caddr_t)&zslots_free);
	}
	lck_mtx_unlock(&zufs_slot_mtx);
}

/* Until the slot's doorbell bit clears or the deadline passes: a short spin, then sleeps the interrupt cuts short */
static bool
zwait(uint32_t bit, uint64_t deadline_ns)
{
	uint64_t spin_end = znow_ns() + zspin_us * 1000ull;

	while (znow_ns() < spin_end) {
		if ((zrd(R_UTRLDBR) & bit) == 0) {
			return true;
		}
		__builtin_arm_yield();
	}
	for (;;) {
		if ((zrd(R_UTRLDBR) & bit) == 0) {
			return true;
		}
		if (znow_ns() > deadline_ns) {
			return false;
		}
		/* asleep first, then a last look: a completion between the two wakes this thread, never passes it by */
		if (zirq_on) {
			assert_wait_timeout((event_t)&zufs_event, THREAD_UNINT, 5, NSEC_PER_MSEC);
		} else {
			assert_wait_timeout((event_t)&zufs_event, THREAD_UNINT, 100, NSEC_PER_USEC);
		}
		if ((zrd(R_UTRLDBR) & bit) == 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			return true;
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/*
 * One command through a slot of its own, waited for. 0: done and GOOD; EIO otherwise (*sense_key: the sense key of a
 * CHECK CONDITION, else 0xff). buf: kernel memory, 4-byte aligned, len whole double words.
 */
static int
zexec(uint8_t lun, const uint8_t cdb[16], int dir, void *buf, uint32_t len, uint8_t *sense_key)
{
	volatile uint32_t *utrd;
	volatile uint8_t *ucd;
	volatile uint32_t *prdt;
	uint32_t slot, entries = 0, ocs, bit;
	uint64_t ucd_pa, started;
	uint8_t response, status;
	int error = 0;

	*sense_key = 0xff;
	if (!zup) {
		return ENXIO;
	}
	if (len != 0 && (buf == NULL || ((uintptr_t)buf & 3) != 0 || (len & 3) != 0)) {
		return EINVAL;
	}
	slot = zslot_get();
	bit = 1u << slot;
	utrd = zutrd(slot);
	ucd = zucd(slot);
	ucd_pa = zarea_pa + AREA_UCD + (uint64_t)slot * UCD_STRIDE;
	prdt = (volatile uint32_t *)(ucd + UCD_PRDT);

	/* the data's pages, 4 KiB at most an entry and never across a 4 KiB boundary of physical memory */
	for (uint32_t done = 0; done < len;) {
		vm_offset_t va = (vm_offset_t)buf + done;
		uint64_t pa = kvtophys(va);
		uint32_t chunk = MIN(len - done, PRD_MAX - (uint32_t)(pa & (PRD_MAX - 1)));

		if (pa == 0 || entries == PRDT_ENTRIES || (!z64 && (pa + chunk) > (1ull << 32))) {
			zslot_put(slot);
			printf("akios-ufs: a buffer the controller cannot reach (va %p, pa 0x%llx, entry %u)\n", (void *)va, pa,
			    entries);
			return EFAULT;
		}
		prdt[entries * 4 + 0] = (uint32_t)pa;
		prdt[entries * 4 + 1] = (uint32_t)(pa >> 32);
		prdt[entries * 4 + 2] = 0;
		prdt[entries * 4 + 3] = chunk - 1;
		entries++;
		done += chunk;
	}
	if (len != 0 && !zcoherent) {
		/* no dirty line of ours may land on what the device writes; what it reads is in memory */
		FlushPoC_DcacheRegion((vm_offset_t)buf, len);
	}

	/* the command UPIU, and the response's place cleared */
	for (uint32_t i = 0; i < UCD_PRDT / 4; i++) {
		((volatile uint32_t *)ucd)[i] = 0;
	}
	ucd[0] = UPIU_COMMAND;
	ucd[1] = len == 0 ? 0 : dir == DIR_IN ? UPIU_FLAG_READ : UPIU_FLAG_WRITE;
	ucd[2] = lun;
	ucd[3] = (uint8_t)slot;                         /* the task tag: the slot, unique among those in flight */
	ucd[12] = (uint8_t)(len >> 24);
	ucd[13] = (uint8_t)(len >> 16);
	ucd[14] = (uint8_t)(len >> 8);
	ucd[15] = (uint8_t)len;
	for (int i = 0; i < 16; i++) {
		ucd[16 + i] = cdb[i];
	}

	utrd[0] = UTRD_CT_UFS | UTRD_INTERRUPT | (len == 0 ? 0 : dir == DIR_IN ? UTRD_DD_IN : UTRD_DD_OUT);
	utrd[1] = 0;
	utrd[2] = OCS_INVALID;
	utrd[3] = 0;
	utrd[4] = (uint32_t)ucd_pa;
	utrd[5] = (uint32_t)(ucd_pa >> 32);
	utrd[6] = ((UCD_RESPONSE / 4) << 16) | (64 / 4);        /* response offset, length (double words) */
	utrd[7] = ((UCD_PRDT / 4) << 16) | entries;             /* PRDT offset (double words), length (entries) */

	started = znow_ns();
	zwr(R_UTRLDBR, bit);
	if (!zwait(bit, started + TIMEOUT_MS * 1000000ull)) {
		/* no answer: the slot taken back (a 0 clears it), and it may be used again once the controller lets go */
		zwr(R_UTRLCLR, ~bit);
		for (int i = 0; i < 1000 && (zrd(R_UTRLDBR) & bit); i++) {
			delay(100);
		}
		ztimeouts++;
		zerrors++;
		printf("akios-ufs: LUN %u op 0x%02x: no answer in %u s (IS 0x%08x, doorbell 0x%08x)\n", lun, cdb[0],
		    TIMEOUT_MS / 1000, zrd(R_IS), zrd(R_UTRLDBR));
		if (zrd(R_UTRLDBR) & bit) {
			return EIO;                     /* the slot stays taken: it is not safe to reuse */
		}
		zslot_put(slot);
		return EIO;
	}
	__asm__ volatile ("dmb sy" ::: "memory");
	os_atomic_add(&zwait_ns, znow_ns() - started, relaxed);
	os_atomic_inc(&zcmds, relaxed);
	ocs = utrd[2] & 0xff;
	response = ucd[UCD_RESPONSE + 6];
	status = ucd[UCD_RESPONSE + 7];
	if (ocs != OCS_SUCCESS) {
		error = EIO;
		printf("akios-ufs: LUN %u op 0x%02x: OCS 0x%x (IS 0x%08x)\n", lun, cdb[0], ocs, zrd(R_IS));
	} else if (response != 0 || status != 0) {
		uint32_t sense_len = ((uint32_t)ucd[UCD_RESPONSE + 32] << 8) | ucd[UCD_RESPONSE + 33];

		error = EIO;
		if (status == 0x02 && sense_len >= 3) {
			*sense_key = ucd[UCD_RESPONSE + 34 + 2] & 0xf;
		}
	}
	if (len != 0 && dir == DIR_IN && !zcoherent) {
		FlushPoC_DcacheRegion((vm_offset_t)buf, len);
	}
	zslot_put(slot);
	if (error != 0) {
		os_atomic_inc(&zerrors, relaxed);
	}
	(void)response;
	return error;
}

/* The same, a unit attention (the device's news of a reset or a change) and a failed transport tried again */
static int
zexec_retry(uint8_t lun, const uint8_t cdb[16], int dir, void *buf, uint32_t len, uint8_t *sense_key)
{
	int error = EIO;

	for (int tries = 0; tries < 4; tries++) {
		error = zexec(lun, cdb, dir, buf, len, sense_key);
		if (error != EIO || (*sense_key != SK_UNIT_ATTENTION && (*sense_key != 0xff || tries >= 1))) {
			break;
		}
		os_atomic_inc(&zretries, relaxed);
	}
	return error;
}

static void
zcdb_rw(uint8_t cdb[16], bool write, bool fua, uint64_t lba, uint32_t blocks)
{
	bzero(cdb, 16);
	if (lba + blocks <= 0xffffffffull && blocks <= 0xffff) {
		cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
		cdb[1] = fua ? 0x08 : 0;
		cdb[2] = (uint8_t)(lba >> 24);
		cdb[3] = (uint8_t)(lba >> 16);
		cdb[4] = (uint8_t)(lba >> 8);
		cdb[5] = (uint8_t)lba;
		cdb[7] = (uint8_t)(blocks >> 8);
		cdb[8] = (uint8_t)blocks;
	} else {
		cdb[0] = write ? SCSI_WRITE16 : SCSI_READ16;
		cdb[1] = fua ? 0x08 : 0;
		for (int i = 0; i < 8; i++) {
			cdb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
		}
		cdb[10] = (uint8_t)(blocks >> 24);
		cdb[11] = (uint8_t)(blocks >> 16);
		cdb[12] = (uint8_t)(blocks >> 8);
		cdb[13] = (uint8_t)blocks;
	}
}

static bool
zlun_ok(uint8_t lun, uint64_t lba, uint64_t blocks)
{
	return zup && lun < AKIOS_ZUFS_MAX_LUNS && (zlun_mask & (1u << lun)) && blocks != 0 &&
	       lba < zlun_blocks[lun] && blocks <= zlun_blocks[lun] - lba;
}

int
akios_zufs_read(uint8_t lun, uint64_t lba, uint64_t blocks, void *buf)
{
	uint8_t cdb[16], sk;
	uint32_t bs, per;
	int error = 0;

	if (!zlun_ok(lun, lba, blocks)) {
		return EINVAL;
	}
	bs = zlun_bsize[lun];
	per = AKIOS_ZUFS_CMD_BYTES / bs;
	while (blocks > 0 && error == 0) {
		uint32_t n = (uint32_t)MIN(blocks, (uint64_t)per);

		zcdb_rw(cdb, false, false, lba, n);
		error = zexec_retry(lun, cdb, DIR_IN, buf, n * bs, &sk);
		if (error != 0) {
			printf("akios-ufs: LUN %u: read of blocks %llu..%llu failed (%d, sense key 0x%x)\n", lun, lba,
			    lba + n - 1, error, sk);
			break;
		}
		os_atomic_add(&zread_bytes, (uint64_t)n * bs, relaxed);
		lba += n;
		blocks -= n;
		buf = (uint8_t *)buf + (size_t)n * bs;
	}
	return error;
}

static bool
zwrite_permitted(uint8_t lun, uint64_t lba, uint64_t blocks)
{
	for (uint32_t i = 0; i < zwrite_ranges; i++) {
		if (zwrite_ok[i].lun == lun && lba >= zwrite_ok[i].first && lba + blocks - 1 <= zwrite_ok[i].last) {
			return true;
		}
	}
	return false;
}

int
akios_zufs_write(uint8_t lun, uint64_t lba, uint64_t blocks, const void *buf, bool fua)
{
	uint8_t cdb[16], sk;
	uint32_t bs, per;
	int error = 0;

	if (!zlun_ok(lun, lba, blocks)) {
		return EINVAL;
	}
	if (!zwrite_permitted(lun, lba, blocks)) {
		os_atomic_inc(&zrefused, relaxed);
		printf("akios-ufs: write REFUSED: LUN %u blocks %llu..%llu are not in a range this kernel may write\n", lun, lba,
		    lba + blocks - 1);
		return EROFS;
	}
	bs = zlun_bsize[lun];
	per = AKIOS_ZUFS_CMD_BYTES / bs;
	while (blocks > 0 && error == 0) {
		uint32_t n = (uint32_t)MIN(blocks, (uint64_t)per);

		zcdb_rw(cdb, true, fua, lba, n);
		error = zexec_retry(lun, cdb, DIR_OUT, (void *)(uintptr_t)buf, n * bs, &sk);
		if (error != 0) {
			printf("akios-ufs: LUN %u: write of blocks %llu..%llu failed (%d, sense key 0x%x)\n", lun, lba,
			    lba + n - 1, error, sk);
			break;
		}
		os_atomic_add(&zwrite_bytes, (uint64_t)n * bs, relaxed);
		lba += n;
		blocks -= n;
		buf = (const uint8_t *)buf + (size_t)n * bs;
	}
	return error;
}

int
akios_zufs_sync(uint8_t lun)
{
	uint8_t cdb[16] = { SCSI_SYNC_CACHE10 }, sk;

	if (!zup || lun >= AKIOS_ZUFS_MAX_LUNS || !(zlun_mask & (1u << lun))) {
		return ENXIO;
	}
	return zexec_retry(lun, cdb, DIR_NONE, NULL, 0, &sk);
}

bool
akios_zufs_allow_write(uint8_t lun, uint64_t first, uint64_t last)
{
	if (zwrite_ranges == WRITE_RANGES || lun >= AKIOS_ZUFS_MAX_LUNS || last < first) {
		return false;
	}
	zwrite_ok[zwrite_ranges].lun = lun;
	zwrite_ok[zwrite_ranges].first = first;
	zwrite_ok[zwrite_ranges].last = last;
	zwrite_ranges++;
	return true;
}

uint32_t
akios_zufs_lun_mask(void)
{
	return zlun_mask;
}

uint64_t
akios_zufs_lun_blocks(uint8_t lun)
{
	return lun < AKIOS_ZUFS_MAX_LUNS ? zlun_blocks[lun] : 0;
}

uint32_t
akios_zufs_lun_bsize(uint8_t lun)
{
	return lun < AKIOS_ZUFS_MAX_LUNS ? zlun_bsize[lun] : 0;
}

void
akios_zufs_report(const char *when)
{
	if (!zup) {
		return;
	}
	printf("akios-ufs: %s (%llu s up): %llu commands (%llu MB read, %llu MB written), %llu errors (%llu timeouts), %llu retried, "
	    "%llu writes refused; waiting %llu ms in all; %u slots, at most %u in flight, %llu waits for a slot; "
	    "%llu interrupts%s, IS errors 0x%x\n", when, znow_ns() / NSEC_PER_SEC, zcmds, zread_bytes >> 20, zwrite_bytes >> 20,
	    zerrors, ztimeouts, zretries, zrefused, zwait_ns / 1000000, zslots, zbusy_most, zslot_waits, zirqs, zirq_on ? "" : " (polling)",
	    zirq_fatal);
}

/* Which logical units answer, and how big each is */
static void
zfind_luns(void)
{
	uint8_t *data = kalloc_data(4096, Z_WAITOK | Z_ZERO), sk;

	if (data == NULL) {
		return;
	}
	for (uint8_t lun = 0; lun < AKIOS_ZUFS_MAX_LUNS; lun++) {
		uint8_t cdb[16] = { SCSI_TEST_UNIT_READY };
		uint64_t last = 0;
		uint32_t bs = 0;
		int error;

		error = zexec_retry(lun, cdb, DIR_NONE, NULL, 0, &sk);
		if (error != 0) {
			if (lun < 4) {
				printf("akios-ufs: LUN %u: TEST UNIT READY failed (sense key 0x%x): not used\n", lun, sk);
			}
			continue;
		}
		bzero(cdb, sizeof(cdb));
		cdb[0] = SCSI_SAI16;
		cdb[1] = 0x10;
		cdb[13] = 32;
		bzero(data, 32);
		if (zexec_retry(lun, cdb, DIR_IN, data, 32, &sk) == 0) {
			for (int i = 0; i < 8; i++) {
				last = last << 8 | data[i];
			}
			bs = (uint32_t)data[8] << 24 | (uint32_t)data[9] << 16 | (uint32_t)data[10] << 8 | data[11];
		} else {
			bzero(cdb, sizeof(cdb));
			cdb[0] = SCSI_READ_CAPACITY10;
			bzero(data, 8);
			if (zexec_retry(lun, cdb, DIR_IN, data, 8, &sk) != 0) {
				printf("akios-ufs: LUN %u: no capacity (sense key 0x%x): not used\n", lun, sk);
				continue;
			}
			last = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
			bs = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
		}
		if (bs < 512 || bs > 65536 || (bs & (bs - 1)) != 0 || last == 0) {
			printf("akios-ufs: LUN %u: a capacity that makes no sense (last LBA 0x%llx, block %u): not used\n", lun,
			    last, bs);
			continue;
		}
		zlun_blocks[lun] = last + 1;
		zlun_bsize[lun] = bs;
		zlun_mask |= 1u << lun;
		printf("akios-ufs: LUN %u: %llu blocks of %u (%llu MB)\n", lun, last + 1, bs, ((last + 1) * bs) >> 20);
	}
	kfree_data(data, 4096);
}

bool
akios_zufs_probe(void)
{
	uint32_t cap, ver, hce, hcs, ie, is, iacr, rsr, dbr, nexus, irq = 1, coherent = 0, slots = SLOTS_MAX;
	vm_offset_t cached = 0;

	zr = (volatile uint8_t *)ml_io_map(ZUFS_PHYS, ZUFS_MAP);
	if (zr == NULL) {
		printf("akios-ufs: the controller's registers could not be mapped\n");
		return false;
	}
	cap = zrd(R_CAP);
	ver = zrd(R_VER);
	hce = zrd(R_HCE);
	hcs = zrd(R_HCS);
	ie = zrd(R_IE);
	is = zrd(R_IS);
	iacr = zrd(R_UTRIACR);
	rsr = zrd(R_UTRLRSR);
	dbr = zrd(R_UTRLDBR);
	nexus = zrd(R_VS_NEXUS);
	printf("akios-ufs: ufs@13200000 as the firmware left it: version 0x%x, CAP 0x%08x (%u slots%s), HCE %u, HCS 0x%08x, "
	    "IE 0x%x, IS 0x%x, UTRIACR 0x%x, AHIT 0x%x, list at 0x%x%08x (run %u, doorbell 0x%x), nexus 0x%x, PRDT entry 2^%u/2^%u\n",
	    ver, cap, CAP_NUTRS(cap), (cap & CAP_64AS) ? ", 64-bit addresses" : "", hce & 1, hcs, ie, is, iacr, zrd(R_AHIT),
	    zrd(R_UTRLBAU), zrd(R_UTRLBA), rsr & 1, dbr, nexus, zrd(R_VS_TXPRDT) & 0xf, zrd(R_VS_RXPRDT) & 0xf);
	if (!(hce & 1) || (hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
		printf("akios-ufs: the controller is not running with a device on its link: not used\n");
		return false;
	}
	for (int i = 0; i < 1000 && zrd(R_UTRLDBR) != 0; i++) {
		delay(100);
	}
	if (zrd(R_UTRLDBR) != 0 || zrd(R_UTMRLDBR) != 0) {
		printf("akios-ufs: requests are still in flight (doorbell 0x%x, task 0x%x): not used\n", zrd(R_UTRLDBR),
		    zrd(R_UTMRLDBR));
		return false;
	}
	PE_parse_boot_argn("ufs_zuma_slots", &slots, sizeof(slots));
	PE_parse_boot_argn("ufs_zuma_irq", &irq, sizeof(irq));
	PE_parse_boot_argn("ufs_zuma_coherent", &coherent, sizeof(coherent));
	PE_parse_boot_argn("ufs_zuma_spin_us", &zspin_us, sizeof(zspin_us));
	zcoherent = coherent != 0;
	z64 = (cap & CAP_64AS) != 0;
	if (zspin_us > 2000) {
		zspin_us = 2000;
	}
	zslots = MIN(MIN(slots, (uint32_t)CAP_NUTRS(cap)), (uint32_t)SLOTS_MAX);
	if (zslots == 0) {
		zslots = 1;
	}
	zslot_mask = zslots == 32 ? 0xffffffffu : (1u << zslots) - 1;

	/* the list and the command descriptors: contiguous memory, used only through an uncached mapping */
	if (kmem_alloc_contig(kernel_map, &cached, AREA_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_PERMANENT,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("akios-ufs: no memory for the transfer list: not used\n");
		return false;
	}
	FlushPoC_DcacheRegion(cached, AREA_SIZE);
	zarea_pa = kvtophys(cached);
	zarea = (volatile uint8_t *)ml_io_map_wcomb((vm_offset_t)zarea_pa, AREA_SIZE);
	if (zarea == NULL || (!(cap & CAP_64AS) && zarea_pa + AREA_SIZE > (1ull << 32))) {
		printf("akios-ufs: the transfer list at 0x%llx cannot be used: not used\n", zarea_pa);
		return false;
	}
	for (uint32_t i = 0; i < AREA_SIZE / 4; i++) {
		((volatile uint32_t *)zarea)[i] = 0;
	}

	/* this kernel's list in, nothing in flight */
	zwr(R_UTRLRSR, 0);
	zwr(R_UTRLBA, (uint32_t)zarea_pa);
	zwr(R_UTRLBAU, (uint32_t)(zarea_pa >> 32));
	zwr(R_UTRLRSR, 1);
	if (zrd(R_UTRLBA) != (uint32_t)zarea_pa || zrd(R_UTRLBAU) != (uint32_t)(zarea_pa >> 32) || !(zrd(R_UTRLRSR) & 1)) {
		printf("akios-ufs: the list did not go in (UTRLBA 0x%x%08x, run %u): not used\n", zrd(R_UTRLBAU), zrd(R_UTRLBA),
		    zrd(R_UTRLRSR) & 1);
		return false;
	}
	if (iacr & (1u << 31)) {
		zwr(R_UTRIACR, 0);              /* no interrupt aggregation: each completion says so at once */
	}
	zwr(R_VS_NEXUS, nexus | zslot_mask);    /* every slot of this list carries a SCSI command */
	zslots_free = zslot_mask;
	zup = true;

	zwr(R_IE, 0);
	zwr(R_IS, zrd(R_IS));
	if (irq != 0) {
		kern_return_t kr = gic_v3_spi_attach(ZUFS_INTID, false, zufs_irq, NULL);

		if (kr == KERN_SUCCESS) {
			zwr(R_IE, IS_UTRCS);
			zirq_on = true;
		} else {
			printf("akios-ufs: the interrupt (SPI %u) could not be taken (%d): completions are polled\n", ZUFS_INTID, kr);
		}
	}
	printf("akios-ufs: this kernel's transfer list at 0x%llx, %u slots, %s; caches %s around transfers\n", zarea_pa, zslots,
	    zirq_on ? "completions by interrupt" : "completions polled", zcoherent ? "not maintained" : "cleaned and invalidated");

	zfind_luns();
	if (!(zlun_mask & 1)) {
		printf("akios-ufs: LUN 0 does not answer: the storage is not used\n");
		akios_zufs_report("at the probe");
		zup = false;
		return false;
	}
	akios_zufs_report("at the probe");
	return true;
}
