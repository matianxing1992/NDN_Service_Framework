#!/usr/bin/env python3
"""Compatibility entry point for the distributed-inference YOLO user."""

from __future__ import annotations

from pathlib import Path
import runpy


if __name__ == "__main__":
    target = (Path(__file__).resolve().parents[2] /
              "examples" / "python" / "NDNSF-DistributedInference" /
              "yolo_split" / "user.py")
    runpy.run_path(str(target), run_name="__main__")
