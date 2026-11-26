/*
 * Bluetooth, the half that is the same on every phone: what iOS's bluetoothd looks for and talks to -- the kernel
 * control com.apple.uart.sk.bluetooth and the Skywalk kernel pipe behind it (hakios.btsk.c), the nodes /dev/btwake,
 * /dev/btreset and /dev/btpoweroff -- and one thread that switches the controller on, off and over again and carries
 * whole H4 packets between bluetoothd and the controller, changing the few answers Apple's stack will not take.
 *
 * (11-26) Presented the way a UART iPhone presents it: the device tree's /arm-io/bluetooth (tools/ak/adt.py: transport
 * H4BC, the address) is the service bluetoothd and BlueTool look up by name, AppleBluetoothModule on it
 * (iokit/Kernel/AppleBluetoothModule.cpp) is the power, open, power-cycle and address user client they call, and
 * AppleOnboardSerialBSDClient below it publishes the kernel control and the pipe as soon as BSD is up
 * (hakios_bthost_publish), long before the controller is: a connection made early waits in skel_ctl_connect until the
 * controller answers. The old platform-attached "bluetooth" service (hakios.bluetooth.cpp, once upstream's
 * RedwoodBluetooth.cpp) is gone.
 *
 * From upstream's branch mtk (redwood_mtkbt_skel.c, 2026-11-10: the skeleton of its MediaTek driver, proven there on
 * the Infinix Hot 20's MT6631 as part of redwood_connsys.c; docs/bluetooth-mtk-porting.md there says how each part was
 * found). Changed here: the phone's operations are not a table to fill in but given by the phone's driver
 * (hakios_bthost_start: devices/akita/drivers/bluetooth for the Pixel 8a's BCM4383), and the log lines go to the
 * kernel log. The changed answers (skel_quirks) are the MediaTek controller's and fire only on its exact answers; a
 * Broadcom controller answers Apple's vendor commands itself, or not: bluetoothd's own log says where it stops.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/sys_domain.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <uuid/uuid.h>

extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);

extern void hakios_btsk_init(void);                            /* hakios.btsk.c: the Skywalk kernel pipe bluetoothd's packets go through */
extern bool hakios_btsk_uuid(uuid_t out);
extern void hakios_btsk_deliver(const uint8_t *bytes, unsigned length);
void hakios_bthost_written(const uint8_t *bytes, unsigned length);    /* called by hakios.btsk.c with what bluetoothd wrote */

#define say(fmt, ...) printf("bthost: " fmt "\n", ##__VA_ARGS__)

#define SKEL_TXQ                8192u           /* what bluetoothd has written and the controller has not been given yet */
#define SKEL_FRAME              4096u           /* one packet from the controller */
#define SKEL_SAID_PACKETS       400u            /* of each direction, said in the log: the host buffer is 64 KB and bluetoothd's own lines want it */
extern uint32_t skel_said_in, skel_said_out;
#define SKEL_ANSWER_MS          2000u           /* a command of the probe is given this long */

/*
 * The operations of one phone. All but the two marked optional are required (the skeleton checks). Each returns 0 or an errno unless it says
 * otherwise, and says (the kernel log) what it is about to do BEFORE a step that could hang a bus: the last line of a log that ends
 * is the step that hung. All of them are called from one thread, the skeleton's, and never two at once.
 */
struct hakios_bt_platform {
	const char      *name;                  /* "mt6768 / x6826b / mt6631": what the log calls it */
	/*
	 * The combo chip up to the point where it takes function commands: rails, clocks, reset, the firmware patch, its start-up script (WMT on
	 * a CONNSYS SoC: redwood_connsys.c's steps 1 to 11). The same chip is Wi-Fi's: if the Wi-Fi driver has already done it, wait for it
	 * here and return 0.
	 */
	int     (*power_on)(void);
	/*
	 * Bluetooth's function on, and off. On a CONNSYS SoC it is one WMT command (WMT_FUNC_CTRL: 01 06 02 00 <0 = BT> <1 = on>, answered
	 * 02 06 01 00 00; the firmware's own events that come before the answer are not it). When function_on returns 0 the controller takes HCI.
	 */
	int     (*function_on)(void);
	void    (*function_off)(void);
	/*
	 * One whole H4 packet to the controller: the type byte (1 command, 2 ACL, 3 SCO, 5 ISO), then the HCI packet. On a combo chip behind
	 * STP that is the payload of one STP frame on channel 0 (BT_TASK_INDX).
	 */
	int     (*send)(const uint8_t *h4, unsigned length);
	/*
	 * One whole H4 packet from the controller into h4 (type byte 4 event, 2 ACL, 3 SCO, 5 ISO): its length, 0 if none came within wait_ms,
	 * less than 0 for an error. Never half a packet and never two; what is not Bluetooth's (another STP channel) is not returned.
	 */
	int     (*receive)(uint8_t *h4, unsigned room, unsigned wait_ms);
	/* optional: called before each send, for a transport that sleeps (BTIF's wake on the Infinix) */
	void    (*wake)(void);
	/*
	 * (11-26: the Wi-Fi address the registry's local-mac-address was made of is gone from here: the address is the device
	 * tree's, /arm-io/bluetooth's local-mac-address, = /chosen/mac-address-bluetooth0, both from tools/ak/adt.py)
	 */
	/*
	 * true: the phone's driver calls hakios_bthost_wake() when bytes have come (its interrupt), and receive with wait_ms 0
	 * returns at once. The bridge then sleeps until bluetoothd writes or the controller speaks, instead of asking every
	 * 2 ms for ever. false: polled, as upstream's.
	 */
	bool    wakes;
};

/* the phone's operations: given by its driver (hakios_bthost_start) */
static struct hakios_bt_platform mtkbt_platform;

/* ---- the platform check ---- */
static bool
skel_platform_complete(const struct hakios_bt_platform *p)
{
	bool ok = true;

#define NEED(cond, what) do { if (!(cond)) { say("the platform lacks: %s", what); ok = false; } } while (0)
	NEED(p->name != NULL, "name");
	NEED(p->power_on != NULL, "power_on (the combo chip up to where it takes function commands)");
	NEED(p->function_on != NULL && p->function_off != NULL, "function_on, function_off (Bluetooth's function)");
	NEED(p->send != NULL, "send (one H4 packet to the controller)");
	NEED(p->receive != NULL, "receive (one H4 packet from the controller)");
#undef NEED
	return ok;
}

