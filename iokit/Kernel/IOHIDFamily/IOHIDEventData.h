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
 * AkIOS: the bytes of an IOHIDEvent as the kernel writes them into an event service's queue.
 *
 * Apple's IOHIDEventData.h takes the per-type structures and the size/field macros from two generated headers
 * (IOHIDEventStructDefs.h, IOHIDEventMacroDefs.h) that are not in the open source. So the structures here are written
 * out by hand, for the types the kernel sends so far, and each layout is checked against iOS 27.2.1 (24B156)
 * IOKit.framework, the reader on the other side of the queue:
 *
 *  - __IOHIDEventCreateWithBytesHelper (0x18f0fb1f4, called from IOHIDEventServicePlugin's dequeue through
 *    IOHIDEventCreateWithBytesInternal): every event, the parent and each child, is one IOHIDSystemQueueElement
 *    header (0x1c bytes: timeStamp, senderID, options, attributeLength, eventCount) followed by attributeLength bytes
 *    of attributes and then the event data. The first header's eventCount is the number of events in the element; a
 *    child is put under the parent by the depth byte at +0xc of its data. The data must be at least 0x10 bytes, its
 *    type at most 0x2c, and at least the size of its type (table at 0x18f0fb674: keyboard = 0x20). Keyboard is not one
 *    of the "legacy" types (4, 0xb, 0x11, 0x25) that the reader converts.
 *  - IOHIDEventCreateKeyboardEvent (0x18f15458c) and __IOHIDEventTypeDescriptorKeyboard (0x18f15871c): keyboard data
 *    is 0x20 bytes, usagePage u16 at 0x10, usage u16 at 0x12, down u32 at 0x14, flags u32 at 0x18 (bit 0 long press,
 *    bits 1-2 click speed, bits 6-8 sticky key phase, bits 9-10 sticky key toggle), pressCount u8 at 0x1c.
 *  - IOHIDEventGetIntegerValueWithOptions (0x18f0fd1b0): kIOHIDEventFieldIsBuiltIn is bit 4 of the data's options
 *    (kIOHIDEventOptionIsBuiltIn).
 *
 * Apple's kernel IOHIDEvent::readBytes (IOHIDEvent.cpp) writes exactly this: a header per event and the data after
 * it, which is what our port does.
 */

#ifndef _IOKIT_IOHIDEVENTDATA_H
#define _IOKIT_IOHIDEVENTDATA_H

#include <IOKit/IOTypes.h>
#include "IOHIDEventTypes.h"

#define IOHIDEVENT_BASE             \
    uint32_t            size;       \
    IOHIDEventType      type;       \
    uint32_t            options;    \
    uint8_t             depth;      \
    uint8_t             reserved[3];

struct IOHIDEventData {
	IOHIDEVENT_BASE;
};

typedef struct _IOHIDKeyboardEventData {
	IOHIDEVENT_BASE;                        /* 0x00 */
	uint16_t        usagePage;              /* 0x10 */
	uint16_t        usage;                  /* 0x12 */
	uint32_t        down;                   /* 0x14 */
	uint32_t        longPress:1;            /* 0x18 */
	uint32_t        clickSpeed:2;
	uint32_t        flags3to5:3;            /* (mouse/slow key state; not read by the descriptor) */
	uint32_t        stickyKeyPhase:3;
	uint32_t        stickyKeyToggle:2;
	uint32_t        flagsReserved:21;
	uint8_t         pressCount;             /* 0x1c */
	uint8_t         keyboardReserved[3];
} IOHIDKeyboardEventData;

_Static_assert(sizeof(struct IOHIDEventData) == 0x10, "IOHIDEventData: iOS 27 reads 0x10 bytes of base");
_Static_assert(sizeof(IOHIDKeyboardEventData) == 0x20, "keyboard event data: iOS 27 wants 0x20 bytes");
_Static_assert(__builtin_offsetof(IOHIDKeyboardEventData, usagePage) == 0x10, "keyboard usagePage at 0x10");
_Static_assert(__builtin_offsetof(IOHIDKeyboardEventData, down) == 0x14, "keyboard down at 0x14");
_Static_assert(__builtin_offsetof(IOHIDKeyboardEventData, pressCount) == 0x1c, "keyboard pressCount at 0x1c");

enum {
	kIOHIDEventOptionIgnore         = 0xf0000000,
	kIOHIDEventOptionIsRepeat       = 0x00010000,
	kIOHIDEventOptionIsZeroEvent    = 0x00800000,
};

/*
 * @typedef    IOHIDSystemQueueElement
 * @abstract   The header in front of each event in an event service's queue (see above).
 */
typedef struct  __attribute__((packed, aligned(4))) _IOHIDSystemQueueElement {
	uint64_t        timeStamp;
	uint64_t        senderID;
	uint32_t        options;
	uint32_t        attributeLength;
	uint32_t        eventCount;
	uint8_t         payload[0];
} IOHIDSystemQueueElement;

_Static_assert(sizeof(IOHIDSystemQueueElement) == 0x1c, "queue element header: iOS 27 wants 0x1c bytes");

#define IOHIDEventFieldEventType(field) (((field) >> 16) & 0xffff)
#define IOHIDEventFieldOffset(field)    ((field) & 0xffff)

/*
 * The size of an event's data by type; 0 for a type the kernel cannot make yet (IOHIDEvent::initWithType then fails,
 * and IOHIDEvent::withBytes refuses such an event from userland).
 */
static inline size_t
IOHIDEventGetDataSize(IOHIDEventType type)
{
	switch (type) {
	case kIOHIDEventTypeKeyboard:
		return sizeof(IOHIDKeyboardEventData);
	default:
		return 0;
	}
}

#endif /* _IOKIT_IOHIDEVENTDATA_H */
