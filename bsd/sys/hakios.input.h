/*
 * /dev/hakios-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * Darwin expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_HAKIOS_INPUT_H_
#define _SYS_HAKIOS_INPUT_H_

#include <stdint.h>

#define HAKIOS_TOUCH_DEVICE    "/dev/hakios-touch"

#define HAKIOS_TOUCH_DOWN      1       /* a finger has landed */
#define HAKIOS_TOUCH_MOVE      2
#define HAKIOS_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define HAKIOS_BUTTON_DOWN     4       /* a hardware button; `finger` says which (HAKIOS_BUTTON_*), x and y are 0 */
#define HAKIOS_BUTTON_UP       5

#define HAKIOS_BUTTON_LOCK     1       /* the side button */
#define HAKIOS_BUTTON_VOLUME_UP 2
#define HAKIOS_BUTTON_VOLUME_DOWN 3
#define HAKIOS_BUTTON_HOME     4

struct hakios_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* HAKIOS_TOUCH_* or HAKIOS_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void hakios_input_init(void);
void hakios_ufs_init(void);           /* ufs=2 to xnuloader: the phone's storage, read only, through the loader (ufs_shared_pa=) */
void hakios_ufs_report(const char *when); /* where reading the storage has spent its time so far */
bool hakios_ufs_dev(dev_t dev);           /* one of the storage's devices */
int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes); /* for drivers */
/* a file of one of the RAM disks (drivers/storage/hakios.image.c): fw/<name> is the firmware userland/build.sh put there */
const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
void hakios_battery_init(void);        /* boot-arg battery=1: Qualcomm UEFI charger state shared by xnuloader */
int hakios_battery_temperature(void);  /* the battery's temperature in C, -273 while it is not known */
/*
 * How the battery is, as the battery's driver says (hakios.akita_battery.c on the Pixel 8a):
 * handed to drivers/power/hakios.battery.c, which keeps it for the sysctls kern.hakios.battery_* and
 * passes it on to drivers/power/hakios.battery.cpp, the power source that Darwin reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct hakios_battery_state {
	uint32_t size;                  /* sizeof(struct hakios_battery_state) */
	int32_t percent_hundredths;     /* the charge, 0..10000 */
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if a charger or a computer is on the USB port */
	int32_t status;                 /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	int32_t charge_uah, full_uah, design_uah, cycles;      /* 0 when not known (yet) */
	int32_t usb_type;               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 a computer's port, 2 a charger, ... */
	int32_t usb_voltage_uv, usb_current_ua;                 /* what the port gives */
};
void hakios_battery_report(const struct hakios_battery_state *state);
void hakios_net_init(void);            /* en0, if there is something to carry its frames: sys/hakios.net.h */
int hakios_wifi_tell(const char *format, ...) __printflike(1, 2);     /* a line of the Wi-Fi station's, always said */
extern void (*hakios_touch_interaction)(void);  /* called at each touch down and move (a device's governor), or NULL */
#endif

#endif /* _SYS_HAKIOS_INPUT_H_ */
