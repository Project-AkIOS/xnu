/*
 * AkIOS: IOAcceleratorES, IOGPU and IOGPUDeviceUserClient, as iOS's IOGPUFamily has them (akios_iogpu.h says why).
 *
 * What is answered so far: opening a device (type 1) and the questions IOGPUDeviceCreateWithOptions asks before it
 * hands back a device -- the configuration (0), the name (1), the event machine with the stamp page (2), the trace
 * filter (3), the device information (4), trace ids (5), the shared information (16), memory data (21) and the
 * allocated size (23). Every other selector answers kIOReturnUnsupported until it is written; the shapes of all 56
 * are in the table, so a caller with the wrong sizes is refused by IOUserClient as it would be by Apple's kernel.
 */
#include "akios_iogpu.h"
#include "MiniIOSurface.h"
#include "akios_shared_event.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <pexpert/pexpert.h>
extern "C" void proc_selfname(char *buf, int size);

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---- IOAcceleratorES ---- */

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

void
IOAcceleratorES::publishMetalPlugin(const char *name, const char *className)
{
	uint32_t metal = 0;
	static char plugin[96];

	/*
	 * Never by default: with these properties a program that asks for Metal gets a device, and CoreAnimation chooses
	 * Metal for the screen. metal_plugin=NAME: another name (Metal builds /System/Library/Extensions/NAME.bundle, so
	 * ../../../akita/lib/X reaches a bundle in our image).
	 */
	if (!PE_parse_boot_argn("metal", &metal, sizeof(metal)) || metal == 0) {
		return;
	}
	static char cls[96];

	strlcpy(plugin, name, sizeof(plugin));
	PE_parse_boot_arg_str("metal_plugin", plugin, sizeof(plugin));
	/* metal_class=NAME: the plug-in's device class (another plug-in has another class: AkiosMetal's AkiosMaliDevice) */
	strlcpy(cls, className, sizeof(cls));
	PE_parse_boot_arg_str("metal_class", cls, sizeof(cls));
	setProperty("MetalPluginName", plugin);
	setProperty("MetalPluginClassName", cls);
	IOLog("%s: metal=1: Metal's plug-in is %s (%s)\n", getName(), plugin, cls);
}

/* ---- IOGPU ---- */

OSDefineMetaClassAndStructors(IOGPU, IOAcceleratorES);

bool
IOGPU::start(IOService *provider)
{
	if (!IOAcceleratorES::start(provider)) {
		return false;
	}
	fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
	    page_size, page_size);
	if (fStamps == NULL) {
		return false;
	}
	bzero(fStamps->getBytesNoCopy(), page_size);
	fSubmitLock = IOLockAlloc();
	if (fSubmitLock == NULL || kernel_thread_start(completionThread, this, &fCompletionThread) != KERN_SUCCESS) {
		return false;
	}
	retireStandIns();
	return true;
}

uint64_t
IOGPU::nowNs(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

void
IOGPU::notify(IOSharedDataQueue *nq, uint64_t token, uint64_t startNs, uint64_t endNs, uint32_t status)
{
	struct notification n = { token, startNs, endNs, status, 0, 0 };
	static int said;

	/* (the standard "data available" message goes to the port when the queue was empty: IODataQueue's own) */
	if (nq == NULL || !nq->enqueue(&n, sizeof(n))) {
		if (said++ < 8) {
			IOLog("IOGPU: a notification (token 0x%llx, status %u) found no room\n", token, status);
		}
	}
}

extern "C" void akios_surface_busy(uint32_t sid, int32_t delta);   /* hakios.akita_gpu.cpp */

static void
releasePending(struct IOGPU::pending *p)
{
	for (uint32_t i = 0; i < p->busySurfaces; i++) {
		akios_surface_busy(p->busySurface[i], -1);
	}
	for (uint32_t i = 0; i < p->waits; i++) {
		OSSafeReleaseNULL(p->wait[i].event);
	}
	for (uint32_t i = 0; i < p->signals; i++) {
		OSSafeReleaseNULL(p->signal[i].event);
	}
	OSSafeReleaseNULL(p->nq);
	OSSafeReleaseNULL(p->owner);
	IOFree(p, sizeof(*p));
}

/* (fSubmitLock held) is something of p's queue ahead of it still waiting to go to the GPU? */
static bool
heldBack(struct IOGPU::pending *list, struct IOGPU::pending *p)
{
	for (struct IOGPU::pending *q = list; q != NULL && q != p; q = q->next) {
		if (q->owner == p->owner && q->queueID == p->queueID && !q->onGPU && q->hq >= 0) {
			return true;
		}
	}
	return false;
}

static bool
waitsMet(struct IOGPU::pending *p)
{
	for (uint32_t i = 0; i < p->waits; i++) {
		if (p->wait[i].event->value() < p->wait[i].value) {
			return false;
		}
	}
	return true;
}

/* the lowest number of the client's submissions not yet completed (~0: none out) */
uint64_t
IOGPU::oldestPendingNumber(OSObject *owner)
{
	uint64_t oldest = ~0ULL;

	IOLockLock(fSubmitLock);
	for (struct pending *p = fPending; p != NULL; p = p->next) {
		if (p->owner == owner && p->number < oldest) {
			oldest = p->number;
		}
	}
	IOLockUnlock(fSubmitLock);
	return oldest;
}

void
IOGPU::queuePending(struct pending *p)
{
	struct pending **pp = &fPending;

	while (*pp != NULL) {
		pp = &(*pp)->next;              /* (oldest first: a queue's buffers keep their order) */
	}
	p->next = NULL;
	*pp = p;
	completeReached(false);         /* (to the GPU at once when nothing holds it back) */
	thread_wakeup(&fPending);
}

/*
 * (fSubmitLock held) Moves every command buffer on: one whose waits are met (and nothing of its queue ahead of it
 * waits) goes to the GPU; one the GPU has passed signals its events and gets its "completed" notification.
 * all: every one completes now (with an error if the GPU had not passed it).
 */
void
IOGPU::completeReached(bool all)
{
	struct pending **pp = &fPending, *p;
	static int said;

	while ((p = *pp) != NULL) {
		uint32_t status = 0;

		if (p->hq >= 0 && !p->onGPU && !all) {
			if (heldBack(fPending, p) || !waitsMet(p)) {
				pp = &p->next;
				continue;
			}
			if (p->length != 0) {
				p->seqno = submitToHardwareQueue(p->hq, p->stream, p->length);
				if (p->seqno == 0) {
					if (said++ < 8) {
						IOLog("IOGPU: a stream (0x%llx, %u bytes) was refused by the GPU's queue\n", p->stream, p->length);
					}
					status = 0xa;                   /* InvalidInput */
				}
			}
			p->onGPU = true;
			if (status == 0 && p->length != 0) {
				pp = &p->next;
				continue;                       /* (looked at again below on a later pass) */
			}
		} else if (p->hq < 0 || (!p->onGPU && all)) {
			status = 1;                             /* (its queue went before it ran) */
		} else if (p->length != 0 && hardwareQueueReached(p->hq) < p->seqno) {
			/*
			 * The watchdog: a submission the GPU has not passed in 10 s is completed with an error (Metal's
			 * MTLCommandBufferErrorTimeout), so its program does not wait for good; the queue's later work keeps its
			 * order behind it on the GPU.
			 */
			if (!all && nowNs() - p->startNs < 10 * NSEC_PER_SEC) {
				pp = &p->next;
				continue;
			}
			if (!all && said++ < 8) {
				IOLog("IOGPU: a command buffer (hardware queue %d, %llu) not done after 10 s: completed with an error\n",
				    p->hq, p->seqno);
			}
			status = all ? 1 : 2;           /* kIOGPUCommandBufferCallbackErrorTimeout */
		} else if (p->length != 0 && hardwareQueueFaulted(p->hq, p->seqno)) {
			status = 0xb;                           /* PageFault */
		}
		/* its events signalled first, then the notification (Apple's order: stamps, then the entry) */
		if (status == 0) {
			for (uint32_t i = 0; i < p->signals; i++) {
				p->signal[i].event->signal(p->signal[i].value);
			}
		}
		notify(p->nq, p->token, p->startNs, nowNs(), status);
		*pp = p->next;
		releasePending(p);
	}
}

/*
 * Completions: woken by the GPU's interrupt (completionEvent) and by new submissions; a millisecond's look at most
 * while work is out, in case an interrupt is missed.
 */
void
IOGPU::completionThread(void *self, wait_result_t)
{
	IOGPU *gpu = (IOGPU *)self;

	thread_set_thread_name(current_thread(), "IOGPU completions");
	IOLockLock(gpu->fSubmitLock);
	for (;;) {
		gpu->completeReached(false);
		if (gpu->fPending == NULL) {
			IOLockSleep(gpu->fSubmitLock, &gpu->fPending, THREAD_UNINT);
			continue;
		}
		void *event = gpu->completionEvent();
		assert_wait_timeout(event != NULL ? event : (void *)&gpu->fPending, THREAD_UNINT, 1, NSEC_PER_MSEC);
		IOLockUnlock(gpu->fSubmitLock);
		thread_block(THREAD_CONTINUE_NULL);
		IOLockLock(gpu->fSubmitLock);
	}
}

void
IOGPU::drainQueue(OSObject *owner, uint32_t queueID)
{
	for (int ms = 0; ms < 2000; ms++) {
		bool left = false;

		IOLockLock(fSubmitLock);
		completeReached(false);
		for (struct pending *p = fPending; p != NULL; p = p->next) {
			left |= p->owner == owner && p->queueID == queueID && p->onGPU;
		}
		if (!left) {
			for (struct pending *p = fPending; p != NULL; p = p->next) {
				if (p->owner == owner && p->queueID == queueID) {
					p->hq = -1;             /* (still waiting for events: given up) */
				}
			}
			completeReached(false);
			IOLockUnlock(fSubmitLock);
			return;
		}
		IOLockUnlock(fSubmitLock);
		IOSleep(1);
	}
	IOLockLock(fSubmitLock);
	for (struct pending *p = fPending; p != NULL; p = p->next) {
		if (p->owner == owner && p->queueID == queueID) {
			p->hq = -1;
		}
	}
	completeReached(false);
	IOLockUnlock(fSubmitLock);
	IOLog("IOGPU: a command queue went with work not finished in 2 s, or waiting for events\n");
}

/*
 * The display driver publishes a bare IOAcceleratorES when it starts before any GPU driver (CoreAnimation waits for
 * one). Once a real GPU is here that stand-in goes: Metal makes a device of every accelerator that names a plug-in, and
 * MTLCreateSystemDefaultDevice aborts unless there is exactly one.
 */
void
IOGPU::retireStandIns(void)
{
	OSDictionary *match = IOService::serviceMatching("IOAcceleratorES");
	OSIterator *it = match != NULL ? IOService::getMatchingServices(match) : NULL;
	OSObject *o;

	OSSafeReleaseNULL(match);
	if (it == NULL) {
		return;
	}
	while ((o = it->getNextObject()) != NULL) {
		IOService *s = OSDynamicCast(IOService, o);

		if (s != NULL && s != this && s->getMetaClass() == IOAcceleratorES::metaClass) {
			IOLog("%s: the stand-in accelerator %s goes\n", getName(), s->getName());
			s->terminate();
		}
	}
	it->release();
}

void
IOGPU::free(void)
{
	OSSafeReleaseNULL(fStamps);
	IOAcceleratorES::free();
}

void
IOGPU::populateAccelConfig(IOGPUConfig *config)
{
	bzero(config, sizeof(*config));
}

uint64_t
IOGPU::maxResourceSize(void)
{
	IOGPUConfig config;

	populateAccelConfig(&config);
	return config.maxResourceSize;
}

const char *
IOGPU::gpuName(void)
{
	return getName();
}

int
IOGPU::newAddressSpace(void)
{
	return -1;
}

void
IOGPU::freeAddressSpace(int as)
{
}

bool
IOGPU::mapIntoAddressSpace(int as, uint64_t va, IOMemoryDescriptor *md, uint64_t offset, uint64_t len, uint32_t flags)
{
	return false;
}

void
IOGPU::unmapFromAddressSpace(int as, uint64_t va, uint64_t len)
{
}

int
IOGPU::newHardwareQueue(int as, uint8_t priority)
{
	return -1;
}

void
IOGPU::freeHardwareQueue(int hq)
{
}

uint64_t
IOGPU::submitToHardwareQueue(int hq, uint64_t stream, uint32_t length)
{
	return 0;
}

uint64_t
IOGPU::hardwareQueueReached(int hq)
{
	return 0;
}

bool
IOGPU::hardwareQueueFaulted(int hq, uint64_t seqno)
{
	return false;
}

void *
IOGPU::completionEvent(void)
{
	return NULL;
}

IOReturn
IOGPU::vendorMethod(OSObject *client, int as, uint32_t selector, IOExternalMethodArguments *args)
{
	return kIOReturnUnsupported;
}

void
IOGPU::clientGone(OSObject *client, int as)
{
}

IOReturn
IOGPU::newVendorUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
    IOUserClient **handler)
{
	return IOService::newUserClient(owningTask, securityID, type, properties, handler);
}

