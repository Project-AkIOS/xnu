/*
 * AHSFirmwareReadyNotifier: "the haptics are ready", as Darwin waits for it.
 *
 * On Apple's phones the haptics driver (AppleHapticsSupport) publishes a service of this class once the actuator's
 * firmware runs. audiomxd's haptic server (AudioToolbox, HapticServerManager::Initialize) looks for it with
 * IOServiceGetMatchingService(IOServiceMatching("AHSFirmwareReadyNotifier")), and when there is none it registers for
 * the service's first match and waits -- with no timeout and nothing above the info log level. Until it comes,
 * com.apple.audio.hapticd (in audiomxd) never answers: every CoreHaptics client's first call times out, the system
 * sounds' vibrations fail ("AHAP playback ... not supported"), and backboardd's button resolver (Haptics.framework)
 * waits forever for "device haptic asset slot count" on a workloop that its virtual display service shares -- so
 * the lock screen's hello animation and Setup's screen recording, which need that display, hang (11-23).
 *
 * Here the haptics are the audio plug-in's "Actuator" device (userland/src/audio_plugin.c): what Darwin plays on it
 * goes to /dev/hakios-haptic, and a device's motor driver plays it only when its boot-arg asks for it (haptic=N;
 * without one nothing moves, whatever Darwin plays). So the "firmware" is ready as soon as the kernel is: the service
 * is published at boot, on every device, with no properties -- audiomxd only looks for its presence.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

extern "C" void hakios_haptics_ready_init(void);

class AHSFirmwareReadyNotifier : public IOService
{
	OSDeclareDefaultStructors(AHSFirmwareReadyNotifier);
};

OSDefineMetaClassAndStructors(AHSFirmwareReadyNotifier, IOService);

void
hakios_haptics_ready_init(void)
{
	AHSFirmwareReadyNotifier * made = OSTypeAlloc(AHSFirmwareReadyNotifier);
	IOService * root = IOService::getServiceRoot();

	if (made == NULL || root == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		IOLog("AHSFirmwareReadyNotifier: cannot be made: Darwin's haptic server will wait for it\n");
		return;
	}
	if (!made->attach(root)) {
		made->release();
		return;
	}
	made->setName("AHSFirmwareReadyNotifier");
	if (!made->start(root)) {
		made->detach(root);
		made->release();
		return;
	}
	made->registerService();
	made->release();        /* (the registry keeps it) */
}
