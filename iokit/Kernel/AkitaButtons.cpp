/*
 * AkitaButtons (AkIOS, 11-26): the Pixel 8a's keys as an iPhone's button service, so that backboardd's own
 * IOHIDEventSystem reads them from the kernel. Only with boot-arg hid_buttons=1.
 *
 * On iPhone14,4 the side and volume buttons are AppleM68Buttons : AppleEmbeddedHIDEventService : IOHIDEventService
 * (ref/drivers/touch-buttons-spec.md 2). This is the same shape under our own name -- nothing in iOS 27 userland asks
 * for the class name "AppleM68Buttons" (no such string in the dyld cache or the rootfs); the event system wants
 * IOHIDEventService (IOObjectConformsTo) and HIDServiceSupport = true, which the base classes give
 * (iokit/Kernel/IOHIDFamily/). What userland does read is in the properties:
 *
 *   PrimaryUsagePage/PrimaryUsage = 0x0B/0x01 (Telephony/Phone, AppleM68Buttons' default), DeviceUsagePairs =
 *   [0x0B/0x01, 0x0C/0x01]: ButtonResolver's legacy interface (backboardd) takes the first built-in 0x0B/0x01
 *   service and is not ready until there is one; TactSwitchHIDServiceFilter matches 0x0B/0x01 and IOHIDKeyboardFilter
 *   0x0C/0x01. Built-In = true (AppleEmbeddedHIDEventService). Transport "SPI", Manufacturer, Product: what the
 *   virtual buttons service in userland/src/inject/backboardd.c says, which is known to work.
 *
 * A press: the keys driver (bsd/dev/hakios.akita_keys.c) calls akios_hid_buttons_post(page, usage, down) from its
 * polling thread call; that dispatches one keyboard event, as AppleM68Buttons' press path does (vtable +0x750,
 * IOHIDEventService::dispatchKeyboardEvent(time, page, usage, value, ...), value 1 down / 0 up):
 *
 *   power        -> 0x0C/0x30 (Consumer Power, the iPhone's "hold")
 *   volume up    -> 0x0C/0xE9 (Volume Increment, "volup")
 *   volume down  -> 0x0C/0xEA (Volume Decrement, "voldown")
 *
 * The Pixel has no ring/silent switch (0x0B/0x2E on the iPhone), so nothing sends that.
 *
 * One more way in, for userland: a property set "AkitaPressButton" = { UsagePage, Usage, Down } on the service
 * (through IOHIDServiceClientSetProperty, which reaches setProperties here through the event system's user client)
 * dispatches that event as if the key had been pressed. It is for the touch injection's tap-to-wake, which presses
 * the side button for a tap while the display is off, and goes once touch is in the kernel too.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#include "IOHIDFamily/AppleEmbeddedHIDEventService.h"
#include "IOHIDFamily/IOHIDEventData.h"

extern "C" void akios_hid_buttons_init(void);
extern "C" void akios_hid_buttons_post(uint32_t usagePage, uint32_t usage, int down);

/* HID usage tables (IOHIDUsageTables.h), the ones used here */
#define kHIDPage_Telephony              0x0B
#define kHIDPage_Consumer               0x0C
#define kHIDUsage_Tfon_Phone            0x01
#define kHIDUsage_Csmr_ConsumerControl  0x01

class AkitaButtons : public AppleEmbeddedHIDEventService
{
	OSDeclareDefaultStructors(AkitaButtons);

