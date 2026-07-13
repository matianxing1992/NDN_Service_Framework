#!/usr/bin/env python3
"""Spec 107 diagnostic attribution and single-branch gate tests."""

from __future__ import annotations

from pathlib import Path
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ndnsf-di"))

from run_spec107_attribution import (  # noqa: E402
    AttributionError,
    build_bottleneck_decision,
    derive_attribution_inputs,
    load_json_lines,
    parse_timeline_log,
)
from spec107_artifacts import materialize_artifact_set  # noqa: E402
from spec107_identity import (  # noqa: E402
    build_campaign_identity,
    build_candidate_identity,
    committed_source_digest,
)


CANDIDATE = (
    "spec107-c1-111111111111-222222222222-333333333333-"
    "444444444444-555555555555-666666666666")


def diagnostic_campaign() -> dict[str, object]:
    return {
        "schema": "ndnsf-di-spec107-campaign-v1",
        "campaignId": "spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
        "candidateId": CANDIDATE,
        "kind": "diagnostic",
        "eligibility": "DIAGNOSTIC_INELIGIBLE",
        "releaseEligible": False,
    }


def reconciliation() -> dict[str, object]:
    return {
        "schema": "ndnsf-di-spec107-timing-reconciliation-v1",
        "candidateId": CANDIDATE,
        "campaignId": "spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
        "coverageRatio": 1.0,
        "verdict": "PASS",
        "steps": [
            {"observedMs": 1000.0, "reconciledMs": 1000.0, "errors": []},
            {"observedMs": 800.0, "reconciledMs": 800.0, "errors": []},
        ],
    }


