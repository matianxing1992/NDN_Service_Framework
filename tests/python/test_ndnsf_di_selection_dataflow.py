from __future__ import annotations

from dataclasses import replace
import hashlib
import json
import threading
import time
import unittest

from ndnsf_distributed_inference.core.contracts import (
    DIRequestEnvelopeV2,
    DIRoleAssignmentV2,
    DISelectionAcceptanceV2,
    DISelectionAssignmentV2,
    ExactPrefixKvKeyV1,
    ShardResidencyEvidenceV2,
    StateReuseBindingV2,
)
from ndnsf_distributed_inference.core.deployment_control import (
    DISelectionParticipant,
    DerivedStateStore,
    GpuMiBAdmissionLedger,
    ModelShardRetentionCache,
    SelectionPreparationCallbacks,
    ShardPreparationCallbacks,
    ShardPreparationPipeline,
)
from ndnsf_distributed_inference.client import (
    SelectionAcceptanceState,
    SelectionAcceptanceTracker,
)
from ndnsf_distributed_inference.sdk.placement import DIProviderOfferV2
from ndnsf_distributed_inference.provider import (
    DIProviderOfferIssuer,
    register_selection_dataflow_v2,
)
from ndnsf_distributed_inference.app_sdk.deployment import (
    require_legacy_deployment_selection,
)


def digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def exact_kv_key() -> ExactPrefixKvKeyV1:
    return ExactPrefixKvKeyV1.create(
        model_identity_hash="sha256:" + "1" * 64,
        model_semantics_digest="sha256:" + "2" * 64,
        adapter_digest="sha256:" + "3" * 64,
        runner_digest="sha256:" + "4" * 64,
        split_digest="sha256:" + "5" * 64,
        tokenizer_digest="sha256:" + "6" * 64,
        prefix_token_digest="sha256:" + "7" * 64,
        prefix_length=128,
        position_digest="sha256:" + "8" * 64,
        layer_start=0,
        layer_end=8,
        precision="bf16",
        layout_digest="sha256:" + "9" * 64,
        runtime_abi_digest="sha256:" + "a" * 64,
        security_domain="tenant-a",
    )


def offer(*, request_id="request-1", attempt=1, gpu_mib=6000,
          expires_at_ms=10_000, provider="/provider/a"):
    values = dict(
        profile="ndnsf-di-provider-offer-v2",
        profile_version=2,
        request_id=request_id,
        attempt=attempt,
        service="/Inference/Generic",
        provider=provider,
        model_intent_digest="sha256:" + "1" * 64,
        boot_epoch="boot-epoch-a",
        resource_sequence=1,
        captured_at_ms=1000,
        expires_at_ms=expires_at_ms,
        accepted_deadline_ms=9000,
        accepted_roles=("stage-0", "stage-1"),
        backends=("onnxruntime",),
        offered_gpu_memory_mb=gpu_mib,
        queue_depth=0,
        estimated_wait_ms=0.0,
        rtt_ms=1.0,
        bandwidth_mbps=1000.0,
        capability_resource_digest="sha256:" + "2" * 64,
        acceptance_predicate_digest="sha256:" + "3" * 64,
        evidence_digest="sha256:" + "4" * 64,
        signer_key_id="provider-key",
        signature="signed",
    )
    return DIProviderOfferV2(**values)


