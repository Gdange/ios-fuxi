#!/usr/bin/env python3
"""Backlight probe: patch sync, gates, API shape and production parser logic."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'xnu/fuxi-backlight'
source = (SRC / 'FuxiBacklight.cpp').read_text()
fb = (ROOT / 'xnu/fuxi-unixnu/SweetFramebuffer.cpp').read_text()


def function(name):
    start = source.index(name + '(')
    # Use the definition, not a class declaration.
    start = source.index('{', start)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


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
assert '#if defined(FUXI)\n\tfuxi_backlight_publish(this);\n#endif\n\tregisterService();' in fb
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
# Hardware-free translation unit, including no framebuffer power callback.
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
enum { BL_PROPERTIES, BL_BRIGHTNESS };
using IOReturn = int;
static bool bl_log(unsigned, uint32_t *) { return false; }
static void bl_value(const char *, uint32_t, const char *, OSObject *) {}
static void IOLog(const char *, ...) {}
static unsigned locked;
static void IOLockLock(void *) { assert(!locked); ++locked; }
static void IOLockUnlock(void *) { assert(locked == 1); --locked; }
struct FuxiBacklight {
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
'''
MODEL += '\nIOReturn FuxiBacklight::setProperties(OSObject *properties)\n' + function('FuxiBacklight::setProperties')
MODEL += r'''
int main() {
    for (int value : {-1, 0, 1, 2, 65536}) {
        assert(!fbl_enabled(0, value));
        assert(fbl_enabled(1, value) == (value == 1));
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
}
'''
with tempfile.TemporaryDirectory(prefix='fuxi-backlight-test-') as tmp:
    path, exe = Path(tmp) / 'backlight.cpp', Path(tmp) / 'backlight'
    path.write_text(MODEL)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(SRC), str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: fuxi backlight (sync, opt-in, registry shape, observation hooks, parser, rate limits)')
