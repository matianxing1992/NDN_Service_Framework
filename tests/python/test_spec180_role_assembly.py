from __future__ import annotations

import pytest


def _digest(value: str) -> str:
    import hashlib
    return "sha256:" + hashlib.sha256(value.encode()).hexdigest()


def _role(**overrides):
    from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec

    values = dict(
        role="BackboneNeck", rank=0, layer_begin=0, layer_end=0,
        recipe_digest=_digest("recipe"), artifact_digest=_digest("artifact"),
        backend="onnxruntime-cpu", role_kind="COMPONENT_SET",
        node_indices=(0, 1),
    )
    values.update(overrides)
    return RoleAssemblySpec(**values)


def test_component_set_uses_node_cover_not_layer_interval():
    role = _role()
    assert role.layer_begin == role.layer_end == 0
    assert role.node_indices == (0, 1)

    with pytest.raises(ValueError, match="layer interval"):
        _role(layer_end=2)
    with pytest.raises(ValueError, match="canonical node set"):
        _role(node_indices=())


def test_range_role_still_requires_non_empty_layer_interval():
    from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec

    with pytest.raises(ValueError, match="non-empty layer interval"):
        RoleAssemblySpec(
            role="stage-0", rank=0, layer_begin=0, layer_end=0,
            recipe_digest=_digest("recipe"), artifact_digest=_digest("artifact"),
            backend="onnxruntime-cpu", role_kind="PIPELINE_RANGE",
        )


def test_execution_role_accepts_component_set_zero_interval():
    from ndnsf_distributed_inference.sdk.placement import ExecutionRole

    component = ExecutionRole(
        role_id="FullModel", stage_id="FullModel", rank=0,
        layer_begin=0, layer_end=0, backend="onnxruntime-cpu",
        adapter_id="yolo26n-onnx", adapter_version="1.0.0",
    )
    assert component.layer_begin == component.layer_end == 0
    assert component.digest().startswith("sha256:")

    # A range role still needs a strictly increasing interval.
    with pytest.raises(ValueError):
        ExecutionRole(
            role_id="stage-0", stage_id="stage-0", rank=0,
            layer_begin=4, layer_end=4, backend="onnxruntime-cpu",
        )
    with pytest.raises(ValueError):
        ExecutionRole(
            role_id="stage-0", stage_id="stage-0", rank=0,
            layer_begin=-1, layer_end=1, backend="onnxruntime-cpu",
        )


def test_certified_component_recipe_uses_same_rule():
    from ndnsf_distributed_inference.adapters.onnx.executor import (
        CertifiedOnnxAssemblyRecipe,
    )

    values = dict(
        model_manifest_digest=_digest("model"),
        artifact_profile_digest=_digest("profile"),
        graph_digest=_digest("graph"),
        canonical_initializer_digest=_digest("weights"),
        adapter_descriptor_digest=_digest("adapter"),
        assembler_descriptor_digest=_digest("assembler"),
        backend_abi="onnxruntime-cpu-v1", role_kind="COMPONENT_SET",
        layer_begin=0, layer_end=0, node_indices=(0,),
        input_names=("images",), output_names=("predictions",),
        expected_inputs=({"name": "images", "dtype": "float32", "shape": [1]},),
        expected_outputs=({"name": "predictions", "dtype": "float32", "shape": [1]},),
        precision="float32",
    )
    assert CertifiedOnnxAssemblyRecipe(**values).layer_end == 0
    with pytest.raises(ValueError, match="layer interval"):
        CertifiedOnnxAssemblyRecipe(**{**values, "layer_end": 1})


