"""Synthetic predecessor files shared by lineage contract tests."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


def create_lineage_fixture(root: Path) -> tuple[Path, dict[str, object]]:
    rows = []
    entries = (
        ("task-closure", "specs/105-ndnsf-di-deployment-readiness/tasks.md"),
        ("release-decision", "specs/105-ndnsf-di-deployment-readiness/release-gate.json"),
        ("performance-negative-evidence", "specs/105-ndnsf-di-deployment-readiness/evidence/performance.md"),
        ("recovery-negative-evidence", "specs/105-ndnsf-di-deployment-readiness/evidence/recovery.md"),
    )
    for classification, relative in entries:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(classification + "\n", encoding="utf-8")
        rows.append({
            "classification": classification,
            "path": relative,
            "sha256": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    payload: dict[str, object] = {
        "schema": "ndnsf-di-lineage-lock-v1",
        "predecessorSpec": "specs/105-ndnsf-di-deployment-readiness",
        "frozenCommit": "a" * 40,
        "files": rows,
        "predecessorReleaseId": "spec105-local-minindn-candidate-r2",
        "predecessorMiniNdnVerdict": "BLOCK",
        "predecessorPhysicalVerdict": "DEFERRED",
    }
    lock = root / "lineage-lock.json"
    lock.write_text(json.dumps(payload), encoding="utf-8")
    return lock, payload
