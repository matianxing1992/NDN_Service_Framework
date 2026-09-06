from __future__ import annotations

import hashlib
import json
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from ndnsf.service import ServiceUser, VerifiedCollaborationData

from ndnsf_distributed_inference.conversation import (
    ConversationCheckpointInvalid,
    ConversationCheckpointV1,
    ConversationContinuation,
    ConversationCoordinator,
    ConversationInputMode,
    ConversationStateConflict,
    ConversationStateUnavailable,
    ConversationStatePromotionTransaction,
    ConversationTurnBindingV1,
    ProviderConversationStateManager,
    ProviderConversationStateReceiptV1,
    ResidencyTier,
    StateLifecycle,
)
from ndnsf_distributed_inference.adapters import ApplicationInput
from ndnsf_distributed_inference.app_sdk.client import APPClient
from ndnsf_distributed_inference.app_sdk.placement import (
    AutomaticStreamingHandle,
    conversation_state_references_for_placement,
    conversation_turn_binding_for_placement,
)
from ndnsf_distributed_inference.sdk.placement import (
    ExecutionRole,
    canonical_digest,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    RequestEnvelopeKey,
    RuntimeJournal,
    StaticRequestEnvelopeKeyProvider,
)


def test_python_user_exposes_only_verified_collaboration_records():
    class Native:
        def __init__(self):
            self.calls = []

        def wait_for_verified_collaboration_data(
                self, request_id, key_scope, topic_prefix, min_count,
                timeout_ms, consume):
            self.calls.append((request_id, key_scope, topic_prefix, min_count,
                               timeout_ms, consume))
            return [{
                "data_name": "/provider/NDNSF/COLLAB/receipt/1",
                "request_id": request_id,
                "key_scope": key_scope,
                "topic": topic_prefix + "/stage-0",
                "producer": "/provider/0",
                "producer_role": "stage-0",
                "sequence": 7,
                "payload": b"receipt",
                "signer_certificate": "/provider/0/KEY/key/cert",
                "wire_digest": "sha256:" + "1" * 64,
            }]

    native = Native()
    user = ServiceUser.__new__(ServiceUser)
    user._native = native
    values = user.wait_for_verified_collaboration_data(
        "/request/turn-1",
        key_scope="ndnsf-di-conversation-state-v1",
        topic_prefix="/ndnsf-di/conversation/receipt",
        min_count=1,
        timeout_ms=1000,
    )
    assert values == (VerifiedCollaborationData(
        data_name="/provider/NDNSF/COLLAB/receipt/1",
        request_id="/request/turn-1",
        key_scope="ndnsf-di-conversation-state-v1",
        topic="/ndnsf-di/conversation/receipt/stage-0",
        producer="/provider/0",
        producer_role="stage-0",
        sequence=7,
        payload=b"receipt",
        signer_certificate="/provider/0/KEY/key/cert",
        wire_digest="sha256:" + "1" * 64,
    ),)
    assert native.calls == [(
        "/request/turn-1", "ndnsf-di-conversation-state-v1",
        "/ndnsf-di/conversation/receipt", 1, 1000, True)]

    with pytest.raises(ValueError, match="min_count"):
        user.wait_for_verified_collaboration_data(
            "/request/turn-1", key_scope="scope", topic_prefix="/topic",
            min_count=0, timeout_ms=1)


def digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode()).hexdigest()


TEST_CHECKPOINT_KEY = hashlib.sha256(
    b"spec175-explicit-test-only-checkpoint-key").digest()


def _conversation_coordinator_for_test(**kwargs) -> ConversationCoordinator:
    """Construct an in-memory coordinator with an explicit test-only key."""
    return ConversationCoordinator(signer_key=TEST_CHECKPOINT_KEY, **kwargs)


def test_coordinator_rejects_an_implicit_shared_test_key():
    with pytest.raises(ValueError, match="owner-injected"):
        ConversationCoordinator()


def test_journal_authentication_subkeys_are_identity_and_purpose_scoped(
        tmp_path: Path):
    alice = RuntimeJournal.for_test(tmp_path / "journal", "alice")
    bob = RuntimeJournal.for_test(tmp_path / "journal", "bob")
    assert (alice.authentication_key_ring("conversation-checkpoint-v1")[0]
            != bob.authentication_key_ring("conversation-checkpoint-v1")[0])
    assert (alice.authentication_key_ring("conversation-checkpoint-v1")[0]
            != alice.authentication_key_ring("another-purpose-v1")[0])


def test_parent_checkpoint_projects_exact_role_local_state_references():
    role_map = {"/role/0": "/provider/0", "/role/1": "/provider/1"}
    role_map_digest = canonical_digest(tuple(sorted(role_map.items())))
    now_ms = int(time.time() * 1000)
    checkpoint = ConversationCheckpointV1(
        conversation_id="placement-conversation-00000000",
        parent_context_epoch=0, context_epoch=1,
        service_name="/LLM/Qwen", requester_identity="/requester",
        security_domain_digest=digest("security"),
        model_contract_digest=digest("model"),
        plan_role_map_digest=role_map_digest,
        logical_prefix_digest=digest("prefix"), prefix_token_count=2,
        role_receipt_digests={
            "/role/0": digest("receipt-0"),
            "/role/1": digest("receipt-1"),
        },
        issued_at_ms=100, expires_at_ms=10_000,
    ).sign(TEST_CHECKPOINT_KEY)
    continuation = ConversationContinuation(
        "placement-conversation-00000000",
        ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=checkpoint.to_bytes(),
        expected_parent_context_epoch=1,
    )
    roles = {
        role: ExecutionRole(role, role, 0, 0, 1, "onnxruntime-cpu")
        for role in role_map
    }
    references = conversation_state_references_for_placement(
        continuation, service_name="/LLM/Qwen",
        providers_by_role=role_map, execution_roles=roles, now_ms=500)
    assert set(references) == set(role_map)
    assert references["/role/0"].role_receipt_digest == digest("receipt-0")
    assert references["/role/1"].role_receipt_digest == digest("receipt-1")
    assert references["/role/0"].checkpoint_digest == checkpoint.checkpoint_digest

    with pytest.raises(ValueError, match="Provider-role placement"):
        conversation_state_references_for_placement(
            continuation, service_name="/LLM/Qwen",
            providers_by_role={**role_map, "/role/0": "/provider/changed"},
            execution_roles=roles, now_ms=500)
    with pytest.raises(ValueError, match="Provider-role placement"):
        conversation_state_references_for_placement(
            continuation, service_name="/LLM/Qwen",
            providers_by_role=role_map, execution_roles=roles, now_ms=10_000)

    turn = conversation_turn_binding_for_placement(
        continuation, service_name="/LLM/Qwen",
        providers_by_role=role_map, execution_roles=roles,
        request_contract_digest=digest("request-contract"), now_ms=500)
    assert isinstance(turn, ConversationTurnBindingV1)
    assert turn.parent_context_epoch == 1
    assert turn.successor_context_epoch == 2
    assert turn.parent_checkpoint_digest == checkpoint.checkpoint_digest

    initial = conversation_turn_binding_for_placement(
        ConversationContinuation("initial-conversation-000000000000"),
        service_name="/LLM/Qwen", providers_by_role=role_map,
        execution_roles=roles,
        request_contract_digest=digest("initial-request"), now_ms=500)
    assert isinstance(initial, ConversationTurnBindingV1)
    assert initial.parent_context_epoch == 0
    assert initial.successor_context_epoch == 1
    assert initial.parent_checkpoint_digest == ""


