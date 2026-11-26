/*
 * AkitaTouchScreen (AkIOS, 11-26): the touch screen as a built-in IOHIDEventService digitizer that backboardd's own
 * IOHIDEventSystem reads -- route B of ref/drivers/touch-buttons-spec.md. Only with boot-arg hid_touch=1.
 *
 * Until now code of ours inside backboardd (userland/src/inject/backboardd.c: send_touch()) read /dev/hakios-touch
 * and dispatched the events through a virtual HID service. This makes the same events here, from the same samples,
 * and sends them the way an iPhone's kernel sends events: through IOHIDEventServiceUserClient's queue to
 * IOHIDEventServicePlugin (iokit/Kernel/IOHIDFamily/).
 *
 * What backboardd wants of the service (spec 1.5, BKHIDDirectTouchEventProcessor): built-in, primary usage
 * 0x0D/0x04 (Digitizer / Touch Screen) -- "acquired main display digitizer" --, DisplayIntegrated. Transport "SPI",
 * Manufacturer and Product as the virtual service had them.
 *
 * The samples: every producer -- the panel's driver (bsd/dev/hakios.akita_touch.c, through hakios_touch_post, which
 * maps the panel onto the displayed area: MiniIOMobileFramebuffer's hakios_touch_view), usbmux's touch ports
 * (hakios_touch_inject) and writes to /dev/hakios-touch -- ends in touch_post() in bsd/dev/hakios.input.c, which
 * hands each touch sample to akios_hid_touch_post() here as well. So x and y are already in the displayed area,
 * 0..65535, as the injection read them.
 *
 * The events are send_touch()'s, field for field (the injection's events are the reference: backboardd took them):
 * per sample one digitizer event of transducer type Hand, index 0, identity 0, at the sampled finger's position,
 * eventMask = what changed without Range (Touch for a landing or a lift, Position for a move), range = touch = some
 * finger is down, DisplayIntegrated, IsBuiltIn; under it one Finger child for each finger that is down plus the one
 * that has just lifted: index i+1, identity i+2, eventMask = the change for the sampled finger (none for the others)
 * plus 0x800 (kIOHIDDigitizerEventFromEdgeTip) for a finger that landed near an edge, position 0..1, tip pressure 1
 * or 0, range = touch = down, DisplayIntegrated, the quality orientation as IOHIDEventCreateDigitizerFingerEvent
 * makes it (quality, density, irregularity 1.0, radii 5.0, accuracy 0). The hand is not marked as a collection
 * (kIOHIDEventOptionIsCollection), as IOHIDEventAppendEvent in userland does not mark it either.
 *
 * Tap to wake, as the injection did it: while the window server has asked the screen for "TouchDetectionMode" 0xfe
 * (display off, a tap wakes it; the low byte, seen as 0x1fe too) a finger landing presses the lock button --
 * through the kernel's buttons service (AkitaButtons, boot-arg hid_buttons=1) -- and the rest of that finger's touch
 * is kept back. The mode reaches us as a property set from the event system (through the user client).
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

#include "IOHIDFamily/AppleEmbeddedHIDEventService.h"
#include "IOHIDFamily/IOHIDEventData.h"

extern "C" void akios_hid_touch_init(void);
extern "C" void akios_hid_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time);
extern "C" void akios_hid_buttons_post(uint32_t usagePage, uint32_t usage, int down);   /* AkitaButtons.cpp */
extern "C" int akios_hid_buttons_up(void);                                              /* AkitaButtons.cpp */

/* sys/hakios.input.h (not seen from iokit/) */
#define AKIOS_TOUCH_DOWN        1
#define AKIOS_TOUCH_MOVE        2
#define AKIOS_TOUCH_UP          3

#define kHIDPage_Digitizer              0x0D
#define kHIDUsage_Dig_TouchScreen       0x04

#define MAX_FINGERS             10

/* TouchDetectionMode, the low byte: display off and a tap is to wake it */
#define TOUCH_MODE_WAKE         0xfe

/* how many property sets setProperties says in full before it says only TouchDetectionMode changes */
#define SET_LINES_MAX           40

