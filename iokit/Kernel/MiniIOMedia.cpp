/*
 * MiniIOMedia: the phone's storage as IOKit shows it on an iPhone (11-21).
 *
 * On an iPhone every volume's device is an IOMedia object of IOStorageFamily (disk0s1s1 ...), with its BSD name,
 * major and minor numbers, size and writability, under an IOBlockStorageDevice that says what the storage is (internal,
 * not removable). iOS 27's coreservicesd looks each mounted file system's device up that way
 * (IOBSDNameMatching -> FSNode_FillIOKitProperties) and aborted at every start here: this kernel's storage is
 * /dev nodes of its own drivers (drivers/storage/hakios.ufs.c: /dev/ufs_NAME; bsd/dev/memdev.c: /dev/mdN) with no
 * IOKit object behind them. So each of those devices gets one: an IOMedia (IOStorageFamily's own class since 11-22;
 * iokit/Families/IOStorage) under one AppleHakiosUFS / AppleHakiosRAMDisk device. Nothing reads or writes
 * through these objects -- they only describe the /dev nodes.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>

/*
 * (11-22, AkIOS) IOMedia, IOBlockStorageDevice and IOBlockStorageDriver are now IOStorageFamily's own classes
 * (iokit/Families/IOStorage); the stand-ins that were here are gone. IOBlockStorageDevice is abstract, so the device is
 * this small subclass that can do nothing: these objects still only describe /dev nodes; nobody does I/O through them.
 * Nothing is started: no IOStorageFamily personality is in gIOKernelConfigTables, so registerService() matches no
 * IOMediaBSDClient (no second /dev node), no partition scheme and no IOBlockStorageDriver onto them.
 */
class AppleHakiosBlockStorageDevice : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(AppleHakiosBlockStorageDevice);
public:
	virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
	virtual IOReturn doFormatMedia(UInt64) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
	virtual UInt32 doGetFormatCapacities(UInt64 *, UInt32) const APPLE_KEXT_OVERRIDE { return 0; }
	virtual char *getVendorString(void) APPLE_KEXT_OVERRIDE { return (char *)"Google"; }
	virtual char *getProductString(void) APPLE_KEXT_OVERRIDE { return (char *)"UFS"; }
	virtual char *getRevisionString(void) APPLE_KEXT_OVERRIDE { return (char *)""; }
	virtual char *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE { return (char *)""; }
	virtual IOReturn reportBlockSize(UInt64 *bs) APPLE_KEXT_OVERRIDE { *bs = 4096; return kIOReturnSuccess; }
	virtual IOReturn reportEjectability(bool *e) APPLE_KEXT_OVERRIDE { *e = false; return kIOReturnSuccess; }
	virtual IOReturn reportMaxValidBlock(UInt64 *m) APPLE_KEXT_OVERRIDE { *m = 0; return kIOReturnSuccess; }
	virtual IOReturn reportMediaState(bool *p, bool *c) APPLE_KEXT_OVERRIDE
	{
		*p = true;
		if (c != NULL) {
			*c = false;
		}
		return kIOReturnSuccess;
	}
	virtual IOReturn reportRemovability(bool *r) APPLE_KEXT_OVERRIDE { *r = false; return kIOReturnSuccess; }
	virtual IOReturn reportWriteProtection(bool *w) APPLE_KEXT_OVERRIDE { *w = false; return kIOReturnSuccess; }
	virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
	    IOStorageCompletion *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
};

OSDefineMetaClassAndStructors(AppleHakiosBlockStorageDevice, IOBlockStorageDevice);

static IOBlockStorageDriver *gDriver[2];

static IOBlockStorageDevice *gStorage[2];       /* 0: the phone's UFS, 1: the RAM disks */
static IOLock *gMediaLock;

static IOBlockStorageDevice *
storageDevice(unsigned which)
{
	IOBlockStorageDevice *dev;
	OSDictionary *proto, *chars;

	if (gStorage[which] != NULL) {
		return gStorage[which];
	}
	dev = OSTypeAlloc(AppleHakiosBlockStorageDevice);
	if (dev == NULL || !dev->init(NULL)) {
		OSSafeReleaseNULL(dev);
		return NULL;
	}
	dev->setName(which == 0 ? "AppleHakiosUFS" : "AppleHakiosRAMDisk");
	proto = OSDictionary::withCapacity(2);
	chars = OSDictionary::withCapacity(3);
	if (proto != NULL) {
		proto->setObject("Physical Interconnect", OSString::withCString(which == 0 ? "UFS" : "Virtual Interface"));
		proto->setObject("Physical Interconnect Location", OSString::withCString(which == 0 ? "Internal" : "Virtual"));
		dev->setProperty("Protocol Characteristics", proto);
		proto->release();
	}
	if (chars != NULL) {
		chars->setObject("Product Name", OSString::withCString(which == 0 ? "UFS" : "RAM Disk"));
		chars->setObject("Medium Type", OSString::withCString("Solid State"));
		dev->setProperty("Device Characteristics", chars);
		chars->release();
	}
	dev->setProperty("Removable", kOSBooleanFalse);
	dev->setProperty("Ejectable", kOSBooleanFalse);
	if (!dev->attach(IOService::getResourceService())) {
		dev->release();
		return NULL;
	}
	dev->registerService();
	gStorage[which] = dev;
	gDriver[which] = OSTypeAlloc(IOBlockStorageDriver);
	if (gDriver[which] != NULL && gDriver[which]->init(NULL) && gDriver[which]->attach(dev)) {
		gDriver[which]->setName("IOBlockStorageDriver");
		gDriver[which]->registerService();
	} else {
		OSSafeReleaseNULL(gDriver[which]);
	}
	return dev;
}

