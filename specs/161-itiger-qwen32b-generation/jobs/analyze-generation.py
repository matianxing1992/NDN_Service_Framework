#!/usr/bin/env python3
"""Summarize retained Spec 161 generation JSONL without dropping failures."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
from typing import Any, Iterable


SCHEMA = "ndnsf-di-qwen-generation-summary-v1"


def _percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(float(value) for value in values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * quantile
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def _metric(values: Iterable[float]) -> dict[str, float | int]:
    rows = [float(value) for value in values]
    if not rows:
        return {
            "sampleCount": 0,
            "min": 0.0,
            "max": 0.0,
            "p50": 0.0,
            "p95": 0.0,
        }
    return {
        "sampleCount": len(rows),
        "min": min(rows),
        "max": max(rows),
        "p50": statistics.median(rows),
        "p95": _percentile(rows, 0.95),
    }


def _is_success(sample: dict[str, Any]) -> bool:
    return (
        sample.get("phase") == "measured"
        and sample.get("status") == "OK"
        and sample.get("stopReason") == "EOS"
        and sample.get("exactReferenceMatch") is True
    )


def _summary_for_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    successful = [row for row in rows if _is_success(row)]
    inter_token = [
        float(value)
        for row in successful
        for value in row.get("interTokenMs", [])
    ]
    return {
        "measuredCount": len(rows),
        "successfulCount": len(successful),
        "completionRate": (
            len(successful) / len(rows) if rows else 0.0
        ),
        "ttftMs": _metric(row.get("ttftMs", 0.0) for row in successful),
        "totalMs": _metric(row.get("totalMs", 0.0) for row in successful),
        "tokensPerSecond": _metric(
            row.get("tokensPerSecond", 0.0) for row in successful),
        "interTokenMs": _metric(inter_token),
    }


def summarize_samples(samples: Iterable[dict[str, Any]]) -> dict[str, Any]:
    rows = [dict(sample) for sample in samples]
    measured = [row for row in rows if row.get("phase") == "measured"]
    successful = [row for row in measured if _is_success(row)]
    prompt_ids = sorted({
        str(row.get("promptId", ""))
        for row in measured
        if str(row.get("promptId", ""))
    })
    failures = Counter(
        f"{row.get('status', 'UNKNOWN')}:{row.get('stopReason', 'UNKNOWN')}"
        for row in measured
        if not _is_success(row)
    )
    per_prompt = {
        prompt_id: _summary_for_rows([
            row for row in measured if row.get("promptId") == prompt_id
        ])
        for prompt_id in prompt_ids
    }
    pooled = _summary_for_rows(measured)
    pooled["classification"] = "descriptive"
    pooled["sampleCount"] = len(successful)
    warmup_count = sum(row.get("phase") == "warmup" for row in rows)
    acceptance_ready = (
        len(prompt_ids) == 5
        and warmup_count == 5
        and len(measured) == 25
        and len(successful) == 25
    )
    return {
        "schemaVersion": SCHEMA,
        "rawRecordCount": len(rows),
        "warmupRecordCount": warmup_count,
        "measuredRecordCount": len(measured),
        "successfulMeasuredCount": len(successful),
        "failureCounts": dict(sorted(failures.items())),
        "perPrompt": per_prompt,
        "pooled": pooled,
        "p99Reported": False,
        "acceptanceReady": acceptance_ready,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-jsonl", required=True)
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args(argv)
    source = Path(args.input_jsonl)
    rows = [
        json.loads(line)
        for line in source.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    summary = summarize_samples(rows)
    target = Path(args.output_json)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