def test_conversation_turn_binding_wire_round_trip_preserves_contract_digest():
    """The binding digest is the continuation contract, not the envelope hash."""
    continuation = ConversationContinuation(
        "wire-binding-conversation-000000",
        turn_input_digest=digest("application-input"),
    )
    continuation = ConversationContinuation(
        **{
            **continuation.__dict__,
            "request_contract_digest": continuation.request_contract(),
        })
    role_map = {"/role/0": "/provider/0"}
    roles = {
        "/role/0": ExecutionRole(
            "/role/0", "/role/0", 0, 0, 1, "onnxruntime-cpu")
    }
    binding = conversation_turn_binding_for_placement(
        continuation, service_name="/LLM/Qwen", providers_by_role=role_map,
        execution_roles=roles,
        request_contract_digest=continuation.request_contract_digest,
        now_ms=int(time.time() * 1000),
    )
    assert binding is not None
    assert binding.request_contract_digest == continuation.request_contract_digest
    assert ConversationTurnBindingV1.from_dict(binding.to_dict()) == binding


def test_app_client_registers_conversation_before_planner_and_binds_handle(
        tmp_path: Path):
    journal = RuntimeJournal.for_test(tmp_path / "journal", "app")
    calls = []

    class Planner:
        def request_streaming(self, **kwargs):
            calls.append(kwargs)
            return AutomaticStreamingHandle(
                None, kwargs["stream_options"],
                logical_request_id=kwargs["request_id"],
                generation_id=kwargs["stream_options"]["generation_id"],
                on_event=kwargs["on_event"],
                on_complete=kwargs["on_complete"],
                on_error=kwargs["on_error"],
            )

    client = APPClient(journal, automatic_planner=Planner())
    app_input = ApplicationInput(
        task_name="task", input_schema_digest=digest("input"),
        options_schema_digest=digest("options"), payload=b"turn",
        options=b"{}")
    continuation = ConversationContinuation("a" * 32)
    handle = client.request_streaming(
        model=None, task=None, input=app_input, timeout_ms=1000,
        on_event=lambda _value: None,
        on_complete=lambda _value: None,
        on_error=lambda _value: None,
        conversation=continuation, canonical_token_ids=(1, 2),
        request_id="turn-fixed")
    assert len(calls) == 1
    # Request IDs are absolute Name components at the native boundary.  The
    # public convenience API accepts either spelling but the conversation
    # owner and planner must share the canonical slash-prefixed identity.
    assert calls[0]["request_id"] == "/turn-fixed"
    assert handle.conversation_turn["requestId"] == "/turn-fixed"
    assert len(handle.conversation_turn["generationId"]) == 32
    assert client.conversation_coordinator._turns["/turn-fixed"][
        "generationId"] == handle.conversation_turn["generationId"]
    # The planner must receive the coordinator-normalized continuation.  An
    # empty digest here would let the turn be registered locally but make the
    # Provider reject the signed request contract later.
    forwarded = calls[0]["conversation"]
    assert forwarded.turn_input_digest == digest("turn")
    assert forwarded.request_contract_digest.startswith("sha256:")
    assert len(forwarded.request_contract_digest) == len(digest("turn"))


def test_app_client_aborts_pending_conversation_when_planner_fails(tmp_path: Path):
    journal = RuntimeJournal.for_test(tmp_path / "journal", "app")

    class Planner:
        def request_streaming(self, **_kwargs):
            raise RuntimeError("planner failed")

    client = APPClient(journal, automatic_planner=Planner())
    app_input = ApplicationInput(
        task_name="task", input_schema_digest=digest("input"),
        options_schema_digest=digest("options"), payload=b"turn",
        options=b"{}")
    continuation = ConversationContinuation("b" * 32)
    with pytest.raises(RuntimeError, match="planner failed"):
        client.request_streaming(
            model=None, task=None, input=app_input, timeout_ms=1000,
            on_event=lambda _value: None,
            on_complete=lambda _value: None,
            on_error=lambda _value: None,
            conversation=continuation, canonical_token_ids=(1, 2),
            request_id="turn-failed")
    assert "turn-failed" not in client.conversation_coordinator._turns


def receipt(role: str, *, conversation: str = "c" * 32,
            parent: int = 0, prefix=(1, 2, 3), provider: str = "/p/0",
            generation: str = "g" * 32):
    return ProviderConversationStateReceiptV1(
        conversation_id=conversation,
        parent_context_epoch=parent,
        successor_context_epoch=parent + 1,
        origin_request_id="/request/original",
        origin_generation_id=generation,
        service_name="/LLM/Qwen",
        requester_identity="/user/alice",
        security_domain_digest=digest("security"),
        model_digest=digest("model"),
        graph_semantic_digest=digest("graph"),
        adapter_digest=digest("adapter"),
        role_name=role,
        role_split_digest=digest(role + "-split"),
        layout_digest=digest(role + "-layout"),
        plan_role_map_digest=digest("map"),
        provider_identity=provider,
        provider_boot_id="boot-1",
        cache_epoch=1,
        prefix_digest=digest("prefix-" + repr(tuple(prefix))),
        prefix_token_count=len(prefix),
        position_digest=digest(role + "-position"),
        state_schema_digest=digest("state"),
        state_component_digests=(digest(role + "-kv"), digest(role + "-recurrent")),
        expires_at_ms=int(time.time() * 1000) + 60_000,
    )


def corrected_receipt(role: str, *, conversation="c" * 32, parent=0,
                      prefix=(1, 2, 3), provider="/p/0",
                      request="/request/original", generation="g" * 32):
    item = receipt(role, conversation=conversation, parent=parent,
                   prefix=prefix, provider=provider, generation=generation)
    # The production helper uses canonical JSON, so derive the exact value via
    # the module's canonical prefix commitment rather than duplicating it here.
    from ndnsf_distributed_inference import conversation as module
    return item.__class__(**{
        **item.__dict__,
        "origin_request_id": request,
        "prefix_digest": module._prefix_digest(prefix),
        "receipt_digest": "",
        "signature": "",
    })