static uint32_t
skel_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/* ---- H4 ---- */

/* the length of the H4 packet that begins the bytes; 0 while its header is not whole or the first byte is not a packet type */
static unsigned
skel_h4_length(const uint8_t *b, unsigned have)
{
	if (have < 1) {
		return 0;
	}
	switch (b[0]) {
	case 1:                                 /* command: opcode (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 2:                                 /* ACL: handle (2), length (2) */
		return have >= 5 ? 5u + (b[3] | ((unsigned)b[4] << 8)) : 0;
	case 3:                                 /* SCO: handle (2), length (1) */
		return have >= 4 ? 4u + b[3] : 0;
	case 4:                                 /* event: code (1), length (1) */
		return have >= 3 ? 3u + b[2] : 0;
	case 5:                                 /* ISO: handle (2), length (2, 14 bits) */
		return have >= 5 ? 5u + ((b[3] | ((unsigned)b[4] << 8)) & 0x3fff) : 0;
	default:
		return 0;
	}
}

static void
skel_dump(const char *direction, const uint8_t *bytes, unsigned length, uint32_t *counter)
{
	char text[3 * 32 + 1];
	unsigned n = length < 32 ? length : 32;

	if ((*counter)++ >= SKEL_SAID_PACKETS) {
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		snprintf(text + 3 * i, 4, " %02x", bytes[i]);
	}
	text[3 * n] = 0;
	say("%s %u bytes:%s%s", direction, length, text, length > n ? " ..." : "");
}

/* ---- what bluetoothd writes: one queue, drained a whole packet at a time by the bridge ---- */
static LCK_GRP_DECLARE(skel_lck_grp, "hakios-bthost");
static LCK_MTX_DECLARE(skel_lock, &skel_lck_grp);
static uint8_t skel_txq[SKEL_TXQ];
static unsigned skel_txq_count;
static uint32_t skel_txq_dropped;               /* bytes that found the queue full */

static bool
skel_queue_locked(const uint8_t *bytes, size_t length)
{
	if (length > SKEL_TXQ - skel_txq_count) {
		skel_txq_dropped += (uint32_t)length;
		return false;
	}
	memcpy(skel_txq + skel_txq_count, bytes, length);
	skel_txq_count += (unsigned)length;
	return true;
}

/* H4 bytes the Skywalk channel's user wrote (hakios.btsk.c's sync_tx) */
static int skel_event;                          /* what the bridge sleeps on */
static volatile bool skel_pending;              /* something to do, said before the wake-up */

/* the phone's driver, from its interrupt: the controller has spoken */
void hakios_bthost_wake(void);
void
hakios_bthost_wake(void)
{
	skel_pending = true;
	wakeup((void *)&skel_event);
}

void
hakios_bthost_written(const uint8_t *bytes, unsigned length)
{
	lck_mtx_lock(&skel_lock);
	(void)skel_queue_locked(bytes, length);
	lck_mtx_unlock(&skel_lock);
	hakios_bthost_wake();
}

/*
 * ---- what this controller says of itself that Apple's stack will not take as it is ----
 * (11-26) An explicit list, no longer every vendor command. Until today every Apple vendor command (OGF 0x3f) the controller
 * did not know (Command Complete, status 1) was turned into a success with zero bytes after it; but bluetoothd takes most
 * failures cleanly -- 0xfd23 sub-command 0x0a, Read_Local_Controller_Cached_Info: _BCM_ReadLocalControllerCachedInfoEventParser
 * logs the status and calls back with it, and the caller says "Failed to read local Fast LE Connection data from controller"
 * and goes on (research of 11-26 on 24B156's bluetoothd) -- and a made-up success tells it a feature is there that is not
 * (the empty Fast LE record of 11-23 said "Fast LE v0 available"). So a failure now goes up as the controller said it,
 * except for the commands below. Checked against this file's history, command by command (11-26):
 *  - 0xfe3b, low-power core statistics (11-17): its failure, passed through, was "EnhancedProfilingStats VSC failed with
 *    status 701" and a restart -- the one failure known to be fatal; done, with the most an event carries (251 zero bytes:
 *    _BCM_LowPowerCoreStatsEventParser reads ~60 counters, 200 bytes ran out at bcm_vsc.c line 823 of 825);
 *  - 0xfce2, every sub-command (Olympic diagnostics and antenna settings, 11-16/17 on 16.6.1): what went wrong was a made-up
 *    success with too few bytes after it -- sub 0x0d's _BCM_ProfilingStatsEventParser asserted
 *    "ByteStream_NumReadBytesAvail(*pBs) >= 2", sub 0x42 and then 0x2c reported a VSC stall and restarted bluetoothd
 *    (bluetooth-olympic-vsc-stall.md); done, with 16 zero bytes (the ones with no completion of their own, 0x2a and 0x3a,
 *    ignore them). Whether bluetoothd takes their failure was never tried: the research of 11-26 names them fatal, and
 *    Bluetooth works with them done, so done they stay;
 *  - 0xfce9 sub-command 0x37 (11-17): the same story, "1, 0x37" made into a success made _BCM_U8_U8_U8EventParser assert
 *    ">= 1" on the bytes after the 0x37, ~0.6 s into every start; done, three zero bytes after the 0x37 (its failure as
 *    such never tried either);
 *  - Write_Default_Erroneous_Data_Reporting (0x0c5b), "Unknown HCI Command" as a Command Status from the MT6631 (upstream's:
 *    a Command Status where a Command Complete is waited for is a stall to bluetoothd whatever the command); done.
 * (0xfdf0 and 0xfce2 sub 0x42 are not given to the controller at all, and 0x204a neither: skel_bridge_once answers them.
 * 0xfdf0 the BCM4383 never answers, it streams a memory dump instead: 11-17.)
 * And a second group, NOT CHECKED: the vendor commands the BCM4383 refused in every iOS 27 boot so far (the kernel logs of
 * 11-24..26: "vendor command ...: the controller says it does not know it"), all answered done with 200 zero bytes by the
 * old rule, with Bluetooth working: 0xfd47, 0xfe10, 0xfe29, 0xfe2c, 0xfe37, 0xfe91; and 0xfce9's other sub-commands, whose
 * answers the old rule turned into successes without a word (0x01, 0x05, 0x1e, 0x35, 0x61 came back "done" in those logs;
 * which of them really were cannot be told from the log). Whether bluetoothd takes their failure is not known yet; they
 * stay done until it is (boot-arg bt_vsc=0: passed through like any other, to find out). Every other refused command,
 * 0xfd23 sub 0x0a first, goes up as it is: if one turns out fatal, it says so here (the log line "passed through"),
 * and bt_vsc=2 brings the old blanket rule back, every refused vendor command done, for a quick way back.
 * Each rewrite and each failure passed through is said in the kernel log once per command (and sub-command).
 *
 * The MT6631 said "unknown" the other way, a Command Status with status 1 where a Command Complete is waited for (which
 * bluetoothd takes for a stall whatever the command): for the commands of the list that form is answered the same way.
 */
