#!/usr/bin/env python3
"""Fail-closed Spec 162 scratch and durable-capacity decision."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


SCHEMA = "ndnsf-di-qwen36-capacity-decision-v2"


def _non_negative_int(document: dict[str, Any], key: str) -> int:
    value = int(document.get(key, 0))
    if value < 0:
        raise ValueError(f"{key} must be non-negative")
    return value


def evaluate_capacity(document: dict[str, Any]) -> dict[str, Any]:
    current = _non_negative_int(document, "currentUsageBytes")
    quota = _non_negative_int(document, "verifiedQuotaBytes")
    scratch_free = _non_negative_int(document, "scratchFreeBytes")
    scratch_peak = _non_negative_int(document, "scratchTemporaryPeakBytes")
    durable_promotion = _non_negative_int(document, "durablePromotionBytes")
    runtime = _non_negative_int(document, "runtimeBytes")
    evidence = _non_negative_int(document, "evidenceBytes")
    reserve = _non_negative_int(document, "reserveBytes")
    durable_capacity = _non_negative_int(document, "durableCapacityBytes")
    scratch_path = str(document.get("scratchPath", ""))
    quota_authority = str(document.get("quotaAuthority", "")).strip()
    durable_capacity_authority = str(
        document.get("durableCapacityAuthority", "")).strip()
    durable_capacity_mode = str(
        document.get("durableCapacityMode", "")).strip()
    user_authorized_global_capacity = bool(
        document.get("userAuthorizedGlobalCapacity", False))
    scratch_authority = str(
        document.get("scratchCapacityAuthority", "")).strip()
    scratch_writable = bool(document.get("scratchWritable", False))

    projected_durable = current + durable_promotion + runtime + evidence
    additional_durable = durable_promotion + runtime + evidence
    reasons: list[str] = []
    if quota_authority and quota > 0:
        if projected_durable + reserve > quota:
            reasons.append("DURABLE_RESERVE_NOT_MET")
    elif durable_capacity_mode == "global-filesystem-free":
        if not user_authorized_global_capacity:
            reasons.append("GLOBAL_CAPACITY_NOT_USER_AUTHORIZED")
        if not durable_capacity_authority or durable_capacity <= 0:
            reasons.append("DURABLE_GLOBAL_CAPACITY_UNVERIFIED")
        elif additional_durable + reserve > durable_capacity:
            reasons.append("DURABLE_GLOBAL_RESERVE_NOT_MET")
    else:
        reasons.append("DURABLE_QUOTA_UNVERIFIED")
    if not scratch_authority:
        reasons.append("SCRATCH_AUTHORITY_UNVERIFIED")
    if not scratch_path.startswith("/"):
        reasons.append("SCRATCH_PATH_INVALID")
    if not scratch_writable:
        reasons.append("SCRATCH_NOT_WRITABLE")
    if scratch_free < scratch_peak:
        reasons.append("SCRATCH_PEAK_EXCEEDS_FREE")

    cleanup = [
        str(path)
        for path in document.get("cleanupCandidates", [])
        if str(path)
    ]
    return {
        "schemaVersion": SCHEMA,
        "allowed": not reasons,
        "blockReasons": reasons,
        "scratchPath": scratch_path,
        "scratchCapacityAuthority": scratch_authority,
        "scratchFreeBytes": scratch_free,
        "scratchTemporaryPeakBytes": scratch_peak,
        "currentUsageBytes": current,
        "verifiedQuotaBytes": quota,
        "quotaAuthority": quota_authority,
        "durableCapacityMode": durable_capacity_mode,
        "durableCapacityBytes": durable_capacity,
        "durableCapacityAuthority": durable_capacity_authority,
        "userAuthorizedGlobalCapacity": user_authorized_global_capacity,
        "durablePromotionBytes": durable_promotion,
        "runtimeBytes": runtime,
        "evidenceBytes": evidence,
        "reserveBytes": reserve,
        "additionalDurableBytes": additional_durable,
        "additionalDurableWithReserveBytes": additional_durable + reserve,
        "projectedDurableBytes": projected_durable,
        "projectedDurableWithReserveBytes": projected_durable + reserve,
        "cleanupInventory": cleanup,
        "cleanupAuthorized": False,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-json", required=True)
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args(argv)
    target = Path(args.output_json)
    decision = evaluate_capacity(json.loads(
        Path(args.input_json).read_text(encoding="utf-8")))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(decision, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0 if decision["allowed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
