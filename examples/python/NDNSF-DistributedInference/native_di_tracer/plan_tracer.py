#!/usr/bin/env python3
"""Generate and validate the native DI tracer policy bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from ndnsf_distributed_inference import write_policy_bundle


SERVICE = "/Inference/NativeTracer"
ROOT = Path(__file__).resolve().parent
CONFIG_FILE = ROOT / "native_tracer_policy.yaml"
GENERATE_QWEN_ARTIFACTS = ROOT / "generate_qwen_native_tracer_artifacts.py"
REQUIRED_ROLES = {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"}
REQUIRED_OUTPUTS = {
    "/Backbone": {"backbone-to-head0", "backbone-to-head1"},
    "/Head/Shard/0": {"head0-to-merge"},
    "/Head/Shard/1": {"head1-to-merge"},
}
FINAL_RESPONSE_ROLE = "/Merge"
FINAL_RESPONSE_SCOPE = "final-response"


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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="/tmp/ndnsf-di-native-tracer")
    parser.add_argument("--config", default=str(CONFIG_FILE))
    parser.add_argument("--summary-json", default="")
    args = parser.parse_args(argv)

    out_dir = Path(args.out)
    ensure_qwen_artifacts(Path(args.config))
    deployment = write_policy_bundle(args.config, out_dir)
    summary = validate_bundle(out_dir)
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
    print()
    print("C++ smoke commands:")
    print(
        "./build/examples/DI_NativePlanSchemaSmoke "
        f"{summary['nativePlan']} {SERVICE} yolo-onnx onnx yolo-detect-auto")
    print(
        "./build/examples/DI_NativePlanManifestSmoke "
        f"{summary['nativePlan']} {summary['manifest']} {SERVICE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