struct skel_fix {
	uint16_t        opcode;
	int16_t         sub;            /* the sub-command (the first parameter), echoed after the status; -1: any */
	uint8_t         data;           /* bytes after the status in the answer made, at least (the echoed sub-command among them) */
	bool            checked;        /* found to stop bluetoothd (above); false: the iOS 27 group, not checked yet */
};

static const struct skel_fix skel_fixes[] = {
	{ 0xfce2, -1, 16, true },
	{ 0xfce9, 0x37, 4, true },
	{ 0xfe3b, -1, 251, true },
	{ 0x0c5b, -1, 0, true },
	{ 0xfd47, -1, 200, false },
	{ 0xfe10, -1, 200, false },
	{ 0xfe29, -1, 200, false },
	{ 0xfe2c, -1, 200, false },
	{ 0xfe37, -1, 200, false },
	{ 0xfe91, -1, 200, false },
	{ 0xfce9, -1, 0, false },       /* (after 0x37: the first match counts) */
};

static uint8_t skel_vsc_sub;            /* the first parameter of the last 0xfce2 command sent: its kind */
static uint32_t skel_vsc_mode = 1;      /* boot-arg bt_vsc: 0 the checked list only, 1 and the iOS 27 group, 2 every vendor command */

/* true the first time KEY (what was done, sub-command, opcode) is seen: each rewrite said once */
static bool
skel_first(uint32_t key)
{
	static uint32_t seen[96];
	static unsigned count;

	for (unsigned i = 0; i < count; i++) {
		if (seen[i] == key) {
			return false;
		}
	}
	if (count < sizeof(seen) / sizeof(seen[0])) {
		seen[count++] = key;
	}
	return true;
}

static const struct skel_fix *
skel_fix_for(uint16_t opcode, int sub)
{
	static const struct skel_fix blanket = { 0, -1, 200, false };

	for (unsigned i = 0; i < sizeof(skel_fixes) / sizeof(skel_fixes[0]); i++) {
		const struct skel_fix *f = &skel_fixes[i];

		if (f->opcode == opcode && (f->sub < 0 || f->sub == sub) && (f->checked || skel_vsc_mode >= 1)) {
			return f;
		}
	}
	return skel_vsc_mode >= 2 && (opcode >> 10) == 0x3f ? &blanket : NULL;
}

/*
 * The answer made for a command of the list: Command Complete, status 0, the bytes the controller gave after its status
 * (have of them, at from), and zeros up to f->data. Its length.
 */
static unsigned
skel_answer_done(uint8_t *packet, uint16_t opcode, const uint8_t *from, unsigned have, const struct skel_fix *f, const char *how)
{
	uint8_t kept[32];
	unsigned data;

	if (have > sizeof(kept)) {
		have = sizeof(kept);
	}
	data = have > f->data ? have : f->data;
	if (have != 0) {
		memcpy(kept, from, have);
	}
	packet[0] = 0x04;
	packet[1] = 0x0e;
	packet[2] = (uint8_t)(4 + data);
	packet[3] = 0x01;
	packet[4] = (uint8_t)opcode;
	packet[5] = (uint8_t)(opcode >> 8);
	packet[6] = 0x00;
	memset(packet + 7, 0, data);
	if (have != 0) {
		memcpy(packet + 7, kept, have);
	}
	if (skel_first(0x01000000u | ((uint32_t)(have != 0 ? kept[0] : 0) << 16) | opcode)) {
		say("vendor command %#x (sub-command %#x): the controller %s; answered as done, %u bytes after the status (%s)",
		    opcode, have != 0 ? kept[0] : 0u, how, data, f->checked ? "on the list" :
		    f->opcode == 0 ? "bt_vsc=2, every vendor command" : "the iOS 27 group, not checked");
	}
	return 7 + data;
}