/* IOGPUFamily's __globalObjectIDCounter: starts at 1 (IOGPU.framework asserts on an id group starting at 0) */
static volatile SInt64 gObjectIDs = 1;

uint64_t
IOGPU::nextObjectIDs(uint64_t count)
{
	return (uint64_t)OSAddAtomic64((SInt64)count, &gObjectIDs);
}

class IOGPUDeviceUserClient;
static IOGPUDeviceUserClient *newDeviceClient(IOGPU *gpu, task_t task, void *securityID, uint32_t options);

IOReturn
IOGPU::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
	/* iOS's IOGPU::newUserClient: 1 the device, 0x21 the memory information; type >> 16 the device's options */
	if ((type & 0xffff) == 1) {
		IOGPUDeviceUserClient *client = newDeviceClient(this, owningTask, securityID, type >> 16);

		if (client == NULL) {
			return kIOReturnNoMemory;
		}
		*handler = (IOUserClient *)client;
		return kIOReturnSuccess;
	}
	if ((type & 0xffff) == 0x21) {
		return kIOReturnUnsupported;
	}
	return newVendorUserClient(owningTask, securityID, type, properties, handler);
}

/* ---- IOGPUDeviceUserClient ---- */

class IOGPUDeviceUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOGPUDeviceUserClient);

	IOGPU *fGPU;
	task_t fTask;
	uint32_t fOptions;
	IOMemoryMap *fStampMap;         /* the stamp page in the client's task, read-only, made at the open */
	uint64_t fGlobalID;             /* this client's object id (selector 4) */
	uint64_t fDeviceID;             /* its device's (selector 16) */
	IOLock *fLock;

	/* shared memory (selector 12): the id is the index + 1 */
	enum { kMaxShmem = 256 };
	struct shmem { IOBufferMemoryDescriptor *memory; IOMemoryMap *map; } fShmem[kMaxShmem];
	void releaseAll(void);

	/* resources (selector 8): the id is the index + 1 */
	struct surfaceMap {
		MiniIOSurface *surface;         /* held; NULL: a free entry */
		IOMemoryDescriptor *memory;
		IOMemoryMap *map;
		uint64_t gpuBase, gpuLength;
		uint32_t refs;                  /* resources using it */
		uint64_t idleAfter;             /* with refs 0: the submission number the GPU must be past */
	};
	struct resource {
		uint32_t kind;
		IOMemoryDescriptor *memory;     /* what the GPU maps (a client buffer's: prepared) */
		IOMemoryMap *map;               /* in the task (kind 0, 0x82): its address is also the GPU's */
		MiniIOSurface *surface;         /* kind 0x82: held */
		struct surfaceMap *shared;      /* kind 0x82: the surface's mapping in this client, shared by its resources */
		uint64_t gpuBase, gpuLength;    /* the GPU mapping (page-aligned) */
		uint32_t sharedSlot;            /* in the client-shared blocks */
		bool prepared;
		uint8_t purgeable;              /* (11-28) its purgeable state as Metal sees it: 2 non-volatile (0: not set = 2) */
		struct resource *nextDeferred;
		uint64_t deferredUntil;         /* freed once every submission of the client numbered below this is done */
	};
	struct resource *fDeferred;     /* deleted while the GPU may still use them */
	/*
	 * SURFACE MAPPINGS (11-26): an IOSurface is mapped into a client's task and GPU address space once and stays there
	 * while the surface lives, whatever its resources do -- as on Apple's GPUs, where every texture over a surface sees
	 * it at the same address. Mapped afresh per texture, CoreAnimation's next texture over a surface still drew from
	 * the address of a texture over it that it had let go (IOGPU.framework keeps what a surface was): the GPU faulted
	 * there and lost the submission -- frames dropped, the panel stuck on the one before.
	 * A mapping goes once no resource uses it, nobody but this cache holds its surface, and the GPU is past it.
	 */
	enum { kMaxSurfaceMaps = 96 };
	struct surfaceMap fSurfaceMaps[kMaxSurfaceMaps];
	struct surfaceMap *surfaceMapFor(MiniIOSurface *surface, uint64_t size, IOReturn *ret);
	void sweepSurfaceMaps(bool all);
	/*
	 * (fLock held) whether anything else of this client's still maps the page: a resource other than r (any kind, live or
	 * deleted but not yet freed) or a cached surface mapping. (11-27: QuartzCore wraps pieces of its own Metal buffers as
	 * no-copy buffers, so a client-memory resource lies inside a buffer of another kind; freeing the big one took the
	 * small one's pages from the GPU -- 176 faults a boot, draws lost.)
	 */
	bool
	pageStillMapped(struct resource *r, uint64_t page)
	{
		for (uint32_t i = 0; i < fResourceCount; i++) {
			struct resource *o = fResources[i];

			if (o != NULL && o != r && o->gpuLength != 0 && page >= o->gpuBase && page < o->gpuBase + o->gpuLength) {
				return true;
			}
		}
		for (struct resource *o = fDeferred; o != NULL; o = o->nextDeferred) {
			if (o != r && o->gpuLength != 0 && page >= o->gpuBase && page < o->gpuBase + o->gpuLength) {
				return true;
			}
		}
		for (uint32_t i = 0; i < kMaxSurfaceMaps; i++) {
			if (fSurfaceMaps[i].surface != NULL && page >= fSurfaceMaps[i].gpuBase &&
			    page < fSurfaceMaps[i].gpuBase + fSurfaceMaps[i].gpuLength) {
				return true;
			}
		}
		return false;
	}
	uint64_t fSubmissions;          /* this client's submissions so far (numbers 1, 2, ...; under the submission lock) */
	void freeDeferred(bool all);
	struct resource **fResources;
	uint32_t fResourceCount;        /* entries in fResources */
	int fAS;                        /* the GPU address space (-1: not made yet) */

	/*
	 * Client-shared slots: two IOGPUEvents (0x80 bytes) a resource, 0x100 a block, each block one buffer mapped
	 * read-only into the task. Slot 0 is the device's (selector 4's allocated size, never written: Apple's stays 0).
	 */
	enum { kSharedSlotSize = 0x80, kSlotsPerBlock = 0x100, kMaxSharedBlocks = 64 };
	struct { IOBufferMemoryDescriptor *memory; IOMemoryMap *map; } fShared[kMaxSharedBlocks];
	uint8_t fSlotUsed[kMaxSharedBlocks * kSlotsPerBlock / 8];
	bool sharedSlot(uint32_t *slot, uint64_t *address);
	void freeSharedSlot(uint32_t slot);
	void releaseResource(struct resource *r);
	IOReturn deleteResource(uint32_t id);

	/* command queues (selector 6): the id is the index + 1 */
	enum { kMaxQueues = 64, kMaxNotificationQueues = 64 };
	/* (11-28) priority/background (selector 26) and QoS (27) kept as IOGPUCommandQueue keeps them (+0x444/0x448/0x450) */
	struct queue { bool used; int hq; uint32_t nq; uint32_t priority, background, qos; } fQueues[kMaxQueues];
	IOReturn purgeable(uint32_t id, uint32_t state, uint32_t *old);
	struct nqueue { IOSharedDataQueue *queue; IOMemoryMap *map; mach_port_t port; } fNQueues[kMaxNotificationQueues];
	bool addressSpace(void);
	void deleteQueue(uint32_t id);
	void destroyNotificationQueue(uint32_t id);
	IOReturn submit(uint32_t queueID, const uint8_t *args, uint32_t argsSize, uint32_t count);
	void kernelCommands(const struct IOGPUCommandBufferArgs *a, struct IOGPU::pending *p);
	void resourceSegments(const uint8_t *list, uint32_t size, struct IOGPU::pending *p);

	static const IOExternalTrap sTraps[14];
	IOReturn t_submit_command_buffer(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
	IOReturn t_unsupported(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
	IOReturn t_delete_resource(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
	IOReturn t_finish_resource_event(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);
	IOReturn t_set_resource_purgeable(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6);

	static const IOExternalMethodDispatch sMethods[56];

	static IOReturn s_unsupported(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_config(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_name(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_event_machine(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_current_trace_filter(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_device_info(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_next_gid_group(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_shared_info(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_memory_data(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_allocated_size(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_create_shmem(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_destroy_shmem(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_perform_mapping(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_finish_object_event(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_set_resource_purgeable(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_set_priority_and_background(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_set_quality_of_service(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_set_app_gpu_role(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_get_app_gpu_role(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_set_resource_owner_identity(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_create_device_assertion(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_new_resource(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_delete_resource(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_new_command_queue(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_delete_command_queue(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_create_notificationqueue(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_destroy_notificationqueue(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_set_notification_queue(OSObject *target, void *reference, IOExternalMethodArguments *args);
	static IOReturn s_submit_command_buffers(OSObject *target, void *reference, IOExternalMethodArguments *args);

public:
	bool setUp(IOGPU *gpu, task_t task, void *securityID, uint32_t options);
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	virtual IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;
	/* IOConnectGetService: the GPU (the driver reads the GPU's description from its properties) */
	virtual IOService *getService(void) APPLE_KEXT_OVERRIDE { return fGPU; }
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOGPUDeviceUserClient, IOUserClient);

#define V       kIOUCVariableStructureSize
#define M(fn, si, ti, so, to) { (IOExternalMethodAction)&IOGPUDeviceUserClient::fn, si, ti, so, to }
#define U(si, ti, so, to) M(s_unsupported, si, ti, so, to)

/* iOS 27.2.1's sDeviceMethods: (function, scalars in, structure in, scalars out, structure out) */
const IOExternalMethodDispatch IOGPUDeviceUserClient::sMethods[56] = {
	M(s_get_config, 0, 0, 0, 0x40),                 /*  0 */
	M(s_get_name, 0, 0, 0, 0x40),                   /*  1 */
	M(s_get_event_machine, 0, 0, 0, 0x218),         /*  2 */
	M(s_get_current_trace_filter, 0, 0, 0, 8),      /*  3 */
	M(s_get_device_info, 0, 0, 0, 0x20),            /*  4 */
	M(s_get_next_gid_group, 0, 0, 0, 0x10),         /*  5 */
	M(s_new_command_queue, 0, 0x408, 0, 0x10),      /*  6 */
	M(s_delete_command_queue, 1, 0, 0, 0),          /*  7 */
	M(s_new_resource, 0, V, 0, V),                  /*  8 */
	M(s_delete_resource, 1, 0, 0, 0),               /*  9 */
	M(s_finish_object_event, 2, 0, 0, 0),           /* 10 */
	M(s_set_resource_purgeable, 2, 0, 1, 0),        /* 11 */
	M(s_create_shmem, 2, 0, 0, 0x10),               /* 12 */
	M(s_destroy_shmem, 1, 0, 0, 0),                 /* 13 */
	M(s_create_notificationqueue, 2, 0, 0, 0x10),   /* 14 */
	M(s_destroy_notificationqueue, 1, 0, 0, 0),     /* 15 */
	M(s_get_shared_info, 0, 0, 0, 8),               /* 16 */
	U(0, 0, 0, 4),                                  /* 17 create_mtlfence */
	U(1, 0, 0, 0),                                  /* 18 destroy_mtlfence */
	U(1, 0, 0, 0x18),                               /* 19 create_mtlevent */
	U(1, 0, 0, 0),                                  /* 20 destroy_mtlevent */
	M(s_get_memory_data, 0, 0, 0, 0x30),            /* 21 */
	U(0, 0, 0, V),                                  /* 22 unsupported (Apple's too) */
	M(s_get_allocated_size, 0, 0, 1, 0),            /* 23 */
	M(s_set_notification_queue, 2, 0, 0, 0),        /* 24 */
	M(s_submit_command_buffers, 4, V, 1, 0),        /* 25 */
	M(s_set_priority_and_background, 1, 0xc, 1, 0), /* 26 */
	M(s_set_quality_of_service, 1, 4, 0, 0),        /* 27 */
	U(0, 0, 2, 0),                                  /* 28 create_mtllateevalevent */
	U(1, 0, 0, 0),                                  /* 29 destroy_mtllateevalevent */
	U(2, 0, 0, 0),                                  /* 30 async_signal_mtlLateEvalevent */
	U(1, 0, 2, 0),                                  /* 31 query_mtlLateEvalevent */
	U(2, 0, 0, 0),                                  /* 32 set_display_params_for_gpu (Apple's base: unsupported too) */
	M(s_set_app_gpu_role, 2, 0, 0, 0),              /* 33 */
	M(s_get_app_gpu_role, 1, 0, 1, 0),              /* 34 */
	M(s_set_resource_owner_identity, 2, 0, 0, 0),   /* 35 */
	U(3, 0, 1, 0),                                  /* 36 create_resource_iosurface */
	U(1, 0, 0, 0),                                  /* 37 resource_detach_backing */
	U(0, 0x18, 0, 0),                               /* 38 resource_replace_backing_bytes */
	U(0, 0x18, 1, 0),                               /* 39 resource_replace_backing_ranges */
	U(1, 0, 2, 0),                                  /* 40 create_vniodesc */
	U(1, 0, 0, 0),                                  /* 41 destroy_vniodesc */
	U(2, 0, 2, 0),                                  /* 42 create_io_command_queue */
	U(1, 0, 0, 0),                                  /* 43 destroy_io_command_queue */
	U(2, 0, 0, 0),                                  /* 44 set_io_notification_queue */
	U(1, V, 0, 0),                                  /* 45 submit_io_commands */
	U(1, 0, 2, 0),                                  /* 46 create_io_command_buffer */
	U(2, 0, 0, 0),                                  /* 47 destroy_io_command_buffer */
	U(2, 0, 0, 0),                                  /* 48 try_cancel_io_command_buffer */
	U(1, 0, 0, 0),                                  /* 49 perform_io */
	U(1, 0, 0, 0),                                  /* 50 io_command_buffer_complete */
	U(3, 0, 0, 0),                                  /* 51 io_command_buffer_barrier_complete */
	U(2, V, 0, 0),                                  /* 52 group_add_resources */
	U(2, V, 0, 0),                                  /* 53 group_remove_resources */
	M(s_create_device_assertion, 2, 0, 1, 0),       /* 54 */
	M(s_perform_mapping, 0, 0, 0, 0),               /* 55 */
};

#undef U
#undef M
#undef V

static IOGPUDeviceUserClient *
newDeviceClient(IOGPU *gpu, task_t task, void *securityID, uint32_t options)
{
	IOGPUDeviceUserClient *client = OSTypeAlloc(IOGPUDeviceUserClient);

	if (client != NULL && !client->setUp(gpu, task, securityID, options)) {
		OSSafeReleaseNULL(client);
	}
	return client;
}

bool
IOGPUDeviceUserClient::setUp(IOGPU *gpu, task_t task, void *securityID, uint32_t options)
{
	if (!initWithTask(task, securityID, 1, NULL)) {
		return false;
	}
	fGPU = gpu;
	fTask = task;
	fOptions = options;
	fAS = -1;
	fSlotUsed[0] = 1;               /* slot 0: the device's */
	fLock = IOLockAlloc();
	/*
	 * As IOGPUDeviceUserClient::deviceUserClientStart: the stamp page goes into the task now, read-only (kIOMapUnique |
	 * kIOMapReadOnly | kIOMapAnywhere, Apple's 0x04001001); no selector maps it and there is no memory type for it.
	 */
	fStampMap = gpu->stampMemory()->createMappingInTask(task, 0, kIOMapAnywhere | kIOMapReadOnly | kIOMapUnique);
	if (fLock == NULL || fStampMap == NULL) {
		return false;
	}
	fGlobalID = IOGPU::nextObjectIDs(1);
	fDeviceID = IOGPU::nextObjectIDs(1);
	if (!attach(gpu)) {
		return false;
	}
	if (!start(gpu)) {
		detach(gpu);
		return false;
	}
	return true;
}

void
IOGPUDeviceUserClient::releaseAll(void)
{
	for (uint32_t i = 1; i <= kMaxQueues; i++) {
		deleteQueue(i);
	}
	for (uint32_t i = 1; i <= kMaxNotificationQueues; i++) {
		destroyNotificationQueue(i);
	}
	if (fLock != NULL && fGPU != NULL) {
		freeDeferred(true);     /* (the queues are drained or given up by now) */
	}
	if (fLock != NULL) {
		IOLockLock(fLock);
	}
	for (uint32_t i = 0; i < fResourceCount; i++) {
		if (fResources[i] != NULL) {
			releaseResource(fResources[i]);
			fResources[i] = NULL;
		}
	}
	sweepSurfaceMaps(true);
	if (fResources != NULL) {
		IOFree(fResources, fResourceCount * sizeof(fResources[0]));
		fResources = NULL;
		fResourceCount = 0;
	}
	if (fAS >= 0) {
		fGPU->clientGone(this, fAS);
		fGPU->freeAddressSpace(fAS);
		fAS = -1;
	}
	for (unsigned b = 0; b < kMaxSharedBlocks; b++) {
		OSSafeReleaseNULL(fShared[b].map);
		OSSafeReleaseNULL(fShared[b].memory);
	}
	for (unsigned i = 0; i < kMaxShmem; i++) {
		OSSafeReleaseNULL(fShmem[i].map);
		OSSafeReleaseNULL(fShmem[i].memory);
	}
	OSSafeReleaseNULL(fStampMap);
	if (fLock != NULL) {
		IOLockUnlock(fLock);
	}
}

IOReturn
IOGPUDeviceUserClient::clientClose(void)
{
	releaseAll();           /* (it takes the locks it needs: deleting a queue waits for the GPU, unlocked) */
	terminate();
	return kIOReturnSuccess;
}

void
IOGPUDeviceUserClient::free(void)
{
	releaseAll();
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOGPUDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	if (selector >= sizeof(sMethods) / sizeof(sMethods[0])) {
		bool ok;

		if (selector < IOGPU::kIOGPUVendorSelectorBase + 64) {
			IOLockLock(fLock);
			ok = addressSpace();
			IOLockUnlock(fLock);
			return ok ? fGPU->vendorMethod(this, fAS, selector, args) : kIOReturnNoResources;
		}
		return kIOReturnUnsupported;
	}
	if (args->asyncWakePort != MACH_PORT_NULL) {
		return kIOReturnBadArgument;    /* Apple's refuses asynchronous calls too */
	}
	return IOUserClient::externalMethod(selector, args, (IOExternalMethodDispatch *)&sMethods[selector], this, NULL);
}

IOReturn
IOGPUDeviceUserClient::s_unsupported(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	/* (11-28) which of these iOS really calls, and who: the first 4 of each selector said, then a count every 64 */
	static uint32_t calls[56];
	uint32_t sel = args->selector < 56 ? args->selector : 55, n = ++calls[sel];

	if (n <= 4 || (n & 63) == 0) {
		char who[32] = "?";

		proc_selfname(who, sizeof(who));
		printf("akios_iogpu: unsupported selector %u by %s (call %u; scalars in %u: %#llx %#llx, structure in %u)\n", sel,
		    who, n, args->scalarInputCount, args->scalarInputCount > 0 ? args->scalarInput[0] : 0,
		    args->scalarInputCount > 1 ? args->scalarInput[1] : 0, args->structureInputSize);
	}
	return kIOReturnUnsupported;
}

IOReturn
IOGPUDeviceUserClient::s_get_config(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	IOGPUConfig config;

	me->fGPU->populateAccelConfig(&config);
	memcpy(args->structureOutput, &config, MIN(sizeof(config), args->structureOutputSize));
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_get_name(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;

	bzero(args->structureOutput, args->structureOutputSize);
	strlcpy((char *)args->structureOutput, me->fGPU->gpuName(), args->structureOutputSize);
	return kIOReturnSuccess;
}

/*
 * The event machine: where the stamp page is in the caller's task and where each channel's stamp is in it.
 * IOGPU.framework tests an event (IOGPUEvent: up to 15 (stamp & ~0xff | channel) slots) against these words without
 * asking the kernel.
 */
struct IOGPUEventMachineInfo {
	uint64_t stampBase;             /* +0x000: the page, in the caller's task */
	uint32_t clientSharedPrivSize;  /* +0x008: a vendor area after a resource's client-shared events (at +0x80) */
	uint32_t word0c;                /* +0x00c */
	uint32_t resourcePrivSize;      /* +0x010: the vendor's tail of a new resource's answer (after 0x58 bytes) */
	int32_t numStamps;              /* +0x014 */
	uint32_t stampOffset[128];      /* +0x018 */
};

IOReturn
IOGPUDeviceUserClient::s_get_event_machine(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	IOGPUEventMachineInfo *info = (IOGPUEventMachineInfo *)args->structureOutput;

	bzero(info, args->structureOutputSize);
	info->stampBase = me->fStampMap->getAddress();
	info->numStamps = kIOGPUEventChannels;
	for (unsigned i = 0; i < kIOGPUEventChannels; i++) {
		info->stampOffset[i] = me->fGPU->stampOffset(i);
	}
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_get_current_trace_filter(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	bzero(args->structureOutput, args->structureOutputSize);
	return kIOReturnSuccess;
}

/*
 * Device information: (this client's object id, the graphics comm page in the task, client-shared block 0 (its first
 * u64 the allocated size), the comm page's length). No comm page and no client-shared block here yet: zeros, which
 * IOGPU.framework takes as "none" (selector 23 answers the allocated size instead).
 */
IOReturn
IOGPUDeviceUserClient::s_get_device_info(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint64_t *out = (uint64_t *)args->structureOutput;

	bzero(out, args->structureOutputSize);
	out[0] = me->fGlobalID;
	return kIOReturnSuccess;
}

/* a group of global object ids for the caller's command buffers and encoders: (first, end); first is never 0 */
IOReturn
IOGPUDeviceUserClient::s_get_next_gid_group(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	uint64_t *out = (uint64_t *)args->structureOutput;

	out[0] = IOGPU::nextObjectIDs(0x1000000);
	out[1] = out[0] + 0x1000000;
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_get_shared_info(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;

	*(uint64_t *)args->structureOutput = me->fDeviceID;
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_get_memory_data(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	bzero(args->structureOutput, args->structureOutputSize);
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_get_allocated_size(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	args->scalarOutput[0] = 0;
	return kIOReturnSuccess;
}

/*
 * Shared memory for the command buffers (types: 0 segment list, 1 kernel commands, 2 debug buffer, 3 sideband; the
 * type changes nothing here, as in Apple's base class): (size, type) in, (task address, size, id) out.
 */
struct IOGPUShmemInfo {
	uint64_t address;
	uint32_t size;
	uint32_t shmemID;
};

IOReturn
IOGPUDeviceUserClient::s_create_shmem(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	IOGPUShmemInfo *out = (IOGPUShmemInfo *)args->structureOutput;
	uint64_t size = args->scalarInput[0];
	IOBufferMemoryDescriptor *md;
	IOMemoryMap *map;
	unsigned i;

	bzero(out, sizeof(*out));
	if (size == 0 || size > 0x10000000) {
		return kIOReturnNoSpace;
	}
	md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
	    round_page(size), page_size);
	if (md == NULL) {
		return kIOReturnNoMemory;
	}
	bzero(md->getBytesNoCopy(), md->getLength());
	map = md->createMappingInTask(me->fTask, 0, kIOMapAnywhere);
	if (map == NULL) {
		md->release();
		return kIOReturnNoMemory;
	}
	IOLockLock(me->fLock);
	for (i = 0; i < kMaxShmem && me->fShmem[i].memory != NULL; i++) {
	}
	if (i == kMaxShmem) {
		IOLockUnlock(me->fLock);
		map->release();
		md->release();
		return kIOReturnNoResources;
	}
	me->fShmem[i].memory = md;
	me->fShmem[i].map = map;
	IOLockUnlock(me->fLock);
	out->address = map->getAddress();
	out->size = (uint32_t)map->getLength();
	out->shmemID = i + 1;
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_destroy_shmem(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint64_t id = args->scalarInput[0];
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(me->fLock);
	if (id >= 1 && id <= kMaxShmem && me->fShmem[id - 1].memory != NULL) {
		OSSafeReleaseNULL(me->fShmem[id - 1].map);
		OSSafeReleaseNULL(me->fShmem[id - 1].memory);
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(me->fLock);
	return ret;
}

/*
 * Metal 4's mapping thread calls this in a loop and stops at kIOReturnAborted; any other immediate answer would make
 * it spin. Nothing is mapped this way here yet.
 */
IOReturn
IOGPUDeviceUserClient::s_perform_mapping(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	return kIOReturnAborted;
}

/* ---- resources ---- */

/* selector 8's argument header (0x58 bytes; the vendor's tail follows) */
struct IOGPUNewResourceArgs {
	uint32_t kind;                  /* 0 system memory, 0x80 the client's memory, 0x82 an IOSurface, 0x40 sparse */
	uint32_t cacheMode;             /* 0x400: write-combined */
	uint16_t width, height, depthOrSlices, pad0e;
	uint8_t faces, mipLevels, pad12, pixelInfo;
	uint32_t flags;                 /* 0x40 zero-fill, 0x2000 private (no processor mapping), 0x10/0x20 GPU read/write */
	uint64_t protectionOptions;
	uint64_t gpuAddress, gpuLength; /* a fixed GPU address asked for */
	uint32_t alignment;
	uint32_t gpuTagHi;
	uint64_t a38;                   /* 0x80: the client's pointer; 0x82: surface id (low), plane (high) */
	uint64_t a40;                   /* 0x82: the field */
	uint64_t size;                  /* bytes to back */
	uint32_t a50, pad54;
};

/* its answer (0x58 bytes, then resourcePrivSize: 0 here) */
struct IOGPUNewResourceOut {
	uint64_t gpuAddress;            /* +0x00 */
	uint64_t cpuAddress;            /* +0x08: the buffer's contents (kind 0) */
	uint64_t clientShared;          /* +0x10: the resource's two IOGPUEvents in the task; never 0 */
	uint64_t metadata;              /* +0x18 */
	uint32_t flags;                 /* +0x20: bit 0, the client zero-fills */
	uint32_t resourceID;            /* +0x24 */
	uint64_t residentSize;          /* +0x28 */
	uint64_t word30;                /* +0x30 */
	uint64_t allocationID[2];       /* +0x38 */
	uint64_t protectionOptions;     /* +0x48 */
	uint64_t gpuBase;               /* +0x50: the GPU address minus the offset inside the allocation */
};

#define kIOGPUResourceSysMem    0x00
#define kIOGPUResourceClient    0x80
#define kIOGPUResourceSurface   0x82
#define kIOGPUFlagZeroFill      0x40
#define kIOGPUFlagPrivate       0x2000
#define kIOGPUFlagGPURead       0x10
#define kIOGPUFlagGPUWrite      0x20

bool
IOGPUDeviceUserClient::sharedSlot(uint32_t *slot, uint64_t *address)
{
	for (uint32_t i = 1; i < kMaxSharedBlocks * kSlotsPerBlock; i++) {
		unsigned b = i / kSlotsPerBlock;

		if (fSlotUsed[i / 8] & (1u << (i % 8))) {
			continue;
		}
		if (fShared[b].memory == NULL) {
			IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, kSlotsPerBlock * kSharedSlotSize, page_size);

			if (md == NULL) {
				return false;
			}
			bzero(md->getBytesNoCopy(), md->getLength());
			fShared[b].map = md->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
			if (fShared[b].map == NULL) {
				md->release();
				return false;
			}
			fShared[b].memory = md;
		}
		/* a zeroed slot: both events "complete" (validMask 0) until submissions write them */
		bzero((uint8_t *)fShared[b].memory->getBytesNoCopy() + (i % kSlotsPerBlock) * kSharedSlotSize, kSharedSlotSize);
		fSlotUsed[i / 8] |= (uint8_t)(1u << (i % 8));
		*slot = i;
		*address = fShared[b].map->getAddress() + (i % kSlotsPerBlock) * kSharedSlotSize;
		return true;
	}
	return false;
}

void
IOGPUDeviceUserClient::freeSharedSlot(uint32_t slot)
{
	if (slot != 0 && slot < kMaxSharedBlocks * kSlotsPerBlock) {
		fSlotUsed[slot / 8] &= (uint8_t)~(1u << (slot % 8));
	}
}

void
IOGPUDeviceUserClient::releaseResource(struct resource *r)
{
	if (r->shared != NULL) {
		/* (fLock held) the surface's mapping stays; once unused, the GPU must get past what was submitted so far */
		if (--r->shared->refs == 0) {
			fGPU->lockSubmissions();
			r->shared->idleAfter = fSubmissions + 1;
			fGPU->unlockSubmissions();
		}
		r->gpuLength = 0;
	}
	/* the GPU's view first (updated before this returns), then the memory */
	if (r->gpuLength != 0 && fAS >= 0) {
		/*
		 * (11-26) the client's own pages sit at their own addresses, and QuartzCore makes buffers over overlapping
		 * pieces of the same memory: the pages another of its buffers still covers stay mapped (unmapped with
		 * everything, a live buffer's pages went with a dead one's and the GPU faulted on them)
		 */
		uint64_t run = 0, end = r->gpuBase + r->gpuLength;

		for (uint64_t page = r->gpuBase; page <= end; page += PAGE_SIZE) {
			bool keep = page < end && pageStillMapped(r, page);

			if (page < end && !keep) {
				if (run == 0) {
					run = page;
				}
				continue;
			}
			if (run != 0) {
				fGPU->unmapFromAddressSpace(fAS, run, page - run);
				run = 0;
			}
		}
	}
	OSSafeReleaseNULL(r->map);
	if (r->memory != NULL && r->prepared) {
		r->memory->complete();
	}
	OSSafeReleaseNULL(r->memory);
	if (r->surface != NULL) {
		MiniIOSurfaceDrop(r->surface);
	}
	freeSharedSlot(r->sharedSlot);
	IOFree(r, sizeof(*r));
}

IOReturn
IOGPUDeviceUserClient::s_new_resource(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	IOGPUNewResourceArgs a;
	IOGPUNewResourceOut *out = (IOGPUNewResourceOut *)args->structureOutput;
	struct resource *r;
	uint64_t shared = 0, offset = 0;
	uint32_t gpuFlags = 0, id;
	IOReturn ret = kIOReturnSuccess;
	static int said;

	if (args->structureInputSize < sizeof(a) || args->structureOutputSize < sizeof(*out)) {
		return kIOReturnBadArgument;
	}
	memcpy(&a, args->structureInput, sizeof(a));
	bzero(out, args->structureOutputSize);
	if (a.gpuAddress != 0 || a.gpuTagHi != 0) {
		/* (GPU addresses are the task's here: one given by the caller is not, yet) */
		if (said++ < 8) {
			IOLog("IOGPU: a resource at a fixed GPU address 0x%llx (tag 0x%x) was asked for: not yet\n", a.gpuAddress,
			    a.gpuTagHi);
		}
		return kIOReturnUnsupported;
	}
	if ((a.flags & (kIOGPUFlagGPURead | kIOGPUFlagGPUWrite)) == kIOGPUFlagGPURead) {
		gpuFlags |= kIOGPUMapReadOnly;
	}
	r = (struct resource *)IOMallocZero(sizeof(*r));
	if (r == NULL) {
		return kIOReturnNoMemory;
	}
	r->kind = a.kind;

	switch (a.kind) {
	case kIOGPUResourceSysMem: {
		IOBufferMemoryDescriptor *md;
		uint64_t size = round_page_64(a.size);

		if (a.size == 0 || a.size > me->fGPU->maxResourceSize()) {
			ret = kIOReturnNoSpace;
			break;
		}
		/*
		 * The GPU does not snoop the processors' caches (coherency "none"): the processors' view is write-combined
		 * whatever was asked, so what they write is in memory when the GPU reads it. The GPU's own view is cached (its
		 * caches are cleaned and invalidated around every submission, as for panthor's buffers): a non-cacheable
		 * mapping left every shader-drawn frame black (11-26) while clears and copies worked.
		 */
		md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
		    (kIOWriteCombineCache << kIOMemoryBufferCacheShift), size, page_size);
		if (md == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		if (a.flags & kIOGPUFlagZeroFill) {
			bzero(md->getBytesNoCopy(), size);
		}
		r->memory = md;
		r->map = md->createMappingInTask(me->fTask, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
		if (r->map == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		r->gpuBase = r->map->getAddress();
		r->gpuLength = size;
		if (!(a.flags & kIOGPUFlagPrivate)) {
			out->cpuAddress = r->gpuBase;
		}
		out->residentSize = size;
		break;
	}
	case kIOGPUResourceClient: {
		/* the client's own pages, at their own addresses (processor-cached: kept clean at submission, later) */
		uint64_t start = trunc_page_64(a.a38), end = round_page_64(a.a38 + a.size);
		IOMemoryDescriptor *md;

		if (a.size == 0 || end <= start) {
			ret = kIOReturnBadArgument;
			break;
		}
		md = IOMemoryDescriptor::withAddressRange(start, end - start, kIODirectionInOut, me->fTask);
		if (md != NULL && md->prepare() != kIOReturnSuccess) {
			/*
			 * (11-26) memory the task may only read (QuartzCore's buffers over read-only shared memory: protection 1,
			 * at most 1): wired for reading, and read-only for the GPU too
			 */
			md->release();
			md = IOMemoryDescriptor::withAddressRange(start, end - start, kIODirectionOut, me->fTask);
			gpuFlags |= kIOGPUMapReadOnly;
		}
		if (md == NULL || (ret = md->prepare()) != kIOReturnSuccess) {
			IOLog("IOGPU: client memory 0x%llx..0x%llx: %s 0x%x\n", start, end, md == NULL ? "no descriptor" :
			    "wiring it failed", ret);
			OSSafeReleaseNULL(md);
			ret = kIOReturnNoMemory;
			break;
		}
		r->memory = md;
		r->prepared = true;
		r->gpuBase = start;
		r->gpuLength = end - start;
		offset = a.a38 - start;
		out->cpuAddress = a.a38;
		out->residentSize = end - start;
		break;
	}
	case kIOGPUResourceSurface: {
		MiniIOSurface *surface = MiniIOSurfaceCopy((uint32_t)a.a38);
		uint32_t plane = (uint32_t)(a.a38 >> 32);
		uint64_t size;

		if (surface == NULL) {
			ret = kIOReturnNotFound;
			break;
		}
		r->surface = surface;
		if (plane >= MAX(surface->info.planeCount, 1u) || plane >= MINISURF_MAX_PLANES) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (surface->client != NULL && surface->scanoutSlot <= 0) {
			ret = kIOReturnUnsupported;     /* (a surface around a client's memory: as panthor's surfaceBuffer) */
			break;
		}
		size = round_page_64(surface->info.allocSize);
		IOLockLock(me->fLock);
		r->shared = me->surfaceMapFor(surface, size, &ret);
		if (r->shared != NULL) {
			r->shared->refs++;
			r->gpuBase = r->shared->gpuBase;
			r->gpuLength = r->shared->gpuLength;
		}
		IOLockUnlock(me->fLock);
		if (r->shared != NULL || ret != kIOReturnSuccess) {
			offset = surface->info.offset + (surface->info.planeCount > 0 ? surface->planes[plane].offset : 0);
			out->residentSize = size;
			break;
		}
		/* (no entry free: a mapping of its own, as before) */
		r->memory = surface->scanoutSlot > 0 ? IOSubMemoryDescriptor::withSubRange(surface->client, 0, size,
		    kIODirectionInOut) : IOSubMemoryDescriptor::withSubRange(surface->memory, PAGE_SIZE, size, kIODirectionInOut);
		if (r->memory == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		/*
		 * Mapped into the task only to give it a GPU address there (no processor address is answered), with the
		 * memory's own cache mode: a write-combined mapping changes the PAGES' attribute (pmap), so the display's and
		 * IOSurface's cached views of the surface went uncached too -- reading a frame cost ~150 ms (11-26). A scan-out
		 * slot's memory is the device's, write-combined already.
		 */
		r->map = r->memory->createMappingInTask(me->fTask, 0, kIOMapAnywhere |
		    (surface->scanoutSlot > 0 ? kIOMapWriteCombineCache : 0));
		if (r->map == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		r->gpuBase = r->map->getAddress();
		r->gpuLength = size;
		offset = surface->info.offset + (surface->info.planeCount > 0 ? surface->planes[plane].offset : 0);
		out->residentSize = size;
		{
			static int shown;

			if (shown++ < 24) {
				IOLog("IOGPU: surface %u (%ux%u, scan-out slot %d) as a resource: %llu bytes at 0x%llx, data at +0x%llx\n",
				    (uint32_t)a.a38, surface->info.width, surface->info.height, surface->scanoutSlot, size, r->gpuBase,
				    offset);
			}
		}
		break;
	}
	default:
		if (said++ < 8) {
			IOLog("IOGPU: a resource of kind 0x%x was asked for: not yet\n", a.kind);
		}
		ret = kIOReturnUnsupported;
		break;
	}

	IOLockLock(me->fLock);
	if (ret == kIOReturnSuccess && !me->addressSpace()) {
		ret = kIOReturnNoResources;
	}
	if (ret == kIOReturnSuccess && r->shared == NULL && !me->fGPU->mapIntoAddressSpace(me->fAS, r->gpuBase, r->memory, 0,
	    r->gpuLength, gpuFlags)) {
		IOLog("IOGPU: kind 0x%x at 0x%llx+%llu: the GPU's map refused it\n", a.kind, r->gpuBase, r->gpuLength);
		r->gpuLength = 0;               /* (nothing left mapped to undo) */
		ret = kIOReturnNoMemory;
	}
	if (ret == kIOReturnSuccess && !me->sharedSlot(&r->sharedSlot, &shared)) {
		IOLog("IOGPU: kind 0x%x: no shared slot left\n", a.kind);
		ret = kIOReturnNoMemory;
	}
	if (ret == kIOReturnSuccess) {
		for (id = 0; id < me->fResourceCount && me->fResources[id] != NULL; id++) {
		}
		if (id == me->fResourceCount) {
			uint32_t count = me->fResourceCount ? me->fResourceCount * 2 : 256;
			struct resource **grown = (struct resource **)IOMallocZero(count * sizeof(grown[0]));

			if (grown == NULL || count > (1u << 20)) {
				if (grown != NULL) {
					IOFree(grown, count * sizeof(grown[0]));
				}
				ret = kIOReturnNoResources;
			} else {
				if (me->fResources != NULL) {
					memcpy(grown, me->fResources, me->fResourceCount * sizeof(grown[0]));
					IOFree(me->fResources, me->fResourceCount * sizeof(grown[0]));
				}
				me->fResources = grown;
				me->fResourceCount = count;
			}
		}
	}
	if (ret != kIOReturnSuccess) {
		static int failed;

		if (failed++ < 24) {
			IOLog("IOGPU: a resource of kind 0x%x (%llu bytes, a38 0x%llx) refused: 0x%x\n", a.kind, a.size, a.a38, ret);
		}
		me->releaseResource(r);
		IOLockUnlock(me->fLock);
		bzero(out, args->structureOutputSize);
		return ret;
	}
	me->fResources[id] = r;
	IOLockUnlock(me->fLock);

	out->gpuAddress = r->gpuBase + offset;
	out->gpuBase = r->gpuBase;
	out->clientShared = shared;
	out->resourceID = id + 1;
	out->protectionOptions = a.protectionOptions;
	return kIOReturnSuccess;
}

/*
 * (fLock not held) The resources deleted while the GPU might still use them are freed once every submission the client
 * had made by then is done: IOGPU.framework deletes a resource as soon as its last Metal object goes, which can be
 * before the command buffers that use it have run (it never waits: "Resource freed while busy" is an assertion).
 */
void
IOGPUDeviceUserClient::freeDeferred(bool all)
{
	uint64_t oldest = all ? ~0ULL : fGPU->oldestPendingNumber(this);
	struct resource *done = NULL, **pp;

	IOLockLock(fLock);
	for (pp = &fDeferred; *pp != NULL;) {
		struct resource *r = *pp;

		if (r->deferredUntil <= oldest) {
			*pp = r->nextDeferred;
			r->nextDeferred = done;
			done = r;
		} else {
			pp = &r->nextDeferred;
		}
	}
	while (done != NULL) {
		struct resource *r = done;

		done = r->nextDeferred;
		releaseResource(r);
	}
	if (!all) {
		sweepSurfaceMaps(false);
	}
	IOLockUnlock(fLock);
}

/* (fLock held) the surface's mapping in this client, made if need be; NULL with *ret success: no entry free */
struct IOGPUDeviceUserClient::surfaceMap *
IOGPUDeviceUserClient::surfaceMapFor(MiniIOSurface *surface, uint64_t size, IOReturn *ret)
{
	struct surfaceMap *e = NULL;

	*ret = kIOReturnSuccess;
	for (uint32_t i = 0; i < kMaxSurfaceMaps; i++) {
		if (fSurfaceMaps[i].surface == surface && fSurfaceMaps[i].gpuLength >= size) {
			return &fSurfaceMaps[i];
		}
	}
	if (!addressSpace()) {
		*ret = kIOReturnNoResources;
		return NULL;
	}
	for (int pass = 0; pass < 2 && e == NULL; pass++) {
		if (pass == 1) {
			sweepSurfaceMaps(false);
		}
		for (uint32_t i = 0; i < kMaxSurfaceMaps && e == NULL; i++) {
			if (fSurfaceMaps[i].surface == NULL) {
				e = &fSurfaceMaps[i];
			}
		}
	}
	if (e == NULL) {
		static int said;

		if (said++ < 4) {
			IOLog("IOGPU: all %u surface mappings in use: surface %u mapped on its own\n", kMaxSurfaceMaps, surface->sid);
		}
		return NULL;
	}
	e->memory = surface->scanoutSlot > 0 ? IOSubMemoryDescriptor::withSubRange(surface->client, 0, size,
	    kIODirectionInOut) : IOSubMemoryDescriptor::withSubRange(surface->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (e->memory == NULL) {
		*ret = kIOReturnNoMemory;
		return NULL;
	}
	/* (in the task only for a GPU address there, with the memory's own cache mode: see the resource's notes) */
	e->map = e->memory->createMappingInTask(fTask, 0, kIOMapAnywhere |
	    (surface->scanoutSlot > 0 ? kIOMapWriteCombineCache : 0));
	if (e->map == NULL || !fGPU->mapIntoAddressSpace(fAS, e->map->getAddress(), e->memory, 0, size, 0)) {
		IOLog("IOGPU: surface %u: no mapping for the GPU\n", surface->sid);
		OSSafeReleaseNULL(e->map);
		OSSafeReleaseNULL(e->memory);
		*ret = kIOReturnNoMemory;
		return NULL;
	}
	surface->retain();
	MiniIOSurface *held = MiniIOSurfaceCopy(surface->sid);   /* (a holder, as the resource's own) */
	surface->release();
	if (held != surface) {
		MiniIOSurfaceDrop(held);
		fGPU->unmapFromAddressSpace(fAS, e->map->getAddress(), size);
		OSSafeReleaseNULL(e->map);
		OSSafeReleaseNULL(e->memory);
		*ret = kIOReturnNotFound;
		return NULL;
	}
	e->surface = held;
	e->gpuBase = e->map->getAddress();
	e->gpuLength = size;
	e->refs = 0;
	e->idleAfter = 0;
	return e;
}

/* (fLock held) mappings no resource uses, whose surface only this cache holds, that the GPU is past; all: every one */
void
IOGPUDeviceUserClient::sweepSurfaceMaps(bool all)
{
	uint64_t oldest = all ? ~0ULL : fGPU->oldestPendingNumber(this);

	for (uint32_t i = 0; i < kMaxSurfaceMaps; i++) {
		struct surfaceMap *e = &fSurfaceMaps[i];

		if (e->surface == NULL) {
			continue;
		}
		if (!all && (e->refs != 0 || e->surface->holders > 1 || e->idleAfter > oldest)) {
			continue;
		}
		if (e->gpuLength != 0 && fAS >= 0) {
			fGPU->unmapFromAddressSpace(fAS, e->gpuBase, e->gpuLength);
		}
		OSSafeReleaseNULL(e->map);
		OSSafeReleaseNULL(e->memory);
		MiniIOSurfaceDrop(e->surface);
		bzero(e, sizeof(*e));
	}
}

IOReturn
IOGPUDeviceUserClient::deleteResource(uint32_t id)
{
	struct resource *r = NULL;

	IOLockLock(fLock);
	if (id >= 1 && id <= fResourceCount) {
		r = fResources[id - 1];
		fResources[id - 1] = NULL;
	}
	if (r != NULL) {
		/* (its id is free again at once; its memory and GPU mapping wait for the submissions made so far) */
		fGPU->lockSubmissions();
		r->deferredUntil = fSubmissions + 1;
		fGPU->unlockSubmissions();
		r->nextDeferred = fDeferred;
		fDeferred = r;
	}
	IOLockUnlock(fLock);
	freeDeferred(false);
	/* (never kIOReturnBusy: IOGPU.framework asserts "Resource freed while busy" on it) */
	return r != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn
IOGPUDeviceUserClient::s_delete_resource(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	return ((IOGPUDeviceUserClient *)target)->deleteResource((uint32_t)args->scalarInput[0]);
}

/* ---- traps (IOConnectTrapN): IOGPU.framework's fast paths ---- */

#define T(fn) { NULL, (IOTrap)&IOGPUDeviceUserClient::fn }
const IOExternalTrap IOGPUDeviceUserClient::sTraps[14] = {
	T(t_submit_command_buffer),     /*  0 */
	T(t_delete_resource),           /*  1 */
	T(t_finish_resource_event),     /*  2 */
	T(t_set_resource_purgeable),    /*  3 */
	T(t_unsupported),               /*  4 group_add_resources */
	T(t_unsupported),               /*  5 group_remove_resources */
	T(t_unsupported),               /*  6 group_update_resources */
	T(t_unsupported),               /*  7 queue_set_resource_groups */
	T(t_unsupported),               /*  8 set_display_params */
	T(t_unsupported),               /*  9 signal_shared_event */
	T(t_unsupported),               /* 10 wait_shared_event */
	T(t_unsupported),               /* 11 signal_mtl_event */
	T(t_unsupported),               /* 12 wait_mtl_event */
	T(t_unsupported),               /* 13 check_background_app_role */
};
#undef T

IOExternalTrap *
IOGPUDeviceUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(sTraps) / sizeof(sTraps[0])) {
		return NULL;
	}
	*targetP = this;
	return (IOExternalTrap *)&sTraps[index];
}

IOReturn
IOGPUDeviceUserClient::t_unsupported(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6)
{
	return kIOReturnUnsupported;
}

IOReturn
IOGPUDeviceUserClient::t_delete_resource(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6)
{
	return deleteResource((uint32_t)(uintptr_t)p1);
}

/*
 * (id, event index): wait until the GPU is done with the resource. No GPU work goes through IOGPU yet, so a resource
 * is never busy: its events stay "complete".
 */
IOReturn
IOGPUDeviceUserClient::t_finish_resource_event(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6)
{
	uint32_t id = (uint32_t)(uintptr_t)p1;
	bool known;

	IOLockLock(fLock);
	known = id >= 1 && id <= fResourceCount && fResources[id - 1] != NULL;
	IOLockUnlock(fLock);
	return known ? kIOReturnSuccess : kIOReturnBadArgument;
}

/*
 * (11-28, as Apple's IOGPUDevice::set_resource_purgeable) the states are IOMemoryDescriptor's / MTLPurgeableState's:
 * 1 keep the current one, 2 non-volatile, 3 volatile, 4 empty; 5 make resident, 6 evict. The answer is the state before.
 * Our resources' memory is wired for the GPU's tables and is never reclaimed (as on a system under no pressure): the
 * state is kept as Metal set it and answered back. (Reclaiming volatile memory waits on unwired, faultable GPU memory.)
 */
IOReturn
IOGPUDeviceUserClient::purgeable(uint32_t id, uint32_t state, uint32_t *old)
{
	struct resource *r;

	if (id == 0 || state < 1 || state > 6) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	r = id <= fResourceCount ? fResources[id - 1] : NULL;
	if (r == NULL) {
		IOLockUnlock(fLock);
		return kIOReturnBadArgument;
	}
	*old = r->purgeable != 0 ? r->purgeable : 2;
	if (state >= 2 && state <= 4) {
		r->purgeable = (uint8_t)state;
	} else if (state == 5) {
		r->purgeable = 2;               /* (resident again: its contents as they are) */
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::t_set_resource_purgeable(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6)
{
	uint32_t old = 0;
	IOReturn ret = purgeable((uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2, &old);
	uint64_t answer = old;

	if (ret == kIOReturnSuccess && p3 != NULL && copyout(&answer, (user_addr_t)(uintptr_t)p3, sizeof(answer)) != 0) {
		return kIOReturnBadArgument;
	}
	return ret;
}

/* Selector 11: the same, the old state in the scalar out */
IOReturn
IOGPUDeviceUserClient::s_set_resource_purgeable(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	uint32_t old = 0;
	IOReturn ret = ((IOGPUDeviceUserClient *)target)->purgeable((uint32_t)args->scalarInput[0],
	    (uint32_t)args->scalarInput[1], &old);

	if (ret == kIOReturnSuccess) {
		args->scalarOutput[0] = old;
	}
	return ret;
}

/* Selector 10 (object, which of its two events): finishing a resource's event. Ours complete with the submission. */
IOReturn
IOGPUDeviceUserClient::s_finish_object_event(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint32_t id = (uint32_t)args->scalarInput[0];
	bool known;

	if (args->scalarInput[1] >= 2) {
		return kIOReturnBadArgument;
	}
	IOLockLock(me->fLock);
	known = id >= 1 && id <= me->fResourceCount && me->fResources[id - 1] != NULL;
	IOLockUnlock(me->fLock);
	return known ? kIOReturnSuccess : kIOReturnBadArgument;
}

/*
 * Selector 26 (queue; {priority, background}) -> the priority taken. As IOGPUCommandQueue::set_priority_and_background:
 * priorities 0-2 and 4, 5 (real-time) only with com.apple.private.gpu-realtime... else High (0); background 2 or 3.
 * Kept per queue; our hardware queue's priority was fixed when it was made (a CSF group's: changing it waits on the
 * Mali scheduler's priority update).
 */
IOReturn
IOGPUDeviceUserClient::s_set_priority_and_background(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	const uint32_t *in = (const uint32_t *)args->structureInput;
	uint32_t id = (uint32_t)args->scalarInput[0], priority, background;

	if (in == NULL || args->structureInputSize < 8) {
		return kIOReturnBadArgument;
	}
	priority = in[0];
	background = in[1];
	if (priority == 3 || priority > 5) {
		return kIOReturnBadArgument;
	}
	if (priority == 5 && !IOTaskHasEntitlement(me->fTask, "com.apple.private.gpu-realtime-priority")) {
		priority = 0;
	}
	if ((background & ~1u) != 2) {
		return kIOReturnBadArgument;
	}
	IOLockLock(me->fLock);
	if (id < 1 || id > kMaxQueues || !me->fQueues[id - 1].used) {
		IOLockUnlock(me->fLock);
		return kIOReturnBadArgument;
	}
	me->fQueues[id - 1].priority = priority;
	me->fQueues[id - 1].background = background;
	IOLockUnlock(me->fLock);
	args->scalarOutput[0] = priority;
	return kIOReturnSuccess;
}

/* Selector 27 (queue; {QoS level 0-4}): kept per queue (IOGPUCommandQueue::setQualityOfService) */
IOReturn
IOGPUDeviceUserClient::s_set_quality_of_service(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	const uint32_t *in = (const uint32_t *)args->structureInput;
	uint32_t id = (uint32_t)args->scalarInput[0];

	if (in == NULL || args->structureInputSize < 4 || in[0] > 4) {
		return kIOReturnBadArgument;
	}
	IOLockLock(me->fLock);
	if (id < 1 || id > kMaxQueues || !me->fQueues[id - 1].used) {
		IOLockUnlock(me->fLock);
		return kIOReturnBadArgument;
	}
	me->fQueues[id - 1].qos = in[0];
	IOLockUnlock(me->fLock);
	return kIOReturnSuccess;
}

/*
 * Selectors 33/34 (pid, role) / (pid) -> role: runningboardd tells the GPU driver each app's role as it changes (11-28:
 * ~64 calls in a boot's first 3 minutes). IOGPUDevice's own answer is "unsupported"; the GPU's driver (AGX on an
 * iPhone) keeps them, for its scheduling. Ours keeps the newest of 256 processes; a process not told about is 0.
 */
static struct { int pid; uint32_t role; } gAppRoles[256];
static uint32_t gAppRoleNext;
static IOLock *gAppRoleLock;

IOReturn
IOGPUDeviceUserClient::s_set_app_gpu_role(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	int pid = (int)args->scalarInput[0];
	uint32_t role = (uint32_t)args->scalarInput[1], i;

	if (pid <= 0) {
		return kIOReturnBadArgument;
	}
	if (gAppRoleLock == NULL) {
		IOLock *lock = IOLockAlloc();

		if (!OSCompareAndSwapPtr(NULL, lock, &gAppRoleLock)) {
			IOLockFree(lock);
		}
	}
	IOLockLock(gAppRoleLock);
	for (i = 0; i < 256 && gAppRoles[i].pid != pid; i++) {
	}
	if (i == 256) {
		i = gAppRoleNext++ % 256;
		gAppRoles[i].pid = pid;
	}
	gAppRoles[i].role = role;
	IOLockUnlock(gAppRoleLock);
	return kIOReturnSuccess;
}

IOReturn
IOGPUDeviceUserClient::s_get_app_gpu_role(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	int pid = (int)args->scalarInput[0];
	uint32_t role = 0;

	if (gAppRoleLock != NULL) {
		IOLockLock(gAppRoleLock);
		for (uint32_t i = 0; i < 256; i++) {
			if (gAppRoles[i].pid == pid) {
				role = gAppRoles[i].role;
				break;
			}
		}
		IOLockUnlock(gAppRoleLock);
	}
	args->scalarOutput[0] = role;
	return kIOReturnSuccess;
}

/*
 * Selector 35 (resource, a task's identity token port name): its memory charged to that task (IOGPUResource::
 * setOwnership): the graphics ledger of the task that really uses it -- a daemon allocating for an app.
 */
extern "C" kern_return_t task_id_token_port_name_to_task(mach_port_name_t name, task_t *taskp);

IOReturn
IOGPUDeviceUserClient::s_set_resource_owner_identity(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint32_t id = (uint32_t)args->scalarInput[0], name = (uint32_t)args->scalarInput[1];
	IOMemoryDescriptor *md = NULL;
	task_t task = TASK_NULL;
	IOReturn ret;

	if (name - 1 >= 0xfffffffeu) {
		return kIOReturnBadArgument;
	}
	IOLockLock(me->fLock);
	if (id >= 1 && id <= me->fResourceCount && me->fResources[id - 1] != NULL) {
		md = me->fResources[id - 1]->memory;
		if (md != NULL) {
			md->retain();
		}
	}
	IOLockUnlock(me->fLock);
	if (md == NULL) {
		return kIOReturnBadArgument;
	}
	if (task_id_token_port_name_to_task(name, &task) != KERN_SUCCESS) {
		ret = kIOReturnBadArgument;
	} else if (task == TASK_NULL) {
		ret = kIOReturnUnsupported;
	} else {
		ret = md->setOwnership(task, kIOMemoryLedgerTagGraphics, 0);
		task_deallocate(task);
	}
	md->release();
	return ret;
}

/*
 * Selector 54 (kind, argument) -> an assertion's port: kind 1 (keep the wired collector off) with
 * com.apple.private.iogpu.disable_wired_collector, else not permitted -- as IOGPUDevice::create_device_assertion. Our
 * GPU has no wired collector: an entitled caller is answered with no object (Apple's answer when it makes none).
 */
IOReturn
IOGPUDeviceUserClient::s_create_device_assertion(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;

	if (args->scalarInput[0] != 1 || !IOTaskHasEntitlement(me->fTask, "com.apple.private.iogpu.disable_wired_collector")) {
		return kIOReturnNotPermitted;
	}
	args->scalarOutput[0] = 0;
	return kIOReturnSuccess;
}

/* ---- command queues, notification queues, submission ---- */

/* (fLock held) the client's GPU address space, made on first use */
bool
IOGPUDeviceUserClient::addressSpace(void)
{
	if (fAS < 0) {
		fAS = fGPU->newAddressSpace();
	}
	return fAS >= 0;
}

/*
 * Selector 6: the process's path (0x400 bytes), +0x400 QoS level, +0x404 "OpenGL queue". Answer (queue id, global
 * trace id). A hardware queue of its own in the client's address space.
 */
IOReturn
IOGPUDeviceUserClient::s_new_command_queue(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint64_t *out = (uint64_t *)args->structureOutput;
	IOReturn ret = kIOReturnNoResources;
	uint32_t i;

	bzero(out, args->structureOutputSize);
	IOLockLock(me->fLock);
	for (i = 0; i < kMaxQueues && me->fQueues[i].used; i++) {
	}
	if (i < kMaxQueues && me->addressSpace()) {
		int hq = me->fGPU->newHardwareQueue(me->fAS, 2);

		if (hq >= 0) {
			me->fQueues[i].used = true;
			me->fQueues[i].hq = hq;
			me->fQueues[i].nq = 0;
			out[0] = i + 1;
			out[1] = IOGPU::nextObjectIDs(1);
			ret = kIOReturnSuccess;
		}
	}
	IOLockUnlock(me->fLock);
	return ret;
}

void
IOGPUDeviceUserClient::deleteQueue(uint32_t id)
{
	int hq;

	if (id < 1 || id > kMaxQueues || fLock == NULL || fGPU == NULL) {
		return;
	}
	IOLockLock(fLock);
	if (!fQueues[id - 1].used) {
		IOLockUnlock(fLock);
		return;
	}
	hq = fQueues[id - 1].hq;
	fQueues[id - 1].used = false;
	IOLockUnlock(fLock);
	fGPU->drainQueue(this, id);
	fGPU->freeHardwareQueue(hq);
}

IOReturn
IOGPUDeviceUserClient::s_delete_command_queue(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	((IOGPUDeviceUserClient *)target)->deleteQueue((uint32_t)args->scalarInput[0]);
	return kIOReturnSuccess;
}

/* Selector 14: (entries, entry size) -> (the queue's memory in the task, id): xnu's IOSharedDataQueue, as Apple's */
IOReturn
IOGPUDeviceUserClient::s_create_notificationqueue(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint64_t entries = args->scalarInput[0], size = args->scalarInput[1];
	struct { uint64_t address; uint32_t id, pad; } *out = (decltype(out))args->structureOutput;
	IOSharedDataQueue *q;
	IOMemoryDescriptor *md;
	IOMemoryMap *map;
	uint32_t i;

	bzero(out, args->structureOutputSize);
	if (entries < 1 || entries > 0x2000 || size < 1 || size > sizeof(IOGPU::notification)) {
		return kIOReturnBadArgument;
	}
	q = IOSharedDataQueue::withEntries((UInt32)entries, (UInt32)size);
	md = q != NULL ? q->getMemoryDescriptor() : NULL;
	map = md != NULL ? md->createMappingInTask(me->fTask, 0, kIOMapAnywhere) : NULL;
	OSSafeReleaseNULL(md);
	if (map == NULL) {
		OSSafeReleaseNULL(q);
		return kIOReturnNoMemory;
	}
	me->fGPU->lockSubmissions();
	for (i = 0; i < kMaxNotificationQueues && me->fNQueues[i].queue != NULL; i++) {
	}
	if (i < kMaxNotificationQueues) {
		me->fNQueues[i].queue = q;
		me->fNQueues[i].map = map;
		me->fNQueues[i].port = MACH_PORT_NULL;
	}
	me->fGPU->unlockSubmissions();
	if (i == kMaxNotificationQueues) {
		map->release();
		q->release();
		return kIOReturnNoResources;
	}
	out->address = map->getAddress();
	out->id = i + 1;
	return kIOReturnSuccess;
}

void
IOGPUDeviceUserClient::destroyNotificationQueue(uint32_t id)
{
	struct nqueue n;

	if (id < 1 || id > kMaxNotificationQueues || fGPU == NULL) {
		return;
	}
	fGPU->lockSubmissions();
	n = fNQueues[id - 1];
	bzero(&fNQueues[id - 1], sizeof(fNQueues[0]));
	if (n.queue != NULL) {
		n.queue->setNotificationPort(MACH_PORT_NULL);  /* (submissions still out hold the queue, not the port) */
	}
	fGPU->unlockSubmissions();
	if (n.port != MACH_PORT_NULL) {
		releaseNotificationPort(n.port);
	}
	OSSafeReleaseNULL(n.map);
	OSSafeReleaseNULL(n.queue);
}

IOReturn
IOGPUDeviceUserClient::s_destroy_notificationqueue(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	((IOGPUDeviceUserClient *)target)->destroyNotificationQueue((uint32_t)args->scalarInput[0]);
	return kIOReturnSuccess;
}

/* IOConnectSetNotificationPort(connection, 0, port, notification queue id): once a queue */
IOReturn
IOGPUDeviceUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	IOReturn ret = kIOReturnBadArgument;

	if (type != 0 || refCon < 1 || refCon > kMaxNotificationQueues) {
		return kIOReturnBadArgument;
	}
	fGPU->lockSubmissions();
	if (fNQueues[refCon - 1].queue != NULL && fNQueues[refCon - 1].port == MACH_PORT_NULL) {
		fNQueues[refCon - 1].port = port;
		fNQueues[refCon - 1].queue->setNotificationPort(port);
		ret = kIOReturnSuccess;
	}
	fGPU->unlockSubmissions();
	return ret;
}

IOReturn
IOGPUDeviceUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
	return registerNotificationPort(port, type, (io_user_reference_t)refCon);
}

IOReturn
IOGPUDeviceUserClient::s_set_notification_queue(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint64_t qid = args->scalarInput[0], nq = args->scalarInput[1];
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(me->fLock);
	if (qid >= 1 && qid <= kMaxQueues && me->fQueues[qid - 1].used && nq >= 1 && nq <= kMaxNotificationQueues) {
		me->fQueues[qid - 1].nq = (uint32_t)nq;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(me->fLock);
	return ret;
}

/*
 * One command buffer's arguments: IOGPU's common 0x30 bytes, then ours.
 *   +0x00 kernel-command shmem, +0x04 segment list, +0x08 sideband, +0x10 "scheduled" token, +0x18 "completed"
 *   token, +0x20 debug shmem; +0x30 the vendor's: the GPU address and length of the stream to run (none: no GPU work).
 * The kernel commands and the segment list (residency, events) are not read yet.
 */
struct IOGPUCommandBufferArgs {
	uint32_t kernelCommandsShmem, segmentListShmem, sidebandShmem, pad0c;
	uint64_t scheduledToken, completedToken;
	uint32_t debugShmem, pad24;
	uint64_t pad28;
	uint64_t stream;                /* +0x30 */
	uint32_t streamLength;          /* +0x38 */
	uint32_t streamFlags;           /* +0x3c */
};

/*
 * (fLock held) A resource segment list: {u64 tag; u32 segments; u32 size | bit31}, then per segment a 0x20-byte header
 * (+0x1c: its entry count) and 64-byte entries {u32 resourceID[6]; u32 sizeKB[6]; u16 usage[6]; u16 pad; u16 count}. The usage
 * bits are the vendor's (AkiosMetal: bit 0 read, bit 1 write). Every IOSurface resource written is marked busy for the
 * display (akios_surface_busy) from now until the command buffer completes -- how the display learns, as on an iPhone,
 * when the GPU has drawn a frame it is asked to show.
 */
void
IOGPUDeviceUserClient::resourceSegments(const uint8_t *list, uint32_t size, struct IOGPU::pending *p)
{
	uint32_t segments = *(const uint32_t *)(list + 8), off = 0x10;

	for (uint32_t sgm = 0; sgm < segments && off + 0x20 <= size; sgm++) {
		uint32_t entries = *(const uint32_t *)(list + off + 0x1c);

		off += 0x20;
		for (uint32_t e = 0; e < entries && off + 64 <= size; e++, off += 64) {
			const uint32_t *ids = (const uint32_t *)(list + off);
			const uint16_t *usage = (const uint16_t *)(list + off + 0x30);
			uint16_t count = *(const uint16_t *)(list + off + 0x3e);   /* (+0x3c: padding) */

			for (uint32_t k = 0; k < count && k < 6; k++) {
				struct resource *r;
				uint32_t sid;
				bool known = false;

				if (!(usage[k] & 2) || ids[k] < 1 || ids[k] > fResourceCount || (r = fResources[ids[k] - 1]) == NULL ||
				    r->surface == NULL) {
					continue;
				}
				sid = r->surface->sid;
				for (uint32_t b = 0; b < p->busySurfaces; b++) {
					known |= p->busySurface[b] == sid;
				}
				if (!known && p->busySurfaces < sizeof(p->busySurface) / sizeof(p->busySurface[0])) {
					p->busySurface[p->busySurfaces++] = sid;
					akios_surface_busy(sid, 1);
				}
			}
		}
	}
}

/*
 * A command buffer's kernel commands, as its segment list names them (records: control word at +0xc; bit 30 with
 * kind 1 a list of byte ranges of the kernel-command shmem, kind 2 the end; bit 30 clear a resource segment list of
 * low30 bytes; bit 31 the last). Each command is {u32 type, u32 size, payload}. Kept: 3 (signal a shared event) and
 * 0xb (the same with an agent mask) -- signalled when the GPU has passed the buffer; 4 (wait for one) -- the buffer
 * goes to the GPU once it is reached; 0xd (signal when scheduled) -- at once. The buffer runs as one stream, so its
 * waits hold back all of it and its signals come after all of it: later than Apple's, never too early.
 * The rest (0xf resource groups, 2 sleep, 8 purged, 9 protection, 0xe responsible tasks, ...) is not acted on yet.
 */
void
IOGPUDeviceUserClient::kernelCommands(const struct IOGPUCommandBufferArgs *a, struct IOGPU::pending *p)
{
	const uint8_t *seg, *kcmd;
	uint64_t segLen, kcmdLen, off = 0;
	static int said;

	IOLockLock(fLock);
	if (a->segmentListShmem < 1 || a->segmentListShmem > kMaxShmem || a->kernelCommandsShmem < 1 ||
	    a->kernelCommandsShmem > kMaxShmem || fShmem[a->segmentListShmem - 1].memory == NULL ||
	    fShmem[a->kernelCommandsShmem - 1].memory == NULL) {
		IOLockUnlock(fLock);
		return;
	}
	seg = (const uint8_t *)fShmem[a->segmentListShmem - 1].memory->getBytesNoCopy();
	segLen = fShmem[a->segmentListShmem - 1].memory->getLength();
	{
		/* (11-26: the first segment lists with a resource record, dumped: their layout checked against the parser) */
		static int dumped;

		bool any = false;

		for (uint32_t o = 0; o < 0x100 && o < segLen; o += 4) {
			any |= *(const uint32_t *)(seg + o) != 0;
		}
		if (dumped < 3 && segLen >= 0x100 && any) {
			dumped++;
			for (uint32_t o = 0; o < 0x100; o += 0x20) {
				const uint32_t *w = (const uint32_t *)(seg + o);

				IOLog("IOGPU: segment list +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", o, w[0], w[1], w[2], w[3],
				    w[4], w[5], w[6], w[7]);
			}
		}
	}
	kcmd = (const uint8_t *)fShmem[a->kernelCommandsShmem - 1].memory->getBytesNoCopy();
	kcmdLen = fShmem[a->kernelCommandsShmem - 1].memory->getLength();

	while (off + 0x10 <= segLen) {
		uint32_t ctl = *(const uint32_t *)(seg + off + 0xc), kind = ctl & 0x3fffffff;

		if (!(ctl & (1u << 30))) {
			if (kind < 0x10 || kind > segLen - off) {
				break;
			}
			resourceSegments(seg + off, kind, p);   /* (the surfaces it writes: busy until it completes) */
			off += kind;
		} else if (kind == 1) {
			uint32_t n = *(const uint32_t *)(seg + off + 8);

			if (off + 0x10 + 8ULL * n > segLen) {
				break;
			}
			for (uint32_t r = 0; r < n; r++) {
				uint32_t begin = *(const uint32_t *)(seg + off + 0x10 + 8 * r);
				uint32_t end = *(const uint32_t *)(seg + off + 0x14 + 8 * r);

				for (uint64_t c = begin; c + 8 <= end && end <= kcmdLen;) {
					uint32_t type = *(const uint32_t *)(kcmd + c), size = *(const uint32_t *)(kcmd + c + 4);

					if (size < 8 || (size & 3) || c + size > end) {
						break;
					}
					if ((type == 3 || type == 4 || type == 0xb || type == 0xd) && size >= 0x18) {
						uint32_t port = *(const uint32_t *)(kcmd + c + 8);
						uint64_t value = *(const uint64_t *)(kcmd + c + 0x10);
						IOSurfaceSharedEvent *e = IOSurfaceSharedEvent::fromPortName(fTask, port);

						if (e == NULL) {
							if (said++ < 8) {
								IOLog("IOGPU: kernel command %u names no shared event (port 0x%x)\n", type, port);
							}
						} else if (type == 0xd) {
							e->signal(value);
							e->release();
						} else if (type == 4 && p->waits < IOGPU::kMaxEventOps) {
							p->wait[p->waits].event = e;
							p->wait[p->waits++].value = value;
						} else if (type != 4 && p->signals < IOGPU::kMaxEventOps) {
							p->signal[p->signals].event = e;
							p->signal[p->signals++].value = value;
						} else {
							e->release();
						}
					}
					c += size;
				}
			}
			off += 0x10 + 8ULL * n;
		} else {
			break;                                  /* (kind 2: the end) */
		}
		if (ctl & (1u << 31)) {
			break;
		}
	}
	IOLockUnlock(fLock);
}

IOReturn
IOGPUDeviceUserClient::submit(uint32_t queueID, const uint8_t *args, uint32_t argsSize, uint32_t count)
{
	struct queue q;
	IOSharedDataQueue *nq = NULL;
	static int said;

	if (argsSize < 0x30 || argsSize > sizeof(IOGPUCommandBufferArgs)) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	q = queueID >= 1 && queueID <= kMaxQueues ? fQueues[queueID - 1] : (struct queue){ false, -1, 0 };
	IOLockUnlock(fLock);
	if (!q.used || q.nq == 0) {
		return kIOReturnBadArgument;
	}
	fGPU->lockSubmissions();
	nq = fNQueues[q.nq - 1].queue;
	if (nq != NULL) {
		nq->retain();
	}
	fGPU->unlockSubmissions();
	if (nq == NULL) {
		return kIOReturnBadArgument;
	}
	for (uint32_t i = 0; i < count; i++) {
		IOGPUCommandBufferArgs a;
		struct IOGPU::pending *p = (struct IOGPU::pending *)IOMallocZero(sizeof(*p));
		uint64_t start = IOGPU::nowNs();

		bzero(&a, sizeof(a));
		memcpy(&a, args + (size_t)i * argsSize, argsSize);
		if (p == NULL) {
			fGPU->lockSubmissions();
			IOGPU::notify(nq, a.scheduledToken, start, start, 0);
			IOGPU::notify(nq, a.completedToken, start, start, 1);
			fGPU->unlockSubmissions();
			continue;
		}
		retain();
		nq->retain();
		p->owner = this;
		p->nq = nq;
		p->queueID = queueID;
		p->hq = q.hq;
		p->stream = a.stream;
		p->length = a.streamLength;
		p->token = a.completedToken;
		p->startNs = start;
		kernelCommands(&a, p);
		fGPU->lockSubmissions();
		p->number = ++fSubmissions;     /* (numbered and queued under one lock: a delete sees both or neither) */
		IOGPU::notify(nq, a.scheduledToken, start, start, 0);
		fGPU->queuePending(p);
		fGPU->unlockSubmissions();
	}
	nq->release();
	freeDeferred(false);
	return kIOReturnSuccess;
}

/* Selector 25: (queue, flags (0), count, argument size), count * size bytes of arguments */
IOReturn
IOGPUDeviceUserClient::s_submit_command_buffers(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
	IOGPUDeviceUserClient *me = (IOGPUDeviceUserClient *)target;
	uint64_t count = args->scalarInput[2], size = args->scalarInput[3];

	args->scalarOutput[0] = 0;
	if (count == 0 || count > 256 || size == 0 || count * size != args->structureInputSize) {
		return kIOReturnBadArgument;
	}
	return me->submit((uint32_t)args->scalarInput[0], (const uint8_t *)args->structureInput, (uint32_t)size,
	           (uint32_t)count);
}

/* Trap 0: (queue, argument size, arguments, where a u32 status goes): one command buffer, IOGPU.framework's fast path */
IOReturn
IOGPUDeviceUserClient::t_submit_command_buffer(void *p1, void *p2, void *p3, void *p4, void *p5, void *p6)
{
	uint8_t args[sizeof(IOGPUCommandBufferArgs)];
	uint32_t size = (uint32_t)(uintptr_t)p2, status;
	IOReturn ret;

	if (size < 0x30 || size > sizeof(args) || copyin((user_addr_t)(uintptr_t)p3, args, size) != 0) {
		return kIOReturnBadArgument;
	}
	ret = submit((uint32_t)(uintptr_t)p1, args, size, 1);
	status = 0;
	if (p4 != NULL) {
		copyout(&status, (user_addr_t)(uintptr_t)p4, sizeof(status));
	}
	return ret;
}
