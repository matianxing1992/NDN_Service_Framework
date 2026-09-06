"""Spec180 native YOLO Merge contract and Python-boundary regressions."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import time

import pytest


ROOT = Path(__file__).resolve().parents[2]
PACKAGE = (Path(os.environ["SPEC180_YOLO_CANONICAL_PACKAGE"])
           if os.environ.get("SPEC180_YOLO_CANONICAL_PACKAGE") else None)


def _adapter_and_candidate():
    if PACKAGE is None:
        pytest.skip("SPEC180_YOLO_CANONICAL_PACKAGE is required for this model check")
    assert (PACKAGE / "manifest.json").is_file(), "configured canonical package is missing"
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter

    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    registry = Path(os.environ.get("SPEC180_YOLO_CATALOGUE_REGISTRY") or (
        ROOT / "specs" / "180-ack-driven-cross-model-qualification"
        / "contracts" / "trust-root-registry-v1.json"))
    adapter = build_yolo26n_adapter(PACKAGE, registry_path=registry)
    model = adapter.describe_model(
        "YOLO26n",
        "sha256:" + manifest["source"]["checkpointSha256"],
        "sha256:" + hashlib.sha256(json.dumps(
            {"preprocessing": manifest.get("preprocessing", {}),
             "postprocessing": manifest.get("postprocessing", {})},
            sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
        source_revision=manifest["graphRevision"],
    )
    graph = adapter.graph.inspect(model)
    candidate = next(item for item in adapter.splitter.enumerate_candidates(model, graph)
                     if len(item.execution_plan.roles) == 4)
    return adapter, model, graph, candidate


def test_native_merge_metadata_reaches_the_selected_role():
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator,
    )

    _, _, graph, candidate = _adapter_and_candidate()
    assert candidate.merge_kind == "NATIVE_POSTPROCESS"
    assert candidate.postprocessing["outputName"] == "predictions"
    specs = AutomaticPlanningCoordinator._v3_role_specs(candidate, graph)
    merge = next(item for item in specs if item.role == "Merge")
    assert merge.merge_kind == "NATIVE_POSTPROCESS"
    assert merge.postprocess_identity == "YOLO26n-canonical-detection-rows"
    assert merge.postprocess_output_name == "predictions"
    assert merge.postprocess_confidence_threshold == pytest.approx(0.001)
    assert merge.postprocess_sort == "confidence-desc,class-asc,xyxy-asc"
    assert all(item.merge_kind == "" for item in specs if item.role != "Merge")


def test_native_merge_is_not_sent_through_python_onnx_assembly():
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator,
    )

    _, _, graph, candidate = _adapter_and_candidate()
    specs = AutomaticPlanningCoordinator._v3_role_specs(candidate, graph)
    # The canonical binding only supplies model-layer assembly identity. A
    # native Merge must survive certification without an assembled model path.
    from ndnsf_distributed_inference.adapters.yolo import YoloCanonicalArtifactBinding

    adapter, model, _, _ = _adapter_and_candidate()
    binding = YoloCanonicalArtifactBinding(
        package_dir=PACKAGE, adapter=adapter, model=model, graph=graph,
        artifact_root="/example/controller/NDNSF/DI/ARTIFACT",
    )
    certified = AutomaticPlanningCoordinator._certify_v3_role_specs(
        candidate, graph, specs, binding.describe(candidate))
    merge = next(item for item in certified if item.role == "Merge")
    assert merge.merge_kind == "NATIVE_POSTPROCESS"
    assert not merge.model_manifest_digest
    assert not merge.assembler_descriptor_digest


def test_yolo_canonical_publication_separates_identity_and_transport():
    from ndnsf_distributed_inference.adapters.yolo import (
        YoloCanonicalArtifactBinding,
    )
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator,
    )

    adapter, model, graph, candidate = _adapter_and_candidate()
    published_payloads = []

    def publish(payload, *, object_label, object_type):
        payload = bytes(payload)
        published_payloads.append((object_label, object_type, payload))
        return {
            "dataName": f"/example/user/NDNSF/LARGE/{len(published_payloads)}",
            "plaintextSize": len(payload),
            "ciphertextDigest": "sha256:" + hashlib.sha256(payload).hexdigest(),
            "encrypted": True,
        }

    binding = YoloCanonicalArtifactBinding(
        package_dir=PACKAGE, adapter=adapter, model=model, graph=graph,
        artifact_root="/example/controller/NDNSF/DI/ARTIFACT",
        publish_encrypted_artifact=publish,
    )
    specs = AutomaticPlanningCoordinator._certify_v3_role_specs(
        candidate, graph,
        AutomaticPlanningCoordinator._v3_role_specs(candidate, graph),
        binding.describe(candidate),
    )
    result = binding.ensure(
        candidate, specs, deadline_ms=int(time.time() * 1000) + 60_000)

    assert [item[0] for item in published_payloads] == [
        "spec180-yolo-canonical-source",
        "spec180-yolo-canonical-initializer",
        "spec180-yolo-canonical-root",
    ]
    root = json.loads(published_payloads[-1][2])
    assert root["schema"] == "ndnsf-di-canonical-model-manifest-v1"
    assert root["state"] == "ACTIVE"
    assert root["metadata"]["canonicalSourceDataName"].endswith("/1")
    assert root["metadata"]["canonicalInitializerDataName"].endswith("/2")
    root_digest = "sha256:" + hashlib.sha256(
        published_payloads[-1][2]).hexdigest()
    assert binding.describe(candidate).model_manifest_digest == root_digest
    assert set(result.artifact_data_names_by_role) == set(
        result.artifact_fetch_data_names_by_role)
    assert all(name.startswith("/example/controller/NDNSF/DI/ARTIFACT/")
               for name in result.artifact_data_names_by_role.values())
    assert set(result.artifact_fetch_data_names_by_role.values()) == {
        "/example/user/NDNSF/LARGE/3"
    }


def test_deployed_native_merge_source_has_no_python_yolo_runtime_import():
    source = (ROOT / "NDNSF-DistributedInference" / "cpp" / "adapters" / "yolo" /
              "NativeYoloMergeRunner.cpp").read_text()
    assert "onnxruntime" not in source.lower()
    assert "pyimport" not in source.lower()
    assert "torch" not in source.lower()
