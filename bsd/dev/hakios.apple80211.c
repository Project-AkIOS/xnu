/*
 * Darwin-on-hakios: Apple80211, answered in the kernel.
 *
 * Darwin's Wi-Fi side -- wifid, and through it Settings and the status bar -- talks to a Wi-Fi
 * driver in requests that were once ioctls and are, where the driver offers no user client,
 * ioctls still: SIOCGA80211 and SIOCSA80211 on any socket, with a struct apple80211req that
 * names the interface, a request type, a value, and a buffer in the caller's memory
 * (IO80211.framework: Apple80211Get/Set and their kin all end there). XNU hands an interface
 * ioctl it does not know to the interface, so they arrive in drivers/net/hakios.net.c, which
 * passes them here when its link is a radio.
 *
 * What is asked is answered from the station boundary (sys/hakios.sta.h): the radio's power,
 * its channels, a scan and what it heard, the network joined. What happens there by itself is
 * told to Darwin the way a driver tells it: kernel events of class 6 (IEEE 802.11), subclass 1,
 * whose code is Apple80211's own event number and whose data begins with the interface's name
 * in 16 bytes (IO80211.framework reads them from a PF_SYSTEM socket and compares the name).
 *
 * Every request is still written to the log -- who asks, get or set, the type, the value, the
 * length -- because which of them wifid needs is found that way. Never what a request carries:
 * a network's name or its key may be in there.
 *
 * The request types and the layouts are Apple's. The old ones are as the headers of its open
 * IO80211 sources once described them (apple80211_ioctl.h, apple80211_var.h); where Darwin userland 16.5
 * differs the layout was read out of IO80211.framework of 16.5, which fills and reads the same
 * structures on the other side (Apple80211GetWithIOCTL, _getScanData, _addScanResultToList,
 * _performAssociation, _dispatchAsyncAssocEvent, _dispatchLinkChangedEvent).
 *
 * (11-26) Darwin userland 27 (24B156) grew two of them, read the same way out of its
 * IO80211.framework: the scan request (Apple80211ScanAsync, _getScanData) by the scan's UUID and
 * an oldest time at its end, and a scan's result (_dispatchAsyncScanEvent, _addScanResultToList,
 * Apple80211GetWithIOCTL for CURRENT_NETWORK) by twice the room for elements and some words
 * after them. What both had before stayed where it was. The request types' names come from its
 * own table (convertApple80211IOCTLToString).
 *
 * A join: ASSOCIATE (20) carries the network's name, perhaps its access point, and the
 * passphrase as typed (the framework does not make the key of it), and is answered at once;
 * when the station has joined or failed, event 9 says so, and ASSOCIATE_RESULT (21) gives the
 * outcome as one of Apple80211's result numbers, which the framework turns into its errors:
 * 1 success, 21 "invalid PMK" (a wrong passphrase), 9 a timeout, 2 anything else.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/hakios.input.h>
#include <sys/hakios.net.h>
#include <sys/hakios.sta.h>
#include <sys/sockio.h>
#include <sys/systm.h>

#define SIOCSA80211             0x802869c8u     /* _IOW('i', 200, struct apple80211req) */
#define SIOCGA80211             0xc02869c9u     /* _IOWR('i', 201, struct apple80211req) */

struct apple80211req {
	char req_if_name[16];
	int32_t req_type;
	int32_t req_val;
	uint32_t req_len;
	user_addr_t req_data __attribute__((aligned(8)));
};

/* (as far as they have come by: the names help reading the log) */
static const char *
type_name(int32_t type)
{
	static const struct {
		int32_t type;
		const char *name;
	} names[] = {
		{ 1, "SSID" }, { 2, "AUTH_TYPE" }, { 3, "CIPHER_KEY" }, { 4, "CHANNEL" }, { 5, "POWERSAVE" }, { 6, "PROTMODE" },
		{ 7, "TXPOWER" }, { 8, "RATE" }, { 9, "BSSID" }, { 10, "SCAN_REQ" }, { 11, "SCAN_RESULT" }, { 12, "CARD_CAPABILITIES" },
		{ 13, "STATE" }, { 14, "PHY_MODE" }, { 15, "OP_MODE" }, { 16, "RSSI" }, { 17, "NOISE" }, { 18, "INT_MIT" }, { 19, "POWER" },
		{ 20, "ASSOCIATE" }, { 21, "ASSOCIATE_RESULT" }, { 22, "DISASSOCIATE" }, { 23, "STATUS_DEV_NAME" }, { 24, "IBSS_MODE" },
		{ 25, "HOST_AP_MODE" }, { 26, "AP_MODE" }, { 27, "SUPPORTED_CHANNELS" }, { 28, "LOCALE" }, { 29, "DEAUTH" },
		{ 30, "COUNTERMEASURES" }, { 32, "RATE_SET" }, { 42, "STATION_LIST" }, { 43, "DRIVER_VERSION" },
		{ 44, "HARDWARE_VERSION" }, { 46, "RSN_IE" }, { 47, "BACKGROUND_SCAN" }, { 48, "AP_IE_LIST" }, { 49, "STATS" },
		{ 50, "ASSOCIATION_STATUS" }, { 51, "COUNTRY_CODE" }, { 54, "RADIO_INFO" }, { 57, "MCS" }, { 66, "MCS_INDEX_SET" },
		{ 79, "STA_STATS" }, { 80, "ROAM_THRESH" }, { 85, "IE" }, { 86, "SCAN_REQ_MULTIPLE" }, { 87, "BTCOEX_MODE" },
		{ 90, "SCANCACHE_CLEAR" }, { 94, "VIRTUAL_IF_CREATE" }, { 95, "VIRTUAL_IF_DELETE" }, { 103, "CURRENT_NETWORK" },
		{ 106, "RSSI_BOUNDS" }, { 107, "ROAM" }, { 115, "POWER_DEBUG_INFO" }, { 207, "CHANNELS_INFO" },
		{ 254, "HW_SUPPORTED_CHANNELS" }, { 365, "DISPLAY_STATE" }, { 390, "BEACON_SCAN_CACHE_REQ" }, { 473, "MIMO_CONFIG" },
		{ 481, "HEARTBEAT" }, { 483, "EXTERNAL_POWER_STATE" }, { 485, "APP_STATE" },
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (names[i].type == type) {
			return names[i].name;
		}
	}
	return "?";
}

/* ---- the layouts ------------------------------------------------------------------------------------------ */

