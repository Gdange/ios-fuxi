#!/usr/bin/env python3
"""Backlight probe: patch sync, gates, parser and logical power interests."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-backlight'
source = (SRC / 'FuxiBacklight.cpp').read_text()
fb = (ROOT / 'xnu/fuxi-unixnu/SweetFramebuffer.cpp').read_text()


def function(name, code=source):
    start = code.index(name + '(')
    # Use the definition, not a class declaration.
    start = code.index('{', start)
    depth, end = 1, start + 1
    while depth:
        depth += (code[end] == '{') - (code[end] == '}')
        end += 1
    return code[start:end]


subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-backlight.py'), '--check'], check=True)
subprocess.run(['python3', str(ROOT / 'tools/sync-fuxi-unixnu.py'), '--check'], check=True)
assert source.startswith('/*') and '#if defined(FUXI)' in source
gate = function('fuxi_backlight_publish')
assert gate.index('PE_parse_boot_argn("fuxibacklight"') < gate.index('if (!fbl_enabled(present, enable)) return;')
assert gate.index('return;') < gate.index('new FuxiBacklight') < gate.index('service->attach(provider)')
start = function('FuxiBacklight::start')
assert start.index('if (!fbl_enabled(present, enable)) return false;') < start.index('IOService::start(provider)')
assert start.index('setProperty("backlight-control", kOSBooleanTrue)') < start.index('registerService();')
assert start.index('publishBrightness(fRequested)') < start.index('registerService();')
assert fb.index('fuxi_backlight_publish(this);') < fb.index('\n\tregisterService();')
assert 'fBacklightPowerNotifications = backlight == 2 || backlight == 3;' in fb
assert 'runAction(&AppleCLCD::powerChangeAction, this, argument)' in fb
power = function('AppleCLCD::powerChangeAction', fb)
assert power.index('IOLockUnlock(fb->fLock);') < power.index('fuxi_backlight_display_state(fb, !off)')
notify = function('fuxi_backlight_display_state')
assert notify.index('if (!fbl_power_notifications(present, enable)) return;') < notify.index('getClientIterator()')
assert notify.index('fbl_withhold_on(present, enable, on)') < notify.index('getClientIterator()')
assert 'OFF-only interest experiment enabled; ON withheld' in start
assert 'on ? kIOMessageDeviceHasPoweredOn : kIOMessageDeviceWillPowerOff' in notify
assert 'service->messageClients(message)' in notify
assert 'class FuxiBacklight : public IODisplay' in source
for key in ('min', 'max', 'value'):
    assert f'brightness->setObject("{key}"' in source
for signature in ('getProperty(const OSSymbol *key) const',
                  'setProperty(const OSSymbol *key, OSObject *value)',
                  'serializeProperties(OSSerialize *s) const',
                  'setProperties(OSObject *properties)', 'newUserClient(task_t task',
                  'externalMethod(uint32_t selector'):
    assert signature in source, signature
assert 'dictionaryWithProperties()' in function('FuxiBacklight::serializeProperties')
assert 'args->scalarInput[i]' in function('FuxiBacklightUserClient::externalMethod')
assert 'return kIOReturnUnsupported;' in function('FuxiBacklightUserClient::externalMethod')
assert 'fRequested = (uint32_t)requested;' in source
assert 'IOLockLock(fLock);' in source and 'IOLockUnlock(fLock);' in source
# Hardware-free translation unit; logical notifications never drive scanout.
code = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
for forbidden in ('ml_io_map', 'IOMemoryDescriptor', 'IOMemoryMap', 'writeRegister',
                  'fuxi_display_kick', 'sweet_fb_display_off', 'setPowerState',
                  'volatile uint', '__asm__', '0x51'):
    assert forbidden not in code, forbidden
ci = (ROOT / 'tools/ci/build.sh').read_text()
assert ci.index('python3 tools/test-fuxi-backlight.py') < ci.index('if [[ $target == kernel-wlan6g ]]')

# Compile the real setProperties body with a small dictionary/number adapter.
# Covers the public flat input, nested diagnostic inputs, malformed data,
# exact endpoints, failed publication, immutable-snapshot update ordering.
MODEL = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <functional>
#include <cstdarg>
#include <cstdio>
#define FUXI 1
#include "fuxi_backlight_logic.h"
struct OSObject { virtual ~OSObject() = default; };
struct OSNumber : OSObject {
    uint64_t value;
    explicit OSNumber(uint64_t v) : value(v) {}
    uint64_t unsigned64BitValue() { return value; }
};
struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> values;
    OSObject *getObject(const char *key) {
        auto p = values.find(key); return p == values.end() ? nullptr : p->second;
    }
};
#define OSDynamicCast(t, p) dynamic_cast<t *>(p)
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnUnsupported, kIOReturnNoMemory };
enum { BL_PROPERTIES, BL_BRIGHTNESS, BL_POWER };
using IOReturn = int;
using UInt32 = uint32_t;
constexpr UInt32 kIOMessageDeviceHasPoweredOn = 0xe0000230;
constexpr UInt32 kIOMessageDeviceWillPowerOff = 0xe0000210;
static bool bl_log(unsigned, uint32_t *event) { *event = 1; return true; }
static void bl_value(const char *, uint32_t, const char *, OSObject *) {}
static std::vector<std::string> logs;
static void IOLog(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logs.emplace_back(buffer);
}
static unsigned locked;
static void IOLockLock(void *) { assert(!locked); ++locked; }
static void IOLockUnlock(void *) { assert(locked == 1); --locked; }
static bool boot_present;
static int boot_value;
static bool PE_parse_boot_argn(const char *name, void *value, unsigned size) {
    assert(std::string(name) == "fuxibacklight" && size == sizeof(int));
    if (boot_present) *static_cast<int *>(value) = boot_value;
    return boot_present;
}
struct OSIterator {
    std::vector<OSObject *> children;
    unsigned next = 0, releases = 0;
    OSObject *getNextObject() { return next < children.size() ? children[next++] : nullptr; }
    void release() { ++releases; }
};
struct IOService : OSObject {
    OSIterator iterator;
    unsigned traversals = 0;
    bool failIterator = false;
    std::vector<UInt32> messages;
    std::function<void(UInt32)> observer;
    OSIterator *getClientIterator() {
        ++traversals; iterator.next = 0;
        return failIterator ? nullptr : &iterator;
    }
    IOReturn messageClients(UInt32 message) {
        assert(!locked);
        messages.push_back(message);
        if (observer) observer(message);
        return kIOReturnSuccess;
    }
};
struct FuxiBacklight : IOService {
    void *fLock = nullptr;
    uint32_t fRequested = FUXI_BACKLIGHT_MAX, published = FUXI_BACKLIGHT_MAX;
    bool fail = false;
    bool publishBrightness(uint32_t value) {
        assert(locked == 1);
        if (fail) return false;
        published = value; return true;
    }
    IOReturn setProperties(OSObject *properties);
};
static int sweet_fb_display_off;
struct IOWorkLoop {
    unsigned calls = 0, depth = 0;
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    IOReturn runAction(Action action, OSObject *owner, void *state) {
        ++calls; ++depth;
        IOReturn result = action(owner, state, nullptr, nullptr, nullptr);
        --depth;
        return result;
    }
};
struct AppleCLCD : IOService {
    void *fLock = nullptr;
    IOWorkLoop workloop;
    IOWorkLoop *fWorkLoop = &workloop;
    bool fPowerOff = false, fDirty = false, fBacklightPowerNotifications = false;
    unsigned fFadeStep = 7, wakes = 0;
    void wakePresentLocked() { assert(locked == 1); ++wakes; }
    IOReturn requestPowerChange(uint64_t);
    static IOReturn powerChangeAction(OSObject *, void *, void *, void *, void *);
};
'''
MODEL += '\nIOReturn FuxiBacklight::setProperties(OSObject *properties)\n' + function('FuxiBacklight::setProperties')
MODEL += '\nvoid fuxi_backlight_display_state(IOService *provider, bool on)\n' + notify
MODEL += '\nIOReturn AppleCLCD::powerChangeAction(OSObject *owner, void *state, void *, void *, void *)\n' + power
MODEL += '\nIOReturn AppleCLCD::requestPowerChange(uint64_t state)\n' + function('AppleCLCD::requestPowerChange', fb)
MODEL += r'''
int main() {
    for (int value : {-1, 0, 1, 2, 3, 4, 65536}) {
        assert(!fbl_enabled(0, value));
        assert(fbl_enabled(1, value) == (value >= 1 && value <= 3));
        assert(!fbl_power_notifications(0, value));
        assert(fbl_power_notifications(1, value) == (value == 2 || value == 3));
        for (bool on : {false, true}) {
            assert(!fbl_withhold_on(0, value, on));
            assert(fbl_withhold_on(1, value, on) == (value == 3 && on));
        }
    }
    for (uint32_t n = 0; n <= 4096; ++n)
        assert(bool(fbl_log_this(n)) == (n && (n <= 64 || n % 256 == 0)));
    assert(!fbl_log_this(UINT32_MAX));
    FuxiBacklight service;
    OSNumber zero(0), half(32768), maximum(65536), high(65537), negative(UINT64_MAX);
    OSObject wrong;
    OSDictionary flat, range, params, nested, empty;
    assert(service.setProperties(nullptr) == kIOReturnBadArgument);
    assert(service.setProperties(&wrong) == kIOReturnBadArgument);
    assert(service.setProperties(&empty) == kIOReturnUnsupported);
    flat.values["brightness"] = &zero;
    assert(service.setProperties(&flat) == kIOReturnSuccess && service.fRequested == 0);
    range.values["value"] = &half;
    params.values["brightness"] = &range;
    nested.values["IODisplayParameters"] = &params;
    assert(service.setProperties(&nested) == kIOReturnSuccess && service.fRequested == 32768);
    flat.values["brightness"] = &maximum;
    assert(service.setProperties(&flat) == kIOReturnSuccess && service.fRequested == 65536);
    OSObject *badValues[] = {&high, &negative, &wrong, &empty};
    for (OSObject *bad : badValues) {
        flat.values["brightness"] = bad;
        assert(service.setProperties(&flat) == kIOReturnBadArgument);
        assert(service.fRequested == 65536 && service.published == 65536);
    }
    nested.values["IODisplayParameters"] = &wrong;
    assert(service.setProperties(&nested) == kIOReturnBadArgument);
    flat.values["brightness"] = &zero;
    service.fail = true;
    assert(service.setProperties(&flat) == kIOReturnNoMemory);
    assert(service.fRequested == 65536 && service.published == 65536 && !locked);

    // Execute the production notification fanout and framebuffer power action.
    AppleCLCD fb;
    IOService unrelated, otherProvider;
    FuxiBacklight backlight, untouched;
    fb.iterator.children = {&unrelated, &backlight};
    otherProvider.iterator.children = {&untouched};
    for (int value : {-1, 0, 1, 2, 3, 4}) {
        for (bool present : {false, true}) {
            for (bool on : {false, true}) {
                boot_present = present; boot_value = value;
                unsigned traversals = fb.traversals;
                size_t count = backlight.messages.size();
                fuxi_backlight_display_state(&fb, on);
                bool enabled = present && (value == 2 || (value == 3 && !on));
                assert(fb.traversals == traversals + enabled);
                assert(backlight.messages.size() == count + enabled);
            }
        }
    }
    backlight.messages.clear();
    boot_present = true; boot_value = 2;
    fb.fBacklightPowerNotifications = true;
    // Interest callbacks may reenter property handling: no framebuffer lock held.
    backlight.observer = [&](UInt32 message) {
        assert(fb.workloop.depth == 1);
        assert(fb.fPowerOff == (message == kIOMessageDeviceWillPowerOff));
        assert(sweet_fb_display_off == int(fb.fPowerOff));
        assert(backlight.setProperties(&flat) == kIOReturnSuccess);
    };
    for (uintptr_t state : {0u, 1u, 1u, 0u, 4u}) {
        fb.fDirty = false; fb.fFadeStep = 6;
        bool changed = fb.fPowerOff != (state == 0);
        assert(fb.requestPowerChange(state) == kIOReturnSuccess);
        assert(fb.fPowerOff == (state == 0) && fb.fDirty == changed);
        assert(fb.fFadeStep == (changed ? 0 : 6));
    }
    assert((backlight.messages == std::vector<UInt32>{0xe0000210, 0xe0000230,
                                                   0xe0000230, 0xe0000210, 0xe0000230}));
    assert(fb.wakes == 5 && !locked && untouched.messages.empty());
    // Withholding the child, iterator allocation failure and mode 1 do not
    // turn a logical power request into an error or change brightness.
    backlight.observer = nullptr;
    uint32_t requested = backlight.fRequested, published = backlight.published;
    fb.failIterator = true;
    size_t count = backlight.messages.size();
    assert(fb.requestPowerChange(0) == kIOReturnSuccess);
    assert(backlight.messages.size() == count);
    fb.failIterator = false;
    fb.iterator.children = {&unrelated};
    fuxi_backlight_display_state(&fb, true);
    assert(backlight.messages.size() == count);
    fb.iterator.children = {&backlight};
    boot_value = 1; fb.fBacklightPowerNotifications = false;
    unsigned traversals = fb.traversals;
    unsigned gated = fb.workloop.calls;
    fb.requestPowerChange(1);
    assert(fb.workloop.calls == gated);
    assert(fb.traversals == traversals && backlight.messages.size() == count);
    assert(fb.iterator.releases == fb.traversals - 1); // one failed iterator
    assert(backlight.fRequested == requested && backlight.published == published);

    // Mode 3 preserves the entire mode 2 power sequence and workloop gate,
    // including duplicates; only ON fanout is removed. Brightness writes do
    // not release a deferred message (there is no deferred state or timer).
    boot_value = 3; fb.fBacklightPowerNotifications = true;
    backlight.messages.clear(); logs.clear();
    backlight.observer = [&](UInt32 message) {
        assert(fb.workloop.depth == 1 && !locked);
        assert(message == kIOMessageDeviceWillPowerOff && fb.fPowerOff);
        assert(sweet_fb_display_off == 1);
    };
    gated = fb.workloop.calls;
    traversals = fb.traversals;
    for (uintptr_t state : {0u, 1u, 1u, 0u, 4u, 0u}) {
        fb.fDirty = false; fb.fFadeStep = 6;
        bool changed = fb.fPowerOff != (state == 0);
        assert(fb.requestPowerChange(state) == kIOReturnSuccess);
        assert(fb.fPowerOff == (state == 0) && fb.fDirty == changed);
        assert(sweet_fb_display_off == int(state == 0));
        assert(fb.fFadeStep == (changed ? 0 : 6));
    }
    assert(fb.workloop.calls == gated + 6 && fb.traversals == traversals + 3);
    assert((backlight.messages == std::vector<UInt32>{0xe0000210, 0xe0000210, 0xe0000210}));
    unsigned withheld = 0;
    for (const auto &log : logs)
        withheld += log.find("on message=0xe0000230 withheld experiment=off-only") != std::string::npos;
    assert(withheld == 3);
    flat.values["brightness"] = &maximum;
    assert(backlight.setProperties(&flat) == kIOReturnSuccess);
    assert(backlight.fRequested == 65536 && backlight.messages.size() == 3);
    fb.failIterator = true;
    assert(fb.requestPowerChange(1) == kIOReturnSuccess && !fb.fPowerOff);
    assert(fb.requestPowerChange(0) == kIOReturnSuccess && fb.fPowerOff);
    assert(backlight.messages.size() == 3 && !locked);
}
'''
with tempfile.TemporaryDirectory(prefix='fuxi-backlight-test-') as tmp:
    path, exe = Path(tmp) / 'backlight.cpp', Path(tmp) / 'backlight'
    path.write_text(MODEL)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(SRC), str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: fuxi backlight (sync, opt-in, registry shape, parser, logical power interests, rate limits)')
