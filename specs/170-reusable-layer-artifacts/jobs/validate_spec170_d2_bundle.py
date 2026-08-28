#!/usr/bin/env python3
"""Fail-closed validation for one immutable Spec170 D2 workload bundle."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Any


GATE_CONTRACTS = {
    "d2a": {
        "service": "/Inference/Spec170LocalTwoGpu",
        "providers": ("/NDNSF-DI/Tracer/provider/local-two-gpu",),
        "providerRoles": {
            "/NDNSF-DI/Tracer/provider/local-two-gpu": (
                "/D2A/Independent/0", "/D2A/Independent/1",
                "/D2A/LocalGroup#0", "/D2A/LocalGroup#1",
            ),
        },
        "required": {
            "d2a-local-two-gpu.sh",
            "spec170_v3_local_two_gpu_provider.py",
            "spec170_v3_local_two_gpu_user.py",
            "artifacts/qwen-native-tracer-backbone.onnx",
        },
        "markers": (
            "SPEC170_D2A_NETWORK_PASS", "torch.cuda.nccl.all_reduce",
            "SPEC170_D2A_GROUP_FAILURE_PASS",
        ),
    },
    "d2b": {
        "service": "/Inference/Spec170Collective",
        "providers": (
            "/NDNSF-DI/Tracer/provider/rank0",
            "/NDNSF-DI/Tracer/provider/rank1",
        ),
        "providerRoles": {
            "/NDNSF-DI/Tracer/provider/rank0": ("/Backbone",),
            "/NDNSF-DI/Tracer/provider/rank1": ("/Head/Shard/0",),
        },
        "required": {
            "d2b-cross-provider.sh",
            "user_driver.py",
            "assignment.csv",
            "native-execution-plan.json",
            "service-manifest.json",
            "artifacts/qwen-native-tracer-backbone.onnx",
            "artifacts/qwen-native-tracer-head0.onnx",
        },
        "markers": (
            "SPEC170_D2B_POSITIVE_PASS", "NDNSF_DATA_V1",
            "di-native-provider", "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION",
        ),
    },
    "d2h": {
        "service": "/Inference/Spec170Hybrid",
        "providers": (
            "/NDNSF-DI/Tracer/provider/p0",
            "/NDNSF-DI/Tracer/provider/p1",
        ),
        "providerRoles": {
            "/NDNSF-DI/Tracer/provider/p0": (
                "/Pipeline/S0/R0", "/Pipeline/S1/R0", "/Pipeline/S2/R0"),
            "/NDNSF-DI/Tracer/provider/p1": (
                "/Pipeline/S0/R1", "/Pipeline/S1/R1", "/Pipeline/S2/R0",
                "/Pipeline/S2/R1"),
        },
        "required": {
            "d2h-hybrid.sh",
            "spec170_v3_hybrid_provider.py",
            "spec170_v3_hybrid_user.py",
            "artifacts/qwen-native-tracer-backbone.onnx",
            "artifacts/qwen-native-tracer-head0.onnx",
            "artifacts/qwen-native-tracer-head1.onnx",
            "artifacts/qwen-native-tracer-merge.onnx",
        },
        "markers": (
            "SPEC170_D2H_HYBRID_WORKLOAD_PASS", "SPEC170_D2H_NUMERIC_ORACLE_PASS",
            'artifact_root="$BUNDLE/artifacts"',
        ),
    },
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_relative(value: str) -> bool:
    path = Path(value)
    return (value != "" and not path.is_absolute() and ".." not in path.parts
            and path.as_posix() == value)


def validate_bundle(
        *,
        bundle: Path,
        workload: Path,
        expected_sif_sha256: str,
        expected_gate: str | None = None,
        expected_service: str | None = None,
) -> dict[str, Any]:
    bundle = bundle.resolve()
    workload = workload.resolve()
    errors: list[str] = []
    manifest_path = bundle / "bundle-manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if not isinstance(manifest, dict):
            raise ValueError("manifest is not an object")
    except Exception as exc:  # noqa: BLE001
        return {"status": "FAIL", "bundle": str(bundle),
                "errors": [f"MANIFEST_INVALID:{exc}"]}

    gate = str(manifest.get("gate", ""))
    contract = GATE_CONTRACTS.get(gate)
    if manifest.get("schemaVersion") != "spec170-d2-bundle-v1":
        errors.append("MANIFEST_SCHEMA_MISMATCH")
    if manifest.get("status") != "PASS":
        errors.append("MANIFEST_NOT_PASS")
    if contract is None:
        errors.append(f"UNKNOWN_GATE:{gate}")
        contract = {"service": "", "providers": (), "providerRoles": {},
                    "required": set(), "markers": ()}
    if expected_gate and gate != expected_gate:
        errors.append(f"GATE_MISMATCH:{gate}:{expected_gate}")
    service = str(manifest.get("service", ""))
    if service != contract["service"]:
        errors.append(f"SERVICE_CONTRACT_MISMATCH:{service}")
    if expected_service and service != expected_service:
        errors.append(f"SERVICE_EXPECTATION_MISMATCH:{service}:{expected_service}")
    if (not re.fullmatch(r"[0-9a-f]{64}", expected_sif_sha256)
            or manifest.get("sifSha256") != expected_sif_sha256):
        errors.append("SIF_SHA256_MISMATCH")
    providers = manifest.get("providers", [])
    if (not isinstance(providers, list)
            or tuple(providers) != tuple(contract["providers"])):
        errors.append("PROVIDER_SET_MISMATCH")

    recorded = manifest.get("files", {})
    if not isinstance(recorded, dict):
        errors.append("FILE_MANIFEST_INVALID")
        recorded = {}
    actual_files = {
        path.relative_to(bundle).as_posix()
        for path in bundle.rglob("*") if path.is_file()
    }
    expected_files = set(recorded) | {"bundle-manifest.json"}
    if actual_files != expected_files:
        errors.append(
            "FILE_SET_MISMATCH:missing=" + ",".join(sorted(expected_files - actual_files))
            + ":extra=" + ",".join(sorted(actual_files - expected_files)))
    for relative, record in recorded.items():
        if not _safe_relative(relative) or not isinstance(record, dict):
            errors.append(f"FILE_RECORD_INVALID:{relative}")
            continue
        path = bundle / relative
        if not path.is_file() or path.is_symlink():
            errors.append(f"FILE_MISSING_OR_SYMLINK:{relative}")
            continue
        if _sha256(path) != record.get("sha256"):
            errors.append(f"FILE_HASH_MISMATCH:{relative}")
        if path.stat().st_size != record.get("bytes"):
            errors.append(f"FILE_SIZE_MISMATCH:{relative}")
    if not set(contract["required"]).issubset(recorded):
        errors.append("GATE_REQUIRED_FILES_MISSING")

    declared_workload = str(manifest.get("workload", ""))
    if (not _safe_relative(declared_workload)
            or workload != (bundle / declared_workload).resolve()):
        errors.append("WORKLOAD_BINDING_MISMATCH")
    try:
        workload_text = workload.read_text(encoding="utf-8")
    except Exception as exc:  # noqa: BLE001
        errors.append(f"WORKLOAD_UNREADABLE:{exc}")
        workload_text = ""
    if not os.access(workload, os.X_OK):
        errors.append("WORKLOAD_NOT_EXECUTABLE")
    if 'dirname -- "$0"' not in workload_text or 'cd "$BUNDLE"' not in workload_text:
        errors.append("WORKLOAD_BUNDLE_CWD_MISSING")
    if "/release/spec170-runtime-*" in workload_text:
        errors.append("WORKLOAD_WILDCARD_RELEASE_SELECTION")
    combined = workload_text
    for relative in sorted(recorded):
        if relative.endswith(".py"):
            path = bundle / relative
            try:
                source = path.read_text(encoding="utf-8")
                ast.parse(source, filename=relative)
                combined += "\n" + source
            except Exception as exc:  # noqa: BLE001
                errors.append(f"PYTHON_SYNTAX_INVALID:{relative}:{exc}")
    if service not in combined:
        errors.append("WORKLOAD_SERVICE_MISSING")
    for marker in contract["markers"]:
        if marker not in combined:
            errors.append(f"GATE_MARKER_MISSING:{marker}")
    if gate == "d2b":
        # Model/runtime READY is not enough to release the User: permission
        # installation is asynchronous and a first request otherwise races an
        # empty provider authorization table.  Keep this as a bundle contract
        # so an older diagnostic workload cannot be staged as a final gate.
        if "provider-$rank.permission-ready" not in workload_text:
            errors.append("D2B_PROVIDER_PERMISSION_BARRIER_MISSING")
        if "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_READY" not in workload_text:
            errors.append("D2B_TARGET_PERMISSION_BARRIER_MISSING")
        for forbidden in (
                "publish_ndnsf_data_v1", "fetch_ndnsf_data_v1",
                "spec170_v3_cross_provider_provider.py",
                "spec170_v3_cross_provider_user.py"):
            if forbidden in combined:
                errors.append(f"D2B_NONPRODUCTION_PATH_PRESENT:{forbidden}")
        try:
            plan = json.loads(
                (bundle / "native-execution-plan.json").read_text(encoding="utf-8"))
            service_plan = next(
                item for item in plan["services"] if item["service"] == service)
            if service_plan.get("executionPolicy") != "DATA_DRIVEN_V2":
                errors.append("D2B_EXECUTION_POLICY_MISMATCH")
            if service_plan.get("roles") != ["/Backbone", "/Head/Shard/0"]:
                errors.append("D2B_ROLE_PLAN_MISMATCH")
            dependencies = service_plan.get("dependencies", [])
            if (len(dependencies) != 1
                    or dependencies[0].get("transportProfile") != "NDNSF_DATA_V1"
                    or dependencies[0].get("expectedSegments") != 0):
                errors.append("D2B_DATA_V1_DEPENDENCY_MISMATCH")
        except Exception as exc:  # noqa: BLE001
            errors.append(f"D2B_PLAN_INVALID:{exc}")
        try:
            service_manifest = json.loads(
                (bundle / "service-manifest.json").read_text(
                    encoding="utf-8"))["services"][0]
            artifacts = service_manifest.get("artifacts", [])
            if len(artifacts) != 2:
                errors.append("D2B_ARTIFACT_SET_MISMATCH")
            for artifact in artifacts:
                metadata = artifact.get("metadata", {})
                if (metadata.get("executionProvider") != "cuda"
                        or str(metadata.get("allowCpuFallback", "")).lower()
                        != "false"):
                    errors.append(
                        f"D2B_CUDA_POLICY_MISMATCH:{artifact.get('role', '')}")
        except Exception as exc:  # noqa: BLE001
            errors.append(f"D2B_MANIFEST_INVALID:{exc}")
    if workload.is_file():
        result = subprocess.run(
            ["bash", "-n", str(workload)], capture_output=True, text=True)
        if result.returncode != 0:
            errors.append(f"WORKLOAD_SHELL_INVALID:{result.stderr.strip()}")

    policy_path = bundle / "controller.policies"
    try:
        policy = policy_path.read_text(encoding="utf-8")
    except OSError as exc:
        errors.append(f"POLICY_UNREADABLE:{exc}")
        policy = ""
    if service not in policy or "/NDNSF-DI/Tracer/user" not in policy:
        errors.append("POLICY_SERVICE_OR_USER_MISSING")
    for provider in providers if isinstance(providers, list) else []:
        if str(provider) not in policy:
            errors.append(f"POLICY_PROVIDER_MISSING:{provider}")
        for role in contract["providerRoles"].get(str(provider), ()):
            permission = service + "/ROLE" + role.replace("#", "%23")
            if permission not in policy:
                errors.append(
                    f"POLICY_PROVIDER_ROLE_MISSING:{provider}:{role}")
    try:
        nfd = (bundle / "nfd.conf").read_text(encoding="utf-8")
        if "@@" in nfd or "/scratch/run/nfd.sock" not in nfd:
            errors.append("NFD_CONFIG_NOT_MATERIALIZED")
    except OSError as exc:
        errors.append(f"NFD_CONFIG_UNREADABLE:{exc}")

    return {
        "schemaVersion": "spec170-d2-bundle-preflight-v1",
        "status": "PASS" if not errors else "FAIL",
        "bundle": str(bundle), "gate": gate, "service": service,
        "workload": str(workload), "sifSha256": expected_sif_sha256,
        "fileCount": len(recorded), "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--workload", type=Path, required=True)
    parser.add_argument("--expected-sif-sha256", required=True)
    parser.add_argument("--expected-gate")
    parser.add_argument("--expected-service")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = validate_bundle(
        bundle=args.bundle, workload=args.workload,
        expected_sif_sha256=args.expected_sif_sha256,
        expected_gate=args.expected_gate, expected_service=args.expected_service,
    )
    encoded = json.dumps(result, sort_keys=True)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    return 0 if result["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