/*
 * A finger that lands this close to an edge of the screen is said to have come in over that edge (the injection's
 * EDGE_SIDE/EDGE_TOP/EDGE_BOTTOM: 16 of 375 points at the sides, 16 of 812 at the top, 34 of 812 at the bottom, the
 * strip of the home bar): Apple's touch controller tells that from the shape of the contact and marks the finger's
 * events; UIKit's screen edge recognizers (swipe up to leave an application, back, pull down) take no other.
 * In 0..65535 units, compared as v * points_across < edge_points * 65535.
 */
#define EDGE_SIDE_NUM           16
#define EDGE_SIDE_DEN           375
#define EDGE_TOP_NUM            16
#define EDGE_TOP_DEN            812
#define EDGE_BOTTOM_NUM         34
#define EDGE_BOTTOM_DEN         812

class AkitaTouchScreen : public AppleEmbeddedHIDEventService
{
	OSDeclareDefaultStructors(AkitaTouchScreen);

	IOWorkLoop *    fWorkLoop;
	IOCommandGate * fGate;
	OSArray *       fUsagePairs;
	OSString *      fTransport;
	OSString *      fManufacturer;
	OSString *      fProduct;

	struct {
		bool     touching, edge;
		uint32_t x, y;          /* 0..65535 */
	} fFingers[MAX_FINGERS];
	uint32_t        fWaking;        /* the fingers whose touch went into waking the display */
	volatile int    fTouchMode;     /* the window server's TouchDetectionMode */
	bool            fToldNoButtons;
	unsigned        fSetLines;      /* property sets said so far (setProperties) */