#define CHANNEL_20MHZ           0x2u
#define CHANNEL_2GHZ            0x8u
#define CHANNEL_5GHZ            0x10u
#define CHANNEL_ACTIVE          0x80u           /* the radio may send there */
#define CHANNEL_DFS             0x100u

struct apple80211_channel {
	uint32_t version, channel, flags;
};

/* SUPPORTED_CHANNELS (27) and HW_SUPPORTED_CHANNELS (254): 4824 bytes */
struct apple80211_channels {
	uint32_t version, count;
	struct apple80211_channel channels[401];
	uint32_t unused;
};

/* CHANNELS_INFO (207): 10016 bytes; of each channel a byte in each of several rows, and a word of flags */
struct apple80211_channels_info {
	uint8_t unknown_0[12];
	uint16_t count;
	uint8_t unknown_1[800];
	uint8_t number[400], indoor[400], dfs[400], passive[400], wide_40[400], wide_80[400];
	uint8_t unknown_2[1602];
	uint32_t flags[400];
	uint8_t unknown_3[3600];
} __attribute__((packed));

/*
 * SCAN_REQ (10): 5528 bytes (Darwin userland 27; 16.5 sent the first 5456 of them, and the
 * same fields are where they were). The UUID is the one the framework made for the scan, as text
 * (uuid_unparse), and comes back in each of its results; the oldest time is the framework's
 * SCAN_MIN_TIMESTAMP, in a unit not known yet (11-26).
 */
#define SCAN_REQUEST_SIZE_16        5456
struct apple80211_scan_request {
	uint32_t version, bss_type;
	uint8_t bssid[6], unused_0[2];
	uint32_t ssid_length;
	uint8_t ssid[32];
	uint32_t scan_type;                     /* 1: asking, 2: listening only */
	uint32_t phy_mode;
	uint16_t dwell_ms, unused_1;
	uint32_t rest_ms;
	uint32_t channel_count;
	struct apple80211_channel channels[400];
	uint64_t flags;
	int32_t rssi_threshold;                 /* the weakest worth hearing; INT32_MIN: no such */
	uint32_t name_count;                    /* networks asked for by name, each in 56 bytes */
	uint8_t names[10][56];
	uint32_t short_ssid;
	char uuid[64];
	uint32_t unused_3;
	uint64_t min_timestamp;
};

/*
 * SCAN_RESULT (11) and CURRENT_NETWORK (103): 2280 bytes (Darwin userland 27; 16.5 had 1164, with
 * room for 1024 bytes of elements and nothing after them). The framework hands in the room for
 * elements (2048) and the scan's UUID, and reads no version. Flags: 0x1 heard in a probe
 * response, 0x4 in a FILS discovery frame, 0x8 the name is a short SSID, 0x10 an OWE network of
 * several names. After the elements (11-26): TIMESTAMP (a unit not known yet; 0 here),
 * RSN_PRIORITY, a word not read into the dictionary, PHY_MODE, MRSNO, MLO and EMLSR.
 */
#define PHY_11A                 0x2u
#define PHY_11B                 0x4u
#define PHY_11G                 0x8u
#define PHY_11N                 0x10u
#define PHY_11AC                0x80u
struct apple80211_scan_result {
	uint32_t version;
	struct apple80211_channel channel;
	uint16_t noise_known;
	int16_t noise, snr, rssi;
	uint16_t beacon_interval, capability;
	uint8_t bssid[6];
	uint8_t rate_count, unused_0;
	uint32_t rates[15];                     /* Mbit/s */
	uint8_t ssid_length, ssid[32], unused_1[3];
	uint32_t age_ms;
	uint8_t flags, unused_2;
	uint16_t elements_length;               /* comes in as the room there is */
	uint8_t elements[2048];
	char uuid[64];                          /* comes in as the scan's */
	uint32_t unused_3;
	uint64_t timestamp;
	uint8_t rsn_priority, unused_4[3];
	uint32_t unknown_0;
	uint32_t phy_mode;                      /* PHY_* */
	uint8_t mrsno, mlo, emlsr, unused_5;
};

/*
 * ASSOCIATE (20): 656 bytes (Darwin userland 27, _performAssociation; 16.5 had 592, with the
 * authentication in two halfwords and so everything after it 4 bytes earlier). Of what follows
 * the key nothing is used (11-26): the RSN element wifid built (a halfword of length at 0xd0, the
 * element at 0xd2), flags, the PMK the framework made of a passphrase (its length at 0x1d8, the
 * key at 0x1dc), a private MAC address (0x1fc), access points not to join (count 0x202, from
 * 0x203), the colocated network's scope (0x240), RSSI limits and 6 GHz and EMLSR settings.
 */
struct apple80211_assoc {
	uint32_t version;
	uint16_t mode, unused_0;
	uint32_t auth_lower, auth_upper;
	uint32_t ssid_length;
	uint8_t ssid[32];
	uint8_t bssid[6], unused_1[2];
	uint32_t key_version, key_length, key_cipher;   /* cipher 10: a passphrase as typed; 6: the key (from 64 hex digits) */
	uint16_t key_flags, key_index;
	uint8_t key[64];
	uint8_t rest[0x290 - 0x8c];
};

#define CIPHER_PMK              6
#define CIPHER_PASSPHRASE       10

/* the link event's data (at least 32 bytes, or the framework drops the event) */
struct apple80211_link_changed {
	uint8_t down, unused_0[3];
	int32_t rssi;
	uint16_t snr, noise;
	uint8_t unused_1[4];
	uint8_t involuntary, unused_2[3];
	uint32_t reason, subreason;
	uint8_t unused_3[4];
};

_Static_assert(sizeof(struct apple80211_assoc) == 656 && offsetof(struct apple80211_assoc, auth_lower) == 0x8 &&
    offsetof(struct apple80211_assoc, ssid_length) == 0x10 && offsetof(struct apple80211_assoc, bssid) == 0x34 &&
    offsetof(struct apple80211_assoc, key_length) == 0x40 && offsetof(struct apple80211_assoc, key_cipher) == 0x44 &&
    offsetof(struct apple80211_assoc, key) == 0x4c, "the association request as Darwin userland 27 has it");
_Static_assert(sizeof(struct apple80211_link_changed) == 32 && offsetof(struct apple80211_link_changed, reason) == 0x14,
    "the link event as Darwin userland 16.5 reads it");
