/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 *
 * FUXI (0093): frames are shown at up to 60 Hz by default (7 fast cores;
 * sweetfb_hz still overrides), the tick runs on absolute deadlines so the
 * copy time does not stretch the period, every 600 presented frames log
 * "SWEET FB: stats" (copy time, swaps, dropped swaps), and display off is a
 * single black fill (write-only: the panel memory is write-combined, the
 * read-modify-write fade read it back uncached). sweet_fb_display_off tells
 * the touch driver (bsd/dev/fuxi_touch.c) that the display is off.
 */
// Ported from UniXNU drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp (BSD-2-Clause-Patent)
// Optional demand-driven timer policy; see LICENSE.NativeMobileDisplay.
#include "SweetSurface.h"
#include "fuxi_unixnu_copy.h"
#include "fuxi_unixnu_display.h"
#if defined(FUXI)
extern "C" {
#include "../../../bsd/dev/fuxi_adreno.h"
void fuxi_perf_start(void);
void fuxi_perf_swap(void);
}
#endif

extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */
extern "C" { int sweet_fb_display_off; }        /* read by bsd/dev/fuxi_touch.c */

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

#if defined(FUXI)
extern void fuxi_backlight_publish(IOService *provider);
extern void fuxi_backlight_display_state(IOService *provider, bool on);
#endif

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
	bool          fPowerOff;      /* RequestPowerChange(0) */
#if defined(FUXI)
	bool          fBacklightPowerNotifications;
#endif
	uint32_t      fFadeStep;      /* fade-out steps done while off */
	uint64_t      fTickAbs;       /* vsync period, absolute time units */
	uint64_t      fNextTick;      /* next vsync deadline */
	uint64_t      fStatCopy, fStatCopyMax, fStatStart;
	uint32_t      fStatFrames, fStatSwaps, fStatDropped;
	uint32_t      fGpuFrames, fCpuFrames;
	uint64_t      fGpuTime, fCpuTime;
	bool          fUnixnuDiff, fUnixnuIdle, fTickArmed;
	bool          fShadowValid;
	void         *fShadow;
	uint64_t      fShadowBytes, fDiffStored, fDiffFull;
	uint32_t      fDiffFrames, fIdleStops, fIdleWakes;
	bool          fScanout, fScanoutLog, fScanoutHeld;
 uint32_t      fScanoutFrames, fScanoutGpu, fScanoutCpu, fScanoutDefers;
 uint64_t      fScanoutTime, fScanoutMax, fScanoutBytes, fScanoutPrepare;
 void scanoutLog(bool gpu, uint64_t bytes, uint64_t copy, uint64_t prepare);
 bool          fGpuPending;
	uint32_t      fGpuBusyTicks, fGpuFailed;

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn requestPowerChange(uint64_t state);
	static IOReturn powerChangeAction(OSObject *owner, void *state,
	    void *, void *, void *);
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void wakePresentLocked(void);
	bool presentWantedLocked(void);
	void free(void) APPLE_KEXT_OVERRIDE;
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);
#if defined(FUXI)
extern "C" void fuxi_display_kick(void);
#endif

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
#if defined(FUXI)
	uint32_t diff = 0, idle = 0;
	PE_parse_boot_argn("fuxi_unixnu_diff", &diff, sizeof(diff));
	PE_parse_boot_argn("fuxi_unixnu_idlefb", &idle, sizeof(idle));
	fUnixnuDiff = diff == 1;
	fUnixnuIdle = idle == 1;
	if (fUnixnuDiff) {
		fShadowBytes = (uint64_t)fWidth * fHeight * 4;
		if (fWidth > 16384 || fHeight > 16384 || !fShadowBytes || fShadowBytes > (64ULL << 20) ||
		    (fWidth & 1) || (fPixels & 7) || (fRowBytes & 7)) {
			fUnixnuDiff = false;
		} else {
			fShadow = IOMallocAligned((vm_size_t)fShadowBytes, 64);
			if (!fShadow) fUnixnuDiff = false;
		}
		IOLog("FUXI UNIXNU FB: differential copy %s (CPU full-panel BGRA only)\n",
		    fUnixnuDiff ? "enabled" : "unavailable");
	}
	if (fUnixnuIdle) IOLog("FUXI UNIXNU FB: demand timer enabled, software 60 Hz, panel power inherited\n");
#endif
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
#if defined(FUXI)
	uint32_t hz = 60;
#else
	uint32_t hz = 10;
