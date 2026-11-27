/*
 * Copyright (c) 1998-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#if defined(__arm64__)
#include <pexpert/arm64/board_config.h>
#endif

const char * gIOKernelConfigTables =
    "("
    "   {"
    "     'IOClass'         = IOPanicPlatform;"
    "     'IOProviderClass' = IOPlatformExpertDevice;"
    "     'IOProbeScore'    = 0:32;"
    "   },"
    "   {"
    "     'IOClass'         = IOExclaveProxy;"
    "     'IOProviderClass' = IOService;"
    "     'IOExclaveProxy'  = 1:32;"
    "   }"
#if defined(__arm64__) && !defined(APPLE_ARM64_ARCH_FAMILY)
    "   ,{"
    "     'IOClass'         = GenericARMPE;"
    "     'IOProviderClass' = IOPlatformExpertDevice;"
    "     'IOProbeScore'    = 1000:32;"
    "   }"
#endif
/*
 * AkIOS (11-22): IOBlockStorageDriver onto the devices of iokit/Kernel/AkitaUFSStorage.cpp (they say device-type
 * Generic; MiniIOMedia's descriptive device does not, so it stays driverless): it publishes each one's whole-disk
 * IOMedia, which Apple's APFS kext (prelinked) matches with its AppleAPFSContainerSchemeWholeDisk personality.
 */
    "   ,{"
    "     'IOClass'         = IOBlockStorageDriver;"
    "     'IOProviderClass' = IOBlockStorageDevice;"
    "     'IOPropertyMatch' = { 'device-type' = Generic; };"
    "   }"
/*
 * AkIOS (11-26): Bluetooth as a UART iPhone has it (iokit/Kernel/AppleBluetoothModule.cpp): AppleBluetoothModule on the
 * device tree's /arm-io/bluetooth (the nub GenericARMPE publishes; Apple's provider class is AppleARMIODevice, there is
 * none here), with Apple's user client; and AppleOnboardSerialBSDClient once BSD is up, which publishes the kernel control
 * com.apple.uart.sk.bluetooth (Apple's hangs on the UART's AppleOnboardSerialSync "bluetooth"; there is no such UART
 * driver here, so on the module).
 */
#if defined(HAKIOS_BT_HOST)
    "   ,{"
    "     'IOClass'           = AppleBluetoothModule;"
    "     'IOProviderClass'   = IOPlatformDevice;"
    "     'IONameMatch'       = bluetooth;"
    "     'IOUserClientClass' = AppleBluetoothModuleUserClient;"
    "   }"
    "   ,{"
    "     'IOClass'           = AppleOnboardSerialBSDClient;"
    "     'IOProviderClass'   = AppleBluetoothModule;"
    "     'IOResourceMatch'   = IOBSD;"
    "   }"
#endif
/*
 * AkIOS (11-27): the SMC as an iPhone has it (iokit/Kernel/AppleSMC.cpp): AppleSMC on the device tree's
 * /arm-io/smc/iop-smc-nub (the nub GenericARMPE publishes; Apple's provider is the RTBuddy nub of the SMC coprocessor),
 * with Apple's user client, which libSMC opens.
 */
    "   ,{"
    "     'IOClass'           = AppleSMC;"
    "     'IOProviderClass'   = IOPlatformDevice;"
    "     'IONameMatch'       = iop-smc-nub;"
    "     'IOUserClientClass' = AppleSMCClient;"
    "   }"
/*
 * AkIOS (11-22): the rest of IOStorageFamily's personalities are NOT registered yet,
 * so nothing matches IOMedia / IOBlockStorageDevice (MiniIOMedia's descriptive objects stay driverless). Draft from the
 * family's Info.plist (337.100.1, iOS side: no IOAppleLabelScheme). Before enabling IOMediaBSDClient, keep it off
 * MiniIOMedia's objects (they already have /dev nodes of their own), e.g. a property those objects carry that excludes them.
 *
 *  "   ,{"
 *  "     'IOClass'         = IOMediaBSDClient;"
 *  "     'IOProviderClass' = IOMedia;"
 *  "     'IOMatchCategory' = IOMediaBSDClient;"
 *  "     'IOResourceMatch' = IOBSD;"
 *  "     'IOProbeScore'    = 30000:32;"
 *  "   },{"
 *  "     'IOClass'         = IOBlockStorageDriver;"
 *  "     'IOProviderClass' = IOBlockStorageDevice;"
 *  "     'IOPropertyMatch' = { 'device-type' = Generic; };"
 *  "   },{"
 *  "     'IOClass'         = IOGUIDPartitionScheme;"
 *  "     'IOProviderClass' = IOMedia;"
 *  "     'IOMatchCategory' = IOStorage;"
 *  "     'IOProbeScore'    = 4000:32;"
 *  "     'IOPropertyMatch' = { 'Whole' = .true.; };"
 *  "     'Content Mask'    = GUID_partition_scheme;"
 *  "   }"
 *
 * (Info.plist also has IOFDiskPartitionScheme (score 3000, Whole, Content Mask FDisk_partition_scheme, a Content Table of
 *  MBR ids) and IOApplePartitionScheme (score 2000); their classes are not built in.)
 */
    ")";
