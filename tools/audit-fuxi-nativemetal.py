#!/usr/bin/env python3
"""Native frontend allows exactly one external load; verify its C ABI exports."""
import importlib.util
from pathlib import Path
import subprocess
import sys

spec = importlib.util.spec_from_file_location('audit', Path(__file__).with_name('audit-fuxi-vulkan.py'))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
frontend, bridge = map(Path, sys.argv[1:])
required = audit.audit(frontend, required_export='_fuxi_native_metal_open',
                       allowed_loads=('/usr/local/lib/libnative_metal_bridge.dylib',))
audit.audit(bridge, required_export='_nmb_device_open')
exports = set(subprocess.check_output(['nm', '-gU', str(bridge)], text=True).split())
native_imports = [symbol for symbol, provider in required.items() if provider in ('native_metal_bridge', 'libnative_metal_bridge')]
assert native_imports, 'frontend must link the replaceable native bridge'
assert all(symbol in exports for symbol in native_imports)
# Audit reports live beside the library, outside the signed bundle.
report = frontend.with_suffix(frontend.suffix+'.undefined.txt')
report.replace(bridge.parent/report.name)
print('PASS: native callback entry, replaceable bridge load and nmb imports')