_Static_assert(sizeof(struct apple80211_channels) == 4824, "the channel list as Darwin userland 16.5 has it");
_Static_assert(sizeof(struct apple80211_channels_info) == 10016 && offsetof(struct apple80211_channels_info, number) == 0x32e &&
    offsetof(struct apple80211_channels_info, flags) == 0x12d0, "the channels' description as Darwin userland 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_request) == 5528 && offsetof(struct apple80211_scan_request, scan_type) == 0x34 &&
    offsetof(struct apple80211_scan_request, dwell_ms) == 0x3c && offsetof(struct apple80211_scan_request, channel_count) == 0x44 &&
    offsetof(struct apple80211_scan_request, channels) == 0x48 && offsetof(struct apple80211_scan_request, flags) == 0x1308 &&
    offsetof(struct apple80211_scan_request, rssi_threshold) == 0x1310 && offsetof(struct apple80211_scan_request, names) == 0x1318 &&
    offsetof(struct apple80211_scan_request, short_ssid) == 0x1548 && offsetof(struct apple80211_scan_request, uuid) == 0x154c &&
    offsetof(struct apple80211_scan_request, min_timestamp) == 0x1590, "the scan request as Darwin userland 27 has it");
_Static_assert(sizeof(struct apple80211_scan_result) == 2280 && offsetof(struct apple80211_scan_result, bssid) == 0x1c &&
    offsetof(struct apple80211_scan_result, rates) == 0x24 && offsetof(struct apple80211_scan_result, ssid_length) == 0x60 &&
    offsetof(struct apple80211_scan_result, age_ms) == 0x84 && offsetof(struct apple80211_scan_result, elements_length) == 0x8a &&
    offsetof(struct apple80211_scan_result, elements) == 0x8c && offsetof(struct apple80211_scan_result, uuid) == 0x88c &&
    offsetof(struct apple80211_scan_result, timestamp) == 0x8d0 && offsetof(struct apple80211_scan_result, rsn_priority) == 0x8d8 &&
    offsetof(struct apple80211_scan_result, phy_mode) == 0x8e0 && offsetof(struct apple80211_scan_result, mrsno) == 0x8e4,
    "a scan's result as Darwin userland 27 has it");

/* Apple80211's events (APPLE80211_M_...) */
enum { EVENT_POWER_CHANGED = 1, EVENT_SSID_CHANGED = 2, EVENT_BSSID_CHANGED = 3, EVENT_LINK_CHANGED = 4, EVENT_ASSOC_DONE = 9,
       EVENT_SCAN_DONE = 10 };

static const char interface_name[16] = "en0";   /* (drivers/net/hakios.net.c makes one interface, and that is its name) */

static LCK_GRP_DECLARE(apple80211_lock_group, "hakios-apple80211");
static LCK_MTX_DECLARE(apple80211_lock, &apple80211_lock_group);
static unsigned result_next;                    /* which of what the last scan heard a SCAN_RESULT request gets next */
static uint32_t join_result;                    /* of the last join asked for: 0 while it runs, then Apple80211's number */

/* ---- channels ---------------------------------------------------------------------------------------- */

static unsigned
channel_number(unsigned frequency)
{
	return frequency == 2484 ? 14 : frequency < 4000 ? (frequency - 2407) / 5 : (frequency - 5000) / 5;
}

static unsigned
channel_frequency(const struct apple80211_channel *channel)
{
	bool low = channel->flags & CHANNEL_2GHZ ? true : channel->flags & CHANNEL_5GHZ ? false : channel->channel <= 14;

	if (channel->channel == 0 || channel->channel > 196 || (channel->flags & 0x2000)) {     /* (0x2000: 6 GHz) */
		return 0;
	}
	return low ? (channel->channel == 14 ? 2484 : 2407 + 5 * channel->channel) : 5000 + 5 * channel->channel;
}

static void
channel_fill(struct apple80211_channel *channel, unsigned frequency, unsigned flags)
{
	channel->version = 1;
	channel->channel = channel_number(frequency);
	channel->flags = CHANNEL_20MHZ | (frequency < 4000 ? CHANNEL_2GHZ : CHANNEL_5GHZ) | (flags & HAKIOS_STA_CHANNEL_PASSIVE ?
	    0 : CHANNEL_ACTIVE) | (flags & HAKIOS_STA_CHANNEL_RADAR ? CHANNEL_DFS : 0);
}

/* ---- the caller's buffer ------------------------------------------------------------------------------ */

/*
 * (11-26) A request comes two ways: as an ioctl, its buffer in the caller's memory, or through the
 * user client (iokit/Kernel/hakios.wlan.cpp, IO80211APIUserClient), which has copied the buffer
 * into the kernel already. Every request is handled inside one of these, the struct apple80211req
 * first, so that what reads and writes the buffer can tell which it is from the request alone.
 */
struct call {
	struct apple80211req req;
	uint8_t *kernel_buffer;                 /* NULL: req_data is the caller's memory */
	size_t capacity;                        /* the kernel buffer's size */
};
_Static_assert(offsetof(struct call, req) == 0, "a request is the start of its call");

static bool
has_buffer(const struct apple80211req *request)
{
	return request->req_data != USER_ADDR_NULL;
}

/* `length` bytes of the buffer from `offset` */
static int
req_read(const struct apple80211req *request, size_t offset, void *to, size_t length)
{
	const struct call *call = (const struct call *)request;

	if (call->kernel_buffer == NULL) {
		return copyin(request->req_data + offset, to, length);
	}
	if (offset > call->capacity || length > call->capacity - offset) {
		return EINVAL;
	}
	memcpy(to, call->kernel_buffer + offset, length);
	return 0;
}

static int
req_write(const struct apple80211req *request, const void *from, size_t length)
{
	const struct call *call = (const struct call *)request;

	if (call->kernel_buffer == NULL) {
		return copyout(from, request->req_data, length);
	}
	if (length > call->capacity) {
		return EINVAL;
	}
	memcpy(call->kernel_buffer, from, length);
	return 0;
}

/* ---- what the caller is given back ------------------------------------------------------------------- */

/* At most as many bytes as it has room for. */
static int
give(const struct apple80211req *request, const void *data, size_t length)
{
	return !has_buffer(request) ? 0 : req_write(request, data, length < request->req_len ? length : request->req_len);
}