def test_streaming_handle_commits_provider_receipts_before_completion():
    from ndnsf_distributed_inference import conversation as module

    request_id = "/request/promotion"
    conversation_id = "promotion-conversation-0000"
    role = "/LLM/Pipeline/Stage/0"
    provider = "/provider/0"
    plan_digest = digest("promotion-map")
    item = corrected_receipt(
        role, conversation=conversation_id, provider=provider,
        request=request_id, prefix=(1, 2))
    item = item.__class__(**{
        **item.__dict__, "plan_role_map_digest": plan_digest,
        "receipt_digest": "", "signature": "",
    })
    record = VerifiedCollaborationData(
        data_name="/provider/NDNSF/COLLAB/receipt/0",
        request_id=request_id,
        key_scope="ndnsf-di-conversation-state-v1",
        topic="/ndnsf-di/conversation/receipt",
        producer=provider,
        producer_role=role,
        sequence=1,
        payload=json.dumps(item.to_dict(), sort_keys=True,
                           separators=(",", ":")).encode(),
        signer_certificate=provider + "/KEY/k/cert",
        wire_digest=digest("receipt-wire"),
    )

    class ServiceUser:
        user = "/user/alice"

        def __init__(self):
            self.controls = []

        def wait_for_verified_collaboration_data(self, *_args, **_kwargs):
            return (record,)

        def publish_collaboration_data(self, target, request, **kwargs):
            self.controls.append((target, request, kwargs))
            return True

    class Coordinator:
        requester_identity = "/user/alice"

        def __init__(self):
            self.commits = []

        def commit_turn(self, request, **kwargs):
            self.commits.append((request, kwargs))
            now = int(time.time() * 1000)
            checkpoint = ConversationCheckpointV1(
                conversation_id=conversation_id,
                parent_context_epoch=0,
                context_epoch=1,
                service_name="/LLM/Qwen",
                requester_identity="/user/alice",
                security_domain_digest=digest("security"),
                model_contract_digest=digest("model"),
                plan_role_map_digest=plan_digest,
                logical_prefix_digest=module._prefix_digest((1, 2)),
                prefix_token_count=2,
                role_receipt_digests={role: item.receipt_digest},
                issued_at_ms=now,
                expires_at_ms=now + 10_000,
            ).sign(TEST_CHECKPOINT_KEY).to_bytes()
            return kwargs["result_payload"], checkpoint

    service_user = ServiceUser()
    coordinator = Coordinator()
    base = SimpleNamespace(
        collaboration=SimpleNamespace(request_id=request_id),
        service_user=service_user,
        conversation_metadata={
            "model_contract_digest": digest("model"),
            "tokenizer_digest": digest("tokenizer"),
            "chat_template_digest": digest("template"),
                "application_messages": b"prompt",
                "service_name": "/LLM/Qwen",
                "plan_digest": digest("sealed-plan"),
                "plan_role_map_digest": plan_digest,
        },
        sealed_plan=SimpleNamespace(
            roles=(SimpleNamespace(role=role),),
            providers_by_role={role: provider},
        ),
    )
    completed = threading.Event()
    handle = AutomaticStreamingHandle(
        None, {}, logical_request_id=request_id,
        on_event=lambda _value: None,
        on_complete=lambda _value: completed.set(),
        on_error=lambda error: pytest.fail(str(error)),
        conversation_expected=True,
    )
    handle._bind_attempt(1, base)
    # A very fast Provider may complete before APPClient attaches the
    # conversation owner.  Completion must remain pending in that window.
    handle._complete(1, b"answer")
    assert not completed.is_set()
    handle._attach_conversation(coordinator, {
        "requestId": request_id,
        "conversationId": conversation_id,
        "parentCheckpoint": None,
        "canonicalTokenIds": (1, 2),
    })
    assert completed.wait(2)
    assert handle.stream_complete == b"answer"
    assert len(coordinator.commits) == 1
    assert len(service_user.controls) == 1


def test_continuation_requires_parent_for_append_and_round_trips_contract():
    with pytest.raises(ValueError):
        ConversationContinuation("c" * 32, ConversationInputMode.APPEND_DELTA)
    value = ConversationContinuation("c" * 32)
    assert value.mode is ConversationInputMode.FULL_CONTEXT
    payload = value.to_dict()
    assert ConversationContinuation.from_dict(payload) == value
    assert "provider" not in repr(payload).lower()
    assert "state" not in repr(payload).lower()

    malformed = dict(payload)
    malformed.pop("requestContractDigest")
    with pytest.raises(ConversationCheckpointInvalid):
        ConversationContinuation.from_dict(malformed)


def test_coordinator_uses_fresh_authority_and_exact_delta_prefix(tmp_path: Path):
    journal = RuntimeJournal.for_test(tmp_path / "journal", "alice")
    coordinator = ConversationCoordinator(
        journal=journal, requester_identity="/user/alice",
        service_name="/LLM/Qwen", security_domain_digest=digest("security"))
    first = coordinator.begin_turn(
        ConversationContinuation("c" * 32), input_payload=b"hello",
        canonical_token_ids=(1, 2, 3))
    assert first["requestId"] != first["generationId"]
    role_receipts = [corrected_receipt("/LLM/Pipeline/Stage/0",
                                      request=first["requestId"],
                                      generation=first["generationId"]),
                     corrected_receipt("/LLM/Pipeline/Stage/1",
                                      request=first["requestId"],
                                      generation=first["generationId"])]
    result, checkpoint = coordinator.commit_turn(
        first["requestId"], result_payload=b"answer", receipts=role_receipts,
        model_contract_digest=digest("model-contract"),
        plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
        chat_template_digest=digest("template"), application_messages=b"hello",
        canonical_token_ids=(1, 2, 3))
    assert result == b"answer"
    continuation = ConversationContinuation(
        "c" * 32, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=checkpoint, expected_parent_context_epoch=1)
    second = coordinator.begin_turn(
        continuation, input_payload=b"world", canonical_token_ids=(1, 2, 3, 4, 5))
    assert second["requestId"] != first["requestId"]
    assert second["generationId"] != first["generationId"]
    assert second["appendedTokenIds"] == (4, 5)


def test_coordinator_compare_and_swap_preserves_parent():
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    first = coordinator.begin_turn(
        ConversationContinuation("d" * 32), input_payload=b"x",
        canonical_token_ids=(1, 2, 3))
    rs = [corrected_receipt("/LLM/Pipeline/Stage/0", conversation="d" * 32,
                            request=first["requestId"],
                            generation=first["generationId"])]
    _, checkpoint = coordinator.commit_turn(
        first["requestId"], result_payload=b"one", receipts=rs,
        model_contract_digest=digest("model-contract"),
        plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
        chat_template_digest=digest("template"), application_messages=b"x",
        canonical_token_ids=(1, 2, 3))
    a = coordinator.begin_turn(
        ConversationContinuation("d" * 32, ConversationInputMode.APPEND_DELTA,
                                  parent_checkpoint=checkpoint,
                                  expected_parent_context_epoch=1),
        input_payload=b"a", canonical_token_ids=(1, 2, 3, 4))
    b = coordinator.begin_turn(
        ConversationContinuation("d" * 32, ConversationInputMode.APPEND_DELTA,
                                  parent_checkpoint=checkpoint,
                                  expected_parent_context_epoch=1),
        input_payload=b"b", canonical_token_ids=(1, 2, 3, 5))
    winner = [corrected_receipt("/LLM/Pipeline/Stage/0", conversation="d" * 32,
                                parent=1, prefix=(1, 2, 3, 4),
                                request=a["requestId"],
                                generation=a["generationId"])]
    coordinator.commit_turn(
        a["requestId"], result_payload=b"two", receipts=winner,
        model_contract_digest=digest("model-contract"),
        plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
        chat_template_digest=digest("template"), application_messages=b"a",
        canonical_token_ids=(1, 2, 3, 4))
    with pytest.raises(ConversationStateConflict):
        coordinator.commit_turn(
            b["requestId"], result_payload=b"loser", receipts=[
                corrected_receipt("/LLM/Pipeline/Stage/0", conversation="d" * 32,
                                  parent=1, prefix=(1, 2, 3, 5),
                                  request=b["requestId"],
                                  generation=b["generationId"])],
            model_contract_digest=digest("model-contract"),
            plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
            chat_template_digest=digest("template"), application_messages=b"b",
            canonical_token_ids=(1, 2, 3, 5))
    assert a["parentCheckpoint"].context_epoch == 1


