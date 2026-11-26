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
 * AkIOS: AppleEmbeddedHIDEventService from IOHIDFamily-2238.100.59, the base of Apple's built-in event services on a
 * phone (AppleM68Buttons : AppleEmbeddedHIDEventService : IOHIDEventService on iPhone14,4, see
 * ref/drivers/touch-buttons-spec.md 2.1). What it adds: every embedded service is Built-In, says its Orientation and
 * Placement when it has one, and reports no report interval. Left out for now: its sensor event dispatchers
 * (accelerometer, gyro, compass, proximity, ambient light, temperature, power, vendor defined, biometric,
 * atmospheric pressure), whose IOHIDEvent constructors and data layouts are not ported yet; they come back with them.
 * Userland never names this class (iOS 27.2.1: no "AppleEmbeddedHIDEventService" string in the dyld cache or the
 * rootfs); the name is kept so that the registry reads as on an iPhone.
 */

#ifndef _IOKIT_HID_APPLEEMBEDDEDHIDEVENTSERVICE_H
#define _IOKIT_HID_APPLEEMBEDDEDHIDEVENTSERVICE_H

#include "IOHIDEventService.h"

typedef uint32_t IOHIDOrientationType;
typedef uint32_t IOHIDPlacementType;

class AppleEmbeddedHIDEventService : public IOHIDEventService
{
	OSDeclareAbstractStructors( AppleEmbeddedHIDEventService )

public:
	virtual bool handleStart(IOService * provider) APPLE_KEXT_OVERRIDE;

protected:
	virtual IOHIDOrientationType getOrientation();

	virtual IOHIDPlacementType getPlacement();

	virtual UInt32          getReportInterval() APPLE_KEXT_OVERRIDE;
};

#endif /* _IOKIT_HID_APPLEEMBEDDEDHIDEVENTSERVICE_H */
