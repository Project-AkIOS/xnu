/*
 * The Bluetooth module the way a UART iPhone presents it (11-26; it replaces hakios.bluetooth.cpp, once upstream's
 * RedwoodBluetooth.cpp, a service named "bluetooth" hung on the platform).
 *
 * On an iPhone (24B156's kernel cache, ~/akita/ref/iogpu-research/bluetooth-transport-path.md):
 *   - the device tree's /arm-io/bluetooth is the service bluetoothd and BlueTool look up (IOServiceNameMatching
 *     "bluetooth"); bluetoothd reads its transport-encoding (3 here: H4BC, a UART) and local-mac-address from it;
 *   - AppleBluetoothModule (com.apple.driver.AppleBluetoothModule) matches that node by name and is what BlueTool and
 *     bluetoothd switch the module with: its user client AppleBluetoothModuleUserClient, seven methods -- 0 power (on or
 *     off), 1 open, 2 close, 3 power cycle, 4 core dump, 5 function-level reset, 6 the address made again;
 *   - AppleOnboardSerialBSDClient, on the UART's serial nub (AppleOnboardSerialSync "bluetooth"), registers the kernel
 *     control com.apple.uart.sk.bluetooth and the Skywalk pipe behind it, with the pipe's IOSkywalkNexusUUID on itself,
 *     as soon as BSD is up (its IOResourceMatch IOBSD).
 * Here the node is tools/ak/adt.py's (with bt=2), published as a nub by GenericARMPE; the module and the serial client are
 * the classes below, matched by iokit/KernelConfigTables.cpp's personalities, and the work is the chip-independent
 * half's, bsd/dev/hakios.bthost.c (the bridge thread, the control, the pipe; the BCM4383's own steps are
 * bsd/dev/hakios.akita_bt.c's). Differences, each a stopgap until there is more:
 *   - the serial client hangs on the module, not on a serial nub of a UART driver of Apple's shape (there is none:
 *     the UART is driven by hakios.akita_bt.c directly);
 *   - the module switches the controller on by itself 30 s into the boot (hakios_bthost_start), since nothing asks it
 *     to: BlueTool cannot run here as it is (no BCM4383 in its firmware table) and its stand-in answers "booted";
 *   - a core dump is said in the log and nothing else; a function-level reset is a power cycle (a UART has no FLR).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <sys/errno.h>
#include <sys/proc.h>

extern "C" {
bool hakios_bthost_publish(void);
bool hakios_bthost_nexus_uuid(char * out, size_t size);
int hakios_bthost_power(int what, unsigned wait_s);
}

#define BT_POWER_WAIT_S 20      /* a power-on is the firmware download (~2 s at 4 Mbaud) and six commands */

class AppleBluetoothModule : public IOService
{
	OSDeclareDefaultStructors(AppleBluetoothModule);

	bool fOpen;

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;

	IOReturn powerModule(bool on);
	IOReturn openModule(void);
	IOReturn closeModule(void);
	IOReturn powerCycleModule(void);
	IOReturn coredumpModule(uint64_t reason);
	IOReturn flrModule(void);
	IOReturn generateMac(void);
};

OSDefineMetaClassAndStructors(AppleBluetoothModule, IOService);

static IOReturn
bt_result(int error)
{
	switch (error) {
	case 0:
		return kIOReturnSuccess;
	case ENODEV:
		return kIOReturnNoDevice;
	case ETIMEDOUT:
		return kIOReturnTimeout;
	case EINTR:
		return kIOReturnAborted;
	default:
		return kIOReturnError;
	}
}

static void
bt_say_caller(const char * what)
{
	char name[32] = "?";

	proc_selfname(name, sizeof(name));
	IOLog("AppleBluetoothModule: %s, asked by %s (pid %d)\n", what, name, proc_selfpid());
}

bool
AppleBluetoothModule::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	/* the address: the device tree's (adt.py: = /chosen/mac-address-bluetooth0); made from /chosen if it has none */
	if (OSDynamicCast(OSData, provider->getProperty("local-mac-address")) == NULL) {
		(void) generateMac();
	}
	registerService();
	IOLog("AppleBluetoothModule: on %s, its user client AppleBluetoothModuleUserClient\n", provider->getName());
	return true;
}

/* 0: on or off. On waits for the firmware and the first answers; off switches the chip's power off. */
IOReturn
AppleBluetoothModule::powerModule(bool on)
{
	bt_say_caller(on ? "power on" : "power off");
	return bt_result(hakios_bthost_power(on ? 1 : 0, BT_POWER_WAIT_S));
}

/* 1, 2: Apple's open and close the module's provider; nothing here needs it held, so the open is only noted */
IOReturn
AppleBluetoothModule::openModule(void)
{
	fOpen = true;
	return kIOReturnSuccess;
}

IOReturn
AppleBluetoothModule::closeModule(void)
{
	fOpen = false;
	return kIOReturnSuccess;
}

/* 3: off and on again -- the chip's power (BT_REG_ON), its firmware downloaded anew */
IOReturn
AppleBluetoothModule::powerCycleModule(void)
{
	bt_say_caller("power cycle");
	return bt_result(hakios_bthost_power(2, BT_POWER_WAIT_S));
}

/* 4: a core dump of the controller's firmware: none can be taken from this chip here; said, and done */
IOReturn
AppleBluetoothModule::coredumpModule(uint64_t reason)
{
	char what[64];

	snprintf(what, sizeof(what), "core dump (reason %llu): none taken", reason);
	bt_say_caller(what);
	return kIOReturnSuccess;
}