#endif
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
#if defined(FUXI)
	nanoseconds_to_absolutetime(SWEET_FB_VSYNC_US * 1000ULL, &fTickAbs);
	fNextTick = mach_absolute_time() + fTickAbs;
	fTickArmed = true;
	fStatStart = mach_absolute_time();
	fVsync->wakeAtTime(kIOTimeOptionsWithLeeway, fNextTick, 0);
	IOLog("SWEET FB: present up to %u Hz (every %u ticks)\n", 60 / fFlushTicks, fFlushTicks);
#else
	fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);
#endif

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
#if defined(FUXI)
	int backlight = 0;
	PE_parse_boot_argn("fuxibacklight", &backlight, sizeof(backlight));
	fBacklightPowerNotifications = backlight == 2 || backlight == 3;
	fuxi_backlight_publish(this);
#endif
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);

#if defined(FUXI)
 uint32_t scanout = 0, check = 0, log = 0;
 PE_parse_boot_argn("fuxigpuscanout", &scanout, sizeof(scanout));
 PE_parse_boot_argn("fuxigpuscanoutcheck", &check, sizeof(check));
 PE_parse_boot_argn("fuxigpuscanoutlog", &log, sizeof(log));
 int quiet = 0;
 PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet));
 fScanout = scanout == 1 && quiet == 1;
 fScanoutLog = log == 1 || fScanout;
 if (scanout == 1 && !fScanout)
  IOLog("FUXI SCANOUT: unavailable, requires sweetfb_noconsole=1; existing present path\n");
 if (fScanout) { sweet_console_muted = 1; fConsolePaused = true; }
 fuxi_gpu_scanout_configure(fPixels, fScanout, check);
 fuxi_gpu_start(fPhys, fWidth, fHeight, fRowBytes);
	fuxi_perf_start();
#endif
	return true;
}

void
AppleCLCD::free(void)
{
	if (fShadow) IOFreeAligned(fShadow, (vm_size_t)fShadowBytes);
	IOMobileFramebuffer::free();
}

/* fLock serializes sleep/wake decisions with swaps, notifications and power requests. */
bool
AppleCLCD::presentWantedLocked(void)
{
	bool notify = false;
	for (unsigned int i = 0; i < fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fClients->getObject(i);
		if (client->fFunc[SWEET_FB_NOTIFY_VSYNC] && client->fPort != MACH_PORT_NULL) notify = true;
	}
	return fuxi_unixnu_tick_wanted(fPowerOff, fFadeStep == 0, fDirty, fGpuPending, notify);
}

void
AppleCLCD::wakePresentLocked(void)
{
#if defined(FUXI)
	if (!fUnixnuIdle || fTickArmed || !presentWantedLocked()) return;
	fTickArmed = true;
	fNextTick = mach_absolute_time() + fTickAbs;
	fVsync->wakeAtTime(kIOTimeOptionsWithLeeway, fNextTick, 0);
	if (++fIdleWakes == 1) IOLog("FUXI UNIXNU FB: timer wake\n");
#endif
}

