#!/usr/bin/env python3
"""Fail-closed preflight for a candidate-bound Spec170 network workload."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


REQUIRED = (
    "native-execution-plan.json",
    "service-manifest.json",
    "user_driver.py",
    "trust-schema.conf",
    "controller.policies",
)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def roles_for_service(plan: dict, service: str) -> list[str]:
    roles: list[str] = []
    for item in plan.get("services", []):
        if not isinstance(item, dict) or item.get("service") != service:
            continue
        value = item.get("roles", [])
        if isinstance(value, list):
            roles.extend(x for x in value if isinstance(x, str) and x.startswith("/"))
    return sorted(set(roles))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", type=Path, required=True)
    ap.add_argument("--workload", type=Path, required=True)
    ap.add_argument("--baseline-bundle", type=Path)
    ap.add_argument("--service", default="/Inference/NativeTracer")
    ap.add_argument("--single-provider", action="store_true")
    ap.add_argument("--allow-diff", action="append", default=[],
                    help="intentionally variant file; may be repeated")
    ap.add_argument("--expected-execution-provider", choices=("cpu", "cuda"))
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()

    errors: list[str] = []
    hashes: dict[str, dict[str, str | None]] = {}
    for name in REQUIRED:
        candidate = args.bundle / name
        reference = args.baseline_bundle / name if args.baseline_bundle else None
        if not candidate.is_file():
            errors.append(f"MISSING:{candidate}")
            continue
        c_hash = digest(candidate)
        r_hash = digest(reference) if reference and reference.is_file() else None
        hashes[name] = {"candidate": c_hash, "baseline": r_hash}
        if reference and r_hash is None:
            errors.append(f"BASELINE_MISSING:{reference}")
        elif reference and c_hash != r_hash and name not in set(args.allow_diff):
            errors.append(f"BUNDLE_HASH_MISMATCH:{name}:{c_hash}:{r_hash}")

    try:
        plan = json.loads((args.bundle / "native-execution-plan.json").read_text())
        manifest = json.loads((args.bundle / "service-manifest.json").read_text())
        if not isinstance(plan, dict) or not isinstance(manifest, dict):
            raise ValueError("top-level JSON must be an object")
    except Exception as exc:  # pragma: no cover - diagnostic path
        errors.append(f"JSON_INVALID:{exc}")
        plan = {}

    if args.expected_execution_provider:
        matched_service = next(
            (item for item in manifest.get("services", [])
             if isinstance(item, dict) and item.get("name") == args.service),
            None,
        )
        artifacts = matched_service.get("artifacts", []) if matched_service else []
        if not artifacts:
            errors.append(f"MANIFEST_ARTIFACTS_MISSING:{args.service}")
        for index, artifact in enumerate(artifacts):
            metadata = artifact.get("metadata", {}) if isinstance(artifact, dict) else {}
            provider = str(metadata.get("executionProvider", "")).lower()
            if provider != args.expected_execution_provider:
                errors.append(f"MANIFEST_PROVIDER_MISMATCH:{index}:{provider}")
            if args.expected_execution_provider == "cuda":
                if str(metadata.get("deviceId", "")) != "0":
                    errors.append(f"MANIFEST_DEVICE_ID_MISMATCH:{index}")
                if str(metadata.get("allowCpuFallback", "")).lower() != "false":
                    errors.append(f"MANIFEST_CPU_FALLBACK_NOT_DISABLED:{index}")

    try:
        workload = args.workload.read_text()
    except Exception as exc:  # pragma: no cover - diagnostic path
        errors.append(f"WORKLOAD_UNREADABLE:{exc}")
        workload = ""

    for marker in ("native-execution-plan.json", "service-manifest.json", "user_driver.py"):
        if marker not in workload:
            errors.append(f"WORKLOAD_MISSING_REFERENCE:{marker}")
    if args.service not in workload:
        errors.append(f"WORKLOAD_SERVICE_MISSING:{args.service}")

    # The current NativeTracer manifest uses relative artifact filenames. The
    # Provider must enter the staged bundle before launching, otherwise a
    # healthy SIF/CUDA process fails later with "file does not exist".
    if "artifacts/" in workload and not re.search(r"\bcd\s+['\"]?\$BUNDLE\b", workload):
        errors.append("WORKLOAD_BUNDLE_CWD_MISSING")

    roles = roles_for_service(plan, args.service)
    if not roles:
        errors.append(f"PLAN_ROLES_MISSING:{args.service}")
    # Restrict the match to entries inside the quoted preference string. A
    # broad slash-to-arrow regex can start at `/user_driver.py` or another
    # earlier path and swallow the whole command before the first mapping.
    mapping_roles = set(re.findall(r"(?:['\"]|;)(/[^;=\"']+)=>/", workload))
    if args.single_provider:
        if "--role-provider-preference" not in workload:
            errors.append("SINGLE_PROVIDER_ROLE_MAP_MISSING")
        for role in roles:
            if role not in mapping_roles:
                errors.append(f"SINGLE_PROVIDER_ROLE_MISSING:{role}")

    result = {
        "status": "PASS" if not errors else "FAIL",
        "bundle": str(args.bundle),
        "baselineBundle": str(args.baseline_bundle) if args.baseline_bundle else None,
        "service": args.service,
        "roles": roles,
        "hashes": hashes,
        "errors": errors,
    }
    encoded = json.dumps(result, sort_keys=True)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n")
    return 0 if not errors else 4


if __name__ == "__main__":
    sys.exit(main())
