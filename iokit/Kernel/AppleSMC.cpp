/*
 * AppleSMC the way an iPhone presents it (11-27): the service libSMC opens (IOServiceMatching("AppleSMC"),
 * IOServiceOpen type 0, then method 0 "open") and asks for the system's keys through method 2, one SMCKeyData
 * structure (168 bytes) in and out.
 *
 * On an iPhone the SMC is a coprocessor (the device tree's /arm-io/smc, AppleSMC on its iop-smc-nub over RTBuddy), and
 * its keys carry what its firmware measures and drives: the PMU, the charger, temperatures, power. Here there is no such
 * coprocessor: the keys are the kernel's own, a table drivers add to (akios_smc_add_key), each read from its driver
 * when asked. tools/ak/adt.py puts the two nodes into the device tree, GenericARMPE publishes the nub,
 * iokit/KernelConfigTables.cpp's personality matches it.
 *
 * Why it is needed even with no keys: without an AppleSMC, libSMC's SMCOpenConnectionWithDefaultService still hands
 * back a connection (with port 0), SMCGetKeyFromIndex then fails with -2 for every index, and PowerlogLiteOperators'
 * walk over the keys -- "until -4 (index out of range)" -- never ended: PerfPowerServices spun on one core all the time
 * (11-27). The real answer to an index past the last key is result 0xb8, which libSMC turns into -4.
 *
 * SMCKeyData (libSMC 24B156: SMCGetKeyFromIndex, its call helper checks keyInfo.dataSize <= 120 and sends 0xa8 bytes):
 *   0x00 key (a four-character code as a 32-bit number)    0x04 vers (6 bytes)      0x0a pLimitData (16 bytes)
 *   0x1c keyInfo: dataSize (u32), dataType (u32, four-character code), dataAttributes (u8 at 0x24)
 *   0x28 result    0x29 status    0x2a data8 (the command)    0x2c data32    0x30 bytes[120]
 * Commands (data8): 5 read a key, 6 write a key, 8 the key at index data32, 9 a key's info.
 * Results: 0 success, 0x84 no such key, 0x85 not readable, 0x86 not writable, 0x87 size mismatch, 0xb8 index past the
 * last key, 0x89 bad argument.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/locks.h>
#include <libkern/OSByteOrder.h>
#include <sys/proc.h>

#define SMC_CMD_READ_KEY        5
#define SMC_CMD_WRITE_KEY       6
#define SMC_CMD_KEY_AT_INDEX    8
#define SMC_CMD_KEY_INFO        9

#define SMC_SUCCESS             0x00
#define SMC_KEY_NOT_FOUND       0x84
#define SMC_KEY_NOT_READABLE    0x85
#define SMC_KEY_NOT_WRITABLE    0x86
#define SMC_KEY_SIZE_MISMATCH   0x87
#define SMC_BAD_ARGUMENT        0x89
#define SMC_INDEX_RANGE         0xb8

#define SMC_ATTR_READ           0x80
#define SMC_ATTR_WRITE          0x40

#define FOURCC(a, b, c, d)      ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

struct SMCKeyData {
	uint32_t key;
	uint8_t  vers[6];
	uint8_t  pLimitData[16];
	uint8_t  pad0[2];
	uint32_t dataSize;
	uint32_t dataType;
	uint8_t  dataAttributes;
	uint8_t  pad1[3];
	uint8_t  result;
	uint8_t  status;
	uint8_t  data8;
	uint8_t  pad2;
	uint32_t data32;
	uint8_t  bytes[120];
};
static_assert(sizeof(struct SMCKeyData) == 0xa8, "SMCKeyData is 168 bytes");
static_assert(__builtin_offsetof(struct SMCKeyData, dataSize) == 0x1c, "keyInfo at 0x1c");
static_assert(__builtin_offsetof(struct SMCKeyData, result) == 0x28, "result at 0x28");
static_assert(__builtin_offsetof(struct SMCKeyData, bytes) == 0x30, "bytes at 0x30");

/* ---- the key table: what the kernel's drivers serve ---- */

typedef int (*akios_smc_read_t)(uint32_t key, uint8_t * out, uint32_t size, void * context);

struct smc_key {
	uint32_t key;
	uint32_t type;
	uint32_t size;
	akios_smc_read_t read;
	void * context;
};

#define SMC_MAX_KEYS 64
static struct smc_key gKeys[SMC_MAX_KEYS];
static uint32_t gKeyCount;
static LCK_GRP_DECLARE(gKeysGroup, "AppleSMC keys");
static LCK_MTX_DECLARE(gKeysLock, &gKeysGroup);   /* from the start: drivers may add keys before AppleSMC starts */

/* "#KEY": how many keys there are, the SMC's own first key */
static int
smc_key_count(uint32_t, uint8_t * out, uint32_t size, void *)
{
	uint32_t count = OSSwapHostToBigInt32(gKeyCount);

	if (size != sizeof(count)) {
		return -1;
	}
	memcpy(out, &count, sizeof(count));
	return 0;
}

/*
 * A driver's key: KEY and TYPE four-character codes (FOURCC), SIZE bytes, READ fills them (big-endian, as the SMC's
 * numbers are) and answers 0, or not 0 when it cannot now. Kept sorted by key, as the SMC lists them.
 */
