/*
 * /dev/hakios-console: the console log of this boot, read-only, from its first character -- the ring the USB serial
 * port and the usbmux log channel send (osfmk/arm64/loader_return.c: the kernel's lines, launchd's and every daemon's
 * that logs to the console). For userland to keep it on the phone: init's boot log (userland/src/init.c,
 * boot_log_after_mount) writes it to /private/var/mobile/Media/hakios-boot/boot-1.log, the boot before as boot-2.log.
 *
 * Every open starts at the beginning of the boot. A read gives what there is; with nothing new it waits up to half a
 * second and returns 0, so a reader just reads again. One reader at a time (a second open is EBUSY).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>   /* hz */
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <libkern/OSAtomic.h>

extern size_t loader_log_copy(uint64_t *cursor, char *out, size_t max);

void hakios_console_log_init(void);

static uint64_t cursor;
static volatile SInt32 opened;
static int wait_chan;

static int
console_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (!OSCompareAndSwap(0, 1, &opened)) {
		return EBUSY;
	}
	cursor = 0;
	return 0;
}

static int
console_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	opened = 0;
	return 0;
}

static int
console_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char buf[1024];
	size_t n;

	n = loader_log_copy(&cursor, buf, MIN(sizeof(buf), (size_t)uio_resid(uio)));
	if (n == 0) {
		/* nothing new: wait a little (interruptibly) so that the reader does not spin, then report none */
		(void)tsleep(&wait_chan, PCATCH | PZERO, "hakios-console", hz / 2);
		n = loader_log_copy(&cursor, buf, MIN(sizeof(buf), (size_t)uio_resid(uio)));
		if (n == 0) {
			return 0;
		}
	}
	return uiomove(buf, (int)n, uio);
}

static const struct cdevsw console_cdevsw = {
	.d_open = console_open, .d_close = console_close, .d_read = console_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_console_log_init(void)
{
	int major = cdevsw_add(-1, &console_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0400, "hakios-console") == NULL) {
		printf("hakios-console: cannot create the device\n");
	}
}
