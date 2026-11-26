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
 * AkIOS: the registry keys and open options of IOHIDFamily (IOHIDFamily-2238.100.59: IOHIDDeviceKeys.h, IOHIDKeys.h,
 * IOHIDEventServiceKeys.h, IOHIDPrivateKeys.h, AppleEmbeddedHIDKeys.h, hidsystem/IOHIDShared.h) that the kernel's
 * event service classes use. Only those: the strings are what iOS 27's IOKit.framework and backboardd look up, so
 * each is spelled exactly as in Apple's headers.
 */

#ifndef _IOKIT_HID_IOHIDKEYS_H_
#define _IOKIT_HID_IOHIDKEYS_H_

/* IOHIDDeviceKeys.h */
#define kIOHIDTransportKey                  "Transport"
#define kIOHIDVendorIDKey                   "VendorID"
#define kIOHIDProductIDKey                  "ProductID"
#define kIOHIDVersionNumberKey              "VersionNumber"
#define kIOHIDManufacturerKey               "Manufacturer"
#define kIOHIDProductKey                    "Product"
#define kIOHIDSerialNumberKey               "SerialNumber"
#define kIOHIDCountryCodeKey                "CountryCode"
#define kIOHIDLocationIDKey                 "LocationID"
#define kIOHIDDeviceUsagePairsKey           "DeviceUsagePairs"
#define kIOHIDDeviceUsageKey                "DeviceUsage"
#define kIOHIDDeviceUsagePageKey            "DeviceUsagePage"
#define kIOHIDPrimaryUsageKey               "PrimaryUsage"
#define kIOHIDPrimaryUsagePageKey           "PrimaryUsagePage"
#define kIOHIDReportIntervalKey             "ReportInterval"
#define kIOHIDBuiltInKey                    "Built-In"
#define kIOHIDPhysicalDeviceUniqueIDKey     "PhysicalDeviceUniqueID"

/* IOHIDKeys.h */
#define kIOHIDVendorIDSourceKey             "VendorIDSource"
#define kIOHIDEventServiceQueueSize         "QueueSize"
#define kIOHIDDeviceOpenedByEventSystemKey  "DeviceOpenedByEventSystem"

/* IOHIDTransport values (IOHIDKeys.h) */
#define kIOHIDTransportSPIValue             "SPI"

/* IOHIDPrivateKeys.h / IOHIDEventServiceKeys.h */
#define kIOHIDEventServicePropertiesKey     "HIDEventServiceProperties"
#define kIOHIDDeviceParametersKey           "HIDDeviceParameters"
#define kIOHIDResetKeyboardKey              "HIDResetKeyboard"
#define kIOHIDResetPointerKey               "HIDResetPointer"
#define kIOHIDDebugConfigKey                "HIDDebug"

/*
 * What IOHIDEventSystem (in backboardd) matches its services by: IOServiceMatching("IOService") plus
 * IOPropertyMatch {HIDServiceSupport = true} (_IOHIDEventSystemCreate). Apple's kernel puts it on its event services
 * with an IOHIDProviderPropertyMerger personality; ours set it themselves.
 */
#define kIOHIDServiceSupportKey             "HIDServiceSupport"

/* AppleEmbeddedHIDKeys.h */
#define kIOHIDOrientationKey                "Orientation"
#define kIOHIDPlacementKey                  "Placement"

/* hidsystem/IOHIDShared.h: options to IOService::open on an event service */
enum {
	kIOHIDOpenedByEventSystem       = 0x10000,
	kIOHIDOpenedByFastPathClient    = 0x20000
};

#endif /* _IOKIT_HID_IOHIDKEYS_H_ */
