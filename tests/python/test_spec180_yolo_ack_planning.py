from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.adapters.yolo.candidates import (  # noqa: E402
    EXPECTED_IDS,
    verify_catalogue,
)
from ndnsf_distributed_inference.app_sdk.placement import (  # noqa: E402
    _v3_candidate_priority_key, _v3_role_kind,
)
from ndnsf_distributed_inference.planner.presplit_first import (  # noqa: E402
    PreSplitFirstStrategy,
)
from ndnsf_distributed_inference.core.contracts import (  # noqa: E402
    DIRequestEnvelopeV2,
)
from ndnsf_distributed_inference.provider import (  # noqa: E402
    DIProviderOfferIssuerV3,
    DistributedInferenceProvider,
)
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceTopologyProfile,
    ExecutionDisposition,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    RoleAssemblySpec,
)


MODEL = "sha256:" + "1" * 64
GRAPH = "sha256:" + "2" * 64
RECIPE = "sha256:" + "3" * 64
ARTIFACT = "sha256:" + "4" * 64
ACK_CLOSED = "sha256:" + "5" * 64


class _AckCaptureProvider:
    provider = "/provider/yolo"

    def __init__(self):
        self.ack = None

    def add_collaboration_handler(self, _service, _roles, _handler, ack,
                                  **_kwargs):
        self.ack = ack


def _v3_request_bytes():
    return DIRequestEnvelopeV2(
        invocation_id="invocation-1",
        request_id="request-1",
        attempt=1,
        service="/ObjectDetection/YOLO26n",
        model_name="YOLO26n",
        model_identity_hash=MODEL,
        task_kind="object-detection",
        input_manifest_digest=ACK_CLOSED,
        input_payload_b64="aW5wdXQ=",
        options_payload_b64="",
        plan_deadline_ms=10_000,
        security_domain="tenant-a",
        task={"name": "object-detection", "placement_profile": "DI_PLACEMENT_V3"},
    ).to_bytes()


def _v3_local_ack(local_artifacts):
    network = _AckCaptureProvider()
    issuer = DIProviderOfferIssuerV3(
        provider=network.provider,
        service="/ObjectDetection/YOLO26n",
        boot_epoch="boot-1",
        devices=(),
        signer_key_id="ack-key",
        sign_offer_digest=lambda digest: "signed:" + digest,
        clock_ms=lambda: 1_000,
    )
    provider = DistributedInferenceProvider(network)
    provider.add_capability_handler(
        "/ObjectDetection/YOLO26n",
        ["FullModel"],
        lambda _ctx: None,
        has_model=True,
        can_provision=False,
        local_artifacts=local_artifacts,
        selection_offer_issuer_v3=issuer,
    )
    assert network.ack is not None
    return network.ack({}, _v3_request_bytes())


def test_role_bound_local_canonical_material_is_preparation_capability():
    decision = _v3_local_ack({
        "FullModel": {
            "path": "/candidate/yolo26n/full-model.onnx",
            "artifact": "spec180-canonical/full-model.onnx",
            "backend": "onnxruntime-cpu",
        },
    })

    assert decision.status
    offer = ProviderOfferV3.from_bytes(bytes(decision.payload))
    assert offer.execution_disposition is ExecutionDisposition.ACCEPT_WITH_PREPARATION
    assert offer.preparation_accepted
    assert offer.residency == ()
    assert not offer.ack_reservation
    assert "REJECT" not in offer.execution_disposition.value


def test_local_material_for_another_role_cannot_advertise_preparation():
    decision = _v3_local_ack({
        "DetectShard0": {
            "path": "/candidate/yolo26n/detect-shard-0.onnx",
            "artifact": "spec180-canonical/detect-shard-0.onnx",
            "backend": "onnxruntime-cpu",
        },
    })

    assert not decision.status
    offer = ProviderOfferV3.from_bytes(bytes(decision.payload))
    assert offer.execution_disposition is ExecutionDisposition.REJECT
    assert not offer.preparation_accepted
    assert offer.residency == ()
    assert not offer.ack_reservation


