"""Compatibility import surface for the distributed-inference YOLO example."""

from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGET = (ROOT / "examples" / "python" / "NDNSF-DistributedInference" /
          "yolo_split" / "yolo_split_lib.py")

spec = importlib.util.spec_from_file_location("_ndnsf_di_yolo_split_lib", TARGET)
module = importlib.util.module_from_spec(spec)
assert spec is not None and spec.loader is not None
spec.loader.exec_module(module)

for name, value in vars(module).items():
    if not name.startswith("_"):
        globals()[name] = value
