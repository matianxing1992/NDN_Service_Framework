from __future__ import annotations


from ndnsf_distributed_inference.adapters.qwen.placement import (
    build_qwen_three_stage_adapter,
)


def _digest(char: str) -> str:
    return "sha256:" + char * 64


def test_rank_one_qwen_presplit_candidate_is_not_marked_hybrid():
    """The ordinary Spec175 four-role fixture must reach the V3 boundary.

    ``HybridPlan(tensor_degrees=(1, ...))`` is semantically a rank-one
    pipeline, but the ordinary V3 validator intentionally rejects the
    hybrid-plan marker.  The splitter therefore leaves that marker absent
    unless tensor parallelism or redistribution is actually requested.
    """
    roles = ("stage-0", "stage-1")
    adapter = build_qwen_three_stage_adapter(
        model_name="NDNSF/Spec175TinyCausalLM",
        revision="spec175-test",
        layer_ranges=((0, 1), (1, 2)),
        artifact_digests_by_role={role: _digest(chr(97 + index))
                                  for index, role in enumerate(roles)},
        weight_bytes_by_role={role: 1 for role in roles},
        tensor_degrees=(1, 1),
        stage_roles=roles,
    )
    descriptor = adapter.describe_model(
        "NDNSF/Spec175TinyCausalLM", _digest("c"), _digest("d"),
        source_revision="spec175-test")
    graph = adapter.graph.inspect(descriptor)
    candidate = adapter.splitter.enumerate_candidates(descriptor, graph)[0]
    assert candidate.hybrid_plan is None
    assert all(value == 1 for value in candidate.tensor_degrees_by_role.values())