def test_provider_assembles_certified_role_from_canonical_root(tmp_path):
    """The four-role slice must assemble each certified subgraph locally.

    Regression guard for the revision-114 Y-B wiring: a component-set role
    with a certified node cover runs its extracted subgraph, not the whole
    canonical model.
    """
    from pathlib import Path
    from types import SimpleNamespace

    from ndnsf_distributed_inference.adapters.yolo import (
        YoloCanonicalArtifactBinding,
        build_yolo26n_adapter,
    )
    from ndnsf_distributed_inference.provider import DistributedInferenceProvider

    import os
    root = Path(__file__).resolve().parents[2]
    explicit_package = os.environ.get("SPEC180_YOLO_CANONICAL_PACKAGE")
    if not explicit_package:
        pytest.skip("SPEC180_YOLO_CANONICAL_PACKAGE is required for this model check")
    package = Path(explicit_package)
    registry = Path(os.environ.get("SPEC180_YOLO_CATALOGUE_REGISTRY") or (
        root / "specs" / "180-ack-driven-cross-model-qualification"
        / "contracts" / "trust-root-registry-v1.json"))
    if not (package / "manifest.json").is_file():
        pytest.fail("explicit canonical candidate package is missing")
    import json
    adapter = build_yolo26n_adapter(package, registry_path=registry)
    manifest = json.loads((package / "manifest.json").read_text())
    source = manifest["source"]
    model_digest = "sha256:" + str(source["checkpointSha256"])
    import hashlib
    semantics_digest = "sha256:" + hashlib.sha256(json.dumps(
        {"preprocessing": manifest.get("preprocessing", {}),
         "postprocessing": manifest.get("postprocessing", {})},
        sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
    model = adapter.describe_model(
        "YOLO26n", model_digest, semantics_digest,
        source_revision=str(manifest.get("graphRevision", "")))
    graph = adapter.graph.inspect(model)
    binding = YoloCanonicalArtifactBinding(
        package_dir=package, adapter=adapter, model=model, graph=graph,
        artifact_root="/example/controller/NDNSF/DI/ARTIFACT")
    shared = next(candidate for candidate
                  in adapter.splitter.enumerate_candidates(model, graph)
                  if len(candidate.execution_plan.roles) == 4)
    specs = adapter.splitter.enumerate_candidates(model, graph)
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator,
    )
    # Certify exactly the way the coordinator does after ACK_CLOSED.
    draft_specs = AutomaticPlanningCoordinator._v3_role_specs(shared, graph)
    certified = AutomaticPlanningCoordinator._certify_v3_role_specs(
        shared, graph, draft_specs, binding.describe())
    backbone = next(spec for spec in certified if spec.role == "BackboneNeck")
    assert backbone.role_kind == "COMPONENT_SET"
    assert backbone.node_indices
    assert backbone.expected_inputs and backbone.expected_outputs
    assert backbone.resource_envelope["maxAssembledBytes"] == (
        2 * backbone.resource_envelope["maxSourceBytes"])

    provider = DistributedInferenceProvider.__new__(
        DistributedInferenceProvider)
    ctx = SimpleNamespace(assignment=SimpleNamespace(role="BackboneNeck"))
    from ndnsf_distributed_inference.provider import ExecutionContext
    execution = provider._local_execution(
        "BackboneNeck", backend="onnxruntime-cpu", temp_dir=str(tmp_path),
        local_artifacts={
            "BackboneNeck": {
                "path": str(package / "canonical" / "yolo26n.onnx"),
                "canonical_initializer_path": str(
                    package / "canonical" / "yolo26n.weights"),
                "filename": "yolo26n.onnx",
                "kind": "onnx-model",
                "backend": "onnxruntime-cpu",
            },
        })
    assembled = provider._assemble_certified_role_execution(
        ctx, execution, backbone,
        {"BackboneNeck": {
            "path": str(package / "canonical" / "yolo26n.onnx"),
            "canonical_initializer_path": str(
                package / "canonical" / "yolo26n.weights"),
            "filename": "yolo26n.onnx",
            "kind": "onnx-model",
            "backend": "onnxruntime-cpu",
        }})
    model_path = assembled.artifact_paths["model"]
    assert model_path.name == "assembled-role.onnx"
    assert model_path.is_file()
    assert model_path.stat().st_size > 0
    metadata = assembled.spec.metadata
    assert metadata["assembledModelDigest"].startswith("sha256:")
    assert metadata["assembledNodeCount"] > 0
    assert metadata["assembledNodeCount"] < 500

    # The assembled subgraph must actually run and expose the role's
    # certified interface names.
    import onnxruntime as ort
    session = ort.InferenceSession(model_path.read_bytes(),
                                   providers=["CPUExecutionProvider"])
    input_names = {item.name for item in session.get_inputs()}
    output_names = {item.name for item in session.get_outputs()}
    assert input_names == {str(item["name"]) for item in backbone.expected_inputs}
    assert output_names == {str(item["name"]) for item in backbone.expected_outputs}
