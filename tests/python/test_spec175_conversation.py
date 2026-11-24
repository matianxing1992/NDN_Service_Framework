"""Conversation wire codecs and protected journal reopen checks."""

from __future__ import annotations

import base64
import hashlib
import json
import time
from pathlib import Path

import pytest

from ndnsf.service import ServiceUser, VerifiedCollaborationData
from ndnsf_distributed_inference.conversation import (
    ConversationCheckpointInvalid, ConversationCheckpointV1,
    ConversationContinuation, ConversationInputMode, ConversationTranscriptRecordV1,
    ConversationTurnBindingV1, ProviderConversationStateReceiptV1,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import (
    RequestEnvelopeKey, RuntimeJournal, RuntimeJournalKeyError,
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


def test_journal_authentication_subkeys_are_identity_and_purpose_scoped(
        tmp_path: Path):
    alice = RuntimeJournal.for_test(tmp_path / "journal", "alice")
    bob = RuntimeJournal.for_test(tmp_path / "journal", "bob")
    assert (alice.authentication_key_ring("conversation-checkpoint-v1")[0]
            != bob.authentication_key_ring("conversation-checkpoint-v1")[0])
    assert (alice.authentication_key_ring("conversation-checkpoint-v1")[0]
            != alice.authentication_key_ring("another-purpose-v1")[0])


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


def test_compact_transport_receipt_preserves_canonical_commitment():
    item = receipt("/LLM/Pipeline/Stage/1")
    original = item.to_dict()
    raw = b"".join(bytes.fromhex(digest.split(":", 1)[1])
                   for digest in item.state_component_digests)
    transport = {**original, "stateComponentDigests": base64.b64encode(raw).decode("ascii")}
    assert ProviderConversationStateReceiptV1.from_transport_dict(transport) == item
    assert ProviderConversationStateReceiptV1.from_transport_dict(original) == item

    changed = bytes([raw[0] ^ 1]) + raw[1:]
    with pytest.raises(ConversationCheckpointInvalid, match="malformed"):
        ProviderConversationStateReceiptV1.from_transport_dict({
            **transport, "stateComponentDigests": "??==",
        })
    with pytest.raises(ConversationCheckpointInvalid, match="malformed"):
        ProviderConversationStateReceiptV1.from_transport_dict({
            **transport, "stateComponentDigests": base64.b64encode(changed).decode("ascii"),
        })


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


def test_conversation_turn_binding_wire_round_trip_preserves_contract_digest():
    continuation = ConversationContinuation(
        "wire-binding-conversation-000000", turn_input_digest=digest("application-input"))
    binding = ConversationTurnBindingV1(
        conversation_id=continuation.conversation_id,
        parent_context_epoch=0, successor_context_epoch=1, service_name="/LLM/Qwen",
        plan_role_map_digest=digest("role-map"),
        request_contract_digest=continuation.request_contract(), retention_deadline_ms=10000,
    )
    assert binding.request_contract_digest == continuation.request_contract()
    assert ConversationTurnBindingV1.from_dict(binding.to_dict()) == binding


def _checkpoint_fixture():
    path = Path(__file__).resolve().parents[1] / "fixtures/spec182/conversation-oracle.json"
    oracle = json.loads(path.read_text())
    cases = [item for item in oracle["cases"] if item["name"] == "first-receipts"]
    assert len(cases) == 1
    case = cases[0]
    wire = case["checkpointWire"].encode()
    checkpoint = ConversationCheckpointV1.from_bytes(wire)
    assert checkpoint.verify(bytes.fromhex(oracle["testOnlyAuthenticationKeyHex"]))
    transcript = ConversationTranscriptRecordV1.from_dict(case["transcript"])
    assert transcript.checkpoint_digest == checkpoint.checkpoint_digest
    payload = json.dumps({"checkpoint": base64.b64encode(wire).decode(),
                          "transcript": transcript.to_dict()}, sort_keys=True,
                         separators=(",", ":"), ensure_ascii=False).encode()
    return wire, payload


def _write_checkpoint_fixture(journal, payload):
    prepared = journal.prepare_envelope("checkpoint", payload, expires_at_ms=10000)
    journal.commit_prepared_envelope(prepared, (("conversation-checkpoint", {
        "envelopeId": "checkpoint", "wireDigest": prepared.wire_digest}),))


def _read_checkpoint_fixture(journal):
    restored = json.loads(journal.read_envelope("checkpoint", at_ms=1))
    wire = base64.b64decode(restored["checkpoint"], validate=True)
    checkpoint = ConversationCheckpointV1.from_bytes(wire)
    transcript = ConversationTranscriptRecordV1.from_dict(restored["transcript"])
    assert transcript.checkpoint_digest == checkpoint.checkpoint_digest
    assert transcript.conversation_id == checkpoint.conversation_id
    assert transcript.context_epoch == checkpoint.context_epoch
    return wire


def test_checkpoint_codec_and_transcript_reopen_from_encrypted_journal(tmp_path: Path):
    root = tmp_path / "journal"
    journal = RuntimeJournal.for_test(root, "alice")
    wire, payload = _checkpoint_fixture()
    _write_checkpoint_fixture(journal, payload)
    records = journal.records()
    assert not any(item["kind"] == "conversation-transcript" for item in records)
    assert any(item["kind"] == "conversation-checkpoint" for item in records)
    assert wire not in journal.path.read_bytes()
    assert _read_checkpoint_fixture(RuntimeJournal.for_test(root, "alice")) == wire


def test_checkpoint_journal_reopen_rejects_wrong_owner_key(tmp_path: Path):
    root = tmp_path / "journal"
    good = RuntimeJournal.for_test(root, "alice")
    _, payload = _checkpoint_fixture()
    _write_checkpoint_fixture(good, payload)
    wrong = RuntimeJournal(
        root, "alice", envelope_key_provider=StaticRequestEnvelopeKeyProvider(
            RequestEnvelopeKey("wrong", b"w" * 32)),
        test_only_allow_ephemeral_state_root=True)
    with pytest.raises(RuntimeJournalKeyError):
        wrong.read_envelope("checkpoint", at_ms=1)


def test_checkpoint_codec_survives_bounded_journal_key_rotation(tmp_path: Path):
    root = tmp_path / "journal"
    old_key = RequestEnvelopeKey("old", b"o" * 32)
    first = RuntimeJournal(root, "alice",
        envelope_key_provider=StaticRequestEnvelopeKeyProvider(old_key),
        test_only_allow_ephemeral_state_root=True)
    wire, payload = _checkpoint_fixture()
    _write_checkpoint_fixture(first, payload)
    rotated = RuntimeJournal(root, "alice",
        envelope_key_provider=StaticRequestEnvelopeKeyProvider(
            RequestEnvelopeKey("new", b"n" * 32), previous=(old_key,)),
        test_only_allow_ephemeral_state_root=True)
    assert _read_checkpoint_fixture(rotated) == wire
