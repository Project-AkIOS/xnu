/*
 *
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
 * AkIOS: IOHIDEvent from IOHIDFamily-2238.100.59, cut down to what the kernel's event services need: making an
 * event (keyboard so far), children, the sender, and writing it out in the layout iOS 27 reads (IOHIDEventData.h).
 * Left out: the constructors of the other event types (their data layouts are in Apple's generated headers, which
 * are not open source -- each comes back with its layout checked, as keyboard was), the fixed/double/data field
 * accessors that only those need, and the DriverKit parts. What is kept is in Apple's order.
 */

#ifndef _IOKIT_IOHIDEVENT_H
#define _IOKIT_IOHIDEVENT_H

#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <IOKit/IOTypes.h>
#include "IOHIDEventTypes.h"

#define ALIGNED_DATA_SIZE(data_size, align_size) ((((data_size - 1) / align_size) + 1) * align_size)

struct IOHIDEventData;

enum {
	kIOHIDEventTimestampTypeAbsolute = 0,
	kIOHIDEventTimestampTypeDefault = kIOHIDEventTimestampTypeAbsolute,
};
typedef uint32_t IOHIDEventTimestampType;

class IOHIDEvent : public OSObject
{
	OSDeclareDefaultStructors(IOHIDEvent)

	IOHIDEventData *    _data;
	OSArray *           _children;
	IOHIDEvent *        _parent;
	size_t              _capacity;
	UInt64              _timeStamp;
	UInt64              _senderID;
	uint64_t            _typeMask;
	IOOptionBits        _options;

	bool initWithCapacity(IOByteCount capacity);
	bool initWithType(IOHIDEventType type, IOByteCount additionalCapacity = 0);
	bool initWithTypeTimeStamp(IOHIDEventType type, UInt64 timeStamp, IOOptionBits options = 0, IOByteCount additionalCapacity = 0);
	IOByteCount readBytes(void * bytes, IOByteCount withLength, UInt32* outCount);

public:
	static IOHIDEvent *     withBytes(const void * bytes, IOByteCount size);

	static IOHIDEvent *     withType(IOHIDEventType type = kIOHIDEventTypeNULL, IOOptionBits options = 0);

	static IOHIDEvent *     keyboardEvent(UInt64 timeStamp, UInt32 usagePage, UInt32 usage, Boolean down,
	    IOOptionBits options = 0);

	static IOHIDEvent *     keyboardEvent(UInt64 timeStamp, UInt32 usagePage, UInt32 usage, Boolean down,
	    UInt8 pressCount, Boolean longPress, UInt8 clickSpeed, IOOptionBits options = 0);

	virtual void            appendChild(IOHIDEvent *childEvent);
	OSArray*                getChildren();

	virtual AbsoluteTime    getTimeStamp();
	virtual void            setTimeStamp(AbsoluteTime timeStamp);

	virtual IOHIDEventType  getType();
	virtual void            setType(IOHIDEventType type);

	virtual IOHIDEventPhaseBits  getPhase();
	virtual void            setPhase(IOHIDEventPhaseBits phase);

	virtual IOHIDEvent *    getEvent(IOHIDEventType type, IOOptionBits options = 0);

	virtual SInt32          getIntegerValue(IOHIDEventField key, IOOptionBits options = 0);
	virtual void            setIntegerValue(IOHIDEventField key, SInt32 value, IOOptionBits options = 0);

	virtual void            free(void) APPLE_KEXT_OVERRIDE;

	virtual size_t          getLength();
	virtual IOByteCount     readBytes(void *bytes, IOByteCount withLength);

	virtual OSData          *createBytes();

	virtual void            setSenderID(uint64_t senderID);

	virtual uint64_t        getLatency(uint32_t scaleFactor);

	virtual UInt64          getTimeStampOfType(IOHIDEventTimestampType type);
	virtual void            setTimeStampOfType(UInt64 timeStamp, IOHIDEventTimestampType type);

	inline  IOOptionBits    getOptions()
	{
		return _options;
	};
};

#endif /* _IOKIT_IOHIDEVENT_H */
