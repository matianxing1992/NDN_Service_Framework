#!/usr/bin/env python3
"""Generate tiny production-ORT bundles for Spec170 hybrid MiniNDN gates."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.policy import write_policy_bundle  # noqa: E402


SERVICE = "/Inference/NativeTracer"
CONFIG = HERE / "native_tracer_policy.yaml"
PROVIDERS = (
    "/NDNSF-DI/Tracer/provider/hybrid0",
    "/NDNSF-DI/Tracer/provider/hybrid1",
    "/NDNSF-DI/Tracer/provider/hybrid2",
    "/NDNSF-DI/Tracer/provider/hybrid3",
    "/NDNSF-DI/Tracer/provider/hybrid4",
)
NODES = ("ucla", "arizona", "wustl", "neu", "memphis")
OBJECT_TEMPLATE = (
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/"
    "{keyScope}/{producerRole}"
)

# These tiny ONNX fixtures are generated from the functions represented by
# their keys and checked by tests with ONNX Runtime. Keeping the sealed bytes
# here makes bundle generation independent of the invoking user's site-packages
# (MiniNDN normally launches the harness through sudo/root).
MODEL_BLOBS = {
    "slice0_4": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6jQIKJAoGaW1hZ2VzEgRmbGF0IgdGbGF0dGVuKgsKBGF4aXMYAaABAgo2CgRmbGF0CgZzdGFydHMKBGVuZHMKBGF4ZXMKBXN0ZXBzEgxhY3RpdmF0aW9uLTAiBVNsaWNlEhBzcGVjMTcwLTEyMS1zMHIwKhYIARAHQgZzdGFydHNKCAAAAAAAAAAAKhQIARAHQgRlbmRzSggEAAAAAAAAACoUCAEQB0IEYXhlc0oIAQAAAAAAAAAqFQgBEAdCBXN0ZXBzSggBAAAAAAAAAFogCgZpbWFnZXMSFgoUCAESEAoCCAEKAggDCgIIAgoCCAJiHgoMYWN0aXZhdGlvbi0wEg4KDAgBEggKAggBCgIIBEIECgAQDA==",
    "identity2": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6egomCgxhY3RpdmF0aW9uLTASDGFjdGl2YXRpb24tMSIISWRlbnRpdHkSEHNwZWMxNzAtMTIxLXMxcjBaHgoMYWN0aXZhdGlvbi0wEg4KDAgBEggKAggBCgIIAmIeCgxhY3RpdmF0aW9uLTESDgoMCAESCAoCCAEKAggCQgQKABAM",
    "sum4": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6iwEKPgoMYWN0aXZhdGlvbi0xEgVmaW5hbCIJUmVkdWNlU3VtKgsKBGF4ZXNAAaABByoPCghrZWVwZGltcxgBoAECEhBzcGVjMTcwLTEyMS1zMnIwWh4KDGFjdGl2YXRpb24tMRIOCgwIARIICgIIAQoCCARiFwoFZmluYWwSDgoMCAESCAoCCAEKAggBQgQKABAM",
    "slice0_2": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6jQIKJAoGaW1hZ2VzEgRmbGF0IgdGbGF0dGVuKgsKBGF4aXMYAaABAgo2CgRmbGF0CgZzdGFydHMKBGVuZHMKBGF4ZXMKBXN0ZXBzEgxhY3RpdmF0aW9uLTAiBVNsaWNlEhBzcGVjMTcwLTIxMi1zMHIwKhYIARAHQgZzdGFydHNKCAAAAAAAAAAAKhQIARAHQgRlbmRzSggCAAAAAAAAACoUCAEQB0IEYXhlc0oIAQAAAAAAAAAqFQgBEAdCBXN0ZXBzSggBAAAAAAAAAFogCgZpbWFnZXMSFgoUCAESEAoCCAEKAggDCgIIAgoCCAJiHgoMYWN0aXZhdGlvbi0wEg4KDAgBEggKAggBCgIIAkIECgAQDA==",
    "slice2_4": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6jQIKJAoGaW1hZ2VzEgRmbGF0IgdGbGF0dGVuKgsKBGF4aXMYAaABAgo2CgRmbGF0CgZzdGFydHMKBGVuZHMKBGF4ZXMKBXN0ZXBzEgxhY3RpdmF0aW9uLTAiBVNsaWNlEhBzcGVjMTcwLTIxMi1zMHIxKhYIARAHQgZzdGFydHNKCAIAAAAAAAAAKhQIARAHQgRlbmRzSggEAAAAAAAAACoUCAEQB0IEYXhlc0oIAQAAAAAAAAAqFQgBEAdCBXN0ZXBzSggBAAAAAAAAAFogCgZpbWFnZXMSFgoUCAESEAoCCAEKAggDCgIIAgoCCAJiHgoMYWN0aXZhdGlvbi0wEg4KDAgBEggKAggBCgIIAkIECgAQDA==",
    "identity4": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6egomCgxhY3RpdmF0aW9uLTASDGFjdGl2YXRpb24tMSIISWRlbnRpdHkSEHNwZWMxNzAtMjEyLXMxcjBaHgoMYWN0aXZhdGlvbi0wEg4KDAgBEggKAggBCgIIBGIeCgxhY3RpdmF0aW9uLTESDgoMCAESCAoCCAEKAggEQgQKABAM",
    "sumadd": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU61AEKQgoMYWN0aXZhdGlvbi0xEglsb2NhbC1zdW0iCVJlZHVjZVN1bSoLCgRheGVzQAGgAQcqDwoIa2VlcGRpbXMYAaABAgokCglsb2NhbC1zdW0KC3BhcnRpYWwtc3VtEgVmaW5hbCIDQWRkEhBzcGVjMTcwLTIxMi1zMnIwWh4KDGFjdGl2YXRpb24tMRIOCgwIARIICgIIAQoCCAJaHQoLcGFydGlhbC1zdW0SDgoMCAESCAoCCAEKAggBYhcKBWZpbmFsEg4KDAgBEggKAggBCgIIAUIECgAQDA==",
    "sum2_partial": "CAkSGW5kbnNmLXNwZWMxNzAtaHlicmlkLWdhdGU6lwEKRAoMYWN0aXZhdGlvbi0xEgtwYXJ0aWFsLXN1bSIJUmVkdWNlU3VtKgsKBGF4ZXNAAaABByoPCghrZWVwZGltcxgBoAECEhBzcGVjMTcwLTIxMi1zMnIxWh4KDGFjdGl2YXRpb24tMRIOCgwIARIICgIIAQoCCAJiHQoLcGFydGlhbC1zdW0SDgoMCAESCAoCCAEKAggBQgQKABAM",
}


def _digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def _file_digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    path.with_suffix(path.suffix + ".sha256").write_text(
        hashlib.sha256(path.read_bytes()).hexdigest() + "\n",
        encoding="utf-8")


def _slice_model(path: Path, *, begin: int, end: int) -> None:
    key = {(0, 4): "slice0_4", (0, 2): "slice0_2", (2, 4): "slice2_4"}.get(
        (begin, end))
    if key is None:
        raise ValueError(f"unsupported sealed slice fixture: {begin}:{end}")
    _write_model_blob(path, key)


def _identity_model(path: Path, *, input_name: str, output_name: str,
                    width: int) -> None:
    if (input_name, output_name) != ("activation-0", "activation-1"):
        raise ValueError("unsupported sealed identity fixture names")
    _write_model_blob(path, {2: "identity2", 4: "identity4"}[width])


def _sum_model(path: Path, *, input_name: str, width: int,
               output_name: str = "final") -> None:
    key = {
        ("activation-1", 4, "final"): "sum4",
        ("activation-1", 2, "partial-sum"): "sum2_partial",
    }.get((input_name, width, output_name))
    if key is None:
        raise ValueError("unsupported sealed sum fixture")
    _write_model_blob(path, key)


def _sum_and_add_model(path: Path) -> None:
    _write_model_blob(path, "sumadd")


def _write_model_blob(path: Path, key: str) -> None:
    path.write_bytes(base64.b64decode(MODEL_BLOBS[key], validate=True))


def _redistribution(*, producers: list[int], consumers: list[int],
                    tensor: str, operation: str, source_layout: str,
                    target_layout: str, tensor_digest: str) -> dict[str, Any]:
    return {
        "producerRanks": producers,
        "consumerRanks": consumers,
        "tensor": tensor,
        "operation": operation,
        "epoch": "epoch-1",
        "integrityDigest": tensor_digest,
        "sourceLayoutDigest": source_layout,
        "targetLayoutDigest": target_layout,
        "axis": 1,
        "temporaryMemoryBytes": 65536,
        "completeOutput": True,
    }


def _dependency(*, producers: list[str], consumers: list[str], scope: str,
                tensor: str, operation_index: int, source_layout: str,
                target_layout: str, tensor_digest: str,
                redistribution: dict[str, Any] | None = None) -> dict[str, Any]:
    value: dict[str, Any] = {
        "producers": producers,
        "consumers": consumers,
        "keyScope": scope,
        "topicPrefix": "/activation",
        "objectNameTemplate": OBJECT_TEMPLATE,
        "required": True,
        "expectedSegments": 0,
        "expectedBytes": 0,
        "tensors": [tensor],
        "transportProfile": "NDNSF_DATA_V1",
        "collectiveOperationIndex": operation_index,
        "collectiveProducerRank": "0",
        "collectiveSourceLayoutDigest": source_layout,
        "collectiveTargetLayoutDigest": target_layout,
        "collectiveTensorDigest": tensor_digest,
    }
    if redistribution is not None:
        value["redistributions"] = [redistribution]
    return value


def _manifest_dependency(value: dict[str, Any]) -> dict[str, Any]:
    aliases = {
        "keyScope": "key_scope",
        "topicPrefix": "topic_prefix",
        "objectNameTemplate": "object_name_template",
        "expectedSegments": "expected_segments",
        "expectedBytes": "expected_bytes",
    }
    return {aliases.get(key, key): item for key, item in value.items()}


def _add_hybrid_provider_policies(path: Path,
                                  providers_by_role: dict[str, str]) -> None:
    roles_by_provider: dict[str, list[str]] = {}
    for role, provider in providers_by_role.items():
        roles_by_provider.setdefault(provider, []).append(role)
    blocks = []
    for provider, roles in sorted(roles_by_provider.items()):
        allowed = "\n".join(
            f"            {SERVICE}/ROLE/{role}" for role in roles)
        blocks.append(
            "    provider-policy\n"
            "    {\n"
            f"        for {provider}\n"
            "        allow\n"
            "        {\n"
            f"            {SERVICE}\n"
            f"{allowed}\n"
            "        }\n"
            "    }\n")
    text = path.read_text(encoding="utf-8")
    marker = "\n}\n\nuser-policies"
    if marker not in text:
        raise RuntimeError("controller policy provider section terminator missing")
    text = text.replace(marker, "\n" + "".join(blocks) + "}\n\nuser-policies", 1)
    path.write_text(text, encoding="utf-8")


def _artifact(role: str, path: Path, *, inputs: list[str], outputs: list[str],
              input_scopes: dict[str, str] | None = None,
              output_scope: str = "") -> dict[str, Any]:
    metadata: dict[str, Any] = {
        "fragmentDigest": _file_digest(path),
        "input_tensors": ",".join(inputs),
        "output_tensors": ",".join(outputs),
        "forceOutputBundle": True,
        "sourceModel": "Spec170 deterministic hybrid control model",
    }
    for name, scope in (input_scopes or {}).items():
        metadata[f"inputScope.{name}"] = scope
    if output_scope:
        metadata["outputBundleScope"] = output_scope
        metadata["outputScope.0"] = output_scope
    return {
        "artifact": "/Artifact/Spec170/Hybrid/" + role,
        "backend": "onnxruntime",
        "filename": path.name,
        "kind": "model",
        "metadata": metadata,
        "path": "artifacts/" + path.name,
        "role": role,
    }


def _profile_121(artifacts: Path) -> dict[str, Any]:
    roles = ["S0R0", "S1R0", "S1R1", "S2R0"]
    files = {role: artifacts / f"spec170-121-{role.lower()}.onnx" for role in roles}
    _slice_model(files["S0R0"], begin=0, end=4)
    _identity_model(files["S1R0"], input_name="activation-0",
                    output_name="activation-1", width=2)
    _identity_model(files["S1R1"], input_name="activation-0",
                    output_name="activation-1", width=2)
    _sum_model(files["S2R0"], input_name="activation-1", width=4)
    layouts = [_digest(f"121-layout-{index}") for index in range(3)]
    tensors = [_digest(f"121-tensor-{index}") for index in range(2)]
    dependencies = [
        _dependency(
            producers=["S0R0"], consumers=["S1R0", "S1R1"],
            scope="boundary-0", tensor="activation-0", operation_index=0,
            source_layout=layouts[0], target_layout=layouts[1],
            tensor_digest=tensors[0],
            redistribution=_redistribution(
                producers=[0], consumers=[1, 2], tensor="activation-0",
                operation="SCATTER", source_layout=layouts[0],
                target_layout=layouts[1], tensor_digest=tensors[0])),
        _dependency(
            producers=["S1R0", "S1R1"], consumers=["S2R0"],
            scope="boundary-1", tensor="activation-1", operation_index=1,
            source_layout=layouts[1], target_layout=layouts[2],
            tensor_digest=tensors[1],
            redistribution=_redistribution(
                producers=[1, 2], consumers=[3], tensor="activation-1",
                operation="GATHER", source_layout=layouts[1],
                target_layout=layouts[2], tensor_digest=tensors[1])),
    ]
    mappings = {
        "S0R0": PROVIDERS[0], "S1R0": PROVIDERS[1],
        "S1R1": PROVIDERS[2], "S2R0": PROVIDERS[3],
    }
    artifact_rows = [
        _artifact("S0R0", files["S0R0"], inputs=["images"],
                  outputs=["activation-0"], output_scope="boundary-0"),
        _artifact("S1R0", files["S1R0"], inputs=["activation-0"],
                  outputs=["activation-1"],
                  input_scopes={"activation-0": "boundary-0"},
                  output_scope="boundary-1"),
        _artifact("S1R1", files["S1R1"], inputs=["activation-0"],
                  outputs=["activation-1"],
                  input_scopes={"activation-0": "boundary-0"},
                  output_scope="boundary-1"),
        _artifact("S2R0", files["S2R0"], inputs=["activation-1"],
                  outputs=["final"],
                  input_scopes={"activation-1": "boundary-1"},
                  output_scope="final-response"),
    ]
    steps = [
        _onnx_step(files["S0R0"], {"images": "images"},
                   {"activation-0": "121-stage0"}),
        {"kind": "scatter", "source": "121-stage0", "axis": 1,
         "targets": ["121-rank0", "121-rank1"]},
        _onnx_step(files["S1R0"], {"activation-0": "121-rank0"},
                   {"activation-1": "121-stage1-r0"}),
        _onnx_step(files["S1R1"], {"activation-0": "121-rank1"},
                   {"activation-1": "121-stage1-r1"}),
        {"kind": "gather", "sources": ["121-stage1-r0", "121-stage1-r1"],
         "axis": 1, "target": "121-stage1"},
        _onnx_step(files["S2R0"], {"activation-1": "121-stage1"},
                   {"final": "final"}),
    ]
    return _profile(roles, dependencies, mappings, artifact_rows, steps, "final")


def _profile_212(artifacts: Path) -> dict[str, Any]:
    roles = ["S0R0", "S0R1", "S1R0", "S2R0", "S2R1"]
    files = {role: artifacts / f"spec170-212-{role.lower()}.onnx" for role in roles}
    _slice_model(files["S0R0"], begin=0, end=2)
    _slice_model(files["S0R1"], begin=2, end=4)
    _identity_model(files["S1R0"], input_name="activation-0",
                    output_name="activation-1", width=4)
    _sum_and_add_model(files["S2R0"])
    _sum_model(files["S2R1"], input_name="activation-1", width=2,
               output_name="partial-sum")
    layouts = [_digest(f"212-layout-{index}") for index in range(3)]
    tensors = [_digest(f"212-tensor-{index}") for index in range(3)]
    dependencies = [
        _dependency(
            producers=["S0R0", "S0R1"], consumers=["S1R0"],
            scope="boundary-0", tensor="activation-0", operation_index=0,
            source_layout=layouts[0], target_layout=layouts[1],
            tensor_digest=tensors[0],
            redistribution=_redistribution(
                producers=[0, 1], consumers=[2], tensor="activation-0",
                operation="GATHER", source_layout=layouts[0],
                target_layout=layouts[1], tensor_digest=tensors[0])),
        _dependency(
            producers=["S1R0"], consumers=["S2R0", "S2R1"],
            scope="boundary-1", tensor="activation-1", operation_index=1,
            source_layout=layouts[1], target_layout=layouts[2],
            tensor_digest=tensors[1],
            redistribution=_redistribution(
                producers=[2], consumers=[3, 4], tensor="activation-1",
                operation="SCATTER", source_layout=layouts[1],
                target_layout=layouts[2], tensor_digest=tensors[1])),
        _dependency(
            producers=["S2R1"], consumers=["S2R0"],
            scope="boundary-2", tensor="partial-sum", operation_index=2,
            source_layout=layouts[2], target_layout=layouts[2],
            tensor_digest=tensors[2]),
    ]
    mappings = {
        "S0R0": PROVIDERS[0], "S0R1": PROVIDERS[1],
        "S1R0": PROVIDERS[2], "S2R0": PROVIDERS[3],
        "S2R1": PROVIDERS[4],
    }
    artifact_rows = [
        _artifact("S0R0", files["S0R0"], inputs=["images"],
                  outputs=["activation-0"], output_scope="boundary-0"),
        _artifact("S0R1", files["S0R1"], inputs=["images"],
                  outputs=["activation-0"], output_scope="boundary-0"),
        _artifact("S1R0", files["S1R0"], inputs=["activation-0"],
                  outputs=["activation-1"],
                  input_scopes={"activation-0": "boundary-0"},
                  output_scope="boundary-1"),
        _artifact("S2R0", files["S2R0"],
                  inputs=["activation-1", "partial-sum"], outputs=["final"],
                  input_scopes={"activation-1": "boundary-1",
                                "partial-sum": "boundary-2"},
                  output_scope="final-response"),
        _artifact("S2R1", files["S2R1"], inputs=["activation-1"],
                  outputs=["partial-sum"],
                  input_scopes={"activation-1": "boundary-1"},
                  output_scope="boundary-2"),
    ]
    steps = [
        _onnx_step(files["S0R0"], {"images": "images"},
                   {"activation-0": "212-stage0-r0"}),
        _onnx_step(files["S0R1"], {"images": "images"},
                   {"activation-0": "212-stage0-r1"}),
        {"kind": "gather", "sources": ["212-stage0-r0", "212-stage0-r1"],
         "axis": 1, "target": "212-stage0"},
        _onnx_step(files["S1R0"], {"activation-0": "212-stage0"},
                   {"activation-1": "212-stage1"}),
        {"kind": "scatter", "source": "212-stage1", "axis": 1,
         "targets": ["212-stage2-r0", "212-stage2-r1"]},
        _onnx_step(files["S2R1"], {"activation-1": "212-stage2-r1"},
                   {"partial-sum": "212-partial"}),
        _onnx_step(files["S2R0"],
                   {"activation-1": "212-stage2-r0",
                    "partial-sum": "212-partial"},
                   {"final": "final"}),
    ]
    return _profile(roles, dependencies, mappings, artifact_rows, steps, "final")


def _onnx_step(path: Path, inputs: dict[str, str],
               outputs: dict[str, str]) -> dict[str, Any]:
    return {
        "kind": "onnx",
        "artifactPath": "artifacts/" + path.name,
        "inputs": inputs,
        "outputs": outputs,
    }


def _profile(roles: list[str], dependencies: list[dict[str, Any]],
             mappings: dict[str, str], artifacts: list[dict[str, Any]],
             steps: list[dict[str, Any]], final_tensor: str) -> dict[str, Any]:
    return {
        "roles": roles,
        "dependencies": dependencies,
        "providersByRole": mappings,
        "artifacts": artifacts,
        "oracleSteps": steps,
        "finalTensor": final_tensor,
    }


def generate_hybrid_native_bundle(output: Path, profile: str,
                                  role_execution_delay_ms: float = 0.0) -> dict[str, Any]:
    profile = str(profile).strip()
    if profile not in {"121", "212"}:
        raise ValueError(f"unsupported hybrid profile: {profile}")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    write_policy_bundle(CONFIG, output)
    artifacts = output / "artifacts"
    if artifacts.exists():
        shutil.rmtree(artifacts)
    artifacts.mkdir()

    profile_data = _profile_121(artifacts) if profile == "121" else _profile_212(artifacts)
    _add_hybrid_provider_policies(
        output / "controller.policies", profile_data["providersByRole"])
    roles = profile_data["roles"]
    dependencies = profile_data["dependencies"]
    role_metadata = {
        role: {"stage": int(role[1]), "rank": int(role[3])}
        for role in roles
    }
    delay_text = f"{role_execution_delay_ms:.3f}".rstrip("0").rstrip(".")
    if role_execution_delay_ms > 0.0:
        for artifact in profile_data["artifacts"]:
            metadata = dict(artifact.get("metadata") or {})
            metadata["executionDelayMs"] = delay_text
            artifact["metadata"] = metadata
    plan = {
        "version": 2,
        "services": [{
            "service": SERVICE,
            "model": f"/Model/Spec170/Hybrid/{profile}/v1",
            "modelFamily": "spec170-hybrid-control",
            "modelFormat": "onnx",
            "plannerKind": "sealed-hybrid-profile",
            "runtimeBackend": "onnxruntime",
            "schemaVersion": 2,
            "executionMode": "native-hybrid",
            "executionPolicy": "DATA_DRIVEN_V2",
            "roles": roles,
            "roleMetadata": role_metadata,
            "dependencies": dependencies,
        }],
    }
    manifest = {
        "services": [{
            "name": SERVICE,
            "model": f"/Model/Spec170/Hybrid/{profile}/v1",
            "roles": roles,
            "dependencies": [_manifest_dependency(dep) for dep in dependencies],
            "artifacts": profile_data["artifacts"],
            "input": {"codec": "tensor-bundle"},
            "output": {"codec": "tensor-bundle",
                       "fields": {"final": {"dtype": "float32"}}},
            "metadata": {
                "diPlanVersion": "di-plan-v2",
                "executionMode": "native-hybrid",
                "executionPolicy": "DATA_DRIVEN_V2",
                "hybridProfile": profile,
                "runtimeBackend": "onnxruntime",
                "physicalGpuEvidence": False,
                **({"roleExecutionDelayMs": role_execution_delay_ms}
                   if role_execution_delay_ms > 0.0 else {}),
            },
        }],
    }
    if role_execution_delay_ms > 0.0:
        plan["services"][0]["metadata"] = {
            "roleExecutionDelayMs": role_execution_delay_ms,
        }
    _write_json(output / "native-execution-plan.json", plan)
    _write_json(output / "service-manifest.json", manifest)

    rows = []
    for role in roles:
        provider = profile_data["providersByRole"][role]
        index = PROVIDERS.index(provider)
        rows.append({
            "assignment": f"hybrid-{profile}",
            "role": role,
            "provider": provider,
            "node": NODES[index],
            "service": SERVICE,
        })
    summary = {
        "schema": "ndnsf-spec170-hybrid-native-bundle-v1",
        "profile": profile,
        "service": SERVICE,
        "roles": roles,
        "providersByRole": profile_data["providersByRole"],
        "artifactDigestsByRole": {
            item["role"]: item["metadata"]["fragmentDigest"]
            for item in profile_data["artifacts"]
        },
        "assignmentRows": rows,
        "dependencyCount": len(dependencies),
        "transportProfile": "NDNSF_DATA_V1",
        "modelFamily": "spec170-hybrid-control",
        "modelFormat": "onnx",
        "plannerKind": "sealed-hybrid-profile",
        "runtimeBackend": "onnxruntime",
        "oracleSteps": profile_data["oracleSteps"],
        "finalTensor": profile_data["finalTensor"],
        "expectedOutput": [0.6000000238418579],
        "roleExecutionDelayMs": role_execution_delay_ms,
        "nativePlan": str(output / "native-execution-plan.json"),
        "manifest": str(output / "service-manifest.json"),
    }
    _write_json(output / "hybrid-bundle-summary.json", summary)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--profile", required=True, choices=("121", "212"))
    parser.add_argument("--role-execution-delay-ms", type=float, default=0.0)
    parser.add_argument("--summary-json", type=Path)
    args = parser.parse_args()
    if args.role_execution_delay_ms < 0.0:
        raise SystemExit("--role-execution-delay-ms must be non-negative")
    summary = generate_hybrid_native_bundle(
        args.out, args.profile, args.role_execution_delay_ms)
    if args.summary_json and args.summary_json.resolve() != (
            args.out / "hybrid-bundle-summary.json").resolve():
        _write_json(args.summary_json, summary)
    print("NDNSF_DI_SPEC170_HYBRID_BUNDLE_OK " + json.dumps(
        {"profile": args.profile, "out": str(args.out)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