/*
 * One IOMedia for /dev/BSDNAME (block major/minor), SIZE bytes in blocks of BSIZE. CONTENT: "Apple_APFS",
 * "Apple_HFS" or "" (the partition's own kind, as IOStorageFamily's partition schemes say it). RAMDISK: under the
 * RAM disks' device, not the phone's storage.
 */
extern "C" void
hakios_iomedia_publish(const char *bsdname, int major, int minor, unsigned long long size, unsigned int bsize,
    int writable, const char *content, int ramdisk)
{
	IOBlockStorageDevice *parent;
	IOMedia *media;
	unsigned unit = 0;

	if (gMediaLock == NULL) {
		gMediaLock = IOLockAlloc();
	}
	IOLockLock(gMediaLock);
	parent = storageDevice(ramdisk ? 1 : 0);
	IOLockUnlock(gMediaLock);
	if (parent == NULL) {
		IOLog("MiniIOMedia: no storage device to put %s under\n", bsdname);
		return;
	}
	media = OSTypeAlloc(IOMedia);
	/* the real IOMedia (11-22): it sets Size, Preferred Block Size, Writable, Whole, Removable, Ejectable, Leaf, Content, Content Hint */
	if (media == NULL || !media->init(0, size, bsize, 0, false, writable != 0, content != NULL ? content : "")) {
		OSSafeReleaseNULL(media);
		return;
	}
	for (const char *c = bsdname; *c; c++) {
		if (*c >= '0' && *c <= '9') {
			unit = unit * 10 + (unsigned)(*c - '0');
		}
	}
	media->setName(bsdname);
	media->setProperty("BSD Name", bsdname);
	media->setProperty("BSD Major", (unsigned long long)major, 32);
	media->setProperty("BSD Minor", (unsigned long long)minor, 32);
	media->setProperty("BSD Unit", (unsigned long long)unit, 32);
	media->setProperty("Size", size, 64);
	media->setProperty("Preferred Block Size", (unsigned long long)bsize, 64);
	media->setProperty("Writable", writable ? kOSBooleanTrue : kOSBooleanFalse);
	media->setProperty("Removable", kOSBooleanFalse);
	media->setProperty("Ejectable", kOSBooleanFalse);
	media->setProperty("Whole", kOSBooleanFalse);
	media->setProperty("Leaf", kOSBooleanTrue);
	media->setProperty("Open", kOSBooleanTrue);
	media->setProperty("Content", content != NULL ? content : "");
	media->setProperty("Content Hint", content != NULL ? content : "");
	if (!media->attach(gDriver[ramdisk ? 1 : 0] != NULL ? (IOService *)gDriver[ramdisk ? 1 : 0] : (IOService *)parent)) {
		media->release();
		return;
	}
	media->registerService();
	media->release();
}

/*
 * The volume's UUID on its media (11-22), as an iPhone's IOMedia has it for each APFS volume: set when the volume is
 * mounted (bsd/miscfs/apfs, bsd/hfs). UUID: 16 bytes.
 */
extern "C" void
hakios_iomedia_set_uuid(const char *bsdname, const unsigned char *uuid)
{
	OSDictionary *match = IOService::serviceMatching("IOMedia");
	OSIterator *it;
	char text[40];
	IORegistryEntry *e;

	if (match == NULL) {
		return;
	}
	snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
	    uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
	    uuid[12], uuid[13], uuid[14], uuid[15]);
	it = IOService::getMatchingServices(match);
	match->release();
	while (it != NULL && (e = (IORegistryEntry *)it->getNextObject()) != NULL) {
		OSString *n = OSDynamicCast(OSString, e->getProperty("BSD Name"));

		if (n != NULL && n->isEqualTo(bsdname)) {
			e->setProperty("UUID", text);
		}
	}
	OSSafeReleaseNULL(it);
}