static unsigned
skel_quirks(uint8_t *packet, unsigned length)
{
	static const uint8_t le_commands[4] = { 0xf7, 0xff, 0xff, 0x7f };

	/* Command Status, status 1 (the MT6631's "unknown"), for a command of the list: done */
	if (length == 7 && packet[0] == 0x04 && packet[1] == 0x0f && packet[2] == 0x04 && packet[3] == 0x01) {
		const uint16_t opcode = (uint16_t)(packet[5] | (packet[6] << 8));
		const struct skel_fix *f = skel_fix_for(opcode, opcode == 0xfce2 ? skel_vsc_sub : -1);

		if (f != NULL && f->sub < 0) {
			return skel_answer_done(packet, opcode, NULL, 0, f, "says it does not know it (Command Status)");
		}
		return length;
	}
	/* Command Complete with a failure (the BCM4383's "unknown" is status 1, then the sub-command if there was one) */
	if (length >= 7 && packet[0] == 0x04 && packet[1] == 0x0e && packet[2] == length - 3 && packet[6] != 0x00) {
		const uint16_t opcode = (uint16_t)(packet[4] | (packet[5] << 8));
		const int sub = length > 7 ? packet[7] : -1;
		const struct skel_fix *f = packet[6] == 0x01 ? skel_fix_for(opcode, sub) : NULL;

		if (f != NULL) {
			return skel_answer_done(packet, opcode, packet + 7, length - 7, f, "says it does not know it");
		}
		if ((opcode >> 10) == 0x3f && skel_first(0x02000000u | ((uint32_t)(sub & 0xff) << 16) | opcode)) {
			say("vendor command %#x%s%x: status %#x from the controller, passed through as it is", opcode,
			    sub >= 0 ? " sub-command 0x" : "", sub >= 0 ? (unsigned)sub : 0u, packet[6]);
		}
		return length;
	}
	/*
	 * A vendor command the controller knows and answers done with no data at all (0xfe3b on 16.6.1, 11-17): bluetoothd
	 * parses statistics out of several (_BCM_LowPowerCoreStatsEventParser asserted "ByteStream_NumReadBytesAvail(*pBs) >= 4"
	 * and aborted, each start ~0.6 s in): 200 zero bytes after the status, enough for each such parser; the ones that read
	 * nothing ignore them. Not 0xfce9 (data on 0xfce9 left the next command unsent, upstream's MT6631).
	 */
	if (length == 7 && packet[0] == 0x04 && packet[1] == 0x0e && packet[2] == 0x04 && (packet[5] >> 2) == 0x3f && packet[6] == 0x00 &&
	    !(packet[4] == 0xe9 && packet[5] == 0xfc)) {
		if (skel_first(0x03000000u | packet[4] | (packet[5] << 8))) {
			say("vendor command %#x: done with nothing after the status; 200 zero bytes added", packet[4] | (packet[5] << 8));
		}
		packet[2] = 4 + 200;
		memset(packet + 7, 0, 200);
		return 7 + 200;
	}
	/*
	 * Read_Local_Supported_Commands: the MT6631 left the LE commands of octets 25-28 and LE Host Support of octet 24 clear
	 * although it answers them; bluetoothd looks in the mask before it sends one and gives up ("HCI initialization failed").
	 * The bits are OR-ed in.
	 */
	if (length >= 7 + 64 && packet[0] == 0x04 && packet[1] == 0x0e && packet[2] == 0x44 && packet[4] == 0x02 && packet[5] == 0x10 && packet[6] == 0x00) {
		for (unsigned i = 0; i < 4; i++) {
			packet[7 + 25 + i] |= le_commands[i];
		}
		packet[7 + 24] |= 0x60;
	}
	return length;
}

/*
 * ---- the kernel control ----
 * bluetoothd's transport (H4BC, the only one of its table that matches here) looks up com.apple.uart.sk.bluetooth on a PF_SYSTEM socket,
 * connects a SOCK_STREAM, and asks it (getsockopt 0x40105412, 16 bytes) for the UUID of the Skywalk channel its packets go through.
 */
static kern_ctl_ref skel_ctl_ref;

/* the controller's state, the bridge thread's to change (skel_thread) */
enum { SKEL_OFF, SKEL_STARTING, SKEL_ON, SKEL_FAILED };
static volatile int skel_state = SKEL_OFF;
#define SKEL_CONNECT_WAIT_S     120u            /* the longest a connection waits for the controller (the inject's two minutes) */

/*
 * (11-26) The control is there from BSD's start (hakios_bthost_publish), the controller only once its firmware runs (~30 s,
 * hakios.akita_bt.c). A connection made in between -- launchd starts bluetoothd for SpringBoard within seconds -- waits
 * here until the controller answers (or has failed, or two minutes have gone by), with bluetoothd's start held as the
 * injected wait in bluetoothd held it until today: a bluetoothd that opened the transport and got no answer died
 * "reason 1201" and its successors aborted (11-16). STOPGAP-ish: on a UART iPhone the controller is up before bluetoothd
 * runs (BlueTool boots it from bluetoothd's own request, our BlueTool stand-in does not), so nothing there needs to wait.
 */
static void
skel_wait_for_controller(const char *who)
{
	uint32_t since = skel_ms();

	if (skel_state == SKEL_ON || skel_state == SKEL_FAILED) {
		return;
	}
	say("control: %s connects before the controller is up: waiting for it", who);
	while (skel_state != SKEL_ON && skel_state != SKEL_FAILED && skel_ms() - since < SKEL_CONNECT_WAIT_S * 1000u) {
		assert_wait_timeout((event_t)&skel_state, THREAD_ABORTSAFE, 500, NSEC_PER_MSEC);
		if (skel_state == SKEL_ON || skel_state == SKEL_FAILED) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_INTERRUPTED) {
			break;
		}
	}
	say("control: %s waited %u ms: the controller is %s", who, skel_ms() - since,
	    skel_state == SKEL_ON ? "up" : skel_state == SKEL_FAILED ? "NOT up (failed)" : "still not up; going on anyway");
}

static errno_t
skel_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	char name[32] = "";

	proc_selfname(name, sizeof(name));
	say("control connected by %s (pid %d): unit %u", name, proc_selfpid(), sac->sc_unit);
	skel_wait_for_controller(name);
	/* (11-23) each bluetoothd's own exchange said from its start: a crash-looping one's last commands were past the limit */
	skel_said_in = skel_said_out = 0;
	return 0;
}

static errno_t
skel_ctl_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	say("control: unit %u disconnected", unit);
	return 0;
}

static errno_t
skel_ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	errno_t error = 0;

	lck_mtx_lock(&skel_lock);
	for (mbuf_t part = m; part != NULL; part = mbuf_next(part)) {
		if (!skel_queue_locked(mbuf_data(part), mbuf_len(part))) {
			error = ENOBUFS;
			break;
		}
	}
	lck_mtx_unlock(&skel_lock);
	mbuf_freem(m);
	hakios_bthost_wake();
	return error;
}

static errno_t
skel_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, __unused void *data, size_t len)
{
	say("control: setsockopt %#x, %zu bytes: taken, nothing done", (unsigned)opt, len);
	return 0;
}

static errno_t
skel_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	uuid_t uuid;
	bool have = (unsigned)opt == 0x40105412u && data != NULL && *len >= sizeof(uuid_t) && hakios_btsk_uuid(uuid);

	say("control: getsockopt %#x, room for %zu bytes: %s", (unsigned)opt, *len, have ? "the channel's UUID" : "nothing");
	if (have) {
		memcpy(data, uuid, sizeof(uuid_t));
		*len = sizeof(uuid_t);
	} else {
		*len = 0;
	}
	return 0;
}

