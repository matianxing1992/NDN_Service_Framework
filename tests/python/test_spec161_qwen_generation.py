from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_LIB = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py"
)
CAPACITY = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/capacity-preflight.py"
)
ANALYZER = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/analyze-generation.py"
)
SCRATCH_PROBE = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/scratch-capacity-probe.sbatch"
)
PREPARE_REFERENCE = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/prepare-reference.sbatch"
)
PREPARE_STAGES = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/prepare-stages.sbatch"
)
PREPARE_QWEN = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/prepare-qwen32b.py"
)
GENERATION_SMOKE = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/generation-smoke.sbatch"
)
GENERATION_RANK = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/generation-rank.sh"
)
GENERATION_RANK_INNER = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/generation-rank-inner.sh"
)
SMOKE_ANALYZER = (
    ROOT
    / "specs/161-itiger-qwen32b-generation/jobs/analyze-generation-smoke.py"
)
USER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py"
)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class FakeClock:
    def __init__(self) -> None:
        self.value = 100.0

    def __call__(self) -> float:
        return self.value

    def advance_ms(self, milliseconds: float) -> None:
        self.value += milliseconds / 1000.0


class QwenGenerationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.pipeline = load_module("spec161_llm_pipeline_lib", PIPELINE_LIB)
        cls.capacity = load_module("spec161_capacity_preflight", CAPACITY)
        cls.analyzer = load_module("spec161_generation_analyzer", ANALYZER)
        cls.prepare = load_module("spec161_prepare_qwen32b", PREPARE_QWEN)
        sys.path.insert(0, str(USER.parent))
        cls.user = load_module("spec161_llm_pipeline_user", USER)

    def test_generation_appends_tokens_and_stops_on_eos(self) -> None:
        clock = FakeClock()
        contexts = []
        tokens = iter([7, 8, 2])

        def step(context, token_epoch, request_id):
            contexts.append((tuple(context), token_epoch, request_id))
            clock.advance_ms(10 + token_epoch)
            return next(tokens)

        result = self.pipeline.run_bounded_qwen_generation(
            input_token_ids=[10, 11],
            max_new_tokens=64,
            eos_token_ids={2},
            generation_id="generation-1",
            token_step=step,
            expected_token_ids=[7, 8, 2],
            decode=lambda values: "decoded:" + ",".join(map(str, values)),
            clock=clock,
        )

        self.assertEqual(
            [item[0] for item in contexts],
            [(10, 11), (10, 11, 7), (10, 11, 7, 8)],
        )
        self.assertEqual(
            [item[2] for item in contexts],
            [
                "generation-1-token-0",
                "generation-1-token-1",
                "generation-1-token-2",
            ],
        )
        self.assertEqual(result.status, "OK")
        self.assertEqual(result.stop_reason, "EOS")
        self.assertEqual(result.generated_token_ids, (7, 8, 2))
        self.assertEqual(result.decoded_text, "decoded:7,8,2")
        self.assertTrue(result.exact_reference_match)
        self.assertEqual(len(result.token_steps), 3)
        self.assertAlmostEqual(result.ttft_ms, 10.0)
        self.assertEqual(len(result.inter_token_ms), 2)

    def test_token_limit_is_retained_as_truncated(self) -> None:
        result = self.pipeline.run_bounded_qwen_generation(
            input_token_ids=[10],
            max_new_tokens=64,
            eos_token_ids={2},
            generation_id="generation-limit",
            token_step=lambda _context, token_epoch, _request_id: 100 + token_epoch,
            expected_token_ids=[100 + index for index in range(64)],
            decode=lambda values: " ".join(map(str, values)),
        )
        self.assertEqual(result.status, "TRUNCATED")
        self.assertEqual(result.stop_reason, "TOKEN_LIMIT")
        self.assertEqual(len(result.generated_token_ids), 64)
        self.assertFalse(result.exact_reference_match)

    def test_frozen_max_token_reference_is_success_when_eos_is_optional(self) -> None:
        result = self.pipeline.run_bounded_qwen_generation(
            input_token_ids=[10],
            max_new_tokens=2,
            eos_token_ids={2},
            generation_id="generation-max-new-tokens",
            token_step=lambda _context, token_epoch, _request_id: 7 + token_epoch,
            expected_token_ids=[7, 8],
            require_eos=False,
            decode=lambda values: " ".join(map(str, values)),
        )
        self.assertEqual(result.status, "OK")
        self.assertEqual(result.stop_reason, "MAX_NEW_TOKENS")
        self.assertEqual(result.generated_token_ids, (7, 8))
        self.assertTrue(result.exact_reference_match)

    def test_mismatch_and_immediate_eos_fail_closed(self) -> None:
        mismatch = self.pipeline.run_bounded_qwen_generation(
            input_token_ids=[10],
            max_new_tokens=64,
            eos_token_ids={2},
            generation_id="generation-mismatch",
            token_step=lambda *_args: 9,
            expected_token_ids=[8, 2],
        )
        self.assertEqual(mismatch.status, "FAILED")
        self.assertEqual(mismatch.stop_reason, "TOKEN_MISMATCH")
        self.assertEqual(len(mismatch.token_steps), 1)

        empty = self.pipeline.run_bounded_qwen_generation(
            input_token_ids=[10],
            max_new_tokens=64,
            eos_token_ids={2},
            generation_id="generation-empty",
            token_step=lambda *_args: 2,
            expected_token_ids=[2],
            decode=lambda _values: "",
        )
        self.assertEqual(empty.status, "FAILED")
        self.assertEqual(empty.stop_reason, "EMPTY_OUTPUT")

        early_eos_tokens = iter([7, 2])
        early_eos = self.pipeline.run_bounded_qwen_generation(
            input_token_ids=[10],
            max_new_tokens=64,
            eos_token_ids={2},
            generation_id="generation-early-eos",
            token_step=lambda *_args: next(early_eos_tokens),
            expected_token_ids=[7, 2, 9],
            decode=lambda _values: "partial answer",
        )
        self.assertEqual(early_eos.status, "FAILED")
        self.assertEqual(early_eos.stop_reason, "TOKEN_MISMATCH")
        self.assertFalse(early_eos.exact_reference_match)

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

    def test_scratch_probe_is_bounded_and_performs_no_download(self) -> None:
        source = SCRATCH_PROBE.read_text(encoding="utf-8")
        self.assertIn("#SBATCH --time=00:05:00", source)
        self.assertIn('${SLURM_TMPDIR:-}', source)
        self.assertIn("/scratch /tmp", source)
        self.assertIn("count=64 conv=fsync", source)
        self.assertIn('"downloadPerformed": False', source)
        self.assertNotIn("huggingface-cli download", source)
        self.assertNotIn("snapshot_download", source)

    def test_t004_preparation_is_one_h100_scratch_job_and_promotes_no_source(
        self,
    ) -> None:
        reference = PREPARE_REFERENCE.read_text(encoding="utf-8")
        stages = PREPARE_STAGES.read_text(encoding="utf-8")
        prepare = PREPARE_QWEN.read_text(encoding="utf-8")

        self.assertIn("#SBATCH --gres=gpu:h100_80gb:1", reference)
        self.assertIn("#SBATCH --mem=256G", reference)
        self.assertIn('${SLURM_TMPDIR:-}', reference)
        self.assertIn("bash /source/jobs/prepare-stages.sbatch", reference)
        self.assertIn("scratchTemporaryPeak", (
            ROOT
            / "specs/161-itiger-qwen32b-generation/evidence/capacity-decision.json"
        ).read_text(encoding="utf-8"))
        self.assertIn('test ! -e "$artifact_partial/huggingface-cache"', reference)
        self.assertIn('"modelSourcePromoted": False', reference)
        self.assertNotIn("rm -rf", reference)
        self.assertIn("/source/jobs/prepare-qwen32b.py", stages)

        self.assertIn("snapshot_download", prepare)
        self.assertIn("trust_remote_code=False", prepare)
        self.assertIn("use_cache=False", prepare)
        self.assertIn("[(0, 21), (21, 42), (42, 64)]", prepare)
        self.assertEqual(
            self.prepare.normalize_eos_ids([151645, 151643, 151645]),
            [151643, 151645],
        )

    def test_t004_smoke_harness_waits_for_cuda_providers_and_has_no_retry(
        self,
    ) -> None:
        job = GENERATION_SMOKE.read_text(encoding="utf-8")
        rank = GENERATION_RANK.read_text(encoding="utf-8")
        inner = GENERATION_RANK_INNER.read_text(encoding="utf-8")
        self.assertIn("#SBATCH --nodes=3", job)
        self.assertIn("#SBATCH --gres=gpu:rtx_5000:1", job)
        self.assertIn("#SBATCH --mem=96G", job)
        self.assertIn("analyze-generation-smoke.py", job)
        self.assertNotIn("sbatch ", job)
        self.assertNotIn("rm -rf", rank)
        self.assertIn("LLM_PIPELINE_PROVIDER_READY", inner)
        self.assertIn("--require-cuda", inner)
        self.assertIn("--generation-campaign-manifest", inner)
        self.assertIn("--max-new-tokens 64", inner)

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

    def test_requester_submits_growing_context_for_each_token(self) -> None:
        contexts = []

        class Client:
            def distributed_inference(self, _service, payload, **_kwargs):
                document = self_outer.pipeline.decode_qwen_pipeline_context(payload)
                contexts.append(document["inputIds"][0])
                token = 7 if len(contexts) == 1 else 2
                response = {
                    "schema": "ndnsf-di-qwen-transformer-response-v1",
                    "topToken": token,
                    "stageCount": 3,
                    "layerRanges": [[0, 21], [21, 42], [42, 64]],
                }
                return types.SimpleNamespace(
                    status=True,
                    payload=json.dumps(response).encode("utf-8"),
                    error="",
                    request_id=_kwargs.get("request_id", ""),
                )

        self_outer = self
        args = types.SimpleNamespace(
            max_new_tokens=64,
            deployment_revision="sha256:test",
            ack_timeout_ms=100,
            timeout_ms=1000,
        )
        result = self.user._run_qwen_transformer_generation_sample(
            Client(),
            args,
            prompt_case={
                "formattedInputIds": [10, 11],
                "referenceGeneratedTokenIds": [7, 2],
                "eosTokenIds": [2],
            },
            generation_id="campaign-prompt-m0",
            decoder=lambda values: "answer" if list(values) == [7, 2] else "",
        )
        self.assertEqual(result.status, "OK")
        self.assertEqual(contexts, [[10, 11], [10, 11, 7]])
        self.assertEqual(
            result.token_steps[0]["transport"]["wireRequestId"],
            "campaign-prompt-m0-token-0",
        )

    def test_requester_campaign_writes_one_warmup_and_five_measured_per_prompt(
        self,
    ) -> None:
        class Client:
            def __init__(self):
                self.calls = 0

            def distributed_inference(self, _service, payload, **_kwargs):
                document = self_outer.pipeline.decode_qwen_pipeline_context(payload)
                self.calls += 1
                token_epoch = int(document["contextEpoch"])
                token = 7 if token_epoch == 0 else 2
                return types.SimpleNamespace(
                    status=True,
                    payload=json.dumps({
                        "schema": "ndnsf-di-qwen-transformer-response-v1",
                        "topToken": token,
                        "stageCount": 3,
                        "layerRanges": [[0, 21], [21, 42], [42, 64]],
                    }).encode("utf-8"),
                    error="",
                    request_id=_kwargs.get("request_id", ""),
                )

        class Tokenizer:
            @staticmethod
            def decode(values, skip_special_tokens=True):
                self_outer.assertTrue(skip_special_tokens)
                return "answer" if values == [7, 2] else ""

        class AutoTokenizer:
            @staticmethod
            def from_pretrained(*_args, **kwargs):
                self_outer.assertTrue(kwargs["local_files_only"])
                return Tokenizer()

        self_outer = self
        campaign = {
            "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
            "campaignId": "spec161-test",
            "generation": {"strategy": "greedy", "maxNewTokens": 64},
            "repetitions": {"warmupPerPrompt": 1, "measuredPerPrompt": 5},
            "prompts": [
                {
                    "promptId": f"prompt-{index}",
                    "formattedInputIds": [10, 11],
                    "referenceGeneratedTokenIds": [7, 2],
                    "eosTokenIds": [2],
                }
                for index in range(5)
            ],
        }
        client = Client()
        original_transformers = sys.modules.get("transformers")
        sys.modules["transformers"] = types.SimpleNamespace(
            AutoTokenizer=AutoTokenizer)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "samples.jsonl"
                args = types.SimpleNamespace(
                    max_new_tokens=64,
                    generation_jsonl=str(output),
                    qwen_tokenizer_dir="/frozen/tokenizer",
                    deployment_revision="sha256:test",
                    ack_timeout_ms=100,
                    timeout_ms=1000,
                )
                rc = self.user._run_qwen_transformer_generation_campaign(
                    client, args, campaign)
                rows = [
                    json.loads(line)
                    for line in output.read_text(encoding="utf-8").splitlines()
                ]
        finally:
            if original_transformers is None:
                sys.modules.pop("transformers", None)
            else:
                sys.modules["transformers"] = original_transformers

        self.assertEqual(rc, 0)
        self.assertEqual(client.calls, 60)
        self.assertEqual(len(rows), 30)
        self.assertEqual(sum(row["phase"] == "warmup" for row in rows), 5)
        self.assertEqual(sum(row["phase"] == "measured" for row in rows), 25)
        self.assertEqual(
            self.analyzer.summarize_samples(rows)["successfulMeasuredCount"],
            25,
        )

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
