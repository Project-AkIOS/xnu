/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2013 Apple Computer, Inc.  All Rights Reserved.
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
 * AkIOS: IOHIDEventService.cpp from IOHIDFamily-2238.100.59 (IOHIDEventService.h says what is kept and what is not).
 * Apple's code where it is kept; the changes are marked "AkIOS:".
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>

#include "IOHIDEventService.h"
#include "IOHIDEvent.h"
#include "IOHIDEventData.h"
#include "IOHIDEventServiceUserClient.h"

#define     SET_HID_PROPERTIES_EMBEDDED(service)                                \
	service->setProperty(kIOHIDPrimaryUsagePageKey, getPrimaryUsagePage(), 32); \
	service->setProperty(kIOHIDPrimaryUsageKey, getPrimaryUsage(), 32);

/* AkIOS: as Apple's SET_HID_PROPERTIES, but a string or array that is not there is left out instead of set to NULL. */
#define     SET_HID_PROPERTY_OBJECT(service, key, object)                       \
	do { OSObject * _o = (object); if (_o) service->setProperty(key, _o); } while (0)

#define     SET_HID_PROPERTIES(service)                                         \
	SET_HID_PROPERTY_OBJECT(service, kIOHIDTransportKey, getTransport());       \
	service->setProperty(kIOHIDLocationIDKey, getLocationID(), 32);             \
	service->setProperty(kIOHIDVendorIDKey, getVendorID(), 32);                 \
	service->setProperty(kIOHIDVendorIDSourceKey, getVendorIDSource(), 32);     \
	service->setProperty(kIOHIDProductIDKey, getProductID(), 32);               \
	service->setProperty(kIOHIDVersionNumberKey, getVersion(), 32);             \
	service->setProperty(kIOHIDCountryCodeKey, getCountryCode(), 32);           \
	SET_HID_PROPERTY_OBJECT(service, kIOHIDManufacturerKey, getManufacturer()); \
	SET_HID_PROPERTY_OBJECT(service, kIOHIDProductKey, getProduct());           \
	SET_HID_PROPERTY_OBJECT(service, kIOHIDSerialNumberKey, getSerialNumber()); \
	SET_HID_PROPERTY_OBJECT(service, kIOHIDDeviceUsagePairsKey, getDeviceUsagePairs()); \
	service->setProperty(kIOHIDReportIntervalKey, getReportInterval(), 32);

#define         _provider                           _reserved->provider
#define     _workLoop                           _reserved->workLoop
#define     _deviceUsagePairs                   _reserved->deviceUsagePairs
#define     _commandGate                        _reserved->commandGate
#define     _debugMask                          _reserved->debugMask
#define     _clientDict                         _reserved->clientDict

//===========================================================================
// IOHIDClientData class
class IOHIDClientData : public OSObject
{
	OSDeclareDefaultStructors(IOHIDClientData)

	IOService * client;
	void *      context;
	IOHIDEventService::Action action;

public:
	static IOHIDClientData* withClientInfo(IOService *client, void* context, IOHIDEventService::Action action);
	inline IOService *  getClient()
	{
		return client;
	}
	inline void *       getContext()
	{
		return context;
	}
	inline IOHIDEventService::Action getAction()
	{
		return action;
	}
};

//===========================================================================
// IOHIDEventService class

#define super IOService

OSDefineMetaClassAndAbstractStructors( IOHIDEventService, IOService )

//====================================================================================================
// IOHIDEventService::init
//====================================================================================================
bool
IOHIDEventService::init( OSDictionary * properties )
{
	if (!super::init(properties)) {
		return false;
	}

	_reserved = IOMallocType(ExpansionData);
	if (!_reserved) {
		return false;
	}

	_clientDictLock = IOLockAlloc();
	if (!_clientDictLock) {
		return false;
	}

	_clientDict = OSDictionary::withCapacity(2);
	if (_clientDict == 0) {
		return false;
	}

	return true;
}