static bool
skel_ctl_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;       /* (a datagram socket: "Protocol wrong type for socket") */
	reg.ctl_sendsize = 65536;
	reg.ctl_recvsize = 65536;
	reg.ctl_connect = skel_ctl_connect;
	reg.ctl_disconnect = skel_ctl_disconnect;
	reg.ctl_send = skel_ctl_send;
	reg.ctl_setopt = skel_ctl_setopt;
	reg.ctl_getopt = skel_ctl_getopt;
	error = ctl_register(&reg, &skel_ctl_ref);
	say("kernel control com.apple.uart.sk.bluetooth %s (%d)", error == 0 ? "registered" : "NOT REGISTERED", error);
	return error == 0;
}

/*
 * ---- the nodes ----
 * /dev/btwake, /dev/btreset, /dev/btpoweroff: bluetoothd opens btwake again and again, BlueTool the others; on Apple's hardware they are pins.
 * Nothing is behind them here: opened, read as empty, written and forgotten, the terminal ioctls taken. (The working driver also makes
 * /dev/cu.bluetooth and /dev/uart.bluetooth, the serial ports of the plain H4 transport; bluetoothd 16.5 does not open them, so they are not here.)
 */
static int
skel_node_open(dev_t dev, int flags, __unused int type, __unused struct proc *p)
{
	static uint32_t said;
	char name[32] = "";

	if (said < 8) {
		said++;
		proc_selfname(name, sizeof(name));
		say("node %u opened by %s (pid %d) with flags %#x", minor(dev), name, proc_selfpid(), flags);
	}
	return 0;
}

static int
skel_node_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
skel_node_read(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return 0;
}

static int
skel_node_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	static uint32_t said;
	uint8_t first = 0;
	user_ssize_t n = uio_resid(uio);

	if (n > 0 && uiomove((caddr_t)&first, 1, uio) == 0 && said < 8) {
		said++;
		say("node %u written %lld byte(s), the first %02x: nothing is behind it", minor(dev), (long long)n, first);
	}
	uio_setresid(uio, 0);
	return 0;
}

static int
skel_node_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	static u_long said[16];
	static unsigned said_count;
	bool seen = false;
	int result = 0;

	switch (cmd) {
	case TIOCGETA: {
		struct termios *t = (struct termios *)(void *)data;

		bzero(t, sizeof(*t));
		t->c_cflag = CS8 | CREAD | CLOCAL | HUPCL;
		t->c_ispeed = t->c_ospeed = 3000000;
		break;
	}
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF: case TIOCEXCL: case TIOCNXCL: case TIOCSDTR: case TIOCCDTR:
	case TIOCMBIS: case TIOCMBIC: case TIOCMSET: case TIOCFLUSH: case TIOCSBRK: case TIOCCBRK: case TIOCDRAIN:
	case FIONBIO: case FIOASYNC:
		break;
	case TIOCMGET:
		*(int *)(void *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR | TIOCM_RTS | TIOCM_DTR;
		break;
	case FIONREAD:
		*(int *)(void *)data = 0;
		break;
	default:
		result = ENOTTY;
		break;
	}
	for (unsigned i = 0; i < said_count; i++) {
		seen |= said[i] == cmd;
	}
	if (!seen && said_count < sizeof(said) / sizeof(said[0])) {
		said[said_count++] = cmd;
		say("node %u: ioctl %#lx: %s", minor(dev), cmd, result == 0 ? "taken" : "not a command of this node (ENOTTY)");
	}
	return result;
}

static int
skel_node_select(__unused dev_t dev, __unused int which, __unused void *wql, __unused struct proc *p)
{
	return 1;
}

