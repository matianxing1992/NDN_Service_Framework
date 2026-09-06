"""Regression checks for the shared candidate consumed by the YOLO adapter."""
from dataclasses import replace

import pytest

from ndnsf_distributed_inference.adapters import build_object_detection_adapter
from ndnsf_distributed_inference.app_sdk.placement import _v3_candidate_priority_key


@pytest.fixture
def candidate():
    adapter = build_object_detection_adapter()
    model = adapter.describe_model(
        "fixture", "sha256:" + "a" * 64, "sha256:" + "b" * 64)
    graph = adapter.graph.inspect(model)
    value = adapter.splitter.enumerate_candidates(model, graph)[0]
    value.validate_against(graph)
    roles = value.execution_plan.roles
    assert len(roles) >= 2
    return replace(value, input_ingress_role=roles[0],
                   result_egress_role=roles[-1])


@pytest.mark.parametrize("field", [
    "selection_priority", "input_ingress_role", "result_egress_role",
    "merge_kind", "postprocessing",
])
def test_signed_candidate_digest_binds_application_decisions(candidate, field):
    changes = {
        "selection_priority": 10,
        "input_ingress_role": candidate.execution_plan.roles[-1],
        "result_egress_role": candidate.execution_plan.roles[0],
        "merge_kind": "NATIVE_POSTPROCESS",
        "postprocessing": {"identity": "fixture-detection-rows"},
    }
    changed = replace(candidate, **{field: changes[field]})
    assert changed.candidate_digest != candidate.candidate_digest


def test_adapter_priority_survives_shared_contract_and_planner(candidate):
    preferred = replace(candidate, selection_priority=10)
    assert sorted([candidate, preferred], key=_v3_candidate_priority_key)[0] is preferred
    assert sorted([preferred, candidate], key=_v3_candidate_priority_key)[0] is preferred


@pytest.mark.parametrize("changes", [
    {"selection_priority": -1},
    {"input_ingress_role": ""},
    {"result_egress_role": ""},
    {"input_ingress_role": "unassigned-role"},
    {"merge_kind": "CPU_FALLBACK"},
    {"postprocessing": []},
])
def test_invalid_application_decisions_fail_before_planning(candidate, changes):
    with pytest.raises(ValueError):
        replace(candidate, **changes)
