#!/usr/bin/env python3
"""Build immutable, minimal D2a/D2b/D2h workload bundles for one SIF hash."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import tempfile
from pathlib import Path
from typing import NamedTuple
from urllib.parse import quote


ARTIFACTS = (
    "qwen-native-tracer-backbone.onnx",
    "qwen-native-tracer-head0.onnx",
    "qwen-native-tracer-head1.onnx",
    "qwen-native-tracer-merge.onnx",
)
ROOT = Path(__file__).resolve().parents[3]
NATIVE_USER_DRIVER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py"
)
DATA_V1_OBJECT_TEMPLATE = (
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/"
    "{keyScope}/{producerRole}"
)


class Gate(NamedTuple):
    service: str
    providers: tuple[str, ...]
    provider_roles: tuple[tuple[str, ...], ...]
    workload_files: tuple[str, ...]
    artifacts: tuple[str, ...]
    tcp_port: int
    tcp_listen: bool


GATES = {
    "d2a": Gate(
        service="/Inference/Spec170LocalTwoGpu",
        providers=("local-two-gpu",),
        provider_roles=((
            "/D2A/Independent/0",
            "/D2A/Independent/1",
            "/D2A/LocalGroup#0",
            "/D2A/LocalGroup#1",
        ),),
        workload_files=(
            "d2a-local-two-gpu.sh",
            "spec170_v3_local_two_gpu_provider.py",
            "spec170_v3_local_two_gpu_user.py",
        ),
        artifacts=(ARTIFACTS[0],),
        tcp_port=6363,
        tcp_listen=False,
    ),
    "d2b": Gate(
        service="/Inference/Spec170Collective",
        providers=("rank0", "rank1"),
        provider_roles=(("/Backbone",), ("/Head/Shard/0",)),
        workload_files=(
            "d2b-cross-provider.sh",
        ),
        artifacts=(ARTIFACTS[0], ARTIFACTS[1]),
        tcp_port=6363,
        tcp_listen=True,
    ),
    "d2h": Gate(
        service="/Inference/Spec170Hybrid",
        providers=("p0", "p1"),
        provider_roles=(
            ("/Pipeline/S0/R0", "/Pipeline/S1/R0", "/Pipeline/S2/R0"),
            ("/Pipeline/S0/R1", "/Pipeline/S1/R1", "/Pipeline/S2/R0",
             "/Pipeline/S2/R1"),
        ),
        workload_files=(
            "d2h-hybrid.sh",
            "spec170_v3_hybrid_provider.py",
            "spec170_v3_hybrid_user.py",
        ),
        artifacts=ARTIFACTS,
        tcp_port=6363,
        tcp_listen=True,
    ),
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _require_file(path: Path) -> Path:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"required regular file is missing: {path}")
    return path


def _copy(source: Path, destination: Path) -> None:
    _require_file(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def _write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _prefixed_file_digest(path: Path) -> str:
    return "sha256:" + _sha256(path)


def _d2b_dependency() -> dict[str, object]:
    source_layout = "sha256:" + hashlib.sha256(
        b"spec170-d2b-backbone-source-layout-v1").hexdigest()
    target_layout = source_layout
    tensor_digest = "sha256:" + hashlib.sha256(
        b"spec170-d2b-features-tensor-v1").hexdigest()
    return {
        "producers": ["/Backbone"],
        "consumers": ["/Head/Shard/0"],
        "keyScope": "backbone-to-head0",
        "topicPrefix": "/activation",
        "objectNameTemplate": DATA_V1_OBJECT_TEMPLATE,
        "required": True,
        "expectedSegments": 0,
        "expectedBytes": 0,
        "tensors": ["features"],
        "transportProfile": "NDNSF_DATA_V1",
        "collectiveOperationIndex": 0,
        "collectiveProducerRank": "0",
        "collectiveSourceLayoutDigest": source_layout,
        "collectiveTargetLayoutDigest": target_layout,
        "collectiveTensorDigest": tensor_digest,
    }


def _manifest_dependency(value: dict[str, object]) -> dict[str, object]:
    aliases = {
        "keyScope": "key_scope",
        "topicPrefix": "topic_prefix",
        "objectNameTemplate": "object_name_template",
        "expectedSegments": "expected_segments",
        "expectedBytes": "expected_bytes",
    }
    return {aliases.get(key, key): item for key, item in value.items()}


def _write_d2b_native_contract(bundle: Path, native_user_driver: Path) -> None:
    _copy(native_user_driver, bundle / "user_driver.py")
    (bundle / "assignment.csv").write_text(
        "assignment,role,provider,service\n"
        "d2b-static,/Backbone,/NDNSF-DI/Tracer/provider/rank0,"
        "/Inference/Spec170Collective\n"
        "d2b-static,/Head/Shard/0,/NDNSF-DI/Tracer/provider/rank1,"
        "/Inference/Spec170Collective\n",
        encoding="utf-8",
    )
    dependency = _d2b_dependency()
    service = GATES["d2b"].service
    roles = ["/Backbone", "/Head/Shard/0"]
    plan = {
        "version": 2,
        "services": [{
            "service": service,
            "model": "/Model/Spec170/D2b/v1",
            "modelFamily": "spec170-d2b-control",
            "modelFormat": "onnx",
            "plannerKind": "sealed-d2b-profile",
            "runtimeBackend": "onnxruntime",
            "schemaVersion": 2,
            "executionMode": "native-d2b",
            "executionPolicy": "DATA_DRIVEN_V2",
            "roles": roles,
            "roleMetadata": {
                "/Backbone": {"stage": 0, "rank": 0},
                "/Head/Shard/0": {"stage": 1, "rank": 1},
            },
            "dependencies": [dependency],
        }],
    }
    backbone = bundle / "artifacts" / ARTIFACTS[0]
    head0 = bundle / "artifacts" / ARTIFACTS[1]
    common = {
        "executionProvider": "cuda",
        "deviceId": "0",
        "allowCpuFallback": "false",
        "forceOutputBundle": True,
        "sourceModel": "Spec170 deterministic D2b control model",
    }
    manifest = {
        "services": [{
            "name": service,
            "model": "/Model/Spec170/D2b/v1",
            "roles": roles,
            "dependencies": [_manifest_dependency(dependency)],
            "artifacts": [
                {
                    "artifact": "/Artifact/Spec170/D2b/Backbone",
                    "backend": "onnxruntime",
                    "filename": backbone.name,
                    "kind": "model",
                    "metadata": {
                        **common,
                        "fragmentDigest": _prefixed_file_digest(backbone),
                        "input_tensors": "images",
                        "output_tensors": "features",
                        "outputBundleScope": "backbone-to-head0",
                        "outputScope.0": "backbone-to-head0",
                    },
                    "path": "artifacts/" + backbone.name,
                    "role": "/Backbone",
                },
                {
                    "artifact": "/Artifact/Spec170/D2b/Head0",
                    "backend": "onnxruntime",
                    "filename": head0.name,
                    "kind": "model",
                    "metadata": {
                        **common,
                        "fragmentDigest": _prefixed_file_digest(head0),
                        "input_tensors": "features",
                        "inputScope.features": "backbone-to-head0",
                        "output_tensors": "detections0",
                        "outputBundleScope": "final-response",
                        "outputScope.0": "final-response",
                    },
                    "path": "artifacts/" + head0.name,
                    "role": "/Head/Shard/0",
                },
            ],
            "input": {"codec": "tensor-bundle"},
            "output": {"codec": "tensor-bundle"},
            "metadata": {
                "diPlanVersion": "di-plan-v2",
                "executionMode": "native-d2b",
                "executionPolicy": "DATA_DRIVEN_V2",
                "runtimeBackend": "onnxruntime",
                "transportProfile": "NDNSF_DATA_V1",
                "physicalGpuEvidence": True,
            },
        }],
    }
    _write_json(bundle / "native-execution-plan.json", plan)
    _write_json(bundle / "service-manifest.json", manifest)


def _render_nfd(template: str, gate: str, port: int, tcp_listen: bool) -> str:
    rendered = (template
                .replace("@@NFD_SOCKET@@", "/scratch/run/nfd.sock")
                .replace("@@TCP_PORT@@", str(port))
                .replace("@@NODE_RANK@@", gate))
    if "@@" in rendered:
        raise ValueError(f"unresolved NFD placeholder for {gate}")
    tcp_start = rendered.find("\n  tcp\n  {")
    tcp_end = rendered.find("\n  }", tcp_start + 1)
    if tcp_start < 0 or tcp_end < 0:
        raise ValueError(f"TCP face-system section is missing for {gate}")
    tcp_section = rendered[tcp_start:tcp_end]
    if not tcp_listen:
        if tcp_section.count("listen yes") != 1:
            raise ValueError(f"TCP listener is not uniquely configurable for {gate}")
        tcp_section = tcp_section.replace("listen yes", "listen no", 1)
        rendered = rendered[:tcp_start] + tcp_section + rendered[tcp_end:]
    expected = "listen yes" if tcp_listen else "listen no"
    if expected not in tcp_section:
        raise ValueError(f"TCP listener state was not materialized for {gate}")
    return rendered


def _render_policy(gate: Gate) -> str:
    if len(gate.providers) != len(gate.provider_roles):
        raise ValueError("provider-role policy shape mismatch")
    providers = "\n".join(
        "    provider-policy\n"
        "    {\n"
        f"        for /NDNSF-DI/Tracer/provider/{identity}\n"
        "        allow\n"
        "        {\n"
        f"            {gate.service}\n"
        + "".join(
            f"            {gate.service}/ROLE{quote(role, safe='/')}\n"
            for role in roles
        )
        +
        "        }\n"
        "    }"
        for identity, roles in zip(gate.providers, gate.provider_roles)
    )
    return (
        "name /NDNSF-DI/Tracer/controller/NDNSF/ControllerPolicy/v1\n\n"
        "provider-policies\n{\n"
        f"{providers}\n"
        "}\n\n"
        "user-policies\n{\n"
        "    user-policy\n"
        "    {\n"
        "        for /NDNSF-DI/Tracer/user\n"
        "        allow\n"
        "        {\n"
        f"            {gate.service}\n"
        "        }\n"
        "    }\n"
        "}\n"
    )


def _file_manifest(bundle: Path) -> dict[str, dict[str, int | str]]:
    result: dict[str, dict[str, int | str]] = {}
    for path in sorted(bundle.rglob("*")):
        if not path.is_file() or path.name == "bundle-manifest.json":
            continue
        relative = path.relative_to(bundle).as_posix()
        result[relative] = {"sha256": _sha256(path), "bytes": path.stat().st_size}
    return result


def build_bundles(
        *,
        base_bundle: Path,
        workloads: Path,
        output_root: Path,
        sif_sha256: str,
        release_id: str,
        native_user_driver: Path = NATIVE_USER_DRIVER,
) -> dict[str, Path]:
    base_bundle = base_bundle.resolve()
    workloads = workloads.resolve()
    output_root = output_root.resolve()
    if not re.fullmatch(r"[0-9a-f]{64}", sif_sha256):
        raise ValueError("SIF SHA-256 must be 64 lower-hex characters")
    if not release_id or not re.fullmatch(r"[A-Za-z0-9._-]+", release_id):
        raise ValueError("release ID is empty or unsafe")
    if output_root.exists():
        raise FileExistsError(f"refusing to overwrite immutable output: {output_root}")
    nfd_template = _require_file(base_bundle / "nfd.conf.in").read_text(
        encoding="utf-8")
    _require_file(base_bundle / "trust-schema.conf")
    _require_file(native_user_driver)
    for gate in GATES.values():
        for name in gate.workload_files:
            _require_file(workloads / name)
        for name in gate.artifacts:
            _require_file(base_bundle / "artifacts" / name)

    output_root.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(
        prefix=output_root.name + ".tmp-", dir=output_root.parent))
    built: dict[str, Path] = {}
    try:
        for name, gate in GATES.items():
            bundle = staging / name
            bundle.mkdir()
            _copy(base_bundle / "trust-schema.conf", bundle / "trust-schema.conf")
            (bundle / "nfd.conf").write_text(
                _render_nfd(
                    nfd_template, name, gate.tcp_port, gate.tcp_listen),
                encoding="utf-8")
            (bundle / "controller.policies").write_text(
                _render_policy(gate), encoding="utf-8")
            for filename in gate.workload_files:
                _copy(workloads / filename, bundle / filename)
            for filename in gate.artifacts:
                _copy(base_bundle / "artifacts" / filename,
                      bundle / "artifacts" / filename)
            if name == "d2b":
                _write_d2b_native_contract(bundle, native_user_driver)
            sources = {
                "base:nfd.conf.in": _sha256(base_bundle / "nfd.conf.in"),
                "base:trust-schema.conf": _sha256(
                    base_bundle / "trust-schema.conf"),
            }
            sources.update({
                f"workloads:{filename}": _sha256(workloads / filename)
                for filename in gate.workload_files
            })
            if name == "d2b":
                sources["native:user_driver.py"] = _sha256(native_user_driver)
            sources.update({
                f"base:artifacts/{filename}": _sha256(
                    base_bundle / "artifacts" / filename)
                for filename in gate.artifacts
            })
            manifest = {
                "schemaVersion": "spec170-d2-bundle-v1",
                "status": "PASS",
                "releaseId": release_id,
                "gate": name,
                "service": gate.service,
                "providers": [
                    f"/NDNSF-DI/Tracer/provider/{identity}"
                    for identity in gate.providers
                ],
                "sifSha256": sif_sha256,
                "workload": gate.workload_files[0],
                "builderSha256": _sha256(Path(__file__).resolve()),
                "sourceDigests": dict(sorted(sources.items())),
                "files": _file_manifest(bundle),
            }
            (bundle / "bundle-manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            built[name] = output_root / name
        os.rename(staging, output_root)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return built


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-bundle", type=Path, required=True)
    parser.add_argument("--workloads", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--sif-sha256", required=True)
    parser.add_argument("--release-id", required=True)
    args = parser.parse_args()
    built = build_bundles(
        base_bundle=args.base_bundle, workloads=args.workloads,
        output_root=args.output_root, sif_sha256=args.sif_sha256,
        release_id=args.release_id,
    )
    print(json.dumps({key: str(value) for key, value in built.items()}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
