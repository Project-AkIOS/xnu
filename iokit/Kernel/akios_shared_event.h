/*
 * AkIOS: IOSurfaceSharedEvent, the kernel's half of MTLSharedEvent (IOSurface.framework's IOSurfaceSharedEvent).
 *
 * A u64 value in a page of its own, mapped read-only into every task that holds the event; the task reads it
 * there. Its port is an IOKit object port (IOUserClient::copyPortNameForObjectInTask), the name programs pass
 * around and put into kernel commands and swaps. Setting the value wakes the waiters (IOSurface trap 6, the GPU's
 * deferred submissions, the display's swaps).
 */
#ifndef _IOKIT_AKIOS_SHARED_EVENT_H
#define _IOKIT_AKIOS_SHARED_EVENT_H

#include <libkern/c++/OSObject.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include <IOKit/IOUserClient.h>

/*
 * (11-27) A task's port for shared-event notifications (Apple's IOSurfaceSharedEventNotificationPort): the async reference
 * IOSurface's add_shared_event_notify_port was called with, known by its refcon (the third word), as remove takes it.
 */
class AkiosEventNotifyPort : public OSObject
{
	OSDeclareDefaultStructors(AkiosEventNotifyPort);
	OSAsyncReference64 fRef;
public:
	static AkiosEventNotifyPort *withReference(const io_user_reference_t *ref);
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	uint64_t refcon(void) const { return fRef[kIOAsyncCalloutRefconIndex]; }
	/* Apple's message: { b, a, value's low word, value's high word } (IOSurfaceSharedEventNotificationPort::sendNotification) */
	void send(uint64_t value, uint64_t a, uint64_t b);
};

class IOSurfaceSharedEvent : public OSObject
{
	OSDeclareDefaultStructors(IOSurfaceSharedEvent);

	enum { kMaxMaps = 32 };
	IOBufferMemoryDescriptor *fPage;
	volatile uint64_t *fValue;
	struct { task_t task; IOMemoryMap *map; } fMaps[kMaxMaps];
	uint64_t fTraceID;
	enum { kMaxNotes = 32 };
	struct { AkiosEventNotifyPort *port; uint64_t value, a, b; } fNotes[kMaxNotes];   /* (11-27) waiting to be sent */
	void deliverDue(void);

public:
	static IOSurfaceSharedEvent *create(void);
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	uint64_t value(void) const { return *fValue; }
	uint64_t traceID(void) const { return fTraceID; }
	void signal(uint64_t value);
	/* until the value is at least `value`: 0 none (a look only), ~0u forever; kIOReturnTimeout if not reached */
	IOReturn wait(uint64_t value, uint32_t timeoutMS);
	/* where the value is in the task (a mapping made on first use) */
	mach_vm_address_t addressInTask(task_t task);

	/* (11-27) send PORT {value, a, b} once the value is at least VALUE (at once if it is): IOSurface notify_shared_event */
	IOReturn notifyAt(AkiosEventNotifyPort *port, uint64_t value, uint64_t a, uint64_t b);
	/* the event a task's port name stands for (held), or NULL */
	static IOSurfaceSharedEvent *fromPortName(task_t task, mach_port_name_t name);
};

#endif
