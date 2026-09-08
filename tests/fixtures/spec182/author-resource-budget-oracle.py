#!/usr/bin/env python3
"""Freeze resource canonical bytes and peak estimates from the maintained SDK."""
from dataclasses import asdict, replace
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
repo = root.parents[2]
sys.path[:0] = [str(repo / 'NDNSF-DistributedInference'),
               str(repo / 'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.splitter import RoleResourceRequirement, _canonical_bytes

base = RoleResourceRequirement(('onnxruntime-cpu', 'onnxruntime-cuda'), 1, 2, 3, 4, 5)
cases = [('complete', base), ('kv-zero', replace(base, kv_bytes=0)),
         ('truncate', replace(base, safety_margin=1.01)),
         ('zero', RoleResourceRequirement(('onnxruntime',), 0, 0, 0, 0, 0)),
         ('binary64-round', RoleResourceRequirement(('onnxruntime',), 2**53 + 1, 0, 0, 0, 0, 1.0)),
         ('sum-overflow', RoleResourceRequirement(('onnxruntime',), 2**64 - 1, 1, 0, 0, 0, 1.0)),
         ('peak-overflow', RoleResourceRequirement(('onnxruntime',), 2**63, 0, 0, 0, 0, 2.0))]
for field in ('weight_bytes', 'workspace_bytes', 'kv_bytes', 'activation_bytes', 'transient_bytes'):
    cases.append(('unknown-' + field, replace(base, **{field: None})))
rows = []
for name, value in cases:
    peak = value.estimated_peak_gpu_memory_bytes
    rows.append({'name': name, 'input': asdict(value),
                 'canonical_json': _canonical_bytes(value).decode(),
                 'peak': peak, 'native_overflow': peak is not None and peak >= 2**64})
(root / 'resource-budget-oracle.json').write_text(json.dumps(rows, indent=2) + '\n')
print(f'{len(rows)} maintained resource budget cases generated')
