#!/usr/bin/env python3
"""Correlate one Spec 162 generation with three stages and two dependencies."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
from typing import Any


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def marker_fields(text: str, marker: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []

    def parse_tail(tail: str) -> dict[str, str]:
        # Provider and Core diagnostics share stdout.  Under concurrent
        # handlers a logger timestamp or the next marker can be inserted
        # between two fields, and two adjacent ``key=value`` fields can lose
        # their separating space (for example ``token-5reason=...``).  Split
        # on every field key rather than treating whitespace as the delimiter.
        matches = list(re.finditer(r"([A-Za-z][A-Za-z0-9_]*)=", tail))
        parsed: dict[str, str] = {}
        for index, match in enumerate(matches):
            key = match.group(1)
            end = (matches[index + 1].start()
                   if index + 1 < len(matches) else len(tail))
            value = tail[match.end():end].strip()
            if value:
                parsed[key] = value
        return parsed

    for line in text.splitlines():
        if marker not in line:
            continue
        tail = line.split(marker, 1)[1].strip()
        rows.append(parse_tail(tail))
    return rows


def canonical_request_id(value: str) -> str:
    """Compare wire names and generation records without a leading slash."""
    return value.lstrip("/")


def request_id_matches(actual: str, expected: str) -> bool:
    return canonical_request_id(actual) == canonical_request_id(expected)


def request_phase_timings(text: str) -> dict[str, dict[str, float]]:
    rows: dict[str, dict[str, float]] = {}
    pattern = re.compile(
        r"LLM_PIPELINE_QWEN_REQUEST_PHASE_TIMING\s+requestId=(\S+)\s+(\{.*\})")
    for line in text.splitlines():
        match = pattern.search(line)
        if match is None:
            continue
        request_id = canonical_request_id(match.group(1))
        require(request_id not in rows, f"duplicate request timing: {request_id}")
        rows[request_id] = {
            str(key): float(value)
            for key, value in json.loads(match.group(2)).items()
        }
    return rows


def ack_release_order(
    text: str,
    request_ids: list[str],
    *,
    rank: int,
) -> dict[str, int]:
    def parse_fields(tail: str) -> dict[str, str]:
        # Concurrent marker writes can concatenate a marker suffix with the
        # next field (``...ACK_TTLrequestId=...``).  Parse the known contract
        # fields by key rather than requiring whitespace before ``requestId``.
        keys = ("requestId", "attempt", "status", "reservationHeld",
                "reason", "role")
        pattern = re.compile(
            r"(" + "|".join(keys) + r")=")
        matches = list(pattern.finditer(tail))
        return {
            match.group(1): tail[match.end():(
                matches[index + 1].start()
                if index + 1 < len(matches) else len(tail)
            )].strip()
            for index, match in enumerate(matches)
        }

    events: list[tuple[int, str, dict[str, str]]] = []
    marker_pattern = re.compile(
        r"NDNSF_DI_(ACK_DECISION|SELECTION_RESERVATION_RELEASED)")
    markers = list(marker_pattern.finditer(text))
    for index, marker_match in enumerate(markers):
        kind = "ack" if marker_match.group(1) == "ACK_DECISION" else "release"
        end = (markers[index + 1].start()
               if index + 1 < len(markers) else len(text))
        # A framework logger can split one marker and its requestId over
        # adjacent physical lines.  Keep the event boundary marker-based,
        # then parse fields across newlines.
        tail = text[marker_match.end():end].replace("\n", " ").strip()
        fields = parse_fields(tail)
        line_number = text.count("\n", 0, marker_match.start()) + 1
        events.append((line_number, kind, fields))

    positions: dict[tuple[str, str], list[int]] = {}
    for line_number, kind, fields in events:
        # Provider diagnostics use canonical absolute NDN-name URIs while the
        # generation JSONL stores the same wire ID without a leading slash.
        # Normalize at the event boundary before joining ACK/release evidence;
        # otherwise every otherwise-valid request appears to have zero ACKs.
        request_id = canonical_request_id(fields.get("requestId", ""))
        if request_id in request_ids:
            if kind == "ack":
                # ACK_DECISION is also emitted by the TTL/diagnostic path,
                # which can lack reservationHeld or be concatenated with a
                # neighbouring marker.  A successful ACK is the admission
                # decision; it must not be coupled to a resource lock or a
                # reservationHeld field, because stages may overlap and Repo
                # work is queued rather than reserved.
                if fields.get("status", "").lower().startswith("true"):
                    positions.setdefault((request_id, kind), []).append(
                        line_number)
            else:
                positions.setdefault((request_id, kind), []).append(
                    line_number)

    for index, request_id in enumerate(request_ids):
        ack_lines = positions.get((request_id, "ack"), [])
        release_lines = positions.get((request_id, "release"), [])
        # NDN retransmission may deliver the same valid ACK more than once.
        # Count it as one logical admission; reject only missing coverage.
        require(
            len(ack_lines) >= 1,
            f"provider {rank} ACK coverage mismatch for {request_id}",
        )
        require(
            len(release_lines) == 1,
            f"provider {rank} release coverage mismatch for {request_id}",
        )
        require(
            ack_lines[0] < release_lines[0],
            f"provider {rank} released before ACK for {request_id}",
        )
        # Requests are independent invocation epochs.  A later ACK may be
        # admitted before an earlier role's diagnostic release line is
        # flushed, especially when the three stages overlap.  Enforcing a
        # global release-before-next-ACK order would reintroduce a fixed
        # serialization barrier and contradict data-driven execution.
    return {
        "ackTrueCount": len(request_ids),
        "reservationReleaseCount": len(request_ids),
        "releaseBeforeNextAckCount": None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--enriched-jsonl", required=True)
    args = parser.parse_args()

    root = Path(args.root)
    stage_manifest = json.loads(
        Path(args.stage_manifest).read_text(encoding="utf-8"))
    ranges = [list(item) for item in stage_manifest["layerRanges"]]
    require(
        len(ranges) == 3
        and ranges[0][0] == 0
        and all(ranges[index][1] == ranges[index + 1][0]
                for index in range(2))
        and ranges[-1][1] == int(stage_manifest["layerCount"])
        and all(start < end for start, end in ranges),
        f"invalid three-stage layer cover: {ranges}",
    )
    raw_rows = [
        json.loads(line)
        for line in (root / "node-0/generation-raw.jsonl")
        .read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    require(len(raw_rows) == 1, f"expected one smoke row, got {len(raw_rows)}")
    row = raw_rows[0]
    require(row.get("status") == "OK", f"generation status: {row.get('status')}")
    require(row.get("stopReason") in {"EOS", "MAX_NEW_TOKENS"},
            f"generation stop reason: {row.get('stopReason')}")
    require(row.get("exactReferenceMatch") is True,
            "generation did not match the frozen RTX reference")
    require(str(row.get("decodedText", "")).strip(),
            "final response text is missing from generation evidence")
    steps = list(row.get("tokenSteps", []))
    generated = list(row.get("generatedTokenIds", []))
    require(steps and len(steps) == len(generated),
            "token step count does not match generated tokens")
    request_ids = [canonical_request_id(str(step.get("requestId", "")))
                   for step in steps]
    require(all(request_ids) and len(set(request_ids)) == len(request_ids),
            "token request IDs are missing or duplicated")
    user_log = (root / "node-0/user.log").read_text(
        encoding="utf-8", errors="replace")
    planning_manifest = json.loads(
        (root / "automatic-planning.json").read_text(encoding="utf-8"))
    require(
        planning_manifest["preSplitCatalog"]["publicationState"] == (
            "REQUIRES_DISTRIBUTED_REPO_REGISTRATION"),
        "campaign used an eagerly published Repo catalog",
    )
    user_lines = user_log.splitlines()

    def event_line(marker: str, request_id: str = "") -> int:
        request_variants = {request_id}
        if request_id:
            bare = request_id.lstrip("/")
            request_variants.update((bare, "/" + bare))
        for number, line in enumerate(user_lines, start=1):
            if marker not in line:
                continue
            if request_id and not any(
                f"requestId={candidate}" in line
                for candidate in request_variants
            ):
                continue
            return number
        return 0

    first_request_id = request_ids[0]
    # The request gate is a submission-level lifecycle marker emitted before
    # the User process constructs its first token-level request ID.  Its
    # requestId therefore names the durable submission, not token-0; correlate
    # it by ordering and validate the explicit REQUEST_FIRST mode instead of
    # requiring equality with the first token request.
    request_gate_line = event_line("SPEC162_REQUEST_GATE_OPEN")
    request_sent_line = event_line(
        "NDNSF_DI_AUTOPLANNING_REQUEST_SENT", first_request_id)
    ack_closed_line = event_line(
        "NDNSF_DI_AUTOPLANNING_ACK_CLOSED", first_request_id)
    artifacts_ready_line = event_line(
        "NDNSF_DI_AUTOPLANNING_ARTIFACTS_READY", first_request_id)
    selection_line = event_line(
        "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED", first_request_id)
    runtime_placement = root / "source/runtime/di-placement.py"
    graph_marker_required = (
        runtime_placement.exists()
        and "NDNSF_DI_AUTOPLANNING_GRAPH_READY" in
        runtime_placement.read_text(encoding="utf-8", errors="replace")
    )
    graph_ready_line = event_line(
        "NDNSF_DI_AUTOPLANNING_GRAPH_READY", first_request_id)
    require(request_gate_line > 0,
            "request gate did not open before the first User Request")
    gate_fields = marker_fields(
        user_lines[request_gate_line - 1], "SPEC162_REQUEST_GATE_OPEN")
    require(gate_fields and gate_fields[0].get("requestId", ""),
            "request gate marker lacks a submission request ID")
    require(gate_fields[0].get("mode") == "REQUEST_FIRST",
            "request gate marker does not declare REQUEST_FIRST mode")
    require(request_sent_line > request_gate_line,
            "User Request was not opened after the request-gate marker")
    require(ack_closed_line > request_sent_line,
            "ACK_CLOSED did not follow Request")
    if graph_marker_required:
        require(graph_ready_line > ack_closed_line,
                "graph planning did not occur after ACK_CLOSED")
    require(artifacts_ready_line > ack_closed_line,
            "model publication occurred before ACK_CLOSED")
    if graph_marker_required:
        require(artifacts_ready_line > graph_ready_line,
                "model publication occurred before graph planning")
    require(selection_line > artifacts_ready_line,
            "Selection committed before model publication")
    require(
        event_line("LLM_PIPELINE_QWEN_DEFERRED_SPLIT") > ack_closed_line,
        "deferred split did not occur after ACK_CLOSED",
    )
    require(
        event_line("LLM_PIPELINE_QWEN_DEFERRED_REPO_PUBLISH_START")
        > ack_closed_line,
        "Repo publication did not start after ACK_CLOSED",
    )
    require(
        event_line("LLM_PIPELINE_QWEN_DEFERRED_REPO_PUBLISH_DONE")
        < selection_line,
        "Repo publication completed after Selection commit",
    )
    final_response = marker_fields(
        user_log, "LLM_PIPELINE_GENERATION_FINAL_RESPONSE")
    require(len(final_response) == 1,
            f"expected one persisted final response marker, got {len(final_response)}")
    final_marker = final_response[0]
    require(final_marker.get("generationId") == str(row.get("generationId", "")),
            "final response marker generation ID mismatch")
    require(final_marker.get("status") == "OK",
            "final response marker is not successful")
    require(final_marker.get("tokenCount") == str(len(row.get("generatedTokenIds", []))),
            "final response marker token count mismatch")
    expected_response_digest = hashlib.sha256(
        str(row.get("decodedText", "")).encode("utf-8")).hexdigest()
    require(final_marker.get("responseSha256") == expected_response_digest,
            "final response marker digest mismatch")
    planning_timings = request_phase_timings(user_log)
    require(set(planning_timings) == set(request_ids),
            "automatic planning timing coverage does not match token requests")

    nodes = [
        (root / f"node-{rank}/hostname.txt").read_text().strip()
        for rank in range(3)
    ]
    require(len(set(nodes)) == 3, f"nodes are not distinct: {nodes}")
    gpus = []
    for rank in range(3):
        values = next(csv.reader([
            (root / f"node-{rank}/gpu.csv").read_text().splitlines()[0]
        ]))
        values = [item.strip() for item in values]
        require(values[0].startswith("GPU-"),
                f"rank {rank} GPU UUID is invalid")
        require("RTX 5000" in values[1],
                f"rank {rank} is not RTX 5000: {values[1]}")
        gpus.append({"rank": rank, "uuid": values[0], "name": values[1]})
    require(len({item["uuid"] for item in gpus}) == 3,
            "GPU UUIDs are not distinct")

    timings_by_rank: list[dict[str, dict[str, str]]] = []
    fetches_by_rank: list[list[dict[str, str]]] = []
    fetch_progress_by_rank: list[list[dict[str, str]]] = []
    fetch_completions_by_rank: list[dict[str, str]] = []
    repo_prepare_by_rank: list[list[dict[str, str]]] = []
    cold_fetch_request_ids_by_rank: list[str] = []
    cold_prepares_by_rank: list[dict[str, str]] = []
    releases_by_rank: list[list[dict[str, str]]] = []
    ack_release_order_by_rank: list[dict[str, int]] = []
    for rank in range(3):
        log = (root / f"node-{rank}/provider-{rank}.log").read_text(
            encoding="utf-8", errors="replace")
        marker_path = root / f"node-{rank}/provider-markers-{rank}.log"
        marker_log = (marker_path.read_text(encoding="utf-8", errors="replace")
                      if marker_path.is_file() else log)
        require(marker_path.is_file(),
                f"provider {rank} marker log is missing")
        require("LLM_PIPELINE_PROVIDER_READY" in log,
                f"provider {rank} did not become ready")
        timings = marker_fields(marker_log, "LLM_PIPELINE_QWEN_STAGE_TIMING")
        require(len(timings) == len(steps),
                f"provider {rank} timing count {len(timings)} != {len(steps)}")
        mapping = {
            canonical_request_id(item.get("requestId", "")): item
            for item in timings
        }
        require(set(mapping) == set(request_ids),
                f"provider {rank} request IDs do not match generation")
        for item in timings:
            require(item.get("role") == f"/LLM/Pipeline/Stage/{rank}",
                    f"provider {rank} role mismatch")
            require(item.get("stage") == str(rank),
                    f"provider {rank} stage mismatch")
            require(item.get("device", "").startswith("cuda"),
                    f"provider {rank} did not execute on CUDA")
            require(item.get("cpuFallback", "").lower() in {"0", "false"},
                    f"provider {rank} used CPU fallback")
            for key in ("input_sha256", "output_sha256"):
                require(re.fullmatch(r"[0-9a-f]{64}", item.get(key, "")) is not None,
                        f"provider {rank} missing {key}")
        execution_ready = marker_fields(
            marker_log, "LLM_PIPELINE_QWEN_STAGE_EXECUTION_READY")
        require(len(execution_ready) == len(steps),
                f"provider {rank} execution-ready count mismatch")
        require({canonical_request_id(item.get("requestId", ""))
                 for item in execution_ready}
                == set(request_ids),
                f"provider {rank} execution-ready request IDs mismatch")
        if rank == 0:
            require(
                not marker_fields(marker_log, "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_WAIT"),
                "stage 0 waited for a predecessor dependency",
            )
        else:
            dependency_wait = marker_fields(
                marker_log, "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_WAIT")
            dependency_ready = marker_fields(
                marker_log, "LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_READY")
            require(len(dependency_wait) == len(steps),
                    f"provider {rank} dependency wait count mismatch")
            require(len(dependency_ready) == len(steps),
                    f"provider {rank} dependency ready count mismatch")
            require({canonical_request_id(item.get("requestId", ""))
                     for item in dependency_wait}
                    == set(request_ids),
                    f"provider {rank} dependency wait request IDs mismatch")
            require({canonical_request_id(item.get("requestId", ""))
                     for item in dependency_ready}
                    == set(request_ids),
                    f"provider {rank} dependency ready request IDs mismatch")
        timings_by_rank.append(mapping)
        fetches_by_rank.append(marker_fields(
            log, "NDNSF_COLLAB_LARGE_FETCH_TIMING event=complete"))
        fetch_progress = marker_fields(
            marker_log, "LLM_PIPELINE_QWEN_REPO_FETCH_PROGRESS")
        require(
            fetch_progress,
            f"provider {rank} has no Repo fetch progress evidence",
        )
        prepares = marker_fields(
            marker_log, "LLM_PIPELINE_QWEN_SELECTION_PREPARE")
        require(len(prepares) == len(steps),
                f"provider {rank} Selection prepare count mismatch")
        cold_prepares = [
            item for item in prepares
            if item.get("cacheHit", "").lower() == "false"
            and item.get("diskCacheHit", "").lower() == "false"
            and float(item.get("fetch_ms", "0")) > 0.0
        ]
        require(len(cold_prepares) == 1,
                f"provider {rank} expected one cold Repo fetch, got "
                f"{len(cold_prepares)}")
        cold_prepare = cold_prepares[0]
        cold_request_id = canonical_request_id(
            cold_prepare.get("requestId", ""))
        require(cold_request_id in request_ids,
                f"provider {rank} cold fetch request is not measured")
        cold_progress = [
            item for item in fetch_progress
            if request_id_matches(item.get("requestId", ""), cold_request_id)
        ]
        require(
            cold_progress,
            f"provider {rank} has no cold-request Repo progress evidence",
        )
        previous_progress = None
        for progress in cold_progress:
            sequence = int(progress.get("sequence", "0"))
            received = int(progress.get("receivedBytes", "0"))
            if previous_progress is not None:
                require(
                    sequence > previous_progress[0]
                    and received >= previous_progress[1],
                    f"provider {rank} Repo progress is not monotonic",
                )
            previous_progress = (sequence, received)
        require(
            any(int(item.get("receivedBytes", "0")) > 0
                for item in cold_progress),
            f"provider {rank} Repo progress has no received bytes",
        )
        completed_fetch = [
            item for item in marker_fields(
                marker_log, "LLM_PIPELINE_QWEN_REPO_FETCH"
            )
            if request_id_matches(item.get("requestId", ""), cold_request_id)
            and item.get("objectName")
        ]
        require(
            completed_fetch,
            f"provider {rank} has no completed Repo fetch evidence",
        )
        completed_fetch = completed_fetch[-1]
        require(
            int(completed_fetch.get("bytes", "0")) > 0,
            f"provider {rank} cold-request Repo completion has no bytes",
        )
        require(
            int(completed_fetch.get("totalSegments", "0")) > 0
            and int(completed_fetch.get("deliveredSegments", "0"))
            == int(completed_fetch.get("totalSegments", "-1")),
            f"provider {rank} cold-request segment coverage is incomplete",
        )
        fetch_completions_by_rank.append(completed_fetch)
        fetch_progress_by_rank.append(fetch_progress)
        cold_fetch_request_ids_by_rank.append(cold_request_id)
        cold_prepares_by_rank.append(cold_prepare)
        cold_seen = False
        for item in prepares:
            request_id = canonical_request_id(item.get("requestId", ""))
            fetch_ms = float(item.get("fetch_ms", "0"))
            if request_id == cold_request_id:
                cold_seen = True
                require(
                    item.get("cacheHit", "").lower() == "false"
                    and item.get("diskCacheHit", "").lower() == "false"
                    and fetch_ms > 0.0
                    and float(item.get("load_ms", "0")) > 0.0,
                    f"provider {rank} cold preparation is incomplete",
                )
            elif not cold_seen:
                # A stage may already have a local artifact path and load its
                # GPU state before its first Repo fetch marker is emitted.
                require(
                    fetch_ms == 0.0
                    and float(item.get("load_ms", "0")) > 0.0,
                    f"provider {rank} preloaded preparation is invalid",
                )
            else:
                require(
                    item.get("cacheHit", "").lower() == "true"
                    and fetch_ms == 0.0,
                    f"provider {rank} warm request missed the GPU model cache",
                )
        require(cold_seen,
                f"provider {rank} cold preparation was not observed")
        fetch_complete = [
            item for item in marker_fields(
                marker_log, "LLM_PIPELINE_QWEN_REPO_FETCH_COMPLETE")
            if request_id_matches(item.get("requestId", ""), cold_request_id)
        ]
        require(len(fetch_complete) == 1,
                f"provider {rank} completed Repo fetch marker coverage mismatch")
        require(int(fetch_complete[0].get("bytes", "0")) ==
                int(completed_fetch.get("bytes", "0")),
                f"provider {rank} fetch completion byte mismatch")
        if rank < 2:
            stage_outputs = marker_fields(
                marker_log, "LLM_PIPELINE_QWEN_STAGE_OUTPUT")
            require(len(stage_outputs) == len(steps),
                    f"provider {rank} stage output count mismatch")
            require({canonical_request_id(item.get("requestId", ""))
                     for item in stage_outputs}
                    == set(request_ids),
                    f"provider {rank} stage output request IDs mismatch")
        else:
            final_published = marker_fields(
                marker_log, "LLM_PIPELINE_QWEN_FINAL_RESPONSE_PUBLISHED")
            require(len(final_published) == len(steps),
                    "final Provider response publication count mismatch")
            require({canonical_request_id(item.get("requestId", ""))
                     for item in final_published}
                    == set(request_ids),
                    "final Provider response request IDs mismatch")
        releases = marker_fields(
            log, "NDNSF_DI_SELECTION_RESERVATION_RELEASED")
        require({canonical_request_id(item.get("requestId", ""))
                 for item in releases} == set(request_ids),
                f"provider {rank} reservation release coverage mismatch")
        repo_prepare_by_rank.append(prepares)
        releases_by_rank.append(releases)
        ack_release_order_by_rank.append(
            ack_release_order(log, request_ids, rank=rank)
        )

    enriched_steps = []
    for step in steps:
        request_id = str(step["requestId"])
        timings = [mapping[request_id] for mapping in timings_by_rank]
        require(timings[0]["output_sha256"] == timings[1]["input_sha256"],
                f"stage 0/1 digest mismatch for {request_id}")
        require(timings[1]["output_sha256"] == timings[2]["input_sha256"],
                f"stage 1/2 digest mismatch for {request_id}")
        require(timings[0]["dataName"] not in {"", "-"},
                f"stage 0 data name missing for {request_id}")
        require(timings[1]["dataName"] not in {"", "-"},
                f"stage 1 data name missing for {request_id}")
        require(timings[0]["dataName"] != timings[1]["dataName"],
                f"dependency names collide for {request_id}")
        require(timings[2]["dataName"] == "-",
                f"final stage unexpectedly published dependency for {request_id}")
        dependencies = []
        for producer, consumer in ((0, 1), (1, 2)):
            data_name = timings[producer]["dataName"]
            matches = [
                item for item in fetches_by_rank[consumer]
                if item.get("dataName") == data_name
            ]
            require(len(matches) == 1,
                    f"dependency fetch evidence missing for {request_id} "
                    f"{producer}->{consumer}")
            dependencies.append({
                "producerStage": producer,
                "consumerStage": consumer,
                "dataName": data_name,
                "sha256": timings[producer]["output_sha256"],
                "encodedBytes": int(matches[0].get("encoded_bytes", "0")),
                "receivedSegments": int(
                    matches[0].get("received_segments", "0")),
                "validatedSegments": int(
                    matches[0].get("validated_segments", "0")),
                "receivedWireBytes": int(
                    matches[0].get("received_wire_bytes", "0")),
            })
        enriched_steps.append({
            **step,
            "stageReceipts": [
                {
                    "rank": rank,
                    "node": nodes[rank],
                    "gpuUuid": gpus[rank]["uuid"],
                    "role": f"/LLM/Pipeline/Stage/{rank}",
                    "layerRange": ranges[rank],
                    "backend": "transformers",
                    "device": timings[rank]["device"],
                    "cpuFallback": False,
                    "inputSha256": timings[rank]["input_sha256"],
                    "outputSha256": timings[rank]["output_sha256"],
                }
                for rank in range(3)
            ],
            "dependencyReceipts": dependencies,
        })

    enriched = {**row, "tokenSteps": enriched_steps}
    Path(args.enriched_jsonl).write_text(
        json.dumps(enriched, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    analysis = {
        "schemaVersion": "ndnsf-di-qwen36-generation-smoke-analysis-v2",
        "status": "PASS",
        "promptId": row["promptId"],
        "decodedText": row["decodedText"],
        "generatedTokenIds": generated,
        "generatedTokenCount": len(generated),
        "exactReferenceMatch": True,
        "stopReason": row["stopReason"],
        "nodes": nodes,
        "gpus": gpus,
        "layerRanges": ranges,
        "tokenRequestCount": len(request_ids),
        "stageReceiptCount": len(request_ids) * 3,
        "dependencyReceiptCount": len(request_ids) * 2,
        "cpuFallbackCount": 0,
        "coldPath": {
            "firstTokenRequestId": request_ids[0],
            "providerColdFetchRequestId": cold_fetch_request_ids_by_rank,
            "providerRepoFetchMs": [
                float(item["fetch_ms"])
                for item in cold_prepares_by_rank
            ],
            "providerGpuLoadMs": [
                float(item["load_ms"])
                for item in cold_prepares_by_rank
            ],
            "providerRepoFetchProgress": [
                [
                    {
                        "requestId": item.get("requestId", ""),
                        "role": item.get("role", ""),
                        "sequence": int(item.get("sequence", "0")),
                        "receivedBytes": int(item.get("receivedBytes", "0")),
                        "verifiedBytes": int(item.get("verifiedBytes", "0")),
                        "totalBytes": int(item.get("totalBytes", "0")),
                        "lastSegment": int(item.get("lastSegment", "-1")),
                        "deliveredSegments": int(
                            item.get("deliveredSegments", "0")
                        ),
                        "totalSegments": int(
                            item.get("totalSegments", "0")
                        ),
                        "retransmittedBytes": int(
                            item.get("retransmittedBytes", "0")
                        ),
                        "elapsedMs": float(item.get("elapsedMs", "0")),
                    }
                    for item in progress
                ]
                for progress in fetch_progress_by_rank
            ],
            "providerRepoFetchCompletion": [
                {
                    "requestId": item.get("requestId", ""),
                    "role": item.get("role", ""),
                    "objectName": item.get("objectName", ""),
                    "bytes": int(item.get("bytes", "0")),
                    "lastSegment": int(item.get("lastSegment", "-1")),
                    "deliveredSegments": int(
                        item.get("deliveredSegments", "0")
                    ),
                    "totalSegments": int(item.get("totalSegments", "0")),
                    "retransmittedBytes": int(
                        item.get("retransmittedBytes", "0")
                    ),
                }
                for item in fetch_completions_by_rank
            ],
            "requestPhasesMs": planning_timings[request_ids[0]],
        },
        "warmPath": {
            "tokenRequestCount": max(0, len(request_ids) - 1),
            "gpuCacheHitCountByRank": [
                sum(item.get("cacheHit", "").lower() == "true"
                    for item in items
                    if canonical_request_id(item.get("requestId", ""))
                    != cold_fetch_request_ids_by_rank[rank])
                for rank, items in enumerate(repo_prepare_by_rank)
            ],
            "repeatedRepoFetchCount": sum(
                float(item.get("fetch_ms", "0")) > 0.0
                for rank, items in enumerate(repo_prepare_by_rank)
                for item in items
                if canonical_request_id(item.get("requestId", ""))
                != cold_fetch_request_ids_by_rank[rank]
            ),
        },
        "reservationReleaseCountByRank": [
            len(items) for items in releases_by_rank
        ],
        "ackReleaseOrderingByRank": ack_release_order_by_rank,
        "timingSemantics": {
            "requestUnit": (
                "one token-level NDNSF collaboration; a complete decoded "
                "answer contains one or more sequential token requests"
            ),
            "phaseTotalsOverlap": True,
            "releaseInvariant": (
                "each admitted request has a true ACK and its own role "
                "release; different requests may overlap"
            ),
        },
    }
    Path(args.output_json).write_text(
        json.dumps(analysis, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(analysis, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