static const struct cdevsw skel_cdevsw = {
	.d_open = skel_node_open, .d_close = skel_node_close, .d_read = skel_node_read, .d_write = skel_node_write,
	.d_ioctl = skel_node_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = skel_node_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
skel_nodes(void)
{
	static const char *const names[] = { "btwake", "btreset", "btpoweroff" };
	int major = cdevsw_add(-1, &skel_cdevsw);
	bool made = major >= 0;

	for (unsigned i = 0; made && i < sizeof(names) / sizeof(names[0]); i++) {
		made = devfs_make_node(makedev(major, i), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "%s", names[i]) != NULL;
	}
	say("/dev/btwake, /dev/btreset, /dev/btpoweroff %s (major %d)", made ? "made" : "NOT MADE", major);
	return made;
}

/* ---- the controller ---- */
static uint8_t skel_frame[SKEL_FRAME];
static uint8_t skel_packet[SKEL_TXQ];
uint32_t skel_said_in, skel_said_out;     /* (not static: reset at each control connection, above) */

/*
 * One packet from the controller, checked: its length is the one its own header says. Anything else is not published (the stack above would
 * lose its place in the stream) and is said, the first few times. 0 if none, less than 0 for the platform's error.
 */
static int
skel_receive(unsigned wait_ms)
{
	static uint32_t said_bad;
	int length = mtkbt_platform.receive(skel_frame, SKEL_FRAME - 64, wait_ms);     /* (64: room for what skel_quirks adds) */

	if (length <= 0) {
		return length;
	}
	if ((unsigned)length > SKEL_FRAME - 64 || skel_h4_length(skel_frame, (unsigned)length) != (unsigned)length) {
		if (said_bad < 8) {
			said_bad++;
			say("receive gave %d bytes that are not one H4 packet (type %02x, its header says %u): dropped", length, skel_frame[0],
			    (unsigned)length <= SKEL_FRAME - 64 ? skel_h4_length(skel_frame, (unsigned)length) : 0);
		}
		return 0;
	}
	return length;
}

/* A command of the probe: sent, and answered by a Command Complete of its opcode with status 0 within SKEL_ANSWER_MS. */
static bool
skel_hci(const char *what, uint16_t opcode)
{
	const struct hakios_bt_platform *p = &mtkbt_platform;
	const uint8_t command[4] = { 0x01, (uint8_t)opcode, (uint8_t)(opcode >> 8), 0x00 };
	uint32_t since = skel_ms();
	int error, length;

	if (p->wake != NULL) {
		p->wake();
	}
	skel_dump("out", command, sizeof(command), &skel_said_out);
	if ((error = p->send(command, sizeof(command))) != 0) {
		say("%s: send failed (%d)", what, error);
		return false;
	}
	while (skel_ms() - since < SKEL_ANSWER_MS) {
		if ((length = skel_receive(10)) < 0) {
			say("%s: receive failed (%d)", what, length);
			return false;
		}
		if (length == 0) {
			continue;
		}
		skel_dump("in ", skel_frame, (unsigned)length, &skel_said_in);
		if (length >= 7 && skel_frame[0] == 0x04 && skel_frame[1] == 0x0e && skel_frame[4] == command[1] && skel_frame[5] == command[2]) {
			if (skel_frame[6] != 0) {
				say("%s: the controller's status is %#x", what, skel_frame[6]);
			}
			return skel_frame[6] == 0;
		}
	}
	say("%s: no Command Complete in %u ms", what, SKEL_ANSWER_MS);
	return false;
}

/*
 * One round of the bridge: what is written is sent a packet at a time; what comes is given to the Skywalk channel. With a
 * controller that wakes the bridge (wakes), it sleeps here until either happens, a second at the most. (11-26: one round
 * at a time, no longer a loop of its own, so that the thread can switch the controller off and on between two rounds.)
 */
static void
skel_bridge_once(void)
{
	const struct hakios_bt_platform *p = &mtkbt_platform;
	static uint32_t pending_since = 0, errors = 0;
	static bool said_pending = false;

	{
		int length;

		for (;;) {
			unsigned n, have;
			int error;

			lck_mtx_lock(&skel_lock);
			have = skel_txq_count;
			if (have > 0 && (skel_txq[0] < 1 || skel_txq[0] > 5 || skel_txq[0] == 4)) {
				/* not a packet the host sends: the stream has lost its framing and nothing after it can be trusted */
				say("%u bytes written begin with %02x, not an H4 packet type: all dropped", have, skel_txq[0]);
				skel_txq_count = 0;
				lck_mtx_unlock(&skel_lock);
				break;
			}
			n = skel_h4_length(skel_txq, have);
			if (n == 0 || n > have) {
				if (n > SKEL_TXQ) {
					say("a packet of %u bytes is longer than the queue (%u): %u bytes dropped", n, SKEL_TXQ, have);
					skel_txq_count = 0;
				}
				lck_mtx_unlock(&skel_lock);
				break;
			}
			memcpy(skel_packet, skel_txq, n);
			memmove(skel_txq, skel_txq + n, have - n);
			skel_txq_count = have - n;
			lck_mtx_unlock(&skel_lock);
			if (n >= 5 && skel_packet[0] == 1 && skel_packet[1] == 0xe2 && skel_packet[2] == 0xfc) {
				skel_vsc_sub = skel_packet[4];
			}
			skel_dump("out", skel_packet, n, &skel_said_out);
			/*
			 * LE_Read_Periodic_Advertiser_List_Size (0x204a) is not given to the controller: it is one of the EXTENDED
			 * advertising commands, and a controller that keeps to the specification refuses the legacy ones after it
			 * (Command Disallowed, 0x0c, to LE_Set_Scan_Parameters 0x200b -- at which bluetoothd restarts, every ten
			 * seconds: the Pixel 8a's BCM4383, 11-15). bluetoothd asks it once and then uses the legacy commands, which
			 * Apple's controllers take. Answered here: done, a list of 6 (what this controller said).
			 */
			if (n == 4 && skel_packet[0] == 1 && skel_packet[1] == 0x4a && skel_packet[2] == 0x20) {
				static const uint8_t answer[8] = { 0x04, 0x0e, 0x05, 0x01, 0x4a, 0x20, 0x00, 0x06 };

				memcpy(skel_frame, answer, sizeof(answer));
				skel_dump("in (ours)", skel_frame, sizeof(answer), &skel_said_in);
				hakios_btsk_deliver(skel_frame, sizeof(answer));
				continue;
			}
			/*
			 * 0xfce2 sub-command 0x42 (BD_VSC_OLYMPIC_SET_DIAG_DEBUG_EVENT_BEHAVIOR) is not given to the controller.
			 * 16.6.1's bluetoothd (unlike 16.7.16's, where BT worked) requires its Command Complete to carry at least
			 * two bytes of return data -- its parser (bluetoothd __text+0x12bf94: ldrh len, sub read-offset, cmp #1,
			 * b.gt) reports VSCommandStall and restarts every ~16 s otherwise ("SET_DIAG_DEBUG_EVENT_BEHAVIOR timedout
			 * waiting for controller"). The BCM4383 answers Apple's vendor commands "unknown" with no data, so answer
			 * it here as done with 16 bytes of zero data (the handler forwards the bytes to its completion without
			 * requiring particular values). Answered on the TX path like 0x204a, so no fold into skel_quirks' bitmask.
			 */
			if (n >= 5 && skel_packet[0] == 1 && skel_packet[1] == 0xe2 && skel_packet[2] == 0xfc && skel_packet[4] == 0x42) {
				static const uint8_t diag[7] = { 0x04, 0x0e, 0x14, 0x01, 0xe2, 0xfc, 0x00 };

				memcpy(skel_frame, diag, sizeof(diag));
				memset(skel_frame + sizeof(diag), 0, 16);
				skel_dump("in (ours)", skel_frame, sizeof(diag) + 16, &skel_said_in);
				hakios_btsk_deliver(skel_frame, sizeof(diag) + 16);
				continue;
			}
			/*
			 * 0xfdf0 (BD_VSC_OLYMPIC_READ_ENHANCED_PROFILING_STATS, 11-17) is not given to the controller either: the
			 * BCM4383 answers it with a vendor event (0xff 0x1b) and then streams what looks like a memory dump (0xff 0x57,
			 * ~1 MB), never a Command Complete -- bluetoothd waits 15 s ("timedout waiting for controller"), says the
			 * firmware crashed and restarts (reason 3602), and the controller answers nothing more, not even a Reset.
			 * Answered here as done with the most an event carries, 251 zero bytes (no counters).
			 */
			if (n >= 4 && skel_packet[0] == 1 && skel_packet[1] == 0xf0 && skel_packet[2] == 0xfd) {
				static const uint8_t stats[7] = { 0x04, 0x0e, 0xff, 0x01, 0xf0, 0xfd, 0x00 };

				memcpy(skel_frame, stats, sizeof(stats));
				memset(skel_frame + sizeof(stats), 0, 251);
				skel_dump("in (ours)", skel_frame, sizeof(stats) + 251, &skel_said_in);
				hakios_btsk_deliver(skel_frame, sizeof(stats) + 251);
				continue;
			}
			if (p->wake != NULL) {
				p->wake();
			}
			if ((error = p->send(skel_packet, n)) != 0 && errors++ < 8) {
				say("send of %u bytes failed (%d)", n, error);
			}
		}
		{
			/* bytes written and not a whole packet for 2 s: said once (a writer that stopped half-way, or a length this cannot frame) */
			unsigned pending;
			uint32_t dropped;

			lck_mtx_lock(&skel_lock);
			pending = skel_txq_count;
			dropped = skel_txq_dropped;
			skel_txq_dropped = 0;
			lck_mtx_unlock(&skel_lock);
			if (dropped != 0) {
				say("the queue was full: %u bytes from bluetoothd were dropped", dropped);
			}
			if (pending == 0) {
				pending_since = 0;
			} else if (pending_since == 0) {
				pending_since = skel_ms() | 1;
			} else if (!said_pending && skel_ms() - pending_since > 2000) {
				said_pending = true;
				say("%u bytes written and not a whole H4 packet in 2 s", pending);
			}
		}
		length = skel_receive(p->wakes ? 0 : 2);
		if (length > 0) {
			length = (int)skel_quirks(skel_frame, (unsigned)length);
			skel_dump("in ", skel_frame, (unsigned)length, &skel_said_in);
			hakios_btsk_deliver(skel_frame, (unsigned)length);
		} else if (length < 0) {
			if (errors++ < 8) {
				say("receive failed (%d)", length);
			}
			delay_for_interval(1, NSEC_PER_MSEC);
		} else if (p->wakes) {
			/*
			 * Nothing came and (above) nothing whole was written: asleep until either happens. The wait is declared
			 * before the look, so a wake-up between the two is not lost; a second at the most, for a packet half written.
			 */
			assert_wait_timeout((event_t)&skel_event, THREAD_UNINT, 1000, NSEC_PER_MSEC);
			if (skel_pending) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				(void)thread_block(THREAD_CONTINUE_NULL);
			}
			skel_pending = false;
		}
	}
}

