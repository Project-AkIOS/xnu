/*
 * AkIOS (11-22): /dev/hakios-serial -- the Pixel 8a's USB serial console. The CDC-ACM function of configurations 3 and
 * 4 (hakios.akita_usb.c: interfaces "AkIOS Serial Console", endpoints 0x86/0x06 + notification 0x87) is a host's
 * /dev/ttyACM0; this is the phone's end of it, for a program of the user space (userland/src/serial_console.c, job
 * com.hakios.serial), and the kernel's console log goes out on it too.
 *
 *   read()   what the host typed (bulk OUT), blocking until there is some (EWOULDBLOCK with O_NONBLOCK)
 *   write()  bytes for the host (bulk IN), through a ring the USB poll thread empties; dropped while no host has the
 *            port open (DTR low), waited for (room in the ring) while one has
 *   select() readable when the host typed something; always writable
 *   ioctl    HAKIOS_SERIAL_KLOG (int): the kernel log on the port on (1) or off (0)
 *
 * THE KERNEL LOG: nothing hooks printf. The USB poll thread, when the bulk IN endpoint is free and a host has the port
 * open, takes the next piece of the console log with loader_log_copy() (osfmk/arm64/loader_return.c, the ring every
 * console character goes into; the usbmux log port reads it the same way) by its own cursor. printf never waits for
 * USB; what came while no host listened is sent when one comes (as much as the 1 MiB ring still has), from the first
 * character of the boot on for the first host. Boot-arg serial_klog=0: off until the ioctl turns it on.
 *
 * The transfers themselves are started by the USB driver's poll thread (acm_service), as PTP's are: here the buffers.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define HAKIOS_SERIAL_KLOG      _IOW('h', 0x53, int)    /* userland/src/serial_console.c has the same */

#define RX_RING         (16 * 1024)
#define TX_RING         (64 * 1024)
#define ACM_MPS         512

static LCK_GRP_DECLARE(serial_grp, "akita serial");
static LCK_MTX_DECLARE(serial_lock, &serial_grp);
static uint8_t rx_ring[RX_RING];                /* what the host typed and nobody read yet */
static uint32_t rx_head, rx_fill;
static uint8_t tx_ring[TX_RING];                /* what a writer left for the host */
static uint32_t tx_head, tx_fill;
static bool link_up, dtr;
/*
 * AkIOS (11-22): when the host opens the port (DTR up) its tty is in cooked mode, echo on, until the program sets it
 * raw: the first bytes sent came back as typed input and the console ran them as a command. So nothing is sent for
 * QUIET_SEND_MS after DTR rises, and what the host sends in its first QUIET_RECV_MS is dropped.
 */
static uint64_t dtr_up_at;
#define QUIET_SEND_MS 500
#define QUIET_RECV_MS 1000

static bool
since_dtr_less_than(uint32_t ms)
{
	uint64_t now = mach_absolute_time(), limit;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &limit);
	return now - dtr_up_at < limit;
}
static int klog_on = 1;
static uint64_t klog_cursor;                    /* the next character of the console log to send */
static struct selinfo rx_sel;

extern size_t loader_log_copy(uint64_t *cursor, char *out, size_t max);

/* from the USB driver's poll thread ---------------------------------------------------------------------------- */

void akita_serial_link(int up);
void akita_serial_dtr(int on);
void akita_serial_out(const uint8_t *data, uint32_t len);
bool akita_serial_out_room(uint32_t len);
uint32_t akita_serial_in_take(uint8_t *buf, uint32_t max);

/* the ACM interfaces configured (1) or gone (0: a reset, a cable out, another configuration) */
void
akita_serial_link(int up)
{
	lck_mtx_lock(&serial_lock);
	link_up = up != 0;
	dtr = false;
	tx_head = tx_fill = 0;
	lck_mtx_unlock(&serial_lock);
	wakeup(&tx_fill);
}

/* SET_CONTROL_LINE_STATE: DTR up while a host program has the tty open */
void
akita_serial_dtr(int on)
{
	lck_mtx_lock(&serial_lock);
	if (dtr && !on) {
		tx_head = tx_fill = 0;          /* nobody to read it */
	}
	if (!dtr && link_up && on) {
		dtr_up_at = mach_absolute_time();
	}
	dtr = link_up && on;
	lck_mtx_unlock(&serial_lock);
	wakeup(&tx_fill);
}

void
akita_serial_out(const uint8_t *data, uint32_t len)
{
	lck_mtx_lock(&serial_lock);
	if (since_dtr_less_than(QUIET_RECV_MS)) {
		lck_mtx_unlock(&serial_lock);
		return;                         /* the host's tty echoing what we sent before it was raw */
	}
	for (uint32_t i = 0; i < len && rx_fill < RX_RING; i++) {
		rx_ring[(rx_head + rx_fill) % RX_RING] = data[i];
		rx_fill++;
	}
	selwakeup(&rx_sel);
	lck_mtx_unlock(&serial_lock);
	wakeup(&rx_fill);
}

bool
akita_serial_out_room(uint32_t len)
{
	return RX_RING - rx_fill >= len;        /* (without the lock: at worst a transfer later) */
}

/*
 * The next bulk IN transfer: what a writer left first, else the kernel log. Never a multiple of the packet size (the
 * host's read would wait for a zero-length packet we do not send): one byte is left for the next time instead.
 */
