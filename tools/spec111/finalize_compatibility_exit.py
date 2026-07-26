#!/usr/bin/env python3
"""Evaluate and persist the bounded Spec 111 compatibility exit gate."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


COMPATIBILITY_STATUSES = {"compatibility-reexport", "compatibility-target"}


def evaluate(
    manifest: dict[str, object],
    snapshot_one: dict[str, object],
    snapshot_two: dict[str, object],
    performance: dict[str, object],
) -> tuple[dict[str, object], dict[str, object]]:
    two_zero_snapshots = all(
        snapshot.get("verdict") == "PASS" and
        snapshot.get("productionCallerCount") == 0
        for snapshot in (snapshot_one, snapshot_two)
    )
    performance_gate = bool(performance.get("gate", {}).get("pass"))
    decisions = []
    eligible_ids = []
    updated_entries = []
    for original in manifest.get("entries", []):
        entry = dict(original)
        if entry.get("status") not in COMPATIBILITY_STATUSES:
            updated_entries.append(entry)
            continue
        external_migration = bool(entry.get("externalMigrationEvidence"))
        user_expiry = bool(entry.get("userApprovedExpiry"))
        external_gate = external_migration or user_expiry
        rollback = bool(entry.get("rollbackRelease"))
        eligible = (
            two_zero_snapshots and performance_gate and
            external_gate and rollback
        )
        missing = []
        if not two_zero_snapshots:
            missing.append("two consecutive zero-caller snapshots")
        if not performance_gate:
            missing.append("passing correctness/performance evidence gate")
        if not external_gate:
            missing.append(
                "external migration evidence or explicit user-approved expiry")
        if not rollback:
            missing.append("rollback release")
        entry["externalUseStatus"] = (
            "migration_confirmed" if external_migration else
            "user_approved_expiry" if user_expiry else
            "external_use_unknown"
        )
        entry["removalEligible"] = eligible
        entry["remainingExitCondition"] = (
            "none" if eligible else "; ".join(missing)
        )
        updated_entries.append(entry)
        decision = {
            "surfaceId": entry["surfaceId"],
            "currentOwner": entry["currentOwner"],
            "twoZeroCallerSnapshots": two_zero_snapshots,
            "performanceGate": performance_gate,
            "externalMigrationOrApprovedExpiry": external_gate,
            "rollbackAvailable": rollback,
            "eligible": eligible,
            "remainingExitCondition": entry["remainingExitCondition"],
        }
        decisions.append(decision)
        if eligible:
            eligible_ids.append(entry["surfaceId"])

    updated_manifest = dict(manifest)
    updated_manifest["entries"] = updated_entries
    updated_manifest["callerCounts"] = {
        "snapshots": [
            snapshot_one.get("snapshotId", "snapshot-1"),
            snapshot_two.get("snapshotId", "snapshot-2"),
        ],
        "productionCallerCounts": [
            snapshot_one.get("productionCallerCount"),
            snapshot_two.get("productionCallerCount"),
        ],
        "twoConsecutiveZeroCallerSnapshots": two_zero_snapshots,
    }
    gate = {
        "schema": "ndnsf-di-spec111-compatibility-exit-gate-v1",
        "twoConsecutiveZeroCallerSnapshots": two_zero_snapshots,
        "performanceGate": performance_gate,
        "evaluatedCompatibilityEntryCount": len(decisions),
        "eligibleCount": len(eligible_ids),
        "eligibleSurfaceIds": eligible_ids,
        "verdict": "ALLOW_BOUNDED_REMOVAL" if eligible_ids else "RETAIN_ALL",
        "decisions": decisions,
    }
    return updated_manifest, gate


def read_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--snapshot-one", type=Path, required=True)
    parser.add_argument("--snapshot-two", type=Path, required=True)
    parser.add_argument("--performance-analysis", type=Path, required=True)
    parser.add_argument("--gate-output", type=Path, required=True)
    args = parser.parse_args()
    updated, gate = evaluate(
        read_json(args.manifest),
        read_json(args.snapshot_one),
        read_json(args.snapshot_two),
        read_json(args.performance_analysis),
    )
    write_json(args.manifest, updated)
    write_json(args.gate_output, gate)
    print(json.dumps(gate, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