def _semantic_partition():
    return {
        "schema": "spec180-yolo-semantic-partition-v1",
        "roleNodeSets": {
            "BackboneNeck": ["node-a"], "DetectShard0": ["node-b"],
            "DetectShard1": ["node-c"], "Merge": ["node-d"],
        },
        "branchOwnership": {"backbone-neck": "BackboneNeck",
                             "detect-scale-0": "DetectShard0",
                             "detect-scale-1": "DetectShard1",
                             "postprocess-merge": "Merge"},
        "nodeBranch": {"node-a": "backbone-neck", "node-b": "detect-scale-0",
                        "node-c": "detect-scale-1", "node-d": "postprocess-merge"},
        "roleInterfaces": {
            role: {"inputs": [{"name": f"{role}-in", "dtype": "float32", "shape": []}],
                   "outputs": [{"name": f"{role}-out", "dtype": "float32", "shape": []}]}
            for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
        },
        "tensorInterfaces": [{
            "edgeId": "cut-0", "producerNode": "node-a",
            "producerRole": "BackboneNeck", "consumerNodes": ["node-b"],
            "consumerRoles": ["DetectShard0"], "dtype": "float32", "shape": [],
        }],
        "dependencyEdges": [{"fromRole": "BackboneNeck",
                              "toRole": "DetectShard0", "tensorEdges": ["cut-0"]}],
        "safeCuts": [{"cutId": "BackboneNeck-to-DetectShard0",
                       "fromRole": "BackboneNeck", "toRole": "DetectShard0",
                       "boundaryTensors": ["cut-0"]}],
        "equivalence": {"oraclePath": "oracle/full-model-output.npy",
                         "method": "pytorch-reference-compared-with-cpu-onnxruntime",
                         "atol": 1e-3, "rtol": 1e-4},
    }


def _role(name: str, index: int) -> RoleAssemblySpec:
    return RoleAssemblySpec(
        role=name,
        rank=0,
        layer_begin=0,
        layer_end=0,
        recipe_digest=RECIPE,
        artifact_digest=ARTIFACT,
        backend="onnxruntime-cpu",
        role_kind="COMPONENT_SET",
        node_indices=(index,),
    )


def _view(provider: str, roles: tuple[str, ...]) -> ProviderPlanningViewV3:
    topology = DeviceTopologyProfile(provider, (), "cpu")
    offer = ProviderOfferV3(
        request_id="req-1",
        attempt=1,
        service="/ObjectDetection/YOLO26n",
        provider=provider,
        model_digest=MODEL,
        graph_digest=GRAPH,
        status=True,
        execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        preparation_accepted=True,
        topology=topology,
        accepted_roles=roles,
        backends=("onnxruntime-cpu",),
        boot_epoch="boot-1",
        captured_at_ms=1,
        expires_at_ms=100,
        signer_key_id="ack-key",
        signature="ack-signature",
    )
    return ProviderPlanningViewV3.from_offer(offer)


def test_catalogue_order_is_not_a_selection_authority():
    rows = [
        {
            "candidateId": "atomic-v1",
            "selectionPriority": 10,
            "roles": [{"role": "FullModel", "kind": "COMPONENT_SET"}],
            "inputIngressRole": "FullModel",
            "resultEgressRole": "FullModel",
            "mergeKind": "NATIVE_POSTPROCESS",
            "safeCuts": [],
            "semanticPartition": {},
        },
        {
            "candidateId": "shared-backbone-two-shard-v1",
            "selectionPriority": 20,
            "roles": [
                {"role": "BackboneNeck", "kind": "COMPONENT_SET"},
                {"role": "DetectShard0", "kind": "COMPONENT_SET"},
                {"role": "DetectShard1", "kind": "COMPONENT_SET"},
                {"role": "Merge", "kind": "COMPONENT_SET"},
            ],
            "inputIngressRole": "BackboneNeck",
            "resultEgressRole": "Merge",
            "mergeKind": "NATIVE_POSTPROCESS",
            "safeCuts": _semantic_partition()["safeCuts"],
            "semanticPartition": _semantic_partition(),
        },
    ]
    import hashlib
    import json

    def digest(row):
        body = json.dumps(row, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=False).encode("utf-8")
        return "sha256:" + hashlib.sha256(body).hexdigest()

    for row in rows:
        row["candidateDigest"] = digest(row)
    catalogue = {
        "schema": "spec180-yolo-catalogue-v1",
        "candidates": list(reversed(rows)),
    }
    verified = verify_catalogue(catalogue)
    assert {item.candidate_id for item in verified} == set(EXPECTED_IDS)
    keyed = sorted(
        (SimpleNamespace(candidate_digest=item.candidate_digest,
                         selection_priority=item.priority)
         for item in verified),
        key=_v3_candidate_priority_key,
    )
    assert keyed[0].selection_priority == 20


