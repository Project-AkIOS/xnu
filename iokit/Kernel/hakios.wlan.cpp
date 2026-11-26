/*
 * HakiosWLAN: the registry entries by which Darwin finds the Wi-Fi interface.
 *
 * en0 is made with the BSD interface KPI (drivers/net/hakios.net.c) and has no IOKit object behind
 * it of its own; Darwin looks for two, as IO80211Family would have made them:
 *
 *   - wifid's device is whatever service has the property IO80211InterfaceRole =
 *     "Infrastructure", and IO80211.framework binds to an interface through a service whose
 *     IOInterfaceName is the interface's name, by opening a user client of type 0 on it. (11-26)
 *     That user client is IO80211APIUserClient, as on an iPhone (where the Wi-Fi dext makes it)
 *     and a Mac (IO80211Family): the same struct apple80211req as the ioctls SIOCGA80211 and
 *     SIOCSA80211, its buffer carried in the call, answered by the same code
 *     (bsd/dev/hakios.apple80211.c). Only a process with com.apple.wlan.authentication may open
 *     it; the others (mDNSResponder, SpringBoard, ...) are refused, say "Falling back to IOCTL
 *     path" and send the ioctls, which stay answered. Events still go out as kernel events: the
 *     user client says it has no event pipe, and the framework then reads the event socket.
 *     (~/akita/ref/iogpu-research/wifi-iokit-path.md has how it was read out of IO80211.framework
 *     and the Mac's IO80211Family.)
 *   - SystemConfiguration (SCNetworkInterfaceCopyAll) lists the network interfaces it finds as
 *     objects of the class IONetworkInterface, and calls one a Wi-Fi interface when its parent
 *     is of the class IO80211Controller; type, unit, name and built-in-ness are properties of
 *     the interface, the hardware address one of the controller. configd makes a network
 *     service (DHCP and the rest) for an interface it has listed, and only an interface with a
 *     service can be the one the system routes through -- which wifid waits for after a join
 *     ("failed to become routable interface") before Settings ticks the network.
 *
 *   - MobileGestalt, which lsd asks for the Wi-Fi address of every bundle it registers, looks the
 *     address up as the property local-mac-address of the device tree's "wlan" node, as a phone
 *     has it; without that it waits, up to 5 s every time, for a service of the class
 *     IONetworkController under that node. With neither, each such question cost 5 s, and a
 *     LaunchServices database rebuilt in full (a first boot on an empty data volume) takes hours,
 *     SpringBoard waiting. (Found by the ios-sapphire port, patches 0157 and 0158, which carries
 *     this file on: github.com/Angelpro09xd/ios-sapphire, APSL 2.0.)
 *
 * So: a controller of the class IO80211Controller -- an IOEthernetController and so an
 * IONetworkController, as in IO80211Family -- under the "wlan" node (the platform when there is
 * none), the radio's address on that node as local-mac-address, and below the controller one
 * object that is both, an IONetworkInterface with wifid's properties. The classes are ours, named
 * as IO80211Family and IONetworkingFamily name theirs; none has anything but properties.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" void HakiosWLANStart(const char * interface, const uint8_t * address);
/* bsd/dev/hakios.apple80211.c: one request, its buffer in the kernel (NULL: none); an errno */
extern "C" int hakios_apple80211_request_kernel(bool is_set, void * req, void * buffer, size_t capacity);

class IONetworkController : public IOService
{
	OSDeclareDefaultStructors(IONetworkController);
};

OSDefineMetaClassAndStructors(IONetworkController, IOService);

class IOEthernetController : public IONetworkController
{
	OSDeclareDefaultStructors(IOEthernetController);
};

OSDefineMetaClassAndStructors(IOEthernetController, IONetworkController);

class IO80211Controller : public IOEthernetController
{
	OSDeclareDefaultStructors(IO80211Controller);
};

OSDefineMetaClassAndStructors(IO80211Controller, IOEthernetController);

class IONetworkInterface : public IOService
{
	OSDeclareDefaultStructors(IONetworkInterface);
};

