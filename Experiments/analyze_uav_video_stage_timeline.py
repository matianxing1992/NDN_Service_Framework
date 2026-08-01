#!/usr/bin/env python3
"""Attribute UAV video latency from an immutable five-boundary timeline.

This analyzer is intentionally offline. It never launches MiniNDN and refuses
to write inside the campaign directory that it reads.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
from typing import Any, Iterable
from urllib.parse import unquote_to_bytes


TIMELINE_RE = re.compile(r"NDNSF_TIMELINE\s+(?P<fields>.*)$")
FRAME_PREFIX = ("NDNSF", "UAV", "VIDEO", "FRAME")
CURSOR_PREFIX = ("NDNSF", "STREAM", "TIMELINE")
STAGE_FIELDS = (
    "capture_to_encoded_ms",
    "encoded_to_materialized_ms",
    "materialized_to_decoder_input_ms",
    "decoder_input_to_output_ms",
    "capture_to_output_ms",
)


@dataclass(frozen=True)
class TimelineEvent:
    wall_seconds: float
    role: str
    event: str
    steady_us: int
    request_id: str
    fields: dict[str, str]


def parse_number_component(component: str) -> int:
    value = unquote_to_bytes(component)
    if not value:
        raise ValueError("empty NDN number component")
    return int.from_bytes(value, byteorder="big", signed=False)


def parse_name(request_id: str, prefix: tuple[str, ...]) -> tuple[str, int, int] | None:
    components = tuple(part for part in request_id.split("/") if part)
    if len(components) != len(prefix) + 3 or components[:len(prefix)] != prefix:
        return None
    try:
        return (
            components[-3],
            parse_number_component(components[-2]),
            parse_number_component(components[-1]),
        )
    except ValueError:
        return None


def parse_timeline(path: Path) -> list[TimelineEvent]:
    events: list[TimelineEvent] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = TIMELINE_RE.search(line)
            if match is None:
                continue
            tokens: dict[str, str] = {}
            for token in match.group("fields").split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    tokens[key] = value
            try:
                wall_seconds = float(line.split(None, 1)[0])
                events.append(TimelineEvent(
                    wall_seconds=wall_seconds,
                    role=tokens.pop("role"),
                    event=tokens.pop("event"),
                    steady_us=int(tokens.pop("steady_us")),
                    request_id=tokens.pop("requestId"),
                    fields=tokens,
                ))
            except (KeyError, ValueError):
                continue
    return events


def first_value(target: dict[Any, int], key: Any, value: int) -> None:
    if key not in target or value < target[key]:
        target[key] = value


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(values: Iterable[float]) -> dict[str, float | int | None]:
    samples = list(values)
    if not samples:
        return {"count": 0, "mean": None, "p50": None, "p95": None, "p99": None}
    return {
        "count": len(samples),
        "mean": statistics.fmean(samples),
        "p50": percentile(samples, 0.50),
        "p95": percentile(samples, 0.95),
        "p99": percentile(samples, 0.99),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_provider(events: Iterable[TimelineEvent]) -> dict[str, Any]:
    capture: dict[tuple[str, int, int], int] = {}
    encoded: dict[tuple[str, int, int], int] = {}
    cursor_frame: dict[tuple[str, int, int], int] = {}
    materialized: dict[tuple[str, int, int], int] = {}

    for item in events:
        frame_name = parse_name(item.request_id, FRAME_PREFIX)
        cursor_name = parse_name(item.request_id, CURSOR_PREFIX)
        if frame_name is not None and item.fields.get("frame_correlation") == "exact":
            if item.event == "source-acquired":
                try:
                    first_value(capture, frame_name,
                                int(item.fields["capture_origin_ns"]) // 1000)
                except (KeyError, ValueError):
                    pass
            elif item.event == "encoded-output-ready":
                first_value(encoded, frame_name, item.steady_us)
        elif cursor_name is not None:
            if item.event == "source-acquired":
                try:
                    cursor_frame[cursor_name] = int(item.fields["source_frame_id"])
                except (KeyError, ValueError):
                    pass
            elif item.event == "signed-and-materialized":
                first_value(materialized, cursor_name, item.steady_us)

    cursors_by_frame: dict[tuple[str, int, int], list[int]] = {}
    for (stream_id, epoch, cursor), source_frame_id in cursor_frame.items():
        cursors_by_frame.setdefault(
            (stream_id, epoch, source_frame_id), []).append(cursor)
    return {
        "capture": capture,
        "encoded": encoded,
        "cursors_by_frame": cursors_by_frame,
        "materialized": materialized,
    }


def collect_consumer(events: Iterable[TimelineEvent]) -> dict[str, Any]:
    decoder_input: dict[tuple[str, int, int], int] = {}
    decoder_output: dict[tuple[str, int, int], tuple[int, float]] = {}
    for item in events:
        cursor_name = parse_name(item.request_id, CURSOR_PREFIX)
        frame_name = parse_name(item.request_id, FRAME_PREFIX)
        if (item.event == "decoder-input" and cursor_name is not None and
                item.fields.get("frame_correlation") == "exact-pts"):
            try:
                key = (cursor_name[0], cursor_name[1],
                       int(item.fields["source_frame_id"]))
                first_value(decoder_input, key, item.steady_us)
            except (KeyError, ValueError):
                pass
        elif (item.event == "decoder-output" and frame_name is not None and
              item.fields.get("frame_correlation") == "exact"):
            current = decoder_output.get(frame_name)
            candidate = (item.steady_us, item.wall_seconds)
            if current is None or candidate[0] < current[0]:
                decoder_output[frame_name] = candidate
    return {"decoder_input": decoder_input, "decoder_output": decoder_output}


def analyze_cell(cell_dir: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    summary_path = cell_dir / "cell-summary.json"
    provider_path = cell_dir / "drone.log"
    consumer_path = cell_dir / "ground-station.log"
    required = (summary_path, provider_path, consumer_path)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"missing diagnostic input: {missing}")

    input_hashes = {path.name: sha256_file(path) for path in required}
    cell_summary = json.loads(summary_path.read_text(encoding="utf-8"))
    measurement = cell_summary["measurement"]
    start = float(measurement["startTimestamp"])
    end = float(measurement["endTimestamp"])
    provider = collect_provider(parse_timeline(provider_path))
    consumer = collect_consumer(parse_timeline(consumer_path))

    candidates = {
        key: value for key, value in consumer["decoder_output"].items()
        if start <= value[1] <= end
    }
    exclusions: dict[str, int] = {}
    rows: list[dict[str, Any]] = []
    for key, (output_us, output_wall) in sorted(candidates.items()):
        stream_id, epoch, source_frame_id = key
        missing_stages: list[str] = []
        capture_us = provider["capture"].get(key)
        encoded_us = provider["encoded"].get(key)
        input_us = consumer["decoder_input"].get(key)
        cursors = provider["cursors_by_frame"].get(key, [])
        last_cursor = max(cursors) if cursors else None
        materialized_us = (
            provider["materialized"].get((stream_id, epoch, last_cursor))
            if last_cursor is not None else None
        )
        for name, value in (
            ("capture", capture_us),
            ("encoded", encoded_us),
            ("last_segment_materialized", materialized_us),
            ("decoder_input", input_us),
            ("decoder_output", output_us),
        ):
            if value is None:
                missing_stages.append(name)
        if missing_stages:
            for name in missing_stages:
                exclusions[f"missing_{name}"] = exclusions.get(
                    f"missing_{name}", 0) + 1
            continue

        assert capture_us is not None
        assert encoded_us is not None
        assert materialized_us is not None
        assert input_us is not None
        ordered = (capture_us, encoded_us, materialized_us, input_us, output_us)
        if any(right < left for left, right in zip(ordered, ordered[1:])):
            exclusions["non_monotonic_timeline"] = (
                exclusions.get("non_monotonic_timeline", 0) + 1)
            continue
        rows.append({
            "stream_id": stream_id,
            "session_epoch": epoch,
            "source_frame_id": source_frame_id,
            "last_source_cursor": last_cursor,
            "source_segment_count": len(cursors),
            "output_wall_seconds": output_wall,
            "capture_us": capture_us,
            "encoded_us": encoded_us,
            "last_segment_materialized_us": materialized_us,
            "decoder_input_us": input_us,
            "decoder_output_us": output_us,
            "capture_to_encoded_ms": (encoded_us - capture_us) / 1000.0,
            "encoded_to_materialized_ms": (
                materialized_us - encoded_us) / 1000.0,
            "materialized_to_decoder_input_ms": (
                input_us - materialized_us) / 1000.0,
            "decoder_input_to_output_ms": (output_us - input_us) / 1000.0,
            "capture_to_output_ms": (output_us - capture_us) / 1000.0,
        })

    stages = {field: summarize(float(row[field]) for row in rows)
              for field in STAGE_FIELDS}
    interval_fields = STAGE_FIELDS[:-1]
    dominant_mean = max(
        interval_fields,
        key=lambda field: float(stages[field]["mean"] or float("-inf")),
    ) if rows else None
    fps = float(cell_summary.get("profile", {}).get(
        "fps", cell_summary.get("metrics", {}).get("requestedFps", 0)))
    result = {
        "schemaVersion": "uav-stage-latency-attribution-v1",
        "cellId": cell_dir.name,
        "fps": fps,
        "measurement": measurement,
        "candidateFrames": len(candidates),
        "completeFrames": len(rows),
        "coverageRatio": len(rows) / len(candidates) if candidates else 0.0,
        "exclusions": dict(sorted(exclusions.items())),
        "dominantIntervalByMean": dominant_mean,
        "sourceSegmentsPerFrame": summarize(
            float(row["source_segment_count"]) for row in rows),
        "stages": stages,
        "inputHashes": input_hashes,
        "clockContract": "same-host-linux-steady-clock",
    }
    after_hashes = {path.name: sha256_file(path) for path in required}
    if input_hashes != after_hashes:
        raise RuntimeError(f"diagnostic input changed while reading {cell_dir}")
    return result, rows


def ensure_external_output(campaign_dir: Path, output_dir: Path) -> None:
    campaign = campaign_dir.resolve()
    output = output_dir.resolve()
    if output == campaign or campaign in output.parents:
        raise ValueError("output directory must be outside the immutable campaign root")
    if output.exists() and any(output.iterdir()):
        raise FileExistsError(f"output directory is not empty: {output}")


def write_cell(output_dir: Path, summary: dict[str, Any],
               rows: list[dict[str, Any]]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "stage-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    fieldnames = list(rows[0]) if rows else [
        "stream_id", "session_epoch", "source_frame_id", "last_source_cursor",
        "source_segment_count", "output_wall_seconds", "capture_us", "encoded_us",
        "last_segment_materialized_us", "decoder_input_us", "decoder_output_us",
        *STAGE_FIELDS,
    ]
    with (output_dir / "per-frame-stages.csv").open(
            "w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_campaign(output_dir: Path, summaries: list[dict[str, Any]]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "schemaVersion": "uav-stage-latency-campaign-v1",
        "cells": summaries,
        "interpretationBoundary": (
            "descriptive stage attribution; no formal matrix rerun and no "
            "causal claim from independent percentiles"),
    }
    (output_dir / "campaign-stage-summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    csv_rows: list[dict[str, Any]] = []
    for cell in summaries:
        for stage, values in cell["stages"].items():
            csv_rows.append({
                "cell_id": cell["cellId"],
                "fps": cell["fps"],
                "candidate_frames": cell["candidateFrames"],
                "complete_frames": cell["completeFrames"],
                "coverage_ratio": cell["coverageRatio"],
                "source_segments_mean": cell["sourceSegmentsPerFrame"]["mean"],
                "source_segments_p50": cell["sourceSegmentsPerFrame"]["p50"],
                "stage": stage,
                **values,
            })
    with (output_dir / "campaign-stage-summary.csv").open(
            "w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(csv_rows[0]))
        writer.writeheader()
        writer.writerows(csv_rows)

    lines = [
        "# UAV Video Five-Stage Latency Attribution",
        "",
        "This is an offline diagnostic replay of immutable Spec 156 logs. It did "
        "not rerun MiniNDN or modify the formal six-rate campaign.",
        "",
        "| FPS | Complete/candidate | Coverage | Source segments/frame mean | p50 | Dominant interval by mean |",
        "|---:|---:|---:|---:|---:|---|",
    ]
    for cell in summaries:
        segments = cell["sourceSegmentsPerFrame"]
        lines.append(
            f"| {cell['fps']:g} | {cell['completeFrames']}/{cell['candidateFrames']} "
            f"| {cell['coverageRatio']:.3%} | {segments['mean']:.3f} "
            f"| {segments['p50']:.3f} | {cell['dominantIntervalByMean']} |")
    lines.extend([
        "",
        "| FPS | Complete/candidate | Coverage | Stage | Mean ms | p50 | p95 | p99 |",
        "|---:|---:|---:|---|---:|---:|---:|---:|",
    ])
    for row in csv_rows:
        lines.append(
            f"| {row['fps']:g} | {row['complete_frames']}/{row['candidate_frames']} "
            f"| {row['coverage_ratio']:.3%} | {row['stage']} "
            f"| {row['mean']:.3f} | {row['p50']:.3f} "
            f"| {row['p95']:.3f} | {row['p99']:.3f} |")
    lines.extend([
        "",
        "Each percentile is computed from per-frame deltas for that stage. "
        "Independent stage percentiles are not summed. The largest interval is "
        "descriptive localization, not proof of a universal causal mechanism.",
        "",
    ])
    (output_dir / "stage-attribution.md").write_text(
        "\n".join(lines), encoding="utf-8")


def cell_sort_key(path: Path) -> tuple[float, str]:
    match = re.fullmatch(r"fps-(\d+(?:\.\d+)?)", path.name)
    return (float(match.group(1)) if match else float("inf"), path.name)


def analyze_campaign(campaign_dir: Path, output_dir: Path) -> list[dict[str, Any]]:
    ensure_external_output(campaign_dir, output_dir)
    cells = sorted(
        (path for path in campaign_dir.glob("fps-*") if path.is_dir()),
        key=cell_sort_key,
    )
    if not cells:
        raise FileNotFoundError(f"no fps-* cells in {campaign_dir}")
    summaries: list[dict[str, Any]] = []
    for cell in cells:
        summary, rows = analyze_cell(cell)
        write_cell(output_dir / cell.name, summary, rows)
        summaries.append(summary)
    write_campaign(output_dir, summaries)
    return summaries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    summaries = analyze_campaign(args.campaign_dir, args.output_dir)
    print(json.dumps({
        "outputDir": str(args.output_dir.resolve()),
        "cells": len(summaries),
        "completeFrames": sum(item["completeFrames"] for item in summaries),
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
