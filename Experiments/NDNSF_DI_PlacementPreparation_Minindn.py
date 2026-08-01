#!/usr/bin/env python3
"""Spec 163 frozen MiniNDN placement/preparation and Qwen3 reference matrix.

The network half runs the real NDNSF security/collaboration carrier in four
MiniNDN namespaces with byte-sized artifacts.  The model half runs the frozen
Qwen3-0.6B snapshot on CPU in the sealed runtime image.  They are deliberately
reported as two evidence layers: this script never represents the local
reference generation as multi-GPU or distributed model execution.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import statistics
import subprocess
import sys
import time
from typing import Any

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "NDNSF-DistributedInference"))
TOPOLOGY = REPO / "Experiments" / "Topology" / "AI_Lab.conf"
MODEL_REPO = "Qwen/Qwen3-0.6B"
MODEL_REVISION = "e6de91484c29aa9480d55605af694f39b081c455"
MODEL_SNAPSHOT = (
    Path("/home/tianxing/.cache/ndnsf-spec163-hf/hub")
    / "models--Qwen--Qwen3-0.6B" / "snapshots" / MODEL_REVISION
)
RUNTIME_IMAGE = "ndnsf-di:spec162-qwen36-runtime-aedbff59-20260728b"
PROMPTS = (
    ("ndn-vs-ip", "zh",
     "请用不超过两句话解释：NDN 按内容名称转发与 IP 按主机地址转发的核心区别是什么？"),
    ("identity-binding", "zh",
     "请用不超过两句话解释为什么模型内容哈希和推理语义哈希必须同时绑定，不能只记录模型名称。"),
    ("pipeline-vs-tensor", "en",
     "In at most two sentences, contrast pipeline parallelism (different layer stages) with tensor parallelism (splitting tensor operations within a layer)."),
    ("stage-timeout", "zh",
     "三阶段分布式推理中间阶段超时。请用一个短句依次说明：标记本次尝试失败、取消并隔离旧消息、在原截止时间内重试或重规划；不要假设请求成功。"),
    ("evidence-summary", "en",
     "A run used three distinct GPUs, transferred two checksum-matched hidden states, produced an EOS-terminated answer, and had zero CPU fallback. In at most two sentences, state that this proves those observations for this run but does not prove universal correctness or optimality."),
)
SYSTEM_PROMPT = (
    "Answer directly, accurately, and concisely. Do not show hidden "
    "reasoning. Finish within two sentences and 48 generated tokens."
)

MATRIX_ROWS = (
    "default", "custom", "fan-in", "fan-out", "input-before-model",
    "model-before-input", "cold-dynamic-split", "repository-publication",
    "warm-exact-reuse", "cache-mismatch", "cache-eviction",
    "restart-invalidates-volatile-residency", "request-state-destroyed",
    "exact-prefix-kv-local-hit", "kv-model-mismatch",
    "kv-semantics-mismatch", "kv-adapter-mismatch", "kv-runner-mismatch",
    "kv-split-mismatch", "kv-layer-mismatch", "kv-prefix-mismatch",
    "kv-position-mismatch", "kv-precision-mismatch", "kv-layout-mismatch",
    "kv-cross-tenant-denial", "kv-expiry", "kv-eviction", "kv-restart",
    "kv-pin-loss", "kv-migration-disabled-clean-fallback",
    "non-llm-object-detection", "opaque-single-node-container",
    "selected-but-preparing", "concurrent-GPU-contention",
    "multi-role-Provider", "acceptance-loss", "UNKNOWN", "retry",
    "partial-Selection-upstream-progress", "failure", "permission-denial",
    "NAC-ABE-routing", "plaintext-permission", "UserToken-mismatch",
    "input-key-grant-tamper", "input-key-grant-replay",
    "object-substitution", "noncanonical", "downgrade", "tamper", "replay",
    "restart-cut", "expiry", "cancel-versus-Response", "replan-adoption",
    "mixed-version", "deferred-default", "preplanned-compatibility",
    "commit-idempotency",
)

PYTHON_GATES = (
    "test_ndnsf_di_placement_strategy.py",
    "test_ndnsf_di_external_placement_strategy.py",
    "test_ndnsf_di_split_candidate_contract.py",
    "test_ndnsf_di_presplit_catalog.py",
    "test_ndnsf_di_presplit_first_strategy.py",
    "test_ndnsf_di_automatic_collaboration_plan.py",
    "test_ndnsf_deferred_collaboration.py",
    "test_ndnsf_di_selection_dataflow.py",
    "test_ndnsf_di_dependency_dag.py",
    "test_ndnsf_di_compensation.py",
    "test_ndnsf_di_lifecycle_history.py",
    "test_ndnsf_di_model_family_adapter.py",
    "test_ndnsf_di_model_adapters.py",
    "test_ndnsf_di_core_state.py",
    "test_ndnsf_di_core_ownership.py",
    "test_spec163_experiment_acceptance.py",
)

# Every retained matrix row names the exact assertion that proves it.  A
# passing file/suite is necessary but never sufficient: validate_row_evidence
# also checks that the named assertion still exists and that its gate passed.
ROW_EVIDENCE = {
    "default": (("test_ndnsf_di_presplit_first_strategy",
                 "test_closed_ack_capacity_generates_balanced_graph_valid_split"),),
    "custom": (("test_ndnsf_di_external_placement_strategy",
                "test_bounded_execution_and_untrusted_return_validation"),),
    "fan-in": (("test_ndnsf_di_dependency_dag",
                "test_fan_out_branches_are_independent_and_fan_in_waits_for_all"),),
    "fan-out": (("test_ndnsf_di_dependency_dag",
                 "test_fan_out_branches_are_independent_and_fan_in_waits_for_all"),),
    "input-before-model": (("test_ndnsf_di_dependency_dag",
                            "test_input_before_model_and_model_before_input_start_exactly_once"),),
    "model-before-input": (("test_ndnsf_di_dependency_dag",
                            "test_input_before_model_and_model_before_input_start_exactly_once"),),
    "cold-dynamic-split": (
        ("test_ndnsf_di_presplit_first_strategy",
         "test_closed_ack_capacity_generates_balanced_graph_valid_split"),
        ("secure-minindn-lifecycle", "runtime:automatic-ack-bound-plan"),
    ),
    "repository-publication": (
        ("test_ndnsf_di_presplit_catalog",
         "test_content_addressed_activation_snapshot_and_idempotency"),
        ("distributed-repo-smoke",
         "runtime:distributed-repo-store-fetch"),
    ),
    "warm-exact-reuse": (
        ("test_ndnsf_di_presplit_first_strategy",
         "test_exact_presplit_and_residency_tier_order_are_deterministic"),
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated"),
    ),
    "cache-mismatch": (("test_ndnsf_di_presplit_first_strategy",
                        "test_stale_or_identity_mismatched_cache_is_not_reused"),),
    "cache-eviction": (("test_ndnsf_di_selection_dataflow",
                        "test_residency_revalidation_miss_promotion_and_retention_fences"),),
    "restart-invalidates-volatile-residency": (
        ("test_ndnsf_di_core_state",
         "test_provider_restart_invalidates_prior_boot_epoch_only"),),
    "request-state-destroyed": (
        ("test_ndnsf_di_selection_dataflow",
         "test_derived_state_defaults_to_terminal_destruction"),),
    "exact-prefix-kv-local-hit": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#binding.revalidate"),),
    "kv-model-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#model_identity_hash"),),
    "kv-semantics-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#model_semantics_digest"),),
    "kv-adapter-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#adapter_digest"),),
    "kv-runner-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#runner_digest"),),
    "kv-split-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#split_digest"),),
    "kv-layer-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#layer_start"),),
    "kv-prefix-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#prefix_token_digest"),),
    "kv-position-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#position_digest"),),
    "kv-precision-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#precision"),),
    "kv-layout-mismatch": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#layout_digest"),),
    "kv-cross-tenant-denial": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#security_domain"),),
    "kv-expiry": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#now_ms"),),
    "kv-eviction": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#cache_epoch"),),
    "kv-restart": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#boot_epoch"),),
    "kv-pin-loss": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_is_exact_domain_bound_and_revalidated#pin_live"),),
    "kv-migration-disabled-clean-fallback": (
        ("test_ndnsf_di_selection_dataflow",
         "test_state_reuse_migration_disabled_falls_back_cleanly"),),
    "non-llm-object-detection": (
        ("test_ndnsf_di_model_family_adapter",
         "test_three_model_families_share_one_candidate_carrier#build_object_detection_adapter"),),
    "opaque-single-node-container": (
        ("test_ndnsf_di_model_family_adapter",
         "test_three_model_families_share_one_candidate_carrier#build_opaque_container_adapter"),),
    "selected-but-preparing": (
        ("test_ndnsf_di_selection_dataflow",
         "test_participant_commits_complete_tuple_before_async_preparation"),),
    "concurrent-GPU-contention": (
        ("test_ndnsf_di_lifecycle_history",
         "test_seeded_concurrent_gpu_history_has_zero_capacity_violation"),),
    "multi-role-Provider": (
        ("test_ndnsf_di_selection_dataflow",
         "test_participant_commits_complete_tuple_before_async_preparation"),
        ("secure-minindn-lifecycle", "runtime:multi-role-provider"),
    ),
    "acceptance-loss": (
        ("test_ndnsf_di_selection_dataflow",
         "test_lost_acceptance_is_unknown_and_retry_is_byte_identical"),),
    "UNKNOWN": (
        ("test_ndnsf_di_selection_dataflow",
         "test_lost_acceptance_is_unknown_and_retry_is_byte_identical#UNKNOWN"),),
    "retry": (
        ("test_ndnsf_di_selection_dataflow",
         "test_lost_acceptance_is_unknown_and_retry_is_byte_identical#byte_identical"),),
    "partial-Selection-upstream-progress": (
        ("test_ndnsf_di_selection_dataflow",
         "test_partial_provider_delivery_allows_upstream_progress"),),
    "failure": (
        ("test_ndnsf_di_compensation",
         "test_compensation_is_idempotent_and_converges_after_partition"),),
    "permission-denial": (
        ("encrypted-permission", "TargetIdentityCheckRejectsWrongTarget"),),
    "NAC-ABE-routing": (
        ("secure-minindn-lifecycle", "runtime:nac-abe-routing"),),
    "plaintext-permission": (
        ("encrypted-permission", "ControllerSignedDataIsEncryptedForTargetOnly"),),
    "UserToken-mismatch": (
        ("tokens-replay", "TokenHandshakeNegativeRegression"),),
    "input-key-grant-tamper": (
        ("crypto-authorization",
         "SelectionGatedInputUsesFreshRecipientWrappedKeyAndBoundAad"),),
    "input-key-grant-replay": (
        ("tokens-replay", "ReplayedRuntimeMessagesOnlyTakeEffectOnce"),),
    "object-substitution": (
        ("test_ndnsf_di_dependency_dag",
         "test_manifest_identity_substitution_and_late_events_fail_closed"),),
    "noncanonical": (
        ("test_ndnsf_di_selection_dataflow",
         "test_all_four_v2_envelopes_are_canonical_and_fail_closed#noncanonical"),),
    "downgrade": (
        ("test_ndnsf_di_selection_dataflow",
         "test_all_four_v2_envelopes_are_canonical_and_fail_closed#schemaVersion"),),
    "tamper": (
        ("test_ndnsf_di_selection_dataflow",
         "test_capacity_binding_tamper_and_cross_attempt_replay_fail#tamper"),),
    "replay": (
        ("test_ndnsf_di_selection_dataflow",
         "test_capacity_binding_tamper_and_cross_attempt_replay_fail#replay"),),
    "restart-cut": (
        ("test_ndnsf_di_lifecycle_history",
         "test_exhaustive_duplicate_reorder_loss_terminal_fencing#restart"),),
    "expiry": (
        ("test_ndnsf_di_selection_dataflow",
         "test_residency_revalidation_miss_promotion_and_retention_fences#expires"),),
    "cancel-versus-Response": (
        ("test_ndnsf_di_compensation",
         "test_deadline_and_cancel_response_race_are_first_terminal_wins"),),
    "replan-adoption": (
        ("test_ndnsf_di_compensation",
         "test_cross_attempt_reuse_requires_exact_safe_adoption"),),
    "mixed-version": (
        ("test_ndnsf_di_external_placement_strategy",
         "test_operator_configuration_selects_exact_version_and_state"),),
    "deferred-default": (
        ("test_ndnsf_deferred_collaboration",
         "test_default_di_is_deferred_and_preplanned_is_compatibility_only"),),
    "preplanned-compatibility": (
        ("test_ndnsf_deferred_collaboration",
         "test_default_di_is_deferred_and_preplanned_is_compatibility_only#PREPLANNED"),),
    "commit-idempotency": (
        ("test_ndnsf_deferred_collaboration",
         "test_byte_identical_commit_is_idempotent_and_conflict_has_no_selection"),),
}


def canonical(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def freeze_model(snapshot: Path, output: Path) -> dict[str, Any]:
    if not snapshot.is_dir():
        raise RuntimeError(f"frozen model snapshot is absent: {snapshot}")
    files = []
    for path in sorted(item for item in snapshot.rglob("*") if item.is_file()):
        resolved = path.resolve(strict=True)
        files.append({
            "path": path.relative_to(snapshot).as_posix(),
            "size": resolved.stat().st_size,
            "sha256": sha256_file(resolved),
        })
    manifest = {
        "schema": "ndnsf-di-frozen-model-manifest-v1",
        "repository": MODEL_REPO,
        "revision": MODEL_REVISION,
        "files": files,
    }
    manifest["contentDigest"] = sha256_bytes(canonical(manifest))
    semantics = {
        "schema": "ndnsf-di-model-semantics-v1",
        "modelContentDigest": manifest["contentDigest"],
        "chatTemplate": "tokenizer.apply_chat_template",
        "systemPrompt": SYSTEM_PROMPT,
        "addGenerationPrompt": True,
        "enableThinking": False,
        "decoding": "greedy",
        "maxNewTokens": 64,
        "promptSetDigest": sha256_bytes(canonical(PROMPTS)),
    }
    manifest["semanticsDigest"] = sha256_bytes(canonical(semantics))
    manifest["semantics"] = semantics
    (output / "model-manifest.json").write_bytes(canonical(manifest) + b"\n")
    return manifest


def quantile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(
        (len(ordered) - 1) * fraction + 0.999999)))
    return ordered[index]


def validate_answer(prompt_id: str, answer: str) -> tuple[bool, str]:
    text = " ".join(answer.strip().lower().split())
    predicates = {
        "ndn-vs-ip": (
            "内容" in text and ("主机" in text or "地址" in text),
            "must contrast content-name and host/address forwarding"),
        "identity-binding": (
            "内容" in text and "语义" in text
            and ("哈希" in text or "hash" in text),
            "must bind both content and semantics hashes"),
        "pipeline-vs-tensor": (
            ("stage" in text or "layer" in text)
            and "tensor" in text
            and any(word in text for word in ("split", "partition", "shard")),
            "must distinguish layer stages from within-layer tensor splitting"),
        "stage-timeout": (
            "失败" in text
            and any(word in text for word in ("取消", "隔离"))
            and any(word in text for word in ("重试", "重规划"))
            and ("截止" in text or "deadline" in text),
            "must fail, cancel/fence, and retry/replan within the deadline"),
        "evidence-summary": (
            ("this run" in text or "observ" in text)
            and any(phrase in text for phrase in (
                "does not prove", "not prove",
                "does not confirm", "not confirm"))
            and any(word in text for word in (
                "universal", "correctness", "optimality", "general")),
            "must limit the claim to observations from this run"),
    }
    if prompt_id not in predicates:
        return False, "unknown prompt contract"
    return predicates[prompt_id]


def matched_qwen_plan(
    manifest: dict[str, Any],
    *,
    prompt_token_count: int,
) -> dict[str, Any]:
    """Compare a hand-authored plan with the real default strategy."""

    from ndnsf_distributed_inference.adapters import build_llm_text_adapter
    from ndnsf_distributed_inference.core.ports import CandidateBudget
    from ndnsf_distributed_inference.planner.presplit_first import (
        PreSplitFirstStrategy,
    )
    from ndnsf_distributed_inference.sdk.placement import (
        PlacementRequest,
        ProviderPlanningView,
        evaluate_placement_strategy,
    )

    adapter = build_llm_text_adapter()
    model = adapter.describe_model(
        MODEL_REPO, manifest["contentDigest"], manifest["semanticsDigest"],
        source_revision=MODEL_REVISION)
    graph = adapter.graph.inspect(model)
    candidates = adapter.splitter.enumerate_candidates(model, graph)
    if not candidates:
        raise RuntimeError("Qwen adapter produced no split candidate")
    candidate = candidates[0]
    candidate.validate_against(graph)
    roles = candidate.execution_plan.roles
    manual_started = time.perf_counter_ns()
    manual_plan = tuple(
        (role, f"/provider/manual-{index}")
        for index, role in enumerate(roles))
    manual_digest = sha256_bytes(canonical(manual_plan))
    manual_ms = (time.perf_counter_ns() - manual_started) / 1e6

    providers = tuple(
        ProviderPlanningView(
            provider=provider, service="/LLM/Qwen/Chat",
            boot_epoch=f"qwen-boot-{index}",
            resource_sequence=1,
            offer_digest=sha256_bytes(
                canonical(("offer", provider, prompt_token_count))),
            evidence_digest=sha256_bytes(canonical(("evidence", provider))),
            expires_at_ms=20_000, accepted_deadline_ms=10_000,
            accepted_roles=(role,), backends=("cpu",),
            usable_gpu_memory_mb=1, queue_depth=0,
            estimated_wait_ms=0.0, rtt_ms=float(index + 1),
            bandwidth_mbps=1000.0, cached_shards=(), reusable_state=(),
        )
        for index, (role, provider) in enumerate(manual_plan)
    )
    request = PlacementRequest(
        request_id=f"qwen-matched-{prompt_token_count}", attempt=1,
        deadline_ms=10_000, model_digest=model.model_digest,
        graph_digest=graph.graph_digest,
        candidate_ids=tuple(item.candidate_digest for item in candidates),
        providers=providers, required_roles=roles,
        budget=CandidateBudget(max_candidates=8, max_policy_ms=100),
        constraints={"prompt_token_count": prompt_token_count},
        model=model, graph=graph, candidates=tuple(candidates),
        catalog_snapshot=(),
    )
    automatic_started = time.perf_counter_ns()
    decision = evaluate_placement_strategy(
        PreSplitFirstStrategy(at_ms=1000, security_domain="spec163-local"),
        request, replay_deterministic=True)
    automatic_ms = (time.perf_counter_ns() - automatic_started) / 1e6
    automatic_plan = tuple(
        (assignment.role, assignment.provider)
        for assignment in decision.assignments)
    automatic_digest = sha256_bytes(canonical(automatic_plan))
    if automatic_plan != manual_plan:
        raise RuntimeError(
            "matched Qwen plan differs: "
            f"manual={manual_plan!r} automatic={automatic_plan!r}")
    return {
        "manualPlanMs": manual_ms,
        "automaticPlanMs": automatic_ms,
        "manualPlanDigest": manual_digest,
        "automaticPlanDigest": automatic_digest,
        "automaticDecisionDigest": decision.digest(),
        "automaticStrategy": "PreSplitFirstStrategy",
        "automaticPlanSource":
            decision.evidence["split_specification"]["source"],
        "matchedPlan": True,
    }


def generation_worker(args: argparse.Namespace) -> int:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    torch.set_num_threads(args.cpu_threads)
    snapshot = Path(args.model_snapshot)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((output / "model-manifest.json").read_text())
    load_start = time.perf_counter_ns()
    tokenizer = AutoTokenizer.from_pretrained(
        snapshot, local_files_only=True, trust_remote_code=False)
    model = AutoModelForCausalLM.from_pretrained(
        snapshot, local_files_only=True, trust_remote_code=False,
        torch_dtype="auto", low_cpu_mem_usage=True)
    model.eval()
    load_ms = (time.perf_counter_ns() - load_start) / 1e6
    records: list[dict[str, Any]] = []
    references: dict[str, str] = {}

    def generate(prompt: str) -> dict[str, Any]:
        messages = [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ]
        rendered = tokenizer.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=True,
            enable_thinking=False)
        encoded = tokenizer(rendered, return_tensors="pt")
        attention_mask = encoded["attention_mask"]
        input_ids = encoded["input_ids"]
        generated: list[int] = []
        latencies: list[float] = []
        past = None
        started = time.perf_counter_ns()
        with torch.inference_mode():
            for _ in range(64):
                step_started = time.perf_counter_ns()
                if past is None:
                    result = model(
                        input_ids=input_ids, attention_mask=attention_mask,
                        use_cache=True)
                else:
                    result = model(
                        input_ids=input_ids[:, -1:],
                        attention_mask=attention_mask,
                        past_key_values=past, use_cache=True)
                token = int(result.logits[:, -1, :].argmax(-1).item())
                latencies.append(
                    (time.perf_counter_ns() - step_started) / 1e6)
                generated.append(token)
                past = result.past_key_values
                input_ids = torch.cat(
                    [input_ids, torch.tensor([[token]], dtype=input_ids.dtype)],
                    dim=1)
                attention_mask = torch.cat(
                    [attention_mask, torch.ones(
                        (1, 1), dtype=attention_mask.dtype)], dim=1)
                if token in set(tokenizer.all_special_ids):
                    break
        total_ms = (time.perf_counter_ns() - started) / 1e6
        answer = tokenizer.decode(generated, skip_special_tokens=True)
        return {
            "answer": answer,
            "generatedTokenIds": generated,
            "generatedTokens": len(generated),
            "eosTerminated": bool(
                generated and generated[-1] in set(tokenizer.all_special_ids)),
            "ttftMs": latencies[0] if latencies else None,
            "perTokenLatencyMs": latencies,
            "totalLatencyMs": total_ms,
            "tokensPerSecond": (
                len(generated) / (total_ms / 1000.0) if total_ms else 0.0),
        }

    for prompt_id, language, prompt in PROMPTS:
        for phase, repetitions in (("warmup", 1), ("measured", 5)):
            for repetition in range(1, repetitions + 1):
                plan_comparison = matched_qwen_plan(
                    manifest,
                    prompt_token_count=len(tokenizer.encode(prompt)))
                value = generate(prompt)
                answer_ok, answer_contract = validate_answer(
                    prompt_id, value["answer"])
                if prompt_id not in references:
                    references[prompt_id] = value["answer"]
                value.update({
                    "schema": "ndnsf-di-qwen3-generation-record-v1",
                    "promptId": prompt_id, "language": language,
                    "prompt": prompt, "phase": phase,
                    "repetition": repetition,
                    **plan_comparison,
                    "referenceConsistent": (
                        value["answer"] == references[prompt_id]),
                    "answerContractPassed": answer_ok,
                    "answerContract": answer_contract,
                    "modelLoadMs": load_ms,
                    "device": "cpu",
                    "gpuMetricsDeferred": True,
                })
                records.append(value)
                with (output / "generations.jsonl").open(
                        "a", encoding="utf-8") as stream:
                    stream.write(json.dumps(
                        value, ensure_ascii=False, sort_keys=True) + "\n")

    measured = [row for row in records if row["phase"] == "measured"]
    summary = {
        "schema": "ndnsf-di-qwen3-generation-summary-v1",
        "model": MODEL_REPO, "revision": MODEL_REVISION,
        "device": "cpu", "loadMs": load_ms,
        "warmupCount": len(records) - len(measured),
        "measuredCount": len(measured),
        "successCount": sum(
            bool(row["answer"]) and row["referenceConsistent"]
            and row["eosTerminated"] and row["answerContractPassed"]
            for row in measured),
        "successRate": sum(
            bool(row["answer"]) and row["referenceConsistent"]
            and row["eosTerminated"] and row["answerContractPassed"]
            for row in measured) / len(measured),
        "ttftMs": {
            "p50": statistics.median(row["ttftMs"] for row in measured),
            "p95": quantile([row["ttftMs"] for row in measured], .95),
        },
        "totalLatencyMs": {
            "p50": statistics.median(
                row["totalLatencyMs"] for row in measured),
            "p95": quantile(
                [row["totalLatencyMs"] for row in measured], .95),
        },
        "tokensPerSecond": {
            "p50": statistics.median(
                row["tokensPerSecond"] for row in measured),
            "p95": quantile(
                [row["tokensPerSecond"] for row in measured], .95),
        },
        "matchedPlanComparison": {
            "allMatched": all(row["matchedPlan"] for row in records),
            "strategy": "PreSplitFirstStrategy",
            "manualPlanMs": {
                "p50": statistics.median(
                    row["manualPlanMs"] for row in measured),
                "p95": quantile(
                    [row["manualPlanMs"] for row in measured], .95),
            },
            "automaticPlanMs": {
                "p50": statistics.median(
                    row["automaticPlanMs"] for row in measured),
                "p95": quantile(
                    [row["automaticPlanMs"] for row in measured], .95),
            },
        },
        "gpuSpecificCriteria": "DEFERRED_NO_LOCAL_CUDA",
    }
    (output / "generation-summary.json").write_bytes(
        canonical(summary) + b"\n")
    return 0


def shell_join(values: list[str]) -> str:
    return " ".join(shlex.quote(value) for value in values)


def marker_fields(path: Path, marker: str) -> dict[str, str]:
    for line in path.read_text(errors="replace").splitlines():
        if marker not in line:
            continue
        fields = {}
        for token in line.split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        return fields
    raise RuntimeError(f"missing {marker} in {path}")


def validate_row_evidence(
    gate_results: dict[str, bool],
    runtime_assertions: dict[str, bool],
) -> list[dict[str, Any]]:
    if set(ROW_EVIDENCE) != set(MATRIX_ROWS):
        raise RuntimeError(
            "matrix evidence coverage mismatch: missing="
            f"{sorted(set(MATRIX_ROWS) - set(ROW_EVIDENCE))} extra="
            f"{sorted(set(ROW_EVIDENCE) - set(MATRIX_ROWS))}")
    python_sources = {
        Path(name).stem: REPO / "tests" / "python" / name
        for name in PYTHON_GATES
    }
    inventory = subprocess.run(
        [str(REPO / "build" / "unit-tests"), "--list_content=HRF"],
        cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=True).stdout
    rows = []
    for row in MATRIX_ROWS:
        retained = []
        for gate, assertion_ref in ROW_EVIDENCE[row]:
            if not gate_results.get(gate, False):
                raise RuntimeError(
                    f"matrix row {row} references failed/missing gate {gate}")
            if assertion_ref.startswith("runtime:"):
                assertion = assertion_ref[len("runtime:"):]
                if not runtime_assertions.get(assertion, False):
                    raise RuntimeError(
                        f"matrix row {row} runtime assertion failed: {assertion}")
                source = "retained runtime logs"
            elif gate in python_sources:
                method, _, needle = assertion_ref.partition("#")
                source_path = python_sources[gate]
                source_text = source_path.read_text()
                if f"def {method}(" not in source_text:
                    raise RuntimeError(
                        f"matrix row {row} assertion missing: "
                        f"{source_path}:{method}")
                if needle and needle not in source_text:
                    raise RuntimeError(
                        f"matrix row {row} sub-assertion missing: "
                        f"{source_path}:{method}#{needle}")
                assertion = assertion_ref
                source = str(source_path.relative_to(REPO))
            else:
                assertion = assertion_ref
                if assertion not in inventory:
                    raise RuntimeError(
                        f"matrix row {row} C++ assertion missing: {assertion}")
                source = "build/unit-tests --list_content=HRF"
            retained.append({
                "gate": gate, "assertion": assertion, "source": source,
            })
        rows.append({
            "row": row,
            "status": "PASS",
            "scope": "exact named assertion(s), including MiniNDN where listed",
            "evidence": retained,
        })
    return rows


def run_network_matrix(output: Path) -> dict[str, Any]:
    # MiniNDN 0.1 parses process argv inside its legacy constructor.  The
    # experiment has already parsed and validated its own CLI, so do not let
    # MiniNDN reinterpret those arguments as its standalone launcher options.
    experiment_argv = sys.argv[:]
    sys.argv[:] = [sys.argv[0]]
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.minindn import Minindn
    from minindn.util import getPopen

    sys.path.insert(0, str(REPO / "Experiments"))
    import NDNSF_NewAPI_Minindn_Perf as perf

    runtime = Path("/tmp") / f"spec163-minindn-{os.getpid()}"
    state = runtime / "state"
    state.mkdir(parents=True, exist_ok=True)
    processes = []

    def start(node, name: str, command: str, env: dict[str, str]):
        log = (output / f"{name}.log").open("wb")
        process = getPopen(
            node, command, envDict=env, shell=True,
            stdout=log, stderr=subprocess.STDOUT)
        processes.append((process, log))
        return process, output / f"{name}.log"

    def wait_log(path: Path, marker: str, timeout: float, process=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if process is not None and process.poll() is not None:
                return False
            if path.exists() and marker in path.read_text(errors="replace"):
                return True
            time.sleep(.2)
        return False

    def stop_all():
        for process, log in reversed(processes):
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
            log.close()

    def install_security(ndn):
        identities = (
            "/example/hello", "/example/hello/controller",
            "/example/hello/user", "/example/hello/provider/A",
            "/example/hello/provider/B", "/example/hello/provider/C")
        security = output / "security"
        security.mkdir(exist_ok=True)
        authority = ndn.net["memphis"]
        root_cert = security / "root.cert"
        perf.node_cmd(authority, "ndnsec key-gen -t r /example/hello > " +
                      shlex.quote(str(root_cert)))
        exported = []
        for index, identity in enumerate(identities[1:]):
            request = security / f"identity-{index}.req"
            cert = security / f"identity-{index}.cert"
            key = security / f"identity-{index}.ndnkey"
            perf.node_cmd(authority, "ndnsec key-gen -n -t r " +
                          shlex.quote(identity) + " > " + shlex.quote(str(request)))
            perf.node_cmd(
                authority, "ndnsec cert-gen -s /example/hello -i ROOT " +
                shlex.quote(str(request)) + " > " + shlex.quote(str(cert)))
            perf.node_cmd(authority, "ndnsec cert-install -f " +
                          shlex.quote(str(cert)))
            perf.node_cmd(
                authority, "ndnsec-export -P 123456 -o " +
                shlex.quote(str(key)) + " -i " + shlex.quote(identity))
            exported.append(key)
        for node in ndn.net.hosts:
            perf.node_cmd(node, "ndnsec cert-install -f " +
                          shlex.quote(str(root_cert)) + " >/dev/null")
            for key in exported:
                perf.node_cmd(node, "ndnsec import -P 123456 " +
                              shlex.quote(str(key)) + " >/dev/null")

    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(output / "minindn"))
    sys.argv[:] = experiment_argv
    gate_results: dict[str, bool] = {}
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, output)
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin(
            [ndn.net["memphis"]],
            ["/example/hello/controller", "/example/hello/user",
             "/example/hello/group"])
        for node_name, provider in (
                ("ucla", "A"), ("arizona", "B"), ("wustl", "C")):
            prefix = f"/example/hello/provider/{provider}"
            routing.addOrigin(
                [ndn.net[node_name]], [prefix, prefix + "/KEY",
                                      "/example/hello/group"])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(
                node, "/example/hello", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(
                node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST)
        install_security(ndn)
        args = type("Args", (), {
            "workload_mode": "closed-loop",
            "timeline_trace": False, "debug_ack": False,
            "performance_mode": False, "dk_bootstrap_check": False,
            "crypto_diagnostics": False, "diag_plaintext_ack": False,
            "diag_plaintext_response": False,
            "svs_parallel_sync_processing": False,
            "svs_parallel_workers": 1, "svs_parallel_queue": 64,
            "svs_sync_publish": False,
            "svs_disable_parallel_production": True,
            "svs_parallel_production_workers": None,
            "svs_disable_parallel_production_signing": True,
            "svs_parallel_production_signing": False,
            "svs_disable_parallel_production_extra_block": True,
            "svs_parallel_production_extra_block": False,
            "svs_sync_batching": False, "svs_sync_batch_ms": 0,
            "ack_threads": -1,
        })()
        env = perf.app_env(output, int(time.time()) + os.getpid(), args)
        env["PYTHONPATH"] = ":".join((
            str(REPO / "pythonWrapper"),
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "NDNSF-DistributedRepo" / "pythonWrapper"),
            "/usr/lib/python3/dist-packages"))
        env["LD_LIBRARY_PATH"] = ":".join((
            str(REPO / "build"), "/usr/local/lib",
            "/opt/onnxruntime/lib", "/opt/ndn-base/lib",
            "/opt/ndnsf-app/lib", "/opt/ndnsf/lib"))
        env["NDN_LOG"] = "ndn_service_framework.*=DEBUG"
        controller, controller_log = start(
            ndn.net["memphis"], "controller",
            f"cd {shlex.quote(str(REPO))} && exec "
            f"{shlex.quote(str(REPO / 'build/examples/App_ServiceController'))}",
            env)
        if not wait_log(
                controller_log, "ServiceController listening on:", 15,
                controller):
            raise RuntimeError("MiniNDN Controller failed to start")
        for node_name, provider in (
                ("ucla", "A"), ("arizona", "B"), ("wustl", "C")):
            command = shell_join([
                "/usr/bin/python3",
                str(REPO / "tests/container/placement-preparation/fake_provider.py"),
                "--provider-id", provider, "--state-dir", str(state)])
            command = f"cd {shlex.quote(str(REPO))} && exec {command}"
            process, log = start(
                ndn.net[node_name], f"provider-{provider}", command, env)
            if not wait_log(log, "SPEC163_PROVIDER_READY", 20, process):
                raise RuntimeError(f"MiniNDN Provider {provider} failed")
        user_command = shell_join([
            "/usr/bin/python3",
            str(REPO / "tests/container/placement-preparation/fake_user.py")])
        user_command = (
            f"cd {shlex.quote(str(REPO))} && exec {user_command}")
        user, user_log = start(
            ndn.net["memphis"], "user", user_command, env)
        user.wait(timeout=45)
        gate_results["secure-minindn-lifecycle"] = (
            user.returncode == 0 and wait_log(
                user_log, "SPEC163_SECURE_DEFERRED_LIFECYCLE_OK", 1))
        if not gate_results["secure-minindn-lifecycle"]:
            raise RuntimeError("secure MiniNDN lifecycle failed")

        # Run the contract/failure corpus inside the requester namespace.
        for test in PYTHON_GATES:
            name = Path(test).stem
            command = shell_join([
                "/usr/bin/python3", str(REPO / "tests/python" / test), "-q"])
            process, _ = start(ndn.net["memphis"], name, command, env)
            process.wait(timeout=90)
            gate_results[name] = process.returncode == 0
        if not all(gate_results.values()):
            raise RuntimeError(
                "MiniNDN matrix gate failure: " +
                json.dumps(gate_results, sort_keys=True))
    finally:
        stop_all()
        try:
            ndn.stop()
        finally:
            Minindn.cleanUp()

    # These fixtures intentionally construct their own identities and Faces.
    # Running them inside a namespace whose PIB already contains experiment
    # identities weakens isolation and causes false failures, so execute them
    # after MiniNDN teardown with a fresh HOME.
    unit_commands = {
        "encrypted-permission": "EncryptedPermissionResponse",
        "crypto-authorization": "GenericDynamicApi/CryptoAndAuthorization",
        "tokens-replay": "GenericDynamicApi/TokensAndReplay",
        "opaque-selection": "GenericOpaqueSelection",
        "collaboration-status": "GenericDynamicApi/CollaborationStatus",
    }
    unit_home = output / "unit-home"
    (unit_home / ".ndn").mkdir(parents=True, exist_ok=True)
    unit_env = dict(os.environ)
    unit_env["HOME"] = str(unit_home)
    unit_env["LD_LIBRARY_PATH"] = ":".join((
        str(REPO / "build"), "/usr/local/lib", "/opt/onnxruntime/lib",
        "/opt/ndn-base/lib", "/opt/ndnsf-app/lib", "/opt/ndnsf/lib"))
    for name, suite in unit_commands.items():
        with (output / f"{name}.log").open("wb") as log:
            completed = subprocess.run(
                [str(REPO / "build/unit-tests"), "--run_test=" + suite,
                 "--log_level=nothing"],
                cwd=REPO, env=unit_env, stdout=log,
                stderr=subprocess.STDOUT, timeout=90, check=False)
        gate_results[name] = completed.returncode == 0
    repo_smoke_started = time.perf_counter_ns()
    with (output / "distributed-repo-smoke.log").open("wb") as log:
        completed = subprocess.run(
            [str(REPO / "build" / "NDNSF-DistributedRepo"
                 / "DistributedRepoSmoke")],
            cwd=REPO, env=unit_env, stdout=log,
            stderr=subprocess.STDOUT, timeout=90, check=False)
    repo_smoke_ms = (time.perf_counter_ns() - repo_smoke_started) / 1e6
    repo_smoke_text = (
        output / "distributed-repo-smoke.log").read_text(errors="replace")
    gate_results["distributed-repo-smoke"] = (
        completed.returncode == 0
        and "DISTRIBUTED_REPO_SMOKE_OK" in repo_smoke_text)
    if not all(gate_results.values()):
        raise RuntimeError(
            "isolated matrix gate failure: " +
            json.dumps(gate_results, sort_keys=True))

    plan = marker_fields(output / "user.log", "SPEC163_PLAN_COMMIT")
    split = marker_fields(output / "user.log", "SPEC163_DYNAMIC_SPLIT")
    scope_key = marker_fields(
        output / "user.log", "SPEC163_SCOPE_KEY_PUBLICATION")
    publication = marker_fields(
        output / "provider-A.log", "SPEC163_OUTPUT_PUBLISHED")
    left_fetch = marker_fields(
        output / "provider-B.log", "SPEC163_INPUT_ARRIVED")
    right_fetch = marker_fields(
        output / "provider-C.log", "SPEC163_INPUT_ARRIVED")
    user_text = (output / "user.log").read_text(errors="replace")
    provider_text = "\n".join(
        (output / f"provider-{provider}.log").read_text(errors="replace")
        for provider in ("A", "B", "C"))
    runtime_assertions = {
        "automatic-ack-bound-plan": (
            split.get("strategy") == "PreSplitFirstStrategy"
            and split.get("source") == "ACK_CAPACITY_GENERATED"
            and split.get("matchedManual") == "True"),
        "multi-role-provider": (
            "provider=/example/hello/provider/B roles=left,merge"
            in user_text),
        "nac-abe-routing": all((
            "/NDNSF/REQUEST/HELLO/" in user_text
            and "attributes=/SERVICE/HELLO" in user_text,
            "/NDNSF/SELECTION/HELLO/" in user_text
            and "attributes=/SERVICE/HELLO" in user_text,
            "/NDNSF/ACK/" in provider_text
            and "attributes=/PERMISSION/HELLO" in provider_text,
            "/NDNSF/RESPONSE/" in provider_text
            and "attributes=/PERMISSION/HELLO" in provider_text,
        )),
        "distributed-repo-store-fetch": (
            gate_results["distributed-repo-smoke"]),
    }
    preparation_metrics = {
        "planningMs": float(plan["planningDurationMs"]),
        "selectionCommitMs": float(plan["commitDurationMs"]),
        "dynamicSplitMs": float(split["durationMs"]),
        "scopeKeyPublicationMs": float(scope_key["durationMs"]),
        "scopeKeyPlaintextBytes": int(scope_key["plaintextBytes"]),
        "distributedRepoStoreFetchSmokeMs": repo_smoke_ms,
        "distributedRepoPayloadBytes": 10,
        "activationPublicationMs": float(publication["durationMs"]),
        "activationPayloadBytes": int(publication["payloadBytes"]),
        "activationFetchWaitMs": [
            float(left_fetch["fetchWaitMs"]),
            float(right_fetch["fetchWaitMs"]),
        ],
    }
    rows = validate_row_evidence(gate_results, runtime_assertions)
    matrix = {
        "schema": "ndnsf-di-spec163-minindn-matrix-v2",
        "rows": rows, "gateResults": gate_results,
        "runtimeAssertions": runtime_assertions,
        "preparationMetrics": preparation_metrics,
        "network": {
            "controllerAndRequester": "memphis",
            "providers": {"A": "ucla", "B": "arizona", "C": "wustl"},
            "modelPayload": "byte-sized",
        },
    }
    (output / "matrix.json").write_bytes(canonical(matrix) + b"\n")
    return matrix


def run_generation_container(output: Path) -> None:
    command = [
        "docker", "run", "--rm", "--memory=7g", "--memory-swap=9g",
        "--cpus=4", "--user", f"{os.getuid()}:{os.getgid()}",
        "--tmpfs", (
            f"/run/ndnsf-di:rw,uid={os.getuid()},gid={os.getgid()},mode=0700"),
        "-e", "HOME=/run/ndnsf-di",
        "--entrypoint", "/opt/venv/bin/python",
        "-v", f"{REPO}:/workspace:ro",
        "-v", ("/home/tianxing/.cache/ndnsf-spec163-hf:"
               "/home/tianxing/.cache/ndnsf-spec163-hf:ro"),
        "-v", f"{output}:/evidence:rw",
        RUNTIME_IMAGE,
        "/workspace/Experiments/NDNSF_DI_PlacementPreparation_Minindn.py",
        "--generation-worker", "--output", "/evidence",
        "--model-snapshot", str(MODEL_SNAPSHOT), "--cpu-threads", "4",
    ]
    subprocess.run(command, cwd=REPO, check=True)


def write_summary(output: Path, manifest: dict[str, Any],
                  matrix: dict[str, Any]) -> None:
    generation = json.loads(
        (output / "generation-summary.json").read_text())
    records = [
        json.loads(line)
        for line in (output / "generations.jsonl").read_text().splitlines()
        if line.strip()
    ]
    if (
        len(records) != 30
        or generation["warmupCount"] != 5
        or generation["measuredCount"] != 25
        or generation["successCount"] != 25
        or generation["successRate"] != 1.0
        or not generation["matchedPlanComparison"]["allMatched"]
        or any(row["generatedTokens"] > 64 for row in records)
        or any(not row["answerContractPassed"] for row in records)
        or any(not row["referenceConsistent"] for row in records)
        or any(not row["eosTerminated"] for row in records)
        or len(matrix["rows"]) != len(MATRIX_ROWS)
        or any(row["status"] != "PASS" for row in matrix["rows"])
    ):
        raise RuntimeError(
            "Spec 163 generation/matrix acceptance predicate failed")
    summary = {
        "schema": "ndnsf-di-spec163-acceptance-summary-v1",
        "status": "PASS",
        "modelRef": {
            "name": MODEL_REPO, "revision": MODEL_REVISION,
            "contentDigest": manifest["contentDigest"],
            "semanticsDigest": manifest["semanticsDigest"],
        },
        "matrixRows": len(matrix["rows"]),
        "matrixPassed": sum(
            row["status"] == "PASS" for row in matrix["rows"]),
        "generation": generation,
        "preparationMetrics": {
            **matrix["preparationMetrics"],
            "modelBytesReadFromFrozenSnapshot": sum(
                item["size"] for item in manifest["files"]),
            "diskToCpuRamModelLoadMs": generation["loadMs"],
            "gpuPreparationMs": None,
            "gpuPreparationStatus": "DEFERRED_NO_LOCAL_CUDA",
            "cacheTier": "frozen-local-disk-to-CPU-RAM",
            "cacheReason": "exact content and semantics digest match",
            "gpuUtilization": None,
            "cpuReferenceGeneration": True,
            "recoveryDistributionSource": (
                "bounded lifecycle-history and compensation gates"),
        },
        "claimBoundary": [
            "No TigerCluster or large-model execution",
            "No local CUDA; GPU reload/reuse criteria deferred",
            "Qwen generation is a local CPU reference, not distributed inference",
            "No malicious-computation, distributed-atomicity, deadlock-freedom, universal-optimizer, or unsupported performance claim",
        ],
    }
    (output / "summary.json").write_bytes(canonical(summary) + b"\n")


def quick_smoke() -> int:
    required = [
        TOPOLOGY, MODEL_SNAPSHOT,
        REPO / "tests/container/placement-preparation/fake_provider.py",
        REPO / "tests/container/placement-preparation/fake_user.py",
        REPO / "build/examples/App_ServiceController",
        REPO / "build/unit-tests",
    ]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise RuntimeError("Spec 163 quick smoke missing: " + repr(missing))
    print(
        "SPEC163_PLACEMENT_PREPARATION_QUICK_SMOKE_OK "
        f"model={MODEL_REPO} revision={MODEL_REVISION}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output")
    parser.add_argument("--quick-smoke", action="store_true")
    parser.add_argument("--generation-worker", action="store_true")
    parser.add_argument("--model-snapshot", default=str(MODEL_SNAPSHOT))
    parser.add_argument("--cpu-threads", type=int, default=4)
    parser.add_argument("--skip-network", action="store_true")
    parser.add_argument("--skip-generation", action="store_true")
    args = parser.parse_args()
    if args.quick_smoke:
        return quick_smoke()
    if not args.output:
        parser.error("--output is required outside --quick-smoke")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.generation_worker:
        return generation_worker(args)
    manifest = freeze_model(MODEL_SNAPSHOT, output)
    if args.skip_network:
        matrix = json.loads((output / "matrix.json").read_text())
    else:
        if os.geteuid() != 0:
            raise SystemExit("full MiniNDN matrix must run as root")
        matrix = run_network_matrix(output)
    if args.skip_generation:
        print(f"SPEC163_MININDN_MATRIX_PASS evidence={output}")
        return 0
    run_generation_container(output)
    write_summary(output, manifest, matrix)
    print(f"SPEC163_MININDN_QWEN3_ALL_GATES_PASS evidence={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