//====================================================================================================
// IOHIDEventService::start
//====================================================================================================
bool
IOHIDEventService::start( IOService * provider )
{
	_provider = provider;
	_provider->retain();

	if (!super::start(provider)) {
		return false;
	}

	if (!handleStart(provider)) {
		return false;
	}

	_workLoop = getWorkLoop();
	if (!_workLoop) {
		return false;
	}

	_workLoop->retain();

	_commandGate = IOCommandGate::commandGate(this);
	if (!_commandGate || (_workLoop->addEventSource(_commandGate) != kIOReturnSuccess)) {
		return false;
	}

	SET_HID_PROPERTIES(this);
	SET_HID_PROPERTIES_EMBEDDED(this);

	publishProperties();

	/*
	 * AkIOS: what Apple's kernel adds to its event services through an IOHIDProviderPropertyMerger personality
	 * (IOHIDFamily's Info.plist): HIDServiceSupport, which IOHIDEventSystem matches on, and the user client class.
	 * Nothing merges properties into ours, so each service says it itself.
	 */
	setProperty(kIOHIDServiceSupportKey, kOSBooleanTrue);
	setProperty(kIOUserClientClassKey, "IOHIDEventServiceUserClient");

	_readyForInputReports = true;

	registerService(kIOServiceAsynchronous);

	return true;
}

//====================================================================================================
// IOHIDEventService::stop
//====================================================================================================
void
IOHIDEventService::stop( IOService * provider )
{
	handleStop( provider );

	super::stop( provider );
}

//====================================================================================================
// IOHIDEventService::setSystemProperties
//
// AkIOS: Apple's also takes the HIDRM device state, and copies kIOHIDPropagatePropertyKeys to the provider (an
// IOHIDInterface there); neither applies to our services.
//====================================================================================================
IOReturn
IOHIDEventService::setSystemProperties( OSDictionary * properties )
{
	OSNumber *      number      = NULL;

	if (!properties) {
		return kIOReturnBadArgument;
	}

	if (properties->getObject(kIOHIDDeviceParametersKey) == kOSBooleanTrue) {
		OSObject *obj = copyProperty(kIOHIDEventServicePropertiesKey);
		OSDictionary * eventServiceProperties = OSDynamicCast(OSDictionary, obj);
		if (eventServiceProperties) {
			if (eventServiceProperties->setOptions(0, 0) & OSDictionary::kImmutable) {
				OSDictionary * copyEventServiceProperties = (OSDictionary*)eventServiceProperties->copyCollection();
				obj->release();
				eventServiceProperties = copyEventServiceProperties;
			}
		} else {
			OSSafeReleaseNULL(obj);
			eventServiceProperties = OSDictionary::withCapacity(4);
		}

		if (eventServiceProperties) {
			eventServiceProperties->merge(properties);
			eventServiceProperties->removeObject(kIOHIDResetKeyboardKey);
			eventServiceProperties->removeObject(kIOHIDResetPointerKey);
			eventServiceProperties->removeObject(kIOHIDDeviceParametersKey);
			setProperty(kIOHIDEventServicePropertiesKey, eventServiceProperties);
			eventServiceProperties->release();
		}
	}
	number = OSDynamicCast(OSNumber, properties->getObject(kIOHIDDebugConfigKey));
	if (number) {
		_debugMask = number->unsigned32BitValue();
	}

	return kIOReturnSuccess;
}

//====================================================================================================
// IOHIDEventService::setProperties
//====================================================================================================
IOReturn
IOHIDEventService::setProperties( OSObject * properties )
{
	OSDictionary *  propertyDict    = OSDynamicCast(OSDictionary, properties);
	IOReturn        ret             = kIOReturnBadArgument;
	if (propertyDict) {
		propertyDict->setObject(kIOHIDDeviceParametersKey, kOSBooleanTrue);
		ret = setSystemProperties( propertyDict );
		propertyDict->removeObject(kIOHIDDeviceParametersKey);
	}
	return ret;
}