OSDefineMetaClassAndStructors(IONetworkInterface, IOService);

class HakiosWLAN : public IONetworkInterface
{
	OSDeclareDefaultStructors(HakiosWLAN);

public:
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosWLAN, IONetworkInterface);

/*
 * IO80211APIUserClient, as IO80211.framework of Darwin userland 27 calls it (the name is the one
 * the sandbox profiles allow). Selectors:
 *   0  a request: struct-in 0x3c30 = { u32 cmd (SIOCGA80211 or SIOCSA80211), u32 the buffer's
 *      length, u8 buffer[0x3c00], struct apple80211req at 0x3c08 }; out: the buffer's bytes, or
 *      for a request without a buffer req_val in 4 bytes. The framework puts the size given back
 *      into the request's req_len.
 *   6  the same with a buffer of 0x3c000 (struct-in 0x3c030), for requests above 0x3c00 bytes
 *   1  use the user client for requests: 1 byte, 1
 *   5  use its event pipe: 1 byte, 0 (the framework then reads the kernel event socket)
 *   7  "DriverKit supported": 1 byte, 1 (as the phone's dext; nothing in the framework reads it)
 *   2, 3, 4 the event pipe, 8 a mapped buffer: not built (macOS answers 8 so too)
 * The input is bigger than an in-band structure and so always arrives as a memory descriptor.
 */
class IO80211APIUserClient : public IOUserClient2022
{
	OSDeclareDefaultStructors(IO80211APIUserClient);

