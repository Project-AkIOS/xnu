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
 *  - Digitizer (type 11): the reader's minimum is 0xa0. Digitizer is one of the reader's "legacy" types, but the
 *    conversion (__IOHIDEventPopulateCurrentEventData) only runs for callers that ask for it; the queue's reader,
 *    IOHIDEventCreateWithBytesInternal, passes 0 and copies the data as it is -- so the kernel writes the current
 *    layout, the one IOKit.framework itself makes. From IOHIDEventCreateDigitizerEvent (0x18f12ab68),
 *    IOHIDEventCreateDigitizerFingerEventWithQuality (0x18f12aa28), IOHIDEventGetDoubleValueWithOptions' digitizer
 *    cases (jump table 0x18f102d6c), __IOHIDEventTypeDescriptorDigitizer (0x18f192244) and IOHIDEventAppendEvent
 *    (0x18f0fb82c): see IOHIDDigitizerEventData below. Range, Touch and DisplayIntegrated are bits 16, 17 and 19 of
 *    the data's options; Collection is bit 1 (kIOHIDEventOptionIsCollection). When a digitizer child is appended to a
 *    digitizer parent, IOKit ORs the child's eventMask into the parent's childEventMask (and it does so again when it
 *    rebuilds an event from the queue's bytes).
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

/*
 * Digitizer event data, 0xa0 bytes, as iOS 27's IOKit.framework lays it out (offsets verified, see above; the field
 * numbers are those of kIOHIDEventFieldDigitizer*).
 */
typedef struct _IOHIDDigitizerEventData {
	IOHIDEVENT_BASE;                        /* 0x00; options: Range bit 16, Touch bit 17, DisplayIntegrated bit 19 */
	struct {
		double  x;                      /* 0x10  field 0, 0..1 on a display-integrated digitizer */
		double  y;                      /* 0x18  field 1 */
		double  z;                      /* 0x20  field 2 */
	} position;
	uint32_t        transducerIndex;        /* 0x28  field 5 */
	uint32_t        transducerType;         /* 0x2c  field 4: kIOHIDDigitizerTransducerType* */
	uint32_t        identity;               /* 0x30  field 6 */
	uint32_t        eventMask;              /* 0x34  field 7: kIOHIDDigitizerEvent* */
	uint32_t        childEventMask;         /* 0x38  field 24 */
	uint32_t        buttonMask;             /* 0x3c  field 3 */
	double          pressure;               /* 0x40  field 10 */
	double          auxPressure;            /* 0x48  field 11 */
	double          twist;                  /* 0x50  field 12 */
	uint32_t        orientationType;        /* 0x58  field 31: kIOHIDDigitizerOrientationType* */
	uint32_t        orientationReserved;    /* 0x5c */
	union {
		struct {
			double  x;              /* 0x60  field 13 */
			double  y;              /* 0x68  field 14 */
		} tilt;
		struct {
			double  quality;        /* 0x60  field 17 */
			double  density;        /* 0x68  field 18 */
			double  irregularity;   /* 0x70  field 19 */
			double  majorRadius;    /* 0x78  field 20 */
			double  minorRadius;    /* 0x80  field 21 */
			double  accuracy;       /* 0x88  field 26 */
		} quality;
	} orientation;
	uint32_t        generationCount;        /* 0x90  field 27 */
	uint32_t        willUpdateMask;         /* 0x94  field 28 */
	uint32_t        didUpdateMask;          /* 0x98  field 29 */
	uint32_t        digitizerReserved;      /* 0x9c */
} IOHIDDigitizerEventData;

/* the transducer bits of a digitizer event's options (IOHIDFamily's IOHIDEventData.h) */
enum {
	kIOHIDTransducerRange               = 0x00010000,
	kIOHIDTransducerTouch               = 0x00020000,
	kIOHIDTransducerInvert              = 0x00040000,
	kIOHIDTransducerDisplayIntegrated   = 0x00080000
};

_Static_assert(sizeof(IOHIDDigitizerEventData) == 0xa0, "digitizer event data: iOS 27 wants 0xa0 bytes");
_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, transducerIndex) == 0x28, "digitizer index at 0x28");
_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, eventMask) == 0x34, "digitizer eventMask at 0x34");
_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, pressure) == 0x40, "digitizer pressure at 0x40");
_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, orientationType) == 0x58, "digitizer orientation at 0x58");
_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, orientation.quality.minorRadius) == 0x80, "minorRadius");
_Static_assert(__builtin_offsetof(IOHIDDigitizerEventData, generationCount) == 0x90, "generationCount at 0x90");

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
	case kIOHIDEventTypeDigitizer:
		return sizeof(IOHIDDigitizerEventData);
	default:
		return 0;
	}
}

#endif /* _IOKIT_IOHIDEVENTDATA_H */
