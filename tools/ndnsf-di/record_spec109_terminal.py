#!/usr/bin/env python3
"""Create immutable terminal records for cells prevented by a verified gate."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath


def digest(value: object) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def safe_relative(value: str) -> Path:
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        raise ValueError("TERMINAL_PATH_INVALID")
    return Path(*pure.parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--reason", required=True)
    parser.add_argument("--scope", choices=("systemic", "model-local", "placement-local"),
                        default="systemic")
    parser.add_argument("--task", action="append", default=[])
    parser.add_argument("--cell", action="append", default=[],
                        help="cell-id=relative/output.json")
    args = parser.parse_args()
    gate = json.loads(Path(args.gate).read_text(encoding="utf-8"))
    if gate.get("status") != "BLOCKED" or gate.get("jobSubmitted") is not False:
        raise ValueError("TERMINAL_GATE_NOT_BLOCKED")
    gate_digest = gate.get("observationDigest") or gate.get("gateDigest")
    if not isinstance(gate_digest, str) or not gate_digest.startswith("sha256:"):
        raise ValueError("TERMINAL_GATE_DIGEST_INVALID")
    root = Path(args.output_root)
    records = []
    for item in args.cell:
        cell_id, separator, relative = item.partition("=")
        if not separator or not cell_id:
            raise ValueError("TERMINAL_CELL_INVALID")
        target = root / safe_relative(relative)
        core = {
            "schemaVersion": "1.0-terminal", "cellId": cell_id,
            "state": "BLOCKED", "reasonCode": args.reason,
            "gateScope": args.scope, "gateDigest": gate_digest,
            "gatePath": str(Path(args.gate)), "jobSubmitted": False,
            "runId": None, "evidenceDigest": None,
            "physicalProduction": "DEFERRED",
        }
        core["recordDigest"] = digest(core)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(core, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        records.append({"cellId": cell_id, "path": str(target),
                        "recordDigest": core["recordDigest"]})
    manifest = {
        "schemaVersion": "1.0-terminal-bundle", "status": "BLOCKED",
        "reasonCode": args.reason, "gateScope": args.scope,
        "gateDigest": gate_digest, "tasks": args.task, "records": records,
        "jobSubmissionCount": 0, "allCellsTerminal": True,
        "physicalProduction": "DEFERRED",
    }
    manifest["manifestDigest"] = digest(manifest)
    path = root / "terminal-bundle.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(manifest, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
