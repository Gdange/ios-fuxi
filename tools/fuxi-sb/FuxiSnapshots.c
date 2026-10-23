/*
 * MIT License
 *
 * Copyright (c) 2026 OOSEX GANG
 * Copyright (c) 2026 ios-fuxi contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/*
 * Ported from HackiOS hackios-xnu-userland userland/src/inject_sb.c (MIT):
 * snapshot_output_format() and the method replacement it is installed with.
 *
 * libfuxi-sb.dylib: loaded into SpringBoard only, through DYLD_INSERT_LIBRARIES in its launchd
 * job (tools/fuxi-sys-root.py --sb-snapshots; dyld honours it because H1's AMFI policy answers
 * request 0x5a with every dyld feature allowed). It changes one thing.
 *
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] (SplashBoard) chooses the file
 * format of the picture SpringBoard keeps of an application that leaves the screen (for the
 * switcher and the next launch): 0 = PNG, 1 and 2 = ATX (an ASTC texture), 3 = JPEG. The iPhone X
 * gets ATX, and iOS compresses it on the CPU: HackiOS measured seconds per picture on their
 * phone, during which the application waits for the answer, until the scene-update watchdog
 * kills it ("no application survived going to the background"). JPEG takes a fraction of a
 * second; it has no transparency, so a picture that needs it becomes a PNG (what iOS itself does
 * on a device without ASTC). The selector and -[XBApplicationSnapshot isImageOpaque] are both in
 * the iOS 16.7 (20H392) shared cache. Unlike HackiOS, the implementation is not checked against
 * an address of one build: it is found by name, and left alone if it is not there.
 *
 * Boot-arg fuxisbjpeg=0 leaves SpringBoard unchanged without a root edit (for A/B runs).
 */
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3
#define SNAPSHOT_CLASS                  "XBApplicationSnapshotManifestImpl"
#define SNAPSHOT_SELECTOR               "_outputFormatForSnapshot:"
#define SNAPSHOT_SAID                   8       /* the first decisions are logged */

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		(void)write(2, line, (size_t)n);
	}
}

static unsigned snapshot_said;
static int hooked;                      /* 0 not yet, 1 replaced, -1 given up */
static IMP original_output_format;

/* +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] */
static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");
	long format = SNAPSHOT_FORMAT_PNG;

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		format = SNAPSHOT_FORMAT_JPEG;
	}
	if (__atomic_fetch_add(&snapshot_said, 1, __ATOMIC_RELAXED) < SNAPSHOT_SAID) {
		say("fuxi: SpringBoard: application snapshot as %s\n", format == SNAPSHOT_FORMAT_JPEG ? "JPEG" : "PNG");
	}
	return format;
}

/* Does kern.bootargs carry WORD=0? (fuxisbjpeg=0: leave SpringBoard alone) */
static bool
boot_arg_off(const char *word)
{
	char args[1024];
	size_t size = sizeof(args) - 1, length = strlen(word);
	const char *p;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return false;
	}
	args[size < sizeof(args) ? size : sizeof(args) - 1] = '\0';
	for (p = args; (p = strstr(p, word)) != NULL; p += length) {
		if ((p == args || p[-1] == ' ') && p[length] == '=' && p[length + 1] == '0' &&
		    (p[length + 2] == '\0' || p[length + 2] == ' ')) {
			return true;
		}
	}
	return false;
}

/* Replace the class method once its class is there. True when done (either way). */
static bool
try_hook(void)
{
	Class cls;
	Method method;

	if (__atomic_load_n(&hooked, __ATOMIC_ACQUIRE) != 0) {
		return true;
	}
	cls = objc_getClass(SNAPSHOT_CLASS);
	if (cls == NULL) {
		return false;                   /* SplashBoard not loaded yet */
	}
	method = class_getClassMethod(cls, sel_registerName(SNAPSHOT_SELECTOR));
	if (method == NULL) {
		__atomic_store_n(&hooked, -1, __ATOMIC_RELEASE);
		say("fuxi: SpringBoard: +[%s %s] not found; application snapshots NOT changed\n", SNAPSHOT_CLASS,
		    SNAPSHOT_SELECTOR);
		return true;
	}
	original_output_format = method_setImplementation(method, (IMP)snapshot_output_format);
	__atomic_store_n(&hooked, 1, __ATOMIC_RELEASE);
	say("fuxi: SpringBoard: application snapshots as JPEG (opaque) or PNG, not ATX\n");
	return true;
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	(void)try_hook();
}

__attribute__((constructor)) static void
fuxi_sb_init(void)
{
	const char *name = getprogname();

	if (name == NULL || strcmp(name, "SpringBoard") != 0) {
		return;
	}
	if (boot_arg_off("fuxisbjpeg")) {
		say("fuxi: SpringBoard: fuxisbjpeg=0, application snapshots unchanged\n");
		return;
	}
	if (!try_hook()) {
		/* called at once for every image already there, then for each one added */
		_dyld_register_func_for_add_image(image_added);
	}
}
