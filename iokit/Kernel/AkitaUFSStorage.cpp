/*
 * AkitaUFSStorage (AkIOS, 11-22): the phone's APFS containers as IOKit block storage devices, so that Apple's APFS
 * kext (prelinked into this kernel by tools/mkprelink.py) attaches to them the way it does on an iPhone:
 *
 *   AppleAkitaUFSVolume (IOBlockStorageDevice, one per container: a byte range of a UFS partition)
 *     -> IOBlockStorageDriver (IOStorageFamily, matched by device-type = Generic: iokit/KernelConfigTables.cpp)
 *       -> IOMedia, Whole -> AppleAPFSContainerScheme (Apple's) -> its container and volume media
 *
 * Reads go through the UFS driver (bsd/dev/hakios.ufs.c: akios_ufs_read_at, the same path as its /dev nodes), a
 * bufferful at a time, synchronously in the caller's thread. Read only: the device says it is write protected.
 */
#include <IOKit/IOService.h>
#include <kern/thread_call.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorage.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>

extern "C" int akios_ufs_read_at(uint32_t part, uint64_t offset, void *buf, uint64_t len);

#define AKIOS_BOUNCE (1024 * 1024)

class AppleAkitaUFSVolume : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(AppleAkitaUFSVolume);
	uint32_t fPart;
	uint64_t fBase, fSize;
	uint32_t fBlock;
	char fName[32];
	IOLock *fLock;
	uint8_t *fBounce;
public:
	bool initVolume(const char *name, uint32_t part, uint64_t base, uint64_t size, uint32_t bsize);
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
	virtual IOReturn doFormatMedia(UInt64) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
	virtual UInt32 doGetFormatCapacities(UInt64 *, UInt32) const APPLE_KEXT_OVERRIDE { return 0; }
	virtual char *getVendorString(void) APPLE_KEXT_OVERRIDE { return (char *)"Google"; }
	virtual char *getProductString(void) APPLE_KEXT_OVERRIDE { return fName; }
	virtual char *getRevisionString(void) APPLE_KEXT_OVERRIDE { return (char *)"1.0"; }
	virtual char *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE { return (char *)"UFS"; }
	virtual IOReturn reportBlockSize(UInt64 *bs) APPLE_KEXT_OVERRIDE { *bs = fBlock; return kIOReturnSuccess; }
	virtual IOReturn reportEjectability(bool *e) APPLE_KEXT_OVERRIDE { *e = false; return kIOReturnSuccess; }
	virtual IOReturn reportMaxValidBlock(UInt64 *m) APPLE_KEXT_OVERRIDE { *m = fSize / fBlock - 1; return kIOReturnSuccess; }
	virtual IOReturn reportMediaState(bool *p, bool *c) APPLE_KEXT_OVERRIDE
	{
		*p = true;
		if (c != NULL) {
			*c = false;
		}
		return kIOReturnSuccess;
	}
	virtual IOReturn reportRemovability(bool *r) APPLE_KEXT_OVERRIDE { *r = false; return kIOReturnSuccess; }
	virtual IOReturn reportWriteProtection(bool *w) APPLE_KEXT_OVERRIDE { *w = true; return kIOReturnSuccess; }
	virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleAkitaUFSVolume, IOBlockStorageDevice);

bool
AppleAkitaUFSVolume::initVolume(const char *name, uint32_t part, uint64_t base, uint64_t size, uint32_t bsize)
{
	OSDictionary *proto, *chars;

	if (!IOBlockStorageDevice::init(NULL)) {
		return false;
	}
	fPart = part;
	fBase = base;
	fSize = size;
	fBlock = bsize;
	strlcpy(fName, name, sizeof(fName));
	fLock = IOLockAlloc();
	fBounce = (uint8_t *)IOMallocData(AKIOS_BOUNCE);
	if (fLock == NULL || fBounce == NULL) {
		return false;
	}
	setName(name);
	setProperty("device-type", "Generic");
	proto = OSDictionary::withCapacity(2);
	if (proto != NULL) {
		proto->setObject("Physical Interconnect", OSString::withCString("UFS"));
		proto->setObject("Physical Interconnect Location", OSString::withCString("Internal"));
		setProperty("Protocol Characteristics", proto);
		proto->release();
	}
	chars = OSDictionary::withCapacity(3);
	if (chars != NULL) {
		chars->setObject("Product Name", OSString::withCString(name));
		chars->setObject("Vendor Name", OSString::withCString("Google"));
		chars->setObject("Medium Type", OSString::withCString("Solid State"));
		setProperty("Device Characteristics", chars);
		chars->release();
	}
	return true;
}

void
AppleAkitaUFSVolume::free(void)
{
	if (fBounce != NULL) {
		IOFreeData(fBounce, AKIOS_BOUNCE);
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
	}
	IOBlockStorageDevice::free();
}

