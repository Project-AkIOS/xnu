/*
 * The Pixel 8a's USB port below the controller: what has to be up before zuma's DWC3 (hakios.akita_usb.c) can run.
 * Google's bootloader turns it off before it hands over -- the DWC3's soft reset then never finishes, it waits for the
 * PHY's clock -- so the kernel brings it up itself, the way Google's Linux drivers do at every attach:
 *
 *   1. the PHY block's three supplies (Android's tree: sub-pd-hsi0, vdd075 / vdd12 / vdd33), in that order: the main
 *      PMIC S2MPG14's LDO8M (enable in LDO_CTRL1 0x44, bits 1:0), LDO9M (L9M_CTRL 0x33, bits 7:6) and LDO19M (L19M_CTRL
 *      0x3d, bits 7:6), through ACPM; 3 on in each field is "on, normal mode". Their voltages are the bootloader's.
 *      LDO19M also feeds the eUSB2 repeater, which needs 3 ms after it before it answers.
 *   2. the eUSB2 repeater (I2C 0x3e on akios_i2c11.c's bus): out of its disabled state (GLOBAL_CONFIG 0xb2, bit 6
 *      DISABLE_P1 cleared), then the five tuning values of Android's tree (hs_tune_eusb).
 *   3. the PHY out of isolation: the PMU's USB PHY control (PMU 0x15460000 + 0x3eb0, the tree's pmu_offset; bit 0 set =
 *      isolation off), through the secure monitor's PMU call, as other PMU writes here go.
 *   4. the link (USB control block, phy@11100000's first range) and the eUSB2 PHY (its second range, 0x11110000).
 *
 * Every step looks first and writes only what is not already so: after HakiOS's U-Boot (which did all this), nothing
 * but step 4 changes, and step 4 is the same re-init the USB driver does after a replug.
 *
 * Found on the new chain (11-25, firmware -> UniXNU -> DarwinBoot): the supplies on (LDO_CTRL1 0x4f, L9M_CTRL 0xe6,
 * L19M_CTRL 0xfc), the PMU's control 0x3 (out of isolation), the repeater's port on; the eUSB2 PHY held in reset
 * (RST_CTRL 0x3). Step 4 alone is what brought USB up; steps 1 to 3 stay as checks for a bootloader that does less.
 *
 * The register facts are from Android's device tree for akita and Google's drivers (exynos-pd_hsi0, eusb_repeater,
 * phy-exynos-usbdrd-eusb, phy-exynos-eusb, s2mpg14-regulator), read as references; the code is ours.
 *
 *   void akios_usb_phy_up(void)      steps 1 to 4 (the USB driver, before the controller's first start)
 *   void akios_usb_phy_link(void)    step 4 alone (the USB driver, when the cable comes back)
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern int akios_pmic_read(unsigned bus, unsigned block, unsigned reg, uint8_t *value);
extern int akios_pmic_update(unsigned bus, unsigned block, unsigned reg, uint8_t mask, uint8_t value, uint8_t *was);
extern bool akios_i2c11_read(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len);
extern bool akios_i2c11_write(uint8_t addr, uint8_t reg, uint8_t value);

void akios_usb_phy_up(void);
void akios_usb_phy_link(void);

/* ---- 1. the supplies ---------------------------------------------------------------------------------------------- */

#define PMIC_MAIN               0               /* S2MPG14 */
#define PMIC_REGULATORS         1
#define S2MPG14_L8M_CTRL        0x32            /* LDO8M's voltage (its enable is in LDO_CTRL1) */
#define S2MPG14_L9M_CTRL        0x33
#define S2MPG14_L19M_CTRL       0x3d
#define S2MPG14_LDO_CTRL1       0x44

static const struct {
	const char *name;
	uint8_t reg, mask, on;
} supplies[3] = {
	{ "LDO8M (vdd075)", S2MPG14_LDO_CTRL1, 0x03, 0x03 },
	{ "LDO9M (vdd12)", S2MPG14_L9M_CTRL, 0xc0, 0xc0 },
	{ "LDO19M (vdd33)", S2MPG14_L19M_CTRL, 0xc0, 0xc0 },
};

