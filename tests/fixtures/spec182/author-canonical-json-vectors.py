#!/usr/bin/env python3
"""Freeze the Python stdlib canonical JSON oracle; never used by native runtime."""
import json
import math
from pathlib import Path
import random
import struct

values = [None, True, False, 0, -1, 2**63-1, 2**64-1, 0.0, -0.0,
          1e-4, 1e-5, 1e15, 1e16, 1e20, 5e-324, 2.2250738585072014e-308,
          1.7976931348623157e308, "中文😀\n\t\u0000", [], {},
          {"z": [], "a": {}, "数字": [1, "1", 1.0, None, False]},
          {"😀": "astral", "\ue000": "bmp", "a": "ascii"}]
random_source = random.Random(182)
for _ in range(2000):
    number = struct.unpack('>d', random_source.getrandbits(64).to_bytes(8, 'big'))[0]
    if math.isfinite(number):
        values.append(number)
rows = [{"value": value, "canonical": json.dumps(value, sort_keys=True,
         separators=(',', ':'), allow_nan=False)} for value in values]
Path(__file__).with_name('canonical-json-vectors.json').write_text(
    json.dumps({"schema": "spec182-canonical-json-v1", "rows": rows}, indent=2) + '\n')
