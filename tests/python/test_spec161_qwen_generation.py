from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import subprocess
import sys
import tempfile
import time
import types
import unittest


ROOT = Path(__file__).resolve().parents[2]
CAPACITY = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/check-qwen32b-capacity.py"
ANALYZER = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/analyze-qwen32b-generation.py"
SMOKE_ANALYZER = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/analyze-qwen32b-generation-smoke.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class QwenGenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.capacity = load_module("spec161_capacity_preflight", CAPACITY)
        cls.analyzer = load_module("spec161_generation_analyzer", ANALYZER)


    def test_capacity_separates_scratch_peak_from_durable_promotion(self) -> None:
        gib = 1024 ** 3
        base = {
            "currentUsageBytes": 67 * gib,
            "verifiedQuotaBytes": 200 * gib,
            "quotaAuthority": "project-quota-command",
            "scratchPath": "/scratch/spec161",
            "scratchCapacityAuthority": "slurm-allocation",
            "scratchFreeBytes": 500 * gib,
            "scratchWritable": True,
            "scratchTemporaryPeakBytes": 150 * gib,
            "durablePromotionBytes": 66 * gib,
            "runtimeBytes": 0,
            "evidenceBytes": 1 * gib,
            "reserveBytes": 20 * gib,
            "cleanupCandidates": ["/project/tma1/ndnsf-di/releases/old-runtime"],
        }
        decision = self.capacity.evaluate_capacity(base)
        self.assertTrue(decision["allowed"])
        self.assertEqual(decision["projectedDurableBytes"], 134 * gib)
        self.assertEqual(
            decision["cleanupInventory"],
            ["/project/tma1/ndnsf-di/releases/old-runtime"],
        )

        no_scratch = self.capacity.evaluate_capacity({
            **base,
            "scratchCapacityAuthority": "",
        })
        self.assertFalse(no_scratch["allowed"])
        self.assertIn("SCRATCH_AUTHORITY_UNVERIFIED", no_scratch["blockReasons"])

        no_quota = self.capacity.evaluate_capacity({
            **base,
            "quotaAuthority": "",
        })
        self.assertFalse(no_quota["allowed"])
        self.assertIn("DURABLE_QUOTA_UNVERIFIED", no_quota["blockReasons"])

    def test_smoke_analyzer_correlates_three_stages_and_two_dependencies(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            request_id = "smoke-prompt-m0-token-0"
            raw = {
                "promptId": "ndn-vs-ip",
                "status": "OK",
                "stopReason": "EOS",
                "exactReferenceMatch": True,
                "decodedText": "answer",
                "generatedTokenIds": [2],
                "tokenSteps": [{
                    "requestId": request_id,
                    "tokenEpoch": 0,
                    "status": "OK",
                }],
            }
            for rank in range(3):
                node = root / f"node-{rank}"
                node.mkdir()
                (node / "hostname.txt").write_text(
                    f"itiger{rank + 7:02d}\n", encoding="utf-8")
                (node / "gpu.csv").write_text(
                    f"GPU-{rank}, NVIDIA RTX 5000 Ada Generation, 1, 32768 MiB\n",
                    encoding="utf-8",
                )
            (root / "node-0/generation-raw.jsonl").write_text(
                json.dumps(raw) + "\n", encoding="utf-8")
            digest_a = "a" * 64
            digest_b = "b" * 64
            digest_c = "c" * 64
            timing_rows = [
                (
                    f"role=/LLM/Pipeline/Stage/0 stage=0 requestId={request_id} "
                    f"device=cuda:0 cpuFallback=0 input_sha256={digest_c} "
                    f"output_sha256={digest_a} dataName=/dependency/0"
                ),
                (
                    f"role=/LLM/Pipeline/Stage/1 stage=1 requestId={request_id} "
                    f"device=cuda:0 cpuFallback=0 input_sha256={digest_a} "
                    f"output_sha256={digest_b} dataName=/dependency/1"
                ),
                (
                    f"role=/LLM/Pipeline/Stage/2 stage=2 requestId={request_id} "
                    f"device=cuda:0 cpuFallback=0 input_sha256={digest_b} "
                    f"output_sha256={digest_c} dataName=-"
                ),
            ]
            for rank, timing in enumerate(timing_rows):
                fetch = ""
                if rank == 1:
                    fetch = (
                        "\nNDNSF_COLLAB_LARGE_FETCH_TIMING event=complete "
                        "dataName=/dependency/0 encoded_bytes=100 "
                        "received_segments=2 validated_segments=2 "
                        "received_wire_bytes=120"
                    )
                elif rank == 2:
                    fetch = (
                        "\nNDNSF_COLLAB_LARGE_FETCH_TIMING event=complete "
                        "dataName=/dependency/1 encoded_bytes=100 "
                        "received_segments=2 validated_segments=2 "
                        "received_wire_bytes=120"
                    )
                (root / f"node-{rank}/provider-{rank}.log").write_text(
                    "LLM_PIPELINE_PROVIDER_READY\n"
                    f"LLM_PIPELINE_QWEN_STAGE_TIMING {timing}{fetch}\n",
                    encoding="utf-8",
                )
            manifest = root / "stage-manifest.json"
            manifest.write_text(json.dumps({
                "layerRanges": [[0, 21], [21, 42], [42, 64]],
            }), encoding="utf-8")
            analysis = root / "analysis.json"
            enriched = root / "enriched.jsonl"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SMOKE_ANALYZER),
                    "--root", str(root),
                    "--stage-manifest", str(manifest),
                    "--output-json", str(analysis),
                    "--enriched-jsonl", str(enriched),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(analysis.read_text(encoding="utf-8"))
            self.assertEqual(result["stageReceiptCount"], 3)
            self.assertEqual(result["dependencyReceiptCount"], 2)
            enriched_row = json.loads(enriched.read_text(encoding="utf-8"))
            self.assertEqual(
                len(enriched_row["tokenSteps"][0]["stageReceipts"]), 3)


    def test_analyzer_keeps_25_measured_records_and_excludes_warmup(self) -> None:
        samples = []
        for prompt_index in range(5):
            prompt_id = f"prompt-{prompt_index}"
            samples.append({
                "promptId": prompt_id,
                "phase": "warmup",
                "status": "OK",
                "stopReason": "EOS",
                "exactReferenceMatch": True,
                "ttftMs": 10.0,
                "totalMs": 100.0,
                "tokensPerSecond": 10.0,
                "interTokenMs": [10.0],
            })
            for repetition in range(5):
                samples.append({
                    "promptId": prompt_id,
                    "phase": "measured",
                    "repetition": repetition,
                    "status": "OK",
                    "stopReason": "EOS",
                    "exactReferenceMatch": True,
                    "ttftMs": 11.0 + repetition,
                    "totalMs": 101.0 + repetition,
                    "tokensPerSecond": 9.0 + repetition,
                    "interTokenMs": [20.0 + repetition],
                })

        summary = self.analyzer.summarize_samples(samples)
        self.assertEqual(summary["rawRecordCount"], 30)
        self.assertEqual(summary["measuredRecordCount"], 25)
        self.assertEqual(summary["successfulMeasuredCount"], 25)
        self.assertEqual(len(summary["perPrompt"]), 5)
        self.assertEqual(summary["pooled"]["sampleCount"], 25)
        self.assertEqual(summary["pooled"]["classification"], "descriptive")
        self.assertTrue(summary["acceptanceReady"])
        self.assertNotIn("p99Ms", json.dumps(summary))

        samples[-1]["status"] = "TRUNCATED"
        samples[-1]["stopReason"] = "TOKEN_LIMIT"
        samples[-1]["exactReferenceMatch"] = False
        summary = self.analyzer.summarize_samples(samples)
        self.assertEqual(summary["measuredRecordCount"], 25)
        self.assertEqual(summary["successfulMeasuredCount"], 24)
        self.assertEqual(summary["failureCounts"], {"TRUNCATED:TOKEN_LIMIT": 1})
        self.assertFalse(summary["acceptanceReady"])

    def test_analyzer_cli_preserves_raw_rows(self) -> None:
        rows = [{
            "promptId": "prompt-0",
            "phase": "measured",
            "status": "FAILED",
            "stopReason": "REQUEST_FAILURE",
            "exactReferenceMatch": False,
            "ttftMs": 0.0,
            "totalMs": 5.0,
            "tokensPerSecond": 0.0,
            "interTokenMs": [],
        }]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            input_path = root / "samples.jsonl"
            output_path = root / "summary.json"
            input_path.write_text(
                "".join(json.dumps(row) + "\n" for row in rows),
                encoding="utf-8",
            )
            rc = self.analyzer.main([
                "--input-jsonl", str(input_path),
                "--output-json", str(output_path),
            ])
            self.assertEqual(rc, 0)
            summary = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["rawRecordCount"], 1)
            self.assertEqual(summary["successfulMeasuredCount"], 0)


if __name__ == "__main__":
    unittest.main()