//====================================================================================================
// IOHIDEventService::free
//====================================================================================================
void
IOHIDEventService::free()
{
	if (_reserved) {
		OSSafeReleaseNULL(_provider);

		if (_commandGate) {
			if (_workLoop) {
				_workLoop->removeEventSource(_commandGate);
			}
			_commandGate->release();
			_commandGate = 0;
		}

		OSSafeReleaseNULL(_deviceUsagePairs);

		if (_clientDict) {
			if (_clientDict->getCount()) {
				IOLog("IOHIDEventService: %s: service never closed by %u client(s)\n", getName(),
				    _clientDict->getCount());
			}
			_clientDict->release();
			_clientDict = NULL;
		}

		if (_workLoop) {
			// not our workloop. don't stop it.
			_workLoop->release();
			_workLoop = NULL;
		}

		IOFreeType(_reserved, ExpansionData);
		_reserved = NULL;
	}

	if (_clientDictLock) {
		IOLockFree(_clientDictLock);
		_clientDictLock = NULL;
	}

	super::free();
}

//==============================================================================
// IOHIDEventService::handleOpen
//==============================================================================
bool
IOHIDEventService::handleOpen(IOService *  client,
    IOOptionBits options __unused,
    void *       argument)
{
	bool accept = false;
	OSDictionary * clientDict;

	IOLockLock(_clientDictLock);
	do {
		// Was this object already registered as our client?
		if (_clientDict->getObject((const OSSymbol *)client)) {
			accept = true;
			break;
		}

		// Copy before mutation.
		clientDict = OSDictionary::withDictionary(_clientDict);
		if (!clientDict) {
			break;
		}

		_clientDict->release();
		_clientDict = clientDict;

		// Add the new client object to our client dict.
		if (!OSDynamicCast(IOHIDClientData, (OSObject *)argument) ||
		    !_clientDict->setObject((const OSSymbol *)client, (IOHIDClientData *)argument)) {
			break;
		}

		accept = true;
	} while (false);
	IOLockUnlock(_clientDictLock);

	return accept;
}

//==============================================================================
// IOHIDEventService::handleClose
//==============================================================================
void
IOHIDEventService::handleClose(IOService * client, IOOptionBits options __unused)
{
	OSDictionary * clientDict;

	IOLockLock(_clientDictLock);

	// Copy before mutation.
	clientDict = OSDictionary::withDictionary(_clientDict);
	if (clientDict) {
		_clientDict->release();
		_clientDict = clientDict;

		if (_clientDict->getObject((const OSSymbol *)client)) {
			_clientDict->removeObject((const OSSymbol *)client);
		}
	}

	IOLockUnlock(_clientDictLock);
}

//==============================================================================
// IOHIDEventService::handleIsOpen
//==============================================================================
bool
IOHIDEventService::handleIsOpen(const IOService * client) const
{
	bool ret;

	IOLockLock(_clientDictLock);
	if (client) {
		ret = _clientDict->getObject((const OSSymbol *)client) != NULL;
	} else {
		ret = (_clientDict->getCount() > 0);
	}
	IOLockUnlock(_clientDictLock);

	return ret;
}

//====================================================================================================
// IOHIDEventService::handleStart
//====================================================================================================
bool
IOHIDEventService::handleStart( IOService * provider __unused )
{
	return true;
}

//====================================================================================================
// IOHIDEventService::handleStop
//====================================================================================================
void
IOHIDEventService::handleStop(  IOService * provider __unused )
{
}

//====================================================================================================
// IOHIDEventService::getTransport ... getSerialNumber: the provider's properties, as Apple's.
//====================================================================================================
OSString *
IOHIDEventService::getTransport()
{
	return _provider ? OSDynamicCast(OSString, _provider->getProperty(kIOHIDTransportKey)) : 0;
}

OSString *
IOHIDEventService::getManufacturer()
{
	return _provider ? OSDynamicCast(OSString, _provider->getProperty(kIOHIDManufacturerKey)) : 0;
}

OSString *
IOHIDEventService::getProduct()
{
	return _provider ? OSDynamicCast(OSString, _provider->getProperty(kIOHIDProductKey)) : 0;
}

OSString *
IOHIDEventService::getSerialNumber()
{
	return _provider ? OSDynamicCast(OSString, _provider->getProperty(kIOHIDSerialNumberKey)) : 0;
}