def assignment(provider_offer, *, roles=None, attempt=1,
               provider=None, deadline_ms=9000,
               state_reuse=None):
    provider = provider or provider_offer.provider
    roles = roles or (
        DIRoleAssignmentV2(
            role="stage-0", graph_node_id="node-0", layer_start=0,
            layer_end=8, artifact_digest="sha256:" + "5" * 64,
            dependency_digest="sha256:" + "6" * 64,
            adapter_id="onnx", adapter_version="1",
            required_gpu_mib=3000,
            input_grant_digests=("sha256:" + "7" * 64,),
        ),
        DIRoleAssignmentV2(
            role="stage-1", graph_node_id="node-1", layer_start=8,
            layer_end=16, artifact_digest="sha256:" + "8" * 64,
            dependency_digest="sha256:" + "9" * 64,
            adapter_id="onnx", adapter_version="1",
            required_gpu_mib=3000,
            input_grant_digests=("sha256:" + "a" * 64,),
        ),
    )
    return DISelectionAssignmentV2(
        invocation_id="invocation-1",
        request_id="request-1",
        attempt=attempt,
        plan_digest="sha256:" + "b" * 64,
        provider=provider,
        provider_boot_epoch=provider_offer.boot_epoch,
        offer_digest=provider_offer.digest(),
        resource_sequence=1,
        roles=roles,
        artifact_set_digest="sha256:" + "c" * 64,
        dependency_graph_digest="sha256:" + "d" * 64,
        deadline_ms=deadline_ms,
        generation=1,
        state_reuse_binding=state_reuse,
    )


def core_context(value):
    return {
        "transaction_id": "txn-1",
        "service_name": "/Inference/Generic",
        "request_id": value.request_id,
        "attempt": value.attempt,
        "selection_identity": digest(value.to_bytes()),
        "selection_payload_digest": digest(value.to_bytes()),
        "provider_identity": value.provider,
        "provider_boot_epoch": value.provider_boot_epoch,
        "expires_at_unix_ms": value.deadline_ms,
        "provider_token_record_ref": "token-record-1",
        "lease_record_ref": "",
    }


