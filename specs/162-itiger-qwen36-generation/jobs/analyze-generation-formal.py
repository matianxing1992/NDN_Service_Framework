#!/usr/bin/env python3
"""Validate and summarize the preregistered Spec 162 cold/warm campaign."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from pathlib import Path
from typing import Any


REQUIRED_REQUEST_PHASES = {
    "adapter_graph_split_ms",
    "request_encode_ms",
    "request_publish_ms",
    "ack_collect_ms",
    "placement_strategy_ms",
    "artifact_resolve_publish_ms",
    "plan_seal_ms",
    "selection_commit_ms",
    "pre_response_setup_total_ms",
    "application_input_encode_ms",
    "client_request_ms",
    "response_wait_decode_ms",
    "token_step_total_ms",
}

PROVIDER_STAGE_TIMING_KEYS = (
    "prefetch_submit_ms",
    "input_wait_ms",
    "input_reference_fetch_ms",
    "ref_wait_ms",
    "fetch_ms",
    "decode_ms",
    "serialize_ms",
    "compute_ms",
    "model_load_ms",
    "artificial_delay_ms",
    "runner_total_ms",
    "publish_ms",
    "total_ms",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def percentile(values: list[float], fraction: float) -> float:
    require(values, "cannot summarize an empty distribution")
    ordered = sorted(float(value) for value in values)
    rank = (len(ordered) - 1) * fraction
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def distribution(values: list[float]) -> dict[str, float | int]:
    require(values, "cannot summarize an empty distribution")
    return {
        "count": len(values),
        "min": min(values),
        "p50": percentile(values, 0.50),
        "mean": statistics.fmean(values),
        "p95Descriptive": percentile(values, 0.95),
        "max": max(values),
    }


def marker_fields(text: str, marker: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in text.splitlines():
        if marker not in line:
            continue
        tail = line.split(marker, 1)[1].strip()
        rows.append(dict(
            item.split("=", 1) for item in tail.split() if "=" in item
        ))
    return rows


def phase_timings(text: str) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    pattern = re.compile(
        r"LLM_PIPELINE_QWEN_REQUEST_PHASE_TIMING\s+requestId=(\S+)\s+(\{.*\})")
    for line in text.splitlines():
        match = pattern.search(line)
        if match is None:
            continue
        request_id = match.group(1)
        values = json.loads(match.group(2))
        require(request_id not in rows, f"duplicate phase timing: {request_id}")
        rows[request_id] = {
            str(key): float(value) for key, value in values.items()
        }
    return rows


def ack_release_order(
    text: str,
    request_ids: list[str],
    *,
    rank: int,
) -> dict[str, int]:
    def canonical_request_id(value: str) -> str:
        return value.lstrip("/")

    def parse_fields(tail: str) -> dict[str, str]:
        # Concurrent diagnostics may concatenate fields or split one marker
        # across physical lines.  Parse the known contract keys over the
        # marker-bounded segment instead of requiring whitespace delimiters.
        keys = (
            "requestId", "attempt", "status", "reservationHeld", "reason",
            "role", "stage", "isFinal", "prefetch_submit_ms", "total_ms",
        )
        matches = list(re.finditer(
            r"(" + "|".join(keys) + r")=", tail))
        return {
            match.group(1): tail[match.end():(
                matches[index + 1].start()
                if index + 1 < len(matches) else len(tail)
            )].replace("\n", " ").strip()
            for index, match in enumerate(matches)
        }

    events: list[tuple[int, str, dict[str, str]]] = []
    marker_pattern = re.compile(
        r"(NDNSF_DI_ACK_DECISION|LLM_PIPELINE_QWEN_STAGE_TIMING|"
        r"NDNSF_DI_SELECTION_RESERVATION_RELEASED)")
    markers = list(marker_pattern.finditer(text))
    for index, marker_match in enumerate(markers):
        marker = marker_match.group(1)
        kind = {
            "NDNSF_DI_ACK_DECISION": "ack",
            "LLM_PIPELINE_QWEN_STAGE_TIMING": "stage",
            "NDNSF_DI_SELECTION_RESERVATION_RELEASED": "release",
        }[marker]
        end = (markers[index + 1].start()
               if index + 1 < len(markers) else len(text))
        fields = parse_fields(text[marker_match.end():end])
        line_number = text.count("\n", 0, marker_match.start()) + 1
        events.append((line_number, kind, fields))

    positions: dict[tuple[str, str], list[int]] = {}
    for line_number, kind, fields in events:
        request_id = canonical_request_id(fields.get("requestId", ""))
        if request_id in request_ids:
            positions.setdefault((request_id, kind), []).append(line_number)
        if kind == "ack" and request_id in request_ids:
            # ACK=true is admission evidence.  It does not reserve an
            # exclusive model/GPU resource; Repo work and stage execution
            # may be queued or overlap across independent requests.
            if fields.get("status", "").lower() != "true":
                positions[(request_id, kind)].pop()

    for index, request_id in enumerate(request_ids):
        ack_lines = positions.get((request_id, "ack"), [])
        stage_lines = positions.get((request_id, "stage"), [])
        release_lines = positions.get((request_id, "release"), [])
        require(
            len(ack_lines) >= 1,
            f"rank {rank} ACK coverage mismatch for {request_id}",
        )
        require(
            len(stage_lines) == 1,
            f"rank {rank} stage timing coverage mismatch for {request_id}",
        )
        require(
            len(release_lines) == 1,
            f"rank {rank} release coverage mismatch for {request_id}",
        )
        require(
            ack_lines[0] < stage_lines[0] < release_lines[0],
            f"rank {rank} ACK/stage/release order invalid for {request_id}",
        )
    return {
        "ackTrueCount": len(request_ids),
        "stageBeforeReleaseCount": len(request_ids),
        "reservationReleaseCount": len(request_ids),
        "releaseBeforeNextAckCount": None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--campaign", required=True)
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args()

    root = Path(args.root)
    campaign = json.loads(Path(args.campaign).read_text(encoding="utf-8"))
    require(
        campaign.get("schemaVersion") == "ndnsf-di-qwen-generation-campaign-v1",
        "unsupported campaign schema",
    )
    prompts = list(campaign.get("prompts", []))
    repetitions = dict(campaign.get("repetitions", {}))
    require(len(prompts) == 5, f"formal campaign requires 5 prompts, got {len(prompts)}")
    require(repetitions.get("warmupPerPrompt") == 1, "expected one warmup per prompt")
    require(repetitions.get("measuredPerPrompt") == 5, "expected five measured per prompt")
    require(repetitions.get("sequential") is True, "campaign must be sequential")

    rows = [
        json.loads(line)
        for line in (root / "node-0/generation-raw.jsonl")
        .read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    require(len(rows) == 30, f"expected 30 generation rows, got {len(rows)}")
    prompt_ids = {str(item["promptId"]) for item in prompts}
    request_ids: list[str] = []
    for row in rows:
        require(row.get("status") == "OK", f"failed generation: {row.get('generationId')}")
        require(row.get("stopReason") == "EOS", "formal generation did not stop at EOS")
        require(row.get("exactReferenceMatch") is True, "reference mismatch")
        require(str(row.get("promptId")) in prompt_ids, "unexpected prompt")
        require(str(row.get("decodedText", "")).strip() != "", "empty decoded answer")
        steps = list(row.get("tokenSteps", []))
        require(steps, "generation has no token steps")
        request_ids.extend(str(step["requestId"]) for step in steps)
    require(len(request_ids) == len(set(request_ids)), "token request IDs are duplicated")

    user_log = (root / "node-0/user.log").read_text(
        encoding="utf-8", errors="replace")
    planning = phase_timings(user_log)
    require(
        set(planning) == set(request_ids),
        f"planning timing coverage {len(planning)} != {len(request_ids)}",
    )
    metric_keys = set(planning[request_ids[0]])
    require(
        REQUIRED_REQUEST_PHASES <= metric_keys,
        "request timing schema is missing required phases: "
        + ",".join(sorted(REQUIRED_REQUEST_PHASES - metric_keys)),
    )
    for request_id in request_ids:
        require(
            set(planning[request_id]) == metric_keys,
            f"request timing schema differs for {request_id}",
        )
        require(
            all(
                math.isfinite(value) and value >= 0.0
                for value in planning[request_id].values()
            ),
            f"request timing contains invalid values for {request_id}",
        )

    releases_by_rank: list[list[dict[str, str]]] = []
    preparation_by_rank: list[list[dict[str, str]]] = []
    residency_by_rank: list[list[dict[str, str]]] = []
    stage_timings_by_rank: list[list[dict[str, str]]] = []
    ack_release_order_by_rank: list[dict[str, int]] = []
    for rank in range(3):
        log = (root / f"node-{rank}/provider-{rank}.log").read_text(
            encoding="utf-8", errors="replace")
        releases = marker_fields(log, "NDNSF_DI_SELECTION_RESERVATION_RELEASED")
        prepares = marker_fields(log, "LLM_PIPELINE_QWEN_SELECTION_PREPARE")
        residency = marker_fields(log, "LLM_PIPELINE_QWEN_MODEL_RESIDENCY")
        stage_timings = marker_fields(log, "LLM_PIPELINE_QWEN_STAGE_TIMING")
        require(
            {item.get("requestId") for item in releases} == set(request_ids),
            f"rank {rank} reservation release coverage mismatch",
        )
        require(len(prepares) == len(request_ids), f"rank {rank} prepare count mismatch")
        require(len(residency) == len(request_ids), f"rank {rank} residency count mismatch")
        require(
            len(stage_timings) == len(request_ids),
            f"rank {rank} stage timing count mismatch",
        )
        stage_by_request = {
            item.get("requestId", ""): item for item in stage_timings
        }
        require(
            len(stage_by_request) == len(stage_timings)
            and set(stage_by_request) == set(request_ids),
            f"rank {rank} stage timing request coverage mismatch",
        )
        stage_timings = [stage_by_request[request_id] for request_id in request_ids]
        for request_id, item in zip(request_ids, stage_timings):
            missing = [
                key for key in PROVIDER_STAGE_TIMING_KEYS if key not in item
            ]
            require(
                not missing,
                f"rank {rank} stage timing missing {','.join(missing)} "
                f"for {request_id}",
            )
            values = [float(item[key]) for key in PROVIDER_STAGE_TIMING_KEYS]
            require(
                all(math.isfinite(value) and value >= 0.0 for value in values),
                f"rank {rank} stage timing has invalid values for {request_id}",
            )
            require(
                item.get("device", "").startswith("cuda")
                and item.get("cpuFallback", "").lower() in {"0", "false"},
                f"rank {rank} stage timing was not CUDA-only for {request_id}",
            )
            require(
                item.get("model_cache_hit", "").lower() in {"1", "true"},
                f"rank {rank} stage execution missed prepared GPU cache "
                f"for {request_id}",
            )
            require(
                float(item["artificial_delay_ms"]) == 0.0,
                f"rank {rank} stage used artificial delay for {request_id}",
            )
        require(
            prepares[0].get("cacheHit", "").lower() == "false",
            f"rank {rank} first request was not cold",
        )
        require(
            prepares[0].get("diskCacheHit", "").lower() == "false"
            and float(prepares[0].get("fetch_ms", "0")) > 0.0,
            f"rank {rank} first request did not fetch its stage from DistributedRepo",
        )
        require(
            all(item.get("cacheHit", "").lower() == "true" for item in prepares[1:]),
            f"rank {rank} warm preparation had a cache miss",
        )
        require(
            all(float(item.get("fetch_ms", "0")) == 0.0
                for item in prepares[1:]),
            f"rank {rank} warm preparation repeated a Repo fetch",
        )
        require(
            all(item.get("device", "").startswith("cuda") for item in prepares),
            f"rank {rank} preparation was not CUDA-resident",
        )
        require(
            all(item.get("cpuFallback", "").lower() in {"0", "false"}
                for item in prepares),
            f"rank {rank} preparation used CPU fallback",
        )
        require(
            all(item.get("cacheHit", "").lower() == "true" for item in residency),
            f"rank {rank} execution did not reuse the prepared GPU model",
        )
        require(
            all(item.get("device", "").startswith("cuda") for item in residency),
            f"rank {rank} execution was not CUDA-resident",
        )
        require(
            all(item.get("cpuFallback", "").lower() in {"0", "false"}
                for item in residency),
            f"rank {rank} execution used CPU fallback",
        )
        releases_by_rank.append(releases)
        preparation_by_rank.append(prepares)
        residency_by_rank.append(residency)
        stage_timings_by_rank.append(stage_timings)
        ack_release_order_by_rank.append(
            ack_release_order(log, request_ids, rank=rank)
        )

    measured = [row for row in rows if row.get("phase") == "measured"]
    warmups = [row for row in rows if row.get("phase") == "warmup"]
    require(len(warmups) == 5 and len(measured) == 25, "phase row count mismatch")

    metric_keys = sorted(metric_keys)
    first_request = request_ids[0]
    warm_request_ids = [item for item in request_ids if item != first_request]
    first_generation = rows[0]
    require(
        first_generation.get("phase") == "warmup"
        and int(first_generation.get("repetition", -1)) == 0
        and str(first_generation.get("promptId")) == str(prompts[0]["promptId"]),
        "the first complete generation is not the preregistered cold warmup",
    )
    post_cold_generations = rows[1:]
    require(
        len(post_cold_generations) == 29,
        "expected 29 complete generations after the cold generation",
    )
    phase_summary: dict[str, Any] = {}
    for key in metric_keys:
        cold_value = planning[first_request].get(key)
        warm_values = [
            planning[item][key] for item in warm_request_ids
        ]
        require(
            cold_value is not None
            and len(warm_values) == len(warm_request_ids),
            f"incomplete phase metric: {key}",
        )
        phase_summary[key] = {
            "coldFirstRequestMs": cold_value,
            "warmRequestsMs": distribution(warm_values),
        }

    output = {
        "schemaVersion": "ndnsf-di-qwen36-generation-formal-analysis-v3",
        "status": "PASS",
        "campaignId": campaign["campaignId"],
        "generationCounts": {
            "total": len(rows),
            "warmup": len(warmups),
            "measured": len(measured),
        },
        "tokenRequestCount": len(request_ids),
        "coldPath": {
            "firstTokenRequestId": first_request,
            "providerFetchMs": [
                float(items[0].get("fetch_ms", "0"))
                for items in preparation_by_rank
            ],
            "providerPrepareMs": [
                float(items[0].get("loadMs", items[0].get("load_ms", "0")))
                for items in preparation_by_rank
            ],
            "providerStagePhasesMsByRank": [
                {
                    key: float(items[0][key])
                    for key in PROVIDER_STAGE_TIMING_KEYS
                }
                for items in stage_timings_by_rank
            ],
            "requestPhasesMs": planning[first_request],
            "firstCompleteGeneration": {
                "generationId": first_generation["generationId"],
                "promptId": first_generation["promptId"],
                "phase": first_generation["phase"],
                "repetition": first_generation["repetition"],
                "tokenRequestCount": len(first_generation["tokenSteps"]),
                "generatedTokenCount": len(
                    first_generation["generatedTokenIds"]),
                "ttftMs": float(first_generation["ttftMs"]),
                "interTokenLatencyMs": [
                    float(value)
                    for value in first_generation.get("interTokenMs", [])
                ],
                "totalLatencyMs": float(first_generation["totalMs"]),
                "tokensPerSecond": float(
                    first_generation["tokensPerSecond"]),
            },
        },
        "warmPath": {
            "requestPhaseDistributionsMs": phase_summary,
            "postColdTokenRequestCount": len(warm_request_ids),
            "providerPreparationDistributionsByRank": [
                {
                    "rank": rank,
                    "repoFetchMs": distribution([
                        float(item.get("fetch_ms", "0"))
                        for item in preparation_by_rank[rank][1:]
                    ]),
                    "gpuLoadMs": distribution([
                        float(item.get("load_ms", "0"))
                        for item in preparation_by_rank[rank][1:]
                    ]),
                }
                for rank in range(3)
            ],
            "providerStagePhaseDistributionsByRank": [
                {
                    "rank": rank,
                    "phasesMs": {
                        key: distribution([
                            float(item[key])
                            for item in stage_timings_by_rank[rank][1:]
                        ])
                        for key in PROVIDER_STAGE_TIMING_KEYS
                    },
                }
                for rank in range(3)
            ],
            "postColdCompleteGenerationCount": len(post_cold_generations),
            "measuredCompleteGenerationCount": len(measured),
            "ttftMs": distribution([float(row["ttftMs"]) for row in measured]),
            "interTokenLatencyMs": distribution([
                float(value)
                for row in measured
                for value in row.get("interTokenMs", [])
            ]),
            "totalLatencyMs": distribution([float(row["totalMs"]) for row in measured]),
            "tokensPerSecond": distribution([
                float(row["tokensPerSecond"]) for row in measured
            ]),
        },
        "correctness": {
            "exactReferenceMatches": len(rows),
            "nonEmptyAnswers": len(rows),
            "cpuFallbackCount": 0,
        },
        "reservationReleaseCountByRank": [
            len(items) for items in releases_by_rank
        ],
        "ackReleaseOrderingByRank": ack_release_order_by_rank,
        "generationSequence": {
            "sequential": True,
            "count": len(rows),
            "generationIds": [str(row["generationId"]) for row in rows],
        },
        "timingSemantics": {
            "applicationUnit": (
                "one complete decoded generation row; 30 sequential rows "
                "equal 5 prompts times (1 warmup plus 5 measured)"
            ),
            "requestUnit": (
                "one token-level NDNSF collaboration; each complete "
                "generation contains one or more sequential token requests"
            ),
            "phaseTotalsOverlap": True,
            "releaseInvariant": (
                "on each Provider, every admitted request has a true ACK "
                "before its own stage timing and role release; independent "
                "requests may overlap"
            ),
        },
        "statisticalBoundary": (
            "p95Descriptive is a descriptive order statistic over 25 measured "
            "generations, not a tail-latency guarantee"
        ),
    }
    Path(args.output_json).write_text(
        json.dumps(output, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(output, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
