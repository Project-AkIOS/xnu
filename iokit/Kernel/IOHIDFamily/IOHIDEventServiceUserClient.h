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
 * AkIOS: IOHIDEventServiceUserClient from IOHIDFamily-2238.100.59: the connection IOHIDEventServicePlugin (in
 * backboardd's IOHIDEventSystem) opens on an event service.
 *
 * The protocol, as iOS 27.2.1 uses it (rootfs IOHIDEventServicePlugin; kernelcache IOExternalMethodDispatch2022[5] at
 * 0xfffffff00801b3e8, see ref/drivers/touch-buttons-spec.md 1.7): IOServiceOpen type 'esuc'; selector 0 open(options),
 * 1 close(options), 2 copyEvent(type, options; event in -> event out), 3 setElementValue(page, usage, value),
 * 4 copyMatchingEvent(XML dictionary -> event); memory type 0 = the event queue; notification port type 0 = the
 * queue's "data available" port; setProperties = the event system's property sets, passed to the service.
 * All of it is Apple's code; changed: the entitlement check (below) and the debug logging.
 */

#ifndef _IOKIT_IOHIDEVENTSERVICEUSERCLIENT_H
#define _IOKIT_IOHIDEVENTSERVICEUSERCLIENT_H

#define kIOHIDEventServiceUserClientType 'esuc'

enum IOHIDEventServiceUserClientCommandCodes {
	kIOHIDEventServiceUserClientOpen,
	kIOHIDEventServiceUserClientClose,
	kIOHIDEventServiceUserClientCopyEvent,
	kIOHIDEventServiceUserClientSetElementValue,
	kIOHIDEventServiceUserClientCopyMatchingEvent,
	kIOHIDEventServiceUserClientNumCommands
};

#ifdef KERNEL

#include <IOKit/IOUserClient.h>
#include <IOKit/IOCommandGate.h>
#include "IOHIDEventService.h"

class IOHIDEventServiceQueue;

class IOHIDEventServiceUserClient : public IOUserClient2022
{
	OSDeclareDefaultStructors(IOHIDEventServiceUserClient)

private:
	typedef struct ExternalMethodGatedArguments {
		uint32_t                          selector;
		IOExternalMethodArgumentsOpaque * arguments;
	} ExternalMethodGatedArguments;

	static const IOExternalMethodDispatch2022 sMethods[kIOHIDEventServiceUserClientNumCommands];

	IOHIDEventService *         _owner;
	IOHIDEventServiceQueue *    _queue;
	IOOptionBits                _options;
	IOCommandGate *             _commandGate;
	uint32_t                    _state;
	uint64_t                    _lastEventTime;
	uint32_t                    _lastEventType;
	uint32_t                    _droppedEventCount;
	uint64_t                    _lastDroppedEventTime;
	uint64_t                    _eventCount;
	mach_port_t                 _queuePort;

	void eventServiceCallback(  IOHIDEventService *             sender,
	    void *                          context,
	    IOHIDEvent *                    event,
	    IOOptionBits                    options);

	static IOReturn _open(      IOHIDEventServiceUserClient *   target,
	    void *                          reference,
	    IOExternalMethodArguments *     arguments);

	static IOReturn _close(     IOHIDEventServiceUserClient *   target,
	    void *                          reference,
	    IOExternalMethodArguments *     arguments);

	static IOReturn _copyEvent(
		IOHIDEventServiceUserClient *   target,
		void *                          reference,
		IOExternalMethodArguments *     arguments);

	static IOReturn _setElementValue(
		IOHIDEventServiceUserClient *   target,
		void *                          reference,
		IOExternalMethodArguments *     arguments);

	static IOReturn _copyMatchingEvent(
		IOHIDEventServiceUserClient *   target,
		void *                          reference,
		IOExternalMethodArguments *     arguments);

	void enqueueEventGated( IOHIDEvent * event);

	bool   serializeDebugState(void * ref, OSSerialize * serializer);

protected:
	// IOUserClient methods
	virtual IOReturn clientClose( void ) APPLE_KEXT_OVERRIDE;
	virtual void stop( IOService * provider ) APPLE_KEXT_OVERRIDE;

	virtual IOService * getService( void ) APPLE_KEXT_OVERRIDE;

	virtual IOReturn registerNotificationPort(
		mach_port_t                     port,
		UInt32                          type,
		UInt32                          refCon ) APPLE_KEXT_OVERRIDE;

	IOReturn registerNotificationPortGated(mach_port_t          port,
	    UInt32               type,
	    UInt32               refCon);

	virtual IOReturn clientMemoryForType(
		UInt32                          type,
		IOOptionBits *                  options,
		IOMemoryDescriptor **           memory ) APPLE_KEXT_OVERRIDE;

	IOReturn clientMemoryForTypeGated(
		IOOptionBits *                  options,
		IOMemoryDescriptor **           memory );

	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArgumentsOpaque * args) APPLE_KEXT_OVERRIDE;

	IOReturn externalMethodGated(ExternalMethodGatedArguments *arguments);

public:
	// others
	virtual bool initWithTask(task_t owningTask, void * security_id, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual bool start( IOService * provider ) APPLE_KEXT_OVERRIDE;
	virtual bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties( OSObject * properties ) APPLE_KEXT_OVERRIDE;
	virtual IOReturn open(IOOptionBits options);
	virtual IOReturn close();
	virtual IOReturn copyEvent(IOHIDEventType type, IOHIDEvent * matching, IOHIDEvent ** event, IOOptionBits options = 0);
	virtual IOReturn setElementValue(UInt32 usagePage, UInt32 usage, UInt32 value);
	virtual IOReturn copyMatchingEvent(OSDictionary *matching, OSData **eventData);
};

#endif /* KERNEL */

#endif /* _IOKIT_IOHIDEVENTSERVICEUSERCLIENT_H */
