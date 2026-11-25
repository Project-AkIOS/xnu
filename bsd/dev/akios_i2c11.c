/*
 * The I2C bus of the Pixel 8a's USB-C chips: zuma's hsi2c@10cb0000 (Linux: i2c-11 under 10cb0000.hsi2c), an Exynos 7
 * HSI2C in a USI block. On it: the MAX77759 (fuel gauge 0x36, charger 0x69, USB-C port controller 0x25) and the eUSB2
 * repeater (0x3e). Two drivers use it -- the battery's (hakios.akita_battery.c) and the USB port's
 * (akios_usb_phy_zuma.c) -- so every transfer here is under one lock.
 *
 * Google's bootloader leaves the controller set up (its timings, the USI in I2C mode); only automatic mode and the FIFOs
 * are made sure of before the first transfer. Transfers are polled: the Exynos 7 variant of the controller, completion
 * and errors in INT_STAT. Register layout: the controller's databook names as Linux's drivers/i2c/busses/i2c-exynos5.c
 * and U-Boot's exynos_hs_i2c.c use them.
 *
 *   bool akios_i2c11_read(addr, reg, buf, len)    the register's number written, a repeated start, len bytes read
 *   bool akios_i2c11_write(addr, reg, value)      one byte written to a register
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define HSI2C_PHYS              0x10cb0000UL

#define CTL                     0x00
#define FIFO_CTL                0x04
#define INT_STAT                0x24
#define FIFO_STAT               0x30
#define TX_DATA                 0x34
#define RX_DATA                 0x38
#define CONF                    0x40
#define AUTO_CONF               0x44
#define TIMEOUT                 0x48
#define TRANS_STATUS            0x50
#define ADDR                    0x70

#define CTL_FUNC_MODE_I2C       (1u << 0)
#define CTL_MASTER              (1u << 3)
#define CTL_RXCHON              (1u << 6)
#define CTL_TXCHON              (1u << 7)
#define FIFO_RX_EN              (1u << 0)
#define FIFO_TX_EN              (1u << 1)
#define CONF_AUTO_MODE          (1u << 31)
#define AUTO_READ               (1u << 16)
#define AUTO_STOP               (1u << 17)
#define AUTO_RUN                (1u << 31)
#define TIMEOUT_EN              (1u << 31)
#define INT_DONE                (1u << 7)
#define INT_ABORT               (1u << 8)
#define INT_NO_ACK              (1u << 9)
#define INT_NO_DEV              (1u << 10)
#define INT_TIMEOUT             (1u << 11)
#define INT_ERRORS              (INT_ABORT | INT_NO_ACK | INT_NO_DEV | INT_TIMEOUT)
#define INT_ALL                 (INT_DONE | INT_ERRORS)
#define FIFO_TX_FULL            (1u << 7)
#define FIFO_RX_EMPTY           (1u << 24)
#define MASTER_BUSY             (1u << 17)
#define WAIT_US                 10000

static LCK_GRP_DECLARE(akios_i2c11_grp, "akios i2c11");
static LCK_MTX_DECLARE(akios_i2c11_lock, &akios_i2c11_grp);
static vm_offset_t regs;
static uint32_t errors;

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(regs + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(regs + off) = v;
}

/* mapped, automatic mode and FIFOs on; under the lock */
static bool
bus_ready(void)
{
	if (regs != 0) {
		return true;
	}
	regs = ml_io_map(HSI2C_PHYS, 0x1000);
	if (regs == 0) {
		printf("akios i2c11: could not map the controller\n");
		return false;
	}
	if (!(rd(CONF) & CONF_AUTO_MODE) || (rd(FIFO_CTL) & (FIFO_RX_EN | FIFO_TX_EN)) != (FIFO_RX_EN | FIFO_TX_EN)) {
		printf("akios i2c11: the controller as found: CONF %#x FIFO_CTL %#x; automatic mode and FIFOs on\n", rd(CONF),
		    rd(FIFO_CTL));
		wr(CONF, rd(CONF) | CONF_AUTO_MODE);
		wr(FIFO_CTL, rd(FIFO_CTL) | FIFO_RX_EN | FIFO_TX_EN);
	}
	return true;
}

static bool
wait_for(uint32_t off, uint32_t mask, uint32_t want)
{
	for (unsigned i = 0; i < WAIT_US; i++) {
		if ((rd(off) & mask) == want) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* the end of a transfer: 0 when done, else what went wrong (~0: nothing within WAIT_US) */
static uint32_t
transfer_end(void)
{
	for (unsigned i = 0; i < WAIT_US; i++) {
		uint32_t s = rd(INT_STAT);

		if (s & INT_ALL) {
			wr(INT_STAT, s);
			return (s & INT_ERRORS) ? s : 0;
		}
		delay(1);
	}
	return ~0u;
}

/* one transfer to or from the chip at addr: len bytes, with a stop after it or not (a repeated start follows) */
static bool
transfer(uint8_t addr, bool read, uint8_t *buf, unsigned len, bool stop)
{
	uint32_t end;
	bool ok = true;

	wr(TIMEOUT, rd(TIMEOUT) & ~TIMEOUT_EN);
	wr(ADDR, (uint32_t)(addr & 0x3ff) << 10);
	wr(CTL, (read ? CTL_RXCHON : CTL_TXCHON) | CTL_FUNC_MODE_I2C | CTL_MASTER);
	wr(INT_STAT, INT_ALL);
	wr(AUTO_CONF, len | AUTO_RUN | (stop ? AUTO_STOP : 0) | (read ? AUTO_READ : 0));
	for (unsigned i = 0; i < len && ok; i++) {
		if (read) {
			ok = wait_for(FIFO_STAT, FIFO_RX_EMPTY, 0);
			if (ok) {
				buf[i] = (uint8_t)rd(RX_DATA);
			}
		} else {
			ok = wait_for(FIFO_STAT, FIFO_TX_FULL, 0);
			if (ok) {
				wr(TX_DATA, buf[i]);
			}
		}
	}
	end = transfer_end();
	if (stop) {
		wait_for(TRANS_STATUS, MASTER_BUSY, 0);
	}
	wr(CTL, CTL_FUNC_MODE_I2C);
	if (!ok || end) {
		if (errors++ < 16) {
			printf("akios i2c11: %s of %u bytes at %#x failed (%s, INT_STAT %#x)\n", read ? "read" : "write", len, addr,
			    ok ? "the end" : "the FIFO", end);
		}
		return false;
	}
	return true;
}

bool akios_i2c11_read(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len);
bool akios_i2c11_write(uint8_t addr, uint8_t reg, uint8_t value);

bool
akios_i2c11_read(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len)
{
	bool ok;

	lck_mtx_lock(&akios_i2c11_lock);
	ok = bus_ready() && transfer(addr, false, &reg, 1, false) && transfer(addr, true, buf, len, true);
	lck_mtx_unlock(&akios_i2c11_lock);
	return ok;
}

bool
akios_i2c11_write(uint8_t addr, uint8_t reg, uint8_t value)
{
	uint8_t b[2] = { reg, value };
	bool ok;

	lck_mtx_lock(&akios_i2c11_lock);
	ok = bus_ready() && transfer(addr, false, b, 2, true);
	lck_mtx_unlock(&akios_i2c11_lock);
	return ok;
}
