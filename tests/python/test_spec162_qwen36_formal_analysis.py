#!/usr/bin/env python3

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
ANALYZER = (
    ROOT
    / "specs/162-itiger-qwen36-generation/jobs/analyze-generation-formal.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("spec162_formal_analysis", ANALYZER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Qwen36FormalAnalysisTest(unittest.TestCase):
    def test_cold_and_warm_campaign_is_correlated(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for rank in range(3):
                (root / f"node-{rank}").mkdir()
            prompts = [{"promptId": f"prompt-{index}"} for index in range(5)]
            campaign = {
                "schemaVersion": "ndnsf-di-qwen-generation-campaign-v1",
                "campaignId": "formal-fixture",
                "repetitions": {
                    "warmupPerPrompt": 1,
                    "measuredPerPrompt": 5,
                    "sequential": True,
                },
                "prompts": prompts,
            }
            campaign_path = root / "campaign.json"
            campaign_path.write_text(json.dumps(campaign), encoding="utf-8")

            rows = []
            request_ids = []
            for prompt in prompts:
                for phase, count in (("warmup", 1), ("measured", 5)):
                    for repetition in range(count):
                        generation_id = (
                            f"formal-fixture-{prompt['promptId']}-"
                            f"{phase}-{repetition}"
                        )
                        step_ids = [
                            f"{generation_id}-token-{token}" for token in range(2)
                        ]
                        request_ids.extend(step_ids)
                        rows.append({
                            "generationId": generation_id,
                            "promptId": prompt["promptId"],
                            "phase": phase,
                            "repetition": repetition,
                            "status": "OK",
                            "stopReason": "EOS",
                            "exactReferenceMatch": True,
                            "decodedText": "answer",
                            "generatedTokenIds": [7, 8],
                            "ttftMs": 20.0,
                            "interTokenMs": [10.0],
                            "totalMs": 30.0,
                            "tokensPerSecond": 66.0,
                            "tokenSteps": [
                                {"requestId": request_id} for request_id in step_ids
                            ],
                        })
            (root / "node-0/generation-raw.jsonl").write_text(
                "".join(json.dumps(row) + "\n" for row in rows),
                encoding="utf-8",
            )
            (root / "node-0/user.log").write_text(
                "".join(
                    "LLM_PIPELINE_QWEN_REQUEST_PHASE_TIMING "
                    f"requestId={request_id} "
                    + json.dumps({
                        "adapter_graph_split_ms": 0.1,
                        "request_encode_ms": 0.2,
                        "request_publish_ms": 0.3,
                        "ack_collect_ms": 1.0,
                        "placement_strategy_ms": 0.4,
                        "artifact_resolve_publish_ms": 0.5,
                        "plan_seal_ms": 0.6,
                        "selection_commit_ms": 0.7,
                        "pre_response_setup_total_ms": 2.0,
                        "application_input_encode_ms": 0.8,
                        "client_request_ms": 2.1,
                        "response_wait_decode_ms": 3.0,
                        "token_step_total_ms": 6.0,
                    }, sort_keys=True)
                    + "\n"
                    for request_id in request_ids
                ),
                encoding="utf-8",
            )
            for rank in range(3):
                lines = []
                for index, request_id in enumerate(request_ids):
                    lines.append(
                        "NDNSF_DI_ACK_DECISION "
                        f"requestId={request_id} attempt=1 status=true "
                        "reason=DI_SELECTION_DATAFLOW_V2_READY\n"
                    )
                    lines.append(
                        "LLM_PIPELINE_QWEN_SELECTION_PREPARE "
                        f"requestId={request_id} role=/LLM/Pipeline/Stage/{rank} "
                        f"cacheHit={'false' if index == 0 else 'true'} "
                        f"diskCacheHit={'false' if index == 0 else 'true'} "
                        f"fetch_ms={'250.0' if index == 0 else '0.0'} "
                        f"load_ms={'100.0' if index == 0 else '0.0'} "
                        "device=cuda:0 cpuFallback=false\n"
                    )
                    lines.append(
                        "LLM_PIPELINE_QWEN_MODEL_RESIDENCY "
                        f"role=/LLM/Pipeline/Stage/{rank} path=/stage/{rank} "
                        "cacheHit=true load_ms=0.0 device=cuda:0 cpuFallback=false\n"
                    )
                    lines.append(
                        "LLM_PIPELINE_QWEN_STAGE_TIMING "
                        f"requestId={request_id} role=/LLM/Pipeline/Stage/{rank} "
                        f"stage={rank} isFinal={1 if rank == 2 else 0} "
                        "prefetch_submit_ms=0.1 input_wait_ms=0.2 "
                        "input_reference_fetch_ms=0.3 ref_wait_ms=0.4 "
                        "fetch_ms=0.5 decode_ms=0.6 serialize_ms=0.7 "
                        "compute_ms=5.0 model_cache_hit=1 model_load_ms=0.0 "
                        "device=cuda:0 cpuFallback=0 artificial_delay_ms=0.0 "
                        "runner_total_ms=6.0 publish_ms=0.8 total_ms=7.0\n"
                    )
                    lines.append(
                        "NDNSF_DI_SELECTION_RESERVATION_RELEASED "
                        f"requestId={request_id} attempt=1 "
                        f"role=/LLM/Pipeline/Stage/{rank} "
                        "reason=RESPONSE_PUBLISHED\n"
                    )
                (root / f"node-{rank}/provider-{rank}.log").write_text(
                    "".join(lines), encoding="utf-8")

            output = root / "analysis.json"
            argv = [
                str(ANALYZER),
                "--root", str(root),
                "--campaign", str(campaign_path),
                "--output-json", str(output),
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(module.main(), 0)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["generationCounts"]["measured"], 25)
            self.assertEqual(result["tokenRequestCount"], 60)
            self.assertEqual(
                result["schemaVersion"],
                "ndnsf-di-qwen36-generation-formal-analysis-v3",
            )
            self.assertEqual(
                result["warmPath"]["totalLatencyMs"]["count"], 25)
            self.assertEqual(
                result["reservationReleaseCountByRank"], [60, 60, 60])
            self.assertEqual(
                result["coldPath"]["firstCompleteGeneration"][
                    "totalLatencyMs"
                ],
                30.0,
            )
            self.assertEqual(
                result["warmPath"]["postColdCompleteGenerationCount"], 29)
            self.assertEqual(
                result["warmPath"]["measuredCompleteGenerationCount"], 25)
            self.assertEqual(
                [
                    item["releaseBeforeNextAckCount"]
                    for item in result["ackReleaseOrderingByRank"]
                ],
                [None, None, None],
            )
            self.assertEqual(
                [
                    item["stageBeforeReleaseCount"]
                    for item in result["ackReleaseOrderingByRank"]
                ],
                [60, 60, 60],
            )
            self.assertEqual(
                result["warmPath"]["postColdTokenRequestCount"], 59)
            self.assertEqual(
                result["warmPath"][
                    "providerStagePhaseDistributionsByRank"
                ][0]["phasesMs"]["compute_ms"]["count"],
                59,
            )

            user_log_path = root / "node-0/user.log"
            original_user_log = user_log_path.read_text(encoding="utf-8")
            user_lines = original_user_log.splitlines()
            payload_offset = user_lines[0].index("{")
            incomplete = json.loads(user_lines[0][payload_offset:])
            incomplete.pop("token_step_total_ms")
            user_lines[0] = (
                user_lines[0][:payload_offset]
                + json.dumps(incomplete, sort_keys=True)
            )
            user_log_path.write_text(
                "\n".join(user_lines) + "\n", encoding="utf-8")
            with mock.patch.object(sys, "argv", argv):
                with self.assertRaisesRegex(
                        RuntimeError, "missing required phases"):
                    module.main()
            user_log_path.write_text(original_user_log, encoding="utf-8")

            provider_log_path = root / "node-0/provider-0.log"
            original_provider_log = provider_log_path.read_text(encoding="utf-8")
            provider_lines = original_provider_log.splitlines()
            stage_index = next(
                index for index, line in enumerate(provider_lines)
                if "LLM_PIPELINE_QWEN_STAGE_TIMING" in line
            )
            release_index = next(
                index for index, line in enumerate(provider_lines)
                if "NDNSF_DI_SELECTION_RESERVATION_RELEASED" in line
            )
            provider_lines[stage_index], provider_lines[release_index] = (
                provider_lines[release_index],
                provider_lines[stage_index],
            )
            provider_log_path.write_text(
                "\n".join(provider_lines) + "\n", encoding="utf-8")
            with mock.patch.object(sys, "argv", argv):
                with self.assertRaisesRegex(
                        RuntimeError, "ACK/stage/release order invalid"):
                    module.main()


if __name__ == "__main__":
    unittest.main()
