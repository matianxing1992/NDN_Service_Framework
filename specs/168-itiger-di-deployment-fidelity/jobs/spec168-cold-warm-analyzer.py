#!/usr/bin/env python3
"""Fail-closed analysis for the Spec 168 30-request cold/warm campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
from typing import Any, Iterable


DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")

# The only accepted non-exact generation is the pre-registered bf16 tie found
# by the immutable numeric-divergence diagnostic.  Keep this allow-list in the
# analyzer so a campaign cannot silently broaden the equivalence policy by
# editing its JSON workload.
NUMERIC_EQUIVALENCE = {
    "promptId": "stage-timeout",
    "classification": "NUMERICALLY_EQUIVALENT_DIVERGENCE",
    "referenceDigest": "sha256:f3a835be2de33dfe720513b5d725d4274ea97f7590b763b823f9d4e67f5dd7ca",
    "modelDigest": "sha256:a317ec50b9a20ebf83a96379016e227dbe83c0b7116e97cfffdfc0bcee4c86db",
    "stageManifestDigest": "sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5",
    "evidenceDigest": "sha256:bd304ae3a3f462342cdd29f523175122506d9fa5228b9d790974de1191b2fca7",
    "firstDivergence": {"tokenIndex": 4},
    "allowedTokenIds": [34859],
}


def require(value: object, code: str) -> None:
    if not value:
        raise RuntimeError(code)


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"SPEC168_JSON_OBJECT_REQUIRED:{path.name}")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 << 20), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def canonical_request_id(value: object) -> str:
    text = str(value or "").strip()
    return "/" + text.lstrip("/") if text else ""


def field(line: str, name: str) -> str:
    match = re.search(rf"(?:^|\s){re.escape(name)}=([^\s]+)", line)
    return match.group(1) if match else ""


def request_lines(text: str, marker: str, request_id: str) -> list[str]:
    expected = canonical_request_id(request_id)
    rows = []
    for line in text.splitlines():
        if marker not in line:
            continue
        if canonical_request_id(field(line, "requestId")) == expected:
            rows.append(line)
            continue
        # Concurrent native/debug writers can concatenate a marker-only
        # Selection line with the following DEBUG record.  In that case the
        # requestId field is absent, but the authenticated SELECTION
        # messageName still terminates in the exact request id.  Keep this
        # fallback narrow so malformed ACK/decision lines remain fail-closed.
        if (
            marker == "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED"
            and "messageName=" in line
            and expected in line
        ):
            rows.append(line)
    return rows


def request_epochs(text: str, marker: str, request_id: str) -> set[int]:
    values = set()
    for line in request_lines(text, marker, request_id):
        raw = field(line, "epoch")
        require(raw.isdigit(), f"SPEC168_DEPENDENCY_EPOCH_MISSING:{marker}")
        values.add(int(raw))
    return values


def request_marker_times(
    text: str, marker: str, request_id: str,
) -> list[float]:
    rows = sorted(
        request_lines(text, marker, request_id),
        key=lambda line: int(field(line, "epoch") or -1),
    )
    values = []
    for line in rows:
        raw = field(line, "monotonicMs")
        require(raw, f"SPEC168_TOKEN_CLOCK_MISSING:{marker}")
        values.append(float(raw))
    require(
        all(right >= left for left, right in zip(values, values[1:])),
        f"SPEC168_TOKEN_CLOCK_NON_MONOTONIC:{marker}",
    )
    return values


def snapshot(line: str) -> dict[str, Any]:
    marker = "snapshot="
    require(marker in line, "SPEC168_RESIDENCY_SNAPSHOT_MISSING")
    value = json.loads(line.split(marker, 1)[1])
    require(
        value.get("schema") == "ndnsf-di.provider-residency.v1",
        "SPEC168_RESIDENCY_SCHEMA_INVALID",
    )
    return value


def describe(values: Iterable[float]) -> dict[str, float | int]:
    sample = [float(item) for item in values]
    require(sample, "SPEC168_EMPTY_DISTRIBUTION")
    quartiles = statistics.quantiles(sample, n=4, method="inclusive") \
        if len(sample) > 1 else [sample[0], sample[0], sample[0]]
    return {
        "count": len(sample),
        "min": min(sample),
        "q1": quartiles[0],
        "median": statistics.median(sample),
        "q3": quartiles[2],
        "max": max(sample),
        "mean": statistics.fmean(sample),
    }


def _metadata(row: dict[str, Any]) -> dict[str, Any]:
    steps = list(row.get("tokenSteps", []))
    require(
        len(steps) == 1 and steps[0].get("mode") == "FULL",
        "SPEC168_DURABLE_INVOCATION_FRAGMENTED",
    )
    value = steps[0].get("metadata", {})
    require(isinstance(value, dict), "SPEC168_RESPONSE_METADATA_MISSING")
    return value


def analyze(args: argparse.Namespace) -> dict[str, Any]:
    root = Path(args.root)
    stage_path = Path(args.stage_manifest)
    campaign_path = Path(args.campaign_manifest)
    generation_path = Path(args.generation_campaign)
    campaign = read_json(campaign_path)
    bindings = campaign.get("bindingDigests", {})
    require(campaign.get("schema") == "ndnsf-di.spec168-campaign.v3",
            "SPEC168_CAMPAIGN_SCHEMA_INVALID")
    require(campaign.get("state") == "FROZEN", "SPEC168_CAMPAIGN_NOT_FROZEN")
    for key, observed in (
        ("sourceDigest", args.expected_source_digest),
        ("sourceBundleDigest", args.expected_source_bundle_digest),
        ("runtimeSifDigest", args.expected_sif_digest),
        ("remoteSmallStageManifestDigest", sha256(stage_path)),
    ):
        require(DIGEST.fullmatch(str(observed)), f"SPEC168_DIGEST_INVALID:{key}")
        require(bindings.get(key) == observed, f"SPEC168_BINDING_MISMATCH:{key}")

    generation = read_json(generation_path)
    require(
        generation.get("schemaVersion") ==
        "ndnsf-di-qwen-generation-campaign-v1",
        "SPEC168_GENERATION_CAMPAIGN_SCHEMA_INVALID",
    )
    prompts = list(generation.get("prompts", []))
    repetitions = generation.get("repetitions", {})
    require(len(prompts) == 5, "SPEC168_FIVE_PROMPTS_REQUIRED")
    require(
        repetitions.get("warmupPerPrompt") == 1
        and repetitions.get("measuredPerPrompt") == 5
        and repetitions.get("sequential") is True,
        "SPEC168_REPETITION_SCHEDULE_INVALID",
    )
    require(
        generation.get("generation", {}).get("maxNewTokens") == 64,
        "SPEC168_TOKEN_LIMIT_INVALID",
    )
    prompt_ids = [str(item.get("promptId", "")) for item in prompts]
    require(
        all(prompt_ids) and len(set(prompt_ids)) == 5,
        "SPEC168_PROMPT_IDENTITIES_INVALID",
    )
    for prompt in prompts:
        policy = prompt.get("numericEquivalence")
        if str(prompt.get("promptId")) == NUMERIC_EQUIVALENCE["promptId"]:
            require(policy == NUMERIC_EQUIVALENCE,
                    "SPEC168_NUMERIC_EQUIVALENCE_POLICY_INVALID")
        else:
            require(policy is None,
                    "SPEC168_UNREGISTERED_NUMERIC_EQUIVALENCE")

    stage_manifest = read_json(stage_path)
    require(
        len(stage_manifest.get("stages", [])) == 3
        and len(stage_manifest.get("layerRanges", [])) == 3,
        "SPEC168_STAGE_COVERAGE_INVALID",
    )
    model_digest = str(stage_manifest.get("modelDigest", ""))
    if not model_digest.startswith("sha256:"):
        model_digest = "sha256:" + model_digest
    require(DIGEST.fullmatch(model_digest), "SPEC168_MODEL_DIGEST_INVALID")
    workload_digest = str(bindings.get("promptSetDigest", ""))

    request_gate = read_json(root / "request-gate-open.json")
    require(
        request_gate.get("certificateMode") == "ON_DEMAND_NETWORK_FETCH",
        "SPEC168_REQUEST_GATE_CERTIFICATE_MODE_INVALID",
    )
    gate_epoch_ms = int(request_gate.get("epochMs", 0))
    require(gate_epoch_ms > 0, "SPEC168_REQUEST_GATE_EPOCH_INVALID")

    rows = [
        json.loads(line) for line in
        (root / "node-0/generation-raw.jsonl").read_text(
            encoding="utf-8").splitlines() if line.strip()
    ]
    require(len(rows) == 30, "SPEC168_THIRTY_INVOCATIONS_REQUIRED")
    expected_schedule = [
        (prompt_id, phase, repetition)
        for prompt_id in prompt_ids
        for phase, count in (("warmup", 1), ("measured", 5))
        for repetition in range(count)
    ]
    observed_schedule = [
        (str(row.get("promptId", "")), str(row.get("phase", "")),
         int(row.get("repetition", -1))) for row in rows
    ]
    require(observed_schedule == expected_schedule,
            "SPEC168_SCHEDULE_ORDER_INVALID")

    request_ids: list[str] = []
    retained_rows: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        require(row.get("status") == "OK", f"SPEC168_GENERATION_FAILED:{index}")
        exact = row.get("exactReferenceMatch") is True
        accepted_numeric = (
            row.get("referenceAcceptance") ==
            "NUMERICALLY_EQUIVALENT_DIVERGENCE"
            and row.get("referenceEvidenceDigest") ==
            NUMERIC_EQUIVALENCE["evidenceDigest"]
            and str(row.get("promptId")) == NUMERIC_EQUIVALENCE["promptId"]
        )
        require(exact or accepted_numeric,
                f"SPEC168_REFERENCE_MISMATCH:{index}")
        require(str(row.get("decodedText", "")).strip(),
                f"SPEC168_ANSWER_MISSING:{index}")
        tokens = list(row.get("generatedTokenIds", []))
        require(len(tokens) >= 2, f"SPEC168_MULTI_TOKEN_REQUIRED:{index}")
        metadata = _metadata(row)
        request_id = canonical_request_id(metadata.get("requestId"))
        expected_id = canonical_request_id(
            f"{args.request_prefix}--sample--{row['generationId']}")
        require(request_id == expected_id,
                f"SPEC168_RESPONSE_REQUEST_ID_MISMATCH:{index}")
        require(int(metadata.get("wireRequestCount", -1)) == 1,
                f"SPEC168_WIRE_REQUEST_COUNT_INVALID:{index}")
        require(int(metadata.get("tokenRequestCount", -1)) == 0,
                f"SPEC168_TOKEN_REQUEST_COUNT_INVALID:{index}")
        require(int(metadata.get("stageCount", -1)) == 3,
                f"SPEC168_STAGE_COUNT_INVALID:{index}")
        expected_cache = "GENERATED" if index == 0 else "REUSE_CACHED"
        require(metadata.get("cacheClass") == expected_cache,
                f"SPEC168_CACHE_CLASS_INVALID:{index}")
        timings = metadata.get("planningTimingsMs", {})
        require(
            isinstance(timings, dict)
            and all(float(value) >= 0.0 for value in timings.values())
            and "pre_response_setup_total_ms" in timings
            and "ack_collect_ms" in timings,
            f"SPEC168_PLANNING_TIMINGS_INVALID:{index}",
        )
        require(
            float(timings["ack_collect_ms"]) <= 5000.0,
            f"SPEC168_ACK_COVERAGE_CLOSURE_TOO_SLOW:{index}",
        )
        inter_token = [float(item) for item in row.get("interTokenMs", [])]
        require(
            len(inter_token) == len(tokens) - 1
            and all(value >= 0.0 for value in inter_token),
            f"SPEC168_INTER_TOKEN_LATENCY_INVALID:{index}",
        )
        require(float(row.get("ttftMs", -1)) >= 0.0,
                f"SPEC168_TTFT_INVALID:{index}")
        require(float(row.get("totalMs", -1)) > 0.0,
                f"SPEC168_TOTAL_LATENCY_INVALID:{index}")
        require(float(row.get("tokensPerSecond", -1)) > 0.0,
                f"SPEC168_THROUGHPUT_INVALID:{index}")
        require(row.get("modelIdentityDigest") == model_digest,
                f"SPEC168_MODEL_IDENTITY_MISMATCH:{index}")
        require(row.get("workloadDigest") == workload_digest,
                f"SPEC168_WORKLOAD_IDENTITY_MISMATCH:{index}")
        request_ids.append(request_id)
        retained_rows.append({
            **row,
            "requestId": request_id,
            "cacheClass": expected_cache,
            "planningTimingsMs": timings,
        })
    require(len(set(request_ids)) == 30, "SPEC168_REQUEST_IDS_NOT_UNIQUE")

    user_text = (root / "node-0/user.log").read_text(errors="replace")
    for index, request_id in enumerate(request_ids):
        for marker in (
            "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
            "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
            "NDNSF_DI_AUTOPLANNING_DECISION",
            "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
        ):
            require(len(request_lines(user_text, marker, request_id)) == 1,
                    f"SPEC168_REQUEST_ID_LINEAGE:{index}:{marker}")
        decision_line = request_lines(
            user_text, "NDNSF_DI_AUTOPLANNING_DECISION", request_id)[0]
        expected_cache = "GENERATED" if index == 0 else "REUSE_CACHED"
        require(field(decision_line, "preparation") == expected_cache,
                f"SPEC168_DECISION_CLASS_INVALID:{index}")
        expected_catalog = 0 if index == 0 else 1
        require(int(field(decision_line, "catalogCount") or -1)
                == expected_catalog,
                f"SPEC168_CATALOG_COUNT_INVALID:{index}")
    require("UserToken/ProviderToken runtime mode: enabled" in user_text,
            "SPEC168_USER_TOKEN_EVIDENCE_MISSING")
    require("Installed user permission" in user_text,
            "SPEC168_USER_PERMISSION_EVIDENCE_MISSING")
    require(
        "MessageValidator Data validation failed" not in user_text,
        "SPEC168_USER_DATA_VALIDATION_FAILED",
    )
    require(
        "MessageValidator Data validated through configured validator" in user_text,
        "SPEC168_USER_NETWORK_CERTIFICATE_VALIDATION_MISSING",
    )

    forbidden = (
        "CPUExecutionProvider", "cpuFallback=true", "LEGACY_READY_SET_V1",
        "ExecutionActivateMessage", "SPEC162_PROVIDER_SETTLE_SECONDS",
        "SPEC162_USER_STARTUP_SETTLE_MS",
    )
    require(not any(item in user_text for item in forbidden),
            "SPEC168_FORBIDDEN_USER_RUNTIME_MARKER")

    nodes: list[str] = []
    gpus: list[dict[str, Any]] = []
    providers: list[dict[str, Any]] = []
    rank2_inter_token: dict[str, list[float]] = {}
    for rank in range(3):
        node_root = root / f"node-{rank}"
        node = (node_root / "hostname.txt").read_text().strip()
        nodes.append(node)
        gpu_row = next(csv.reader([
            (node_root / "gpu.csv").read_text().splitlines()[0]]))
        require(len(gpu_row) >= 4 and "RTX 5000" in gpu_row[1],
                f"SPEC168_GPU_INVALID:rank={rank}")
        gpus.append({"rank": rank, "uuid": gpu_row[0].strip(),
                     "name": gpu_row[1].strip(), "node": node})
        text = (node_root / f"provider-{rank}.log").read_text(
            errors="replace")
        require(
            "MessageValidator Data validation failed" not in text,
            f"SPEC168_PROVIDER_DATA_VALIDATION_FAILED:rank={rank}",
        )
        require(
            "MessageValidator Data validated through configured validator" in text,
            f"SPEC168_PROVIDER_NETWORK_CERTIFICATE_VALIDATION_MISSING:rank={rank}",
        )
        marker_path = node_root / f"provider-markers-{rank}.log"
        marker_text = marker_path.read_text(errors="replace") \
            if marker_path.is_file() else text
        require("NAC_ABE_BOOTSTRAP" in text,
                f"SPEC168_NAC_ABE_EVIDENCE_MISSING:rank={rank}")
        require("Installed provider permission" in text,
                f"SPEC168_PROVIDER_PERMISSION_MISSING:rank={rank}")
        require("device=cuda:0" in text and "cpuFallback=false" in text,
                f"SPEC168_CUDA_EVIDENCE_MISSING:rank={rank}")
        require(not any(item in text for item in forbidden),
                f"SPEC168_FORBIDDEN_PROVIDER_MARKER:rank={rank}")
        # Provider stdout is a concurrent stream: an ACK decision and the
        # fetch-complete marker can be byte-interleaved onto one physical line
        # (including an ACK prefix).  Count the authoritative marker itself;
        # filtering by line prefix would discard valid fetches and classify a
        # completed campaign as a cardinality failure.
        fetch_lines = [
            line for line in text.splitlines()
            if "LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE" in line
        ]
        require(
            len(fetch_lines) == len(rows),
            f"SPEC168_REPO_FETCH_CARDINALITY_INVALID:rank={rank}",
        )
        ack_lines = [
            line for line in text.splitlines()
            if "NDNSF_DI_ACK_DECISION" in line
        ]
        # ACK print fields share stdout with pipeline marker writers.  A
        # single logical ACK may therefore be split across adjacent records,
        # and a fragment can carry the marker more than once.  The user-side
        # ACK_CLOSED lineage remains the exact per-request cardinality gate;
        # here require one marker stream covering every request, while
        # retaining any extra fragments as evidence rather than dropping them.
        require(
            len(ack_lines) >= len(rows)
            and all(request_id in text for request_id in request_ids),
            f"SPEC168_ACK_STREAM_COVERAGE_INVALID:rank={rank}",
        )

        previous_counters: dict[str, int] | None = None
        boot_epoch = ""
        for index, (request_id, row) in enumerate(zip(request_ids, rows)):
            release_lines = request_lines(
                marker_text, "LLM_PIPELINE_QWEN_RESIDENCY_RELEASE", request_id)
            require(len(release_lines) == 1,
                    f"SPEC168_RELEASE_CARDINALITY:rank={rank}:sample={index}")
            residency = snapshot(release_lines[0])
            current_boot = str(residency.get("providerBootEpoch", ""))
            require(current_boot, f"SPEC168_BOOT_EPOCH_MISSING:rank={rank}")
            if not boot_epoch:
                boot_epoch = current_boot
            require(current_boot == boot_epoch,
                    f"SPEC168_BOOT_EPOCH_CHANGED:rank={rank}:sample={index}")
            counters = {
                key: int(value)
                for key, value in residency.get("counters", {}).items()
            }
            require(counters.get("deviceLoadCount") == 1,
                    f"SPEC168_DEVICE_RELOAD:rank={rank}:sample={index}")
            require(counters.get("repoUniqueBytes", 0) > 0,
                    f"SPEC168_REPO_UNIQUE_BYTES_MISSING:rank={rank}")
            require(counters.get("repoWireBytes", 0) > 0,
                    f"SPEC168_REPO_WIRE_BYTES_MISSING:rank={rank}")
            require(counters.get("gpuHitCount") == index + 1,
                    f"SPEC168_GPU_HIT_SEQUENCE_INVALID:rank={rank}:sample={index}")
            if previous_counters is not None:
                for key in ("repoUniqueBytes", "repoWireBytes",
                            "deviceLoadCount"):
                    require(counters.get(key) == previous_counters.get(key),
                            f"SPEC168_WARM_COUNTER_GREW:{rank}:{index}:{key}")
            previous_counters = counters

            tokens = list(row.get("generatedTokenIds", []))
            expected_epochs = set(range(len(tokens)))
            if rank == 0:
                produced = request_epochs(
                    marker_text,
                    "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED", request_id)
                require(produced == expected_epochs,
                        f"SPEC168_STAGE0_EPOCHS_INVALID:sample={index}")
            else:
                received = request_epochs(
                    marker_text,
                    "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED", request_id)
                output_marker = (
                    "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED" if rank == 1
                    else "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED")
                produced = request_epochs(marker_text, output_marker, request_id)
                require(produced == expected_epochs and produced <= received,
                        f"SPEC168_DEPENDENCY_EVIDENCE_MISSING:{rank}:{index}")
                if rank == 2:
                    times = request_marker_times(
                        marker_text, output_marker, request_id)
                    require(len(times) == len(tokens),
                            f"SPEC168_TOKEN_TIMESTAMPS_INVALID:{index}")
                    rank2_inter_token[request_id] = [
                        right - left for left, right in zip(times, times[1:])]
        providers.append({
            "rank": rank, "node": node, "gpuUuid": gpu_row[0].strip(),
            "providerBootEpoch": boot_epoch,
            "finalCounters": previous_counters,
            "logDigest": sha256(node_root / f"provider-{rank}.log"),
        })

    require(len(set(nodes)) == 3, "SPEC168_DISTINCT_NODES_REQUIRED")
    require(len({item["uuid"] for item in gpus}) == 3,
            "SPEC168_DISTINCT_GPUS_REQUIRED")
    require(not list(root.rglob("*.token")) and not list(root.rglob("*.key")),
            "SPEC168_SECRET_RETAINED")

    measured = [row for row in retained_rows if row["phase"] == "measured"]
    prompt_distributions: dict[str, Any] = {}
    for prompt_id in prompt_ids:
        group = [row for row in measured if row["promptId"] == prompt_id]
        require(len(group) == 5, f"SPEC168_PROMPT_SAMPLE_COUNT:{prompt_id}")
        prompt_distributions[prompt_id] = {
            "ttftMs": describe(row["ttftMs"] for row in group),
            "totalMs": describe(row["totalMs"] for row in group),
            "tokensPerSecond": describe(
                row["tokensPerSecond"] for row in group),
            "interTokenMs": describe(
                value for row in group for value in row["interTokenMs"]),
            "planningTotalMs": describe(
                row["planningTimingsMs"]["pre_response_setup_total_ms"]
                for row in group),
        }
    cache_distributions = {}
    for cache_class in ("GENERATED", "REUSE_CACHED"):
        group = [row for row in retained_rows
                 if row["cacheClass"] == cache_class]
        require(group, f"SPEC168_CACHE_CLASS_EMPTY:{cache_class}")
        cache_distributions[cache_class] = {
            "ttftMs": describe(row["ttftMs"] for row in group),
            "totalMs": describe(row["totalMs"] for row in group),
            "tokensPerSecond": describe(
                row["tokensPerSecond"] for row in group),
        }

    return {
        "schema": "ndnsf-di.spec168-cold-warm-analysis.v1",
        "status": "PASS",
        "campaignId": campaign["campaignId"],
        "generationCampaignId": generation["campaignId"],
        "sourceDigest": args.expected_source_digest,
        "sourceBundleDigest": args.expected_source_bundle_digest,
        "runtimeSifDigest": args.expected_sif_digest,
        "stageManifestDigest": sha256(stage_path),
        "modelIdentityDigest": model_digest,
        "workloadDigest": workload_digest,
        "requestCount": len(rows),
        "wireRequestCount": len(rows),
        "tokenRequestCount": 0,
        "coldRequestCount": 1,
        "warmRequestCount": 29,
        "cpuFallbackCount": 0,
        "rows": retained_rows,
        "rank2InterTokenMs": rank2_inter_token,
        "promptDistributions": prompt_distributions,
        "cacheDistributions": cache_distributions,
        "nodes": nodes,
        "gpus": gpus,
        "providers": providers,
        "causalReuseVerdict": "PASS",
        "securityVerdict": "PASS",
        "statisticalClaimBoundary": (
            "Descriptive distributions only; n=5 measured repetitions per "
            "prompt do not support an inferential significance claim."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--campaign-manifest", required=True)
    parser.add_argument("--generation-campaign", required=True)
    parser.add_argument("--expected-source-digest", required=True)
    parser.add_argument("--expected-source-bundle-digest", required=True)
    parser.add_argument("--expected-sif-digest", required=True)
    parser.add_argument("--request-prefix", required=True)
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args()
    try:
        result = analyze(args)
    except Exception as error:  # noqa: BLE001
        result = {
            "schema": "ndnsf-di.spec168-cold-warm-analysis.v1",
            "status": "FAIL",
            "failure": f"{type(error).__name__}:{error}",
        }
        Path(args.output_json).write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(result, sort_keys=True))
        return 1
    Path(args.output_json).write_text(
        json.dumps(result, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
