/*
 * AkIOS: the kernel build has no SDK <TargetConditionals.h>; IOStorageFamily's headers include it (and test TARGET_OS_*
 * without it). This kernel is the iOS (embedded) family: every TARGET_OS_OSX block must take the iOS side, because some
 * of them change class layouts (IOStorage, IOBlockStorageDriver, IOBlockStorageDevice) that Apple's iOS APFS kext uses.
 * On the -I path (and -include'd) only for iokit/Families/IOStorage and its in-kernel users (iokit/conf/Makefile.template).
 */
#ifndef AKIOS_IOSTORAGE_TARGETCONDITIONALS_H
#define AKIOS_IOSTORAGE_TARGETCONDITIONALS_H
#define TARGET_OS_OSX           0
#define TARGET_OS_MAC           1
#define TARGET_OS_IPHONE        1
#define TARGET_OS_IOS           1
#define TARGET_OS_EMBEDDED      1
#define TARGET_OS_DRIVERKIT     0
#define TARGET_OS_SIMULATOR     0
#define TARGET_OS_WATCH         0
#define TARGET_OS_TV            0
#define TARGET_OS_BRIDGE        0
#define TARGET_OS_VISION        0
#endif
