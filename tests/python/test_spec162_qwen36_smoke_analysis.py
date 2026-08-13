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
    / "specs/162-itiger-qwen36-generation/jobs/analyze-generation-smoke.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "spec162_smoke_analysis", ANALYZER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Qwen36SmokeAnalysisTest(unittest.TestCase):
    def test_marker_parser_handles_interleaved_key_boundaries(self) -> None:
        module = load_module()
        rows = module.marker_fields(
            "prefix LLM_PIPELINE_QWEN_SELECTION_PREPARE "
            "requestId=/submission-token-5reason=DI_SELECTION_DATAFLOW_V2_READY "
            "cacheHit=true diskCacheHit=true fetch_ms=0.00",
            "LLM_PIPELINE_QWEN_SELECTION_PREPARE",
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["requestId"], "/submission-token-5")
        self.assertEqual(rows[0]["reason"], "DI_SELECTION_DATAFLOW_V2_READY")
        self.assertEqual(rows[0]["cacheHit"], "true")

    def test_repo_cold_then_gpu_warm_requests_are_proven(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = root / "stage-manifest.json"
            manifest_path.write_text(json.dumps({
                "layerCount": 64,
                "layerRanges": [[0, 21], [21, 42], [42, 64]],
            }), encoding="utf-8")
            request_ids = ["smoke-token-0", "smoke-token-1"]
            row = {
                "status": "OK",
                "stopReason": "EOS",
                "exactReferenceMatch": True,
                "promptId": "prompt-0",
                "generationId": "generation-0",
                "decodedText": "answer",
                "generatedTokenIds": [7, 8],
                "tokenSteps": [
                    {"requestId": request_id, "actualTokenId": 7 + index}
                    for index, request_id in enumerate(request_ids)
                ],
            }
            for rank in range(3):
                node = root / f"node-{rank}"
                node.mkdir()
                (node / "hostname.txt").write_text(
                    f"itiger0{rank + 7}\n", encoding="utf-8")
                (node / "gpu.csv").write_text(
                    f"GPU-{rank}, NVIDIA RTX 5000 Ada Generation, 555.1, 32760 MiB\n",
                    encoding="utf-8",
                )
            (root / "node-0/generation-raw.jsonl").write_text(
                json.dumps(row) + "\n", encoding="utf-8")
            (root / "node-0/user.log").write_text("".join(
                "LLM_PIPELINE_QWEN_REQUEST_PHASE_TIMING "
                f"requestId={request_id} "
                '{"ack_collect_ms":1.0,"selection_commit_ms":2.0}\n'
                for request_id in request_ids
            ) + (
                "SPEC162_REQUEST_GATE_OPEN "
                "requestId=smoke-submission epochMs=1000 mode=REQUEST_FIRST\n"
                "NDNSF_DI_AUTOPLANNING_REQUEST_SENT "
                "requestId=/smoke-token-0 mode=DEFERRED\n"
                "NDNSF_DI_AUTOPLANNING_ACK_CLOSED "
                "requestId=/smoke-token-0 ackCount=3\n"
                "NDNSF_DI_AUTOPLANNING_GRAPH_READY "
                "requestId=/smoke-token-0 after=ACK_CLOSED\n"
                "LLM_PIPELINE_QWEN_DEFERRED_SPLIT "
                "requestId=/smoke-token-0\n"
                "LLM_PIPELINE_QWEN_DEFERRED_REPO_PUBLISH_START "
                "requestId=/smoke-token-0\n"
                "LLM_PIPELINE_QWEN_DEFERRED_REPO_PUBLISH_DONE "
                "requestId=/smoke-token-0\n"
                "NDNSF_DI_AUTOPLANNING_ARTIFACTS_READY "
                "requestId=/smoke-token-0\n"
                "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED "
                "requestId=/smoke-token-0\n"
                "LLM_PIPELINE_GENERATION_FINAL_RESPONSE "
                "generationId=generation-0 status=OK tokenCount=2 "
                "responseSha256=0db52f4076c082518412afd3dd3576e2cb0c63703fd7fed5e23ade60efef31d9\n"
            ), encoding="utf-8")
            (root / "automatic-planning.json").write_text(json.dumps({
                "preSplitCatalog": {
                    "publicationState":
                    "REQUIRES_DISTRIBUTED_REPO_REGISTRATION",
                },
            }), encoding="utf-8")

            for rank in range(3):
                lines = ["LLM_PIPELINE_PROVIDER_READY\n"]
                for index, request_id in enumerate(request_ids):
                    lines.append(
                        "NDNSF_DI_ACK_DECISION "
                        f"requestId={request_id} attempt=1 status=true "
                        "reservationHeld=true "
                        "reason=DI_SELECTION_DATAFLOW_V2_READY\n"
                    )
                    input_digest = "1" * 64 if rank == 0 else (
                        "a" * 64 if rank == 1 else "b" * 64)
                    output_digest = (
                        "a" * 64 if rank == 0 else
                        "b" * 64 if rank == 1 else "c" * 64)
                    data_name = (
                        f"/activation/{request_id}/{rank}"
                        if rank < 2 else "-")
                    lines.append(
                        "LLM_PIPELINE_QWEN_STAGE_TIMING "
                        f"requestId={request_id} "
                        f"role=/LLM/Pipeline/Stage/{rank} stage={rank} "
                        "device=cuda:0 cpuFallback=false "
                        f"input_sha256={input_digest} "
                        f"output_sha256={output_digest} "
                        f"dataName={data_name}\n"
                    )
                    lines.append(
                        "LLM_PIPELINE_QWEN_SELECTION_PREPARE "
                        f"requestId={'/' + request_id} "
                        f"role=/LLM/Pipeline/Stage/{rank} "
                        f"cacheHit={'false' if index == 0 else 'true'} "
                        f"diskCacheHit={'false' if index == 0 else 'true'} "
                        f"fetch_ms={'250.0' if index == 0 else '0.0'} "
                        f"load_ms={'100.0' if index == 0 else '0.0'} "
                        "device=cuda:0 cpuFallback=false\n"
                    )
                    lines.append(
                        "LLM_PIPELINE_QWEN_STAGE_EXECUTION_READY "
                        f"requestId={request_id} role=/LLM/Pipeline/Stage/{rank}\n"
                    )
                    if rank > 0:
                        lines.extend([
                            "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_WAIT "
                            f"requestId={request_id}\n",
                            "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_READY "
                            f"requestId={request_id}\n",
                        ])
                    if index == 0:
                        lines.extend([
                            "LLM_PIPELINE_QWEN_REPO_FETCH_PROGRESS "
                            f"requestId=/{request_id} sequence=1 "
                            "receivedBytes=100 verifiedBytes=100 totalBytes=100 "
                            "lastSegment=0 deliveredSegments=1 totalSegments=1 "
                            "retransmittedBytes=0 elapsedMs=1.0\n",
                            "LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE "
                            f"requestId=/{request_id} role=/LLM/Pipeline/Stage/{rank} "
                            "bytes=100 deliveredSegments=1 totalSegments=1\n",
                            "LLM_PIPELINE_QWEN_REPO_FETCH "
                            f"requestId=/{request_id} objectName=/artifact/{rank} "
                            "bytes=100 deliveredSegments=1 totalSegments=1 "
                            "lastSegment=0\n",
                        ])
                    if rank < 2:
                        lines.append(
                            "LLM_PIPELINE_QWEN_STAGE_OUTPUT "
                            f"requestId={request_id}\n"
                        )
                    else:
                        lines.append(
                            "LLM_PIPELINE_QWEN_FINAL_RESPONSE_PUBLISHED "
                            f"requestId={request_id}\n"
                        )
                    lines.append(
                        "NDNSF_DI_SELECTION_RESERVATION_RELEASED "
                        f"requestId={request_id} attempt=1 "
                        f"role=/LLM/Pipeline/Stage/{rank} "
                        "reason=RESPONSE_PUBLISHED\n"
                    )
                    if rank > 0:
                        lines.append(
                            "NDNSF_COLLAB_LARGE_FETCH_TIMING event=complete "
                            f"dataName=/activation/{request_id}/{rank - 1} "
                            "encoded_bytes=128 received_segments=1 "
                            "validated_segments=1 received_wire_bytes=192\n"
                        )
                (root / f"node-{rank}/provider-{rank}.log").write_text(
                    "".join(lines), encoding="utf-8")
                (root / f"node-{rank}/provider-markers-{rank}.log").write_text(
                    "".join(lines), encoding="utf-8")

            output = root / "analysis.json"
            enriched = root / "enriched.jsonl"
            argv = [
                str(ANALYZER),
                "--root", str(root),
                "--stage-manifest", str(manifest_path),
                "--output-json", str(output),
                "--enriched-jsonl", str(enriched),
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(module.main(), 0)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(
                result["warmPath"]["repeatedRepoFetchCount"], 0)
            self.assertEqual(
                result["reservationReleaseCountByRank"], [2, 2, 2])
            self.assertEqual(
                [
                    item["releaseBeforeNextAckCount"]
                    for item in result["ackReleaseOrderingByRank"]
                ],
                [None, None, None],
            )
            self.assertEqual(
                result["timingSemantics"]["requestUnit"].split(";", 1)[0],
                "one token-level NDNSF collaboration",
            )

    def test_overlapping_ack_before_release_is_allowed(self) -> None:
        module = load_module()
        text = "\n".join([
            "NDNSF_DI_ACK_DECISION requestId=r0 attempt=1 status=true "
            "reservationHeld=true reason=ready",
            "NDNSF_DI_ACK_DECISION requestId=r1 attempt=1 status=true "
            "reservationHeld=true reason=ready",
            "NDNSF_DI_SELECTION_RESERVATION_RELEASED "
            "requestId=r0 attempt=1 role=/stage/0 reason=done",
            "NDNSF_DI_SELECTION_RESERVATION_RELEASED "
            "requestId=r1 attempt=1 role=/stage/0 reason=done",
        ])
        result = module.ack_release_order(text, ["r0", "r1"], rank=0)
        self.assertEqual(result["ackTrueCount"], 2)
        self.assertIsNone(result["releaseBeforeNextAckCount"])


if __name__ == "__main__":
    unittest.main()
