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
 * AkIOS: AppleEmbeddedHIDEventService.cpp from IOHIDFamily-2238.100.59 (the header says what is left out).
 */

#include "IOHIDEvent.h"
#include "AppleEmbeddedHIDEventService.h"


//===========================================================================
// AppleEmbeddedHIDEventService class
#define super IOHIDEventService

OSDefineMetaClassAndAbstractStructors( AppleEmbeddedHIDEventService, IOHIDEventService )

//====================================================================================================
// AppleEmbeddedHIDEventService::handleStart
//====================================================================================================
bool
AppleEmbeddedHIDEventService::handleStart(IOService * provider)
{
	uint32_t value;

	if (!super::handleStart(provider)) {
		return false;
	}

	value = getOrientation();
	if (value) {
		setProperty(kIOHIDOrientationKey, value, 32);
	}

	value = getPlacement();
	if (value) {
		setProperty(kIOHIDPlacementKey, value, 32);
	}

	// RY: all embedded services are built-in
	setProperty(kIOHIDBuiltInKey, true);

	return true;
}

//====================================================================================================
// AppleEmbeddedHIDEventService::getOrientation
//====================================================================================================
IOHIDOrientationType
AppleEmbeddedHIDEventService::getOrientation()
{
	return 0;
}

//====================================================================================================
// AppleEmbeddedHIDEventService::getPlacement
//====================================================================================================
IOHIDPlacementType
AppleEmbeddedHIDEventService::getPlacement()
{
	return 0;
}

//====================================================================================================
// AppleEmbeddedHIDEventService::getReportInterval
//====================================================================================================
UInt32
AppleEmbeddedHIDEventService::getReportInterval()
{
	return 0;
}
