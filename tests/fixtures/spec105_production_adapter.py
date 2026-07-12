#!/usr/bin/env python3
"""Executable fixture proving production CLI adapters consume their inputs."""

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("provider", "run", "bench"))
    parser.add_argument("--profile")
    parser.add_argument("--plan")
    parser.add_argument("--request")
    parser.add_argument("--campaign")
    parser.add_argument("--out")
    args = parser.parse_args()
    inputs = {
        name: Path(value) for name, value in vars(args).items()
        if name not in {"mode", "out"} and value
    }
    if not inputs or not all(path.is_file() for path in inputs.values()):
        return 2
    payload = {
        "schema": "spec105-production-adapter-fixture-v1",
        "mode": args.mode,
        "inputs": {name: str(path.resolve()) for name, path in inputs.items()},
    }
    if args.out:
        output = Path(args.out)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="utf-8")
    else:
        print(json.dumps(payload, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
