#!/usr/bin/env python3
"""Runtime v1 contract tests for NDNSF-DistributedInference."""

from __future__ import annotations

import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from ndnsf_distributed_inference.runtime_v1 import (
    BoundedDependencyTransferQueues,
    ContextObjectKind,
    DependencyTransferItem,
    ExactForwardCacheEntry,
    ExactForwardCacheManager,
    ExecutionAttemptV1,
    ExecutionEvidenceV1,
    FailureAction,
    FragmentResidency,
    GenericAdmissionLease,
    KvCacheTelemetry,
    LongContextManager,
    ModelFragmentKey,
    ModelManifestV1,
    PlanCache,
    PlanLeaseBindingsV1,
    ProviderFragmentInventoryManager,
    ProviderProfileV1,
    RuntimeTelemetryV1,
    RunnerKind,
    MeasuredTelemetrySnapshotV1,
    ProviderCapabilityV3,
    TerminalReasonV1,
    RolePipelineScheduler,
    RoleWorkItem,
    RetryPolicy,
    TransferQueueKind,
    adaptive_segment_size,
    choose_cache_placement,
    compress_payload,
    encode_ack_metadata,
    exact_forward_cache_key_for_stage,
    generate_fallback_plans,
    make_plan_lease,
    parse_ack_metadata,
    prefix_state,
    proportional_layer_allocation,
    runtime_v1_smoke,
    simulate_prefill_decode,
    validate_linear_llm_plan,
    write_runtime_report,
)
from ndnsf_distributed_inference.experimental.semantic_cache import (
    SemanticCacheAckHint,
    SemanticCacheDisposition,
    SemanticPatternMeta,
    SemanticPatternRank,
    SemanticServiceCacheEntry,
    SemanticServiceCacheKey,
    SemanticServiceCacheManager,
    choose_semantic_cache_provider,
    parse_semantic_cache_ack_hint,
    rank_semantic_patterns,
    semantic_cache_ack_fields,
    semantic_cache_token_saving_ratio,
)
from ndnsf_distributed_inference.runtime_v1_evidence import write_minindn_runtime_v1_evidence