IOReturn
AppleAkitaUFSVolume::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
    IOStorageAttributes *attributes __unused, IOStorageCompletion *completion)
{
	UInt64 offset = block * fBlock, len = nblks * fBlock, done = 0;
	IOReturn status = kIOReturnSuccess;

	if (buffer->getDirection() == kIODirectionOut) {
		IOStorage::complete(completion, kIOReturnNotWritable, 0);
		return kIOReturnSuccess;
	}
	if (offset > fSize || len > fSize - offset || len > buffer->getLength()) {
		IOStorage::complete(completion, kIOReturnBadArgument, 0);
		return kIOReturnSuccess;
	}
	IOLockLock(fLock);
	while (done < len) {
		UInt64 n = len - done;

		if (n > AKIOS_BOUNCE) {
			n = AKIOS_BOUNCE;
		}
		if (akios_ufs_read_at(fPart, fBase + offset + done, fBounce, n) != 0) {
			status = kIOReturnIOError;
			break;
		}
		if (buffer->writeBytes(done, fBounce, n) != n) {
			status = kIOReturnDMAError;
			break;
		}
		done += n;
	}
	IOLockUnlock(fLock);
	IOStorage::complete(completion, status, done);
	return kIOReturnSuccess;
}

static bool
akios_has_bsd(const char *n)
{
	for (; *n; n++) {
		if (n[0] == 'B' && n[1] == 'S' && n[2] == 'D') {
			return true;
		}
	}
	return false;
}

/* (diagnostic) the service plane below each device, 15 s after it was published: what attached to it */
static void
akios_tree(IORegistryEntry *e, int depth)
{
	OSIterator *it;
	IORegistryEntry *c;
	OSString *bsd = OSDynamicCast(OSString, e->getProperty("BSD Name"));
	OSString *hint = OSDynamicCast(OSString, e->getProperty("Content Hint"));
	OSString *content = OSDynamicCast(OSString, e->getProperty("Content"));

	IOLog("akios: tree: %*s%s <%s>%s%s%s%s%s%s\n", depth * 2, "", e->getName(), e->getMetaClass()->getClassName(),
	    bsd ? " bsd=" : "", bsd ? bsd->getCStringNoCopy() : "", hint ? " hint=" : "", hint ? hint->getCStringNoCopy() : "",
	    content ? " content=" : "", content ? content->getCStringNoCopy() : "");
	if (strncmp(e->getMetaClass()->getClassName(), "AppleAPFS", 9) == 0 && !akios_has_bsd(e->getMetaClass()->getClassName())) {
		OSDictionary *props = e->dictionaryWithProperties();
		OSCollectionIterator *ki = props ? OSCollectionIterator::withCollection(props) : NULL;
		OSSymbol *k;

		while (ki != NULL && (k = OSDynamicCast(OSSymbol, ki->getNextObject())) != NULL) {
			OSObject *v = props->getObject(k);
			OSString *vs = OSDynamicCast(OSString, v);
			OSNumber *vn = OSDynamicCast(OSNumber, v);
			OSBoolean *vb = OSDynamicCast(OSBoolean, v);

			IOLog("akios: tree: %*s  . %s = %s%s%s\n", depth * 2, "", k->getCStringNoCopy(),
			    vs ? vs->getCStringNoCopy() : "", vn ? (vn->unsigned64BitValue() ? "(number, nonzero)" : "0") : "",
			    vb ? (vb->isTrue() ? "true" : "false") : (vs || vn ? "" : v->getMetaClass()->getClassName()));
		}
		OSSafeReleaseNULL(ki);
		OSSafeReleaseNULL(props);
	}
	if (depth > 8) {
		return;
	}
	it = e->getChildIterator(gIOServicePlane);
	while (it != NULL && (c = OSDynamicCast(IORegistryEntry, it->getNextObject())) != NULL) {
		akios_tree(c, depth + 1);
	}
	OSSafeReleaseNULL(it);
}

static void
akios_tree_later(thread_call_param_t p0, thread_call_param_t)
{
	IOService *dev = (IOService *)p0;

	static int round[8];
	int i = 0;

	akios_tree(dev, 0);
	dev->release();
	(void)round;
	(void)i;
}

extern "C" void
akios_storage_publish(const char *name, uint32_t part, uint64_t base, uint64_t size, uint32_t bsize)
{
	AppleAkitaUFSVolume *dev = OSTypeAlloc(AppleAkitaUFSVolume);

	if (dev == NULL || !dev->initVolume(name, part, base, size, bsize)) {
		OSSafeReleaseNULL(dev);
		IOLog("akios: %s: no storage device\n", name);
		return;
	}
	if (!dev->attach(IOService::getResourceService())) {
		dev->release();
		return;
	}
	dev->registerService();
	{
		thread_call_t tc = thread_call_allocate(akios_tree_later, dev);
		if (tc != NULL) {
			uint64_t deadline;

			dev->retain();
			clock_interval_to_deadline(15, kSecondScale, &deadline);
			thread_call_enter_delayed(tc, deadline);
			thread_call_t tc2 = thread_call_allocate(akios_tree_later, dev);
			if (tc2 != NULL) {
				dev->retain();
				clock_interval_to_deadline(60, kSecondScale, &deadline);
				thread_call_enter_delayed(tc2, deadline);
			}
		}
	}
	IOLog("akios: %s: IOKit storage device, %llu MB at %llu MB of UFS partition %u\n", name, size >> 20, base >> 20, part);
	dev->release();
}
