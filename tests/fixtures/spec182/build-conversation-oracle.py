#!/usr/bin/env python3
"""Freeze legacy checkpoint wire evidence for native C++ tests (offline only)."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
from unittest.mock import patch
import base64
from dataclasses import replace

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py"


def journal_vector(cases, key, reference):
    # Load the actual reference modules without executing application bootstrap.
    package = SOURCE.parent
    for name, directory in (("spec182_reference", package),
                            ("spec182_reference.core", package / "core"),
                            ("spec182_reference.app_sdk", package / "app_sdk")):
        module = types.ModuleType(name)
        module.__path__ = [str(directory)]
        sys.modules[name] = module
    name = "spec182_reference.app_sdk.runtime_journal"
    path = package / "app_sdk/runtime_journal.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    provider = module.StaticRequestEnvelopeKeyProvider(module.RequestEnvelopeKey("fixture-key", key))
    with tempfile.TemporaryDirectory(prefix="spec182-journal-oracle-") as root:
        journal = module.RuntimeJournal(root, "fixture-owner", envelope_key_provider=provider,
                                        test_only_allow_ephemeral_state_root=True)
        payloads = []
        for index, case in enumerate(cases):
            checkpoint = json.loads(case["checkpointWire"])
            epoch = checkpoint["contextEpoch"]
            envelope_id = "conversation-" + hashlib.sha256(checkpoint["conversationId"].encode()).hexdigest() + "-" + str(epoch)
            payload = json.dumps({"checkpoint": base64.b64encode(case["checkpointWire"].encode()).decode(),
                                  "transcript": case["transcript"]},
                                 sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
            with patch.object(module.secrets, "token_bytes", return_value=bytes([index + 1]) * 12), \
                 patch.object(module.time, "time", return_value=2_000_000_000 + index):
                prepared = journal.prepare_envelope(envelope_id, payload, expires_at_ms=checkpoint["expiresAtMs"])
                index_record = {
                    "conversationId": checkpoint["conversationId"], "contextEpoch": epoch,
                    "checkpointDigest": checkpoint["checkpointDigest"], "envelopeId": envelope_id,
                    "wireDigest": prepared.wire_digest,
                    "payloadDigest": "sha256:" + hashlib.sha256(payload).hexdigest(),
                    "expiresAtMs": checkpoint["expiresAtMs"],
                }
                if "nativeInitialPromptTokenCount" in case:
                    index_record["nativeInitialPromptTokenCount"] = case["nativeInitialPromptTokenCount"]
                journal.commit_prepared_envelope(prepared, (("conversation-checkpoint", index_record),))
            assert journal.read_envelope(envelope_id, at_ms=2_000_000_000_003) == payload
            payloads.append({"envelopeId": envelope_id, "plaintext": payload.decode(),
                             "encoded": prepared.encoded.decode(), "wireDigest": prepared.wire_digest})
        reopened = module.RuntimeJournal(root, "fixture-owner", envelope_key_provider=provider,
                                         test_only_allow_ephemeral_state_root=True)
        for item in payloads:
            assert reopened.read_envelope(item["envelopeId"], at_ms=2_000_000_000_003).decode() == item["plaintext"]
        with patch.object(reference.time, "time", return_value=2_000_000_000.003):
            owner = reference.ConversationCoordinator(journal=reopened, signer_key=key)
            assert owner.checkpoint(json.loads(cases[-1]["checkpointWire"])["conversationId"]).decode() == cases[-1]["checkpointWire"]
        return {"sourceSha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "identity": "fixture-owner", "keyId": "fixture-key",
                "authenticationSubkeyHex": journal.authentication_key_ring("conversation-checkpoint-v1")[0].hex(),
                "journalWire": journal.path.read_text(), "envelopes": payloads}


def build():
    spec = importlib.util.spec_from_file_location("spec182_conversation_reference", SOURCE)
    reference = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = reference
    spec.loader.exec_module(reference)
    # Public fixture material only; never load production keys or journals.
    key = bytes(range(32))
    digest = lambda value: "sha256:" + hashlib.sha256(value.encode()).hexdigest()
    cases = []
    parent = None
    for epoch, tokens, with_receipts in ((1, (10, 11), False), (2, (10, 11, 12, 13), False),
                                        (1, (10, 11), True), (2, (10, 11, 12, 13), True)):
        if epoch == 1:
            parent = None
        receipts = []
        if with_receipts:
            for role in ("B", "A"):
                receipts.append(reference.ProviderConversationStateReceiptV1(
                    conversation_id="0123456789abcdef0123456789abcdef",
                    parent_context_epoch=epoch - 1, successor_context_epoch=epoch,
                    origin_request_id="/request/" + str(epoch), origin_generation_id="1" * 32,
                    service_name="/service/会话", requester_identity="/requester/A",
                    security_domain_digest=digest("security"), model_digest=digest("model"),
                    graph_semantic_digest=digest("graph"), adapter_digest=digest("adapter"),
                    role_name="/role/" + role, role_split_digest=digest("split"),
                    layout_digest=digest("layout"), plan_role_map_digest=digest("roles"),
                    provider_identity="/provider/" + role, provider_boot_id="boot-" + role,
                    cache_epoch=1, prefix_digest=reference._prefix_digest(tokens),
                    prefix_token_count=len(tokens), position_digest=digest("position"),
                    state_schema_digest=digest("state"), state_component_digests=(digest("kv"),),
                    expires_at_ms=2_000_000_100_000 + epoch,
                ).sign(key))
        checkpoint = reference.ConversationCheckpointV1(
            conversation_id="0123456789abcdef0123456789abcdef",
            parent_context_epoch=epoch - 1, context_epoch=epoch,
            service_name="/service/会话", requester_identity="/requester/A",
            security_domain_digest=digest("security"), model_contract_digest=digest("model"),
            plan_role_map_digest=digest("roles"),
            logical_prefix_digest=reference._prefix_digest(tokens),
            prefix_token_count=len(tokens),
            role_receipt_digests=({item.role_name: item.receipt_digest for item in receipts}
                if with_receipts else {"/role/B": digest("receipt-B-" + str(epoch)),
                                       "/role/A": digest("receipt-A-" + str(epoch))}),
            issued_at_ms=2_000_000_000_000 + epoch,
            expires_at_ms=2_000_000_100_000 + epoch,
        ).sign(key)
        wire = checkpoint.to_bytes()
        parsed = reference.ConversationCheckpointV1.from_bytes(wire)
        assert parsed.to_bytes() == wire and parsed.verify(key)
        assert not parsed.verify(bytes(reversed(key)))
        # Digest validation must detect authenticated field changes.
        changed = json.loads(wire)
        changed["prefixTokenCount"] += 1
        try:
            reference.ConversationCheckpointV1.from_bytes(reference._canonical_payload(changed))
        except (ValueError, reference.ConversationCheckpointInvalid):
            pass
        else:
            raise AssertionError("reference accepted altered checkpoint")
        continuation = reference.ConversationContinuation(
            conversation_id=checkpoint.conversation_id,
            mode=(reference.ConversationInputMode.APPEND_DELTA if parent
                  else reference.ConversationInputMode.FULL_CONTEXT),
            parent_checkpoint=parent,
            expected_parent_context_epoch=epoch - 1 if parent else None,
            turn_input_digest=digest("input-" + str(epoch)),
        )
        cases.append({"name": ("first" if epoch == 1 else "append") + ("-receipts" if with_receipts else ""),
                      "canonicalTokenIds": list(tokens),
                      "continuation": continuation.to_dict(),
                      "checkpointWire": wire.decode(),
                      "checkpointDigest": checkpoint.checkpoint_digest,
                      "signature": checkpoint.signature})
        if with_receipts:
            transcript = reference.ConversationTranscriptRecordV1(
                conversation_id=checkpoint.conversation_id, context_epoch=epoch,
                requester_identity=checkpoint.requester_identity, service_name=checkpoint.service_name,
                security_domain_digest=checkpoint.security_domain_digest,
                application_messages="用户消息".encode(), tokenizer_digest=digest("tokenizer"),
                chat_template_digest=digest("template"), canonical_token_ids=tokens,
                prefix_digest=checkpoint.logical_prefix_digest, prefix_token_count=len(tokens),
                provider_role_receipts=tuple(reference._canonical_payload(item.to_dict()) for item in receipts),
                checkpoint_digest=checkpoint.checkpoint_digest, plan_role_map_digest=checkpoint.plan_role_map_digest,
                created_at_ms=checkpoint.issued_at_ms, expires_at_ms=checkpoint.expires_at_ms,
            )
            cases[-1]["transcript"] = transcript.to_dict()
            assert reference.ConversationTranscriptRecordV1.from_dict(transcript.to_dict()).to_dict() == transcript.to_dict()
        parent = wire
    # Native state lineage uses an existing PROMPT/APPEND hash chain. Keep
    # checkpoint/transcript token-list digests unchanged and bind both via
    # authenticated role receipts, with the original prefill count retained.
    native_cases = []
    for case in cases[2:]:
        tokens = case["canonicalTokenIds"]
        tokenizer = case["transcript"]["tokenizerDigest"]
        runtime_prefix = digest("NDNSF-DI-PREFIX-V1/PROMPT\n" + tokenizer + "\n1\n" + str(tokens[0]))
        for count, token in enumerate(tokens[1:], 2):
            runtime_prefix = digest("NDNSF-DI-PREFIX-V1/APPEND\n" + runtime_prefix + "\n" + str(count) + "\n" + str(token))
        receipts = [replace(reference.ProviderConversationStateReceiptV1.from_dict(
            json.loads(base64.b64decode(encoded))), prefix_digest=runtime_prefix,
            receipt_digest="", signature="").sign(key) for encoded in case["transcript"]["providerRoleReceipts"]]
        cp = replace(reference.ConversationCheckpointV1.from_bytes(case["checkpointWire"].encode()),
                     role_receipt_digests={r.role_name: r.receipt_digest for r in receipts},
                     checkpoint_digest="", signature="").sign(key)
        transcript = replace(reference.ConversationTranscriptRecordV1.from_dict(case["transcript"]),
            provider_role_receipts=tuple(reference._canonical_payload(r.to_dict()) for r in receipts),
            checkpoint_digest=cp.checkpoint_digest)
        native_case = {**case, "name": case["name"] + "-native-state", "checkpointWire": cp.to_bytes().decode(),
                       "checkpointDigest": cp.checkpoint_digest, "signature": cp.signature,
                       "transcript": transcript.to_dict(), "nativeInitialPromptTokenCount": 1,
                       "runtimePrefixDigest": runtime_prefix}
        native_case["continuation"] = dict(case["continuation"])
        if native_cases:
            native_case["continuation"]["parentCheckpoint"] = base64.b64encode(native_cases[-1]["checkpointWire"].encode()).decode()
        native_cases.append(native_case)
    return {"schema": "spec182-conversation-oracle-v1",
            "source": str(SOURCE.relative_to(ROOT)),
            "sourceSha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
            "testOnlyAuthenticationKeyHex": key.hex(), "cases": cases,
            "journal": journal_vector(cases[2:], key, reference),
            "nativeStateCases": native_cases,
            "nativeStateJournal": journal_vector(native_cases, key, reference)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    encoded = json.dumps(build(), ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    if args.check:
        if args.output.read_text() != encoded:
            raise SystemExit("conversation oracle differs from reference")
    else:
        args.output.write_text(encoded)
    print("PASS: 6 checkpoints, 4 transcripts, 4 encrypted transactions; legacy owner restore and authentication checks")


if __name__ == "__main__":
    main()