	IOReturn sampleGated(void * type, void * finger, void * xy, void * time);
	void send(uint32_t type, unsigned f, uint64_t time);

public:
	virtual bool init(OSDictionary * properties = NULL) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOWorkLoop * getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
	virtual bool handleStart(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;

	void sample(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time);

protected:
	virtual OSString * getTransport(void) APPLE_KEXT_OVERRIDE;
	virtual OSString * getManufacturer(void) APPLE_KEXT_OVERRIDE;
	virtual OSString * getProduct(void) APPLE_KEXT_OVERRIDE;
	virtual OSArray * getDeviceUsagePairs(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AkitaTouchScreen, AppleEmbeddedHIDEventService);

#define super AppleEmbeddedHIDEventService

static AkitaTouchScreen * volatile gAkitaTouchScreen;

bool
AkitaTouchScreen::init(OSDictionary * properties)
{
	OSDictionary * pair;
	OSNumber * page, * usage;

	/* our own work loop, as AkitaButtons: the registry root we hang off has none */
	fWorkLoop = IOWorkLoop::workLoop();
	if (fWorkLoop == NULL) {
		return false;
	}
	fGate = IOCommandGate::commandGate(this);
	if (fGate == NULL || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess) {
		return false;
	}
	fUsagePairs = OSArray::withCapacity(1);
	pair = OSDictionary::withCapacity(2);
	page = OSNumber::withNumber(kHIDPage_Digitizer, 32);
	usage = OSNumber::withNumber(kHIDUsage_Dig_TouchScreen, 32);
	if (fUsagePairs && pair && page && usage) {
		pair->setObject(kIOHIDDeviceUsagePageKey, page);
		pair->setObject(kIOHIDDeviceUsageKey, usage);
		fUsagePairs->setObject(pair);
	}
	OSSafeReleaseNULL(pair);
	OSSafeReleaseNULL(page);
	OSSafeReleaseNULL(usage);
	if (fUsagePairs == NULL || fUsagePairs->getCount() != 1) {
		return false;
	}
	fTransport = OSString::withCStringNoCopy(kIOHIDTransportSPIValue);
	fManufacturer = OSString::withCStringNoCopy("akita");
	fProduct = OSString::withCStringNoCopy("Akita touch screen");
	if (fTransport == NULL || fManufacturer == NULL || fProduct == NULL) {
		return false;
	}
	return super::init(properties);
}

void
AkitaTouchScreen::free(void)
{
	OSSafeReleaseNULL(fUsagePairs);
	OSSafeReleaseNULL(fTransport);
	OSSafeReleaseNULL(fManufacturer);
	OSSafeReleaseNULL(fProduct);
	if (fGate != NULL && fWorkLoop != NULL) {
		fWorkLoop->removeEventSource(fGate);
	}
	OSSafeReleaseNULL(fGate);
	super::free();
	OSSafeReleaseNULL(fWorkLoop);
}

IOWorkLoop *
AkitaTouchScreen::getWorkLoop(void) const
{
	return fWorkLoop;
}

bool
AkitaTouchScreen::handleStart(IOService * provider)
{
	if (!super::handleStart(provider)) {    /* (Built-In) */
		return false;
	}
	/* backboardd: _queue_addDigitizerStateForService reads DisplayIntegrated (spec 1.5) */
	setProperty("DisplayIntegrated", true);
	/* (the virtual touch screen in the injection had these top-level too, besides DeviceUsagePairs) */
	setProperty(kIOHIDDeviceUsagePageKey, kHIDPage_Digitizer, 32);
	setProperty(kIOHIDDeviceUsageKey, kHIDUsage_Dig_TouchScreen, 32);
	return true;
}

OSString *
AkitaTouchScreen::getTransport(void)
{
	return fTransport;
}

OSString *
AkitaTouchScreen::getManufacturer(void)
{
	return fManufacturer;
}

OSString *
AkitaTouchScreen::getProduct(void)
{
	return fProduct;
}

OSArray *
AkitaTouchScreen::getDeviceUsagePairs(void)
{
	return fUsagePairs;
}

/* The event system's property sets: TouchDetectionMode is kept for tap to wake; all go on to IOHIDEventService. */
IOReturn
AkitaTouchScreen::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSNumber * mode = dict ? OSDynamicCast(OSNumber, dict->getObject("TouchDetectionMode")) : NULL;

	if (mode != NULL && fSetLines >= SET_LINES_MAX && (int)mode->unsigned32BitValue() != fTouchMode) {
		/* (after the first SET_LINES_MAX lines: only the changes of the mode, which tap to wake depends on) */
		IOLog("AkitaTouchScreen: TouchDetectionMode %#x\n", mode->unsigned32BitValue());
	}
	if (dict != NULL && fSetLines < SET_LINES_MAX) {
		/*
		 * The keys the event system (or anyone) sets, to compare with the injection's "the event server sets" lines:
		 * the first SET_LINES_MAX of them.
		 */
		OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
		const OSSymbol * key;

		while (keys != NULL && fSetLines < SET_LINES_MAX &&
		    (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL) {
			OSObject * value = dict->getObject(key);
			OSNumber * n = OSDynamicCast(OSNumber, value);
			OSBoolean * b = OSDynamicCast(OSBoolean, value);
			OSString * s = OSDynamicCast(OSString, value);

			fSetLines++;
			if (n != NULL) {
				IOLog("AkitaTouchScreen: set %s = %#llx\n", key->getCStringNoCopy(), n->unsigned64BitValue());
			} else if (b != NULL) {
				IOLog("AkitaTouchScreen: set %s = %s\n", key->getCStringNoCopy(), b->getValue() ? "true" : "false");
			} else if (s != NULL) {
				IOLog("AkitaTouchScreen: set %s = \"%s\"\n", key->getCStringNoCopy(), s->getCStringNoCopy());
			} else {
				IOLog("AkitaTouchScreen: set %s = (%s)\n", key->getCStringNoCopy(),
				    value != NULL ? value->getMetaClass()->getClassName() : "null");
			}
		}
		OSSafeReleaseNULL(keys);
	}
	if (mode != NULL) {
		fTouchMode = (int)mode->unsigned32BitValue();
	}
	return super::setProperties(properties);
}

/* One frame: the hand, and under it every finger that is down or has just lifted (send_touch()). */
void
AkitaTouchScreen::send(uint32_t type, unsigned f, uint64_t time)
{
	uint32_t change = type == AKIOS_TOUCH_MOVE ? kIOHIDDigitizerEventPosition :
	    kIOHIDDigitizerEventRange | kIOHIDDigitizerEventTouch;
	bool any = false;
	IOHIDEvent * hand;

	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fFingers[i].touching;
	}

	/*
	 * (the options the event is made with go into each event's queue header, and iOS 27 keeps that word as the event
	 * object's options: IOHIDEventCreateDigitizerEvent puts absolute | range | touch there, so we do too)
	 */
	hand = IOHIDEvent::digitizerEvent(time, 0, kIOHIDDigitizerTransducerTypeHand, any, 0, 0, 0, 0, 0, 0, 0,
	    any ? kIOHIDTransducerRange | kIOHIDTransducerTouch : 0);
	if (hand == NULL) {
		return;
	}
	hand->setDoubleValue(kIOHIDEventFieldDigitizerX, fFingers[f].x / 65535.0, 0);
	hand->setDoubleValue(kIOHIDEventFieldDigitizerY, fFingers[f].y / 65535.0, 0);
	hand->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, 0);
	hand->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, (SInt32)(change & ~(uint32_t)kIOHIDDigitizerEventRange));
	hand->setIntegerValue(kIOHIDEventFieldDigitizerTouch, any);
	hand->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);
	hand->setIntegerValue(kIOHIDEventFieldIsBuiltIn, 1);

	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		bool touching = fFingers[i].touching;
		IOHIDEvent * finger;

		if (!touching && i != f) {
			continue;
		}
		finger = IOHIDEvent::digitizerEvent(time, i + 1, kIOHIDDigitizerTransducerTypeFinger, touching, 0, 0, 0, 0, 0,
		    0, 0, touching ? kIOHIDTransducerRange | kIOHIDTransducerTouch : 0);
		if (finger == NULL) {
			continue;
		}
		finger->setDoubleValue(kIOHIDEventFieldDigitizerX, fFingers[i].x / 65535.0, 0);
		finger->setDoubleValue(kIOHIDEventFieldDigitizerY, fFingers[i].y / 65535.0, 0);
		finger->setDoubleValue(kIOHIDEventFieldDigitizerPressure, touching ? 1.0 : 0.0, 0);
		finger->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, (SInt32)(i + 2));
		finger->setIntegerValue(kIOHIDEventFieldDigitizerEventMask,
		    (SInt32)((i == f ? change : 0) | (fFingers[i].edge ? kIOHIDDigitizerEventFromEdgeTip : 0)));
		finger->setIntegerValue(kIOHIDEventFieldDigitizerTouch, touching);
		finger->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);
		/* (IOHIDEventCreateDigitizerFingerEvent's quality orientation; radii 5.0 from digitizerEvent already) */
		finger->setDoubleValue(kIOHIDEventFieldDigitizerQuality, 1.0, 0);
		finger->setDoubleValue(kIOHIDEventFieldDigitizerDensity, 1.0, 0);
		finger->setDoubleValue(kIOHIDEventFieldDigitizerIrregularity, 1.0, 0);
		finger->setDoubleValue(kIOHIDEventFieldDigitizerQualityRadiiAccuracy, 0.0, 0);
		hand->appendChild(finger);
		finger->release();
	}
	/* (as the injection's hand: appendChild marks a collection, IOHIDEventAppendEvent does not) */
	hand->setIntegerValue(kIOHIDEventFieldDigitizerCollection, 0);

	dispatchEvent(hand);
	hand->release();
}