static void
sweet_fb_rearm(AppleCLCD *fb, IOTimerEventSource *sender)
{
#if defined(FUXI)
 if (fb->fScanout && fuxi_gpu_scanout_owned()) {
  /* Poll ownership/fences at ~1 ms while active, independently of 60 Hz
   * notification deadlines. No extra notification or vsync count per poll. */
  IOLockLock(fb->fLock);
  if (fb->fUnixnuIdle && !fb->presentWantedLocked()) {
   fb->fTickArmed = false;
   IOLockUnlock(fb->fLock);
   return;
  }
  uint64_t now = mach_absolute_time();
  if (now >= fb->fNextTick)
   fb->fNextTick = fuxi_unixnu_next_tick(fb->fNextTick, fb->fTickAbs, now);
  uint64_t deadline = fb->fNextTick;
  bool active = fb->fGpuPending || (!fb->fScanoutHeld &&
      (fb->fPowerOff ? fb->fFadeStep == 0 : fb->fDirty));
  uint64_t poll = now + fb->fTickAbs/16;
  if (active && poll < deadline) deadline = poll;
  fb->fTickArmed = true;
  sender->wakeAtTime(kIOTimeOptionsWithLeeway, deadline, 0);
  IOLockUnlock(fb->fLock);
  return;
 }
	if (!fb->fUnixnuIdle) {
		/* Preserve the existing continuous-tick path when the option is absent. */
		fb->fNextTick = fuxi_unixnu_next_tick(fb->fNextTick, fb->fTickAbs, mach_absolute_time());
		sender->wakeAtTime(kIOTimeOptionsWithLeeway, fb->fNextTick, 0);
		return;
	}
	/* Keep scheduling and wakeups under the same lock: no lost idle->swap wake. */
	IOLockLock(fb->fLock);
	if (!fb->presentWantedLocked()) {
		fb->fTickArmed = false;
		if (++fb->fIdleStops == 1) IOLog("FUXI UNIXNU FB: timer idle\n");
		IOLockUnlock(fb->fLock);
		return;
	}
	uint64_t now = mach_absolute_time();
	fb->fNextTick = fuxi_unixnu_next_tick(fb->fNextTick, fb->fTickAbs, now);
	sender->wakeAtTime(kIOTimeOptionsWithLeeway, fb->fNextTick, 0);
	IOLockUnlock(fb->fLock);
#else
	(void)fb;
	sender->setTimeoutUS(SWEET_FB_VSYNC_US);
#endif
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
 bool vsyncTick = true;
#if defined(FUXI)
 if (fb->fScanout && fuxi_gpu_scanout_owned()) vsyncTick = now >= fb->fNextTick;
#endif
 uint64_t count = vsyncTick ? ++fb->fVsyncCount : fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

#if defined(FUXI)
 uint64_t elapsed = 0;
 int completed = sweet_surface_gpu_poll(&elapsed);
 bool gpuBusy = completed == 0;
 if (gpuBusy) fb->fGpuBusyTicks++;
 if (fb->fGpuPending && completed) {
  fb->fGpuPending = false;
  if (completed == 1) {
   fb->fGpuFrames++;
   fb->fGpuTime += elapsed;
   fb->scanoutLog(true, fb->fScanoutBytes, elapsed, fb->fScanoutPrepare);
   if (fuxi_gpu_scanout_publish() == 0) fuxi_display_kick();
  } else {
   fb->fGpuFailed++;
   /* Replay the newest swap through the CPU on this tick. */
   IOLockLock(fb->fLock); fb->fDirty = true; IOLockUnlock(fb->fLock);
  }
 }
#endif
	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		if (vsyncTick) client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
#if defined(FUXI)
 if (gpuBusy) { /* retain latest pending swap; no copy or black clear yet */
  IOLockUnlock(fb->fLock);
  sweet_fb_rearm(fb, sender);
  return;
 }
#endif
#if defined(FUXI)
 /* All framebuffer writers, including CPU fallback and black clear, share
  * the panel ownership guard. Keep the latest swap pending while draining. */
 if (fb->fScanout && (fb->fDirty || (fb->fPowerOff && fb->fFadeStep == 0))) {
  int writable = fuxi_gpu_scanout_acquire();
  if (writable != 1) {
   fb->fScanoutHeld = writable < 0;
   fb->fScanoutDefers++;
   IOLockUnlock(fb->fLock);
   sweet_fb_rearm(fb, sender);
   return;
  }
 }
#endif
	if (fb->fPowerOff) {
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
#if defined(FUXI)
		if (step == 0) {
			fb->fShadowValid = false;
			if (fb->fUnixnuDiff) {
				fuxi_unixnu_fill_pixels((volatile void *)fb->fPixels,
				    (uint64_t)fb->fRowBytes * fb->fHeight, fb->fRowBytes,
				    fb->fWidth, fb->fHeight, 0);
			} else {
				/* Plain stores, no DC ZVA (bzero) on the write-combined mapping. */
				for (uint32_t y = 0; y < fb->fHeight; y++) {
					volatile uint64_t *row = (volatile uint64_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
					for (uint32_t x = 0; x < fb->fWidth / 2; x++) {
						row[x] = 0;
					}
				}
			}
			__asm__ volatile ("dsb sy" ::: "memory");
            if (fb->fScanout && fuxi_gpu_scanout_publish() == 0) fuxi_display_kick();
		}
#else
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
		}
#endif
		sweet_fb_rearm(fb, sender);
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0 && (vsyncTick || fb->fFlushTicks == 1);
	uint8_t *rec = NULL;
	if (flush) {
		rec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		fb->present(rec);
		IOFreeData(rec, SWEET_FB_SWAP_REC_SIZE);
	}
	sweet_fb_rearm(fb, sender);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;
static uint64_t sweet_fb_swap_count;

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg sweetfb_hz,
 * default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
#if defined(FUXI)
	fuxi_perf_swap();
#endif
	IOLockLock(fLock);
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fStatSwaps++;
	if (fDirty) {
		fStatDropped++;         /* the previous frame was never shown */
	}
	fDirty = true;
	wakePresentLocked();
	IOLockUnlock(fLock);
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

void
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);
	uint64_t t0 = mach_absolute_time();
	bool gpuFrame = false, cpuFrame = false;
 uint64_t cpuBytes = 0, cpuTicks = 0;

	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			sweet_console_muted = 1;
		}
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			fShadowValid = false;
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			int copied = 0;
#if defined(FUXI)
			uint64_t copyStart = mach_absolute_time();
			/* CA's composed frame has one layer. Preserve the CPU path for
			 * multi-layer records rather than submit multiple fences per frame. */
			if ((mask & (mask-1)) == 0)
				copied = sweet_surface_gpu_copy(s, src, sx, sy, dx, dy, w, h);
            if (copied == 3 || (copied < 0 && fScanout)) {
             fScanoutDefers++;
             IOLockLock(fLock); fDirty = true; IOLockUnlock(fLock);
             s->release();
             return; /* busy producer/ring: do not scan-copy the source on CPU */
            }
			if (copied == 2) {
             fScanoutPrepare = mach_absolute_time() - copyStart;
             fScanoutBytes = (uint64_t)w * h * 4;
				fShadowValid = false;
				gpuFrame = true;
				fGpuPending = true;
			}
			copyStart = mach_absolute_time();
