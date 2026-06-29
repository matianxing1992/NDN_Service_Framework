#!/usr/bin/env python3
"""Generate and validate the native DI tracer policy bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

from ndnsf_distributed_inference import write_policy_bundle


SERVICE = "/Inference/NativeTracer"
ROOT = Path(__file__).resolve().parent
CONFIG_FILE = ROOT / "native_tracer_policy.yaml"
GENERATE_QWEN_ARTIFACTS = ROOT / "generate_qwen_native_tracer_artifacts.py"
OPTIMIZE_NATIVE_TRACER = ROOT / "optimize_native_tracer_plan.py"
REQUIRED_ROLES = {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"}
REQUIRED_OUTPUTS = {
    "/Backbone": {"backbone-to-head0", "backbone-to-head1"},
    "/Head/Shard/0": {"head0-to-merge"},
    "/Head/Shard/1": {"head1-to-merge"},
}
FINAL_RESPONSE_ROLE = "/Merge"
FINAL_RESPONSE_SCOPE = "final-response"
PADDING_TENSOR = "__ndnsf_padding"
MAX_SEGMENT_SIZE = 7000


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolve_artifact_path(raw_path: str) -> Path:
    path = Path(raw_path)
    if not path.is_absolute():
        path = ROOT / path
    return path


def verify_sidecar(path: Path) -> None:
    sidecar = path.with_suffix(path.suffix + ".sha256")
    if not sidecar.exists():
        raise RuntimeError(f"missing sha256 sidecar: {sidecar}")
    expected = sidecar.read_text().strip()
    observed = sha256_file(path)
    if observed != expected:
        raise RuntimeError(f"sha256 mismatch for {path}: {observed} != {expected}")


def write_json_with_sidecar(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    path.with_suffix(path.suffix + ".sha256").write_text(
        sha256_file(path) + "\n",
        encoding="utf-8")


def padded_expected_segments(expected_bytes: int) -> int:
    # Collaboration large Data is encrypted and then segmented, so leave a small
    # envelope margin when planning exact segment fetches.
    return max(1, math.ceil((expected_bytes + 512) / MAX_SEGMENT_SIZE))


def apply_activation_padding(out_dir: Path, activation_pad_bytes: int) -> None:
    if activation_pad_bytes <= 0:
        return

    manifest_path = out_dir / "service-manifest.json"
    plan_path = out_dir / "native-execution-plan.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    plan = json.loads(plan_path.read_text(encoding="utf-8"))

    service_manifest = next(
        item for item in manifest["services"] if item["name"] == SERVICE)
    service_plan = next(
        item for item in plan["services"] if item["service"] == SERVICE)

    for artifact in service_manifest["artifacts"]:
        if artifact["role"] != "/Backbone":
            continue
        metadata = dict(artifact.get("metadata") or {})
        metadata["outputBundlePadBytes"] = str(activation_pad_bytes)
        metadata["outputBundlePadTensor"] = PADDING_TENSOR
        artifact["metadata"] = metadata

    for dep in service_plan["dependencies"]:
        if dep.get("producers") != ["/Backbone"]:
            continue
        tensors = list(dep.get("tensors") or [])
        if PADDING_TENSOR not in tensors:
            tensors.append(PADDING_TENSOR)
        dep["tensors"] = tensors
        expected_bytes = int(dep.get("expectedBytes", 0) or 0) + activation_pad_bytes
        dep["expectedBytes"] = expected_bytes
        dep["expectedSegments"] = padded_expected_segments(expected_bytes)
        dep["segmentNaming"] = {
            "mode": "ndn-segment-component",
            "staticSegmentCount": dep["expectedSegments"],
            "dynamicFallback": False,
        }

    service_manifest.setdefault("metadata", {})["activationPadBytes"] = activation_pad_bytes
    service_plan.setdefault("metadata", {})["activationPadBytes"] = activation_pad_bytes
    write_json_with_sidecar(manifest_path, manifest)
    write_json_with_sidecar(plan_path, plan)


def apply_role_execution_delay(out_dir: Path, role_execution_delay_ms: float) -> None:
    if role_execution_delay_ms <= 0.0:
        return

    manifest_path = out_dir / "service-manifest.json"
    plan_path = out_dir / "native-execution-plan.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    plan = json.loads(plan_path.read_text(encoding="utf-8"))

    service_manifest = next(
        item for item in manifest["services"] if item["name"] == SERVICE)
    service_plan = next(
        item for item in plan["services"] if item["service"] == SERVICE)
    delay_text = f"{role_execution_delay_ms:.3f}".rstrip("0").rstrip(".")

    for artifact in service_manifest["artifacts"]:
        if artifact["role"] not in REQUIRED_ROLES:
            continue
        metadata = dict(artifact.get("metadata") or {})
        metadata["executionDelayMs"] = delay_text
        artifact["metadata"] = metadata

    service_manifest.setdefault("metadata", {})["roleExecutionDelayMs"] = role_execution_delay_ms
    service_plan.setdefault("metadata", {})["roleExecutionDelayMs"] = role_execution_delay_ms
    write_json_with_sidecar(manifest_path, manifest)
    write_json_with_sidecar(plan_path, plan)


def ensure_qwen_artifacts(config_path: Path) -> None:
    if config_path.resolve() != CONFIG_FILE.resolve():
        return
    required = [
        ROOT / "artifacts/qwen-native-tracer-backbone.onnx",
        ROOT / "artifacts/qwen-native-tracer-head0.onnx",
        ROOT / "artifacts/qwen-native-tracer-head1.onnx",
        ROOT / "artifacts/qwen-native-tracer-merge.onnx",
    ]
    if all(path.exists() and path.with_suffix(path.suffix + ".sha256").exists()
           for path in required):
        return
    subprocess.run(
        ["python3", str(GENERATE_QWEN_ARTIFACTS)],
        cwd=str(ROOT),
        check=True,
    )


def validate_bundle(out_dir: Path) -> dict:
    manifest_path = out_dir / "service-manifest.json"
    plan_path = out_dir / "native-execution-plan.json"
    for path in (manifest_path, plan_path):
        if not path.exists():
            raise RuntimeError(f"missing generated file: {path}")
        verify_sidecar(path)

    manifest = json.loads(manifest_path.read_text())
    plan = json.loads(plan_path.read_text())

    service_manifest = next(
        item for item in manifest["services"] if item["name"] == SERVICE)
    service_plan = next(
        item for item in plan["services"] if item["service"] == SERVICE)

    roles = set(service_plan["roles"])
    if roles != REQUIRED_ROLES:
        raise RuntimeError(f"unexpected tracer roles: {sorted(roles)}")
    if service_plan.get("modelFamily") != "yolo-onnx":
        raise RuntimeError("native plan modelFamily must be yolo-onnx")
    if service_plan.get("modelFormat") != "onnx":
        raise RuntimeError("native plan modelFormat must be onnx")
    if service_plan.get("plannerKind") != "yolo-detect-auto":
        raise RuntimeError("native plan plannerKind must be yolo-detect-auto")

    dependencies = service_plan["dependencies"]
    scopes_by_producer: dict[str, set[str]] = {role: set() for role in REQUIRED_ROLES}
    for dep in dependencies:
        scope = dep["keyScope"]
        if not dep["consumers"]:
            raise RuntimeError(
                f"dependency {scope} has no consumers; final responses belong "
                "in runner metadata, not dependency edges")
        for producer in dep["producers"]:
            scopes_by_producer.setdefault(producer, set()).add(scope)
        if not dep.get("objectNameTemplate"):
            raise RuntimeError(f"dependency {scope} must carry objectNameTemplate")
        if int(dep.get("expectedSegments", 0)) <= 0:
            raise RuntimeError(f"dependency {scope} must declare expectedSegments")

    for role, required_scopes in REQUIRED_OUTPUTS.items():
        observed = scopes_by_producer.get(role, set())
        if not required_scopes.issubset(observed):
            raise RuntimeError(
                f"role {role} missing output scopes: {sorted(required_scopes - observed)}")

    artifacts = service_manifest["artifacts"]
    artifact_roles = {item["role"] for item in artifacts}
    if artifact_roles != REQUIRED_ROLES:
        raise RuntimeError(f"unexpected artifact roles: {sorted(artifact_roles)}")
    for artifact in artifacts:
        path = resolve_artifact_path(artifact["path"])
        if not path.exists():
            raise RuntimeError(f"missing tracer artifact: {path}")
        metadata = dict(artifact.get("metadata") or {})
        if artifact["role"] == FINAL_RESPONSE_ROLE and FINAL_RESPONSE_SCOPE not in metadata.values():
            raise RuntimeError("merge artifact metadata must expose final-response scope")
        for scope in REQUIRED_OUTPUTS.get(artifact["role"], set()):
            if scope not in metadata.values() and not (
                metadata.get("forceOutputBundle") and metadata.get("outputBundleScope")
            ):
                raise RuntimeError(
                    f"artifact metadata for {artifact['role']} does not expose {scope}")

    return {
        "service": SERVICE,
        "roles": len(roles),
        "dependencies": len(dependencies),
        "artifacts": len(artifacts),
        "manifest": str(manifest_path),
        "nativePlan": str(plan_path),
        "manifestSha256": sha256_file(manifest_path),
        "nativePlanSha256": sha256_file(plan_path),
    }


def generate_optimization_evidence(out_dir: Path,
                                   runtime_candidate: str,
                                   provider_profiles_json: str,
                                   activation_pad_bytes: int,
                                   role_execution_delay_ms: float,
                                   workload_concurrency: int,
                                   target_rps: float) -> dict:
    optimization_path = out_dir / "planner-optimization.json"
    optimization_csv = out_dir / "planner-optimization.csv"
    subprocess.run(
        [
            "python3", str(OPTIMIZE_NATIVE_TRACER),
            "--plan", str(out_dir / "native-execution-plan.json"),
            "--manifest", str(out_dir / "service-manifest.json"),
            "--out", str(optimization_path),
            "--csv-out", str(optimization_csv),
            "--runtime-candidate", runtime_candidate,
            "--workload-concurrency", str(workload_concurrency),
            "--target-rps", str(target_rps),
        ] + (
            ["--provider-profiles-json", provider_profiles_json]
            if provider_profiles_json else []
        ),
        cwd=str(ROOT),
        check=True,
    )
    evidence = json.loads(optimization_path.read_text(encoding="utf-8"))
    if evidence.get("contractVersion") != "di-plan-v2":
        raise RuntimeError("planner optimization evidence must use di-plan-v2")
    if evidence.get("service") != SERVICE:
        raise RuntimeError("planner optimization evidence service mismatch")
    if not evidence.get("modelUnchanged"):
        raise RuntimeError("planner optimization evidence changed the Qwen NativeTracer model")
    candidates = evidence.get("candidates", [])
    if len(candidates) < 5:
        raise RuntimeError("planner optimization evidence must include at least five candidates")
    selected = evidence.get("selectedCandidate", {})
    if selected.get("id") != runtime_candidate:
        raise RuntimeError(
            f"unexpected selected NativeTracer candidate: {selected.get('id')}")
    return {
        "optimizationEvidence": str(optimization_path),
        "optimizationEvidenceCsv": str(optimization_csv),
        "optimizationEvidenceSha256": sha256_file(optimization_path),
        "optimizationEvidenceCsvSha256": sha256_file(optimization_csv),
        "optimizationContractVersion": evidence["contractVersion"],
        "candidateCount": evidence["candidateCount"],
        "selectedCandidate": selected["id"],
        "selectedCandidateEstimatedMs": selected["totalEstimatedMs"],
        "plannerRecommendedCandidate": evidence["plannerRecommendedCandidate"]["id"],
        "plannerRecommendedCandidateEstimatedMs": (
            evidence["plannerRecommendedCandidate"]["totalEstimatedMs"]),
        "bestEstimatedCandidate": evidence["bestEstimatedCandidate"]["id"],
        "activationPadBytes": activation_pad_bytes,
        "roleExecutionDelayMs": role_execution_delay_ms,
        "workloadConcurrency": workload_concurrency,
        "targetRps": target_rps,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="/tmp/ndnsf-di-native-tracer")
    parser.add_argument("--config", default=str(CONFIG_FILE))
    parser.add_argument("--summary-json", default="")
    parser.add_argument("--runtime-candidate", default="shared-backbone-current",
                        choices=["shared-backbone-current", "single-provider-serial"])
    parser.add_argument("--provider-profiles-json", default="")
    parser.add_argument("--activation-pad-bytes", type=int, default=0)
    parser.add_argument("--role-execution-delay-ms", type=float, default=0.0)
    parser.add_argument("--workload-concurrency", type=int, default=1)
    parser.add_argument("--target-rps", type=float, default=0.0)
    args = parser.parse_args(argv)
    if args.activation_pad_bytes < 0:
        raise SystemExit("--activation-pad-bytes must be non-negative")
    if args.role_execution_delay_ms < 0:
        raise SystemExit("--role-execution-delay-ms must be non-negative")
    if args.workload_concurrency <= 0:
        raise SystemExit("--workload-concurrency must be positive")
    if args.target_rps < 0.0:
        raise SystemExit("--target-rps must be non-negative")

    out_dir = Path(args.out)
    ensure_qwen_artifacts(Path(args.config))
    deployment = write_policy_bundle(args.config, out_dir)
    apply_activation_padding(out_dir, args.activation_pad_bytes)
    apply_role_execution_delay(out_dir, args.role_execution_delay_ms)
    summary = validate_bundle(out_dir)
    summary.update(generate_optimization_evidence(
        out_dir,
        args.runtime_candidate,
        args.provider_profiles_json,
        args.activation_pad_bytes,
        args.role_execution_delay_ms,
        args.workload_concurrency,
        args.target_rps))
    summary.update({
        "controllerPolicy": deployment.policy_file,
        "trustSchema": deployment.trust_schema,
    })

    if args.summary_json:
        Path(args.summary_json).write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n")

    print("NDNSF_DI_NATIVE_TRACER_POLICY_OK")
    print("service:", summary["service"])
    print("roles:", summary["roles"])
    print("dependencies:", summary["dependencies"])
    print("artifacts:", summary["artifacts"])
    print("native plan:", summary["nativePlan"])
    print("manifest:", summary["manifest"])
    print("optimization evidence:", summary["optimizationEvidence"])
    print("selected candidate:", summary["selectedCandidate"])
    print()
    print("C++ smoke commands:")
    print(
        "./build/examples/di-native-plan-schema-smoke "
        f"{summary['nativePlan']} {SERVICE} yolo-onnx onnx yolo-detect-auto")
    print(
        "./build/examples/di-native-plan-manifest-smoke "
        f"{summary['nativePlan']} {summary['manifest']} {SERVICE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