	static IOReturn s_request(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn s_one(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn s_zero(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn s_unsupported(OSObject * target, void * reference, IOExternalMethodArguments * args);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArgumentsOpaque * args) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IO80211APIUserClient, IOUserClient2022);

#define SIOCSA80211             0x802869c8u
#define SIOCGA80211             0xc02869c9u
#define REQUEST_SIZE            0x3c30u         /* selector 0 */
#define LARGE_REQUEST_SIZE      0x3c030u        /* selector 6 */
#define APPLE80211REQ_SIZE      40u             /* struct apple80211req, at the end of either */

bool
IO80211APIUserClient::start(IOService * provider)
{
	if (!IOUserClient2022::start(provider)) {
		return false;
	}
	/* (an IOUserClient2022 is not opened without these; the entitlement as IO80211Family's initWithTask checks it) */
	setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
	setProperty(kIOUserClientDefaultLockingSetPropertiesKey, kOSBooleanTrue);
	setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey, kOSBooleanTrue);
	setProperty(kIOUserClientEntitlementsKey, "com.apple.wlan.authentication");
	return true;
}

IOReturn
IO80211APIUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IO80211APIUserClient::externalMethod(uint32_t selector, IOExternalMethodArgumentsOpaque * args)
{
#define V kIOUCVariableStructureSize
	static const IOExternalMethodDispatch2022 methods[] = {
		{ &IO80211APIUserClient::s_request, 0, V, 0, V, false, NULL },          /* 0 */
		{ &IO80211APIUserClient::s_one, 0, 0, 0, 1, false, NULL },              /* 1 */
		{ &IO80211APIUserClient::s_unsupported, 0, 0, 0, 0, true, NULL },       /* 2 */
		{ &IO80211APIUserClient::s_unsupported, 0, 0, 0, 0, false, NULL },      /* 3 */
		{ &IO80211APIUserClient::s_unsupported, 0, 0, 0, 0, false, NULL },      /* 4 */
		{ &IO80211APIUserClient::s_zero, 0, 0, 0, 1, false, NULL },             /* 5 */
		{ &IO80211APIUserClient::s_request, 0, V, 0, V, false, NULL },          /* 6 */
		{ &IO80211APIUserClient::s_one, 0, 0, 0, 1, false, NULL },              /* 7 */
		{ &IO80211APIUserClient::s_unsupported, 2, 0, 0, 0, false, NULL },      /* 8 */
	};
#undef V

	return dispatchExternalMethod(selector, args, methods, sizeof(methods) / sizeof(methods[0]), this,
	           (void *) (uintptr_t) (selector == 6));
}

/* one byte out, in band (the framework's buffer is a single byte) */
static IOReturn
answerByte(IOExternalMethodArguments * args, uint8_t value)
{
	if (args->structureOutput == NULL || args->structureOutputSize < 1) {
		return kIOReturnBadArgument;
	}
	*(uint8_t *) args->structureOutput = value;
	args->structureOutputSize = 1;
	return kIOReturnSuccess;
}

IOReturn
IO80211APIUserClient::s_one(OSObject * target, void * reference, IOExternalMethodArguments * args)
{
	return answerByte(args, 1);
}

IOReturn
IO80211APIUserClient::s_zero(OSObject * target, void * reference, IOExternalMethodArguments * args)
{
	return answerByte(args, 0);
}

IOReturn
IO80211APIUserClient::s_unsupported(OSObject * target, void * reference, IOExternalMethodArguments * args)
{
	return kIOReturnUnsupported;
}

IOReturn
IO80211APIUserClient::s_request(OSObject * target, void * reference, IOExternalMethodArguments * args)
{
	const bool large = reference != NULL;
	const uint32_t size = large ? LARGE_REQUEST_SIZE : REQUEST_SIZE;
	const uint32_t capacity = size - 8 - APPLE80211REQ_SIZE;         /* the buffer: 0x3c00 or 0x3c000 */
	IOMemoryDescriptor * in = args->structureInputDescriptor;
	IOMemoryDescriptor * out = args->structureOutputDescriptor;
	uint32_t inSize = in != NULL ? (uint32_t) in->getLength() : args->structureInputSize;
	uint32_t outSize = out != NULL ? (uint32_t) out->getLength() : args->structureOutputSize;
	uint8_t * call;
	uint8_t * req;
	uint8_t * buffer;
	uint32_t cmd, length, given;
	int32_t value;
	bool isSet;
	IOReturn ret = kIOReturnSuccess;
	int error;

	if (inSize != size) {
		return kIOReturnBadArgument;
	}
	if ((call = (uint8_t *) IOMallocData(size)) == NULL) {
		return kIOReturnNoMemory;
	}
	if (in != NULL) {
		if ((ret = in->prepare()) == kIOReturnSuccess) {
			if (in->readBytes(0, call, size) != size) {
				ret = kIOReturnBadArgument;
			}
			in->complete();
		}
	} else if (args->structureInput != NULL) {
		memcpy(call, args->structureInput, size);
	} else {
		ret = kIOReturnBadArgument;
	}
	if (ret != kIOReturnSuccess) {
		goto out;
	}
	memcpy(&cmd, call, 4);
	memcpy(&length, call + 4, 4);
	buffer = call + 8;
	req = call + 8 + capacity;
	isSet = cmd == SIOCSA80211;
	if ((cmd != SIOCSA80211 && cmd != SIOCGA80211) || length > capacity || outSize > capacity) {
		ret = kIOReturnBadArgument;
		goto out;
	}
	/*
	 * A get has a buffer when the caller gave one (length: its size) or asks for more than the 4
	 * bytes of req_val (the framework's first request, DRIVER_VERSION into 0x180 bytes, sends no
	 * length); the room it has is the output's size. A set carries `length` bytes. Without a
	 * buffer the answer is req_val. The buffer stays what the caller put in it: a scan's result
	 * reads the room for elements and the UUID from there.
	 */
	{
		uint32_t reqLen = isSet ? length : outSize;
		bool hasBuffer = isSet ? length != 0 : outSize != 0 && (length != 0 || outSize > 4);

		memcpy(req + 0x18, &reqLen, 4);
		error = hakios_apple80211_request_kernel(isSet, req, hasBuffer ? buffer : NULL, capacity);
		if (error != 0) {
			ret = (IOReturn) error;         /* (an errno, as the ioctl gives it; the framework reads it so) */
			goto out;
		}
		if (isSet) {
			given = 0;              /* (sets that give something back -- 94, 153, 293 -- are not built) */
		} else if (!hasBuffer) {
			memcpy(&value, req + 0x14, 4);
			memcpy(buffer, &value, 4);
			given = outSize < 4 ? outSize : 4;
		} else {
			memcpy(&given, req + 0x18, 4);
			given = given < outSize ? given : outSize;
		}
	}
	if (out != NULL) {
		if ((ret = out->prepare()) == kIOReturnSuccess) {
			out->writeBytes(0, buffer, given);
			out->complete();
			args->structureOutputDescriptorSize = given;
		}
	} else {
		if (given != 0) {
			memcpy(args->structureOutput, buffer, given);
		}
		args->structureOutputSize = given;
	}
out:
	IOFreeData(call, size);
	return ret;
}

/* Type 0: IO80211APIUserClient (see above); there is no other. */
IOReturn
HakiosWLAN::newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties, IOUserClient ** handler)
{
	static unsigned said;
	IO80211APIUserClient * client;

	if (type != 0) {
		if (said++ < 8) {
			IOLog("HakiosWLAN: a user client of type %u is asked for: there is none\n", (unsigned) type);
		}
		return kIOReturnUnsupported;
	}
	client = OSTypeAlloc(IO80211APIUserClient);
	if (client == NULL) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

/* attach, start, register: the service, or NULL */
static IOService *
publish(IOService * service, IOService * provider, const char * name)
{
	if (service == NULL || !service->attach(provider)) {
		OSSafeReleaseNULL(service);
		return NULL;
	}
	service->setName(name);
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return NULL;
	}
	service->registerService();
	return service;
}

