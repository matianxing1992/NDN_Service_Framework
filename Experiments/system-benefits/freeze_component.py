#!/usr/bin/env python3
"""Freeze/verify native component source and complete installed loader closure."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def freeze():
    binary = Path("/usr/local/bin/ndnsf-credential-cost")
    closure = subprocess.check_output(["ldd", str(binary)], text=True,
                                      env=dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib"))
    if "not found" in closure or str(HERE.parents[1]) in closure:
        raise RuntimeError("invalid installed closure")
    paths = {binary, HERE / "credential-cost.cpp", HERE / "wscript",
             HERE / "analyze_credentials.py", Path(__file__).resolve()}
    for word in closure.split():
        if word.startswith("/") and Path(word).is_file():
            paths.add(Path(word).resolve())
    return {"scope": "native local component", "closure": closure,
            "sha256": {str(p): sha(p) for p in sorted(paths)}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    if args.verify:
        prior = json.loads(args.verify.read_text())
        mismatches = [p for p, expected in prior["sha256"].items() if sha(p) != expected]
        if mismatches:
            raise SystemExit("candidate changed: " + ", ".join(mismatches))
        print("COMPONENT_CANDIDATE_UNCHANGED")
    else:
        print(json.dumps(freeze(), indent=2, sort_keys=True))
