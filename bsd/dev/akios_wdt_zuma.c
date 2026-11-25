/*
 * The watchdogs of zuma's CPU clusters (Android's tree: watchdog_cl0@10060000, watchdog_cl1@10070000; Samsung's
 * watchdog: WTCON, WTDAT the reload value, WTCNT the counter, 16 bits). Google's bootloader leaves one counting for the
 * kernel -- about two minutes -- and AkIOS's firmware leaves it so as its own failsafe. Here every one found counting
 * is made as slow as it goes (prescaler 255, divider 128, reload 0xffff: about two and a half minutes, measured with
 * U-Boot's net) and fed every 10 s from a thread call: a healthy kernel runs as long as it likes, a hung one is
 * restarted by the watchdog. One found stopped is left stopped (its reset path in the PMU is the bootloader's to
 * arm; nothing here arms one).
 *
 * Found on the new chain (11-25): both counting -- cl0 WTCON 0x1af39 (prescaler 0xaf), cl1 WTCON 0x18021 (prescaler
 * 0x80, reload 0x8000) -- and both fed from here; the phone ran past the old two-minute restart.
 *
 * Boot-args: wdt=0 stops the counting ones instead (no net); wdt_starve=N stops feeding after N seconds of uptime (a
 * test: the phone should restart about 150 s later), as the loader-given watchdog's feeder (loader_return.c) does.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define WTCON                   0x00
#define WTDAT                   0x04
#define WTCNT                   0x08
#define WTCON_RSTEN             (1u << 0)
#define WTCON_ENABLE            (1u << 5)
#define WTCON_DIV_MASK          (3u << 3)
#define WTCON_DIV128            (3u << 3)
#define WTCON_PRESCALE_MASK     (0xffu << 8)
#define WTCON_PRESCALE_MAX      (0xffu << 8)
#define WTCNT_MAX               0xffff
#define FEED_SECONDS            10

static const struct {
	const char *name;
	uint64_t phys;
} wdts[2] = {
	{ "cl0", 0x10060000ULL },
	{ "cl1", 0x10070000ULL },
};
static vm_offset_t fed[2];
static uint32_t starve_s;
static thread_call_t feed_call;

static inline uint32_t rd(vm_offset_t base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr(vm_offset_t base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }

static void
akios_wdt_feed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t up_ns, deadline;

	absolutetime_to_nanoseconds(mach_absolute_time(), &up_ns);
	if (starve_s != 0 && up_ns >= (uint64_t)starve_s * NSEC_PER_SEC) {
		printf("akios wdt: wdt_starve=%u: no longer fed\n", starve_s);
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		if (fed[i] != 0) {
			wr(fed[i], WTCNT, rd(fed[i], WTDAT));
		}
	}
	clock_interval_to_deadline(FEED_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(feed_call, deadline);
}

__startup_func
static void
akios_wdt_init(void)
{
	uint32_t keep = 1;
	unsigned feeding = 0;

	PE_parse_boot_argn("wdt", &keep, sizeof(keep));
	PE_parse_boot_argn("wdt_starve", &starve_s, sizeof(starve_s));
	for (unsigned i = 0; i < 2; i++) {
		vm_offset_t w = ml_io_map(wdts[i].phys, 0x1000);
		uint32_t con, dat, cnt;

		if (w == 0) {
			continue;
		}
		con = rd(w, WTCON);
		dat = rd(w, WTDAT);
		cnt = rd(w, WTCNT);
		if (!(con & WTCON_ENABLE)) {
			printf("akios wdt: %s stopped (WTCON %#x): left so\n", wdts[i].name, con);
			continue;
		}
		if (!keep) {
			wr(w, WTCON, con & ~(WTCON_ENABLE | WTCON_RSTEN));
			printf("akios wdt: %s was counting (WTCON %#x WTDAT %#x WTCNT %#x): stopped (wdt=0)\n", wdts[i].name,
			    con, dat, cnt);
			continue;
		}
		wr(w, WTDAT, WTCNT_MAX);
		wr(w, WTCNT, WTCNT_MAX);
		wr(w, WTCON, (con & ~(WTCON_PRESCALE_MASK | WTCON_DIV_MASK)) | WTCON_PRESCALE_MAX | WTCON_DIV128);
		wr(w, WTCNT, WTCNT_MAX);
		fed[i] = w;
		feeding++;
		printf("akios wdt: %s was counting (WTCON %#x WTDAT %#x WTCNT %#x): now WTCON %#x WTDAT %#x, fed every %u s\n",
		    wdts[i].name, con, dat, cnt, rd(w, WTCON), rd(w, WTDAT), FEED_SECONDS);
	}
	if (feeding != 0) {
		feed_call = thread_call_allocate(akios_wdt_feed, NULL);
		akios_wdt_feed(NULL, NULL);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, akios_wdt_init);
