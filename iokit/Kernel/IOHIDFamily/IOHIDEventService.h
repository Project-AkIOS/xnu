/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2009 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * AkIOS: IOHIDEventService from IOHIDFamily-2238.100.59, for services that make their events in the kernel (the
 * phone's buttons now, the touch screen next).
 *
 * The class name matters: IOHIDEventSystem (in backboardd) loads the generic IOHIDEventServicePlugin for a service
 * when IOObjectConformsTo(service, "IOHIDEventService") (_IOHIDServiceCreate, iOS 27.2.1 IOKit.framework). The plugin
 * then opens an IOHIDEventServiceUserClient on it with type 'esuc' and reads the events from its queue.
 *
 * What is kept is in Apple's order and does what Apple's does: start publishing the HID properties
 * (Transport, VendorID, ..., DeviceUsagePairs, ReportInterval, PrimaryUsagePage/PrimaryUsage), open/close by clients
 * with an event action each, dispatchEvent to every open client, dispatchKeyboardEvent, copyEvent/copyMatchingEvent
 * hooks, setSystemProperties, newUserClient for 'esuc'. Left out, because they belong to subsystems we do not have:
 * the IOHIDDevice/IOHIDInterface element parsing (getReportElements, parseSupportedElements, the pointer/digitizer/
 * multi-axis/game controller dispatchers built on elements), the legacy IOHIDSystem shims (IOHIDKeyboard/
 * IOHIDConsumer/IOHIDPointing nubs), the key-chord debug actions (stackshot/NMI), HIDRM device state, the fast path
 * user client, DriverKit (IOHIDEventService as a DriverKit class, OSAction completions), and matchPropertyTable's
 * DeviceUsagePairs matching (no personalities match on our services).
 */

#ifndef _IOKIT_HID_IOHIDEVENTSERVICE_H
#define _IOKIT_HID_IOHIDEVENTSERVICE_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include "IOHIDKeys.h"
#include "IOHIDEventTypes.h"
#include "IOHIDEvent.h"

enum {
	kHIDDispatchOptionPointerNoAcceleration         = 0x01,
	kHIDDispatchOptionPointerDisplayIntegrated      = 0x02,
	kHIDDispatchOptionPointerAffixToScreen          = kHIDDispatchOptionPointerDisplayIntegrated,
	kHIDDispatchOptionPointerAbsolutToRelative      = 0x04
};

enum {
	kHIDDispatchOptionScrollNoAcceleration              = (1 << 0),
	kHIDDispatchOptionScrollMomentumAny                 = 0x00e,
	kHIDDispatchOptionScrollMomentumContinue            = (1 << 1),
	kHIDDispatchOptionScrollMomentumStart               = (1 << 2),
	kHIDDispatchOptionScrollMomentumEnd                 = (1 << 3),

	kHIDDispatchOptionPhaseAny                          = 0xff0,
	kHIDDispatchOptionPhaseBegan                        = (1 << 4),
	kHIDDispatchOptionPhaseChanged                      = (1 << 5),
	kHIDDispatchOptionPhaseEnded                        = (1 << 6),
	kHIDDispatchOptionPhaseCanceled                     = (1 << 7),
	kHIDDispatchOptionPhaseMayBegin                     = (1 << 11),

	kHIDDispatchOptionDeliveryNotificationForce         = (1 << 12),
	kHIDDispatchOptionDeliveryNotificationSuppress      = (1 << 13)
};

enum {
	kHIDDispatchOptionKeyboardNoRepeat                  = (1 << 0)
};

class IOHIDEventService : public IOService
{
	OSDeclareAbstractStructors( IOHIDEventService )

	friend class AppleEmbeddedHIDEventService;
	friend class IOHIDEventServiceUserClient;

private:
	IOLock *                _clientDictLock;
	bool                    _readyForInputReports;

	struct ExpansionData {
		IOService *             provider;
		IOWorkLoop *            workLoop;
		OSArray *               deviceUsagePairs;
		IOCommandGate *         commandGate;
		OSDictionary *          clientDict;
		UInt32                  debugMask;
	};
	/*! @var reserved
	 *   Reserved for future use.  (Internal use only)  */
	ExpansionData *         _reserved;

	void publishProperties();

protected:
	virtual void            free(void) APPLE_KEXT_OVERRIDE;

	/*! @function handleOpen
	 *   @abstract Handle a client open on the interface.
	 *   @discussion This method is called by IOService::open() with the
	 *   arbitration lock held, and must return true to accept the client open.
	 *   @param client The client object that requested the open.
	 *   @param options Options passed to IOService::open().
	 *   @param argument Argument passed to IOService::open().
	 *   @result true to accept the client open, false otherwise. */
	virtual bool handleOpen(IOService *  client,
	    IOOptionBits options,
	    void *       argument) APPLE_KEXT_OVERRIDE;

	/*! @function handleClose
	 *   @abstract Handle a client close on the interface.
	 *   @param client The client object that requested the close.
	 *   @param options Options passed to IOService::close(). */
	virtual void handleClose(IOService * client, IOOptionBits options) APPLE_KEXT_OVERRIDE;

	/*! @function handleIsOpen
	 *   @abstract Query whether a client has an open on the interface.
	 *   @result true if the specified client, or any client if none (0) is
	 *   specified, presently has an open on this object. */
	virtual bool handleIsOpen(const IOService * client) const APPLE_KEXT_OVERRIDE;

	/*! @function handleStart
	 *   @abstract Prepare the hardware and driver to support I/O operations.
	 *   @discussion IOHIDEventService will call this method from start() before
	 *   any I/O operations are issued to the concrete subclass. A subclass that
	 *   overrides this method should begin its implementation by calling the
	 *   version in super, and then check the return value.
	 *   @param provider The provider argument passed to start().
	 *   @result True on success, or false otherwise. */
	virtual bool            handleStart( IOService * provider );

	/*! @function handleStop
	 *   @abstract Quiesce the hardware and stop the driver.
	 *   @param provider The provider argument passed to stop(). */
	virtual void            handleStop(  IOService * provider );

	virtual OSString *      getTransport();
	virtual UInt32          getLocationID();
	virtual UInt32          getVendorID();
	virtual UInt32          getVendorIDSource();
	virtual UInt32          getProductID();
	virtual UInt32          getVersion();
	virtual UInt32          getCountryCode();
	virtual OSString *      getManufacturer();
	virtual OSString *      getProduct();
	virtual OSString *      getSerialNumber();

	virtual IOReturn        setElementValue(
		UInt32                      usagePage,
		UInt32                      usage,
		UInt32                      value );

	virtual UInt32          getElementValue(
		UInt32                      usagePage,
		UInt32                      usage );

	virtual void            dispatchKeyboardEvent(
		AbsoluteTime                timeStamp,
		UInt32                      usagePage,
		UInt32                      usage,
		UInt32                      value,
		IOOptionBits                options = 0 );

public:
	bool                    readyForReports();

	virtual bool            init(OSDictionary * properties = 0) APPLE_KEXT_OVERRIDE;
	virtual bool            start( IOService * provider ) APPLE_KEXT_OVERRIDE;
	virtual void            stop( IOService * provider ) APPLE_KEXT_OVERRIDE;
	virtual IOReturn        setSystemProperties( OSDictionary * properties );
	virtual IOReturn        setProperties( OSObject * properties ) APPLE_KEXT_OVERRIDE;
	virtual IOReturn        newUserClient(
		task_t owningTask,
		void * securityID,
		UInt32 type,
		OSDictionary * properties,
		IOUserClient ** handler ) APPLE_KEXT_OVERRIDE;

protected:
	virtual OSArray *       getDeviceUsagePairs();

	virtual UInt32          getReportInterval();

public:
	typedef void (*Action)(OSObject *target, OSObject * sender, void *context, OSObject *event, IOOptionBits options);

	virtual bool            open(
		IOService *                 client,
		IOOptionBits                options,
		void *                      context,
		Action                      action);

protected:
	virtual void            dispatchEvent(IOHIDEvent * event, IOOptionBits options = 0);

	virtual UInt32          getPrimaryUsagePage();

	virtual UInt32          getPrimaryUsage();

public:
	virtual IOHIDEvent *    copyEvent(
		IOHIDEventType              type,
		IOHIDEvent *                matching = 0,
		IOOptionBits                options = 0);

protected:
	virtual IOHIDEvent *copyMatchingEvent(OSDictionary *matching);

	virtual void            dispatchKeyboardEvent(AbsoluteTime                timeStamp,
	    UInt32                      usagePage,
	    UInt32                      usage,
	    UInt32                      value,
	    UInt8                       pressCount,
	    UInt8                       longPress,
	    UInt8                       clickSpeed,
	    IOOptionBits                options = 0 );

public:
	virtual void            close( IOService * forClient, IOOptionBits options = 0 ) APPLE_KEXT_OVERRIDE;

	virtual IOReturn message(UInt32 type, IOService * provider, void * argument) APPLE_KEXT_OVERRIDE;
};

#endif /* !_IOKIT_HID_IOHIDEVENTSERVICE_H */
