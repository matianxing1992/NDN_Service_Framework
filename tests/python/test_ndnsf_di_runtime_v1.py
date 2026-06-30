#!/usr/bin/env python3
"""Runtime v1 contract tests for NDNSF-DistributedInference."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from ndnsf_distributed_inference.runtime_v1 import (
    BoundedDependencyTransferQueues,
    ContextObjectKind,
    DependencyTransferItem,
    FailureAction,
    KvCacheTelemetry,
    LongContextManager,
    ModelManifestV1,
    PlanCache,
    ProviderProfileV1,
    RuntimeTelemetryV1,
    RolePipelineScheduler,
    RoleWorkItem,
    RetryPolicy,
    TransferQueueKind,
    adaptive_segment_size,
    choose_cache_placement,
    compress_payload,
    encode_ack_metadata,
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
from ndnsf_distributed_inference.runtime_v1_evidence import write_minindn_runtime_v1_evidence


class RuntimeV1ContractsTest(unittest.TestCase):
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
            self.assertGreater(evidence["timeToFirstTokenMs"], 0)
            self.assertTrue(Path(evidence["leasePath"]).exists())
            self.assertTrue(Path(evidence["telemetryCsv"]).exists())
            self.assertTrue(Path(evidence["reportPath"]).exists())


if __name__ == "__main__":
    unittest.main()