class SelectionDataflowV2Test(unittest.TestCase):
    def test_positive_ack_offer_is_signed_and_capacity_held(self):
        request = DIRequestEnvelopeV2(
            invocation_id="invocation-1", request_id="request-1", attempt=1,
            service="/Inference/Generic",
            model_name="Qwen/Qwen3-0.6B",
            model_identity_hash="sha256:" + "1" * 64,
            task_kind="generic", input_manifest_digest="sha256:" + "2" * 64,
            input_payload_b64="aW5wdXQ=", options_payload_b64="",
            plan_deadline_ms=9000, security_domain="tenant-a",
        )
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=6000,
        )
        issuer = DIProviderOfferIssuer(
            provider="/provider/a", service="/Inference/Generic",
            boot_epoch="boot-epoch-a", ledger=ledger,
            offered_gpu_memory_mb=6000, signer_key_id="provider-key",
            sign_offer_digest=lambda value: "sig:" + value,
            clock_ms=lambda: 1000,
        )
        decision = issuer.issue(
            request.to_bytes(), accepted_roles=("stage-0", "stage-1"),
            backends=("onnxruntime",), rtt_ms=1.0,
            bandwidth_mbps=1000.0,
        )
        self.assertTrue(decision.status)
        self.assertEqual(decision.pending_state_ttl_ms, 8000)
        issued = DIProviderOfferV2.from_bytes(decision.payload)
        self.assertEqual(ledger.held_mib(now_ms=1000), 6000)
        self.assertEqual(issuer.lookup(issued.digest()), issued)
        second = issuer.issue(
            replace(request, request_id="request-2").to_bytes(),
            accepted_roles=("stage-0",), backends=("onnxruntime",),
        )
        self.assertFalse(second.status)
        issuer.release_unused(request_id="request-1", attempt=1)
        self.assertEqual(ledger.held_mib(now_ms=1000), 0)

    def test_repeated_ack_reuses_the_existing_offer_without_double_holding(self):
        request = DIRequestEnvelopeV2(
            invocation_id="invocation-1", request_id="request-1", attempt=1,
            service="/Inference/Generic",
            model_name="Qwen/Qwen3-0.6B",
            model_identity_hash="sha256:" + "1" * 64,
            task_kind="generic", input_manifest_digest="sha256:" + "2" * 64,
            input_payload_b64="aW5wdXQ=", options_payload_b64="",
            plan_deadline_ms=9000, security_domain="tenant-a",
        )
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=6000,
        )
        issuer = DIProviderOfferIssuer(
            provider="/provider/a", service="/Inference/Generic",
            boot_epoch="boot-epoch-a", ledger=ledger,
            offered_gpu_memory_mb=6000, signer_key_id="provider-key",
            sign_offer_digest=lambda value: "sig:" + value,
            clock_ms=lambda: 1000,
        )
        first = issuer.issue(
            request.to_bytes(), accepted_roles=("stage-0",),
            backends=("transformers",),
        )
        repeated = issuer.issue(
            request.to_bytes(), accepted_roles=("stage-0",),
            backends=("transformers",),
        )
        self.assertTrue(first.status)
        self.assertTrue(repeated.status)
        self.assertEqual(first.payload, repeated.payload)
        self.assertEqual(first.pending_state_ttl_ms, 8000)
        self.assertEqual(repeated.pending_state_ttl_ms, 8000)
        self.assertEqual(ledger.held_mib(now_ms=1000), 6000)
        self.assertEqual(len(issuer._offers), 1)

    def test_offer_rejects_deadline_beyond_provider_retention_limit(self):
        request = DIRequestEnvelopeV2(
            invocation_id="invocation-1", request_id="request-1", attempt=1,
            service="/Inference/Generic",
            model_name="Qwen/Qwen3-0.6B",
            model_identity_hash="sha256:" + "1" * 64,
            task_kind="generic", input_manifest_digest="sha256:" + "2" * 64,
            input_payload_b64="aW5wdXQ=", options_payload_b64="",
            plan_deadline_ms=12_001, security_domain="tenant-a",
        )
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=6000,
        )
        issuer = DIProviderOfferIssuer(
            provider="/provider/a", service="/Inference/Generic",
            boot_epoch="boot-epoch-a", ledger=ledger,
            offered_gpu_memory_mb=6000, signer_key_id="provider-key",
            sign_offer_digest=lambda value: "sig:" + value,
            max_pending_state_ttl_ms=10_000,
            clock_ms=lambda: 2_000,
        )
        decision = issuer.issue(
            request.to_bytes(), accepted_roles=("stage-0",),
            backends=("onnxruntime",),
        )
        self.assertFalse(decision.status)
        self.assertEqual(
            decision.message,
            "DI_V2_REQUEST_DEADLINE_EXCEEDS_PROVIDER_LIMIT",
        )
        self.assertEqual(ledger.held_mib(now_ms=2_000), 0)

    def test_all_four_v2_envelopes_are_canonical_and_fail_closed(self):
        request = DIRequestEnvelopeV2(
            invocation_id="invocation-1", request_id="request-1", attempt=1,
            service="/Inference/Generic",
            model_name="Qwen/Qwen3-0.6B",
            model_identity_hash="sha256:" + "1" * 64,
            task_kind="generic", input_manifest_digest="sha256:" + "2" * 64,
            input_payload_b64="aW5wdXQ=",
            options_payload_b64="",
            plan_deadline_ms=9000, security_domain="tenant-a",
        )
        self.assertEqual(
            DIRequestEnvelopeV2.from_bytes(request.to_bytes()), request)
        provider_offer = offer()
        self.assertEqual(
            DIProviderOfferV2.from_bytes(provider_offer.to_bytes()),
            provider_offer)
        selected = assignment(provider_offer)
        self.assertEqual(
            DISelectionAssignmentV2.from_bytes(selected.to_bytes()), selected)
        accepted = DISelectionAcceptanceV2(
            invocation_id=selected.invocation_id,
            request_id=selected.request_id,
            attempt=selected.attempt,
            assignment_digest=selected.digest(),
            provider=selected.provider,
            provider_boot_epoch=selected.provider_boot_epoch,
            offer_digest=selected.offer_digest,
            role_tuple_digest=selected.role_tuple_digest(),
            accepted_gpu_mib=selected.required_gpu_mib(),
            generation=selected.generation,
            transaction_id="txn-1",
            accepted_at_ms=2000,
            expires_at_ms=selected.deadline_ms,
        )
        self.assertEqual(
            DISelectionAcceptanceV2.from_bytes(accepted.to_bytes()), accepted)
        changed = json.loads(selected.to_bytes())
        changed["schemaVersion"] = 1
        with self.assertRaises(ValueError):
            DISelectionAssignmentV2.from_bytes(
                json.dumps(changed, sort_keys=True,
                           separators=(",", ":")).encode())
        noncanonical = json.dumps(
            json.loads(request.to_bytes()), indent=2).encode()
        with self.assertRaises(ValueError):
            DIRequestEnvelopeV2.from_bytes(noncanonical)

    def test_gpu_offers_cannot_overlap_and_unused_hold_releases(self):
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=8000,
        )
        first = offer(gpu_mib=6000)
        ledger.hold_offer(first, now_ms=1500)
        with self.assertRaises(ValueError):
            ledger.hold_offer(
                offer(request_id="request-2", gpu_mib=3000), now_ms=1500)
        ledger.release_offer(first.digest(), reason="NOT_SELECTED")
        second = offer(request_id="request-2", gpu_mib=3000)
        ledger.hold_offer(second, now_ms=1500)
        self.assertEqual(ledger.held_mib(), 3000)

    def test_participant_commits_complete_tuple_before_async_preparation(self):
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=8000,
        )
        provider_offer = offer()
        ledger.hold_offer(provider_offer, now_ms=1500)
        entered = threading.Event()
        release = threading.Event()
        starts = []

        def prepare_role(context):
            self.assertFalse(hasattr(context, "provider_token"))
            entered.set()
            release.wait(1)

        participant = DISelectionParticipant(
            provider="/provider/a",
            boot_epoch="boot-epoch-a",
            ledger=ledger,
            offer_lookup=lambda value: (
                provider_offer if value == provider_offer.digest() else None),
            callbacks=SelectionPreparationCallbacks(
                prepare_role=prepare_role,
                start_role=lambda role: starts.append(role),
            ),
            clock_ms=lambda: 2000,
        )
        selected = assignment(provider_offer)
        prepared = participant.prepare(core_context(selected),
                                       selected.to_bytes())
        later_core_expiry = core_context(selected)
        later_core_expiry["expires_at_unix_ms"] = selected.deadline_ms + 1000
        participant.prepare(later_core_expiry, selected.to_bytes())
        earlier_core_expiry = core_context(selected)
        earlier_core_expiry["expires_at_unix_ms"] = selected.deadline_ms - 1
        with self.assertRaisesRegex(
                ValueError,
                r"binding mismatch: core_expiry$"):
            participant.prepare(earlier_core_expiry, selected.to_bytes())
        self.assertEqual(ledger.committed_mib(), 0)
        committed_view = {
            "transaction_id": "txn-1",
            "commit_blob": prepared["commit_blob"],
            "acceptance_payload": prepared["acceptance_payload"],
        }
        started_at = time.monotonic()
        participant.on_committed(committed_view)
        self.assertLess(time.monotonic() - started_at, 0.05)
        self.assertTrue(entered.wait(0.5))
        self.assertEqual(ledger.committed_mib(), 6000)
        self.assertEqual(participant.roles(), ("stage-0", "stage-1"))
        participant.mark_input_ready("stage-0")
        self.assertEqual(starts, [])
        release.set()
        participant.wait_for_preparation(timeout=1)
        self.assertEqual(starts, ["stage-0"])
        participant.mark_input_ready("stage-1")
        self.assertEqual(starts, ["stage-0", "stage-1"])

        recovered_ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=8000,
        )
        recovered = DISelectionParticipant(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            ledger=recovered_ledger,
            offer_lookup=lambda _value: None,
            callbacks=SelectionPreparationCallbacks(
                prepare_role=lambda _context: None,
                start_role=lambda _role: None,
            ),
            clock_ms=lambda: 2000,
        )
        recovered.on_committed(committed_view)
        recovered.wait_for_preparation(timeout=1)
        self.assertEqual(recovered_ledger.committed_mib(), 6000)

    def test_terminal_release_allows_only_the_next_sequential_request(self):
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=6000,
        )
        issuer = DIProviderOfferIssuer(
            provider="/provider/a", service="/Inference/Generic",
            boot_epoch="boot-epoch-a", ledger=ledger,
            offered_gpu_memory_mb=6000, signer_key_id="provider-key",
            sign_offer_digest=lambda value: "sig:" + value,
            clock_ms=lambda: 1000,
        )

        def request(request_id):
            return DIRequestEnvelopeV2(
                invocation_id="invocation-" + request_id,
                request_id=request_id, attempt=1,
                service="/Inference/Generic",
                model_name="Qwen/Qwen3.6-27B",
                model_identity_hash="sha256:" + "1" * 64,
                task_kind="text-generation",
                input_manifest_digest="sha256:" + "2" * 64,
                input_payload_b64="aW5wdXQ=", options_payload_b64="",
                plan_deadline_ms=9000, security_domain="tenant-a",
            )

        first_decision = issuer.issue(
            request("request-1").to_bytes(),
            accepted_roles=("stage-0",), backends=("transformers",),
        )
        self.assertTrue(first_decision.status)
        first_offer = DIProviderOfferV2.from_bytes(first_decision.payload)
        selected = assignment(first_offer, roles=(assignment(first_offer).roles[0],))
        participant = DISelectionParticipant(
            provider="/provider/a", boot_epoch="boot-epoch-a", ledger=ledger,
            offer_lookup=issuer.lookup,
            callbacks=SelectionPreparationCallbacks(
                prepare_role=lambda _context: None,
                start_role=lambda _role: None,
            ),
            clock_ms=lambda: 2000,
        )
        prepared = participant.prepare(core_context(selected), selected.to_bytes())
        participant.on_committed({
            "transaction_id": "txn-1",
            "commit_blob": prepared["commit_blob"],
            "acceptance_payload": prepared["acceptance_payload"],
        })
        participant.wait_role_prepared(
            selected.to_bytes(), "stage-0", timeout=1)
        self.assertEqual(ledger.committed_mib(), 6000)

        overlapping = issuer.issue(
            request("request-2").to_bytes(),
            accepted_roles=("stage-0",), backends=("transformers",),
        )
        self.assertFalse(overlapping.status)
        self.assertEqual(overlapping.message, "DI_GPU_CAPACITY_UNAVAILABLE")

        resident_model_cache = {"stage-0": object()}
        self.assertTrue(participant.mark_role_terminal(
            selected.to_bytes(), "stage-0", reason="RESPONSE_PUBLISHED"))
        self.assertFalse(participant.mark_role_terminal(
            selected.to_bytes(), "stage-0", reason="DUPLICATE"))
        self.assertEqual(ledger.committed_mib(), 0)
        self.assertIn("stage-0", resident_model_cache)

        next_decision = issuer.issue(
            request("request-3").to_bytes(),
            accepted_roles=("stage-0",), backends=("transformers",),
        )
        self.assertTrue(next_decision.status)

    def test_capacity_binding_tamper_and_cross_attempt_replay_fail(self):
        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=8000,
        )
        provider_offer = offer()
        ledger.hold_offer(provider_offer, now_ms=1500)
        participant = DISelectionParticipant(
            provider="/provider/a", boot_epoch="boot-epoch-a", ledger=ledger,
            offer_lookup=lambda _digest: provider_offer,
            callbacks=SelectionPreparationCallbacks(
                prepare_role=lambda _context: None,
                start_role=lambda _role: None,
            ),
            clock_ms=lambda: 2000,
        )
        selected = assignment(provider_offer)
        participant.prepare(core_context(selected), selected.to_bytes())
        with self.assertRaises(ValueError):
            participant.prepare(core_context(
                assignment(provider_offer, attempt=2)),
                assignment(provider_offer, attempt=2).to_bytes())
        too_large_role = (DIRoleAssignmentV2(
            role="stage-0", graph_node_id="node-0", layer_start=0,
            layer_end=8, artifact_digest="sha256:" + "5" * 64,
            dependency_digest="sha256:" + "6" * 64,
            adapter_id="onnx", adapter_version="1",
            required_gpu_mib=7000,
            input_grant_digests=("sha256:" + "7" * 64,),
        ),)
        with self.assertRaises(ValueError):
            participant.prepare(
                core_context(assignment(provider_offer, roles=too_large_role)),
                assignment(provider_offer, roles=too_large_role).to_bytes())
        first, second = assignment(provider_offer).roles
        with self.assertRaises(ValueError):
            assignment(
                provider_offer,
                roles=(first, replace(
                    second,
                    input_grant_digests=first.input_grant_digests,
                )),
            )
        with self.assertRaises(ValueError):
            assignment(
                provider_offer,
                roles=(first, replace(
                    second, layer_start=4, layer_end=12,
                )),
            )

    def test_partial_provider_delivery_allows_upstream_progress(self):
        base_roles = assignment(offer()).roles
        offer_a = offer(gpu_mib=3000)
        offer_b = replace(
            offer(gpu_mib=3000, provider="/provider/b"),
            boot_epoch="boot-epoch-b",
        )
        selected_a = assignment(offer_a, roles=(base_roles[0],))
        selected_b = assignment(
            offer_b,
            roles=(replace(
                base_roles[1],
                input_grant_digests=("sha256:" + "e" * 64,),
            ),),
        )
        starts = []

        def make_participant(provider_offer):
            ledger = GpuMiBAdmissionLedger(
                provider=provider_offer.provider,
                boot_epoch=provider_offer.boot_epoch,
                capacity_mib=3000,
            )
            ledger.hold_offer(provider_offer, now_ms=1500)
            return DISelectionParticipant(
                provider=provider_offer.provider,
                boot_epoch=provider_offer.boot_epoch,
                ledger=ledger,
                offer_lookup=lambda value, expected=provider_offer: (
                    expected if value == expected.digest() else None),
                callbacks=SelectionPreparationCallbacks(
                    prepare_role=lambda _context: None,
                    start_role=lambda role: starts.append(role),
                ),
                clock_ms=lambda: 2000,
            )

        participant_a = make_participant(offer_a)
        participant_b = make_participant(offer_b)
        prepared_a = participant_a.prepare(
            core_context(selected_a), selected_a.to_bytes())
        participant_a.on_committed({
            "transaction_id": "txn-1",
            "commit_blob": prepared_a["commit_blob"],
            "acceptance_payload": prepared_a["acceptance_payload"],
        })
        participant_a.mark_input_ready("stage-0")
        participant_a.wait_for_preparation(timeout=1)
        self.assertEqual(starts, ["stage-0"])
        self.assertEqual(participant_b.roles(), ())

        context_b = core_context(selected_b)
        context_b["transaction_id"] = "txn-2"
        prepared_b = participant_b.prepare(
            context_b, selected_b.to_bytes())
        participant_b.on_committed({
            "transaction_id": "txn-2",
            "commit_blob": prepared_b["commit_blob"],
            "acceptance_payload": prepared_b["acceptance_payload"],
        })
        participant_b.mark_input_ready("stage-1")
        participant_b.wait_for_preparation(timeout=1)
        self.assertEqual(starts, ["stage-0", "stage-1"])

    def test_state_reuse_is_exact_domain_bound_and_revalidated(self):
        key = exact_kv_key()
        digest_fields = (
            "model_identity_hash", "model_semantics_digest",
            "adapter_digest", "runner_digest", "split_digest",
            "tokenizer_digest", "prefix_token_digest", "position_digest",
            "layout_digest", "runtime_abi_digest",
        )
        for index, field in enumerate(digest_fields):
            changed = replace(
                key, **{field: "sha256:" + format(index + 11, "x")[-1] * 64})
            self.assertNotEqual(
                changed.digest(), key.digest(),
                msg=f"{field} must fence exact-prefix KV reuse")
        for field, value in (
                ("prefix_length", key.prefix_length + 1),
                ("layer_start", 1), ("layer_end", 9),
                ("precision", "fp16"),
                ("security_domain", "tenant-b")):
            self.assertNotEqual(
                replace(key, **{field: value}).digest(), key.digest(),
                msg=f"{field} must fence exact-prefix KV reuse")
        binding = StateReuseBindingV2(
            contract_kind="EXACT_PREFIX_KV_V1", state_key=key.digest(),
            provider="/provider/a", provider_boot_epoch="boot-epoch-a",
            cache_epoch=3, pin_id="pin-1", security_domain="tenant-a",
            layer_start=0, layer_end=8, expires_at_ms=5000,
            authorized_requester="/user/a",
        )
        binding.revalidate(
            now_ms=2000, provider="/provider/a",
            boot_epoch="boot-epoch-a", cache_epoch=3, pin_live=True,
            security_domain="tenant-a", requester="/user/a",
            layer_start=0, layer_end=8,
        )
        revalidation_cases = (
            {"now_ms": 5000},
            {"provider": "/provider/b"},
            {"boot_epoch": "boot-epoch-b"},
            {"cache_epoch": 4},
            {"pin_live": False},
            {"security_domain": "tenant-b"},
            {"requester": "/user/b"},
            {"layer_start": 1},
            {"layer_end": 9},
        )
        valid = dict(
            now_ms=2000, provider="/provider/a",
            boot_epoch="boot-epoch-a", cache_epoch=3, pin_live=True,
            security_domain="tenant-a", requester="/user/a",
            layer_start=0, layer_end=8)
        for mutation in revalidation_cases:
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                binding.revalidate(**dict(valid, **mutation))

    def test_state_reuse_migration_disabled_falls_back_cleanly(self):
        key = exact_kv_key()
        binding = StateReuseBindingV2(
            contract_kind="EXACT_PREFIX_KV_V1", state_key=key.digest(),
            provider="/provider/a", provider_boot_epoch="boot-epoch-a",
            cache_epoch=3, pin_id="pin-1", security_domain="tenant-a",
            layer_start=0, layer_end=8, expires_at_ms=5000,
            authorized_requester="/user/a",
            migration_mode="DISABLED", fallback="CLEAN_COMPUTE",
        )
        selected = assignment(offer(), state_reuse=binding)
        restored = DISelectionAssignmentV2.from_bytes(selected.to_bytes())
        self.assertEqual(restored.state_reuse_binding, binding)
        self.assertEqual(restored.state_reuse_binding.migration_mode, "DISABLED")
        try:
            binding.revalidate(
                now_ms=2000, provider="/provider/b",
                boot_epoch="boot-epoch-b", cache_epoch=4, pin_live=False,
                security_domain="tenant-a", requester="/user/a",
                layer_start=0, layer_end=8,
            )
        except ValueError:
            disposition = binding.fallback
        else:
            disposition = "LOCAL_REUSE"
        self.assertEqual(disposition, "CLEAN_COMPUTE")
        with self.assertRaisesRegex(ValueError, "invalid StateReuseBindingV2"):
            replace(binding, migration_mode="PLAINTEXT_TRANSFER")

    def test_lost_acceptance_is_unknown_and_retry_is_byte_identical(self):
        tracker = SelectionAcceptanceTracker(
            request_id="request-1", attempt=1, deadline_ms=9000,
            encryption_key=b"k" * 32,
            clock_ms=lambda: 1000,
        )
        payload = b"canonical-selection"
        tracker.record_selection("/provider/a", payload)
        tracker.mark_delivery_timeout("/provider/a")
        self.assertEqual(
            tracker.state("/provider/a"), SelectionAcceptanceState.UNKNOWN)
        self.assertEqual(tracker.retry_payload("/provider/a"), payload)
        with self.assertRaises(ValueError):
            tracker.record_selection("/provider/a", payload + b"-changed")
        expired = SelectionAcceptanceTracker(
            request_id="request-2", attempt=1, deadline_ms=9000,
            encryption_key=b"e" * 32, clock_ms=lambda: 9001,
        )
        expired.record_selection("/provider/a", payload)
        with self.assertRaises(TimeoutError):
            expired.retry_payload("/provider/a")

    def test_residency_revalidation_miss_promotion_and_retention_fences(self):
        events = []
        callbacks = ShardPreparationCallbacks(
            fetch_from_repository=lambda artifact: (
                events.append(("fetch", artifact)) or b"verified-shard"),
            verify_content=lambda artifact, payload: events.append(
                ("verify", artifact, payload)),
            promote_to_disk=lambda artifact, payload: events.append(
                ("disk", artifact, payload)),
            load_to_ram=lambda artifact: events.append(("ram", artifact)),
            load_to_gpu=lambda artifact: events.append(("gpu", artifact)),
        )
        artifact = digest(b"artifact")
        evidence = ShardResidencyEvidenceV2(
            artifact_digest=artifact, provider="/provider/a",
            provider_boot_epoch="boot-epoch-a", tier="DISK",
            cache_epoch=2, captured_at_ms=1000, expires_at_ms=5000,
            pin_until_ms=1000, reload_feasible=True,
            content_verified=True, signer_key_id="provider-key",
            signature="signed",
        )
        pipeline = ShardPreparationPipeline(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            cache_epoch=2, callbacks=callbacks,
            verify_signature=lambda item: item.signature == "signed",
        )
        self.assertEqual(pipeline.ensure_gpu(
            artifact, evidence, now_ms=2000, pin_live=True), "GPU_LOADED")
        self.assertEqual(events, [("ram", artifact), ("gpu", artifact)])
        events.clear()
        pipeline.ensure_gpu(
            artifact, evidence, now_ms=6000, pin_live=False)
        self.assertEqual([event[0] for event in events],
                         ["fetch", "verify", "disk", "ram", "gpu"])

        retention = ModelShardRetentionCache(max_entries=1)
        retention.pin_selected(artifact)
        other = digest(b"other")
        retention.retain(other)
        self.assertTrue(retention.contains(artifact))
        with self.assertRaises(RuntimeError):
            retention.pin_selected(other)
        retention.release(artifact, selected=True)
        retention.retain(digest(b"third"))
        self.assertFalse(retention.contains(artifact))

    def test_derived_state_defaults_to_terminal_destruction(self):
        key = exact_kv_key()
        binding = StateReuseBindingV2(
            contract_kind="EXACT_PREFIX_KV_V1", state_key=key.digest(),
            provider="/provider/a", provider_boot_epoch="boot-epoch-a",
            cache_epoch=3, pin_id="pin-1", security_domain="tenant-a",
            layer_start=0, layer_end=8, expires_at_ms=5000,
            authorized_requester="/user/a",
        )
        store = DerivedStateStore(max_entries=2)
        store.put(request_id="request-1", binding=binding)
        store.destroy_request_state("request-1")
        self.assertFalse(store.contains(binding.state_key))

    def test_provider_registration_uses_only_generic_core_participant_api(self):
        class NativeProvider:
            def __init__(self):
                self.calls = []

            def configure_opaque_selection_store(self, **kwargs):
                self.calls.append(("store", kwargs))

            def register_opaque_selection_participant(
                self, service, **kwargs,
            ):
                self.calls.append(("participant", service, kwargs))

        ledger = GpuMiBAdmissionLedger(
            provider="/provider/a", boot_epoch="boot-epoch-a",
            capacity_mib=8000,
        )
        participant = DISelectionParticipant(
            provider="/provider/a", boot_epoch="boot-epoch-a", ledger=ledger,
            offer_lookup=lambda _digest: None,
            callbacks=SelectionPreparationCallbacks(
                prepare_role=lambda _context: None,
                start_role=lambda _role: None,
            ),
            clock_ms=lambda: 2000,
        )
        native = NativeProvider()
        self.assertIs(register_selection_dataflow_v2(
            native, service="/Inference/Generic", participant=participant,
            wal_path="/tmp/selection.wal", storage_key=b"k" * 32,
            storage_key_epoch="epoch-1"), participant)
        self.assertEqual([item[0] for item in native.calls],
                         ["store", "participant"])
        require_legacy_deployment_selection("SELECTION_LOADS_V1")
        with self.assertRaises(ValueError):
            require_legacy_deployment_selection("SELECTION_DATAFLOW_V2")


if __name__ == "__main__":
    unittest.main()
