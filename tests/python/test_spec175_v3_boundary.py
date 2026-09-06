from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

import pytest
from types import SimpleNamespace


from ndnsf_distributed_inference.app_sdk.placement import (
    validate_spec175_ordinary_v3_proposal,
)
from ndnsf_distributed_inference.sdk.placement import (
    PlacementProposalV3,
    RoleAssemblySpec,
)
from ndnsf_distributed_inference.adapters.qwen.placement import (
    build_qwen_three_stage_adapter,
)


def _digest(char: str) -> str:
    return "sha256:" + char * 64


def _proposal(roles=("stage-0", "stage-1"), providers=None, *, role_kinds=None,
              ranks=None):
    providers = providers or tuple(f"/provider/{index}" for index, _ in enumerate(roles))
    role_kinds = role_kinds or {role: "PIPELINE_RANGE" for role in roles}
    ranks = ranks or {role: 0 for role in roles}
    specs = tuple(
        RoleAssemblySpec(
            role=role,
            rank=ranks[role],
            layer_begin=index,
            layer_end=index + 1,
            recipe_digest=_digest("a"),
            artifact_digest=_digest("b"),
            backend="onnxruntime-cpu",
            role_kind=role_kinds[role],
        )
        for index, role in enumerate(roles)
    )
    return PlacementProposalV3(
        request_id="spec175-boundary",
        attempt=1,
        model_digest=_digest("c"),
        graph_digest=_digest("d"),
        roles=specs,
        provider_by_role={role: provider for role, provider in zip(roles, providers)},
    )


def test_ordinary_v3_accepts_one_two_and_four_presplit_roles():
    for count in (1, 2, 4):
        roles = tuple(f"stage-{index}" for index in range(count))
        proposal = _proposal(roles)
        assert validate_spec175_ordinary_v3_proposal(
            proposal, required_roles=roles,
            tensor_degrees_by_role={role: 1 for role in roles},
            hybrid_plan=None,
        ) is None


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


@pytest.mark.parametrize(
    ("kwargs", "message"),
    (
        ({"placement_profile": "DI_PLACEMENT_V2"}, "DI_PLACEMENT_V3"),
        ({"hybrid_plan": object()}, "hybrid"),
        ({"tensor_degrees_by_role": {"stage-0": 2, "stage-1": 1}}, "tensor degree"),
        ({"required_roles": ("stage-0", "stage-1", "stage-2")}, "role"),
        ({"providers": ("/provider/a", "/provider/a")}, "one-to-one"),
        ({"role_kinds": {"stage-0": "TENSOR_RANK", "stage-1": "PIPELINE_RANGE"}}, "rank"),
        ({"ranks": {"stage-0": 1, "stage-1": 0}}, "rank"),
    ),
)
def test_ordinary_v3_rejects_non_spec175_shapes(kwargs, message):
    roles = ("stage-0", "stage-1")
    placement_profile = kwargs.pop("placement_profile", "DI_PLACEMENT_V3")
    duplicate_providers = kwargs.get("providers") == ("/provider/a", "/provider/a")
    if duplicate_providers:
        with pytest.raises(ValueError, match=message):
            _proposal(roles, providers=kwargs["providers"])
        return
    proposal = _proposal(
        roles,
        providers=kwargs.pop("providers", None),
        role_kinds=kwargs.pop("role_kinds", None),
        ranks=kwargs.pop("ranks", None),
    )
    with pytest.raises(ValueError, match=message):
        validate_spec175_ordinary_v3_proposal(
            proposal,
            required_roles=kwargs.pop("required_roles", roles),
            tensor_degrees_by_role=kwargs.pop(
                "tensor_degrees_by_role", {role: 1 for role in roles}),
            hybrid_plan=kwargs.pop("hybrid_plan", None),
            placement_profile=placement_profile,
        )


def test_public_streaming_boundary_rejects_v2_before_network_submission():
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator,
    )

    coordinator = object.__new__(AutomaticPlanningCoordinator)
    coordinator.strategy = SimpleNamespace(placement_profile="DI_PLACEMENT_V2")
    with pytest.raises(ValueError, match="DI_PLACEMENT_V3"):
        coordinator.request_streaming(
            model=object(), task=object(), input=object(), timeout_ms=1000,
            on_event=lambda _payload: None,
            on_complete=lambda _payload: None,
            on_error=lambda _error: None,
        )


