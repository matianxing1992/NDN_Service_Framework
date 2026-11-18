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