	IOWorkLoop *    fWorkLoop;
	OSArray *       fUsagePairs;
	OSString *      fTransport;
	OSString *      fManufacturer;
	OSString *      fProduct;

public:
	virtual bool init(OSDictionary * properties = NULL) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOWorkLoop * getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
	virtual bool handleStart(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;

	void press(uint32_t usagePage, uint32_t usage, bool down);

protected:
	virtual OSString * getTransport(void) APPLE_KEXT_OVERRIDE;
	virtual OSString * getManufacturer(void) APPLE_KEXT_OVERRIDE;
	virtual OSString * getProduct(void) APPLE_KEXT_OVERRIDE;
	virtual OSArray * getDeviceUsagePairs(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AkitaButtons, AppleEmbeddedHIDEventService);

#define super AppleEmbeddedHIDEventService

/* The one service, once it has started; read by the keys driver's thread call. */
static AkitaButtons * volatile gAkitaButtons;

static OSDictionary *
usagePair(uint32_t usagePage, uint32_t usage)
{
	OSDictionary * pair = OSDictionary::withCapacity(2);
	OSNumber * page = OSNumber::withNumber(usagePage, 32);
	OSNumber * use = OSNumber::withNumber(usage, 32);

	if (pair && page && use) {
		pair->setObject(kIOHIDDeviceUsagePageKey, page);
		pair->setObject(kIOHIDDeviceUsageKey, use);
	} else {
		OSSafeReleaseNULL(pair);
	}
	OSSafeReleaseNULL(page);
	OSSafeReleaseNULL(use);
	return pair;
}

bool
AkitaButtons::init(OSDictionary * properties)
{
	OSDictionary * phone, * consumer;

	/*
	 * Our own work loop: IOHIDEventService::start and its user clients take the service's work loop, and the
	 * registry root we hang off has none to give.
	 */
	fWorkLoop = IOWorkLoop::workLoop();
	if (fWorkLoop == NULL) {
		return false;
	}
	fUsagePairs = OSArray::withCapacity(2);
	phone = usagePair(kHIDPage_Telephony, kHIDUsage_Tfon_Phone);
	consumer = usagePair(kHIDPage_Consumer, kHIDUsage_Csmr_ConsumerControl);
	if (fUsagePairs && phone && consumer) {
		fUsagePairs->setObject(phone);
		fUsagePairs->setObject(consumer);
	}
	OSSafeReleaseNULL(phone);
	OSSafeReleaseNULL(consumer);
	if (fUsagePairs == NULL || fUsagePairs->getCount() != 2) {
		return false;
	}
	fTransport = OSString::withCStringNoCopy(kIOHIDTransportSPIValue);
	fManufacturer = OSString::withCStringNoCopy("akita");
	fProduct = OSString::withCStringNoCopy("Akita buttons");
	if (fTransport == NULL || fManufacturer == NULL || fProduct == NULL) {
		return false;
	}
	return super::init(properties);
}

void
AkitaButtons::free(void)
{
	OSSafeReleaseNULL(fUsagePairs);
	OSSafeReleaseNULL(fTransport);
	OSSafeReleaseNULL(fManufacturer);
	OSSafeReleaseNULL(fProduct);
	super::free();
	OSSafeReleaseNULL(fWorkLoop);
}

IOWorkLoop *
AkitaButtons::getWorkLoop(void) const
{
	return fWorkLoop;
}

bool
AkitaButtons::handleStart(IOService * provider)
{
	OSArray * buttons;
	static const uint64_t usages[] = {
		/* ButtonUsagePairs, as AppleM68Buttons publishes them: page << 32 | usage */
		((uint64_t)kHIDPage_Consumer << 32) | 0x30,
		((uint64_t)kHIDPage_Consumer << 32) | 0xE9,
		((uint64_t)kHIDPage_Consumer << 32) | 0xEA,
	};

	if (!super::handleStart(provider)) {
		return false;
	}
	buttons = OSArray::withCapacity(3);
	if (buttons) {
		for (unsigned i = 0; i < sizeof(usages) / sizeof(usages[0]); i++) {
			OSNumber * n = OSNumber::withNumber(usages[i], 64);

			if (n) {
				buttons->setObject(n);
				n->release();
			}
		}
		setProperty("ButtonUsagePairs", buttons);
		buttons->release();
	}
	return true;
}

OSString *
AkitaButtons::getTransport(void)
{
	return fTransport;
}

OSString *
AkitaButtons::getManufacturer(void)
{
	return fManufacturer;
}

OSString *
AkitaButtons::getProduct(void)
{
	return fProduct;
}

/* The first pair is the primary one (IOHIDEventService::getPrimaryUsagePage/getPrimaryUsage take pair 0). */
OSArray *
AkitaButtons::getDeviceUsagePairs(void)
{
	return fUsagePairs;
}

/*
 * The event a key makes. The event is marked built-in (kIOHIDEventFieldIsBuiltIn, bit 4 of the event's options):
 * the virtual service in userland set that on each of its button events and backboardd acted on them; Apple's
 * kernel does not, and the bit costs nothing if the event system sets it again.
 */
void
AkitaButtons::press(uint32_t usagePage, uint32_t usage, bool down)
{
	dispatchKeyboardEvent(mach_absolute_time(), usagePage, usage, down ? 1 : 0, kIOHIDEventOptionIsBuiltIn);
}

IOReturn
AkitaButtons::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSDictionary * button = dict ? OSDynamicCast(OSDictionary, dict->getObject("AkitaPressButton")) : NULL;

	if (button != NULL) {
		OSNumber * page = OSDynamicCast(OSNumber, button->getObject("UsagePage"));
		OSNumber * usage = OSDynamicCast(OSNumber, button->getObject("Usage"));
		OSObject * down = button->getObject("Down");
		bool isDown = down == kOSBooleanTrue ||
		    (OSDynamicCast(OSNumber, down) && ((OSNumber *)down)->unsigned32BitValue() != 0);

		if (page == NULL || usage == NULL) {
			return kIOReturnBadArgument;
		}
		press(page->unsigned32BitValue(), usage->unsigned32BitValue(), isDown);
		if (dict->getCount() == 1) {
			return kIOReturnSuccess;
		}
	}
	return super::setProperties(properties);
}

/*
 * Called from bsd_init once IOKit is up (as AHSFirmwareReadyNotifier is): makes and starts the service under the
 * registry root. Off unless boot-arg hid_buttons=1: while userland's virtual buttons service exists, a second
 * built-in 0x0B/0x01 service would take ButtonResolver's place and every press would arrive twice.
 */
void
akios_hid_buttons_init(void)
{
	uint32_t enabled = 0;
	AkitaButtons * made;
	IOService * root;

	if (!PE_parse_boot_argn("hid_buttons", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	made = OSTypeAlloc(AkitaButtons);
	root = IOService::getServiceRoot();
	if (made == NULL || root == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		IOLog("AkitaButtons: cannot be made\n");
		return;
	}
	made->setName("buttons");
	if (!made->attach(root)) {
		made->release();
		IOLog("AkitaButtons: cannot attach\n");
		return;
	}
	if (!made->start(root)) {        /* (registers itself: IOHIDEventService::start) */
		made->detach(root);
		made->release();
		IOLog("AkitaButtons: does not start\n");
		return;
	}
	__atomic_store_n(&gAkitaButtons, made, __ATOMIC_RELEASE);   /* (keeps the reference from OSTypeAlloc) */
	IOLog("AkitaButtons: up, registry ID 0x%llx\n", made->getRegistryEntryID());
}

/* From the keys driver: a key went down (down = 1) or up (0). Before the service is up, the press is dropped. */
void
akios_hid_buttons_post(uint32_t usagePage, uint32_t usage, int down)
{
	AkitaButtons * buttons = __atomic_load_n(&gAkitaButtons, __ATOMIC_ACQUIRE);

	if (buttons != NULL) {
		buttons->press(usagePage, usage, down != 0);
	}
}