class Spec107AttributionTest(unittest.TestCase):
    def test_harness_revalidates_clean_committed_source_at_execution_time(self) -> None:
        harness_path = REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        module_spec = importlib.util.spec_from_file_location(
            "spec107_llm_harness_source", harness_path)
        assert module_spec is not None and module_spec.loader is not None
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(
                ["git", "config", "user.email", "spec107@example.invalid"],
                cwd=root, check=True)
            subprocess.run(
                ["git", "config", "user.name", "Spec 107 Test"],
                cwd=root, check=True)
            source = root / "source.txt"
            source.write_text("frozen\n", encoding="utf-8")
            subprocess.run(["git", "add", "source.txt"], cwd=root, check=True)
            subprocess.run(
                ["git", "-c", "commit.gpgsign=false", "commit", "-qm", "frozen"],
                cwd=root, check=True)
            placeholder = lambda value: "sha256:" + value * 64
            candidate = build_candidate_identity({
                "source": committed_source_digest(root),
                "profile": placeholder("2"), "model": placeholder("3"),
                "plan": placeholder("4"), "artifact": placeholder("5"),
                "lineage": placeholder("6"), "workload": placeholder("7"),
                "tokenizer": placeholder("8"), "trustPolicy": placeholder("9"),
                "command": placeholder("a"),
            })
            harness.validate_spec107_source_binding(candidate, root)
            source.write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(
                    ValueError, "SPEC107_SOURCE_TREE_DIRTY"):
                harness.validate_spec107_source_binding(candidate, root)
            subprocess.run(["git", "add", "source.txt"], cwd=root, check=True)
            subprocess.run(
                ["git", "-c", "commit.gpgsign=false", "commit", "-qm", "changed"],
                cwd=root, check=True)
            with self.assertRaisesRegex(
                    ValueError, "SPEC107_SOURCE_CANDIDATE_DIGEST_MISMATCH"):
                harness.validate_spec107_source_binding(candidate, root)

    def test_harness_binds_profile_and_workload_to_actual_diagnostic_cell(self) -> None:
        harness_path = REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        module_spec = importlib.util.spec_from_file_location(
            "spec107_llm_harness_profile_workload", harness_path)
        assert module_spec is not None and module_spec.loader is not None
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile = root / "profile.json"
            workload = root / "workload.json"
            profile.write_text(json.dumps({
                "schema": "ndnsf-di-spec107-diagnostic-profile-v1",
                "topology": "Experiments/Topology/AI_Lab.conf",
                "stageCount": 3,
                "roles": [f"/LLM/Pipeline/Stage/{index}" for index in range(3)],
                "runtime": "qwen-onnx-cpu-native",
                "physicalProductionOverall": "DEFERRED",
            }) + "\n", encoding="utf-8")
            workload.write_text(json.dumps({
                "schema": "ndnsf-di-spec107-diagnostic-workload-v1",
                "prompt": "frozen prompt",
                "expectedTokenIds": [1, 2, 3],
                "automaticRetry": False,
                "cells": [{
                    "ordinal": 1, "warmupRequests": 0,
                    "measuredRequests": 1, "maxNewTokens": 3,
                    "measuredDurationSeconds": 0, "requestIntervalMs": 0,
                }],
            }) + "\n", encoding="utf-8")

            def digest(path: Path) -> str:
                return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()

            candidate = build_candidate_identity({
                "source": "sha256:" + "1" * 64,
                "profile": digest(profile), "model": "sha256:" + "3" * 64,
                "plan": "sha256:" + "4" * 64,
                "artifact": "sha256:" + "5" * 64,
                "lineage": "sha256:" + "6" * 64,
                "workload": digest(workload), "tokenizer": "sha256:" + "8" * 64,
                "trustPolicy": "sha256:" + "9" * 64,
                "command": "sha256:" + "a" * 64,
            })
            args = SimpleNamespace(
                topology_file="Experiments/Topology/AI_Lab.conf", stages=3,
                runtime="qwen-onnx-cpu-native", prompt="frozen prompt",
                expected_token_ids="1,2,3", warmup_requests=0,
                measured_requests=1, max_new_tokens=3,
                measured_duration_s=0.0, request_interval_ms=0.0)
            cell = {"ordinal": 1}
            harness.validate_spec107_profile_workload_binding(
                candidate, args, cell, profile, workload)
            workload.write_text('{"schema":"tampered"}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                    ValueError, "SPEC107_WORKLOAD_CANDIDATE_DIGEST_MISMATCH"):
                harness.validate_spec107_profile_workload_binding(
                    candidate, args, cell, profile, workload)

    def test_harness_invalid_preflight_is_retained_without_creating_output(self) -> None:
        harness_path = REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        module_spec = importlib.util.spec_from_file_location(
            "spec107_llm_harness_preflight", harness_path)
        assert module_spec is not None and module_spec.loader is not None
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            rows = []
            for index in range(3):
                path = source / f"stage-{index}.onnx"
                path.write_bytes(f"stage-{index}".encode())
                rows.append({
                    "role": f"/LLM/Pipeline/Stage/{index}",
                    "source": path.name,
                })
            materialized = materialize_artifact_set(
                source_root=source,
                output_root=root / "results/spec107-artifacts",
                artifacts=rows,
                model_revision="frozen",
                repo_root=root,
                reserve_bytes=0,
            )
            store = Path(materialized["storePath"])
            plan = root / "plan.json"
            plan.write_text(json.dumps({
                "version": 2,
                "services": [{
                    "runtimeBackend": "onnxruntime",
                    "roles": [f"/LLM/Pipeline/Stage/{index}" for index in range(3)],
                }],
            }) + "\n", encoding="utf-8")

            def digest(path: Path) -> str:
                return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()

            candidate = build_candidate_identity({
                "source": "sha256:" + "1" * 64,
                "profile": "sha256:" + "2" * 64,
                "model": "sha256:" + "3" * 64,
                "plan": digest(plan),
                "artifact": digest(store / "artifact-set.json"),
                "lineage": "sha256:" + "6" * 64,
                "workload": "sha256:" + "7" * 64,
                "tokenizer": "sha256:" + "8" * 64,
                "trustPolicy": "sha256:" + "9" * 64,
                "command": "sha256:" + "a" * 64,
            })
            campaign = build_campaign_identity(
                candidate, kind="diagnostic", ordinal=1,
                command_digest="sha256:" + "a" * 64,
                output_root="results/spec107-attribution-c1/warm-single")
            with self.assertRaisesRegex(SystemExit, "INVALID_PREFLIGHT"):
                harness.enforce_spec107_harness_preflight(
                    candidate=candidate,
                    campaign=campaign,
                    artifact_store=store,
                    artifact_manifest=materialized["manifest"],
                    plan=plan,
                    repo_root=root,
                    projected_new_bytes=256 * 1024 * 1024,
                    free_bytes=0,
                )
            output = root / str(campaign["outputRoot"])
            self.assertFalse(output.exists())
            record = output.with_name(output.name + ".invalid-preflight.json")
            self.assertTrue(record.is_file())
            self.assertEqual(
                json.loads(record.read_text(encoding="utf-8"))["verdict"],
                "INVALID_PREFLIGHT")

    def test_harness_binds_store_and_qwen_manifests_to_candidate_digests(self) -> None:
        harness_path = REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        module_spec = importlib.util.spec_from_file_location(
            "spec107_llm_harness_binding", harness_path)
        assert module_spec is not None and module_spec.loader is not None
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            source.mkdir()
            rows = []
            for index in range(3):
                path = source / f"stage-{index}.onnx"
                path.write_bytes(f"stage-{index}".encode())
                rows.append({
                    "role": f"/LLM/Pipeline/Stage/{index}",
                    "source": path.name,
                })
            materialized = materialize_artifact_set(
                source_root=source,
                output_root=root / "results/spec107-artifacts",
                artifacts=rows,
                model_revision="frozen",
                repo_root=root,
                reserve_bytes=0,
            )
            store = Path(materialized["storePath"])
            service = root / "service.json"
            runtime = root / "runtime.json"
            plan = root / "plan.json"
            policy = root / "policy.yaml"
            service.write_text('{"schema":"qwen-service"}\n', encoding="utf-8")
            runtime.write_text('{"schema":"qwen-runtime"}\n', encoding="utf-8")
            plan.write_text('{"version":2}\n', encoding="utf-8")
            policy.write_text('trust:\n  app_roots: [/example/llm-pipeline]\n',
                              encoding="utf-8")

            def digest(path: Path) -> str:
                return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()

            digests = {
                "source": "sha256:" + "1" * 64,
                "profile": "sha256:" + "2" * 64,
                "model": digest(service),
                "plan": digest(plan),
                "artifact": digest(store / "artifact-set.json"),
                "lineage": "sha256:" + "6" * 64,
                "workload": "sha256:" + "7" * 64,
                "tokenizer": digest(runtime),
                "trustPolicy": digest(policy),
                "command": "sha256:" + "a" * 64,
            }
            candidate = build_candidate_identity(digests)
            result = harness.validate_spec107_artifact_binding(
                candidate, store, service, runtime)
            self.assertEqual(result["artifactSetDigest"],
                             materialized["manifest"]["artifactSetDigest"])
            harness.validate_spec107_execution_binding(candidate, plan, policy)
            plan.write_text('{"version":3}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                ValueError, "SPEC107_PLAN_CANDIDATE_DIGEST_MISMATCH"):
                harness.validate_spec107_execution_binding(candidate, plan, policy)
            service.write_text('{"schema":"tampered"}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                ValueError, "SPEC107_MODEL_CANDIDATE_DIGEST_MISMATCH"):
                harness.validate_spec107_artifact_binding(
                    candidate, store, service, runtime)

    def test_harness_binds_actual_diagnostic_arguments_to_command_profile(self) -> None:
        harness_path = REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        module_spec = importlib.util.spec_from_file_location(
            "spec107_llm_harness_command", harness_path)
        assert module_spec is not None and module_spec.loader is not None
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp) / "command.json"
            profile.write_text(json.dumps({
                "schema": "ndnsf-di-spec107-diagnostic-command-profile-v1",
                "execution": {
                    "runtime": "qwen-onnx-cpu-native",
                    "prompt": "frozen prompt", "ndnLog": "*=INFO",
                    "timingSampleRate": 1,
                },
                "cells": [{
                    "name": "warm-single", "ordinal": 1,
                    "outputRoot": "results/spec107-attribution-c1/warm-single",
                    "warmupRequests": 0, "measuredRequests": 1,
                    "maxNewTokens": 32, "measuredDurationSeconds": 0,
                    "requestIntervalMs": 0,
                }],
            }), encoding="utf-8")
            command_digest = "sha256:" + hashlib.sha256(
                profile.read_bytes()).hexdigest()
            digests = {
                "source": "sha256:" + "1" * 64,
                "profile": "sha256:" + "2" * 64,
                "model": "sha256:" + "3" * 64,
                "plan": "sha256:" + "4" * 64,
                "artifact": "sha256:" + "5" * 64,
                "lineage": "sha256:" + "6" * 64,
                "workload": "sha256:" + "7" * 64,
                "tokenizer": "sha256:" + "8" * 64,
                "trustPolicy": "sha256:" + "9" * 64,
                "command": command_digest,
            }
            candidate = build_candidate_identity(digests)
            campaign = {
                "commandDigest": command_digest, "ordinal": 1,
                "outputRoot": "results/spec107-attribution-c1/warm-single",
            }
            args = SimpleNamespace(
                runtime="qwen-onnx-cpu-native", prompt="frozen prompt",
                ndn_log="*=INFO", spec107_timing_sample_rate=1,
                warmup_requests=0, measured_requests=1, max_new_tokens=32,
                measured_duration_s=0.0, request_interval_ms=0.0,
            )
            cell = harness.validate_spec107_command_binding(
                candidate, campaign, args, profile)
            self.assertEqual(cell["name"], "warm-single")
            args.max_new_tokens = 31
            with self.assertRaisesRegex(
                ValueError, "SPEC107_COMMAND_ARGUMENT_MISMATCH"):
                harness.validate_spec107_command_binding(
                    candidate, campaign, args, profile)
            args.max_new_tokens = 32
            flat_value = json.loads(profile.read_text(encoding="utf-8"))
            flat_value["cells"][0]["outputRoot"] = (
                "results/spec107-c1-attribution-warm-single")
            profile.write_text(json.dumps(flat_value), encoding="utf-8")
            flat_digest = "sha256:" + hashlib.sha256(
                profile.read_bytes()).hexdigest()
            flat_candidate = build_candidate_identity({
                **digests, "command": flat_digest})
            flat_campaign = {
                "commandDigest": flat_digest, "ordinal": 1,
                "outputRoot": "results/spec107-c1-attribution-warm-single",
            }
            with self.assertRaisesRegex(
                    ValueError, "SPEC107_COMMAND_OUTPUT_ROOT_INVALID"):
                harness.validate_spec107_command_binding(
                    flat_candidate, flat_campaign, args, profile)

    def test_raw_events_derive_complete_reconciliation_and_hypotheses(self) -> None:
        campaign = diagnostic_campaign()
        request_id = "/request/1"
        client = []
        for component, event, start, end in (
            ("inter-token", "inter_token", 0.0, 0.0),
            ("encode-decode", "request_encode", 0.0, 1.0),
            ("encode-decode", "response_decode", 9.0, 10.0),
            ("observed-step", "token_step", 0.0, 10.0),
        ):
            client.append({
                "schema": "ndnsf-di-spec107-client-timing-event-v1",
                "candidateId": campaign["candidateId"],
                "campaignId": campaign["campaignId"],
                "generationId": "generation-1", "tokenEpoch": 0,
                "requestId": request_id, "attemptEpoch": 0,
                "component": component, "event": event,
                "startMs": start, "endMs": end,
                "status": "COMPLETED", "sampled": True,
            })
        pairs = (
            ("request_created", "request_publish_done"),
            ("ack_selection_start", "ack_selection_done"),
            ("role_validation_start", "role_validation_done"),
            ("role_queue_enter", "role_queue_exit"),
            ("role_compute_start", "role_compute_done"),
            ("dependency_fetch_start", "dependency_fetch_done"),
            ("dependency_publish_start", "dependency_publish_done"),
            ("response_observed", "callback_done"),
        )
        timeline = []
        steady = 0
        for start_event, done_event in pairs:
            timeline.extend([
                {"event": start_event, "steadyUs": steady,
                 "requestId": request_id, "fields": {}},
                {"event": done_event, "steadyUs": steady + 1000,
                 "requestId": request_id, "fields": {}},
            ])
            steady += 1000
        reconciliation, hypotheses = derive_attribution_inputs(
            campaign=campaign, client_events=client, timeline_rows=timeline)
        self.assertEqual(reconciliation["verdict"], "PASS")
        self.assertEqual(reconciliation["coverageRatio"], 1.0)
        self.assertEqual(len(hypotheses), 4)
    def test_selects_exactly_one_unique_branch_at_or_above_twenty_five_percent(self) -> None:
        hypotheses = [
            {"branch": "generation-session", "avoidableMs": 500.0,
             "sourceTouchpoints": ["llm_pipeline/user.py"]},
            {"branch": "onnx-compute", "avoidableMs": 200.0,
             "sourceTouchpoints": ["OnnxRuntimeModelRunner.cpp"]},
            {"branch": "tensor-codec", "avoidableMs": 100.0,
             "sourceTouchpoints": ["TensorBundleCodec.cpp"]},
        ]
        result = build_bottleneck_decision(
            campaign=diagnostic_campaign(), reconciliation=reconciliation(),
            hypotheses=hypotheses)
        self.assertEqual(result["verdict"], "SELECTED")
        self.assertEqual(result["selectedBranch"], "generation-session")
        self.assertAlmostEqual(result["dominanceRatio"], 500.0 / 900.0)
        self.assertEqual(
            [row["branch"] for row in result["rejectedBranches"]],
            ["onnx-compute", "tensor-codec"],
        )
        self.assertFalse(result["releaseEligible"])
        self.assertEqual(result["eligibility"], "DIAGNOSTIC_INELIGIBLE")

    def test_no_branch_at_threshold_requires_replan(self) -> None:
        result = build_bottleneck_decision(
            campaign=diagnostic_campaign(), reconciliation=reconciliation(),
            hypotheses=[
                {"branch": "a", "avoidableMs": 200.0,
                 "sourceTouchpoints": ["a.cpp"]},
                {"branch": "b", "avoidableMs": 190.0,
                 "sourceTouchpoints": ["b.cpp"]},
            ])
        self.assertEqual(result["verdict"], "BLOCK_REPLAN")
        self.assertIsNone(result["selectedBranch"])
        self.assertEqual(result["reason"], "NO_BRANCH_MEETS_DOMINANCE")

    def test_tied_largest_branches_require_replan_not_arbitrary_selection(self) -> None:
        result = build_bottleneck_decision(
            campaign=diagnostic_campaign(), reconciliation=reconciliation(),
            hypotheses=[
                {"branch": "a", "avoidableMs": 300.0,
                 "sourceTouchpoints": ["a.cpp"]},
                {"branch": "b", "avoidableMs": 300.0,
                 "sourceTouchpoints": ["b.cpp"]},
            ])
        self.assertEqual(result["verdict"], "BLOCK_REPLAN")
        self.assertEqual(result["reason"], "LARGEST_BRANCH_NOT_UNIQUE")

    def test_acceptance_eligible_or_identity_mismatched_campaign_is_rejected(self) -> None:
        campaign = diagnostic_campaign()
        campaign["releaseEligible"] = True
        campaign["eligibility"] = "EVIDENCE_ELIGIBLE"
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_CAMPAIGN_NOT_DIAGNOSTIC"):
            build_bottleneck_decision(
                campaign=campaign, reconciliation=reconciliation(), hypotheses=[])

        campaign = diagnostic_campaign()
        evidence = reconciliation()
        evidence["campaignId"] = "spec107-c1-diagnostic-r2-bbbbbbbbbbbb"
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_IDENTITY_MISMATCH"):
            build_bottleneck_decision(
                campaign=campaign, reconciliation=evidence, hypotheses=[])

    def test_incomplete_timing_duplicate_hypothesis_and_invalid_measurement_fail(self) -> None:
        evidence = reconciliation()
        evidence["coverageRatio"] = 0.98
        evidence["verdict"] = "BLOCK_COVERAGE"
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_TIMING_INVALID"):
            build_bottleneck_decision(
                campaign=diagnostic_campaign(), reconciliation=evidence, hypotheses=[])

        duplicate = [
            {"branch": "a", "avoidableMs": 300.0, "sourceTouchpoints": ["a"]},
            {"branch": "a", "avoidableMs": 200.0, "sourceTouchpoints": ["b"]},
        ]
        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_BRANCH_DUPLICATE"):
            build_bottleneck_decision(
                campaign=diagnostic_campaign(), reconciliation=reconciliation(),
                hypotheses=duplicate)

        with self.assertRaisesRegex(AttributionError, "ATTRIBUTION_MEASUREMENT_INVALID"):
            build_bottleneck_decision(
                campaign=diagnostic_campaign(), reconciliation=reconciliation(),
                hypotheses=[{"branch": "a", "avoidableMs": -1,
                             "sourceTouchpoints": ["a"]}])

    def test_client_timing_writer_is_exclusive_sampled_and_content_free(self) -> None:
        llm_dir = REPO / "examples/python/NDNSF-DistributedInference/llm_pipeline"
        sys.path.insert(0, str(llm_dir))
        try:
            module_spec = importlib.util.spec_from_file_location(
                "spec107_llm_user", llm_dir / "user.py")
            assert module_spec is not None and module_spec.loader is not None
            module = importlib.util.module_from_spec(module_spec)
            module_spec.loader.exec_module(module)
        finally:
            sys.path.remove(str(llm_dir))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "client-timing.jsonl"
            writer = module._Spec107ClientTimingWriter(
                path, candidate_id=CANDIDATE,
                campaign_id="spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
                sample_rate=1)
            writer.event(
                generation_id="generation-1", token_epoch=0,
                request_id="/request/1", component="encode-decode",
                event="request_encode", started_ms=1.0, ended_ms=2.0)
            writer.close()
            row = json.loads(path.read_text(encoding="utf-8"))
            self.assertTrue(row["sampled"])
            self.assertNotIn("payload", row)
            self.assertNotIn("token", row)
            self.assertNotIn("tensor", row)
            with self.assertRaises(FileExistsError):
                module._Spec107ClientTimingWriter(
                    path, candidate_id=CANDIDATE,
                    campaign_id="spec107-c1-diagnostic-r1-aaaaaaaaaaaa",
                    sample_rate=1)

    def test_parses_core_timeline_and_harness_exposes_diagnostic_only_flags(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "provider.log"
            log.write_text(
                "prefix NDNSF_TIMELINE role=di-provider event=role_compute_start "
                "steady_us=123 timestamp_us=456 requestId=/request/1 "
                "sessionId=generation-1 role=/LLM/Stage/0 attemptEpoch=0\n",
                encoding="utf-8")
            rows = parse_timeline_log(log)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["event"], "role_compute_start")
            self.assertEqual(rows[0]["fields"]["attemptEpoch"], "0")

            jsonl = root / "events.jsonl"
            jsonl.write_text('{"sampled":true}\n', encoding="utf-8")
            self.assertEqual(load_json_lines(jsonl), [{"sampled": True}])

        harness_path = REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        module_spec = importlib.util.spec_from_file_location(
            "spec107_llm_harness", harness_path)
        assert module_spec is not None and module_spec.loader is not None
        harness = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(harness)
        args = harness.build_parser().parse_args([
            "--spec107-diagnostic", "generation-session-attribution",
            "--candidate-manifest", "candidate.json",
            "--campaign-manifest", "campaign.json",
            "--spec107-timing-sample-rate", "10",
            "--spec107-artifact-store", "artifact-store",
            "--spec107-qwen-service-manifest", "service.json",
            "--spec107-qwen-runtime-manifest", "runtime.json",
            "--spec107-command-profile", "command.json",
        ])
        self.assertEqual(args.spec107_diagnostic, "generation-session-attribution")
        self.assertEqual(args.candidate_manifest, "candidate.json")
        self.assertEqual(args.campaign_manifest, "campaign.json")
        self.assertEqual(args.spec107_timing_sample_rate, 10)
        self.assertEqual(args.spec107_artifact_store, "artifact-store")
        self.assertEqual(args.spec107_qwen_service_manifest, "service.json")
        self.assertEqual(args.spec107_qwen_runtime_manifest, "runtime.json")
        self.assertEqual(args.spec107_command_profile, "command.json")


if __name__ == "__main__":
    unittest.main()