def test_atomic_only_capability_selects_full_model():
    strategy = PreSplitFirstStrategy(at_ms=1)
    decision = strategy.propose_v3(
        request_id="req-1",
        attempt=1,
        model_digest=MODEL,
        graph_digest=GRAPH,
        roles=(_role("FullModel", 0),),
        providers=(_view("/p0", ("FullModel",)),),
        ack_closed_digest=ACK_CLOSED,
    )
    assert decision.provider_by_role == {"FullModel": "/p0"}


def test_shared_candidate_requires_four_distinct_role_owners():
    strategy = PreSplitFirstStrategy(at_ms=1)
    names = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
    decision = strategy.propose_v3(
        request_id="req-1",
        attempt=1,
        model_digest=MODEL,
        graph_digest=GRAPH,
        roles=tuple(_role(name, index) for index, name in enumerate(names)),
        providers=tuple(_view(f"/p{index}", (name,))
                         for index, name in enumerate(names)),
        ack_closed_digest=ACK_CLOSED,
    )
    assert decision.provider_by_role == dict(zip(names, ("/p0", "/p1", "/p2", "/p3")))
    assert len(set(decision.provider_by_role.values())) == 4


def test_insufficient_shared_capability_fails_before_selection():
    strategy = PreSplitFirstStrategy(at_ms=1)
    names = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
    with pytest.raises(ValueError, match="no distinct feasible Provider"):
        strategy.propose_v3(
            request_id="req-1",
            attempt=1,
            model_digest=MODEL,
            graph_digest=GRAPH,
            roles=tuple(_role(name, index) for index, name in enumerate(names)),
            providers=(_view("/p0", names),),
            ack_closed_digest=ACK_CLOSED,
        )


def test_expired_ack_cannot_enter_planning_view():
    view = _view("/p0", ("FullModel",))
    with pytest.raises(ValueError, match="stale or expired"):
        # Reconstruct from the immutable view only to exercise the offer
        # boundary with the same request identity and a closed deadline.
        ProviderPlanningViewV3.from_offer(
            ProviderOfferV3(
                request_id=view.request_id,
                attempt=view.attempt,
                service="/ObjectDetection/YOLO26n",
                provider=view.provider,
                model_digest=MODEL,
                graph_digest=GRAPH,
                status=True,
                execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
                preparation_accepted=True,
                topology=view.topology,
                accepted_roles=view.accepted_roles,
                backends=view.backends,
                boot_epoch=view.boot_epoch,
                captured_at_ms=1,
                expires_at_ms=100,
                signer_key_id="ack-key",
                signature="ack-signature",
            ),
            now_ms=100,
        )


def test_yolo_detect_shards_are_component_sets_not_tensor_ranks():
    assert _v3_role_kind("DetectShard0") == "COMPONENT_SET"
    assert _v3_role_kind("DetectShard1") == "COMPONENT_SET"
    assert _v3_role_kind("/Stage/0/Shard/0") == "TENSOR_RANK"


def test_negative_catalogue_priority_fails_before_enumeration():
    rows = [{
        "candidateId": "atomic-v1",
        "selectionPriority": -1,
        "roles": [{"role": "FullModel", "kind": "COMPONENT_SET"}],
        "inputIngressRole": "FullModel",
        "resultEgressRole": "FullModel",
        "mergeKind": "NATIVE_POSTPROCESS",
        "safeCuts": [],
        "semanticPartition": {},
    }, {
        "candidateId": "shared-backbone-two-shard-v1",
        "selectionPriority": 20,
        "roles": [
            {"role": "BackboneNeck", "kind": "COMPONENT_SET"},
            {"role": "DetectShard0", "kind": "COMPONENT_SET"},
            {"role": "DetectShard1", "kind": "COMPONENT_SET"},
            {"role": "Merge", "kind": "COMPONENT_SET"},
        ],
        "inputIngressRole": "BackboneNeck",
        "resultEgressRole": "Merge",
        "mergeKind": "NATIVE_POSTPROCESS",
        "safeCuts": _semantic_partition()["safeCuts"],
        "semanticPartition": _semantic_partition(),
    }]
    import hashlib
    import json
    for row in rows:
        body = json.dumps(row, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=False).encode("utf-8")
        row["candidateDigest"] = "sha256:" + hashlib.sha256(body).hexdigest()
    with pytest.raises(ValueError, match="priority must be non-negative"):
        verify_catalogue({
            "schema": "spec180-yolo-catalogue-v1",
            "candidates": rows,
        })
