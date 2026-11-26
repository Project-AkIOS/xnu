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
 * AkIOS: IOHIDEvent.cpp from IOHIDFamily-2238.100.59 (see IOHIDEvent.h for what is left out). The serialization --
 * getLength, readBytes, createBytes, withBytes -- is Apple's, line for line; the integer field accessors, which Apple
 * builds from generated macros, are written out for the fields of the types we have.
 */

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <os/overflow.h>
#include "IOHIDEvent.h"
#include "IOHIDEventData.h"

#define EXTERNAL ((unsigned int) -1)

#define super OSObject
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

OSDefineMetaClassAndStructors(IOHIDEvent, OSObject)

//==============================================================================
// IOHIDEvent::initWithCapacity
//==============================================================================
bool
IOHIDEvent::initWithCapacity(IOByteCount capacity)
{
	if (!super::init()) {
		return false;
	}

	if (capacity >= UINT32_MAX) {
		return false;
	}

	if (_data && (!capacity || _capacity < capacity)) {
		// clean out old data's storage if it isn't big enough
		IOFreeData(_data, _capacity);
		_data = NULL;
	}

	_capacity = capacity;

	if (!_capacity) {
		return false;
	}

	if (!_data && !(_data = (IOHIDEventData *) IOMallocZeroData(_capacity))) {
		return false;
	}

	_data->size = (uint32_t)_capacity;
	_children = NULL;

	return true;
}

//==============================================================================
// IOHIDEvent::initWithType
//==============================================================================
bool
IOHIDEvent::initWithType(IOHIDEventType type, IOByteCount additionalCapacity)
{
	size_t capacity = IOHIDEventGetDataSize(type);

	/* (a type whose layout we do not have yet: no event rather than a wrong one) */
	if (capacity == 0) {
		return false;
	}

	if (!initWithCapacity((IOByteCount)(capacity + additionalCapacity))) {
		return false;
	}

	_data->type = type;
	_typeMask   = IOHIDEventTypeMask(type);

	return true;
}

//==============================================================================
// IOHIDEvent::initWithTypeTimeStamp
//==============================================================================
bool
IOHIDEvent::initWithTypeTimeStamp(IOHIDEventType type, UInt64 timeStamp, IOOptionBits options, IOByteCount additionalCapacity)
{
	if (!initWithType(type, additionalCapacity)) {
		return false;
	}

	_timeStamp  = timeStamp;
	_options    = options;

	_data->options = options;

	return true;
}

//==============================================================================
// IOHIDEvent::free
//==============================================================================
void
IOHIDEvent::free()
{
	if (_capacity != EXTERNAL && _data && _capacity) {
		IOFreeData(_data, _capacity);
		_data = NULL;
		_capacity = 0;
	}

	if (_children) {
		_children->release();
		_children = NULL;
	}
	super::free();
}

//==============================================================================
// IOHIDEvent::getTimeStamp
//
// (Apple's also converts from mach continuous time when the event was made with kIOHIDEventOptionContinuousTime;
// our events are all stamped with mach_absolute_time.)
//==============================================================================
AbsoluteTime
IOHIDEvent::getTimeStamp()
{
	return _timeStamp;
}

//==============================================================================
// IOHIDEvent::setTimeStamp
//==============================================================================
void
IOHIDEvent::setTimeStamp( AbsoluteTime timeStamp )
{
	_timeStamp = timeStamp;
}

//==============================================================================
// IOHIDEvent::getTimeStampOfType
//==============================================================================
UInt64
IOHIDEvent::getTimeStampOfType(IOHIDEventTimestampType type __unused)
{
	return _timeStamp;
}