/* the bridge thread's state changed: said to whoever waits for it (skel_wait_for_controller) */
static void
skel_set_state(int state)
{
	skel_state = state;
	wakeup((event_t)&skel_state);
}

/*
 * What AppleBluetoothModule's user client and the phone's driver ask of the controller (hakios_bthost_power): power on or
 * off, a power cycle. Done by the bridge thread between two rounds, since the platform's operations are never called from
 * two threads; the asker waits for skel_done to reach its number.
 */
static LCK_MTX_DECLARE(skel_ask_lock, &skel_lck_grp);
static volatile bool skel_want_on;
static volatile uint32_t skel_cycles_asked, skel_cycles_done;
static volatile uint32_t skel_asked, skel_done;
static bool skel_thread_started, skel_dead;

/* the controller switched on, its firmware loaded and asked what a host asks first; true when all six are answered */
static bool
skel_up(void)
{
	static const struct {
		const char *what;
		uint16_t opcode;
	} probe[] = {
		{ "HCI_Reset", 0x0c03 },
		{ "HCI Read_Local_Version_Information", 0x1001 },
		{ "HCI Read_BD_ADDR", 0x1009 },
		{ "HCI Read_Local_Supported_Features", 0x1003 },
		{ "HCI Read_Buffer_Size", 0x1005 },
		{ "HCI LE_Read_Buffer_Size", 0x2002 },
	};
	const struct hakios_bt_platform *p = &mtkbt_platform;
	unsigned good = 0;
	int error;

	say("%s: power on", p->name);
	if ((error = p->power_on()) != 0) {
		say("power_on failed (%d): no Bluetooth", error);
		return false;
	}
	say("%s: Bluetooth's function on", p->name);
	if ((error = p->function_on()) != 0) {
		say("function_on failed (%d): no Bluetooth", error);
		return false;
	}
	for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		good += skel_hci(probe[i].what, probe[i].opcode);
	}
	say("%u of %u HCI commands answered as a controller answers", good, (unsigned)(sizeof(probe) / sizeof(probe[0])));
	if (good != sizeof(probe) / sizeof(probe[0])) {
		say("not a controller iOS can be given: Bluetooth's function off");
		p->function_off();
		return false;
	}
	say("%s: the bridge runs", p->name);
	return true;
}

static void
skel_thread(__unused void *arg, __unused wait_result_t w)
{
	const struct hakios_bt_platform *p = &mtkbt_platform;

	if (!skel_platform_complete(p)) {
		say("the platform is not complete: no Bluetooth");
		skel_dead = true;
		skel_set_state(SKEL_FAILED);
		skel_done = skel_asked;
		wakeup((event_t)&skel_done);
		thread_terminate(current_thread());
		return;
	}
	for (;;) {
		uint32_t asked = skel_asked, cycles = skel_cycles_asked;

		if (cycles != skel_cycles_done) {
			/* a power cycle (the user client's 3 and 5): off, and on again below if it is wanted on */
			if (skel_state == SKEL_ON) {
				say("%s: power cycle: Bluetooth's function off", p->name);
				p->function_off();
			}
			skel_set_state(SKEL_OFF);
			skel_cycles_done = cycles;
		}
		if (skel_want_on && skel_state == SKEL_OFF) {
			skel_set_state(SKEL_STARTING);
			skel_set_state(skel_up() ? SKEL_ON : SKEL_FAILED);
		} else if (!skel_want_on && (skel_state == SKEL_ON || skel_state == SKEL_FAILED)) {
			if (skel_state == SKEL_ON) {
				p->function_off();
			}
			say("%s: switched off, as asked", p->name);
			skel_set_state(SKEL_OFF);
		}
		if (skel_done != asked) {
			skel_done = asked;
			wakeup((event_t)&skel_done);
		}
		if (skel_state == SKEL_ON) {
			skel_bridge_once();
		} else {
			/* nothing to carry: asleep until asked again (what bluetoothd writes meanwhile waits in the queue) */
			assert_wait_timeout((event_t)&skel_event, THREAD_UNINT, 1000, NSEC_PER_MSEC);
			if (skel_pending) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				(void)thread_block(THREAD_CONTINUE_NULL);
			}
			skel_pending = false;
		}
	}
}