#endif
			bool diffCopied = false;
#if defined(FUXI)
			if (copied != 2 && fUnixnuDiff && sweet_console_muted &&
			    s->pixelFormat == SWEET_FB_FORMAT_BGRA && (mask & (mask - 1)) == 0 &&
			    dx == 0 && dy == 0 && w == (int32_t)fWidth && h == (int32_t)fHeight) {
				uint64_t offset = (uint64_t)s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4;
				uint64_t stored = fuxi_unixnu_copy_diff((volatile void *)fPixels,
				    (uint64_t)fRowBytes * fHeight, fRowBytes, (const void *)(src + offset),
				    s->memory->getLength() - offset, s->bytesPerRow, fWidth, fHeight,
				    fShadow, fShadowBytes, fShadowValid);
				if (stored != UINT64_MAX) {
					diffCopied = fShadowValid = true;
					fDiffStored += stored;
					fDiffFull += (uint64_t)fWidth * fHeight * 4;
					if (++fDiffFrames == 1 || fDiffFrames % 600 == 0)
						IOLog("FUXI UNIXNU FB: diff %u frames stored %llu of %llu bytes\n",
						    fDiffFrames, fDiffStored, fDiffFull);
				}
			}
#endif
			if (copied != 2 && !diffCopied) {
				fShadowValid = false;
#if defined(FUXI)
				if (fUnixnuDiff) {
					uint64_t offset = (uint64_t)s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4;
					uint64_t dest = (uint64_t)dy * fRowBytes + (uint64_t)dx * 4;
					fuxi_unixnu_copy_pixels((volatile void *)(fPixels + dest),
					    (uint64_t)fRowBytes * fHeight - dest, fRowBytes, (const void *)(src + offset),
					    s->memory->getLength() - offset, s->bytesPerRow, w, h);
				} else
#endif
				for (int32_t y = 0; y < h; y++) {
					memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
					    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
					    (size_t)w * 4);
				}
			}
#if defined(FUXI)
			if (copied != 2) {
				cpuFrame = true;
                uint64_t duration = mach_absolute_time() - copyStart;
				fCpuTime += duration;
                cpuTicks += duration;
                cpuBytes += (uint64_t)w * h * 4;
			}
#endif
		}
		s->release();
	}