/* (the numeric ones, which Apple writes out five times) */
static UInt32
providerNumber(IOService * provider, const char * key)
{
	UInt32 value = 0;

	if (provider) {
		OSObject *obj = provider->copyProperty(key);
		OSNumber * number = OSDynamicCast(OSNumber, obj);
		if (number) {
			value = number->unsigned32BitValue();
		}
		OSSafeReleaseNULL(obj);
	}
	return value;
}

UInt32
IOHIDEventService::getLocationID()
{
	return providerNumber(_provider, kIOHIDLocationIDKey);
}

UInt32
IOHIDEventService::getVendorID()
{
	return providerNumber(_provider, kIOHIDVendorIDKey);
}

UInt32
IOHIDEventService::getVendorIDSource()
{
	return providerNumber(_provider, kIOHIDVendorIDSourceKey);
}

UInt32
IOHIDEventService::getProductID()
{
	return providerNumber(_provider, kIOHIDProductIDKey);
}

UInt32
IOHIDEventService::getVersion()
{
	return providerNumber(_provider, kIOHIDVersionNumberKey);
}

UInt32
IOHIDEventService::getCountryCode()
{
	return providerNumber(_provider, kIOHIDCountryCodeKey);
}

//====================================================================================================
// IOHIDEventService::setElementValue
//====================================================================================================
IOReturn
IOHIDEventService::setElementValue(
	UInt32                      usagePage __unused,
	UInt32                      usage __unused,
	UInt32                      value __unused )
{
	return kIOReturnUnsupported;
}

//====================================================================================================
// IOHIDEventService::getElementValue
//====================================================================================================
UInt32
IOHIDEventService::getElementValue(
	UInt32                      usagePage __unused,
	UInt32                      usage __unused )
{
	return 0;
}

//====================================================================================================
// IOHIDEventService::dispatchKeyboardEvent
//====================================================================================================
void
IOHIDEventService::dispatchKeyboardEvent(
	AbsoluteTime                timeStamp,
	UInt32                      usagePage,
	UInt32                      usage,
	UInt32                      value,
	IOOptionBits                options)
{
	dispatchKeyboardEvent(timeStamp, usagePage, usage, value, 1, 0, 0, options);
}

bool
IOHIDEventService::readyForReports()
{
	return _readyForInputReports;
}

//==============================================================================
// IOHIDEventService::getDeviceUsagePairs
//==============================================================================
OSArray *
IOHIDEventService::getDeviceUsagePairs()
{
	//RY: Correctly deal with kIOHIDDeviceUsagePairsKey
	OSObject *obj = _provider ? _provider->copyProperty(kIOHIDDeviceUsagePairsKey) : NULL;
	OSArray * providerUsagePairs = OSDynamicCast(OSArray, obj);

	if (providerUsagePairs && (providerUsagePairs != _deviceUsagePairs)) {
		setProperty(kIOHIDDeviceUsagePairsKey, providerUsagePairs);
		if (_deviceUsagePairs) {
			_deviceUsagePairs->release();
		}

		_deviceUsagePairs = providerUsagePairs;
		_deviceUsagePairs->retain();
	} else if (!_deviceUsagePairs) {
		_deviceUsagePairs = OSArray::withCapacity(2);

		if (_deviceUsagePairs) {
			OSDictionary * pair = OSDictionary::withCapacity(2);

			if (pair) {
				OSNumber * number;

				number = OSNumber::withNumber(getPrimaryUsagePage(), 32);
				if (number) {
					pair->setObject(kIOHIDDeviceUsagePageKey, number);
					number->release();
				}

				number = OSNumber::withNumber(getPrimaryUsage(), 32);
				if (number) {
					pair->setObject(kIOHIDDeviceUsageKey, number);
					number->release();
				}

				_deviceUsagePairs->setObject(pair);
				pair->release();
			}
		}
	}
	OSSafeReleaseNULL(obj);
	return _deviceUsagePairs;
}