/* An access point in the form a scan's result has; the caller has said how much room for elements there is. */
/* (11-26) the kinds a network speaks: by band and rates, then HT (element 45) and VHT (element 191) */
static uint32_t
bss_phy(const struct hakios_sta_bss *bss)
{
	uint32_t phy = bss->frequency >= 4000 ? PHY_11A : PHY_11B;

	for (unsigned at = 0; at + 2 <= bss->elements_length && at + 2 + bss->elements[at + 1] <= bss->elements_length;
	    at += 2 + bss->elements[at + 1]) {
		for (unsigned i = 0; bss->frequency < 4000 && (bss->elements[at] == 1 || bss->elements[at] == 50) && i < bss->elements[at + 1];
		    i++) {
			if ((bss->elements[at + 2 + i] & 0x7f) > 22) {  /* (above 11 Mbit/s: OFDM) */
				phy |= PHY_11G;
			}
		}
		phy |= bss->elements[at] == 45 ? PHY_11N : bss->elements[at] == 191 ? PHY_11AC : 0;
	}
	return phy;
}

static int
give_network(const struct apple80211req *request, const struct hakios_sta_bss *bss)
{
	struct apple80211_scan_result *result = kalloc_data(sizeof(*result), Z_WAITOK | Z_ZERO);
	unsigned room = sizeof(result->elements);
	char uuid[sizeof(result->uuid)] = "";
	int error;

	if (result == NULL) {
		return ENOMEM;
	}
	/* (the room the caller said, and what its buffer has room for: a caller of 16.5's size has 1024) */
	if (request->req_len < sizeof(*result)) {
		room = request->req_len > offsetof(struct apple80211_scan_result, elements) ? request->req_len -
		    (unsigned)offsetof(struct apple80211_scan_result, elements) : 0;
	} else if (has_buffer(request)) {
		(void)req_read(request, offsetof(struct apple80211_scan_result, uuid), uuid, sizeof(uuid));
	}
	if (request->req_len >= offsetof(struct apple80211_scan_result, elements) && has_buffer(request) &&
	    req_read(request, 0, result, offsetof(struct apple80211_scan_result, elements)) == 0 && result->elements_length < room) {
		room = result->elements_length;
	}
	bzero(result, sizeof(*result));
	result->version = 1;
	memcpy(result->uuid, uuid, sizeof(result->uuid));
	result->uuid[sizeof(result->uuid) - 1] = '\0';
	channel_fill(&result->channel, bss->frequency, 0);
	result->rssi = bss->rssi;
	result->beacon_interval = bss->beacon_interval;
	result->capability = bss->capability;
	memcpy(result->bssid, bss->bssid, sizeof(result->bssid));
	result->ssid_length = bss->ssid_length;
	memcpy(result->ssid, bss->ssid, bss->ssid_length);
	result->age_ms = bss->age_ms;
	/*
	 * (11-26) when it was heard, on the clock CoreWiFi measures a result's age by (mach_continuous_time, the boot's):
	 * left 0, every result was as old as the boot, and auto-join's maxAge filter threw them all out ("All scan
	 * results are filtered out"), so a known network was never rejoined. The station's age in ms taken off now.
	 */
	/*
	 * (11-28) In nanoseconds of that clock, not its ticks: with the timebase at 24 MHz (11-27) the ticks were taken as
	 * nanoseconds and every result was ~1/41 of the boot old -- 4.7 s at 197 s, "Filtering based on maxAge
	 * (... age=192398ms)", the known network never a candidate (candidateBSSCount=0) and auto-join scanning again
	 * every ~10 s, for ever.
	 */
	{
		uint64_t now, ago = (uint64_t)bss->age_ms * NSEC_PER_MSEC;

		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		result->timestamp = now > ago ? now - ago : 0;
	}
	result->elements_length = (uint16_t)(bss->elements_length <= room ? bss->elements_length : 0);
	memcpy(result->elements, bss->elements, result->elements_length);
	result->phy_mode = bss_phy(bss);
	/* the rates it says it has (elements 1 and 50: units of 500 kbit/s, the top bit marks the ones everybody must have) */
	for (unsigned at = 0; at + 2 <= bss->elements_length && at + 2 + bss->elements[at + 1] <= bss->elements_length;
	    at += 2 + bss->elements[at + 1]) {
		for (unsigned i = 0; (bss->elements[at] == 1 || bss->elements[at] == 50) && i < bss->elements[at + 1]; i++) {
			unsigned rate = (bss->elements[at + 2 + i] & 0x7f) / 2;

			if (rate != 0 && rate <= 54 && result->rate_count < 15) {
				result->rates[result->rate_count++] = rate;
			}
		}
	}
	{
		/* (11-19, why wifid's auto-join finds no network: what a result carries, the first 60 said) */
		static unsigned said;

		if (said < 60) {
			said++;
			printf("apple80211: a network given: \"%.*s\" channel %u flags 0x%x, %d dBm, age %u ms, capability 0x%04x, elements %u of "
			    "%u (room %u, the request %u bytes), %u rates, phy %#x\n", (int)result->ssid_length, result->ssid,
			    result->channel.channel, result->channel.flags, result->rssi, result->age_ms, result->capability,
			    result->elements_length, bss->elements_length, room, request->req_len, result->rate_count, result->phy_mode);
			for (unsigned at = 0; at + 2 <= result->elements_length && at + 2 + result->elements[at + 1] <= result->elements_length;
			    at += 2 + result->elements[at + 1]) {
				if (result->elements[at] == 48) {       /* the RSN element: version, group, pairwise and key management suites */
					char text[3 * 40 + 1] = "";
					unsigned n = result->elements[at + 1] < 40 ? result->elements[at + 1] : 40;

					for (unsigned i = 0; i < n; i++) {
						snprintf(text + 3 * i, 4, " %02x", result->elements[at + 2 + i]);
					}
					printf("apple80211:   its RSN element (%u bytes):%s\n", result->elements[at + 1], text);
				}
			}
		}
	}
	error = give(request, result, sizeof(*result));
	kfree_data(result, sizeof(*result));
	return error;
}

/* ---- get ---------------------------------------------------------------------------------------------- */

