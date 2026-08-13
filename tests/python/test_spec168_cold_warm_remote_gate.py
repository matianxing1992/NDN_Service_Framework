#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess


ROOT = Path(__file__).resolve().parents[2]
JOBS = ROOT / "specs/168-itiger-di-deployment-fidelity/jobs"
ANALYZER = JOBS / "spec168-cold-warm-analyzer.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "spec168_cold_warm", ANALYZER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


cold_warm = load_module()


class Spec168ColdWarmRemoteGateTest(unittest.TestCase):
    def fixture(self, root: Path) -> argparse.Namespace:
        request_prefix = "spec168-cold-warm"
        generation_campaign_id = "spec168-formal-fixture"
        model = "sha256:" + "a" * 64
        workload = "sha256:" + "b" * 64
        source = "sha256:" + "c" * 64
        bundle = "sha256:" + "d" * 64
        sif = "sha256:" + "e" * 64
        stage_path = root / "stage-manifest.json"
        stage_path.write_text(json.dumps({
            "modelDigest": model,
            "layerRanges": [[0, 1], [1, 2], [2, 3]],
            "stages": [{"stageIndex": rank} for rank in range(3)],
        }), encoding="utf-8")
        stage_digest = "sha256:" + hashlib.sha256(
            stage_path.read_bytes()).hexdigest()
        campaign_path = root / "campaign-manifest.json"
        campaign_path.write_text(json.dumps({
            "schema": "ndnsf-di.spec168-campaign.v3",
            "state": "FROZEN",
            "campaignId": "spec168-campaign-v3-cold-warm-fixture",
            "bindingDigests": {
                "sourceDigest": source,
                "sourceBundleDigest": bundle,
                "runtimeSifDigest": sif,
                "remoteSmallStageManifestDigest": stage_digest,
                "promptSetDigest": workload,
            },
        }), encoding="utf-8")
        prompt_ids = [f"prompt-{index}" for index in range(5)]
        generation_path = root / "generation-campaign.json"
        generation_path.write_text(json.dumps({
            "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
            "campaignId": generation_campaign_id,
            "generation": {
                "strategy": "greedy", "maxNewTokens": 64,
                "requireEos": False,
            },
            "repetitions": {
                "warmupPerPrompt": 1,
                "measuredPerPrompt": 5,
                "sequential": True,
            },
            "prompts": [{"promptId": value} for value in prompt_ids],
        }), encoding="utf-8")

        rows = []
        request_ids = []
        for prompt_id in prompt_ids:
            for phase, count, marker in (
                ("warmup", 1, "w"), ("measured", 5, "m"),
            ):
                for repetition in range(count):
                    generation_id = (
                        f"{generation_campaign_id}-{prompt_id}-"
                        f"{marker}{repetition}")
                    request_id = (
                        f"/{request_prefix}--sample--{generation_id}")
                    request_ids.append(request_id)
                    index = len(rows)
                    cache_class = "GENERATED" if index == 0 else "REUSE_CACHED"
                    rows.append({
                        "schemaVersion": "ndnsf-di-qwen-generation-sample-v1",
                        "campaignId": generation_campaign_id,
                        "generationId": generation_id,
                        "promptId": prompt_id,
                        "phase": phase,
                        "repetition": repetition,
                        "status": "OK",
                        "stopReason": "EOS",
                        "exactReferenceMatch": True,
                        "decodedText": f"complete answer {index}",
                        "generatedTokenIds": [10, 11, 12],
                        "ttftMs": 100.0 + index,
                        "interTokenMs": [20.0, 21.0],
                        "totalMs": 150.0 + index,
                        "tokensPerSecond": 20.0,
                        "modelIdentityDigest": model,
                        "workloadDigest": workload,
                        "tokenSteps": [{
                            "mode": "FULL",
                            "metadata": {
                                "requestId": request_id,
                                "wireRequestCount": 1,
                                "tokenRequestCount": 0,
                                "stageCount": 3,
                                "cacheClass": cache_class,
                                "planningTimingsMs": {
                                    "ack_collect_ms": 25.0 + index,
                                    "pre_response_setup_total_ms": 50.0 + index,
                                },
                            },
                        }],
                    })
        (root / "node-0").mkdir()
        (root / "node-0/generation-raw.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in rows),
            encoding="utf-8",
        )
        user_lines = [
            "UserToken/ProviderToken runtime mode: enabled",
            "Installed user permission",
            "MessageValidator Data validated through configured validator "
            "name=/provider/NDNSF/ACK",
        ]
        for index, request_id in enumerate(request_ids):
            cache = "GENERATED" if index == 0 else "REUSE_CACHED"
            catalog = 0 if index == 0 else 1
            user_lines.extend([
                f"NDNSF_DI_AUTOPLANNING_REQUEST_SENT requestId={request_id}",
                f"NDNSF_DI_AUTOPLANNING_ACK_CLOSED requestId={request_id}",
                "NDNSF_DI_AUTOPLANNING_DECISION "
                f"requestId={request_id} preparation={cache} "
                f"catalogCount={catalog}",
                "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED "
                f"requestId={request_id}",
            ])
        (root / "node-0/user.log").write_text(
            "\n".join(user_lines) + "\n", encoding="utf-8")

        (root / "request-gate-open.json").write_text(json.dumps({
            "schemaVersion": "ndnsf-di-request-gate-v1",
            "requestId": request_prefix,
            "epochMs": 1000,
            "mode": "REQUEST_FIRST",
            "certificateMode": "ON_DEMAND_NETWORK_FETCH",
        }), encoding="utf-8")

        for rank in range(3):
            node = root / f"node-{rank}"
            node.mkdir(exist_ok=True)
            (node / "hostname.txt").write_text(
                f"itiger{rank + 7:02d}\n", encoding="utf-8")
            (node / "gpu.csv").write_text(
                f"GPU-{rank}, NVIDIA RTX 5000 Ada Generation, 555.1, 32760 MiB\n",
                encoding="utf-8",
            )
            lines = [
                "LLM_PIPELINE_PROVIDER_READY device=cuda:0 cpuFallback=false",
                "NAC_ABE_BOOTSTRAP complete",
                "Installed provider permission",
                "MessageValidator Data validated through configured validator "
                "name=/user/NDNSF/SELECTION",
            ]
            for index, (request_id, row) in enumerate(zip(request_ids, rows)):
                lines.extend([
                    "LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE "
                    f"requestId={request_id} bytes=1024",
                    f"NDNSF_DI_ACK_DECISION requestId={request_id} status=true",
                    "LLM_PIPELINE_QWEN_SELECTION_PREPARE "
                    f"requestId={request_id} device=cuda:0 cpuFallback=false",
                ])
                for epoch in range(len(row["generatedTokenIds"])):
                    if rank == 0:
                        lines.append(
                            "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED "
                            f"requestId={request_id} epoch={epoch} "
                            f"monotonicMs={1000 + index * 100 + epoch * 20}")
                    else:
                        lines.append(
                            "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED "
                            f"requestId={request_id} epoch={epoch} "
                            f"monotonicMs={1000 + index * 100 + epoch * 20}")
                        output = (
                            "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
                            if rank == 1 else
                            "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED")
                        lines.append(
                            f"{output} requestId={request_id} epoch={epoch} "
                            f"monotonicMs={1001 + index * 100 + epoch * 20}")
                counters = {
                    "repoUniqueBytes": 1024,
                    "repoWireBytes": 1100,
                    "duplicatePayloadBytes": 0,
                    "ramLoadCount": 0,
                    "deviceLoadCount": 1,
                    "diskHitCount": 0,
                    "ramHitCount": 0,
                    "gpuHitCount": index + 1,
                    "evictionCount": 0,
                    "invalidationCount": 0,
                }
                residency = {
                    "schema": "ndnsf-di.provider-residency.v1",
                    "providerBootEpoch": f"boot-{rank}",
                    "records": [{}],
                    "counters": counters,
                }
                lines.append(
                    "LLM_PIPELINE_QWEN_RESIDENCY_RELEASE "
                    f"requestId={request_id} role=stage-{rank} "
                    "snapshot=" + json.dumps(
                        residency, sort_keys=True, separators=(",", ":")))
            (node / f"provider-{rank}.log").write_text(
                "\n".join(lines) + "\n", encoding="utf-8")
            (node / f"provider-markers-{rank}.log").write_text(
                "\n".join(lines) + "\n", encoding="utf-8")

        return argparse.Namespace(
            root=str(root), stage_manifest=str(stage_path),
            campaign_manifest=str(campaign_path),
            generation_campaign=str(generation_path),
            expected_source_digest=source,
            expected_source_bundle_digest=bundle,
            expected_sif_digest=sif,
            request_prefix=request_prefix,
            output_json=str(root / "analysis.json"),
        )

    def test_accepts_one_cold_and_twenty_nine_causally_warm_requests(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = cold_warm.analyze(self.fixture(Path(temporary)))
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["requestCount"], 30)
        self.assertEqual(result["coldRequestCount"], 1)
        self.assertEqual(result["warmRequestCount"], 29)
        self.assertEqual(result["causalReuseVerdict"], "PASS")
        self.assertEqual(len(result["rows"]), 30)

    def test_accepts_interleaved_selection_marker_with_message_name_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            user_log = root / "node-0/user.log"
            text = user_log.read_text(encoding="utf-8")
            first = next(
                line for line in text.splitlines()
                if line.startswith("NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED")
            )
            request_id = first.split("requestId=", 1)[1].split()[0]
            interleaved = (
                "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED123.456 DEBUG "
                "messageName=/NDNSF/SELECTION/" + request_id +
                " messageType=SELECTION"
            )
            user_log.write_text(text.replace(first, interleaved, 1),
                                encoding="utf-8")
            result = cold_warm.analyze(args)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["requestCount"], 30)

    def test_retains_concurrent_ack_fragments_without_rejecting_complete_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            provider = root / "node-0/provider-0.log"
            text = provider.read_text(encoding="utf-8")
            provider.write_text(
                text + "NDNSF_DI_ACK_DECISION "
                "requestId=/spec168-cold-warm--sample--spec168-formal-fixture-"
                "prompt-0-w0 fragment=true\n",
                encoding="utf-8",
            )
            result = cold_warm.analyze(args)
        self.assertEqual(result["status"], "PASS")

    def test_rejects_a_warm_request_that_loads_the_gpu_again(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            marker = root / "node-1/provider-markers-1.log"
            text = marker.read_text(encoding="utf-8")
            old = '"deviceLoadCount":1,"diskHitCount":0'
            new = '"deviceLoadCount":2,"diskHitCount":0'
            first = text.index(old)
            second = text.index(old, first + len(old))
            text = text[:second] + text[second:].replace(old, new, 1)
            marker.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "DEVICE_RELOAD"):
                cold_warm.analyze(args)

    def test_rejects_missing_per_token_latency(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            path = root / "node-0/generation-raw.jsonl"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[0]["interTokenMs"] = []
            path.write_text(
                "".join(json.dumps(row) + "\n" for row in rows),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "INTER_TOKEN_LATENCY"):
                cold_warm.analyze(args)

    def test_rejects_slow_ack_coverage_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            path = root / "node-0/generation-raw.jsonl"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[1]["tokenSteps"][0]["metadata"]["planningTimingsMs"][
                "ack_collect_ms"] = 5000.001
            path.write_text(
                "".join(json.dumps(row) + "\n" for row in rows),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "ACK_COVERAGE_CLOSURE"):
                cold_warm.analyze(args)

    def test_allows_local_pib_miss_when_network_validator_succeeds(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            path = root / "node-2/provider-2.log"
            with path.open("a", encoding="utf-8") as output:
                output.write(
                    "2.000 DEBUG: local PIB certificate lookup miss "
                    "name=/provider/2/NDNSF/SELECTION-STATUS\n")
            result = cold_warm.analyze(args)
            self.assertEqual("PASS", result["status"])

    def test_rejects_final_certificate_validation_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            path = root / "node-2/provider-2.log"
            with path.open("a", encoding="utf-8") as output:
                output.write(
                    "MessageValidator Data validation failed "
                    "name=/provider/2/NDNSF/SELECTION-STATUS\n")
            with self.assertRaisesRegex(RuntimeError, "DATA_VALIDATION_FAILED"):
                cold_warm.analyze(args)

    def test_repeated_job_is_one_allocation_without_retry_or_model_prepare(self):
        source = (JOBS / "gate-e-small-repeated.sbatch").read_text(
            encoding="utf-8")
        self.assertIn("#SBATCH --nodes=3", source)
        self.assertIn("spec168-cold-warm-analyzer.py", source)
        self.assertIn("spec168_cold_warm_contract.py", source)
        self.assertNotIn("snapshot_download", source)
        self.assertNotIn("prepare-qwen", source)
        self.assertNotIn("--dependency=", source)
        self.assertNotRegex(source, r"\bsbatch\b")
        self.assertNotRegex(source, r"\bsleep\s+300(?:\.0+)?\b")

    def test_exact_frozen_formal_manifest_satisfies_repetition_contract(self):
        path = (
            ROOT / "results/spec168-container-candidate/"
            "20260804T095113Z-v63-analyzer-score-evidence/"
            "formal-campaign.json"
        )
        formal = json.loads(path.read_text(encoding="utf-8"))
        repetitions = formal["repetitions"]
        self.assertEqual(repetitions["warmupPerPrompt"], 1)
        self.assertEqual(repetitions["measuredPerPrompt"], 5)
        self.assertIs(repetitions["sequential"], True)
        self.assertEqual(len(formal["prompts"]), 5)
        self.assertEqual(formal["generation"]["maxNewTokens"], 64)
        completed = subprocess.run(
            [
                "python3", str(JOBS / "spec168_cold_warm_contract.py"),
                "--manifest", str(path),
            ],
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("requests=30", completed.stdout)


if __name__ == "__main__":
    unittest.main()
