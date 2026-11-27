/* AkIOS: IOSurfaceSharedEvent (akios_shared_event.h) */
#include "akios_shared_event.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(IOSurfaceSharedEvent, OSObject);
OSDefineMetaClassAndStructors(AkiosEventNotifyPort, OSObject);

/* (IOUserClient's async-reference helpers are protected statics: reached through a subclass that is never made) */
class AkiosAsync : public IOUserClient
{
public:
	static void set(OSAsyncReference64 ref, mach_port_t port, mach_vm_address_t callback, io_user_reference_t refcon)
	{
		IOUserClient::setAsyncReference64(ref, port, callback, refcon);
	}
	static IOReturn send(OSAsyncReference64 ref, IOReturn result, io_user_reference_t *args, UInt32 count)
	{
		return IOUserClient::sendAsyncResult64(ref, result, args, count);
	}
};

AkiosEventNotifyPort *
AkiosEventNotifyPort::withReference(const io_user_reference_t *ref)
{
	AkiosEventNotifyPort *p;

	if (ref == NULL || ref[0] == 0 || (p = OSTypeAlloc(AkiosEventNotifyPort)) == NULL) {
		return NULL;
	}
	if (!p->init()) {
		p->release();
		return NULL;
	}
	AkiosAsync::set(p->fRef, (mach_port_t)(uintptr_t)ref[0], ref[kIOAsyncCalloutFuncIndex],
	    ref[kIOAsyncCalloutRefconIndex]);
	return p;
}

void
AkiosEventNotifyPort::free(void)
{
	if (fRef[0] != 0) {
		IOUserClient::releaseAsyncReference64(fRef);
	}
	OSObject::free();
}

void
AkiosEventNotifyPort::send(uint64_t value, uint64_t a, uint64_t b)
{
	io_user_reference_t args[4] = { b, a, value, value >> 32 };

	AkiosAsync::send(fRef, kIOReturnSuccess, args, 4);
}

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
	for (unsigned i = 0; i < kMaxNotes; i++) {
		OSSafeReleaseNULL(fNotes[i].port);
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
	deliverDue();
}

/* (11-27) the notifications whose value is reached, sent (outside the lock) and forgotten */
void
IOSurfaceSharedEvent::deliverDue(void)
{
	struct { AkiosEventNotifyPort *port; uint64_t value, a, b; } due[kMaxNotes];
	unsigned n = 0;
	uint64_t now;

	IOLockLock(gEventLock);
	now = *fValue;
	for (unsigned i = 0; i < kMaxNotes; i++) {
		if (fNotes[i].port != NULL && now >= fNotes[i].value) {
			due[n].port = fNotes[i].port;
			due[n].value = now;
			due[n].a = fNotes[i].a;
			due[n].b = fNotes[i].b;
			n++;
			fNotes[i].port = NULL;
		}
	}
	IOLockUnlock(gEventLock);
	for (unsigned i = 0; i < n; i++) {
		due[i].port->send(due[i].value, due[i].a, due[i].b);
		due[i].port->release();
	}
}

IOReturn
IOSurfaceSharedEvent::notifyAt(AkiosEventNotifyPort *port, uint64_t value, uint64_t a, uint64_t b)
{
	IOReturn ret = kIOReturnNoMemory;

	if (port == NULL) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gEventLock);
	for (unsigned i = 0; i < kMaxNotes; i++) {
		if (fNotes[i].port == NULL) {
			port->retain();
			fNotes[i].port = port;
			fNotes[i].value = value;
			fNotes[i].a = a;
			fNotes[i].b = b;
			ret = kIOReturnSuccess;
			break;
		}
	}
	IOLockUnlock(gEventLock);
	deliverDue();                   /* (reached already: sent now) */
	return ret;
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