def test_prepare_checkpoint_is_deterministic_before_provider_controls():
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    request_id = "/request/prepare-checkpoint"
    conversation_id = "prepare-checkpoint-conversation"
    turn = coordinator.begin_turn(
        ConversationContinuation(conversation_id), input_payload=b"turn",
        canonical_token_ids=(1, 2, 3), request_id=request_id,
        generation_id="c" * 32)
    receipt_value = corrected_receipt(
        "/LLM/Pipeline/Stage/0", conversation=conversation_id,
        prefix=(1, 2, 3), request=request_id)
    receipts = (receipt_value.__class__(
        **{**receipt_value.__dict__,
           "origin_generation_id": turn["generationId"],
           # The digest covers originGenerationId; let the constructor
           # recompute it for this deliberately rewritten fixture.
           "receipt_digest": ""}),)
    kwargs = {
        "result_payload": b"answer",
        "receipts": receipts,
        "model_contract_digest": digest("model-contract"),
        "plan_role_map_digest": digest("map"),
        "tokenizer_digest": digest("tok"),
        "chat_template_digest": digest("template"),
        "application_messages": b"turn",
        "canonical_token_ids": (1, 2, 3),
    }
    now_ms = int(time.time() * 1000)
    preview = coordinator.prepare_checkpoint(
        request_id, **kwargs, now_ms=now_ms)
    assert coordinator.checkpoint(conversation_id) is None
    _result, committed = coordinator.commit_turn(
        request_id, **kwargs, now_ms=now_ms)
    assert committed == preview


def test_commit_turn_rejects_receipt_from_a_different_generation():
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    request_id = "/request/commit-binding"
    conversation_id = "commit-binding-conversation"
    turn = coordinator.begin_turn(
        ConversationContinuation(conversation_id), input_payload=b"turn",
        canonical_token_ids=(1, 2), request_id=request_id,
        generation_id="a" * 32)
    receipt_value = corrected_receipt(
        "/LLM/Pipeline/Stage/0", conversation=conversation_id,
        request=request_id, generation="b" * 32, prefix=(1, 2))
    with pytest.raises(ConversationCheckpointInvalid, match="role receipt set"):
        coordinator.commit_turn(
            request_id, result_payload=b"answer", receipts=(receipt_value,),
            model_contract_digest=digest("model-contract"),
            plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
            chat_template_digest=digest("template"),
            application_messages=b"turn", canonical_token_ids=(1, 2))
    assert coordinator.checkpoint(conversation_id) is None
    assert request_id in coordinator._turns
    assert turn["generationId"] == "a" * 32


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("requester_identity", "/user/bob"),
        ("security_domain_digest", digest("other-security")),
        ("model_digest", digest("other-model")),
    ),
)
def test_checkpoint_rejects_mixed_receipt_scope_when_coordinator_scope_is_blank(
        field, value):
    """The first receipt must not silently authorize later role receipts."""
    coordinator = _conversation_coordinator_for_test()
    request_id = "/request/mixed-receipt-scope"
    conversation_id = "mixed-receipt-scope-conversation"
    turn = coordinator.begin_turn(
        ConversationContinuation(conversation_id), input_payload=b"turn",
        canonical_token_ids=(1, 2), request_id=request_id,
        generation_id="ab" * 16)
    first = corrected_receipt(
        "/LLM/Pipeline/Stage/0", conversation=conversation_id,
        request=request_id, generation=turn["generationId"], prefix=(1, 2))
    second = corrected_receipt(
        "/LLM/Pipeline/Stage/1", conversation=conversation_id,
        request=request_id, generation=turn["generationId"], prefix=(1, 2))
    second = second.__class__(**{
        **second.__dict__, field: value, "receipt_digest": "", "signature": ""
    })
    kwargs = {
        "result_payload": b"answer",
        "receipts": (first, second),
        "model_contract_digest": digest("model-contract"),
        "plan_role_map_digest": digest("map"),
        "tokenizer_digest": digest("tok"),
        "chat_template_digest": digest("template"),
        "application_messages": b"turn",
        "canonical_token_ids": (1, 2),
    }
    with pytest.raises(ConversationCheckpointInvalid, match="role receipt set"):
        coordinator.prepare_checkpoint(request_id, **kwargs)
    with pytest.raises(ConversationCheckpointInvalid, match="role receipt set"):
        coordinator.commit_turn(request_id, **kwargs)
    assert coordinator.checkpoint(conversation_id) is None
    assert request_id in coordinator._turns


def test_provider_manager_separates_request_local_and_conversation_state():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local("/request/one", item.role_name, object(), logical_bytes=128)
    assert manager.request_local_count() == 1
    entry = manager.promote_request_local(
        request_id="/request/one", role=item.role_name, receipt=item,
        logical_bytes=128)
    assert entry.residency_tier is ResidencyTier.GPU_RESIDENT
    assert entry.lifecycle is StateLifecycle.IDLE
    assert manager.request_local_count() == 0
    assert manager.lookup(item) is not None
    assert manager.metrics().promotions == 1


def test_cross_request_acquire_requires_append_parent_and_pins_exact_state():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=256, host_byte_quota=256)
    origin_request = "/request/origin"
    role = "/LLM/Pipeline/Stage/0"
    item = corrected_receipt(
        role, conversation="cross-request-conversation-0000",
        provider="/p/0", parent=0, prefix=(1, 2), request=origin_request)
    manager.put_request_local(origin_request, role, {"kv": "origin"},
                              logical_bytes=32)
    manager.promote_request_local(
        request_id=origin_request, role=role, receipt=item, logical_bytes=32)
    continuation = ConversationContinuation(
        item.conversation_id, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=b"opaque-parent", expected_parent_context_epoch=1)

    acquired = manager.acquire_for_request(
        continuation, request_id="/request/resumed", role=role, receipt=item,
        now_ms=100)
    assert acquired.opaque_state == {"kv": "origin"}
    assert acquired.lifecycle is StateLifecycle.PINNED
    assert acquired.pin_count == 1
    assert manager.request_local_count() == 0
    assert manager.metrics().conversation_hits == 1
    assert manager.release_for_request(
        continuation, request_id="/request/resumed", receipt=item)
    assert manager.lookup(item, now_ms=101) is not None

    with pytest.raises(ConversationCheckpointInvalid, match="fresh request"):
        manager.acquire_for_request(
            continuation, request_id=origin_request, role=role, receipt=item)
    with pytest.raises(ConversationCheckpointInvalid, match="parent epoch"):
        manager.acquire_for_request(
            ConversationContinuation(
                item.conversation_id, ConversationInputMode.APPEND_DELTA,
                parent_checkpoint=b"opaque-parent", expected_parent_context_epoch=2),
            request_id="/request/other", role=role, receipt=item)


def test_cross_request_acquire_rejects_a_valid_receipt_with_wrong_checkpoint():
    from ndnsf_distributed_inference import conversation as module

    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=256, host_byte_quota=256)
    request_id = "/request/checkpoint-origin"
    role = "/LLM/Pipeline/Stage/0"
    item = corrected_receipt(
        role, conversation="checkpoint-binding-conversation-0000",
        provider="/p/0", request=request_id, prefix=(1, 2))
    now_ms = int(time.time() * 1000)
    checkpoint = ConversationCheckpointV1(
        conversation_id=item.conversation_id,
        parent_context_epoch=0,
        context_epoch=1,
        service_name="/LLM/Qwen",
        requester_identity="/user/alice",
        security_domain_digest=digest("security"),
        model_contract_digest=digest("model"),
        plan_role_map_digest=item.plan_role_map_digest,
        logical_prefix_digest=module._prefix_digest((1, 2)),
        prefix_token_count=2,
        role_receipt_digests={role: item.receipt_digest},
        issued_at_ms=now_ms,
        expires_at_ms=now_ms + 10_000,
    ).sign(TEST_CHECKPOINT_KEY)
    manager.put_request_local(request_id, role, {"kv": "bound"}, logical_bytes=32)
    manager.promote_request_local(
        request_id=request_id, role=role, receipt=item, logical_bytes=32,
        checkpoint_digest=checkpoint.checkpoint_digest, now_ms=now_ms)
    continuation = ConversationContinuation(
        item.conversation_id, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=checkpoint.to_bytes(), expected_parent_context_epoch=1)
    assert manager.acquire_for_request(
        continuation, request_id="/request/checkpoint-child", role=role,
        receipt=item, now_ms=now_ms + 1).opaque_state == {"kv": "bound"}

    wrong = ConversationCheckpointV1(
        **{**checkpoint.__dict__, "logical_prefix_digest": digest("wrong"),
           "checkpoint_digest": "", "signature": ""})
    wrong_continuation = ConversationContinuation(
        item.conversation_id, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=wrong.to_bytes(), expected_parent_context_epoch=1)
    with pytest.raises(ConversationCheckpointInvalid, match="checkpoint mismatch"):
        manager.acquire_for_request(
            wrong_continuation, request_id="/request/checkpoint-other",
            role=role, receipt=item, now_ms=now_ms + 1)


