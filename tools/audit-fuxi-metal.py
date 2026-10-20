#!/usr/bin/env python3
"""Audit signed M4 artifacts using the lane Mach-O checks, record system imports."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
spec=importlib.util.spec_from_file_location('audit',Path(__file__).with_name('audit-fuxi-vulkan.py'))
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('binaries',type=Path,nargs='+');args=ap.parse_args()
required={}
for binary in args.binaries:required.update(audit.audit(binary,required_export=None))
out=args.binaries[-1].parent
(out/'M4-required-symbol-libs.json').write_text(json.dumps(required,indent=2))
(out/'M4-required-symbols.json').write_text(json.dumps([{'pattern':'^('+'|'.join(re.escape(s) for s in sorted(required))+')$'}]))
