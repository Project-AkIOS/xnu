/*
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
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
 * AkIOS: IOHIDEventServiceUserClient.cpp from IOHIDFamily-2238.100.59 (the header says what it is for). Apple's code;
 * the changes are marked "AkIOS:".
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSUnserialize.h>
#include <kern/clock.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>

#include "IOHIDEventServiceUserClient.h"
#include "IOHIDEventServiceQueue.h"
#include "IOHIDEventData.h"
#include "IOHIDEvent.h"

extern "C" void *get_bsdtask_info(task_t);

#define kQueueSizeMin   0
#define kQueueSizeFake  128
#define kQueueSizeMax   16384
#define kEventSizeMax   131072 // 128KB

#define kIOHIDSystemUserAccessServiceEntitlement "com.apple.hid.system.user-access-service"

//===========================================================================
// IOHIDEventServiceUserClient class

#define super IOUserClient2022

OSDefineMetaClassAndStructors(IOHIDEventServiceUserClient, IOUserClient2022)

//==============================================================================
// IOHIDEventServiceUserClient::sMethods
//==============================================================================
const IOExternalMethodDispatch2022 IOHIDEventServiceUserClient::sMethods[kIOHIDEventServiceUserClientNumCommands] = {
	{ //    kIOHIDEventServiceUserClientOpen
		.function = (IOExternalMethodAction) &IOHIDEventServiceUserClient::_open,
		.checkScalarInputCount = 1, .checkStructureInputSize = 0,
		.checkScalarOutputCount = 0, .checkStructureOutputSize = 0,
		.allowAsync = false, .checkEntitlement = NULL,
	},
	{ //    kIOHIDEventServiceUserClientClose
		.function = (IOExternalMethodAction) &IOHIDEventServiceUserClient::_close,
		.checkScalarInputCount = 1, .checkStructureInputSize = 0,
		.checkScalarOutputCount = 0, .checkStructureOutputSize = 0,
		.allowAsync = false, .checkEntitlement = NULL,
	},
	{ //    kIOHIDEventServiceUserClientCopyEvent
		.function = (IOExternalMethodAction) &IOHIDEventServiceUserClient::_copyEvent,
		.checkScalarInputCount = 2, .checkStructureInputSize = kIOUCVariableStructureSize,
		.checkScalarOutputCount = 0, .checkStructureOutputSize = kIOUCVariableStructureSize,
		.allowAsync = false, .checkEntitlement = NULL,
	},
	{ //    kIOHIDEventServiceUserClientSetElementValue
		.function = (IOExternalMethodAction) &IOHIDEventServiceUserClient::_setElementValue,
		.checkScalarInputCount = 3, .checkStructureInputSize = 0,
		.checkScalarOutputCount = 0, .checkStructureOutputSize = 0,
		.allowAsync = false, .checkEntitlement = NULL,
	},
	{ //    kIOHIDEventServiceUserClientCopyMatchingEvent
		.function = (IOExternalMethodAction) &IOHIDEventServiceUserClient::_copyMatchingEvent,
		.checkScalarInputCount = 0, .checkStructureInputSize = kIOUCVariableStructureSize,
		.checkScalarOutputCount = 0, .checkStructureOutputSize = kIOUCVariableStructureSize,
		.allowAsync = false, .checkEntitlement = NULL,
	},
};

enum {
	kUserClientStateOpen  = 0x1,
	kUserClientStateClose = 0x2
};

//==============================================================================
// IOHIDEventServiceUserClient::getService
//==============================================================================
IOService *
IOHIDEventServiceUserClient::getService( void )
{
	return _owner;
}

//==============================================================================
// IOHIDEventServiceUserClient::clientClose
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::clientClose( void )
{
	terminate();

	return kIOReturnSuccess;
}

//==============================================================================
// IOHIDEventServiceUserClient::registerNotificationPort
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::registerNotificationPort(
	mach_port_t                 port,
	UInt32                      type,
	UInt32                      refCon __unused )
{
	if (isInactive()) {
		return kIOReturnOffline;
	}

	return _commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this,
	           &IOHIDEventServiceUserClient::registerNotificationPortGated), port, (void *)(intptr_t)type);
}


IOReturn
IOHIDEventServiceUserClient::registerNotificationPortGated(mach_port_t port, UInt32 type __unused, UInt32 refCon __unused)
{
	releaseNotificationPort(_queuePort);
	_queuePort = port;

	if (_queue) {
		_queue->setNotificationPort(port);
	}

	return kIOReturnSuccess;
}

//==============================================================================
// IOHIDEventServiceUserClient::clientMemoryForType
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::clientMemoryForType(
	UInt32                      type __unused,
	IOOptionBits *              options,
	IOMemoryDescriptor **       memory )
{
	if (isInactive()) {
		return kIOReturnOffline;
	}

	return _commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this,
	           &IOHIDEventServiceUserClient::clientMemoryForTypeGated), options, memory);
}

//==============================================================================
// IOHIDEventServiceUserClient::clientMemoryForType
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::clientMemoryForTypeGated(
	IOOptionBits *              options,
	IOMemoryDescriptor **       memory )
{
	IOReturn ret = kIOReturnNoMemory;

	if (_queue) {
		IOMemoryDescriptor * memoryToShare = _queue->getMemoryDescriptor();

		// if we got some memory
		if (memoryToShare) {
			// Memory will be released by user client
			// when last map is destroyed.

			memoryToShare->retain();

			ret = kIOReturnSuccess;
		}

		// set the result
		*options = 0;
		*memory  = memoryToShare;
	}

	return ret;
}



//==============================================================================
// IOHIDEventServiceUserClient::externalMethod
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::externalMethod(uint32_t selector, IOExternalMethodArgumentsOpaque * args)
{
	ExternalMethodGatedArguments gatedArguments = {selector, args};

	return _commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this,
	           &IOHIDEventServiceUserClient::externalMethodGated), &gatedArguments);
}

//==============================================================================
// IOHIDEventServiceUserClient::externalMethodGated
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::externalMethodGated(ExternalMethodGatedArguments * arguments)
{
	if (isInactive()) {
		return kIOReturnOffline;
	}

	return dispatchExternalMethod(arguments->selector, arguments->arguments, sMethods,
	           sizeof(sMethods) / sizeof(sMethods[0]), this, NULL);
}


//==============================================================================
// IOHIDEventServiceUserClient::initWithTask
//
// AkIOS: Apple's refuses a task without the com.apple.hid.system.user-access-service entitlement. backboardd has it
// on an iPhone; here a task's entitlements depend on how far our code signing goes (and the event services are all
// built-in buttons and screens), so a task without it is told about in the log and let in.
//==============================================================================
bool
IOHIDEventServiceUserClient::initWithTask(task_t owningTask, void * security_id, UInt32 type)
{
	OSObject* entitlement = NULL;

	if (!super::initWithTask(owningTask, security_id, type)) {
		IOLog("IOHIDEventServiceUserClient: initWithTask failed\n");
		return false;
	}

	entitlement = copyClientEntitlement(owningTask, kIOHIDSystemUserAccessServiceEntitlement);
	if (entitlement != kOSBooleanTrue) {
		proc_t process = (proc_t)get_bsdtask_info(owningTask);
		char name[64];

		bzero(name, sizeof(name));
		if (process) {
			proc_name(proc_pid(process), name, sizeof(name));
		}
		IOLog("IOHIDEventServiceUserClient: %s is not entitled (%s); allowed\n", name,
		    kIOHIDSystemUserAccessServiceEntitlement);
	}
	OSSafeReleaseNULL(entitlement);

	_owner        = NULL;
	_commandGate  = NULL;
	_state        = 0;
	_queue        = NULL;

	return true;
}

//==============================================================================
// IOHIDEventServiceUserClient::start
//==============================================================================
bool
IOHIDEventServiceUserClient::start( IOService * provider )
{
	OSObject *    object;
	OSNumber *    num;
	uint32_t      queueSize = kQueueSizeMax;
	IOWorkLoop *  workLoop;
	OSSerializer * debugStateSerializer;
	uint32_t      forceNotifyUsagePair = 0;
	uint32_t      queueSizeOverride = 0;
	uint32_t      qOptions = 0;

	if (!super::start(provider)) {
		return false;
	}

	_owner = OSDynamicCast(IOHIDEventService, provider);
	if (!_owner) {
		return false;
	}

	_owner->retain();

	// If the provider's usage pair matches this boot arg, set force notify flag on queue.
	if (PE_parse_boot_argn("hidq_force_usage_pair", &forceNotifyUsagePair, sizeof(forceNotifyUsagePair))) {
		uint32_t usagePage  = (forceNotifyUsagePair >> 16) & 0xffff;
		uint32_t usage      = forceNotifyUsagePair & 0xffff;

		if (usagePage == _owner->getPrimaryUsagePage() && usage == _owner->getPrimaryUsage()) {
			qOptions |= kIOHIDEventServiceQueueOptionNotificationForce;
		}
	}

	// Use property for queue size, if it exists.
	object = provider->copyProperty(kIOHIDEventServiceQueueSize);
	num = OSDynamicCast(OSNumber, object);
	if (num) {
		queueSize = num->unsigned32BitValue();
	}
	OSSafeReleaseNULL(object);

	// Use boot-arg for queue size - takes precedence over property.
	if (PE_parse_boot_argn("hidq_size", &queueSizeOverride, sizeof(queueSizeOverride)) && queueSizeOverride) {
		queueSize = queueSizeOverride;
		provider->setProperty(kIOHIDEventServiceQueueSize, queueSizeOverride, 32);
	}

	if (queueSize) {
		_queue = IOHIDEventServiceQueue::withCapacity(this, queueSize, qOptions);
		if (!_queue) {
			return false;
		}
	}

	workLoop = getWorkLoop();
	if (!workLoop) {
		return false;
	}

	_commandGate = IOCommandGate::commandGate(this);
	if (!_commandGate || workLoop->addEventSource(_commandGate) != kIOReturnSuccess) {
		return false;
	}

	debugStateSerializer = OSSerializer::forTarget(this, OSMemberFunctionCast(OSSerializerCallback, this, &IOHIDEventServiceUserClient::serializeDebugState));
	if (debugStateSerializer) {
		setProperty("DebugState", debugStateSerializer);
		debugStateSerializer->release();
	}

	setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);
	setProperty(kIOUserClientDefaultLockingSetPropertiesKey, kOSBooleanTrue);
	setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey, kOSBooleanFalse);
	setProperty(kIOUserClientEntitlementsKey, kOSBooleanFalse);

	return true;
}

void
IOHIDEventServiceUserClient::stop( IOService * provider )
{
	close();

	IOWorkLoop * workLoop = getWorkLoop();

	if (workLoop && _commandGate) {
		workLoop->removeEventSource(_commandGate);
	}

	releaseNotificationPort(_queuePort);
	_queuePort = MACH_PORT_NULL;

	super::stop(provider);
}

//==============================================================================
// IOHIDEventServiceUserClient::_open
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::_open(
	IOHIDEventServiceUserClient *   target,
	void *                          reference __unused,
	IOExternalMethodArguments *     arguments)
{
	return target->open((IOOptionBits)arguments->scalarInput[0]);
}

//==============================================================================
// IOHIDEventServiceUserClient::open
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::open(IOOptionBits options)
{
	if (!_owner) {
		return kIOReturnOffline;
	}

	if (_state == kUserClientStateOpen) {
		return kIOReturnStillOpen;
	}

	_options = options;

	if (!_owner->open(  this,
	    options,
	    NULL,
	    OSMemberFunctionCast(IOHIDEventService::Action,
	    this, &IOHIDEventServiceUserClient::eventServiceCallback))) {
		return kIOReturnExclusiveAccess;
	}

	_state = kUserClientStateOpen;

	return kIOReturnSuccess;
}

//==============================================================================
// IOHIDEventServiceUserClient::_close
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::_close(
	IOHIDEventServiceUserClient *   target,
	void *                          reference __unused,
	IOExternalMethodArguments *     arguments __unused)
{
	return target->close();
}

//==============================================================================
// IOHIDEventServiceUserClient::close
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::close()
{
	if (_owner && _state == kUserClientStateOpen) {
		_owner->close(this, _options | kIOHIDOpenedByEventSystem);
		_state = kUserClientStateClose;
	}

	return kIOReturnSuccess;
}

//==============================================================================
// IOHIDEventServiceUserClient::_copyEvent
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::_copyEvent(
	IOHIDEventServiceUserClient *   target,
	void *                          reference __unused,
	IOExternalMethodArguments *     arguments)
{
	IOHIDEvent *    inEvent     = NULL;
	IOHIDEvent *    outEvent    = NULL;
	IOReturn        ret         = kIOReturnError;
	IOByteCount     length      = 0;
	IOMemoryMap *   mmap        = NULL;
	void *          outData     = NULL;
	size_t          outSize     = 0;

	if (arguments->structureInputSize >= kEventSizeMax) {
		return kIOReturnNoMemory;
	}

	if (arguments->structureInput && arguments->structureInputSize) {
		inEvent = IOHIDEvent::withBytes(arguments->structureInput, arguments->structureInputSize);
	}

	if (arguments->structureOutputDescriptor) {
		// Memory mapping could fail if process is terminating or userland is shutting down
		ret = arguments->structureOutputDescriptor->prepare();
		if (ret != kIOReturnSuccess) {
			goto exit;
		}

		mmap = arguments->structureOutputDescriptor->map();
		if (!mmap) {
			ret = kIOReturnNoMemory;
			goto map_fail;
		}

		outData = (void *)mmap->getVirtualAddress();
		outSize = arguments->structureOutputDescriptor->getLength();
	} else if (arguments->structureOutput) {
		outData = arguments->structureOutput;
		outSize = arguments->structureOutputSize;
	} else {
		IOLog("IOHIDEventServiceUserClient: _copyEvent: No output data\n");
		goto exit;
	}

	do {
		ret = target->copyEvent((IOHIDEventType)arguments->scalarInput[0], inEvent, &outEvent, (IOOptionBits)arguments->scalarInput[1]);

		if (ret) {
			break;
		}

		length = outEvent->getLength();

		if (length > outSize) {
			IOLog("IOHIDEventServiceUserClient: event length:%d expected:%ld\n", (unsigned int)length, outSize);
			ret = kIOReturnBadArgument;
			break;
		}

		outEvent->readBytes(outData, outSize);
		if (arguments->structureOutputDescriptor) {
			arguments->structureOutputDescriptorSize = (uint32_t)length;
		} else {
			arguments->structureOutputSize = (uint32_t)length;
		}
	} while (0);

map_fail:
	if (arguments->structureOutputDescriptor) {
		OSSafeReleaseNULL(mmap);
		arguments->structureOutputDescriptor->complete();
	}

exit:
	if (inEvent) {
		inEvent->release();
	}

	if (outEvent) {
		outEvent->release();
	}

	return ret;
}

//==============================================================================
// IOHIDEventServiceUserClient::copyEvent
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::copyEvent(IOHIDEventType type, IOHIDEvent * matching, IOHIDEvent ** event, IOOptionBits options)
{
	if (!event) {
		return kIOReturnBadArgument;
	}
	if (!_owner) {
		return kIOReturnOffline;
	}
	if (_state != kUserClientStateOpen) {
		return kIOReturnNotOpen;
	}

	*event = _owner->copyEvent(type, matching, options);

	return *event == NULL ? kIOReturnUnsupported : kIOReturnSuccess;
}

//==============================================================================
// IOHIDEventServiceUserClient::_setElementValue
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::_setElementValue(
	IOHIDEventServiceUserClient *   target,
	void *                          reference __unused,
	IOExternalMethodArguments *     arguments)
{
	return target->setElementValue((UInt32)arguments->scalarInput[0], (UInt32)arguments->scalarInput[1], (UInt32)arguments->scalarInput[2]);
}

//==============================================================================
// IOHIDEventServiceUserClient::setElementValue
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::setElementValue(UInt32 usagePage, UInt32 usage, UInt32 value)
{
	if (_owner && _state == kUserClientStateOpen) {
		return _owner->setElementValue(usagePage, usage, value);
	}
	return kIOReturnNoDevice;
}

//==============================================================================
// IOHIDEventServiceUserClient::_copyMatchingEvent
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::_copyMatchingEvent(
	IOHIDEventServiceUserClient *   target,
	void *                          reference __unused,
	IOExternalMethodArguments *     arguments)
{
	IOReturn ret = kIOReturnError;
	OSObject *obj = NULL;
	OSDictionary *matching = NULL;
	OSData *result = NULL;

	if (!arguments->structureVariableOutputData) {
		return kIOReturnBadArgument;
	}

	if (arguments->structureInput && arguments->structureInputSize) {
		obj = OSUnserializeXML((const char *)arguments->structureInput,
		    arguments->structureInputSize);
		matching = OSDynamicCast(OSDictionary, obj);
		if (!matching) {
			OSSafeReleaseNULL(obj);
			return kIOReturnBadArgument;
		}
	}

	ret = target->copyMatchingEvent(matching, &result);
	if (ret == kIOReturnSuccess && result) {
		// result will be released for us
		*arguments->structureVariableOutputData = result;
	}

	OSSafeReleaseNULL(obj);

	return ret;
}

//==============================================================================
// IOHIDEventServiceUserClient::copyMatchingEvent
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::copyMatchingEvent(OSDictionary *matching, OSData **eventData)
{
	IOReturn ret = kIOReturnError;
	IOHIDEvent *event = NULL;

	if (!eventData) {
		return kIOReturnBadArgument;
	}
	if (!_owner || _state != kUserClientStateOpen) {
		return kIOReturnNotOpen;
	}

	event = _owner->copyMatchingEvent(matching);
	if (!event) {
		return kIOReturnUnsupported;
	}

	*eventData = event->createBytes();
	ret = *eventData ? kIOReturnSuccess : kIOReturnNoMemory;

	OSSafeReleaseNULL(event);

	return ret;
}

//==============================================================================
// IOHIDEventServiceUserClient::didTerminate
//==============================================================================
bool
IOHIDEventServiceUserClient::didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
	close();

	return super::didTerminate(provider, options, defer);
}

//==============================================================================
// IOHIDEventServiceUserClient::free
//==============================================================================
void
IOHIDEventServiceUserClient::free()
{
	OSSafeReleaseNULL(_queue);
	OSSafeReleaseNULL(_owner);
	OSSafeReleaseNULL(_commandGate);

	super::free();
}

//==============================================================================
// IOHIDEventServiceUserClient::setProperties
//==============================================================================
IOReturn
IOHIDEventServiceUserClient::setProperties( OSObject * properties )
{
	return _owner ? _owner->setProperties(properties) : kIOReturnOffline;
}


//==============================================================================
// IOHIDEventServiceUserClient::eventServiceCallback
//==============================================================================
void
IOHIDEventServiceUserClient::eventServiceCallback(
	IOHIDEventService *             sender __unused,
	void *                          context __unused,
	IOHIDEvent *                    event,
	IOOptionBits                    options __unused)
{
	if (!_queue || _state != kUserClientStateOpen) {
		return;
	}

	_commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &IOHIDEventServiceUserClient::enqueueEventGated), event);
}

//==============================================================================
// IOHIDEventServiceUserClient::enqueueEventGated
//==============================================================================
void
IOHIDEventServiceUserClient::enqueueEventGated( IOHIDEvent * event)
{
	//enqueue the event
	if (_queue) {
		++_eventCount;
		_lastEventTime = mach_continuous_time();
		_lastEventType = event->getType();
		Boolean result = _queue->enqueueEvent(event);
		if (result == false) {
			_lastDroppedEventTime = _lastEventTime;
			++_droppedEventCount;
		}
	}
}


//====================================================================================================
// IOHIDEventServiceUserClient::serializeDebugState
//====================================================================================================
bool
IOHIDEventServiceUserClient::serializeDebugState(void * ref __unused, OSSerialize * serializer)
{
	bool          result = false;
	uint64_t      currentTime, deltaTime;
	uint64_t      nanoTime;
	OSDictionary  *debugDict = OSDictionary::withCapacity(6);
	OSNumber      *num;

	if (!debugDict) {
		return false;
	}

	currentTime =  mach_continuous_time();

	if (_queue) {
		debugDict->setObject("EventQueue", _queue);
	}

	if (_eventCount) {
		num = OSNumber::withNumber(_eventCount, 64);
		if (num) {
			debugDict->setObject("EnqueueEventCount", num);
			OSSafeReleaseNULL(num);
		}
	}
	if (_lastEventTime) {
		deltaTime = currentTime - _lastEventTime;
		absolutetime_to_nanoseconds(deltaTime, &nanoTime);
		num = OSNumber::withNumber(nanoTime, 64);
		if (num) {
			debugDict->setObject("LastEventTime", num);
			OSSafeReleaseNULL(num);
		}
	}
	if (_lastEventType) {
		num = OSNumber::withNumber(_lastEventType, 32);
		if (num) {
			debugDict->setObject("LastEventType", num);
			OSSafeReleaseNULL(num);
		}
	}
	if (_lastDroppedEventTime) {
		deltaTime = currentTime - _lastDroppedEventTime;
		absolutetime_to_nanoseconds(deltaTime, &nanoTime);
		num = OSNumber::withNumber(nanoTime, 64);
		if (num) {
			debugDict->setObject("LastDroppedEventTime", num);
			OSSafeReleaseNULL(num);
		}
	}
	if (_droppedEventCount) {
		num = OSNumber::withNumber(_droppedEventCount, 32);
		if (num) {
			debugDict->setObject("DroppedEventCount", num);
			OSSafeReleaseNULL(num);
		}
	}
	result = debugDict->serialize(serializer);
	debugDict->release();

	return result;
}