def test_multi_role_promotion_is_atomic_and_keeps_request_state_until_commit():
    request_id = "/request/aggregate"
    conversation_id = "aggregate-conversation-000000000000"
    managers = []
    candidates = []
    for index in range(2):
        role = f"/LLM/Pipeline/Stage/{index}"
        manager = ProviderConversationStateManager(
            provider_identity=f"/p/{index}", provider_boot_id="boot-1",
            gpu_byte_quota=256, host_byte_quota=256)
        item = corrected_receipt(
            role, conversation=conversation_id, prefix=(1, 2),
            provider=f"/p/{index}")
        item = item.__class__(**{
            **item.__dict__, "origin_request_id": request_id,
            "origin_generation_id": "a" * 32,
            "receipt_digest": "", "signature": ""})
        manager.put_request_local(request_id, role, {"role": index},
                                  logical_bytes=64)
        managers.append(manager)
        candidates.append((manager, request_id, role, item, 64))

    transaction = ConversationStatePromotionTransaction(candidates)
    assert all(manager.request_local_count() == 1 for manager in managers)
    assert all(item[0].lookup(item[3]) is None for item in candidates)
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    turn = coordinator.begin_turn(
        ConversationContinuation(conversation_id), input_payload=b"turn",
        canonical_token_ids=(1, 2), request_id=request_id,
        generation_id="a" * 32)
    coordinator.commit_turn(
        request_id, result_payload=b"answer",
        receipts=tuple(item[3] for item in candidates),
        model_contract_digest=digest("model-contract"),
        plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
        chat_template_digest=digest("template"), application_messages=b"turn",
        canonical_token_ids=(1, 2), promotion_transaction=transaction)
    assert transaction.committed
    assert all(manager.request_local_count() == 0 for manager in managers)
    committed_checkpoint = ConversationCheckpointV1.from_bytes(
        coordinator.checkpoint(conversation_id))
    assert all(
        (entry := item[0].lookup(item[3])) is not None and
        entry.checkpoint_digest == committed_checkpoint.checkpoint_digest
        for item in candidates)
    assert turn["requestId"] == request_id


def test_multi_role_promotion_rolls_back_if_protected_journal_fails():
    class FailingJournal:
        has_envelope_key = True

        def records(self):
            return []

        def prepare_envelope(self, *_args, **_kwargs):
            raise RuntimeError("journal unavailable")

    request_id = "/request/aggregate-failure"
    conversation_id = "aggregate-failure-0000000000"
    managers = []
    candidates = []
    for index in range(2):
        role = f"/LLM/Pipeline/Stage/{index}"
        manager = ProviderConversationStateManager(
            provider_identity=f"/p/{index}", provider_boot_id="boot-1",
            gpu_byte_quota=256, host_byte_quota=256)
        item = corrected_receipt(
            role, conversation=conversation_id, prefix=(1, 2),
            provider=f"/p/{index}")
        item = item.__class__(**{
            **item.__dict__, "origin_request_id": request_id,
            "origin_generation_id": "b" * 32,
            "receipt_digest": "", "signature": ""})
        manager.put_request_local(request_id, role, {"role": index},
                                  logical_bytes=64)
        managers.append(manager)
        candidates.append((manager, request_id, role, item, 64))
    transaction = ConversationStatePromotionTransaction(candidates)
    coordinator = _conversation_coordinator_for_test(
        journal=FailingJournal(), requester_identity="/user/alice",
        service_name="/LLM/Qwen", security_domain_digest=digest("security"))
    coordinator.begin_turn(
        ConversationContinuation(conversation_id), input_payload=b"turn",
        canonical_token_ids=(1, 2), request_id=request_id,
        generation_id="b" * 32)
    with pytest.raises(RuntimeError, match="journal unavailable"):
        coordinator.commit_turn(
            request_id, result_payload=b"answer",
            receipts=tuple(item[3] for item in candidates),
            model_contract_digest=digest("model-contract"),
            plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
            chat_template_digest=digest("template"), application_messages=b"turn",
            canonical_token_ids=(1, 2), promotion_transaction=transaction)
    assert not transaction.committed
    assert all(manager.request_local_count() == 1 for manager in managers)
    assert all(manager.lookup(item[3]) is None for item in candidates)
    assert coordinator.checkpoint(conversation_id) is None


def test_provider_manager_host_prefetch_is_single_flight_and_pinned():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local("/request/one", item.role_name, {"kv": 1}, logical_bytes=128)
    manager.promote_request_local(request_id="/request/one", role=item.role_name,
                                  receipt=item, logical_bytes=128)
    manager.pause_to_host(item)
    one = manager.prefetch_to_gpu(item)
    two = manager.prefetch_to_gpu(item)
    assert one is two
    assert one.result(timeout=2).residency_tier is ResidencyTier.GPU_RESIDENT
    pinned = manager.pin(item)
    assert pinned.pin_count == 1
    assert manager.evict_inactive(now_ms=pinned.expires_at_ms + 1) == 0
    manager.unpin(item)


def test_provider_manager_pause_to_host_materializes_distinct_host_state():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    device_state = {"kv": object()}
    manager.put_request_local(
        "/request/offload", item.role_name, device_state, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/offload", role=item.role_name, receipt=item,
        logical_bytes=128)

    host_state = {"kv": bytearray(b"host-state")}
    paused = manager.pause_to_host(
        item, copy_state=lambda value: host_state if value is device_state else None)

    assert paused.residency_tier is ResidencyTier.HOST_RESIDENT
    assert paused.lifecycle is StateLifecycle.IDLE
    assert paused.opaque_state is host_state
    assert paused.opaque_state is not device_state
    assert paused.transfer_bytes == 128


def test_provider_manager_pause_copy_failure_restores_gpu_entry():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    device_state = {"kv": object()}
    manager.put_request_local(
        "/request/offload-failure", item.role_name, device_state,
        logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/offload-failure", role=item.role_name,
        receipt=item, logical_bytes=128)

    with pytest.raises(RuntimeError, match="device-to-host failed"):
        manager.pause_to_host(
            item,
            copy_state=lambda _value: (_ for _ in ()).throw(
                RuntimeError("device-to-host failed")))

    current = manager.lookup(item)
    assert current is not None
    assert current.residency_tier is ResidencyTier.GPU_RESIDENT
    assert current.lifecycle is StateLifecycle.IDLE
    assert current.opaque_state is device_state