static bool
supplies_on(void)
{
	uint8_t was[3] = { 0 }, now[3] = { 0 }, vsel8 = 0;
	bool ok = true, changed = false;

	(void)akios_pmic_read(PMIC_MAIN, PMIC_REGULATORS, S2MPG14_L8M_CTRL, &vsel8);
	for (unsigned i = 0; i < 3; i++) {
		int error = akios_pmic_update(PMIC_MAIN, PMIC_REGULATORS, supplies[i].reg, supplies[i].mask, supplies[i].on,
		    &was[i]);

		if (error != 0) {
			printf("akios usb phy: %s: the PMIC did not take it (error %d)\n", supplies[i].name, error);
			ok = false;
			break;
		}
		if ((was[i] & supplies[i].mask) != supplies[i].on) {
			changed = true;
			delay(1000);            /* (an LDO's enable time is 128 us) */
		}
	}
	for (unsigned i = 0; i < 3; i++) {
		(void)akios_pmic_read(PMIC_MAIN, PMIC_REGULATORS, supplies[i].reg, &now[i]);
	}
	printf("akios usb phy: supplies %s: LDO_CTRL1 %#x -> %#x, L9M_CTRL %#x -> %#x, L19M_CTRL %#x -> %#x (L8M_CTRL %#x)\n",
	    !ok ? "NOT ALL ON" : changed ? "turned on" : "on already", was[0], now[0], was[1], now[1], was[2], now[2], vsel8);
	if (changed) {
		delay(3000);                    /* the repeater: 3 ms from its supply before it answers */
	}
	return ok;
}

/* ---- 2. the eUSB2 repeater ---------------------------------------------------------------------------------------- */

#define REPEATER                0x3e
#define RPTR_REV_ID             0xb0
#define RPTR_GLOBAL_CONFIG      0xb2
#define RPTR_DISABLE_P1         0x40

static const uint8_t repeater_tune[5][2] = {    /* Android's hs_tune_eusb: register, value (whole bytes) */
	{ 0x50, 0x0a },                         /* eusb_mode_control */
	{ 0x70, 0x5f },                         /* usb2_tx_adjust_port1 */
	{ 0x71, 0x2e },                         /* usb2_hs_rx_pre_emphasis_p1 */
	{ 0x73, 0x80 },                         /* usb2_disconnect_squelch_port1 */
	{ 0x79, 0x40 },                         /* eusb_rx_adjust_port1 */
};

static void
repeater_on(void)
{
	uint8_t rev = 0, cfg = 0, now = 0, back[5] = { 0 };
	uint32_t tune = 1;
	unsigned tuned = 0;

	if (!akios_i2c11_read(REPEATER, RPTR_REV_ID, &rev, 1) || !akios_i2c11_read(REPEATER, RPTR_GLOBAL_CONFIG, &cfg, 1)) {
		printf("akios usb phy: the eUSB2 repeater does not answer at %#x\n", REPEATER);
		return;
	}
	if (cfg & RPTR_DISABLE_P1) {
		(void)akios_i2c11_write(REPEATER, RPTR_GLOBAL_CONFIG, (uint8_t)(cfg & ~RPTR_DISABLE_P1));
	}
	(void)akios_i2c11_read(REPEATER, RPTR_GLOBAL_CONFIG, &now, 1);
	PE_parse_boot_argn("usb_repeater_tune", &tune, sizeof(tune));
	for (unsigned i = 0; tune && i < 5; i++) {
		uint8_t v = 0;

		if (akios_i2c11_read(REPEATER, repeater_tune[i][0], &v, 1) && v != repeater_tune[i][1]) {
			(void)akios_i2c11_write(REPEATER, repeater_tune[i][0], repeater_tune[i][1]);
			tuned++;
		}
		(void)akios_i2c11_read(REPEATER, repeater_tune[i][0], &back[i], 1);
	}
	printf("akios usb phy: eUSB2 repeater (revision %#x): GLOBAL_CONFIG %#x -> %#x (%s); tuning %s (%u changed): "
	    "%#x %#x %#x %#x %#x\n", rev, cfg, now, (now & RPTR_DISABLE_P1) ? "STILL DISABLED" : "port on",
	    tune ? "Android's" : "left alone", tuned, back[0], back[1], back[2], back[3], back[4]);
}

/* ---- 3. the PMU's isolation --------------------------------------------------------------------------------------- */

#define PMU_USB_PHY             0x15463eb0ULL   /* system-controller@15460000 + pmu_offset 0x3eb0: bit 0, 1 = isolation off */
#define TENSOR_SMC_PMU_SEC_REG  0x82000504u
#define PMU_REG_READ            0
#define PMU_REG_RMW             2

static uint64_t
smc(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = a4;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4) : :
	    "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void
isolation_off(void)
{
	uint32_t was = (uint32_t)smc(TENSOR_SMC_PMU_SEC_REG, PMU_USB_PHY, PMU_REG_READ, 0, 0), now;
	uint64_t r = 0;

	if (!(was & 1)) {
		r = smc(TENSOR_SMC_PMU_SEC_REG, PMU_USB_PHY, PMU_REG_RMW, 1, 1);
	}
	now = (uint32_t)smc(TENSOR_SMC_PMU_SEC_REG, PMU_USB_PHY, PMU_REG_READ, 0, 0);
	printf("akios usb phy: PMU USB PHY control %#x -> %#x (%s)%s\n", was, now, (was & 1) ? "out of isolation already" :
	    "taken out of isolation", r != 0 ? ": THE SECURE MONITOR REFUSED" : "");
}