//==============================================================================
// IOHIDEventService::getReportInterval
//==============================================================================
UInt32
IOHIDEventService::getReportInterval()
{
	UInt32 interval = 8000; // default to 8 milliseconds
	OSObject *object = copyProperty(kIOHIDReportIntervalKey, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
	OSNumber *num = OSDynamicCast(OSNumber, object);

	if (num) {
		interval = num->unsigned32BitValue();
	}
	OSSafeReleaseNULL(object);

	return interval;
}

//==============================================================================
// IOHIDEventService::close
//==============================================================================
void
IOHIDEventService::close(IOService *forClient, IOOptionBits options)
{
	if (options & kIOHIDOpenedByEventSystem) {
		if (_provider && !isInactive()) {
			_provider->setProperty(kIOHIDDeviceOpenedByEventSystemKey, kOSBooleanFalse);
		}
	}
	super::close(forClient, options);
}

OSDefineMetaClassAndStructors(IOHIDClientData, OSObject)

IOHIDClientData *
IOHIDClientData::withClientInfo(IOService *client, void* context, IOHIDEventService::Action action)
{
	IOHIDClientData * data = OSTypeAlloc(IOHIDClientData);

	if (!data) {
	} else if (data->init()) {
		data->client  = client;
		data->context = context;
		data->action  = action;
	} else {
		data->release();
		data = NULL;
	}

	return data;
}

//==============================================================================
// IOHIDEventService::open
//==============================================================================
bool
IOHIDEventService::open(   IOService *                 client,
    IOOptionBits                options,
    void *                      context,
    Action                      action)
{
	IOHIDClientData * clientData = IOHIDClientData::withClientInfo(client, context, action);
	bool ret = false;

	if (clientData) {
		if (super::open(client, options, clientData)) {
			ret = true;
		}
		clientData->release();
	}

	return ret;
}

//==============================================================================
// IOHIDEventService::dispatchEvent
//
// The event goes to every client that has the service open, with this service's registry entry ID as its sender
// (iOS 27's event system takes the sender from the queue element's header). AkIOS: Apple's also tickles display
// sleep for a key down (IOHIDSystem, macOS), adds a perf child event under kIOHIDDebugPerfEvent and traces; not here.
//==============================================================================
void
IOHIDEventService::dispatchEvent(IOHIDEvent * event, IOOptionBits options)
{
	OSDictionary *          clientDict;
	OSCollectionIterator *  iterator;
	IOHIDClientData *       clientData;
	OSObject *              clientKey;
	IOService *             client;
	void *                  context;
	Action                  action;

	event->setSenderID(getRegistryEntryID());

	IOLockLock(_clientDictLock);

	_clientDict->retain();
	clientDict = _clientDict;

	IOLockUnlock(_clientDictLock);

	if (!(iterator = OSCollectionIterator::withCollection(clientDict))) {
		clientDict->release();
		return;
	}

	while ((clientKey = iterator->getNextObject())) {
		clientData = OSDynamicCast(IOHIDClientData, clientDict->getObject((const OSSymbol *)clientKey));

		if (!clientData) {
			continue;
		}

		client  = clientData->getClient();
		context = clientData->getContext();
		action  = clientData->getAction();

		if (action) {
			(*action)(client, this, context, event, options);
		}
	}

	iterator->release();
	clientDict->release();
}

//==============================================================================
// IOHIDEventService::getPrimaryUsagePage
//==============================================================================
UInt32
IOHIDEventService::getPrimaryUsagePage()
{
	UInt32          primaryUsagePage = 0;
	OSArray *       deviceUsagePairs = getDeviceUsagePairs();

	if (deviceUsagePairs && deviceUsagePairs->getCount()) {
		OSDictionary * pair = OSDynamicCast(OSDictionary, deviceUsagePairs->getObject(0));

		if (pair) {
			OSNumber * number = OSDynamicCast(OSNumber, pair->getObject(kIOHIDDeviceUsagePageKey));

			if (number) {
				primaryUsagePage = number->unsigned32BitValue();
			}
		}
	}

	return primaryUsagePage;
}

//==============================================================================
// IOHIDEventService::getPrimaryUsage
//==============================================================================
UInt32
IOHIDEventService::getPrimaryUsage()
{
	UInt32          primaryUsage            = 0;
	OSArray *       deviceUsagePairs        = getDeviceUsagePairs();

	if (deviceUsagePairs && deviceUsagePairs->getCount()) {
		OSDictionary * pair = OSDynamicCast(OSDictionary, deviceUsagePairs->getObject(0));

		if (pair) {
			OSNumber * number = OSDynamicCast(OSNumber, pair->getObject(kIOHIDDeviceUsageKey));

			if (number) {
				primaryUsage = number->unsigned32BitValue();
			}
		}
	}

	return primaryUsage;
}

//==============================================================================
// IOHIDEventService::copyEvent
//==============================================================================
IOHIDEvent *
IOHIDEventService::copyEvent(
	IOHIDEventType              type __unused,
	IOHIDEvent *                matching __unused,
	IOOptionBits                options __unused)
{
	return NULL;
}

//==============================================================================
// IOService::newUserClient
//
// Type 'esuc' (kIOHIDEventServiceUserClientType) is what IOHIDEventServicePlugin opens. AkIOS: Apple's also makes
// the fast path user client for its own type; there is none here.
//==============================================================================
IOReturn
IOHIDEventService::newUserClient(
	task_t owningTask,
	void * securityID,
	UInt32 type,
	OSDictionary * properties,
	IOUserClient ** handler )
{
	IOUserClient *client = NULL;

	if (type == kIOHIDEventServiceUserClientType) {
		client = OSTypeAlloc(IOHIDEventServiceUserClient);
	} else {
		return super::newUserClient(owningTask, securityID, type, properties, handler);
	}

	if (client) {
		if (!client->initWithTask(owningTask, securityID, type)) {
			client->release();
			return kIOReturnBadArgument;
		}

		if (!client->attach(this)) {
			client->release();
			return kIOReturnUnsupported;
		}

		if (!client->start(this)) {
			client->detach(this);
			client->release();
			return kIOReturnUnsupported;
		}

		*handler = client;
		return kIOReturnSuccess;
	}
	return kIOReturnNoMemory;
}

//==============================================================================
// IOHIDEventService::message
//==============================================================================

IOReturn
IOHIDEventService::message( UInt32 type, IOService * provider __unused, void * argument)
{
	IOReturn result = kIOReturnSuccess;

	if (type == kIOMessageServiceIsRequestingClose) {
		result = messageClients(type, argument);
	}

	return result;
}

//==============================================================================
// IOHIDEventService::copyMatchingEvent
//==============================================================================
IOHIDEvent *
IOHIDEventService::copyMatchingEvent(OSDictionary *matching __unused)
{
	return NULL;
}

//====================================================================================================
// IOHIDEventService::dispatchKeyboardEvent
//
// AkIOS: Apple's first tracks the keys held for its debug key chords (stackshot, sysdiagnose, NMI on the power
// button) and afterwards feeds the legacy IOHIDSystem shims; both left out. The event itself is Apple's.
//====================================================================================================
void
IOHIDEventService::dispatchKeyboardEvent(AbsoluteTime                timeStamp,
    UInt32                      usagePage,
    UInt32                      usage,
    UInt32                      value,
    UInt8                       pressCount,
    UInt8                       longPress,
    UInt8                       clickSpeed,
    IOOptionBits                options)
{
	if (!_readyForInputReports) {
		return;
	}

	IOHIDEvent * event = IOHIDEvent::keyboardEvent(timeStamp, usagePage, usage, (value != 0), pressCount, longPress,
	    clickSpeed, options);
	if (event) {
		dispatchEvent(event);
		event->release();
	}
}

//====================================================================================================
// IOHIDEventService::publishProperties
//
// The provider's properties that the event system reads off the service, copied up when they have the right type.
// AkIOS: only those that can matter on a phone (Apple's list also has BootProtocol, the pointer/scroll acceleration
// keys, the fast path and spatial accessory keys and UniformTypeIdentifiers).
//====================================================================================================
void
IOHIDEventService::publishProperties()
{
	static const char * const keys[] = { kIOHIDPhysicalDeviceUniqueIDKey, kIOHIDBuiltInKey };

	if (!_provider) {
		return;
	}
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		OSObject * object = _provider->copyProperty(keys[i]);
		if ((i == 0 && OSDynamicCast(OSString, object)) || (i == 1 && OSDynamicCast(OSBoolean, object))) {
			setProperty(keys[i], object);
		}
		OSSafeReleaseNULL(object);
	}
}
