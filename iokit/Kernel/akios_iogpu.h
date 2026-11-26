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
#include <IOKit/IOSharedDataQueue.h>

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

#define kIOGPUMapReadOnly       1u
#define kIOGPUMapUncached       4u

class IOGPU : public IOAcceleratorES
{
	OSDeclareDefaultStructors(IOGPU);

protected:
	IOBufferMemoryDescriptor *fStamps;      /* u32 stamps that the GPU's completions advance; mapped read-only into clients */
	void retireStandIns(void);

	/*
	 * Command buffers between submission and completion, oldest first: waiting for shared events before they go to
	 * the GPU (held back in their queue's order), or on the GPU. completionThread moves them on.
	 */
public:
	enum { kMaxEventOps = 8 };
	struct eventOp { class IOSurfaceSharedEvent *event; uint64_t value; };       /* (event held) */
	struct pending {
		struct pending *next;
		OSObject *owner;                /* the user client (held) */
		uint32_t queueID;
		int hq;                         /* -1: given up (its queue went): completes with an error at once */
		bool onGPU;                     /* submitted to hq (seqno) */
		uint64_t seqno;
		uint64_t stream;                /* what goes to the GPU once the waits are met (length 0: nothing) */
		uint32_t length;
		uint64_t token;                 /* the "completed" block, echoed back */
		uint64_t startNs;
		IOSharedDataQueue *nq;          /* held */
		uint32_t waits, signals;
		struct eventOp wait[kMaxEventOps], signal[kMaxEventOps];
	};
protected:
	struct pending *fPending;
	IOLock *fSubmitLock;                    /* fPending, and every notification queue's enqueue and port */
	thread_t fCompletionThread;
	static void completionThread(void *self, wait_result_t);
	void completeReached(bool all);

public:
	/* (the notification entry IOGPU.framework dequeues: 0x28 bytes) */
	struct notification {
		uint64_t token;
		uint64_t startNs, endNs;
		uint32_t status, pad;
		uint64_t faultAddress;
	};
	static uint64_t nowNs(void);
	void lockSubmissions(void) { IOLockLock(fSubmitLock); }
	void unlockSubmissions(void) { IOLockUnlock(fSubmitLock); }
	/* (fSubmitLock held) */
	static void notify(IOSharedDataQueue *nq, uint64_t token, uint64_t startNs, uint64_t endNs, uint32_t status);
	/* (fSubmitLock held) p filled in, owner and nq held by it: to the GPU now if nothing holds it back, else later */
	void queuePending(struct pending *p);
	/* a queue going: wait for its work (2 s at most), then complete what is left with an error */
	void drainQueue(OSObject *owner, uint32_t queueID);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	/* the vendor's part */
	virtual void populateAccelConfig(IOGPUConfig *config);
	uint64_t maxResourceSize(void);
	virtual const char *gpuName(void);
	/* IOServiceOpen types that are not IOGPU's own (the vendor's private client); IOService's default by default */
	virtual IOReturn newVendorUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	    IOUserClient **handler);

	/*
	 * A client's GPU address space (Apple's IOGPUTask). GPU addresses are the task's own: a resource sits at the same
	 * address for the processors and the GPU. -1: none to give.
	 */
	virtual int newAddressSpace(void);
	virtual void freeAddressSpace(int as);
	/* len bytes of md from offset at va; flags kIOGPUMapReadOnly | kIOGPUMapUncached */
	virtual bool mapIntoAddressSpace(int as, uint64_t va, IOMemoryDescriptor *md, uint64_t offset, uint64_t len,
	    uint32_t flags);
	/* the pages gone from the GPU's tables, and the GPU's view updated before this returns (the memory may go next) */
	virtual void unmapFromAddressSpace(int as, uint64_t va, uint64_t len);

	/*
	 * The GPU's own queues (Apple's channels/work queues): one per IOGPU command queue, in its client's address space.
	 * Work goes in as (GPU address, length) of a vendor stream; each submission gets a sequence number 1, 2, ...
	 * (0: refused), and reached() says how far the GPU is. completionEvent() is woken when the GPU may have finished
	 * something (its interrupt).
	 */
	virtual int newHardwareQueue(int as, uint8_t priority);
	virtual void freeHardwareQueue(int hq);
	virtual uint64_t submitToHardwareQueue(int hq, uint64_t stream, uint32_t length);
	virtual uint64_t hardwareQueueReached(int hq);
	virtual bool hardwareQueueFaulted(int hq, uint64_t seqno);
	virtual void *completionEvent(void);

	IOBufferMemoryDescriptor *stampMemory(void) { return fStamps; }
	uint32_t stampOffset(unsigned channel) { return channel * sizeof(uint32_t); }
	static uint64_t nextObjectIDs(uint64_t count);  /* IOGPU's global object ids (trace ids): the first of count */
};

#endif /* _IOKIT_AKIOS_IOGPU_H */
