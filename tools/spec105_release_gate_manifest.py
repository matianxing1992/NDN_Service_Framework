#!/usr/bin/env python3
"""Refresh the digest manifest for a Spec 105 release-gate input."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, required=True)
    args = parser.parse_args()
    payload = json.loads(args.input.read_text(encoding="utf-8"))
    root = args.evidence_root.resolve()
    paths = sorted({
        str(path)
        for dimension in payload["dimensions"].values()
        for path in dimension["artifacts"]
    })
    manifest = []
    for relative in paths:
        path = (root / relative).resolve()
        path.relative_to(root)
        if not path.is_file():
            raise SystemExit(f"missing evidence: {relative}")
        manifest.append({
            "path": relative,
            "sha256": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    repo = Path(__file__).resolve().parents[1]
    payload["evidence_root"] = str(args.evidence_root)
    payload["evidence_manifest"] = manifest
    payload["gate_generator_commit"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    args.input.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