def test_provider_manager_acquire_uses_host_to_gpu_copy_callback():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    host_state = {"kv": bytearray(b"host")}
    manager.put_request_local(
        "/request/parent", item.role_name, host_state, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/parent", role=item.role_name, receipt=item,
        logical_bytes=128)
    manager.pause_to_host(item)
    continuation = ConversationContinuation(
        item.conversation_id, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=b"opaque-parent", expected_parent_context_epoch=1)
    calls = []

    acquired = manager.acquire_for_request(
        continuation, request_id="/request/child", role=item.role_name,
        receipt=item,
        copy_state=lambda value: calls.append(value) or {"gpu": value})

    assert calls == [host_state]
    assert acquired.residency_tier is ResidencyTier.GPU_RESIDENT
    assert acquired.opaque_state == {"gpu": host_state}
    assert acquired.transfer_bytes == 128
    manager.release_for_request(
        continuation, request_id="/request/child", receipt=item)


def test_provider_manager_prefetch_failure_restores_host_and_allows_retry():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local("/request/retry", item.role_name,
                              {"kv": bytearray(b"state")}, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/retry", role=item.role_name, receipt=item,
        logical_bytes=128)
    manager.pause_to_host(item)
    calls = 0

    def fail_once(state):
        nonlocal calls
        calls += 1
        if calls == 1:
            raise RuntimeError("transient host-to-device failure")
        return {"gpu": state}

    with pytest.raises(RuntimeError, match="transient host-to-device"):
        manager.prefetch_to_gpu(item, copy_state=fail_once).result(timeout=2)
    after_failure = manager.lookup(item)
    assert after_failure is not None
    assert after_failure.residency_tier is ResidencyTier.HOST_RESIDENT
    assert after_failure.lifecycle is StateLifecycle.IDLE

    restored = manager.prefetch_to_gpu(item, copy_state=fail_once).result(timeout=2)
    assert restored.residency_tier is ResidencyTier.GPU_RESIDENT
    assert restored.lifecycle is StateLifecycle.IDLE
    assert calls == 2


def test_provider_manager_cancels_old_prefetch_generation_before_retry():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local("/request/generation", item.role_name,
                              {"kv": 1}, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/generation", role=item.role_name, receipt=item,
        logical_bytes=128)
    manager.pause_to_host(item)
    started = threading.Event()
    release = threading.Event()

    def old_copy(state):
        started.set()
        assert release.wait(timeout=2)
        return {"old": state}

    old = manager.prefetch_to_gpu(item, copy_state=old_copy)
    assert started.wait(timeout=2)
    assert manager.cancel_prefetch(item)
    new = manager.prefetch_to_gpu(item, copy_state=lambda state: {"new": state})
    release.set()
    with pytest.raises(ConversationStateUnavailable, match="cancelled"):
        old.result(timeout=2)
    current = new.result(timeout=2)
    assert current.opaque_state["new"] == {"kv": 1}
    assert current.residency_tier is ResidencyTier.GPU_RESIDENT


def test_provider_manager_acquire_revokes_prefetch_when_request_is_cancelled():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024,
        prefetch_delay_ms=200)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local("/request/cancel", item.role_name,
                              {"kv": 1}, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/cancel", role=item.role_name, receipt=item,
        logical_bytes=128)
    manager.pause_to_host(item)
    continuation = ConversationContinuation(
        item.conversation_id, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=b"opaque-parent", expected_parent_context_epoch=1)
    cancelled = threading.Event()
    outcome = []

    def acquire():
        try:
            manager.acquire_for_request(
                continuation, request_id="/request/cancelled",
                role=item.role_name, receipt=item,
                cancelled=cancelled.is_set)
        except BaseException as exc:  # noqa: BLE001 - assertion below
            outcome.append(exc)

    thread = threading.Thread(target=acquire)
    thread.start()
    time.sleep(0.03)
    cancelled.set()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert len(outcome) == 1
    assert isinstance(outcome[0], ConversationStateUnavailable)
    assert "cancelled" in str(outcome[0]).lower()
    current = manager.lookup(item)
    assert current is not None
    assert current.residency_tier is ResidencyTier.HOST_RESIDENT
    assert current.lifecycle is StateLifecycle.IDLE


def test_provider_manager_cancellation_does_not_revoke_shared_prefetch_waiter():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024,
        prefetch_delay_ms=200)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local("/request/shared", item.role_name,
                              {"kv": 1}, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/shared", role=item.role_name, receipt=item,
        logical_bytes=128)
    manager.pause_to_host(item)
    continuation = ConversationContinuation(
        item.conversation_id, ConversationInputMode.APPEND_DELTA,
        parent_checkpoint=b"opaque-parent", expected_parent_context_epoch=1)
    cancelled = threading.Event()
    outcomes = {}

    def acquire(label, request_id, is_cancelled):
        try:
            outcomes[label] = manager.acquire_for_request(
                continuation, request_id=request_id, role=item.role_name,
                receipt=item, cancelled=is_cancelled)
        except BaseException as exc:  # noqa: BLE001 - assertion below
            outcomes[label] = exc

    cancelled_thread = threading.Thread(
        target=acquire,
        args=("cancelled", "/request/shared-cancelled", cancelled.is_set))
    winner_thread = threading.Thread(
        target=acquire,
        args=("winner", "/request/shared-winner", lambda: False))
    cancelled_thread.start()
    time.sleep(0.03)
    winner_thread.start()
    time.sleep(0.04)
    cancelled.set()
    cancelled_thread.join(timeout=2)
    winner_thread.join(timeout=2)
    assert not cancelled_thread.is_alive()
    assert not winner_thread.is_alive()
    assert isinstance(outcomes["cancelled"], ConversationStateUnavailable)
    assert "cancelled" in str(outcomes["cancelled"]).lower()
    assert outcomes["winner"].residency_tier is ResidencyTier.GPU_RESIDENT
    manager.unpin(item)


def test_provider_manager_provider_boot_invalidation_clears_all_state_views():
    released = []

    def zeroize(value):
        released.append(id(value))

    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024,
        zeroize_state=zeroize)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    request_state = {"request": bytearray(b"request")}
    manager.put_request_local("/request/boot", item.role_name,
                              request_state, logical_bytes=128)
    manager.promote_request_local(
        request_id="/request/boot", role=item.role_name, receipt=item,
        logical_bytes=128)
    # Keep a second staged/request-local value so reboot cleanup covers both
    # the committed conversation store and an in-flight aggregate candidate.
    staged = corrected_receipt(
        item.role_name, conversation="boot-staged-conversation-0000",
        request="/request/staged")
    staged_state = {"staged": bytearray(b"staged")}
    manager.put_request_local("/request/staged", staged.role_name,
                              staged_state, logical_bytes=64)
    manager.stage_request_local_promotion(
        request_id="/request/staged", role=staged.role_name, receipt=staged,
        logical_bytes=64)
    assert manager.request_local_count() == 1
    assert manager.invalidate_provider_boot("boot-2") == 1
    assert manager.request_local_count() == 0
    assert manager.lookup(item) is None
    assert len(set(released)) == 2


def test_provider_manager_exact_invalidation_preserves_other_conversations():
    released = []

    def zeroize(value):
        released.append(value["conversation"])

    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024,
        zeroize_state=zeroize)
    target = corrected_receipt(
        "/LLM/Pipeline/Stage/0", conversation="target-conversation-0000000000000000",
        request="/request/target")
    survivor = corrected_receipt(
        "/LLM/Pipeline/Stage/0", conversation="survivor-conversation-00000000000000",
        request="/request/survivor")
    for receipt, label in ((target, "target"), (survivor, "survivor")):
        manager.put_request_local(
            receipt.origin_request_id, receipt.role_name,
            {"conversation": label}, logical_bytes=64)
        manager.promote_request_local(
            request_id=receipt.origin_request_id, role=receipt.role_name,
            receipt=receipt, logical_bytes=64)

    assert manager.invalidate_conversation_state(target) is True
    assert manager.lookup(target) is None
    assert manager.lookup(survivor) is not None
    assert manager.provider_boot_id == "boot-1"
    assert released == ["target"]
    assert manager.invalidate_conversation_state(target) is False