static int
get(struct apple80211req *request)
{
	struct hakios_sta_state *state;
	int error = 0;

	switch (request->req_type) {
	case 43:
	case 44: {                              /* the driver's and the hardware's name: the text alone, in 256 bytes */
		static const char name[256] = "hakios: ath11k for the WCN6750";

		return give(request, name, sizeof(name));
	}
	case 12: {
		/*
		 * What the card can do, a bit each (apple80211_var.h: APPLE80211_CAP_*): the 13 bytes are the
		 * bits alone, from the first (the framework hands in a buffer that begins after the old
		 * header's version word). AES, AES-CCM, power management, short slot and preamble, WPA2, WME,
		 * short guard interval at 20 MHz. Read wrong, the framework sees no WPA2 and builds no join
		 * for a WPA2 network ("Unable to join").
		 */
		uint8_t bits[13] = { 0 };
		static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };

		for (unsigned i = 0; i < sizeof(has); i++) {
			bits[has[i] / 8] |= (uint8_t)(1u << (has[i] % 8));
		}
		return give(request, bits, sizeof(bits));
	}
	case 19: {                              /* the radio's power: one radio. Until the driver is up it counts as on: it */
		struct {                        /* is being brought up, and Darwin asks this once, early */
			uint32_t version, radios, state[4];
		} answer = { .version = 1, .radios = 1, .state = { !hakios_sta_present() || hakios_sta_powered() } };

		return give(request, &answer, sizeof(answer));
	}
	case 15:                                /* the mode: a station (APPLE80211_M_STA) */
		request->req_val = 1;
		return 0;
	case 106: {
		/*
		 * RSSI_BOUNDS: the weakest and the strongest signal the radio reports and the one below which a network is not
		 * joined automatically. wifid asks ONCE, when it opens the device, and keeps what it had made up before if this
		 * is refused: a threshold of +7 dBm, under which every network there is falls -- its auto-join then finds the
		 * known network in each scan and refuses it ("Below minimum RSSI threshold" at -63 dBm; 11-19, the tester's
		 * "it does not auto connect"; ~/akita/ref/wifi-autojoin-rssi.md). -85 lets CoreWiFi's own limits (-80, and
		 * -90 for its last try) decide, as on an iPhone.
		 */
		const struct { uint32_t version; int32_t weakest, strongest, threshold; } bounds = { 1, -100, 0, -85 };

		return give(request, &bounds, sizeof(bounds));
	}
	case 27:
	case 254: {                             /* the channels the radio may tune to */
		struct apple80211_channels *answer;
		struct hakios_sta_channel channels[96];
		unsigned count = hakios_sta_channels(channels, 96);

		if (count == 0 || (answer = kalloc_data(sizeof(*answer), Z_WAITOK | Z_ZERO)) == NULL) {
			return count == 0 ? ENXIO : ENOMEM;
		}
		answer->version = 1;
		answer->count = count;
		for (unsigned i = 0; i < count; i++) {
			channel_fill(&answer->channels[i], channels[i].frequency, channels[i].flags);
		}
		error = give(request, answer, sizeof(*answer));
		kfree_data(answer, sizeof(*answer));
		return error;
	}
	case 207: {                             /* ... and more of each */
		struct apple80211_channels_info *answer;
		struct hakios_sta_channel channels[96];
		unsigned count = hakios_sta_channels(channels, 96);

		if (count == 0 || (answer = kalloc_data(sizeof(*answer), Z_WAITOK | Z_ZERO)) == NULL) {
			return count == 0 ? ENXIO : ENOMEM;
		}
		answer->count = (uint16_t)count;
		for (unsigned i = 0; i < count; i++) {
			struct apple80211_channel channel;

			channel_fill(&channel, channels[i].frequency, channels[i].flags);
			answer->number[i] = (uint8_t)channel.channel;
			answer->dfs[i] = (channels[i].flags & HAKIOS_STA_CHANNEL_RADAR) != 0;
			answer->passive[i] = (channels[i].flags & HAKIOS_STA_CHANNEL_PASSIVE) != 0;
			answer->flags[i] = channel.flags;
		}
		error = give(request, answer, sizeof(*answer));
		kfree_data(answer, sizeof(*answer));
		return error;
	}
	case 21: {                              /* how the last join went */
		/*
		 * (11-26) iOS 27 reads it as STATE: {version, result} in a 16-byte buffer, the result at offset 4
		 * (_dispatchAsyncAssocEvent: RawGet 21, then __errorForAssociateResult), and 0xffff means "not over yet, ask
		 * again". Answered in req_val only, the buffer stayed empty: a join that worked was reported failed (error
		 * -3902), Settings said "Failed to join" and wifid never saved the network, so it never rejoined it.
		 */
		struct {
			uint32_t version, result;
		} answer = { .version = 1 };

		lck_mtx_lock(&apple80211_lock);
		answer.result = join_result != 0 ? join_result : 0xffff;
		lck_mtx_unlock(&apple80211_lock);
		request->req_val = (int32_t)answer.result;
		return request->req_len >= sizeof(answer) ? give(request, &answer, sizeof(answer)) : 0;
	}
	case 11: {                              /* what the last scan heard, one access point a request; EIO: that was all */
		struct hakios_sta_bss *bss = kalloc_data(sizeof(*bss), Z_WAITOK);
		unsigned index;
		bool there;

		if (bss == NULL) {
			return ENOMEM;
		}
		lck_mtx_lock(&apple80211_lock);
		index = result_next++;
		lck_mtx_unlock(&apple80211_lock);
		there = hakios_sta_heard(index, bss);
		error = there ? give_network(request, bss) : EIO;
		kfree_data(bss, sizeof(*bss));
		return error;
	}
	default:
		break;
	}

	/* the rest is about the network joined, and there is none unless the station says so */
	if ((state = kalloc_data(sizeof(*state), Z_WAITOK)) == NULL) {
		return ENOMEM;
	}
	hakios_sta_state(state);
	switch (request->req_type) {
	case 13: {                              /* the state: APPLE80211_S_INIT 0, _SCAN 1, _AUTH 2, _ASSOC 3, _RUN 4 */
		/* (11-26: Darwin userland 27 reads it from 8 bytes, a version and the state; the value is kept as well) */
		struct {
			uint32_t version, state;
		} answer = { .version = 1, .state = state->phase == HAKIOS_STA_JOINED ? 4 : state->phase == HAKIOS_STA_JOINING ? 2 :
			     state->phase == HAKIOS_STA_SCANNING ? 1 : 0 };

		request->req_val = (int32_t)answer.state;
		error = give(request, &answer, sizeof(answer));
		break;
	}
	case 1:                                 /* its name; the length goes back in the request */
		if (state->phase != HAKIOS_STA_JOINED) {
			error = ENXIO;
		} else if ((error = give(request, state->bss.ssid, state->bss.ssid_length)) == 0) {
			request->req_len = state->bss.ssid_length;
		}
		break;
	case 9:                                 /* its access point's address */
		error = state->phase != HAKIOS_STA_JOINED ? ENXIO : give(request, state->bss.bssid, 6);
		break;
	case 4: {                               /* its channel */
		struct {
			uint32_t version;
			struct apple80211_channel channel;
		} answer = { .version = 1 };

		if (state->phase != HAKIOS_STA_JOINED) {
			error = ENXIO;
		} else {
			channel_fill(&answer.channel, state->bss.frequency, 0);
			error = give(request, &answer, sizeof(answer));
		}
		break;
	}
	case 16: {                              /* how strongly it is heard: dBm, per radio and all together, twice over */
		struct {
			uint32_t version, radios, unit;
			int32_t each[4], all, each_ext[4], all_ext;
		} answer = { .version = 1, .radios = 1, .each = { state->bss.rssi }, .all = state->bss.rssi,
			     .each_ext = { state->bss.rssi }, .all_ext = state->bss.rssi };

		error = state->phase != HAKIOS_STA_JOINED ? ENXIO : give(request, &answer, sizeof(answer));
		break;
	}
	case 103:                               /* all of it, as a scan's result */
		error = state->phase != HAKIOS_STA_JOINED ? ENXIO : give_network(request, &state->bss);
		break;
	case 8:                                 /* (11-28) the rate it sends at, Mbit/s, in the value (no buffer asked) */
		if (state->phase != HAKIOS_STA_JOINED || state->rate_mbps == 0) {
			error = ENXIO;
		} else {
			struct {
				uint32_t version, radios, rate[4];
			} answer = { .version = 1, .radios = 1, .rate = { state->rate_mbps } };

			request->req_val = (int32_t)state->rate_mbps;
			error = request->req_len >= sizeof(answer) ? give(request, &answer, sizeof(answer)) : 0;
		}
		break;
	case 17: {                              /* (11-28) the noise on its channel: as RSSI's layout, dBm */
		struct {
			uint32_t version, radios, unit;
			int32_t each[4], all, each_ext[4], all_ext;
		} answer = { .version = 1, .radios = 1, .each = { state->noise }, .all = state->noise,
			     .each_ext = { state->noise }, .all_ext = state->noise };

		error = state->phase != HAKIOS_STA_JOINED || state->noise == 0 ? ENXIO : give(request, &answer, sizeof(answer));
		break;
	}
	case 14: {                              /* (11-28) the kinds of 802.11: what the radio has, what the network uses */
		struct {
			uint32_t version, supported, active;
		} answer = { .version = 1, .supported = PHY_11A | PHY_11B | PHY_11G | PHY_11N | PHY_11AC };

		if (state->phase == HAKIOS_STA_JOINED) {
			uint32_t phy = bss_phy(&state->bss);

			/* the newest both have */
			answer.active = phy & PHY_11AC ? PHY_11AC : phy & PHY_11N ? PHY_11N : phy & PHY_11G ? PHY_11G :
			    phy & PHY_11A ? PHY_11A : PHY_11B;
		}
		request->req_val = (int32_t)answer.active;
		error = give(request, &answer, sizeof(answer));
		break;
	}
	case 51:                                /* (11-28) the country it works under: two letters and a 0 */
		if (state->country[0] == 0) {
			error = ENXIO;
		} else {
			error = give(request, state->country, request->req_len < 3 ? request->req_len : 3);
		}
		break;
	case 2: {                               /* how it was joined: open system, then WPA2 with a passphrase */
		struct {
			uint32_t version, lower, upper, extended;
		} answer = { .version = 1, .lower = 1, .upper = 8 };

		error = state->phase != HAKIOS_STA_JOINED ? ENXIO : give(request, &answer, sizeof(answer));
		break;
	}
	default:
		error = EOPNOTSUPP;
		break;
	}
	kfree_data(state, sizeof(*state));
	return error;
}