/* A sample, on the work loop: the producers are several threads (the panel's driver, usbmux, writers). */
IOReturn
AkitaTouchScreen::sampleGated(void * typeArg, void * fingerArg, void * xyArg, void * timeArg)
{
	uint32_t type = (uint32_t)(uintptr_t)typeArg;
	unsigned f = (unsigned)(uintptr_t)fingerArg;
	uint32_t x = (uint32_t)((uintptr_t)xyArg >> 16), y = (uint32_t)((uintptr_t)xyArg & 0xffff);
	uint64_t time = *(uint64_t *)timeArg;

	if (f >= MAX_FINGERS) {
		return kIOReturnSuccess;
	}
	if (type == AKIOS_TOUCH_DOWN && (fTouchMode & 0xff) == TOUCH_MODE_WAKE) {
		/* the display is off and a tap is to wake it: the lock button, and the rest of this touch is ours */
		if (akios_hid_buttons_up()) {
			akios_hid_buttons_post(0x0C, 0x30, 1);
			akios_hid_buttons_post(0x0C, 0x30, 0);
		} else if (!fToldNoButtons) {
			fToldNoButtons = true;
			IOLog("AkitaTouchScreen: a tap to wake, but no AkitaButtons (boot-arg hid_buttons=1) to press the lock button with\n");
		}
		fWaking |= 1u << f;
		return kIOReturnSuccess;
	}
	if (fWaking & (1u << f)) {
		if (type == AKIOS_TOUCH_UP) {
			fWaking &= ~(1u << f);
		}
		return kIOReturnSuccess;
	}
	if (type != AKIOS_TOUCH_DOWN && !fFingers[f].touching) {
		return kIOReturnSuccess;
	}
	fFingers[f].touching = type != AKIOS_TOUCH_UP;
	fFingers[f].x = x;
	fFingers[f].y = y;
	if (type == AKIOS_TOUCH_DOWN) {
		fFingers[f].edge = (uint64_t)x * EDGE_SIDE_DEN < (uint64_t)EDGE_SIDE_NUM * 65535 ||
		    (uint64_t)(65535 - x) * EDGE_SIDE_DEN < (uint64_t)EDGE_SIDE_NUM * 65535 ||
		    (uint64_t)y * EDGE_TOP_DEN < (uint64_t)EDGE_TOP_NUM * 65535 ||
		    (uint64_t)(65535 - y) * EDGE_BOTTOM_DEN < (uint64_t)EDGE_BOTTOM_NUM * 65535;
	}
	send(type, f, time);
	return kIOReturnSuccess;
}