/* 5: the function-level reset of a PCIe module; on a UART the nearest thing is a power cycle */
IOReturn
AppleBluetoothModule::flrModule(void)
{
	bt_say_caller("function-level reset (a power cycle on a UART)");
	return bt_result(hakios_bthost_power(2, BT_POWER_WAIT_S));
}

/* 6: local-mac-address on the provider, from /chosen/mac-address-bluetooth0 (what iBoot would have put there) */
IOReturn
AppleBluetoothModule::generateMac(void)
{
	IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	OSData * mac = chosen != NULL ? OSDynamicCast(OSData, chosen->getProperty("mac-address-bluetooth0")) : NULL;
	IOService * provider = getProvider();
	IOReturn result = kIOReturnNotFound;

	if (mac != NULL && mac->getLength() == 6 && provider != NULL) {
		provider->setProperty("local-mac-address", mac);
		result = kIOReturnSuccess;
	}
	OSSafeReleaseNULL(chosen);
	if (result != kIOReturnSuccess) {
		IOLog("AppleBluetoothModule: no /chosen/mac-address-bluetooth0: no address\n");
	}
	return result;
}

/* ---- the user client: Apple's seven methods ---- */

class AppleBluetoothModuleUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleBluetoothModuleUserClient);

	AppleBluetoothModule * fModule;

	static IOReturn sPower(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sOpen(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sClose(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sPowerCycle(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sCoredump(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sFLR(OSObject * target, void * reference, IOExternalMethodArguments * args);
	static IOReturn sGenerateMac(OSObject * target, void * reference, IOExternalMethodArguments * args);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleBluetoothModuleUserClient, IOUserClient);

bool
AppleBluetoothModuleUserClient::start(IOService * provider)
{
	fModule = OSDynamicCast(AppleBluetoothModule, provider);
	return fModule != NULL && IOUserClient::start(provider);
}

IOReturn
AppleBluetoothModuleUserClient::clientClose(void)
{
	if (fModule != NULL) {
		(void) fModule->closeModule();
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
AppleBluetoothModuleUserClient::sPower(OSObject * target, void *, IOExternalMethodArguments * args)
{
	return ((AppleBluetoothModule *) target)->powerModule(args->scalarInput[0] != 0);
}

IOReturn
AppleBluetoothModuleUserClient::sOpen(OSObject * target, void *, IOExternalMethodArguments *)
{
	return ((AppleBluetoothModule *) target)->openModule();
}

IOReturn
AppleBluetoothModuleUserClient::sClose(OSObject * target, void *, IOExternalMethodArguments *)
{
	return ((AppleBluetoothModule *) target)->closeModule();
}

IOReturn
AppleBluetoothModuleUserClient::sPowerCycle(OSObject * target, void *, IOExternalMethodArguments *)
{
	return ((AppleBluetoothModule *) target)->powerCycleModule();
}

IOReturn
AppleBluetoothModuleUserClient::sCoredump(OSObject * target, void *, IOExternalMethodArguments * args)
{
	return ((AppleBluetoothModule *) target)->coredumpModule(args->scalarInput[0]);
}

IOReturn
AppleBluetoothModuleUserClient::sFLR(OSObject * target, void *, IOExternalMethodArguments *)
{
	return ((AppleBluetoothModule *) target)->flrModule();
}

IOReturn
AppleBluetoothModuleUserClient::sGenerateMac(OSObject * target, void *, IOExternalMethodArguments *)
{
	return ((AppleBluetoothModule *) target)->generateMac();
}

IOReturn
AppleBluetoothModuleUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch *, OSObject *, void * reference)
{
	/* { function, scalars in, structure in, scalars out, structure out }: the counts of 24B156's table */
	static const IOExternalMethodDispatch methods[] = {
		{ &AppleBluetoothModuleUserClient::sPower, 1, 0, 0, 0 },
		{ &AppleBluetoothModuleUserClient::sOpen, 0, 0, 0, 0 },
		{ &AppleBluetoothModuleUserClient::sClose, 0, 0, 0, 0 },
		{ &AppleBluetoothModuleUserClient::sPowerCycle, 0, 0, 0, 0 },
		{ &AppleBluetoothModuleUserClient::sCoredump, 1, 0, 0, 0 },
		{ &AppleBluetoothModuleUserClient::sFLR, 0, 0, 0, 0 },
		{ &AppleBluetoothModuleUserClient::sGenerateMac, 0, 0, 0, 0 },
	};

	if (selector >= sizeof(methods) / sizeof(methods[0]) || fModule == NULL) {
		return kIOReturnBadArgument;
	}
	return IOUserClient::externalMethod(selector, args, (IOExternalMethodDispatch *) &methods[selector], fModule, reference);
}

/* ---- the serial client: the kernel control and the pipe, at BSD's start ---- */

class AppleOnboardSerialBSDClient : public IOService
{
	OSDeclareDefaultStructors(AppleOnboardSerialBSDClient);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleOnboardSerialBSDClient, IOService);

bool
AppleOnboardSerialBSDClient::start(IOService * provider)
{
	char uuid[40] = "(no UUID)";

	if (!IOService::start(provider)) {
		return false;
	}
	if (!hakios_bthost_publish()) {
		IOLog("AppleOnboardSerialBSDClient: com.apple.uart.sk.bluetooth could not be published (no controller registered?)\n");
		return false;
	}
	if (hakios_bthost_nexus_uuid(uuid, sizeof(uuid))) {
		setProperty("IOSkywalkNexusUUID", uuid);
	}
	registerService();
	IOLog("AppleOnboardSerialBSDClient: com.apple.uart.sk.bluetooth and its Skywalk pipe %s are published\n", uuid);
	return true;
}