#if defined(FUXI)
 if (!gpuFrame) {
  if (cpuFrame) scanoutLog(false, cpuBytes, cpuTicks, 0);
  if (fuxi_gpu_scanout_publish() == 0) fuxi_display_kick();
 }
	fCpuFrames += cpuFrame;
	uint64_t t1 = mach_absolute_time(), took = t1 - t0;
	fStatCopy += took;
	fStatCopyMax = took > fStatCopyMax ? took : fStatCopyMax;
	if (++fStatFrames >= 600) {
		uint64_t avg_ns, max_ns, span_ns;
		absolutetime_to_nanoseconds(fStatCopy / fStatFrames, &avg_ns);
		absolutetime_to_nanoseconds(fStatCopyMax, &max_ns);
		absolutetime_to_nanoseconds(t1 - fStatStart, &span_ns);
		IOLockLock(fLock);
		uint32_t swaps = fStatSwaps, dropped = fStatDropped;
		fStatSwaps = fStatDropped = 0;
		IOLockUnlock(fLock);
		IOLog("SWEET FB: stats %u frames in %llu ms, copy avg %llu us max %llu us, "
		    "%u swaps, %u not shown\n", fStatFrames, span_ns / 1000000, avg_ns / 1000,
		    max_ns / 1000, swaps, dropped);
		uint64_t gpu_ns, cpu_ns;
		absolutetime_to_nanoseconds(fGpuTime, &gpu_ns);
		absolutetime_to_nanoseconds(fCpuTime, &cpu_ns);
		struct fg_times split;
		sweet_surface_gpu_stats(&split);
		uint64_t ticks[] = {split.pin, split.lookup, split.map, split.clean,
		    split.submit, split.wait, split.unmap, split.cpu};
		uint64_t us[8];
		unsigned attempts = split.attempts;
		for (unsigned i = 0; i < 8; i++) {
			uint64_t ns;
			absolutetime_to_nanoseconds(ticks[i], &ns);
			us[i] = attempts ? ns / 1000 / attempts : 0;
		}
		IOLog("FUXI GPU: presents GPU %u CPU %u, GPU retire avg %llu us CPU avg %llu us, "
		    "split pin %llu lookup %llu map+TLB %llu clean %llu submit %llu wait %llu unmap %llu us, "
		    "GPU CPU-time %llu us, cache hit %u miss %u, sleeps %u, 2M %u 64K %u, busy ticks %u failed %u\n",
		    fGpuFrames, fCpuFrames, fGpuFrames ? gpu_ns / 1000 / fGpuFrames : 0ULL,
		    fCpuFrames ? cpu_ns / 1000 / fCpuFrames : 0ULL,
		    us[0], us[1], us[2], us[3], us[4], us[5], us[6], us[7],
		    split.hits, split.misses, split.sleeps, split.blocks, split.groups, fGpuBusyTicks, fGpuFailed);
		fGpuBusyTicks = fGpuFailed = 0;
		fGpuFrames = fCpuFrames = 0;
		fGpuTime = fCpuTime = 0;
		fStatFrames = 0;
		fStatCopy = fStatCopyMax = 0;
		fStatStart = t1;
	}
#endif
}

void
AppleCLCD::scanoutLog(bool gpu, uint64_t bytes, uint64_t copy, uint64_t prepare)
{
#if defined(FUXI)
 if (!fScanoutLog) return;
 uint64_t ns, prep;
 absolutetime_to_nanoseconds(copy, &ns);
 absolutetime_to_nanoseconds(prepare, &prep);
 ++fScanoutFrames;
 if (gpu) ++fScanoutGpu; else ++fScanoutCpu;
 fScanoutTime += ns;
 if (ns > fScanoutMax) fScanoutMax = ns;
 IOLog("FUXI SCANOUT: frame=%u path=%s bytes=%llu copy_us=%llu prepare_us=%llu\n",
     fScanoutFrames, gpu ? "GPU" : "CPU", bytes, ns/1000, prep/1000);
 if (fScanoutFrames % 120 == 0)
  IOLog("FUXI SCANOUT: summary frames=%u GPU=%u CPU=%u copy_avg_us=%llu copy_max_us=%llu deferred=%u\n",
      fScanoutFrames, fScanoutGpu, fScanoutCpu, fScanoutTime/1000/fScanoutFrames,
      fScanoutMax/1000, fScanoutDefers);
#else
 (void)gpu; (void)bytes; (void)copy; (void)prepare;
#endif
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	fFB->wakePresentLocked();
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

IOReturn
AppleCLCD::requestPowerChange(uint64_t state)
{
	void *argument = (void *)(uintptr_t)state;
#if defined(FUXI)
	/* Serialize modes 2/3 requests and interest delivery with each other.
	 * Never call arbitrary interest clients holding fLock. */
	if (fBacklightPowerNotifications)
		return fWorkLoop->runAction(&AppleCLCD::powerChangeAction, this, argument);
#endif
	return powerChangeAction(this, argument, NULL, NULL, NULL);
}

IOReturn
AppleCLCD::powerChangeAction(OSObject *owner, void *state, void *, void *, void *)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	bool off = state == NULL;
	IOLockLock(fb->fLock);
	if (off != fb->fPowerOff) {
		fb->fPowerOff = off;
		fb->fFadeStep = 0;
		fb->fDirty = true;     /* on: redraw the latest frame */
	}
	sweet_fb_display_off = off ? 1 : 0;
	fb->wakePresentLocked();
	IOLockUnlock(fb->fLock);
	IOLog("SWEET FB: display %s\n", off ? "off" : "on");
#if defined(FUXI)
	if (fb->fBacklightPowerNotifications) fuxi_backlight_display_state(fb, !off);
#endif
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			fFB->wakePresentLocked();
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			ret = fFB->requestPowerChange(args->scalarInput[0]);
		}
		break;
	case kSweetFBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */
