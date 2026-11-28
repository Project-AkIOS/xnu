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

/*
 * (11-28) The request types' names, all of them: iOS 27's own table (IO80211.framework, _convertApple80211IOCTLToString:
 * an array of 608 names indexed by the type, at 0x2d083b120 in 24B156's cache), APPLE80211_IOC_ left off. For the log.
 */
static const char *const type_names[] = {
	"", "SSID", "AUTH_TYPE", "CIPHER_KEY", "CHANNEL", "POWERSAVE", "PROTMODE", "TXPOWER", "RATE", "BSSID", "SCAN_REQ",
	"SCAN_RESULT", "CARD_CAPABILITIES", "STATE", "PHY_MODE", "OP_MODE", "RSSI", "NOISE", "INT_MIT", "POWER",
	"ASSOCIATE", "ASSOCIATE_RESULT", "DISASSOCIATE", "STATUS_DEV_NAME", "IBSS_MODE", "HOST_AP_MODE", "AP_MODE",
	"SUPPORTED_CHANNELS", "LOCALE", "DEAUTH", "COUNTERMEASURES", "FRAG_THRESHOLD", "RATE_SET", "SHORT_SLOT",
	"MULTICAST_RATE", "SHORT_RETRY_LIMIT", "LONG_RETRY_LIMIT", "TX_ANTENNA", "RX_ANTENNA", "ANTENNA_DIVERSITY", "ROM",
	"DTIM_INT", "STATION_LIST", "DRIVER_VERSION", "HARDWARE_VERSION", "RAND", "RSN_IE", "BACKGROUND_SCAN", "AP_IE_LIST",
	"STATS", "ASSOCIATION_STATUS", "COUNTRY_CODE", "DEBUG_FLAGS", "LAST_RX_PKT_DATA", "RADIO_INFO", "GUARD_INTERVAL",
	"MIMO_POWERSAVE", "MCS", "RIFS", "LDPC", "MSDU", "MPDU", "BLOCK_ACK", "PLS", "PSMP", "PHY_SUB_MODE",
	"MCS_INDEX_SET", "CACHE_THRESH_BCAST", "CACHE_THRESH_DIRECT", "WOW_PARAMETERS", "WOW_ENABLED", "40MHZ_INTOLERANT",
	"PID_LOCK", "STA_IE_LIST", "STA_AUTHORIZE", "STA_DISASSOCIATE", "STA_DEAUTH", "RSN_CONF", "KEY_RSC", "STA_STATS",
	"ROAM_THRESH", "VENDOR_DBG_FLAGS", "CACHE_AGE_THRESH", "PMK_CACHE", "LINK_QUAL_EVENT_PARAMS", "IE",
	"SCAN_REQ_MULTIPLE", "BTCOEX_MODE", "WOW_TEST", "CLEAR_PMKSA_CACHE", "SCANCACHE_CLEAR", "P2P_ENABLE", "P2P_LISTEN",
	"P2P_SCAN", "VIRTUAL_IF_CREATE", "VIRTUAL_IF_DELETE", "VIRTUAL_IF_ROLE", "VIRTUAL_IF_PARENT", "P2P_GO_CONF",
	"P2P_NOA_LIST", "P2P_OPP_PS", "P2P_CT_WINDOW", "BT_COEX_FLAGS", "CURRENT_NETWORK", "BT_POWER", "AVAILABILITY",
	"RSSI_BOUNDS", "ROAM", "TX_CHAIN_POWER", "CDD_MODE", "LAST_BCAST_SCAN_TIME", "THERMAL_THROTTLING", "FACTORY_MODE",
	"REASSOCIATE", "", "POWER_DEBUG_INFO", "AWDL_SYNC_PARAMS", "AWDL_SYNC_ENABLED",
	"AWDL_EXTENSION_STATE_MACHINE_PARAMETERS", "AWDL_SERVICE_PARAMS", "AWDL_PEER_SERVICE_REQUEST",
	"AWDL_ELECTION_ALGORITHM_ENABLED", "AWDL_ELECTION_ID", "AWDL_MAX_TREE_DEPTH", "AWDL_GUARD_TIME", "AWDL_BSSID",
	"AWDL_ELECTION_METRIC", "AWDL_AVAILABILITY_WINDOW_AP_ALIGNMENT", "AWDL_SYNC_FRAME_AP_BEACON_ALIGNMENT",
	"AWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE", "PEER_CACHE_MAXIMUM_SIZE", "AWDL_OUI", "AWDL_MASTER_CHANNEL",
	"AWDL_TOP_MASTER", "AWDL_SYNC_STATE", "AWDL_ELECTION_RSSI_THRESHOLDS", "AWDL_PRESENCE_MODE",
	"AWDL_ELECTION_MASTER_COUNTS", "AWDL_PERIODIC_SYNC_FRAME_PACKET_LIFETIME", "AWDL_MASTER_MODE_SYNC_FRAME_PERIOD",
	"AWDL_NON_ELECTION_MASTER_MODE_SYNC_FRAME_PERIOD", "AWDL_EXPLICIT_AVAILABILITY_WINDOW_EXTENSION_OPT_OUT",
	"AWDL_GET_AWDL_MASTER_DATABASE", "PEER_CACHE_CONTROL", "AWDL_BATTERY_LEVEL",
	"AWDL_BT_COEX_AW_PROTECTED_PERIOD_LENGTH", "AWDL_BT_COEX_AGREEMENT", "AWDL_BT_COEX_AGREEMENT_ENABLED",
	"AWDL_STRATEGY", "AWDL_OOB_REQUEST", "AWDL_MAX_NO_MASTER_PERIODS", "AWDL_SYNC_FRAME_TEMPLATE", "LOG_FLAGS",
	"PEER_STATS", "HT_CAPABILITY", "AWDL_ELECTION_PARAMS", "LINK_CHANGED_EVENT_DATA", "GET_DEBUG_INFO",
	"AWDL_DEVICE_CAPABILITIES", "AWDL_RSSI_MEASUREMENT_REQUEST", "AWDL_AES_KEY", "AWDL_SCAN_RESERVED_TIME", "AWDL_CTL",
	"AWDL_SOCIAL_TIME_SLOTS", "AWDL_PEER_TRAFFIC_REGISTRATION", "EXTENDED_STATS", "BEACON_PERIOD",
	"AWDL_FORCED_ROAM_CONFIG", "AWDL_QUIET", "ACL_POLICY", "ACL_ADD", "ACL_REMOVE", "ACL_FLUSH", "ACL_LIST",
	"CHAIN_ACK", "DESENSE", "OFFLOAD_SCANNING", "OFFLOAD_RSN", "OFFLOAD_COUNTRY_CODE", "OFFLOAD_KEEPALIVE_L2",
	"OFFLOAD_ARP_NDP", "VHT_MCS_INDEX_SET", "DWDS", "INTERRUPT_STATS", "INTERRUPT_STATS_RESET", "TIMER_STATS",
	"TIMER_STATS_RESET", "OFFLOAD_STATS", "OFFLOAD_STATS_RESET", "OFFLOAD_BEACONS", "ROAMING", "OFFLOAD_ARP",
	"OFFLOAD_NDP", "OFFLOAD_SCAN", "DESENSE_LEVEL", "MCS_VHT", "TX_NSS", "GAS_REQ", "GAS_START", "GAS_SET_PEER",
	"GAS_RESULTS", "AWDL_BTLE_PEER_INDICATION", "AWDL_BTLE_STATE_PARAMS", "AWDL_PEER_DATABASE",
	"AWDL_BTLE_ENABLE_SYNC_WITH_PARAMS", "AWDL_SECONDARY_MASTER_CHANNEL", "PHY_STATS", "CHANNELS_INFO",
	"AWDL_AF_TX_MODE", "ERROR_STRING", "ERROR_NO", "AWDL_PIGGYBACK_SCAN_REQ", "AWDL_PRIVATE_ELECTION_ID",
	"AWDL_MIN_RATE", "VHT_CAPABILITY", "BGSCAN_CACHE_RESULTS", "ROAM_PROFILE", "AWDL_OPER_MODE", "RESTORE_DEFAULTS",
	"AWDL_ENCRYPTION_KEYS", "AWDL_ENCRYPTION_TYPE", "BTCOEX_PROFILES", "BTCOEX_CONFIG", "AWDL_STATISTICS",
	"AWDL_ENABLE_ROAMING", "AWDL_OOB_AUTO_REQUEST", "AWDL_TXCAL_PERIOD", "CHIP_COUNTER_STATS", "DBG_GUARD_TIME_PARAMS",
	"AWDL_AWDL_ADVERTISERS", "LEAKY_AP_STATS_MODE", "CAPTURE", "LEAKY_AP_STATS", "AWDL_BLOCK_SET_COMMANDS",
	"LEAKY_AP_AWD_MODE", "BTCOEX_OPTIONS", "FORCE_SYNC_TO_PEER", "COUNTRY_CHANNELS", "PRIVATE_MAC", "RESET_CHIP",
	"CRASH", "RANGING_ENABLE", "RANGING_START", "RANGING_AUTHENTICATE", "AWDL_PREFERRED_CHANNELS",
	"LEAKY_AP_SSID_STATS", "AWDL_RSDB_CAPS", "AWDL_DEV_STATS", "LAST_ASSOC_HISTORY", "AWDL_COMMON_CHANNEL",
	"AWDL_PEERS_INFO", "TKO_PARAMS", "TKO_DUMP", "AWDL_NEARBY_LOG_TRIGGER", "HW_SUPPORTED_CHANNELS", "BTCOEX_PROFILE",
	"BTCOEX_PROFILE_ACTIVE", "TRAP_INFO", "THERMAL_INDEX", "MAX_NSS_FOR_AP", "BTCOEX_2G_CHAIN_DISABLE", "POWER_BUDGET",
	"AWDL_DFSP_CONFIG", "AWDL_DFSP_UCSA_CONFIG", "SCAN_BACKOFF_REPORT", "OFFLOAD_TCPKA_ENABLE", "RANGING_CAPS",
	"SUPPRESS_SCANS", "NAN_ENABLED", "NAN_INIT", "NAN_CLUSTER_ID", "NAN_DISC_BCN_PERIOD", "NAN_SYNC_BCN_PERIOD",
	"NAN_DW_AWAKE_PERIOD", "NAN_BCN_TX_CONTROL", "NAN_HOST_ELECTION", "NAN_RSSI_CLOSE", "NAN_RSSI_MIDDLE",
	"NAN_RSSI_5G_CLOSE", "NAN_RSSI_5G_MIDDLE", "NAN_DEVICE_INFO", "NAN_MASTER_PREFERENCE", "NAN_RANDOM_FACTOR",
	"NAN_RANDOM_FACTOR_ROTATION", "NAN_HOP_COUNT_TO_ANCHOR_MASTER", "NAN_MASTER_RANK", "NAN_MAX_PEERS",
	"NAN_MASTER_CHANNEL", "NAN_SECONDARY_MASTER_CHANNEL", "NAN_TX_RATE", "NAN_WARMUP_TIME",
	"NAN_CURRENT_ANCHOR_MASTER_RECORD", "NAN_LAST_ANCHOR_MASTER_RECORD", "COMPANION_SKYWALK_IF_CREATE", "NAN_CW",
	"NAN_CW_RS", "NAN_SYNC_STATS", "NAN_HOP_COUNT_LIMIT", "NAN_AF_TX", "NAN_CHANNEL_SEQUENCE", "NAN_PUBLISH",
	"NAN_CANCEL_PUBLISH", "NAN_PUBLISHED_SERVICE_LIST", "NAN_SUBSCRIBE", "NAN_SUBSCRIBED_SERVICE_LIST",
	"NAN_PUBLISHERS_LIST", "NAN_CANCEL_SUBSCRIBE", "NAN_FOLLOW_UP_TRANSMIT", "NAN_DISCOVERY_RESULT",
	"NAN_FOLLOW_UP_RECEIVE", "NAN_SERVICE_DISCOVERY_STATS", "NAN_SERVICE_DESCRIPTOR_EXTENSION", "NAN_RANGING_REQUEST",
	"NAN_RANGING_RESPONSE", "NAN_RANGING_CANCEL", "NAN_DP_REQ", "NAN_DP_RESP", "NAN_SCHEDULE_UPDATE", "NAN_DP_END",
	"NAN_DP_CONF", "NAN_ADVERTISERS", "NAN_COMMITTED_AVAILABILITY", "NAN_POTENTIAL_AVAILABILITY",
	"NAN_DEVICE_CAPABILITY", "NAN_NDC_AVAILABILITY", "NAN_IMMUTABLE_AVAILABILITY", "NAN_RANGING_AVAILABILITY",
	"NAN_PEER_DATABASE", "NAN_CTL", "NAN_DP_INFO", "NAN_VENDOR_PAYLOAD", "NAN_ELECTION_ROLE", "NAN_QOS_PARAMS",
	"NAN_FAST_DISCOVERY", "RADIO_CHANSEQ_CONTROL", "NAN_DATA_PATH_RESTORE", "HOST_AP_MODE_HIDDEN", "LQM_CONFIG",
	"AWDL_CCA", "TRAP_CRASHTRACER_MINI_DUMP", "TRAFFIC_REGISTRATION", "AWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES",
	"AWDL_OOB_SEC_PAYLOAD_REQUEST", "AWDL_LOW_LATENCY_STATISTICS", "LLW_PARAMS", "DYNAMIC_RSSI_WINDOW_CONFIG",
	"BEACON_INFO", "SOFTAP_PARAMS", "CHIP_POWER_RANGE", "SOFTAP_TRIGGER_CSA", "SOFTAP_STATS",
	"AWDL_SIDECAR_DIAGNOSTICS", "SOFTAP_WIFI_NETWORK_INFO_IE", "NSS", "", "NAN_LLW_PARAMS", "HP2P_CAPS", "RLLW_STATS",
	"BT_COEX_ULOFDMA_DISABLE", "HW_ADDR", "SCAN_CONTROL", "HE_MCS_INDEX_SET", "CHIP_DIAGS", "USB_HOST_NOTIFICATION",
	"LOWLATENCY_STATISTICS", "DISPLAY_STATE", "NAN_OOB_AF_TX", "NAN_DATA_PATH_KEEP_ALIVE_IDENTIFIER", "SET_MAC_ADDRESS",
	"ASSOCIATE_EXTENDED_RESULT", "AWDL_AIRPLAY_STATISTICS", "HP2P_CTRL", "BSS_BLACKLIST", "ASSOC_READY_STATUS",
	"TXRX_CHAIN_INFO", "MIMO_STATUS", "CUR_PMK", "DYNSAR_DETAIL", "RANDOMISATION_STATUS", "COUNTRY_CHANNELS_INFO",
	"SET_RANGING_IDENTIFIER", "SET_RANGING_FOLLOW_MODE", "ABORT_SCAN", "SET_PROPERTY", "LQM_METRICS_CRASHTRACER_DATA",
	"NAN_DP_LINK_STATUS", "LQM_SUMMARY", "COLOCATED_NETWORK_SCOPE_ID", "ROAM_CACHE_UPDATE", "USERSPACE_P2P_OPTIONS",
	"BEACON_SCAN_CACHE_REQ", "SLOW_WIFI_FEATURE_ENABLED", "PM_MODE", "CCA", "SET_WIFI_ASSERTION_STATE",
	"AWDL_UPDATE_SERVICE_COUNT", "LQM_RING_BUFFER_DATA", "RX_RATE", "NAN_RESTRICT_POWER_SAVE",
	"REASSOCIATE_WITH_CORECAPTURE", "NAN_INFRA_STATUS", "NAN_INFRA_RELAY_PACKETS_DUMP", "LINKDOWN_DEBOUNCE_STATUS",
	"SOFTAP_EXTENDED_CAPABILITIES_IE", "ENABLE_PACKET_TS", "DISABLE_PACKET_TS", "TIMESYNC_INFO", "REALTIME_QOS_MSCS",
	"", "", "", "TX_RATE", "NANPHS_ASSOCIATION", "NANPHS_TERMINATED", "P2P_COEX_PROTECTION", "POST_PRESENCE_DONE",
	"AWDL_MI_BITMAP", "AWDL_UMI_TX_PEER", "PSF_DWELL", "LTE_RESTRICTED_CHANNELS", "NANPH_SOFTAP_CSA", "IOR_LEGEND",
	"6G_MODE", "COUNTRY_BAND_SUPPORT", "WCL_FW_HOT_CHANNELS", "WCL_LEAVE_NETWORK", "WCL_REASSOC",
	"WCL_LOW_LATENCY_INFO", "WCL_SET_ROAM_LOCK", "WCL_LEGACY_ROAM_PROFILE_CONFIG", "WCL_ROAM_PROFILE_CONFIG",
	"WCL_ROAM_USER_CACHE", "AWDL_CONFIG", "WCL_BSS_INFO", "WCL_TRAFFIC_COUNTERS", "WCL_SCAN_ABORT",
	"WCL_REAL_TIME_MODE", "WCL_ARP_MODE", "WCL_JOIN_ABORT", "WCL_TRIGGER_CC", "WCL_GET_TX_BLANKING_STATUS",
	"WCL_SCAN_REQ", "WCL_ASSOCIATE", "WCL_QOS_PARAMS", "WCL_LINK_UP_DONE", "SSID_TRANSITION_SUPPORT",
	"WCL_SET_SCAN_HOME_AWAY_TIME", "HE_COUNTERS", "GAME_MODE_STATE", "WCL_CHANNELS_INFO", "GATEWAY_ADDRESS",
	"UNICAST_INFRA_PEER_ADDRESS", "INFRA_PEERS_LOGGING_ENABLED", "VOICE_IND_STATE", "", "SLOW_WIFI_RECOVERY",
	"P2P_STEERING_METRIC", "RSN_XE", "SIB_COEX_STATUS", "MWS_ACCESSORY_POWER_LIMIT_WIFI_ENH", "WCL_EXTENDED_BSS_INFO",
	"WCL_ULOFDMA_STATE", "WCL_ACTION_FRAME", "WCL_LOW_LATENCY_INFO_STATS", "WCL_ADVISORTY_INFO", "WCL_TX_RX_LATENCY",
	"ACTIVE_SOCKETS", "AWDL_RTG_PEER_STATS_CONFIG", "GAS_ABORT", "OS_FEATURE_FLAGS", "DHCP_RENEWAL_DATA",
	"MOVING_NETWORK", "BATTERY_POWERSAVE_CONFIG", "MIMO_CONFIG", "WCL_CONFIG_BG_MOTIONPROFILE", "WCL_CONFIG_BG_NETWORK",
	"WCL_CONFIG_BGSCAN", "WCL_CONFIG_BG_PARAMS", "WCL_BGSCAN_CACHE_RESULT", "POWER_PROFILE", "FASTLANE_PROFILE_STATE",
	"HEARTBEAT", "INTERFACE_SETTING", "EXTERNAL_POWER_STATE", "APP_REPORT", "APP_STATE", "BYPASS_TX_POWER_CAP",
	"DPS_SYMPTOMS_INPUT", "FACETIME_WIFICALLING_PARAMS", "IPV4_PARAMS", "PRODUCT_INFO", "WCL_WNM_OPS",
	"WCL_WNM_OFFLOAD", "WCL_LIMITED_AGGREGATION", "WCL_BCN_MUTE_CONFIG", "AWDL_MAC_ADDRESS_IN_USE", "WOW_LPAS_CONFIG",
	"EAP_FILTER_CONFIG", "WOW_LOW_POWER_MODE", "DUAL_POWER_MODE", "BGSCAN_PARAMS", "BGSCAN_DYNAMIC_PROFILE_PARAMS",
	"WCL_UPDATE_FAST_LANE", "VIRTUAL_IF_STATE", "WCL_ASSOCIATED_SLEEP", "", "", "CONGESTION_CTRL_IND", "MIS_MAX_STA",
	"STAND_ALONE_MODE_STATE", "IPV6_PARAMS", "TRIGGER_DISCONNECT", "INFRA_ENUMERATED", "LMTPC_CONFIG",
	"TRAFFIC_ENG_PARAMS", "WIFI_NOISE_PER_ANT", "BLOCKED_BANDS", "", "SYSTEM_STATE", "R1_COEX_ULOFDMA_DISABLE",
	"RC2_COEX_ULOFDMA_DISABLE", "LE_SCAN_PARAM", "TIMESYNC_GPIO", "HOST_CLOCK_INFO", "FW_CLOCK_INFO", "FW_CLOCK_SOURCE",
	"TIMESYNC_TX_POLICY", "TIMESYNC_RX_POLICY", "TIMESTAMPING_EN", "TIMESYNC_STATS", "WCL_SOI_CONFIG", "ROAM_PARAMS",
	"ROAM_ADAPTIVE_PARAMS", "STATUS_FOR_SCAN_UUID", "PLATFORM_CONFIG", "SYSTEM_SLEEP_CONFIG", "SLOWWIFI_PROBE_RESULT",
	"SLOWWIFI_USER_INPUT_RESULT", "SLOWWIFI_SYMPTOMS_TRIGGER", "MWS_TIME_SHARING_WIFI_ENH",
	"MWS_WIFI_TYPE_7_BITMAP_WIFI_ENH", "MWS_COEX_BITMAP_WIFI_ENH", "MWS_DISABLE_OCL_BITMAP_WIFI_ENH",
	"MWS_RFEM_CONFIG_WIFI_ENH", "MWS_ASSOC_PROTECTION_BITMAP_WIFI_ENH", "MWS_SCAN_FREQ_WIFI_ENH",
	"MWS_SCAN_FREQ_MODE_WIFI_ENH", "MWS_CONDITION_ID_BITMAP_WIFI_ENH", "MWS_ANTENNA_SELECTION_WIFI_ENH", "", "",
	"NDD_REQ", "DEVICE_TYPE_IN_DHCP_ALLOW", "NAN_CREATE_GCR_SESSION", "NAN_JOIN_GCR_SESSION", "NAN_GCR_RATE",
	"NAN_GCR_LINK_CONDITION", "DBRG_ENTROPY", "SDB_ENABLE", "NAN_PAIRING_KEYS", "PROTOCOL_CLEANUP_COMPLETE",
	"SMARTCCA_OPMODE", "BTCOEX_EXT_PROFILE", "CONNECTION_ID", "FILTER_MONITOR_EVENTS", "TD_IMMINENT", "HAS_BATTERY",
	"NAN_CHANNEL_SEQ_MISMATCH_COUNT", "LQM_STATISTICS", "NAN_MATCHING_SLOTS_COUNT", "HE_CAPABILITY",
	"DEVICE_ORIENTATION", "ACCESSORY_STATE", "P2P_DEVICE_CAPABILITY", "POWERTABLE_VERSION", "OS_ELIGIBILITY", "",
	"NAN_COUNTRY_CODE_SYNCING", "TX_MODE_CONFIG", "NAN_VENDOR_PAYLOAD_COMPACT", "NAN_ADDITIONAL_ATTR", "NAN_CCA",
	"REAL_TIME_3PARTY_APP", "NAN_MULTICAST_PEER_OP", "NB_RANGING_SESSION", "ROAM_TRAIN_DATA", "", "", "MCS_HE",
	"MCS_BE", "BE_MCS_INDEX_SET", "ROAM_MODEL_SSID", "MOTION_STATE", "MITIGATE_INTERFERENCE",
	"NAN_CHANNEL_SEQ_TIME_SLOTS", "NAN_FORCE_CHANNEL_SEQ", "MAX_LINK_SPEED", "NAN_KEEP_ALIVE_OOB_AF_OFFLOAD",
	"POWER_STATUS", "ELNA_POWER_GAIN", "ROAM_SLEEP_HISTORY", "BW_CHANGE", "RC2_BT_COEX_PARAM", "", "", "P2P_STATE",
	"SUSPEND_INTERFACE", "AGGRESSIVE_EDCA",
};

static const char *
type_name(int32_t type)
{
	return type >= 0 && (size_t)type < sizeof(type_names) / sizeof(type_names[0]) && type_names[type][0] ? type_names[type] : "?";
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
#define PHY_11AX                0x100u          /* (11-28: IO80211's convertPhyTypeToString: 0x100 "11AX", 0x200 "11BE") */
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
		/* (11-28) HE (802.11ax): the extension element (255) whose extension ID is 35, HE Capabilities */
		if (bss->elements[at] == 255 && bss->elements[at + 1] >= 1 && bss->elements[at + 2] == 35) {
			phy |= PHY_11AX;
		}
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
	case 57:                                /* (11-28) the rate's MCS index, in the value */
	case 353:                               /* ... and its spatial streams (NSS) */
		if (state->phase != HAKIOS_STA_JOINED || state->nss == 0) {
			error = ENXIO;
		} else {
			request->req_val = request->req_type == 57 ? state->mcs : state->nss;
			error = 0;
		}
		break;
	case 5:                                 /* (11-28) power saving, in the value: 0 off, 1 802.11 power save */
		if (state->powersave < 0) {
			error = ENXIO;
		} else {
			request->req_val = state->powersave;
			error = 0;
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
		} answer = { .version = 1, .supported = PHY_11A | PHY_11B | PHY_11G | PHY_11N | PHY_11AC | PHY_11AX };

		if (state->phase == HAKIOS_STA_JOINED) {
			uint32_t phy = bss_phy(&state->bss);

			/* the newest both have */
			answer.active = phy & PHY_11AX ? PHY_11AX : phy & PHY_11AC ? PHY_11AC : phy & PHY_11N ? PHY_11N : phy & PHY_11G ? PHY_11G :
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
			/* (11-28) COUNTRY_CODE: {version, two letters}: the radio is put under that country's rules */
			if (request->req_type == 51 && n >= 6 && told[4] >= 'A' && told[4] <= 'Z' && told[5] >= 'A' && told[5] <= 'Z') {
				(void)hakios_sta_ask_country((const char *)&told[4]);
			}
			snprintf(said, said_size, " (told: %02x %02x %02x %02x %02x %02x %02x %02x)", told[0], told[1], told[2], told[3],
			    told[4], told[5], told[6], told[7]);
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
	/*
	 * (11-28) More of what wifid tells a driver (names from iOS's table): SET_PROPERTY (383), DHCP_RENEWAL_DATA (470),
	 * MOVING_NETWORK (471), POWER_PROFILE (479), FASTLANE_PROFILE_STATE (480), INTERFACE_SETTING (482), IPV4_PARAMS
	 * (489) and IPV6_PARAMS (510) -- for the firmware's offloads (ARP, keepalive), INFRA_ENUMERATED (512), ROAM_PARAMS
	 * (531), DEVICE_TYPE_IN_DHCP_ALLOW (552), PROTOCOL_CLEANUP_COMPLETE (560), FILTER_MONITOR_EVENTS (564),
	 * ACCESSORY_STATE (572), REAL_TIME_3PARTY_APP (582). Taken: nothing of them is used by the radio yet (no offloads,
	 * no roaming of our own); what they carry is not logged (addresses are in some), only SET_PROPERTY's first words.
	 */
	case 383: {
		uint32_t head[4] = { 0 };

		if (request->req_len >= sizeof(head) && req_read(request, 0, head, sizeof(head)) == 0) {
			snprintf(said, said_size, " (property: %08x %08x %08x %08x)", head[0], head[1], head[2], head[3]);
		}
		return 0;
	}
	case 470:
	case 471:
	case 479:
	case 480:
	case 482:
	case 489:
	case 510:
	case 512:
	case 531:
	case 552:
	case 560:
	case 564:
	case 572:
	case 582:
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