def test_provider_manager_exact_invalidation_rejects_pinned_state():
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=1024, host_byte_quota=1024)
    item = corrected_receipt("/LLM/Pipeline/Stage/0")
    manager.put_request_local(
        item.origin_request_id, item.role_name, {"kv": 1}, logical_bytes=64)
    manager.promote_request_local(
        request_id=item.origin_request_id, role=item.role_name,
        receipt=item, logical_bytes=64)
    manager.pin(item)

    with pytest.raises(ConversationStateUnavailable, match="active"):
        manager.invalidate_conversation_state(item)
    manager.unpin(item)
    assert manager.lookup(item) is not None


def test_finalize_has_no_event_cursor_and_enforces_cap():
    coordinator = _conversation_coordinator_for_test()
    turn = coordinator.begin_turn(
        ConversationContinuation("e" * 32), input_payload=b"x",
        canonical_token_ids=(1, 2))
    assert coordinator.finalize_token_suffix(turn, 2) == ("CHECKPOINT_FINALIZE", 2)
    assert coordinator.finalize_token_suffix(turn, 2) == ("CHECKPOINT_FINALIZE", 4)
    with pytest.raises(ConversationCheckpointInvalid):
        coordinator.finalize_token_suffix(turn, 29)


def _commit_one_turn(coordinator: ConversationCoordinator) -> bytes:
    turn = coordinator.begin_turn(
        ConversationContinuation("restart-conversation"), input_payload=b"x",
        canonical_token_ids=(1, 2, 3))
    _, checkpoint = coordinator.commit_turn(
        turn["requestId"], result_payload=b"answer",
        receipts=[corrected_receipt("/LLM/Pipeline/Stage/0",
                                    conversation="restart-conversation",
                                    request=turn["requestId"],
                                    generation=turn["generationId"])],
        model_contract_digest=digest("model-contract"),
        plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
        chat_template_digest=digest("template"), application_messages=b"x",
        canonical_token_ids=(1, 2, 3))
    return checkpoint