/* ---- 4. the link and the eUSB2 PHY -------------------------------------------------------------------------------- */

#define USBCON_PHYS             0x11100000UL
#define EUSB_PHYS               0x11110000UL
#define USBCON_LINKCTRL         0x04
#define USBCON_LINK_CLKRST      0x0c
#define USBCON_UTMI_CTRL        0x10
#define EUSB_RST_CTRL           0x00
#define EUSB_CMN_CTRL           0x04
#define EUSB_PLLCFG0            0x08
#define EUSB_PLLCFG1            0x0c
#define EUSB_TXTUNE             0x14
#define EUSB_TESTSE             0x20
static vm_offset_t usbcon_regs, eusb_regs;

static void
clrset(vm_offset_t base, uint32_t off, uint32_t clr, uint32_t set)
{
	volatile uint32_t *r = (volatile uint32_t *)(base + off);

	*r = (*r & ~clr) | set;
}

void
akios_usb_phy_link(void)
{
	if (usbcon_regs == 0) {
		usbcon_regs = ml_io_map(USBCON_PHYS, 0x1000);
		eusb_regs = ml_io_map(EUSB_PHYS, 0x1000);
		if (usbcon_regs == 0 || eusb_regs == 0) {
			printf("akios usb phy: could not map the link or the PHY\n");
			usbcon_regs = 0;
			return;
		}
	}
	printf("akios usb phy: before: RST_CTRL %#x CMN_CTRL %#x LINKCTRL %#x UTMI_CTRL %#x\n",
	    *(volatile uint32_t *)(eusb_regs + EUSB_RST_CTRL), *(volatile uint32_t *)(eusb_regs + EUSB_CMN_CTRL),
	    *(volatile uint32_t *)(usbcon_regs + USBCON_LINKCTRL), *(volatile uint32_t *)(usbcon_regs + USBCON_UTMI_CTRL));
	/* the link: Q-channel off, filters, a link reset, VBUS and B-session forced valid */
	clrset(usbcon_regs, USBCON_LINKCTRL, 1u << 8, 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12);
	delay(500);
	clrset(usbcon_regs, USBCON_LINKCTRL, 0xf0, 1u << 8 | 0xf0);
	clrset(usbcon_regs, USBCON_LINK_CLKRST, 0, 1);
	delay(10);
	clrset(usbcon_regs, USBCON_LINK_CLKRST, 1, 0);
	clrset(usbcon_regs, USBCON_UTMI_CTRL, 0, 3);
	/* the eUSB2 PHY: held in reset, repeater mode at 19.2 MHz (PLL feedback 368), out of IDDQ, enabled, out of reset */
	clrset(eusb_regs, EUSB_RST_CTRL, 0, 1u << 0 | 1u << 1 | 1u << 4 | 1u << 5);
	clrset(eusb_regs, EUSB_CMN_CTRL, 7u << 4, 1u << 10);
	clrset(eusb_regs, EUSB_PLLCFG0, 0xfffu << 8, 368u << 8);
	clrset(eusb_regs, EUSB_PLLCFG1, 0xfu << 8, 0);
	clrset(eusb_regs, EUSB_TXTUNE, 3u << 1, 0);
	clrset(eusb_regs, EUSB_TESTSE, 1u << 6, 0);
	clrset(eusb_regs, EUSB_CMN_CTRL, 1u << 0, 0);
	delay(10);
	clrset(eusb_regs, EUSB_RST_CTRL, 1u << 0, 0);
	delay(10);
	clrset(eusb_regs, EUSB_CMN_CTRL, 0, 1u << 0);
	delay(3600);
	clrset(eusb_regs, EUSB_RST_CTRL, 1u << 4 | 1u << 5, 0);
	printf("akios usb phy: the eUSB2 link made: RST_CTRL %#x CMN_CTRL %#x LINKCTRL %#x UTMI_CTRL %#x\n",
	    *(volatile uint32_t *)(eusb_regs + EUSB_RST_CTRL), *(volatile uint32_t *)(eusb_regs + EUSB_CMN_CTRL),
	    *(volatile uint32_t *)(usbcon_regs + USBCON_LINKCTRL), *(volatile uint32_t *)(usbcon_regs + USBCON_UTMI_CTRL));
}

void
akios_usb_phy_up(void)
{
	if (supplies_on()) {
		repeater_on();
	}
	isolation_off();
	akios_usb_phy_link();
}