void
AkitaTouchScreen::sample(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time)
{
	if (type < AKIOS_TOUCH_DOWN || type > AKIOS_TOUCH_UP) {
		return;
	}
	if (x > 65535) {
		x = 65535;
	}
	if (y > 65535) {
		y = 65535;
	}
	fGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &AkitaTouchScreen::sampleGated),
	    (void *)(uintptr_t)type, (void *)(uintptr_t)finger, (void *)(uintptr_t)((x << 16) | y), &time);
}

/*
 * Called from bsd_init once IOKit is up (next to akios_hid_buttons_init). Off unless boot-arg hid_touch=1: while the
 * injection's virtual touch screen exists, a second main display digitizer would get every touch twice.
 */
void
akios_hid_touch_init(void)
{
	uint32_t enabled = 0;
	AkitaTouchScreen * made;
	IOService * root;

	if (!PE_parse_boot_argn("hid_touch", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	made = OSTypeAlloc(AkitaTouchScreen);
	root = IOService::getServiceRoot();
	if (made == NULL || root == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		IOLog("AkitaTouchScreen: cannot be made\n");
		return;
	}
	made->setName("multi-touch");
	if (!made->attach(root)) {
		made->release();
		IOLog("AkitaTouchScreen: cannot attach\n");
		return;
	}
	if (!made->start(root)) {        /* (registers itself: IOHIDEventService::start) */
		made->detach(root);
		made->release();
		IOLog("AkitaTouchScreen: does not start\n");
		return;
	}
	__atomic_store_n(&gAkitaTouchScreen, made, __ATOMIC_RELEASE);   /* (keeps the reference from OSTypeAlloc) */
	IOLog("AkitaTouchScreen: up, registry ID 0x%llx\n", made->getRegistryEntryID());
}

/* From touch_post() (bsd/dev/hakios.input.c): a touch sample in the displayed area, 0..65535. Dropped until up. */
void
akios_hid_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time)
{
	AkitaTouchScreen * screen = __atomic_load_n(&gAkitaTouchScreen, __ATOMIC_ACQUIRE);

	if (screen != NULL) {
		screen->sample(type, finger, x, y, time);
	}
}
