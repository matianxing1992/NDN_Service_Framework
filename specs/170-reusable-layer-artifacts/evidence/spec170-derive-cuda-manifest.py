#!/usr/bin/env python3
"""Derive a deterministic CUDA manifest from the current D0 manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--service", default="/Inference/NativeTracer")
    args = ap.parse_args()

    document = json.loads(args.input.read_text(encoding="utf-8"))
    matched = 0
    for service in document.get("services", []):
        if service.get("name") != args.service:
            continue
        for artifact in service.get("artifacts", []):
            metadata = artifact.setdefault("metadata", {})
            metadata["executionProvider"] = "cuda"
            metadata["deviceId"] = "0"
            metadata["allowCpuFallback"] = "false"
            matched += 1
    if matched == 0:
        raise SystemExit(f"service/artifacts not found: {args.service}")
    args.output.write_text(
        json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"status": "PASS", "service": args.service, "artifacts": matched}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