def test_conversation_checkpoint_and_transcript_recover_from_encrypted_journal(
        tmp_path: Path):
    root = tmp_path / "journal"
    journal = RuntimeJournal.for_test(root, "alice")
    checkpoint = _commit_one_turn(
        ConversationCoordinator(journal=journal, requester_identity="/user/alice",
                                 service_name="/LLM/Qwen",
                                 security_domain_digest=digest("security")))
    # The logical index is non-secret, while the checkpoint/transcript bytes
    # are only present in the protected envelope.
    records = journal.records()
    assert not any(record.get("kind") == "conversation-transcript"
                   for record in records)
    assert any(record.get("kind") == "conversation-checkpoint"
               for record in records)
    restored = ConversationCoordinator(
        journal=RuntimeJournal.for_test(root, "alice"),
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    assert restored.checkpoint("restart-conversation") == checkpoint
    assert restored.transcript("restart-conversation") is not None


def test_conversation_recovery_rejects_wrong_journal_key(tmp_path: Path):
    root = tmp_path / "journal"
    good = RuntimeJournal.for_test(root, "alice")
    _commit_one_turn(ConversationCoordinator(
        journal=good, requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security")))
    wrong = RuntimeJournal(
        root, "alice", envelope_key_provider=StaticRequestEnvelopeKeyProvider(
            RequestEnvelopeKey("wrong", b"w" * 32)),
        test_only_allow_ephemeral_state_root=True)
    with pytest.raises(ConversationCheckpointInvalid):
        ConversationCoordinator(
            journal=wrong, requester_identity="/user/alice",
            service_name="/LLM/Qwen", security_domain_digest=digest("security"))


def test_conversation_checkpoint_survives_bounded_journal_key_rotation(
        tmp_path: Path):
    root = tmp_path / "journal"
    old_key = RequestEnvelopeKey("old", b"o" * 32)
    first = RuntimeJournal(
        root, "alice",
        envelope_key_provider=StaticRequestEnvelopeKeyProvider(old_key),
        test_only_allow_ephemeral_state_root=True)
    checkpoint = _commit_one_turn(ConversationCoordinator(
        journal=first, requester_identity="/user/alice",
        service_name="/LLM/Qwen", security_domain_digest=digest("security")))

    rotated = RuntimeJournal(
        root, "alice",
        envelope_key_provider=StaticRequestEnvelopeKeyProvider(
            RequestEnvelopeKey("new", b"n" * 32), previous=(old_key,)),
        test_only_allow_ephemeral_state_root=True)
    restored = ConversationCoordinator(
        journal=rotated, requester_identity="/user/alice",
        service_name="/LLM/Qwen", security_domain_digest=digest("security"))
    assert restored.checkpoint("restart-conversation") == checkpoint


def _commit_conversation_turn(coordinator: ConversationCoordinator, *,
                              conversation: str, roles: tuple[str, ...],
                              tokens: tuple[int, ...], payload: bytes = b"answer",
                              parent_checkpoint: bytes | None = None,
                              expected_epoch: int | None = None) -> bytes:
    mode = (ConversationInputMode.FULL_CONTEXT if parent_checkpoint is None
            else ConversationInputMode.APPEND_DELTA)
    turn = coordinator.begin_turn(
        ConversationContinuation(
            conversation, mode, parent_checkpoint, expected_epoch),
        input_payload=payload, canonical_token_ids=tokens)
    receipts = tuple(
        corrected_receipt(
            role, conversation=conversation, parent=(expected_epoch or 0),
            prefix=tokens, provider=f"/p/{index}", request=turn["requestId"],
            generation=turn["generationId"])
        for index, role in enumerate(roles))
    _, checkpoint = coordinator.commit_turn(
        turn["requestId"], result_payload=payload, receipts=receipts,
        model_contract_digest=digest("model-contract"),
        plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
        chat_template_digest=digest("template"), application_messages=payload,
        canonical_token_ids=tokens)
    return checkpoint


def _print_metrics(**values: object) -> None:
    """Emit bounded, non-secret evidence consumed by the Spec175 G2 runner."""
    print("SPEC175_CONVERSATION_METRICS " + json.dumps(values, sort_keys=True))


def test_i16_one_provider_two_turns_finalizes_without_event() -> None:
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    first = coordinator.begin_turn(
        ConversationContinuation("i16-conversation"), input_payload=b"hello",
        canonical_token_ids=(1, 2, 3))
    assert coordinator.finalize_token_suffix(first, 1) == (
        "CHECKPOINT_FINALIZE", 1)
    checkpoint = _commit_conversation_turn(
        coordinator, conversation="i16-conversation", roles=("/role/0",),
        tokens=(1, 2, 3))
    second = coordinator.begin_turn(
        ConversationContinuation(
            "i16-conversation", ConversationInputMode.APPEND_DELTA,
            checkpoint, 1), input_payload=b"world",
        canonical_token_ids=(1, 2, 3, 4))
    assert second["requestId"] != first["requestId"]
    assert second["generationId"] != first["generationId"]
    assert second["appendedTokenIds"] == (4,)
    _print_metrics(
        scope="request-local+conversation-scoped", providerRoles=1,
        requestLocalPrefill=1, conversationPromotions=1,
        conversationHits=0, deltaPrefillTokens=1, eventCount=0,
        contextEpoch=1)


def test_i17_four_role_two_turns_commit_one_successor_checkpoint() -> None:
    roles = tuple(f"/LLM/Pipeline/Stage/{index}" for index in range(4))
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    checkpoint = _commit_conversation_turn(
        coordinator, conversation="i17-conversation", roles=roles,
        tokens=(1, 2, 3, 4))
    turn = coordinator.begin_turn(
        ConversationContinuation(
            "i17-conversation", ConversationInputMode.APPEND_DELTA,
            checkpoint, 1), input_payload=b"next",
        canonical_token_ids=(1, 2, 3, 4, 5, 6))
    successor = _commit_conversation_turn(
        coordinator, conversation="i17-conversation", roles=roles,
        tokens=(1, 2, 3, 4, 5, 6), payload=b"answer-2",
        parent_checkpoint=checkpoint, expected_epoch=1)
    restored = coordinator.checkpoint("i17-conversation")
    assert restored == successor
    assert ConversationCheckpointV1.from_bytes(successor).context_epoch == 2
    assert turn["appendedTokenIds"] == (5, 6)
    _print_metrics(
        scope="conversation-scoped", providerRoles=4,
        roleReceiptCount=4, conversationPromotions=4,
        successorContextEpoch=2, deltaPrefillTokens=2,
        successorCheckpoint=1)


def test_i18_three_conversations_are_isolated_across_host_prefetch() -> None:
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=256, host_byte_quota=256, gpu_entry_quota=8,
        host_entry_quota=8)
    receipts = tuple(
        corrected_receipt(
            "/LLM/Pipeline/Stage/0", conversation=f"i18-conversation-{index}",
            provider="/p/0")
        for index in range(3))
    for index, item in enumerate(receipts):
        manager.put_request_local(
            f"/request/i18/{index}", item.role_name, {"state": index},
            logical_bytes=32)
        manager.promote_request_local(
            request_id=f"/request/i18/{index}", role=item.role_name,
            receipt=item, logical_bytes=32)
    paused = manager.pause_to_host(receipts[1])
    assert paused.residency_tier is ResidencyTier.HOST_RESIDENT
    future = manager.prefetch_to_gpu(receipts[1])
    assert future.result(timeout=2).residency_tier is ResidencyTier.GPU_RESIDENT
    assert manager.lookup(receipts[0]) is not None
    assert manager.lookup(receipts[2]) is not None
    metrics = manager.metrics()
    assert metrics.conversation_entries == 3
    _print_metrics(
        scope="conversation-scoped", conversationEntries=metrics.conversation_entries,
        conversationHits=metrics.conversation_hits,
        hostRestore=1, prefetchEntries=metrics.prefetched_entries,
        isolatedConversations=3, stateTensorBytesOnNdn=0)


def test_i19_invalid_state_mutations_fallback_without_runner_use() -> None:
    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=256, host_byte_quota=256)
    item = corrected_receipt("/LLM/Pipeline/Stage/0", provider="/p/0")
    manager.put_request_local("/request/i19", item.role_name, object(),
                              logical_bytes=32)
    manager.promote_request_local(
        request_id="/request/i19", role=item.role_name, receipt=item,
        logical_bytes=32)
    wrong_boot = corrected_receipt(
        item.role_name, provider="/p/0")
    wrong_boot = wrong_boot.__class__(**{
        **wrong_boot.__dict__, "provider_boot_id": "boot-forged",
        "receipt_digest": "", "signature": ""})
    assert manager.lookup(wrong_boot) is None
    expired = corrected_receipt(item.role_name, provider="/p/0")
    expired = expired.__class__(**{
        **expired.__dict__, "expires_at_ms": 1,
        "receipt_digest": "", "signature": ""})
    assert manager.lookup(expired, now_ms=2) is None
    assert manager.request_local_count() == 0

    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    with pytest.raises(ConversationCheckpointInvalid):
        coordinator.begin_turn(
            ConversationContinuation(
                "i19-conversation", ConversationInputMode.APPEND_DELTA,
                b"forged-checkpoint", 1), input_payload=b"x",
            canonical_token_ids=(1, 2))
    metrics = manager.metrics()
    _print_metrics(
        scope="conversation-scoped", conversationMisses=metrics.conversation_misses,
        explicitFallbackOrFailure=1, incompatibleRunnerCalls=0,
        stateTensorBytesOnNdn=0)


def test_i20_same_parent_has_one_winner_and_cancelled_prefetch_leaks_nothing() -> None:
    roles = ("/LLM/Pipeline/Stage/0",)
    coordinator = _conversation_coordinator_for_test(
        requester_identity="/user/alice", service_name="/LLM/Qwen",
        security_domain_digest=digest("security"))
    checkpoint = _commit_conversation_turn(
        coordinator, conversation="i20-conversation", roles=roles,
        tokens=(1, 2, 3))
    a = coordinator.begin_turn(
        ConversationContinuation(
            "i20-conversation", ConversationInputMode.APPEND_DELTA,
            checkpoint, 1), input_payload=b"a",
        canonical_token_ids=(1, 2, 3, 4))
    b = coordinator.begin_turn(
        ConversationContinuation(
            "i20-conversation", ConversationInputMode.APPEND_DELTA,
            checkpoint, 1), input_payload=b"b",
        canonical_token_ids=(1, 2, 3, 5))
    _commit_conversation_turn(
        coordinator, conversation="i20-conversation", roles=roles,
        tokens=(1, 2, 3, 4), parent_checkpoint=checkpoint,
        expected_epoch=1, payload=b"winner")
    with pytest.raises(ConversationStateConflict):
        coordinator.commit_turn(
            b["requestId"], result_payload=b"loser",
            receipts=(corrected_receipt(
                roles[0], conversation="i20-conversation", parent=1,
                prefix=(1, 2, 3, 5), provider="/p/0",
                request=b["requestId"],
                generation=b["generationId"]),),
            model_contract_digest=digest("model-contract"),
            plan_role_map_digest=digest("map"), tokenizer_digest=digest("tok"),
            chat_template_digest=digest("template"), application_messages=b"b",
            canonical_token_ids=(1, 2, 3, 5))
    assert a["parentCheckpoint"].context_epoch == 1

    manager = ProviderConversationStateManager(
        provider_identity="/p/0", provider_boot_id="boot-1",
        gpu_byte_quota=256, host_byte_quota=256)
    item = corrected_receipt("/LLM/Pipeline/Stage/0", conversation="i20-state-000000000000",
                             provider="/p/0")
    manager.put_request_local("/request/i20", item.role_name, {"kv": 1},
                              logical_bytes=32)
    manager.promote_request_local(request_id="/request/i20", role=item.role_name,
                                  receipt=item, logical_bytes=32)
    manager.pause_to_host(item)
    started = threading.Event()
    release = threading.Event()

    def blocked_copy(state):
        started.set()
        assert release.wait(timeout=2)
        return state

    future = manager.prefetch_to_gpu(item, copy_state=blocked_copy)
    assert started.wait(timeout=2)
    manager.cancel_prefetch(item)
    release.set()
    with pytest.raises(ConversationStateUnavailable):
        future.result(timeout=2)
    entry = manager.lookup(item)
    assert entry is not None
    assert entry.residency_tier is ResidencyTier.HOST_RESIDENT
    assert entry.lifecycle is StateLifecycle.IDLE
    assert manager.metrics().prefetched_entries == 0
    _print_metrics(
        scope="conversation-scoped", conflictCount=1,
        prefetchCancelled=1, prefetchedEntries=manager.metrics().prefetched_entries,
        stateLeak=0, applicationCallbacksAfterCancel=0)