/* Once, when the interface of a radio exists. */
void
HakiosWLANStart(const char * interface, const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	OSDictionary * match = IOService::nameMatching("wlan");
	IOService * node = match != NULL ? IOService::copyMatchingService(match) : NULL;   /* the device tree's */
	IO80211Controller * controller = OSTypeAlloc(IO80211Controller);
	HakiosWLAN * wlan = OSTypeAlloc(HakiosWLAN);
	OSData * mac;
	OSNumber * number;

	if (platform == NULL || controller == NULL || wlan == NULL || !controller->init() || !wlan->init()) {
		OSSafeReleaseNULL(controller);
		OSSafeReleaseNULL(wlan);
		OSSafeReleaseNULL(node);
		OSSafeReleaseNULL(match);
		IOLog("HakiosWLAN: cannot create the services\n");
		return;
	}
	if ((mac = OSData::withBytes(address, 6)) != NULL) {
		controller->setProperty("IOMACAddress", mac);
		if (node != NULL) {
			node->setProperty("local-mac-address", mac);     /* where MobileGestalt reads it first */
		}
		mac->release();
	}
	controller->setProperty("IOBuiltin", true);
	OSSafeReleaseNULL(match);
	controller = (IO80211Controller *) publish(controller, node != NULL ? node : platform, "HakiosWLANController");
	OSSafeReleaseNULL(node);
	if (controller == NULL) {
		wlan->release();
		IOLog("HakiosWLAN: the controller did not start\n");
		return;
	}

	wlan->setProperty("IOInterfaceName", interface);
	wlan->setProperty("BSD Name", interface);
	wlan->setProperty("IOInterfaceNamePrefix", "en");
	if ((number = OSNumber::withNumber(6ULL, 32)) != NULL) {        /* IFT_ETHER */
		wlan->setProperty("IOInterfaceType", number);
		number->release();
	}
	if ((number = OSNumber::withNumber((unsigned long long) (interface[2] - '0'), 32)) != NULL) {
		wlan->setProperty("IOInterfaceUnit", number);
		number->release();
	}
	wlan->setProperty("IOBuiltin", true);
	wlan->setProperty("IOPrimaryInterface", true);
	wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
	if (publish(wlan, controller, "HakiosWLAN") == NULL) {
		IOLog("HakiosWLAN: the service did not start\n");
	} else {
		wlan->release();
	}
	controller->release();
}
