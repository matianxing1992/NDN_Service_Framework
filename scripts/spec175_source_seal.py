#!/usr/bin/env python3
"""Create a content-bound source seal for the Spec175 G0 gate."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import spec175_contract_gate as gate  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", default=str(ROOT))
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    project_root = Path(args.project_root).expanduser().resolve()
    revision, all_dirty = gate._git_state(project_root)
    dirty = [
        line for line in all_dirty
        if gate._is_in_scope_status(line)
        and gate._is_source_subject_path(gate._status_path(line))
    ]
    files: dict[str, object] = {}
    for line in dirty:
        name = gate._status_path(line)
        path = project_root / name
        files[name] = {
            "status": line[:2],
            "sha256": gate._sha256(path) if path.is_file() else None,
        }
    payload = {
        "schemaVersion": "spec175-source-seal-v1",
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "sourceRevision": revision,
        "dirtyFiles": dict(sorted(files.items())),
    }
    output = Path(args.output).expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(json.dumps({
        "output": str(output), "sourceRevision": revision,
        "dirtyFileCount": len(files),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