/*
 * A phone's Bluetooth driver gives its controller's operations, at boot (they are copied): from then on the controller can be
 * published, switched on and off. Nothing is done to the controller yet.
 */
void hakios_bthost_register(const struct hakios_bt_platform *platform);
void
hakios_bthost_register(const struct hakios_bt_platform *platform)
{
	static bool registered;

	if (registered || platform == NULL) {
		return;
	}
	registered = true;
	mtkbt_platform = *platform;
	(void)PE_parse_boot_argn("bt_vsc", &skel_vsc_mode, sizeof(skel_vsc_mode));
}

static bool
skel_registered(void)
{
	return mtkbt_platform.name != NULL;
}

/*
 * The kernel control com.apple.uart.sk.bluetooth, the Skywalk pipe behind it and the nodes /dev/btwake, /dev/btreset,
 * /dev/btpoweroff: published once, as soon as BSD is up (11-26: by AppleOnboardSerialBSDClient's start, its IOResourceMatch
 * IOBSD; hakios_bthost_start does it too, for a kernel whose device tree has no /arm-io/bluetooth). Until 11-26 they came
 * only once the controller answered, ~45 s into the boot, and bluetoothd had to be held back in user space until then.
 * false without a phone's controller (hakios_bthost_register) or when a part could not be made.
 */
bool hakios_bthost_publish(void);
bool
hakios_bthost_publish(void)
{
	static bool nodes, control, pipe;
	uuid_t uuid;
	bool done;

	if (!skel_registered()) {
		return false;
	}
	lck_mtx_lock(&skel_ask_lock);
	if (!nodes) {
		nodes = skel_nodes();
	}
	if (!control) {
		control = skel_ctl_register();
	}
	if (!pipe) {
		hakios_btsk_init();
		pipe = hakios_btsk_uuid(uuid);
	}
	done = nodes && control && pipe;
	lck_mtx_unlock(&skel_ask_lock);
	return done;
}

/* the pipe's UUID as text (36 characters and the 0), for the registry's IOSkywalkNexusUUID; false until published */
bool hakios_bthost_nexus_uuid(char *out, size_t size);
bool
hakios_bthost_nexus_uuid(char *out, size_t size)
{
	uuid_t uuid;
	uuid_string_t text;

	if (size < sizeof(text) || !hakios_btsk_uuid(uuid)) {
		return false;
	}
	uuid_unparse_upper(uuid, text);
	strlcpy(out, text, size);
	return true;
}

static void
skel_thread_start_locked(void)
{
	thread_t thread;

	if (skel_thread_started) {
		return;
	}
	skel_thread_started = true;
	if (kernel_thread_start_priority(skel_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		skel_dead = true;
		say("the bridge thread could not be started: no Bluetooth");
	}
}

/*
 * What the module's user client asks (iokit/Kernel/AppleBluetoothModule.cpp): 0 off, 1 on, 2 a power cycle (off and on
 * again). Waits until the bridge thread has done it, at most wait_s seconds (0: does not wait). 0 when the controller is
 * then as asked; ENODEV without a controller, EIO when it did not come up, ETIMEDOUT when the wait ran out.
 */
int hakios_bthost_power(int what, unsigned wait_s);
int
hakios_bthost_power(int what, unsigned wait_s)
{
	uint32_t mine, since = skel_ms();

	if (!skel_registered() || skel_dead) {
		return ENODEV;
	}
	lck_mtx_lock(&skel_ask_lock);
	skel_want_on = what != 0;
	if (what == 2 || (what == 1 && skel_state == SKEL_FAILED)) {
		skel_cycles_asked++;            /* (on again after a failure: another try) */
	}
	mine = ++skel_asked;
	skel_thread_start_locked();
	lck_mtx_unlock(&skel_ask_lock);
	hakios_bthost_wake();
	if (wait_s == 0) {
		return 0;
	}
	while ((int32_t)(skel_done - mine) < 0 && !skel_dead) {
		if (skel_ms() - since > wait_s * 1000u) {
			return ETIMEDOUT;
		}
		assert_wait_timeout((event_t)&skel_done, THREAD_ABORTSAFE, 250, NSEC_PER_MSEC);
		if ((int32_t)(skel_done - mine) >= 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_INTERRUPTED) {
			return EINTR;
		}
	}
	if (skel_dead) {
		return ENODEV;
	}
	return (what != 0) == (skel_state == SKEL_ON) ? 0 : EIO;
}

/* the controller's state for the module's registry entry: 0 off, 1 starting, 2 on, 3 failed */
int hakios_bthost_state(void);
int
hakios_bthost_state(void)
{
	return skel_state;
}

/*
 * The phone's driver, once its controller may be switched on (hakios.akita_bt.c, 30 s into the boot): the operations
 * registered if they are not yet, the control published if it is not yet, and the controller switched on -- the module's
 * autoOn: nothing in iOS asks for it here, BlueTool's stand-in answers bluetoothd's "boot" without asking the module.
 * Unless the user client has already been asked for something: then that is what stands.
 */
void hakios_bthost_start(const struct hakios_bt_platform *platform);
void
hakios_bthost_start(const struct hakios_bt_platform *platform)
{
	static bool started;

	if (started || platform == NULL) {
		return;
	}
	started = true;
	hakios_bthost_register(platform);
	if (!hakios_bthost_publish()) {
		say("the kernel control, the pipe or the nodes could not be made: no Bluetooth for iOS");
		return;
	}
	if (skel_asked == 0) {
		(void)hakios_bthost_power(1, 0);
	}
}
