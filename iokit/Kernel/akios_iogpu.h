/*
 * AkIOS: the GPU's service as iOS's own IOGPUFamily shapes it.
 *
 * Metal finds a GPU by the class IOAcceleratorES and loads the bundle its MetalPluginName names. Apple's bundles are
 * built on IOGPU.framework, whose IOGPUDeviceCreate opens the service with IOServiceOpen type 1 and speaks the
 * external methods of IOGPUDeviceUserClient (56 selectors and 14 traps on iOS 27.2.1, 24B156). This file gives our
 * kernel those classes under Apple's names, so IOGPU.framework works against our GPU unchanged:
 *
 *   IOAcceleratorES : IOService     what Metal and CoreAnimation look for
 *   IOGPU : IOAcceleratorES         one GPU: its stamp page, its configuration, its user clients
 *   IOGPUDeviceUserClient           IOServiceOpen type 1 (type >> 16: the caller's options)
 *
 * A vendor's driver is a subclass of IOGPU (as Apple's AGXAccelerator is) that fills in the virtuals below.
 * The selector numbers and structure sizes are iOS 27.2.1's: they differ from macOS and from release to release
 * (tools/iogpu in the umbrella repo dumps them from a kernelcache).
 */
#ifndef _IOKIT_AKIOS_IOGPU_H
#define _IOKIT_AKIOS_IOGPU_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
public:
	/* boot-arg metal=1: name the Metal plug-in (metal_plugin=NAME overrides the name) */
	void publishMetalPlugin(const char *name, const char *className);
};

/* IOGPUConfig: selector 0's answer, 0x40 bytes (IOGPUDeviceCreateWithOptions copies it to its device at +0xa0) */
struct IOGPUConfig {
	uint32_t configBits;            /* +0x00 */
	uint32_t deviceBits;            /* +0x04 */
	uint64_t textureRam;            /* +0x08 */
	uint64_t videoRam;              /* +0x10 */
	uint32_t accelID;               /* +0x18: the IOSurface accelerator id */
	uint32_t pad1c;
	uint64_t sharedMemorySize;      /* +0x20: the most system memory the GPU may have wired */
	uint64_t maxResourceSize;       /* +0x28: Metal's maxBufferLength */
	uint64_t word30;                /* +0x30..0x3f: not read by IOGPU.framework */
	uint32_t word38, word3c;
};

/*
 * Event channels: one u32 stamp each in the stamp page, advanced by 0x100 per completed submission (the low byte is
 * IOGPU.framework's: an event slot is (stamp & ~0xff) | channel). Apple allows up to 128.
 */
#define kIOGPUEventChannels     16u

class IOGPU : public IOAcceleratorES
{
	OSDeclareDefaultStructors(IOGPU);

protected:
	IOBufferMemoryDescriptor *fStamps;      /* u32 stamps that the GPU's completions advance; mapped read-only into clients */
	void retireStandIns(void);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	/* the vendor's part */
	virtual void populateAccelConfig(IOGPUConfig *config);
	virtual const char *gpuName(void);
	/* IOServiceOpen types that are not IOGPU's own (the vendor's private client); IOService's default by default */
	virtual IOReturn newVendorUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler);

	IOBufferMemoryDescriptor *stampMemory(void) { return fStamps; }
	uint32_t stampOffset(unsigned channel) { return channel * sizeof(uint32_t); }
	static uint64_t nextObjectIDs(uint64_t count);  /* IOGPU's global object ids (trace ids): the first of count */
};

#endif /* _IOKIT_AKIOS_IOGPU_H */