/* ---- set ---------------------------------------------------------------------------------------------- */

static int
set_scan(const struct apple80211req *request, char *said, size_t said_size)
{
	struct apple80211_scan_request *asked;
	struct hakios_sta_scan *what;
	unsigned unknown = 0;
	int error;

	/* (Darwin userland 27's size, or 16.5's: the fields read here are where they were) */
	if (request->req_len != sizeof(*asked) && request->req_len != SCAN_REQUEST_SIZE_16) {
		return EINVAL;
	}
	if (!hakios_sta_present()) {
		return EBUSY;                   /* (the radio is still being brought up: the framework tries again) */
	}
	asked = kalloc_data(sizeof(*asked), Z_WAITOK | Z_ZERO);
	what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
	if (asked == NULL || what == NULL || (error = req_read(request, 0, asked, request->req_len)) != 0) {
		error = asked == NULL || what == NULL ? ENOMEM : error;
		goto out;
	}
	if (asked->ssid_length <= sizeof(what->ssid)) {
		what->ssid_length = (uint8_t)asked->ssid_length;
		memcpy(what->ssid, asked->ssid, asked->ssid_length);
	}
	what->passive = asked->scan_type == 2;
	what->dwell_ms = asked->dwell_ms;
	/* (more channels than there is room to name: every channel) */
	for (unsigned i = 0; i < asked->channel_count && i < 400 && asked->channel_count <= 64; i++) {
		unsigned frequency = channel_frequency(&asked->channels[i]);

		if (frequency != 0) {
			what->frequencies[what->frequency_count++] = (uint16_t)frequency;
		} else {
			unknown++;
		}
	}
	snprintf(said, said_size, " (a scan: type %u, %u channels of which %u are not the radio's, %u ms on each, a name of %u bytes, "
	    "%u more names, flags %#llx)", asked->scan_type, asked->channel_count, unknown, asked->dwell_ms, asked->ssid_length,
	    asked->name_count, asked->flags);
	if (asked->channel_count != 0 && asked->channel_count <= 64 && what->frequency_count == 0) {
		error = EINVAL;                 /* (nothing of what is asked for is a channel of this radio's) */
	} else {
		lck_mtx_lock(&apple80211_lock);
		result_next = 0;
		lck_mtx_unlock(&apple80211_lock);
		error = hakios_sta_ask_scan(what);
	}
out:
	if (asked != NULL) {
		kfree_data(asked, sizeof(*asked));
	}
	if (what != NULL) {
		kfree_data(what, sizeof(*what));
	}
	return error;
}

