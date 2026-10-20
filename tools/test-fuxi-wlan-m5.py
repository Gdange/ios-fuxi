#!/usr/bin/env python3
"""Compatibility entry point: current WLAN C is M6; M5 logs stay decodable."""
import runpy
from pathlib import Path

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('test-fuxi-wlan-m6.py')), run_name='__main__')