//==============================================================================
// IOHIDEvent::setTimeStampOfType
//==============================================================================
void
IOHIDEvent::setTimeStampOfType(UInt64 timeStamp, IOHIDEventTimestampType type __unused)
{
	_timeStamp = timeStamp;
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// IOHIDEvent::withType
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
IOHIDEvent *
IOHIDEvent::withType(IOHIDEventType type, IOOptionBits options)
{
	IOHIDEvent *me = OSTypeAlloc(IOHIDEvent);

	if (me && !me->initWithType(type)) {
		me->release();
		return NULL;
	}

	if (me) {
		me->_options = options;
	}

	return me;
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// IOHIDEvent::keyboardEvent
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
IOHIDEvent *
IOHIDEvent::keyboardEvent(UInt64 timeStamp, UInt32 usagePage, UInt32 usage, Boolean down, IOOptionBits options)
{
	IOHIDEvent *me = OSTypeAlloc(IOHIDEvent);

	if (me && !me->initWithTypeTimeStamp(kIOHIDEventTypeKeyboard, timeStamp, options | kIOHIDEventOptionIsAbsolute)) {
		me->release();
		return NULL;
	}
	if (!me) {
		return NULL;
	}

	IOHIDKeyboardEventData * keyboard = (IOHIDKeyboardEventData *)me->_data;
	keyboard->usagePage = (uint16_t)usagePage;
	keyboard->usage = (uint16_t)usage;
	keyboard->down = down;
	keyboard->pressCount = 1;

	return me;
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// IOHIDEvent::keyboardEvent
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
IOHIDEvent *
IOHIDEvent::keyboardEvent(UInt64 timeStamp, UInt32 usagePage, UInt32 usage, Boolean down, UInt8 pressCount,
    Boolean longPress, UInt8 clickSpeed, IOOptionBits options)
{
	IOHIDEvent * me = NULL;

	me = IOHIDEvent::keyboardEvent(timeStamp, usagePage, usage, down, options);

	if (me) {
		IOHIDKeyboardEventData * keyboard = (IOHIDKeyboardEventData *)me->_data;
		keyboard->pressCount = pressCount;
		keyboard->longPress = longPress ? 1 : 0;
		keyboard->clickSpeed = clickSpeed & 3;
	}

	return me;
}

//==============================================================================
// IOHIDEvent::appendChild
//==============================================================================
void
IOHIDEvent::appendChild(IOHIDEvent *childEvent)
{
	childEvent->_parent = this;
	if (!_children) {
		const OSObject *events[] = { childEvent };

		_children = OSArray::withObjects(events, 1);

		_data->options |= kIOHIDEventOptionIsCollection;
	} else {
		_children->setObject(childEvent);
	}
}

//==============================================================================
// IOHIDEvent::getType
//==============================================================================
IOHIDEventType
IOHIDEvent::getType()
{
	return _data->type;
}

//==============================================================================
// IOHIDEvent::setType
//==============================================================================
void
IOHIDEvent::setType(IOHIDEventType type)
{
	initWithType(type);
}

//==============================================================================
// IOHIDEvent::getEvent
//==============================================================================
IOHIDEvent *
IOHIDEvent::getEvent(IOHIDEventType type, IOOptionBits options __unused)
{
	return (_data->type == type) ? this : NULL;
}

//==============================================================================
// IOHIDEvent::getIntegerValue
//
// Apple's goes through the generated GET_EVENT_VALUE macros. Here: the base fields of every event and the keyboard
// fields, by the offsets IOHIDEventData.h has (the same as iOS 27's IOHIDEventGetIntegerValueWithOptions).
//==============================================================================
SInt32
IOHIDEvent::getIntegerValue(IOHIDEventField key, IOOptionBits options)
{
	IOHIDEventType  fieldEvType = IOHIDEventFieldEventType(key);
	IOHIDEvent *    ev          = (fieldEvType == kIOHIDEventTypeNULL) ? this : getEvent(fieldEvType, options);

	if (!ev) {
		return 0;
	}
	if (fieldEvType == kIOHIDEventTypeNULL) {
		if (key == kIOHIDEventFieldIsBuiltIn) {
			return (ev->_data->options & kIOHIDEventOptionIsBuiltIn) ? 1 : 0;
		}
		return 0;
	}
	if (fieldEvType == kIOHIDEventTypeKeyboard) {
		IOHIDKeyboardEventData * keyboard = (IOHIDKeyboardEventData *)ev->_data;

		switch (key) {
		case kIOHIDEventFieldKeyboardUsagePage:
			return keyboard->usagePage;
		case kIOHIDEventFieldKeyboardUsage:
			return keyboard->usage;
		case kIOHIDEventFieldKeyboardDown:
			return (SInt32)keyboard->down;
		case kIOHIDEventFieldKeyboardRepeat:
			return (ev->_data->options & kIOHIDEventOptionIsRepeat) ? 1 : 0;
		case kIOHIDEventFieldKeyboardPressCount:
			return keyboard->pressCount;
		case kIOHIDEventFieldKeyboardLongPress:
			return keyboard->longPress;
		case kIOHIDEventFieldKeyboardClickSpeed:
			return keyboard->clickSpeed;
		case kIOHIDEventFieldKeyboardStickyKeyPhase:
			return keyboard->stickyKeyPhase;
		case kIOHIDEventFieldKeyboardStickyKeyToggle:
			return keyboard->stickyKeyToggle;
		default:
			return 0;
		}
	}
	return 0;
}

//==============================================================================
// IOHIDEvent::setIntegerValue
//==============================================================================
void
IOHIDEvent::setIntegerValue(IOHIDEventField key, SInt32 value, IOOptionBits options)
{
	IOHIDEventType  fieldEvType = IOHIDEventFieldEventType(key);
	IOHIDEvent *    ev          = (fieldEvType == kIOHIDEventTypeNULL) ? this : getEvent(fieldEvType, options);

	if (!ev) {
		return;
	}
	if (fieldEvType == kIOHIDEventTypeNULL) {
		if (key == kIOHIDEventFieldIsBuiltIn) {
			if (value) {
				ev->_data->options |= kIOHIDEventOptionIsBuiltIn;
			} else {
				ev->_data->options &= ~(uint32_t)kIOHIDEventOptionIsBuiltIn;
			}
		}
		return;
	}
	if (fieldEvType == kIOHIDEventTypeKeyboard) {
		IOHIDKeyboardEventData * keyboard = (IOHIDKeyboardEventData *)ev->_data;

		switch (key) {
		case kIOHIDEventFieldKeyboardUsagePage:
			keyboard->usagePage = (uint16_t)value;
			break;
		case kIOHIDEventFieldKeyboardUsage:
			keyboard->usage = (uint16_t)value;
			break;
		case kIOHIDEventFieldKeyboardDown:
			keyboard->down = (uint32_t)value;
			break;
		case kIOHIDEventFieldKeyboardPressCount:
			keyboard->pressCount = (uint8_t)value;
			break;
		case kIOHIDEventFieldKeyboardLongPress:
			keyboard->longPress = value ? 1 : 0;
			break;
		case kIOHIDEventFieldKeyboardClickSpeed:
			keyboard->clickSpeed = (uint32_t)value & 3;
			break;
		default:
			break;
		}
	}
}

//==============================================================================
// IOHIDEvent::getLength
//==============================================================================
size_t
IOHIDEvent::getLength()
{
	IOByteCount length = _data->size;

	length += sizeof(IOHIDSystemQueueElement);

	if (_children) {
		UInt32          i, childCount;
		IOHIDEvent *    child;

		childCount = _children->getCount();

		for (i = 0; i < childCount; i++) {
			if ((child = (IOHIDEvent *)_children->getObject(i))) {
				length += child->getLength();
			}
		}
	}

	return length;
}

//==============================================================================
// IOHIDEvent::withBytes
//
// The reverse of readBytes, for the matching event a client may pass to copyEvent.
//==============================================================================
IOHIDEvent *
IOHIDEvent::withBytes(const void * bytes, IOByteCount size)
{
	IOHIDSystemQueueElement *   queueElement = NULL;
	IOHIDEvent *                parent      = NULL;
	UInt32                      index       = 0;
	UInt32                      offset      = 0;
	UInt32                      sz          = 0;
	UInt32                      attributeLength = 0;
	UInt32                      eventCount = 0;
	UInt32                      eventDataSize = 0;
	UInt32                      eventDataType = 0;

	if (!bytes || !size) {
		return NULL;
	}

	for (index = 0; offset < size; index++) {
		if (sizeof(IOHIDSystemQueueElement) >= (size - offset)) {
			break;
		}

		queueElement = (IOHIDSystemQueueElement *)((const UInt8 *)bytes + offset);
		attributeLength = queueElement->attributeLength;

		// to prevent complier reordering
		// local assignment should be enforced
		// before validation (55894984)
		COMPILER_BARRIER();

		if (os_add3_overflow(sizeof(IOHIDSystemQueueElement), attributeLength, offset, &offset)) {
			break;
		}
		COMPILER_BARRIER();

		if (offset >= size) {
			break;
		}

		if ((size - offset) < sizeof(IOHIDEventData)) {
			break;
		}

		IOHIDEventData *eventData = (IOHIDEventData *)(queueElement->payload + attributeLength);

		eventDataType = eventData->type;
		eventDataSize = eventData->size;
		COMPILER_BARRIER();

		if (eventDataType >= kIOHIDEventTypeCount) {
			break;
		}

		// Back calculate additional capacity needed
		sz = (UInt32)IOHIDEventGetDataSize(eventDataType);
		if (sz == 0 || sz > eventDataSize) {
			break;
		}

		IOHIDEvent *event = OSTypeAlloc(IOHIDEvent);

		if (!event) {
			break;
		}

		if (!event->initWithType(eventDataType, eventDataSize - sz) ||
		    eventDataSize != event->_data->size ||
		    (size - offset) < eventDataSize) {
			event->release();
			break;
		}

		bcopy(eventData, event->_data, event->_data->size);
		event->_timeStamp = queueElement->timeStamp;
		event->_options = queueElement->options;
		event->_senderID = queueElement->senderID;

		if (!parent) {
			parent = event;
			eventCount = queueElement->eventCount;
		} else {
			//Append event here;
			parent->appendChild(event);
			event->release();
		}

		if (eventCount == 0 || index >= (eventCount - 1)) {
			break;
		}

		if (os_add_overflow(offset, eventDataSize, &offset)) {
			break;
		}
		COMPILER_BARRIER();
	}

	return parent;
}

//==============================================================================
// IOHIDEvent::getChildren
//==============================================================================
OSArray*
IOHIDEvent::getChildren()
{
	return _children;
}

//==============================================================================
// IOHIDEvent::readBytes
//==============================================================================
IOByteCount
IOHIDEvent::readBytes(void * bytes, IOByteCount withLength)
{
	UInt32 count = 0;
	return readBytes(bytes, withLength, &count);
}

//==============================================================================
// IOHIDEvent::readBytes
//
// One IOHIDSystemQueueElement header and the data for this event, then the same for each child, depth first; the
// first header's eventCount ends up as the number of events written (iOS 27's reader takes the count from there).
//==============================================================================
IOByteCount
IOHIDEvent::readBytes(void * bytes, IOByteCount withLength, UInt32* outCount)
{
	IOByteCount size = 0;
	IOHIDSystemQueueElement * queueElement = NULL;

	if (withLength < sizeof(IOHIDSystemQueueElement)) {
		return 0;
	}

	queueElement    = (IOHIDSystemQueueElement *)bytes;

	queueElement->timeStamp         = _timeStamp;
	queueElement->options           = _options;
	queueElement->senderID          = _senderID;
	queueElement->attributeLength   = 0;

	size += sizeof(IOHIDSystemQueueElement);

	if ((size + _data->size) > withLength) {
		return 0;
	}

	if (_parent) {
		if (_parent->_data) {
			_data->depth = _parent->_data->depth + 1;
		}
	}

	bcopy(_data, (UInt8*)bytes + size, _data->size);
	size += _data->size;

	if (_children) {
		UInt32          i, childCount;
		IOHIDEvent *    child;

		childCount = _children->getCount();

		for (i = 0; i < childCount; i++) {
			if ((child = (IOHIDEvent *)_children->getObject(i))) {
				size += child->readBytes((UInt8*)bytes + size, withLength - size, outCount);
			}
		}
	}

	if (outCount) {
		*outCount = *outCount + 1;
		queueElement->eventCount = *outCount;
	} else {
		queueElement->eventCount = 1;
	}

	return size;
}

//==============================================================================
// IOHIDEvent::createBytes
//==============================================================================
// Note: this function only serializes one level of children.
OSData *
IOHIDEvent::createBytes()
{
	OSData *result = NULL;
	IOHIDSystemQueueElement queueElement = { };

	result = OSData::withCapacity((unsigned int)getLength());
	if (!result) {
		return NULL;
	}

	queueElement.timeStamp         = _timeStamp;
	queueElement.options           = _options;
	queueElement.eventCount        = 1 + (_children ? _children->getCount() : 0);
	queueElement.senderID          = _senderID;
	queueElement.attributeLength   = 0;

	result->appendBytes(&queueElement, sizeof(queueElement));
	result->appendBytes(_data, _data->size);

	if (!_children) {
		return result;
	}

	for (unsigned int i = 0; i < _children->getCount(); i++) {
		IOHIDEvent *child = (IOHIDEvent *)_children->getObject(i);
		if (!child) {
			continue;
		}

		queueElement.timeStamp         = child->_timeStamp;
		queueElement.options           = child->_options;
		queueElement.senderID          = child->_senderID;
		queueElement.attributeLength   = 0;

		result->appendBytes(&queueElement, sizeof(queueElement));
		result->appendBytes(child->_data, child->_data->size);
	}

	return result;
}

//==============================================================================
// IOHIDEvent::getPhase
//==============================================================================
IOHIDEventPhaseBits
IOHIDEvent::getPhase()
{
	return (_data->options >> kIOHIDEventEventOptionPhaseShift) & kIOHIDEventEventPhaseMask;
}

//==============================================================================
// IOHIDEvent::setPhase
//==============================================================================
void
IOHIDEvent::setPhase(IOHIDEventPhaseBits phase)
{
	_data->options &= ~(kIOHIDEventEventPhaseMask << kIOHIDEventEventOptionPhaseShift);
	_data->options |= ((phase & kIOHIDEventEventPhaseMask) << kIOHIDEventEventOptionPhaseShift);
}

//==============================================================================
// IOHIDEvent::setSenderID
//==============================================================================
void
IOHIDEvent::setSenderID(uint64_t senderID)
{
	_senderID = senderID;

	if (_children) {
		for (unsigned int i = 0; i < _children->getCount(); i++) {
			IOHIDEvent *child = (IOHIDEvent *)_children->getObject(i);
			if (!child) {
				continue;
			}
			child->setSenderID(senderID);
		}
	}
}

//==============================================================================
// IOHIDEvent::getLatency
//==============================================================================
uint64_t
IOHIDEvent::getLatency(uint32_t scaleFactor)
{
	uint64_t        now = mach_absolute_time();
	uint64_t        ts = getTimeStamp();
	uint64_t        ns;

	absolutetime_to_nanoseconds(now > ts ? now - ts : 0, &ns);

	return ns / (scaleFactor ? scaleFactor : 1);
}