static int
set_join(const struct apple80211req *request, char *said, size_t said_size)
{
	struct apple80211_assoc *asked;
	struct hakios_sta_join *what;
	int error;

	if (request->req_len != sizeof(*asked)) {     /* (16.5's 592 bytes put the fields elsewhere) */
		return EINVAL;
	}
	asked = kalloc_data(sizeof(*asked), Z_WAITOK | Z_ZERO);
	what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
	if (asked == NULL || what == NULL || (error = req_read(request, 0, asked, offsetof(struct apple80211_assoc, rest))) != 0) {
		error = asked == NULL || what == NULL ? ENOMEM : error;
		goto out;
	}
	/* (lengths and kinds only: the name, the access point and the key stay out of the log) */
	snprintf(said, said_size, " (a join: a name of %u bytes, %s access point, authentication %#x/%#x, a key of %u bytes of kind %u)",
	    asked->ssid_length, asked->bssid[0] | asked->bssid[1] | asked->bssid[2] | asked->bssid[3] | asked->bssid[4] |
	    asked->bssid[5] ? "a given" : "any", asked->auth_lower, asked->auth_upper, asked->key_length, asked->key_cipher);
	if (asked->ssid_length == 0 || asked->ssid_length > sizeof(what->ssid)) {
		error = EINVAL;
		goto out;
	}
	what->ssid_length = (uint8_t)asked->ssid_length;
	memcpy(what->ssid, asked->ssid, asked->ssid_length);
	memcpy(what->bssid, asked->bssid, sizeof(what->bssid));
	if (asked->key_length == 0) {
		what->security = HAKIOS_STA_OPEN;
	} else if (asked->key_cipher == CIPHER_PASSPHRASE && asked->key_length >= 8 && asked->key_length <= 63) {
		what->security = HAKIOS_STA_WPA2_PSK;
		what->passphrase_length = (uint8_t)asked->key_length;
		memcpy(what->passphrase, asked->key, asked->key_length);
	} else if (asked->key_cipher == CIPHER_PMK && asked->key_length == sizeof(what->pmk)) {
		what->security = HAKIOS_STA_WPA2_PSK;
		memcpy(what->pmk, asked->key, sizeof(what->pmk));
	} else {
		error = EINVAL;
		goto out;
	}
	lck_mtx_lock(&apple80211_lock);
	join_result = 0;
	lck_mtx_unlock(&apple80211_lock);
	error = hakios_sta_ask_join(what);
out:
	if (asked != NULL) {
		memset_s(asked, sizeof(*asked), 0, sizeof(*asked));
		kfree_data(asked, sizeof(*asked));
	}
	if (what != NULL) {
		memset_s(what, sizeof(*what), 0, sizeof(*what));
		kfree_data(what, sizeof(*what));
	}
	return error;
}

/* (11-28) what wifid has told about the phone (sets 483, 365, 485): for a power policy to read */
static uint8_t told_external_power;
static bool told_display_on = true;
static uint16_t told_app_state;