uint32_t
akita_serial_in_take(uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;
	bool klog;

	lck_mtx_lock(&serial_lock);
	if (!link_up || !dtr || since_dtr_less_than(QUIET_SEND_MS)) {
		lck_mtx_unlock(&serial_lock);
		return 0;
	}
	if (tx_fill) {
		uint32_t first;

		n = tx_fill < max ? tx_fill : max;
		first = TX_RING - tx_head < n ? TX_RING - tx_head : n;   /* the ring's end, then its start */
		memcpy(buf, tx_ring + tx_head, first);
		memcpy(buf + first, tx_ring, n - first);
	}
	if (n > 1 && n % ACM_MPS == 0) {
		n--;
	}
	tx_head = (tx_head + n) % TX_RING;
	tx_fill -= n;
	klog = klog_on;
	lck_mtx_unlock(&serial_lock);
	if (n) {
		wakeup(&tx_fill);
		return n;
	}
	if (!klog) {
		return 0;
	}
	n = (uint32_t)loader_log_copy(&klog_cursor, (char *)buf, max);
	/* whole lines when there are any, so a writer's bytes do not land inside one */
	if (n > 0 && buf[n - 1] != '\n') {
		uint32_t keep = n;

		while (keep > 0 && buf[keep - 1] != '\n') {
			keep--;
		}
		if (keep > 0) {
			klog_cursor -= n - keep;
			n = keep;
		}
	}
	if (n > 1 && n % ACM_MPS == 0) {
		klog_cursor--;
		n--;
	}
	return n;
}

/* the device node --------------------------------------------------------------------------------------------- */

static int
serial_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static int
serial_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static int
serial_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	int error = 0;

	lck_mtx_lock(&serial_lock);
	while (rx_fill == 0) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(&serial_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&rx_fill, &serial_lock, PCATCH | PRIBIO, "serial_read", NULL);
		if (error) {
			lck_mtx_unlock(&serial_lock);
			return error;
		}
	}
	while (rx_fill > 0 && uio_resid(uio) > 0 && !error) {
		uint32_t n = RX_RING - rx_head;

		if (n > rx_fill) {
			n = rx_fill;
		}
		if ((user_ssize_t)n > uio_resid(uio)) {
			n = (uint32_t)uio_resid(uio);
		}
		error = uiomove((char *)rx_ring + rx_head, (int)n, uio);
		if (!error) {
			rx_head = (rx_head + n) % RX_RING;
			rx_fill -= n;
		}
	}
	lck_mtx_unlock(&serial_lock);
	return error;
}

static int
serial_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t chunk[512];
	int error = 0;

	while (uio_resid(uio) > 0 && !error) {
		uint32_t n = uio_resid(uio) > (user_ssize_t)sizeof(chunk) ? (uint32_t)sizeof(chunk) : (uint32_t)uio_resid(uio);

		error = uiomove((char *)chunk, (int)n, uio);
		if (error) {
			break;
		}
		lck_mtx_lock(&serial_lock);
		while (link_up && dtr && TX_RING - tx_fill < n && !error) {
			error = msleep(&tx_fill, &serial_lock, PCATCH | PRIBIO, "serial_write", NULL);
		}
		if (!error && link_up && dtr) {         /* (no host: dropped) */
			for (uint32_t i = 0; i < n; i++) {
				tx_ring[(tx_head + tx_fill + i) % TX_RING] = chunk[i];
			}
			tx_fill += n;
		}
		lck_mtx_unlock(&serial_lock);
	}
	return error;
}

static int
serial_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused proc_t p)
{
	if (cmd == HAKIOS_SERIAL_KLOG) {
		klog_on = *(int *)data != 0;
		return 0;
	}
	if (cmd == FIONBIO || cmd == FIOASYNC) {
		return 0;               /* (fcntl O_NONBLOCK: the read looks at IO_NDELAY) */
	}
	return ENOTTY;
}

static int
serial_select(__unused dev_t dev, int which, void *wql, proc_t p)
{
	int ready = 0;

	switch (which) {
	case FREAD:
		lck_mtx_lock(&serial_lock);
		if (rx_fill > 0) {
			ready = 1;
		} else {
			selrecord(p, &rx_sel, wql);
		}
		lck_mtx_unlock(&serial_lock);
		break;
	case FWRITE:
		ready = 1;
		break;
	default:
		break;
	}
	return ready;
}

static const struct cdevsw serial_cdevsw = {
	.d_open = serial_open,
	.d_close = serial_close,
	.d_read = serial_read,
	.d_write = serial_write,
	.d_ioctl = serial_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_ttys = NULL,
	.d_select = serial_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type = 0,
};

static void
serial_node(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	int major = cdevsw_add(-1, &serial_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "hakios-serial") == NULL) {
		printf("akita serial: /dev/hakios-serial could not be made\n");
		return;
	}
	printf("akita serial: /dev/hakios-serial (the CDC-ACM console: the host's ttyACM)\n");
}

/* from the USB driver's start (usbmux=1): the node, once BSD's devfs is there */
void akita_serial_init(void);

void
akita_serial_init(void)
{
	static thread_call_t call;
	uint64_t deadline;
	uint32_t on = 1;

	if (PE_parse_boot_argn("serial_klog", &on, sizeof(on))) {
		klog_on = on != 0;
	}
	call = thread_call_allocate(serial_node, NULL);
	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