def test_request_token_streaming_rejects_v2_before_request_publication():
    """The generic request entry point must share the streamed V3 gate."""
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator,
    )

    coordinator = object.__new__(AutomaticPlanningCoordinator)
    coordinator.ack_timeout_ms = 100
    coordinator.strategy = SimpleNamespace(placement_profile="DI_PLACEMENT_V2")
    coordinator._resolve_adapter = lambda *_args: object()
    coordinator._request_v3 = lambda **_kwargs: pytest.fail(
        "V2 streamed request reached the V3/network path")
    model = SimpleNamespace(intent_digest=_digest("e"))
    with pytest.raises(ValueError, match="DI_PLACEMENT_V3"):
        coordinator.request(
            model=model,
            task=object(),
            input=object(),
            timeout_ms=1000,
            generation_mode="TOKEN_STREAMING",
        )


@pytest.mark.parametrize(
    ("case", "expected"),
    (
        ("v2", "DI_PLACEMENT_V3"),
        ("hybrid", "hybrid/TensorGroup"),
        ("tensor-degree", "tensor degree"),
        ("missing-role", "role"),
        ("duplicate-provider", "one-to-one"),
        ("tensor-rank", "rank"),
        ("nonzero-rank", "rank"),
    ),
)
def test_ordinary_v3_boundary_rejects_shapes_in_fresh_process(case, expected):
    """The rejection contract must hold across a real process boundary.

    The in-process tests above protect the Python call sites.  This subprocess
    check catches an accidentally stale import path or process launcher that
    bypasses the same validator, without starting MiniNDN or spending network
    resources while the production path is being repaired.
    """
    repo_root = Path(__file__).resolve().parents[2]
    source_root = repo_root / "NDNSF-DistributedInference"
    repo_client_root = repo_root / "NDNSF-DistributedRepo/pythonWrapper"
    code = r'''
import sys
from ndnsf_distributed_inference.app_sdk.placement import (
    validate_spec175_ordinary_v3_proposal,
)
from ndnsf_distributed_inference.sdk.placement import PlacementProposalV3, RoleAssemblySpec

case = sys.argv[1]
roles = ("stage-0", "stage-1")
providers = ("/provider/0", "/provider/1")
kinds = {role: "PIPELINE_RANGE" for role in roles}
ranks = {role: 0 for role in roles}
required = roles
degrees = {role: 1 for role in roles}
profile = "DI_PLACEMENT_V3"
hybrid = None
if case == "v2":
    profile = "DI_PLACEMENT_V2"
elif case == "hybrid":
    hybrid = object()
elif case == "tensor-degree":
    degrees["stage-0"] = 2
elif case == "missing-role":
    required = ("stage-0", "stage-1", "stage-2")
elif case == "duplicate-provider":
    providers = ("/provider/0", "/provider/0")
elif case == "tensor-rank":
    kinds["stage-0"] = "TENSOR_RANK"
elif case == "nonzero-rank":
    ranks["stage-0"] = 1

def digest(char):
    return "sha256:" + char * 64

try:
    proposal = PlacementProposalV3(
        request_id="fresh-process-boundary", attempt=1,
        model_digest=digest("a"), graph_digest=digest("b"),
        roles=tuple(RoleAssemblySpec(
            role=role, rank=ranks[role], layer_begin=index,
            layer_end=index + 1, recipe_digest=digest("c"),
            artifact_digest=digest("d"), backend="onnxruntime-cpu",
            role_kind=kinds[role]) for index, role in enumerate(roles)),
        provider_by_role={role: provider for role, provider in zip(roles, providers)},
    )
    validate_spec175_ordinary_v3_proposal(
        proposal, required_roles=required, tensor_degrees_by_role=degrees,
        hybrid_plan=hybrid, placement_profile=profile)
except ValueError as exc:
    print(type(exc).__name__ + ":" + str(exc))
    raise SystemExit(0)
raise SystemExit("unexpected acceptance: " + case)
'''
    environment = os.environ.copy()
    environment["PYTHONPATH"] = os.pathsep.join(
        (str(source_root), str(repo_client_root),
         environment.get("PYTHONPATH", "")))
    result = subprocess.run(
        [sys.executable, "-c", code, case],
        cwd=source_root,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr or result.stdout
    assert "ValueError:" in result.stdout
    assert expected in result.stdout