static int
set(const struct apple80211req *request, char *said, size_t said_size)
{
	switch (request->req_type) {
	case 20:
		return set_join(request, said, said_size);
	case 22:                                /* leave the network */
		return hakios_sta_ask_leave();
	case 19: {                              /* the radio's power */
		struct {
			uint32_t version, radios, state[4];
		} asked;
		int error;

		if (request->req_len < sizeof(asked) || (error = req_read(request, 0, &asked, sizeof(asked))) != 0) {
			return request->req_len < sizeof(asked) ? EINVAL : error;
		}
		snprintf(said, said_size, " (power: %s)", asked.state[0] ? "on" : "off");
		return hakios_sta_ask_power(asked.state[0] != 0);
	}
	case 10:
		return set_scan(request, said, said_size);
	/*
	 * (11-28) What wifid tells a driver about the phone, for its power policy: external power (483, a byte), the
	 * display on/off (365, the value), the foreground app's state (485, 2 bytes), its heartbeat (481), the antenna
	 * setup it wants (473, 4 bytes), the country it believes in (51: version and 2 letters). Kept and taken; the radio's
	 * own power saving and regulatory country are the firmware's (the CLM), not changed from here yet.
	 */
	case 483:
	case 485:
	case 473:
	case 51: {
		uint8_t told[8] = { 0 };
		size_t n = request->req_len < sizeof(told) ? request->req_len : sizeof(told);
		int error = n != 0 ? req_read(request, 0, told, n) : 0;

		if (error == 0) {
			lck_mtx_lock(&apple80211_lock);
			if (request->req_type == 483) {
				told_external_power = told[0];
			} else if (request->req_type == 485) {
				told_app_state = (uint16_t)(told[0] | told[1] << 8);
			}
			lck_mtx_unlock(&apple80211_lock);
			snprintf(said, said_size, " (told: %02x %02x %02x %02x)", told[0], told[1], told[2], told[3]);
		}
		return error;
	}
	case 365:
		lck_mtx_lock(&apple80211_lock);
		told_display_on = request->req_val != 0;
		lck_mtx_unlock(&apple80211_lock);
		snprintf(said, said_size, " (display %s)", request->req_val ? "on" : "off");
		return 0;
	case 481:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

/* ---- events -------------------------------------------------------------------------------------------- */

static void
post(uint32_t code, const void *data, uint32_t length)
{
	struct kev_msg message = { .vendor_code = KEV_VENDOR_APPLE, .kev_class = KEV_IEEE80211_CLASS, .kev_subclass = 1,
		                   .event_code = code };
	char name[16];

	memcpy(name, interface_name, sizeof(name));
	message.dv[0].data_length = sizeof(name);
	message.dv[0].data_ptr = name;
	if (length != 0) {
		message.dv[1].data_length = length;
		message.dv[1].data_ptr = __DECONST(void *, data);
	}
	kev_post_msg(&message);
}

/* What the station side says has happened (drivers/wifi/hakios.sta.c calls this, on its thread or the station's). */
void
hakios_apple80211_event(enum hakios_sta_event event, int status)
{
	static unsigned said;

	if (said < 200) {
		said++;
		printf("apple80211: the station says: %s, %d\n", event == HAKIOS_STA_POWER ? "power" : event == HAKIOS_STA_SCAN_DONE ?
		    "a scan is over" : event == HAKIOS_STA_JOIN_DONE ? "a join is over" : "the link", status);
	}
	switch (event) {
	case HAKIOS_STA_POWER:
		post(EVENT_POWER_CHANGED, NULL, 0);
		break;
	case HAKIOS_STA_SCAN_DONE:
		lck_mtx_lock(&apple80211_lock);
		result_next = 0;
		lck_mtx_unlock(&apple80211_lock);
		post(EVENT_SCAN_DONE, NULL, 0);
		break;
	case HAKIOS_STA_JOIN_DONE:
		lck_mtx_lock(&apple80211_lock);
		join_result = status == 0 ? 1 : status == EAUTH ? 21 : status == ETIMEDOUT ? 9 : 2;
		lck_mtx_unlock(&apple80211_lock);
		{
			/*
			 * (11-26) iOS 27's assoc-done event carries 0x21c bytes (_dispatchAsyncAssocEvent: ten records of 0x34
			 * bytes about the access points tried, then words at 0x208..0x218, 0x210 an extended status of
			 * 1000-1010 when there is one) and only then asks ASSOCIATE_RESULT how the join went. With no data it
			 * gave up at once with -3902 ("not supported", join_status 0xaaaaaaaa): every join reported failed.
			 * All zeros: no records, no extended status; the outcome is request 21's.
			 */
			static const uint8_t assoc_done[0x21c];

			post(EVENT_ASSOC_DONE, assoc_done, sizeof(assoc_done));
		}
		break;
	case HAKIOS_STA_LINK: {
		struct apple80211_link_changed link = { .down = status != 1, .involuntary = status != 1 && status != 0,
			                                .reason = status != 1 && status != 0 ? 4 : 0 };      /* (4: gone, not asked) */
		struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);

		if (state != NULL) {
			hakios_sta_state(state);
			link.rssi = status == 1 ? state->bss.rssi : 0;
			kfree_data(state, sizeof(*state));
		}
		post(EVENT_SSID_CHANGED, NULL, 0);
		post(EVENT_BSSID_CHANGED, NULL, 0);
		post(EVENT_LINK_CHANGED, &link, sizeof(link));
		break;
	}
	}
}

/* ---- the requests as they arrive --------------------------------------------------------------------- */

/*
 * How often each kind of request came and how it fared: said when the run ends, because the
 * lines said along the way can be lost with the middle of a long log.
 */
static struct {
	int32_t type;
	bool set;
	unsigned times, answered;
	int last_error;
} kinds[96];
static unsigned kind_count;

static void
note(const struct apple80211req *request, bool is_set, int error)
{
	unsigned i;

	lck_mtx_lock(&apple80211_lock);
	for (i = 0; i < kind_count && !(kinds[i].type == request->req_type && kinds[i].set == is_set); i++) {
	}
	if (i < sizeof(kinds) / sizeof(kinds[0])) {
		if (i == kind_count) {
			kinds[kind_count++] = (typeof(kinds[0])) { .type = request->req_type, .set = is_set };
		}
		kinds[i].times++;
		kinds[i].answered += error == 0;
		kinds[i].last_error = error != 0 ? error : kinds[i].last_error;
	}
	lck_mtx_unlock(&apple80211_lock);
}

void
hakios_apple80211_summary(void)
{
	static bool said;

	if (said) {
		return;
	}
	said = true;
	for (unsigned i = 0; i < kind_count; i++) {
		printf("apple80211: in this run: %s %d (%s) %u times, answered %u times%s", kinds[i].set ? "set" : "get", kinds[i].type,
		    type_name(kinds[i].type), kinds[i].times, kinds[i].answered, kinds[i].last_error == 0 ? "\n" : "");
		if (kinds[i].last_error != 0) {
			printf("; the last refusal: %d%s\n", kinds[i].last_error, kinds[i].last_error == EOPNOTSUPP ? " (not built)" : "");
		}
	}
}

/* One request, either way it came; the request log says which. */
static int
answer(struct call *call, bool is_set, const char *how)
{
	static unsigned said;
	struct apple80211req *request = &call->req;
	char who[32] = "?", what[200] = "";
	int error;

	proc_selfname(who, sizeof(who));
	error = is_set ? set(request, what, sizeof(what)) : get(request);
	note(request, is_set, error);
	/* (a request that is repeated for ever is said a few hundred times) */
	if (said < 800) {
		said++;
		printf("apple80211: %s %s %d (%s)%s, value %d, %u bytes%s: %s", who, is_set ? "sets" : "gets", request->req_type,
		    type_name(request->req_type), how, request->req_val, request->req_len, what, error == 0 ? "answered\n" : error ==
		    EOPNOTSUPP ? "REFUSED\n" : "");
		if (error != 0 && error != EOPNOTSUPP) {
			printf("error %d\n", error);
		}
	}
	return error;
}

/* An Apple80211 request for the radio's interface, from the process that is running now. */
errno_t
hakios_apple80211_ioctl(u_long command, void *data)
{
	struct call call = { .req = *(struct apple80211req *)data };
	int error;

	if (command != SIOCSA80211 && command != SIOCGA80211) {
		return EOPNOTSUPP;
	}
	error = answer(&call, command == SIOCSA80211, "");
	*(struct apple80211req *)data = call.req;       /* (req_val and req_len go back) */
	return error;
}

/*
 * (11-26) The same through IO80211.framework's user client (iokit/Kernel/hakios.wlan.cpp): `req`
 * is the request's struct apple80211req (40 bytes) with req_len set by the user client (what a get
 * has room for, what a set carries), and its buffer is in the kernel already, `capacity` bytes of
 * it; NULL for a request without one. req_val and req_len come back in `req`.
 */
int hakios_apple80211_request_kernel(bool is_set, void *req, void *buffer, size_t capacity);

int
hakios_apple80211_request_kernel(bool is_set, void *req, void *buffer, size_t capacity)
{
	struct call call = { .req = *(struct apple80211req *)req, .kernel_buffer = buffer, .capacity = capacity };
	int error;

	_Static_assert(sizeof(struct apple80211req) == 40, "the request as IO80211.framework has it");
	if (buffer == NULL) {
		call.req.req_data = USER_ADDR_NULL;
		call.req.req_len = 0;
	} else {
		call.req.req_data = (user_addr_t)(uintptr_t)buffer;     /* (not followed: req_read and req_write use the buffer) */
	}
	error = answer(&call, is_set, " (by the user client)");
	*(struct apple80211req *)req = call.req;
	return error;
}
