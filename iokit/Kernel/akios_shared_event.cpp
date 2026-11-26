/* AkIOS: IOSurfaceSharedEvent (akios_shared_event.h) */
#include "akios_shared_event.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(IOSurfaceSharedEvent, OSObject);

static IOLock *gEventLock;              /* every event's maps and waits (signals are rare enough for one lock) */
static volatile SInt64 gEventTraceIDs = 1;

IOSurfaceSharedEvent *
IOSurfaceSharedEvent::create(void)
{
	IOSurfaceSharedEvent *e;

	if (gEventLock == NULL) {
		IOLock *lock = IOLockAlloc();

		if (lock == NULL || !OSCompareAndSwapPtr(NULL, lock, (void * volatile *)&gEventLock)) {
			if (lock != NULL) {
				IOLockFree(lock);
			}
		}
	}
	e = OSTypeAlloc(IOSurfaceSharedEvent);
	if (e == NULL || !e->init()) {
		OSSafeReleaseNULL(e);
		return NULL;
	}
	e->fPage = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
	    page_size, page_size);
	if (e->fPage == NULL) {
		e->release();
		return NULL;
	}
	bzero(e->fPage->getBytesNoCopy(), page_size);
	e->fValue = (volatile uint64_t *)e->fPage->getBytesNoCopy();
	e->fTraceID = (uint64_t)OSIncrementAtomic64(&gEventTraceIDs);
	return e;
}

void
IOSurfaceSharedEvent::free(void)
{
	for (unsigned i = 0; i < kMaxMaps; i++) {
		OSSafeReleaseNULL(fMaps[i].map);
	}
	OSSafeReleaseNULL(fPage);
	OSObject::free();
}

void
IOSurfaceSharedEvent::signal(uint64_t value)
{
	IOLockLock(gEventLock);
	*fValue = value;
	IOLockWakeup(gEventLock, this, false);
	IOLockUnlock(gEventLock);
}

IOReturn
IOSurfaceSharedEvent::wait(uint64_t value, uint32_t timeoutMS)
{
	uint64_t deadline = 0;
	IOReturn ret = kIOReturnSuccess;

	if (*fValue >= value) {
		return kIOReturnSuccess;
	}
	if (timeoutMS == 0) {
		return kIOReturnTimeout;
	}
	if (timeoutMS != ~0u) {
		clock_interval_to_deadline(timeoutMS, kMillisecondScale, &deadline);
	}
	IOLockLock(gEventLock);
	while (*fValue < value) {
		int r = deadline ? IOLockSleepDeadline(gEventLock, this, deadline, THREAD_ABORTSAFE) :
		    IOLockSleep(gEventLock, this, THREAD_ABORTSAFE);

		if (r == THREAD_TIMED_OUT) {
			ret = *fValue >= value ? kIOReturnSuccess : kIOReturnTimeout;
			break;
		}
		if (r == THREAD_INTERRUPTED) {
			ret = kIOReturnAborted;
			break;
		}
	}
	IOLockUnlock(gEventLock);
	return ret;
}

mach_vm_address_t
IOSurfaceSharedEvent::addressInTask(task_t task)
{
	mach_vm_address_t address = 0;
	int free = -1;

	IOLockLock(gEventLock);
	for (unsigned i = 0; i < kMaxMaps; i++) {
		if (fMaps[i].map != NULL && fMaps[i].task == task) {
			address = fMaps[i].map->getAddress();
			break;
		}
		if (fMaps[i].map == NULL && free < 0) {
			free = (int)i;
		}
	}
	if (address == 0 && free >= 0) {
		IOMemoryMap *map = fPage->createMappingInTask(task, 0, kIOMapAnywhere | kIOMapReadOnly);

		if (map != NULL) {
			fMaps[free].task = task;
			fMaps[free].map = map;
			address = map->getAddress();
		}
	}
	IOLockUnlock(gEventLock);
	return address;
}

IOSurfaceSharedEvent *
IOSurfaceSharedEvent::fromPortName(task_t task, mach_port_name_t name)
{
	OSObject *o = NULL;
	IOSurfaceSharedEvent *e;

	if (name == 0 || IOUserClient::copyObjectForPortNameInTask(task, name, &o) != kIOReturnSuccess) {
		return NULL;
	}
	e = OSDynamicCast(IOSurfaceSharedEvent, o);
	if (e == NULL) {
		OSSafeReleaseNULL(o);
	}
	return e;
}
