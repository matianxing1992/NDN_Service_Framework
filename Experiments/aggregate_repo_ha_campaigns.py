#!/usr/bin/env python3
"""Aggregate canonical Spec 077 MiniNDN campaign summaries."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("summary", nargs="+", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--max-stable-failure-rate", type=float, default=0.01)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for path in args.summary:
        obj = json.loads(path.read_text(encoding="utf-8"))
        latency = obj.get("latencyMs", {})
        row = {
            "summary": str(path),
            "mode": obj.get("objectMode", ""),
            "offeredRps": float(obj.get("offeredRps", 0)),
            "achievedRps": float(obj.get("achievedRps", 0)),
            "concurrency": int(obj.get("concurrency", 0)),
            "readRatio": float(obj.get("readRatio", 0)),
            "replicationFactor": int(obj.get("replicationFactor", 0)),
            "writeConsistency": obj.get("writeConsistency", ""),
            "attempted": int(obj.get("attempted", 0)),
            "failed": int(obj.get("failed", 0)),
            "failureRate": float(obj.get("failureRate", 0)),
            "rejectionCount": int(obj.get("rejectionCount", 0)),
            "p50Ms": float(latency.get("p50", 0)),
            "p95Ms": float(latency.get("p95", 0)),
            "p99Ms": float(latency.get("p99", 0)),
            "stddevMs": float(latency.get("stddev", 0)),
        }
        row["stable"] = int(
            row["failureRate"] <= args.max_stable_failure_rate)
        rows.append(row)
    rows.sort(key=lambda row: (row["concurrency"], row["offeredRps"]))
    csv_path = args.output_dir / "campaign-summary.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else ["summary"])
        writer.writeheader()
        writer.writerows(rows)
    stable = [row for row in rows if row.get("stable")]
    aggregate = {
        "campaignCount": len(rows),
        "stableCampaignCount": len(stable),
        "maxStableOfferedRps": max(
            [row["offeredRps"] for row in stable] or [0.0]),
        "maxStableAchievedRps": max(
            [row["achievedRps"] for row in stable] or [0.0]),
        "rows": rows,
        "csv": str(csv_path),
    }
    (args.output_dir / "campaign-summary.json").write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(json.dumps(aggregate, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