class RuntimeV1ContractsTest(unittest.TestCase):
    def test_configured_capability_is_not_measured_telemetry(self) -> None:
        capability = ProviderCapabilityV3(
            provider_name="p0",
            supported_runner_kinds=("onnxruntime-cuda",),
            total_gpu_memory_mb=8192,
        )
        self.assertEqual(capability.source, "profile")
        measured = MeasuredTelemetrySnapshotV1(
            provider_name="p0", provider_boot_id="boot", sequence=1,
            measured_at_ms=1000, source="measured", status="measured",
            device_id="GPU-1", free_gpu_memory_mb=4096,
        )
        self.assertTrue(measured.is_fresh(at_ms=2999))
        self.assertFalse(measured.is_fresh(at_ms=3001))
        configured = MeasuredTelemetrySnapshotV1(
            provider_name="p0", provider_boot_id="boot", sequence=1,
            measured_at_ms=1000, source="profile", status="configured",
        )
        self.assertFalse(configured.is_fresh(at_ms=1001))

    def test_execution_attempt_is_bounded_and_typed(self) -> None:
        attempt = ExecutionAttemptV1("req", 1, "plan", TerminalReasonV1.PROVIDER_LOST)
        self.assertEqual(attempt.attempt_epoch, 1)
        with self.assertRaises(ValueError):
            ExecutionAttemptV1("req", 2, "plan")

    def test_execution_evidence_schema_and_redaction(self) -> None:
        payload = {
            "schema": "ndnsf-di-execution-evidence-v1",
            "providerName": "/p0", "providerBootId": "boot", "evidenceEpoch": 1,
            "runnerKind": "onnxruntime-cuda", "realCompute": True,
            "device": {"kind": "cuda", "id": "GPU-1"},
            "runtimeVersion": "ort", "modelDigest": "sha256:m",
            "planDigest": "sha256:p", "artifactDigests": {"/s0": "sha256:a"},
            "roles": ["/s0"], "createdAtMs": 1,
        }
        value = ExecutionEvidenceV1.from_dict(payload)
        self.assertEqual(value.runner_kind, RunnerKind.ONNXRUNTIME_CUDA)
        with self.assertRaises(ValueError):
            ExecutionEvidenceV1.from_dict({**payload, "token": "secret"})

    def test_ack_metadata_round_trip_keeps_legacy_and_typed_fields(self) -> None:
        profile = ProviderProfileV1(
            provider="llm-8gb",
            gpu_memory_mb=8192,
            ram_memory_mb=8192,
            flops_tflops=8,
            llm_stage_capacity_mb=8192,
            max_context_tokens=16384,
            kv_cache_budget_mb=2048,
        )
        payload = encode_ack_metadata({
            "role": "/LLM/Stage/2",
            "roles": ["/LLM/Stage/2"],
            **profile.to_ack_fields(),
        })
        fields = parse_ack_metadata(payload)
        self.assertEqual(fields["role"], "/LLM/Stage/2")
        self.assertEqual(fields["roles"], "/LLM/Stage/2")
        self.assertEqual(fields["providerProfile"]["provider"], "llm-8gb")
        self.assertEqual(fields["providerProfile"]["max_context_tokens"], 16384)

    def test_provider_fragment_inventory_reports_actual_residency(self) -> None:
        key = ModelFragmentKey(
            model_id="qwen-test",
            model_digest="sha256:model",
            runtime_backend="onnx-cpu",
            precision="fp32",
            split_strategy="pipeline",
            fragment_digest="sha256:stage0",
        )
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "stage0.onnx"
            artifact.write_bytes(b"fake onnx")
            manager = ProviderFragmentInventoryManager(
                "/provider/A",
                supported_backends=("onnx-cpu",),
                free_cpu_memory_mb=2048,
            )
            manager.register_fragment(
                key,
                disk_path=artifact,
                memory_footprint_mb=128,
                repo_available=True,
            )
            self.assertEqual(manager.state_for(key).residency, FragmentResidency.DISK_RESIDENT)

            manager.mark_cpu_resident(key)
            self.assertEqual(manager.state_for(key).residency, FragmentResidency.CPU_RESIDENT)

            manager.mark_gpu_loaded(key)
            state = manager.state_for(key)
            self.assertEqual(state.residency, FragmentResidency.GPU_LOADED)
            self.assertGreater(state.last_used_ms, 0)

            manager.evict(key, from_gpu=True, from_cpu=True)
            self.assertEqual(manager.state_for(key).residency, FragmentResidency.DISK_RESIDENT)
            metadata = manager.ack_metadata(lease_offers=(GenericAdmissionLease(
                lease_id="lease-1",
                request_id="req-1",
                service_name="/Inference/NativeTracer",
                provider_name="/provider/A",
                expires_at_ms=4102444800000,
            ),))
            parsed = parse_ack_metadata(encode_ack_metadata(metadata.to_ack_fields()))
            self.assertEqual(
                parsed["genericAckMetadata"]["service_payload"]["fragment_states"][0]["residency"],
                "DISK_RESIDENT",
            )

    def test_proportional_allocation_matches_capacity_ratio(self) -> None:
        providers = [
            ProviderProfileV1("llm-2gb", llm_stage_capacity_mb=2048, flops_tflops=2),
            ProviderProfileV1("llm-4gb", llm_stage_capacity_mb=4096, flops_tflops=4),
            ProviderProfileV1("llm-8gb", llm_stage_capacity_mb=8192, flops_tflops=8),
        ]
        self.assertEqual(
            proportional_layer_allocation(providers, 28),
            {"llm-2gb": 4, "llm-4gb": 8, "llm-8gb": 16},
        )

    def test_long_context_cache_hit_and_eviction_are_reported(self) -> None:
        manager = LongContextManager(budget_mb=1.0)
        manager.put(prefix_state(
            object_id="prefix-a",
            prefix_id="prefix-a",
            model_id="qwen",
            tokenizer_id="tok",
            provider="llm-8gb",
            token_count=128,
            byte_count=128,
        ), pin=True)
        self.assertIsNotNone(manager.get(ContextObjectKind.PREFIX_STATE, prefix_id="prefix-a"))
        self.assertIsNone(manager.get(ContextObjectKind.PREFIX_STATE, prefix_id="missing"))
        telemetry = manager.telemetry()
        self.assertEqual(telemetry.hits, 1)
        self.assertEqual(telemetry.misses, 1)

    def test_exact_forward_cache_hits_only_identical_forward_scope(self) -> None:
        model = ModelManifestV1(
            model_id="qwen-test",
            revision="r1",
            tokenizer_id="qwen-tokenizer",
            layers=14,
            kv_cache_bytes_per_token_per_layer=64,
        )
        stage = {
            "stageId": "stage-0",
            "role": "/LLM/Stage/0",
            "layerStart": 0,
            "layerEnd": 6,
        }
        key = exact_forward_cache_key_for_stage(
            model,
            stage,
            token_ids=[101, 102, 103],
            plan_hash="plan-a",
            split_layout_hash="layout-a",
            runtime_backend="onnxruntime",
            dtype="float16",
            quantization="none",
            security_epoch="epoch-1",
        )
        manager = ExactForwardCacheManager(budget_mb=64)
        manager.put(ExactForwardCacheEntry(
            key=key,
            provider="llm-4gb",
            object_name=key.data_name(provider="/llm-4gb"),
            byte_count=1024,
            token_count=3,
        ))

        self.assertIsNotNone(manager.get(key))
        token_miss = exact_forward_cache_key_for_stage(
            model,
            stage,
            token_ids=[101, 102, 104],
            plan_hash="plan-a",
            split_layout_hash="layout-a",
            runtime_backend="onnxruntime",
            dtype="float16",
            quantization="none",
            security_epoch="epoch-1",
        )
        stage_miss = exact_forward_cache_key_for_stage(
            model,
            {**stage, "layerEnd": 7},
            token_ids=[101, 102, 103],
            plan_hash="plan-a",
            split_layout_hash="layout-a",
            runtime_backend="onnxruntime",
            dtype="float16",
            quantization="none",
            security_epoch="epoch-1",
        )
        plan_miss = exact_forward_cache_key_for_stage(
            model,
            stage,
            token_ids=[101, 102, 103],
            plan_hash="plan-b",
            split_layout_hash="layout-b",
            runtime_backend="onnxruntime",
            dtype="float16",
            quantization="none",
            security_epoch="epoch-1",
        )
        self.assertIsNone(manager.get(token_miss))
        self.assertIsNone(manager.get(stage_miss))
        self.assertIsNone(manager.get(plan_miss))

        telemetry = manager.telemetry()
        self.assertEqual(telemetry.hits, 1)
        self.assertEqual(telemetry.misses, 3)
        self.assertEqual(telemetry.resident_exact_cache_key_digests, (key.digest(),))

    def test_exact_forward_cache_uses_provider_local_object_names(self) -> None:
        model = ModelManifestV1(
            model_id="qwen-test",
            revision="r1",
            tokenizer_id="qwen-tokenizer",
            layers=14,
        )
        stage = {
            "stageId": "stage-0",
            "role": "/LLM/Stage/0",
            "layerStart": 0,
            "layerEnd": 6,
        }
        key = exact_forward_cache_key_for_stage(
            model,
            stage,
            token_ids=[1, 2, 3],
            plan_hash="plan-a",
            split_layout_hash="layout-a",
            runtime_backend="onnxruntime",
            security_epoch="epoch-1",
        )
        provider_name = key.data_name(provider="/llm-4gb")
        other_provider_name = key.data_name(provider="/llm-8gb")
        self.assertNotEqual(provider_name, other_provider_name)
        self.assertTrue(provider_name.startswith("/llm-4gb/EXACT-FORWARD-CACHE/"))

        manager = ExactForwardCacheManager()
        manager.put(ExactForwardCacheEntry(
            key=key,
            provider="llm-4gb",
            object_name=provider_name,
            byte_count=512,
            token_count=3,
        ))
        self.assertIsNotNone(manager.get(key))
        self.assertEqual(manager.telemetry().resident_exact_cache_key_digests, (key.digest(),))

    def test_semantic_service_cache_hits_only_same_scope_and_confidence(self) -> None:
        key = SemanticServiceCacheKey(
            service_name="/LLM/Qwen/Chat",
            model_id="qwen-small",
            tokenizer_id="qwen-tokenizer",
            policy_epoch="/Policy/chat/v1",
            semantic_pattern_id="weather-question",
            response_schema="chat-completion-v1",
        )
        manager = SemanticServiceCacheManager(budget_mb=1, min_admission_score=0)
        admitted = manager.put(SemanticServiceCacheEntry(
            key=key,
            provider="llm-8gb",
            response_payload=b"cached answer",
            confidence_threshold=0.85,
            estimated_output_tokens=100,
            estimated_saved_decode_tokens=100,
            byte_count=64,
        ))
        self.assertTrue(admitted)
        self.assertIsNotNone(manager.get(key, confidence=0.91))
        self.assertIsNone(manager.get(key, confidence=0.7))
        policy_miss = SemanticServiceCacheKey(
            service_name="/LLM/Qwen/Chat",
            model_id="qwen-small",
            tokenizer_id="qwen-tokenizer",
            policy_epoch="/Policy/chat/v2",
            semantic_pattern_id="weather-question",
            response_schema="chat-completion-v1",
        )
        self.assertIsNone(manager.get(policy_miss, confidence=0.95))
        telemetry = manager.telemetry()
        self.assertEqual(telemetry["hits"], 1)
        self.assertEqual(telemetry["misses"], 2)

    def test_semantic_service_cache_admission_and_eviction_are_token_saving_aware(self) -> None:
        manager = SemanticServiceCacheManager(budget_mb=0.0001, min_admission_score=5000)
        low_value_key = SemanticServiceCacheKey(
            service_name="/LLM/Qwen/Chat",
            model_id="qwen",
            tokenizer_id="tok",
            policy_epoch="epoch",
            semantic_pattern_id="short",
        )
        self.assertFalse(manager.put(SemanticServiceCacheEntry(
            key=low_value_key,
            response_payload=b"x" * 64,
            estimated_output_tokens=1,
            estimated_saved_decode_tokens=1,
            byte_count=64,
            reuse_likelihood=0.1,
        )))

        high_value = SemanticServiceCacheEntry(
            key=SemanticServiceCacheKey(
                service_name="/LLM/Qwen/Chat",
                model_id="qwen",
                tokenizer_id="tok",
                policy_epoch="epoch",
                semantic_pattern_id="long",
            ),
            response_payload=b"x" * 96,
            estimated_output_tokens=500,
            estimated_saved_decode_tokens=500,
            byte_count=96,
            reuse_likelihood=1.0,
        )
        medium_value = SemanticServiceCacheEntry(
            key=SemanticServiceCacheKey(
                service_name="/LLM/Qwen/Chat",
                model_id="qwen",
                tokenizer_id="tok",
                policy_epoch="epoch",
                semantic_pattern_id="medium",
            ),
            response_payload=b"y" * 96,
            estimated_output_tokens=100,
            estimated_saved_decode_tokens=100,
            byte_count=96,
            reuse_likelihood=1.0,
        )
        self.assertTrue(manager.put(high_value))
        self.assertTrue(manager.put(medium_value))
        self.assertIsNotNone(manager.get(high_value.key, confidence=0.95))
        self.assertIsNone(manager.get(medium_value.key, confidence=0.95))
        telemetry = manager.telemetry()
        self.assertEqual(telemetry["rejections"], 1)
        self.assertGreaterEqual(telemetry["evictions"], 1)

    def test_semantic_pattern_metadata_ranks_by_token_saving(self) -> None:
        patterns = rank_semantic_patterns([
            SemanticPatternMeta(
                pattern_id="short",
                query_count=10,
                estimated_saved_tokens=20,
                token_saving_ratio=0.05,
            ),
            SemanticPatternMeta(
                pattern_id="long",
                query_count=3,
                estimated_saved_tokens=500,
                token_saving_ratio=0.7,
            ),
            SemanticPatternMeta(
                pattern_id="medium",
                query_count=5,
                estimated_saved_tokens=150,
                token_saving_ratio=0.3,
            ),
            SemanticPatternMeta(
                pattern_id="tiny",
                query_count=50,
                estimated_saved_tokens=5,
                token_saving_ratio=0.01,
            ),
        ])
        self.assertEqual([pattern.pattern_id for pattern in patterns], [
            "long",
            "medium",
            "short",
            "tiny",
        ])
        self.assertEqual(patterns[0].rank, SemanticPatternRank.HIGH)
        self.assertEqual(patterns[1].rank, SemanticPatternRank.MID)
        self.assertEqual(patterns[2].rank, SemanticPatternRank.LOW)
        self.assertEqual(patterns[3].rank, SemanticPatternRank.UNKNOWN)
        self.assertAlmostEqual(
            semantic_cache_token_saving_ratio(saved_tokens=25, total_tokens=100),
            0.25,
        )

    def test_semantic_cache_entry_from_pattern_uses_rank_and_saved_tokens(self) -> None:
        key = SemanticServiceCacheKey(
            service_name="/LLM/Qwen/Chat",
            model_id="qwen",
            tokenizer_id="tok",
            policy_epoch="epoch",
            semantic_pattern_id="faq-long",
        )
        manager = SemanticServiceCacheManager(min_admission_score=0)
        manager.register_patterns([
            SemanticPatternMeta(
                pattern_id="faq-long",
                conversation_round=2,
                query_count=9,
                estimated_saved_tokens=600,
                token_saving_ratio=0.6,
            ),
            SemanticPatternMeta(
                pattern_id="small-talk",
                conversation_round=1,
                query_count=30,
                estimated_saved_tokens=10,
                token_saving_ratio=0.02,
            ),
        ])
        entry = manager.entry_from_pattern(
            key=key,
            response_payload=b"cached response",
            estimated_output_tokens=100,
            byte_count=128,
        )
        self.assertEqual(entry.conversation_round, 2)
        self.assertEqual(entry.pattern_rank, SemanticPatternRank.HIGH)
        self.assertEqual(entry.estimated_saved_decode_tokens, 600)
        self.assertGreater(entry.cache_benefit_score, 0)
        self.assertTrue(manager.put(entry))
        hint = manager.hint_for(key, confidence=0.95)
        self.assertEqual(hint.pattern_rank, SemanticPatternRank.HIGH)
        self.assertEqual(hint.token_saving_ratio, 0.6)
        telemetry = manager.telemetry()
        self.assertEqual(telemetry["patternCount"], 2)
        self.assertEqual(telemetry["highRankPatterns"], 1)

    def test_semantic_cache_ack_hint_is_coarse_and_private(self) -> None:
        hint = SemanticCacheAckHint(
            disposition=SemanticCacheDisposition.HIT,
            confidence=0.93,
            estimated_saved_decode_tokens=512,
            policy_epoch="/Policy/chat/v1",
            pattern_rank=SemanticPatternRank.HIGH,
            token_saving_ratio=0.6,
        )
        fields = parse_ack_metadata(encode_ack_metadata(semantic_cache_ack_fields(hint)))
        self.assertEqual(fields["semanticCache"], "hit")
        self.assertEqual(fields["semanticCacheConfidenceBucket"], "high")
        self.assertEqual(fields["semanticCacheEstimatedSavedTokens"], "512")
        self.assertEqual(fields["semanticCachePatternRank"], "high")
        self.assertEqual(fields["semanticCacheTokenSavingRatioBucket"], "high")
        self.assertNotIn("semanticPatternId", fields)
        self.assertNotIn("prompt", fields)
        self.assertNotIn("embedding", fields)
        parsed = parse_semantic_cache_ack_hint(fields)
        self.assertEqual(parsed.disposition, SemanticCacheDisposition.HIT)
        self.assertEqual(parsed.estimated_saved_decode_tokens, 512)
        self.assertEqual(parsed.pattern_rank, SemanticPatternRank.HIGH)

    def test_semantic_cache_provider_selection_prefers_hit_and_saved_tokens(self) -> None:
        provider = choose_semantic_cache_provider({
            "provider-a": semantic_cache_ack_fields(SemanticCacheAckHint(
                disposition=SemanticCacheDisposition.CANDIDATE,
                confidence=0.8,
                estimated_saved_decode_tokens=800,
                pattern_rank=SemanticPatternRank.HIGH,
            )),
            "provider-b": semantic_cache_ack_fields(SemanticCacheAckHint(
                disposition=SemanticCacheDisposition.HIT,
                confidence=0.95,
                estimated_saved_decode_tokens=200,
                pattern_rank=SemanticPatternRank.LOW,
            )),
            "provider-c": semantic_cache_ack_fields(SemanticCacheAckHint(
                disposition=SemanticCacheDisposition.MISS,
                confidence=0.0,
                estimated_saved_decode_tokens=1000,
            )),
        })
        self.assertEqual(provider, "provider-b")

    def test_semantic_cache_hint_reports_candidate_below_threshold(self) -> None:
        key = SemanticServiceCacheKey(
            service_name="/LLM/Qwen/Chat",
            model_id="qwen",
            tokenizer_id="tok",
            policy_epoch="epoch",
            semantic_pattern_id="mission",
        )
        manager = SemanticServiceCacheManager(min_admission_score=0)
        manager.put(SemanticServiceCacheEntry(
            key=key,
            confidence_threshold=0.9,
            estimated_saved_decode_tokens=300,
            byte_count=32,
        ))
        hint = manager.hint_for(key, confidence=0.82)
        self.assertEqual(hint.disposition, SemanticCacheDisposition.CANDIDATE)
        self.assertEqual(hint.estimated_saved_decode_tokens, 300)

    def test_ack_metadata_does_not_advertise_provider_local_cache_keys(self) -> None:
        telemetry = RuntimeTelemetryV1(
            provider="llm-8gb",
            runtime_backend="onnxruntime",
            kv_cache=KvCacheTelemetry(
                resident_exact_cache_key_digests=("exact-key-a", "exact-key-b"),
            ),
        )
        fields = parse_ack_metadata(encode_ack_metadata(telemetry.to_ack_fields()))
        self.assertNotIn("residentExactCacheKeyDigests", fields)

    def test_cache_placement_prefers_resident_prefix(self) -> None:
        providers = [
            ProviderProfileV1("llm-4gb", llm_stage_capacity_mb=4096, flops_tflops=4,
                              kv_cache_budget_mb=1024),
            ProviderProfileV1("llm-8gb", llm_stage_capacity_mb=8192, flops_tflops=8,
                              kv_cache_budget_mb=2048),
        ]
        telemetry = {
            "llm-4gb": RuntimeTelemetryV1(
                provider="llm-4gb",
                kv_cache=KvCacheTelemetry(
                    budget_mb=1024,
                    used_mb=100,
                    resident_prefix_ids=("mission",),
                ),
            )
        }
        decision = choose_cache_placement(
            providers,
            telemetry,
            prefix_id="mission",
            required_kv_mb=64,
        )
        self.assertEqual(decision.provider, "llm-4gb")
        self.assertTrue(decision.expected_hit)

    def test_plan_cache_round_trip_and_plan_invariants(self) -> None:
        plan = {
            "planId": "plan-a",
            "plannerMode": "proportional",
            "stages": [
                {"role": "/LLM/Stage/0", "layerStart": 0, "layerEnd": 3},
                {"role": "/LLM/Stage/1", "layerStart": 4, "layerEnd": 11},
            ],
            "dependencies": [{"from": "stage-0", "to": "stage-1"}],
            "shards": [],
        }
        validate_linear_llm_plan(plan)
        model = ModelManifestV1(model_id="qwen", revision="r1", layers=12)
        providers = [ProviderProfileV1("llm-8gb", llm_stage_capacity_mb=8192, flops_tflops=8)]
        lease = make_plan_lease(plan, model=model, providers=providers, ttl_ms=60000)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cache.json"
            cache = PlanCache(path)
            cache.put(lease)
            cache.save()
            loaded = PlanCache(path)
            self.assertIsNotNone(loaded.get(lease.plan_key))
            report = Path(tmp) / "report.json"
            write_runtime_report(report, lease=lease)
            self.assertTrue(report.exists())

    def test_bound_plan_cache_requires_exact_current_runtime_facts(self) -> None:
        plan = {"planId": "bound-plan", "plannerMode": "runtime-aware"}
        model = ModelManifestV1(model_id="qwen", revision="r1", layers=12)
        providers = [ProviderProfileV1(
            "/provider/A", llm_stage_capacity_mb=8192, flops_tflops=8)]
        telemetry = MeasuredTelemetrySnapshotV1(
            provider_name="/provider/A", provider_boot_id="boot-a",
            sequence=7, resource_sequence=6, measured_at_ms=1_000,
            source="linux-proc", status="measured", evidence_epoch=3,
            runner_kind="onnxruntime-cpu", runtime_version="ort-1.26",
            model_digest="sha256:model", plan_digest="sha256:plan",
            artifact_digests={"/Stage/0": "sha256:stage0"},
            device_id="cpu0", membership_version="members-v1",
            network_profile_version="network-v1", cache_version="cache-v1",
        )
        lease = make_plan_lease(
            plan, model=model, providers=providers,
            telemetry_by_provider={"/provider/A": telemetry},
            membership_version="members-v1",
            network_profile_version="network-v1",
            cache_version="cache-v1",
        )
        self.assertIsInstance(lease.bindings, PlanLeaseBindingsV1)
        self.assertEqual(lease.bindings.provider_boot_ids,
                         {"/provider/A": "boot-a"})

        with tempfile.TemporaryDirectory() as tmp:
            cache = PlanCache(Path(tmp) / "bound-cache.json")
            cache.put(lease)
            cache.save()
            loaded = PlanCache(cache.path)
            self.assertIsNone(loaded.get(lease.plan_key))
            self.assertIsNotNone(loaded.get(
                lease.plan_key, bindings=lease.bindings))
            self.assertIsNone(loaded.get(
                lease.plan_key,
                bindings=replace(
                    lease.bindings,
                    provider_boot_ids={"/provider/A": "boot-b"}),
            ))
            self.assertIsNone(loaded.get(
                lease.plan_key,
                bindings=replace(
                    lease.bindings,
                    telemetry_versions={"/provider/A": "boot-a:8:7"}),
            ))

    def test_runtime_smoke_payload(self) -> None:
        payload = runtime_v1_smoke()
        self.assertEqual(payload["allocation"]["llm-8gb"], 16)
        self.assertEqual(payload["cachePlacement"]["provider"], "llm-8gb")

    def test_role_scheduler_allows_provider_to_advance_without_request_barrier(self) -> None:
        scheduler = RolePipelineScheduler()
        scheduler.submit(RoleWorkItem("req-1", "/LLM/Stage/0", "p0", sequence=0))
        scheduler.submit(RoleWorkItem("req-1", "/LLM/Stage/1", "p1", dependencies=("/LLM/Stage/0",), sequence=1))
        scheduler.submit(RoleWorkItem("req-2", "/LLM/Stage/0", "p0", sequence=2))

        first = scheduler.next_ready("p0")
        self.assertEqual(first.request_id, "req-1")
        scheduler.complete("req-1", "/LLM/Stage/0")

        # Provider p0 can immediately start req-2 stage 0 even though p1 has
        # not finished req-1 stage 1 yet.
        second = scheduler.next_ready("p0")
        self.assertEqual(second.request_id, "req-2")
        self.assertEqual(scheduler.next_ready("p1").request_id, "req-1")

    def test_fallback_plans_and_retry_policy(self) -> None:
        plan = {
            "plannerMode": "proportional",
            "stages": [
                {"role": "/LLM/Stage/0", "provider": "p0", "layerStart": 0, "layerEnd": 3},
                {"role": "/LLM/Stage/1", "provider": "p1", "layerStart": 4, "layerEnd": 7},
            ],
            "dependencies": [{"from": "stage-0", "to": "stage-1"}],
            "shards": [],
        }
        fallbacks = generate_fallback_plans(plan)
        self.assertGreaterEqual(len(fallbacks), 2)
        policy = RetryPolicy(max_attempts=1)
        self.assertEqual(
            policy.action_for(
                attempt=1,
                same_provider_available=True,
                alternate_provider_available=True,
                fallback_available=True),
            FailureAction.RETRY_ALTERNATE_PROVIDER,
        )
        self.assertTrue(policy.is_straggler(observed_ms=250, expected_ms=100))

    def test_bounded_dependency_transfer_queues(self) -> None:
        queues = BoundedDependencyTransferQueues(prefetch_window=1, publish_window=1)
        queues.submit(DependencyTransferItem(
            "a",
            TransferQueueKind.PREFETCH,
            bytes_count=1024,
            priority=1,
        ))
        queues.submit(DependencyTransferItem(
            "b",
            TransferQueueKind.PREFETCH,
            bytes_count=1024,
            priority=0,
        ))
        self.assertEqual(queues.next(TransferQueueKind.PREFETCH).item_id, "a")
        self.assertIsNone(queues.next(TransferQueueKind.PREFETCH))
        queues.complete(TransferQueueKind.PREFETCH)
        self.assertEqual(queues.next(TransferQueueKind.PREFETCH).item_id, "b")

    def test_segment_size_compression_and_prefill_decode(self) -> None:
        self.assertGreaterEqual(adaptive_segment_size(1_000_000, rtt_ms=20, bandwidth_mbps=100), 4096)
        compressed, meta = compress_payload(b"x" * 4096)
        self.assertEqual(meta["compression"], "zlib")
        self.assertLess(len(compressed), 4096)
        result = simulate_prefill_decode(
            request_id="req",
            provider=ProviderProfileV1("p", flops_tflops=8),
            model=ModelManifestV1(
                model_id="qwen",
                layers=28,
                supports_streaming=True,
            ),
            prompt_tokens=1024,
            generated_tokens=16,
            microbatch=4,
        )
        self.assertGreater(result.time_to_first_token_ms, 0)
        self.assertEqual(len(result.chunks), 4)

    def test_minindn_runtime_v1_evidence_writer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            model_path = root / "model.json"
            providers_path = root / "providers.json"
            model_path.write_text("""{
  "modelId": "qwen-test",
  "revision": "r1",
  "modelFamily": "llm",
  "layers": 14,
  "memoryPerLayerMb": 64.0,
  "flopsPerLayerTflop": 0.02,
  "contextWindowTokens": 8192,
  "tokenizerId": "qwen-tokenizer",
  "kvCacheBytesPerTokenPerLayer": 64,
  "supportsStreaming": true
}
""", encoding="utf-8")
            providers_path.write_text("""{
  "providers": [
    {
      "provider": "llm-2gb",
      "gpuMemoryMb": 2048,
      "ramMemoryMb": 8192,
      "flopsTflops": 4.0,
      "llmStageCapacityMb": 2048,
      "maxContextTokens": 4096,
      "kvCacheBudgetMb": 512
    },
    {
      "provider": "llm-4gb",
      "gpuMemoryMb": 4096,
      "ramMemoryMb": 16384,
      "flopsTflops": 8.0,
      "llmStageCapacityMb": 4096,
      "maxContextTokens": 8192,
      "kvCacheBudgetMb": 1024
    }
  ]
}
""", encoding="utf-8")
            evidence = write_minindn_runtime_v1_evidence(
                out_dir=root / "evidence",
                model_path=model_path,
                provider_profiles_path=providers_path,
                target_rps=10.0,
                context_tokens=4096,
                generated_tokens=8,
                prefix_id="shared-prefix",
                policy_summary={
                    "summary": {
                        "layerAllocation": {"llm-2gb": 5, "llm-4gb": 9},
                    },
                },
            )
            self.assertEqual(evidence["status"], "available")
            self.assertTrue(evidence["allocationMatchesPolicy"])
            self.assertEqual(evidence["cacheProvider"], "llm-4gb")
            self.assertTrue(evidence["exactForwardCacheHit"])
            self.assertIn("exactForwardCacheKeyDigest", evidence)
            self.assertEqual(evidence["exactForwardCacheSource"], "provider-local")
            self.assertTrue(evidence["exactForwardCacheObjectName"].startswith(
                "/llm-4gb/EXACT-FORWARD-CACHE/"))
            self.assertGreater(evidence["timeToFirstTokenMs"], 0)
            self.assertTrue(Path(evidence["leasePath"]).exists())
            self.assertTrue(Path(evidence["telemetryCsv"]).exists())
            self.assertTrue(Path(evidence["reportPath"]).exists())


if __name__ == "__main__":
    unittest.main()