extern "C" bool
akios_smc_add_key(uint32_t key, uint32_t type, uint32_t size, akios_smc_read_t read, void * context)
{
	bool added = false;

	if (size == 0 || size > sizeof(((struct SMCKeyData *) NULL)->bytes) || read == NULL) {
		return false;
	}
	lck_mtx_lock(&gKeysLock);
	if (gKeyCount < SMC_MAX_KEYS) {
		uint32_t at = 0;

		while (at < gKeyCount && gKeys[at].key < key) {
			at++;
		}
		if (at == gKeyCount || gKeys[at].key != key) {
			memmove(&gKeys[at + 1], &gKeys[at], (gKeyCount - at) * sizeof(gKeys[0]));
			gKeys[at] = (struct smc_key){ key, type, size, read, context };
			gKeyCount++;
			added = true;
		}
	}
	lck_mtx_unlock(&gKeysLock);
	return added;
}

static const struct smc_key *
smc_find(uint32_t key)
{
	for (uint32_t i = 0; i < gKeyCount; i++) {
		if (gKeys[i].key == key) {
			return &gKeys[i];
		}
	}
	return NULL;
}

static void
smc_answer(const struct SMCKeyData * in, struct SMCKeyData * out)
{
	const struct smc_key * k;

	bzero(out, sizeof(*out));
	out->key = in->key;
	lck_mtx_lock(&gKeysLock);
	switch (in->data8) {
	case SMC_CMD_KEY_AT_INDEX:
		if (in->data32 >= gKeyCount) {
			out->result = SMC_INDEX_RANGE;
		} else {
			out->key = gKeys[in->data32].key;
		}
		break;
	case SMC_CMD_KEY_INFO:
		if ((k = smc_find(in->key)) == NULL) {
			out->result = SMC_KEY_NOT_FOUND;
		} else {
			out->dataSize = k->size;
			out->dataType = k->type;
			out->dataAttributes = SMC_ATTR_READ;
		}
		break;
	case SMC_CMD_READ_KEY:
		if ((k = smc_find(in->key)) == NULL) {
			out->result = SMC_KEY_NOT_FOUND;
		} else if (in->dataSize != 0 && in->dataSize != k->size) {
			out->result = SMC_KEY_SIZE_MISMATCH;
		} else if (k->read(k->key, out->bytes, k->size, k->context) != 0) {
			out->result = SMC_KEY_NOT_READABLE;
		} else {
			out->dataSize = k->size;
			out->dataType = k->type;
		}
		break;
	case SMC_CMD_WRITE_KEY:
		out->result = smc_find(in->key) == NULL ? SMC_KEY_NOT_FOUND : SMC_KEY_NOT_WRITABLE;
		break;
	default:
		out->result = SMC_BAD_ARGUMENT;
		break;
	}
	lck_mtx_unlock(&gKeysLock);
}

/* ---- the service ---- */

class AppleSMC : public IOService
{
	OSDeclareDefaultStructors(AppleSMC);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleSMC, IOService);

bool
AppleSMC::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	akios_smc_add_key(FOURCC('#', 'K', 'E', 'Y'), FOURCC('u', 'i', '3', '2'), 4, smc_key_count, NULL);
	registerService();
	IOLog("AppleSMC: on %s, %u key(s); its user client AppleSMCClient\n", provider->getName(), gKeyCount);
	return true;
}

/* ---- the user client: 0 open, 1 close, 2 one SMCKeyData in and out ---- */

class AppleSMCClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleSMCClient);

	static IOReturn sOpen(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sClose(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sKeyData(OSObject * target, void * reference, IOExternalMethodArguments * args);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleSMCClient, IOUserClient);

IOReturn
AppleSMCClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
AppleSMCClient::sOpen(OSObject *, void *, IOExternalMethodArguments *)
{
	return kIOReturnSuccess;
}

IOReturn
AppleSMCClient::sClose(OSObject *, void *, IOExternalMethodArguments *)
{
	return kIOReturnSuccess;
}

IOReturn
AppleSMCClient::sKeyData(OSObject *, void *, IOExternalMethodArguments * args)
{
	if (args->structureInput == NULL || args->structureOutput == NULL ||
	    args->structureInputSize < sizeof(struct SMCKeyData) || args->structureOutputSize < sizeof(struct SMCKeyData)) {
		return kIOReturnBadArgument;
	}
	smc_answer((const struct SMCKeyData *) args->structureInput, (struct SMCKeyData *) args->structureOutput);
	args->structureOutputSize = sizeof(struct SMCKeyData);
	return kIOReturnSuccess;
}

IOReturn
AppleSMCClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch *, OSObject *, void * reference)
{
	/* { function, scalars in, structure in, scalars out, structure out } */
	static const IOExternalMethodDispatch methods[] = {
		{ &AppleSMCClient::sOpen, 0, 0, 0, 0 },
		{ &AppleSMCClient::sClose, 0, 0, 0, 0 },
		{ &AppleSMCClient::sKeyData, 0, sizeof(struct SMCKeyData), 0, sizeof(struct SMCKeyData) },
	};

	if (selector >= sizeof(methods) / sizeof(methods[0])) {
		return kIOReturnUnsupported;
	}
	return IOUserClient::externalMethod(selector, args, (IOExternalMethodDispatch *) &methods[selector], this, reference);
}
