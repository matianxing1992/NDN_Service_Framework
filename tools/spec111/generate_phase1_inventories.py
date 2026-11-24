#!/usr/bin/env python3
"""Verify the frozen historical Spec111 evidence without regenerating it."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Dict

REPO = Path(__file__).resolve().parents[2]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def verify_historical_baseline(root: Path, baseline: Path, reference: Path) -> Dict[str, Any]:
    """Verify an explicitly supplied archive, without requiring it in ordinary unit tests."""
    root = root.resolve()
    for path in (baseline, reference):
        if not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise ValueError("historical-baseline-missing-or-too-large: " + str(path))
    expected = json.loads(reference.read_text(encoding="utf-8"))
    if not isinstance(expected, dict):
        raise ValueError("historical-baseline-reference-invalid")
    if sha256(baseline) != expected["baselineSha256"]:
        raise ValueError("historical-baseline-digest-mismatch")
    payload = json.loads(baseline.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("historical-baseline-invalid")
    rows = payload.get("files", [])
    if (not isinstance(rows, list) or not rows or len(rows) != expected["frozenFileCount"] or
            payload.get("sourceCommit") != expected["sourceCommit"]):
        raise ValueError("historical-baseline-identity-mismatch")
    seen = set()
    for item in rows:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise ValueError("historical-baseline-row-invalid")
        relative = Path(item["path"])
        path = (root / relative).resolve()
        try:
            path.relative_to(root)
        except ValueError:
            raise ValueError("historical-baseline-path-escape")
        if relative.is_absolute() or str(relative) in seen or not path.is_file():
            raise ValueError("historical-baseline-file-missing-or-duplicate: " + str(relative))
        seen.add(str(relative))
        if sha256(path) != item["sha256"]:
            raise ValueError("historical-baseline-file-digest-mismatch: " + str(relative))
    return {"verifiedFileCount": len(rows), "sourceCommit": payload["sourceCommit"]}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-baseline", type=Path, required=True,
                        help="verify an existing historical archive")
    arguments = parser.parse_args()
    try:
        report = verify_historical_baseline(
            REPO, arguments.verify_baseline,
            REPO / "tests/fixtures/ndnsf-di-core-app-separation/historical-evidence-baseline.json")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, str(error) + "\n")
    print(json.dumps(report, sort_keys=True))
