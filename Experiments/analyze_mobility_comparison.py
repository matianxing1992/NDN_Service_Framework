#!/usr/bin/env python3
"""Validate, summarize, and plot a trace-paired Spec 171 comparison.

The analyzer intentionally treats a fixed single-provider gRPC run as a
diagnostic control and uses sequential four-provider gRPC as the primary fair
baseline. It rejects incomplete cells, trace-phase mismatches, and a trace
hash mismatch before producing a figure.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
from typing import Any, Iterable


SYSTEM_ORDER = ("grpc-single", "grpc", "ndnsf")
SYSTEM_LABELS = {
    "grpc-single": "gRPC-1 (no failover)",
    "grpc": "gRPC-SEQ-4",
    "ndnsf": "NDNSF",
}
BOOTSTRAP_SEED = 171
BOOTSTRAP_SAMPLES = 20_000


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def _number(value: Any, *, name: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{name} is not numeric: {value!r}") from error
    if not math.isfinite(result):
        raise ValueError(f"{name} is not finite: {value!r}")
    return result


def _summary_row(record: dict[str, Any]) -> dict[str, Any]:
    summary = record.get("summary")
    if not isinstance(summary, dict):
        raise ValueError(f"record has no summary: {record.get('system')}")
    sent = int(summary.get("sent", 0))
    success = int(summary.get("success", 0))
    if sent <= 0 or success < 0 or success > sent:
        raise ValueError(f"invalid request counts in {record.get('system')}")
    row = {
        "seed": int(record["seed"]),
        "condition": str(record["condition"]),
        "system": str(record["system"]),
        "system_label": SYSTEM_LABELS.get(str(record["system"]), str(record["system"])),
        "sent": sent,
        "success": success,
        "success_rate": success / sent,
        "deadline_failures": int(summary.get("deadline_failures", sent - success)),
        "mean_ms": _number(summary.get("mean_ms", 0.0), name="mean_ms") if success else None,
        "p50_ms": _number(summary.get("p50_ms", 0.0), name="p50_ms") if success else None,
        "p95_ms": _number(summary.get("p95_ms", 0.0), name="p95_ms") if success else None,
        "p99_ms": _number(summary.get("p99_ms", 0.0), name="p99_ms") if success else None,
        "attempts": int(summary.get("attempts", 0)),
        "failovers": int(summary.get("failovers", 0)),
        "provider_executions": int(summary.get(
            "provider_executions", summary.get("handler_executions_observed", 0)) or 0),
        "traffic_launch_offset_s": _number(
            summary.get("traffic_launch_offset_s"), name="traffic_launch_offset_s"),
        "measurement_start_lateness_ms": _number(
            summary.get("measurement_start_lateness_ms"),
            name="measurement_start_lateness_ms"),
        "trace_sha256": None,
        "trace_metrics": record.get("trace_metrics"),
    }
    return row


def _manifest_metadata(record: dict[str, Any]) -> tuple[str, float]:
    path = Path(str(record.get("cell_manifest", "")))
    if not path.is_file():
        raise ValueError(f"cell manifest is missing: {path}")
    manifest = read_json(path)
    trace_sha = str(manifest.get("trace_sha256", ""))
    if not trace_sha:
        raise ValueError(f"manifest has no trace hash: {path}")
    try:
        phase = float(manifest["traffic_start_delay_s"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"manifest has no traffic phase: {path}") from error
    return trace_sha, phase


def validate_records(aggregate: dict[str, Any], *, condition: str | None = None,
                     expected_systems: Iterable[str] = SYSTEM_ORDER) -> list[dict[str, Any]]:
    records = [item for item in aggregate.get("records", [])
               if isinstance(item, dict) and item.get("status") == "complete"]
    if not records:
        raise ValueError("aggregate has no complete records")
    conditions = sorted({str(item.get("condition")) for item in records})
    selected = condition or (conditions[0] if len(conditions) == 1 else None)
    if selected is None:
        raise ValueError(f"aggregate contains multiple conditions; pass --condition: {conditions}")
    records = [item for item in records if item.get("condition") == selected]
    expected = set(expected_systems)
    systems = {str(item.get("system")) for item in records}
    if systems != expected:
        raise ValueError(f"expected systems {sorted(expected)}, found {sorted(systems)}")
    by_seed: dict[int, dict[str, dict[str, Any]]] = {}
    rows: list[dict[str, Any]] = []
    for record in records:
        row = _summary_row(record)
        if not bool(record.get("trace_source_match")):
            raise ValueError(f"trace source mismatch: seed={row['seed']} system={row['system']}")
        if not bool(record.get("request_count_match")):
            raise ValueError(f"request count mismatch: seed={row['seed']} system={row['system']}")
        if not bool(record.get("traffic_phase_match")):
            raise ValueError(f"traffic phase mismatch: seed={row['seed']} system={row['system']}")
        trace_sha, phase = _manifest_metadata(record)
        if abs(row["traffic_launch_offset_s"] - phase) > 0.05:
            raise ValueError(f"summary/manifest phase mismatch: seed={row['seed']} system={row['system']}")
        row["trace_sha256"] = trace_sha
        by_seed.setdefault(row["seed"], {})[row["system"]] = row
        rows.append(row)
    if not by_seed:
        raise ValueError("selected condition has no complete records")
    for seed, systems_for_seed in by_seed.items():
        if set(systems_for_seed) != expected:
            raise ValueError(f"seed {seed} is not complete: {sorted(systems_for_seed)}")
        hashes = {row["trace_sha256"] for row in systems_for_seed.values()}
        if len(hashes) != 1:
            raise ValueError(f"trace hashes differ at seed {seed}")
        metrics = [row.get("trace_metrics") for row in systems_for_seed.values()]
        if any(not isinstance(item, dict) for item in metrics):
            raise ValueError(f"measurement coverage metrics missing at seed {seed}")
        if any(item != metrics[0] for item in metrics[1:]):
            raise ValueError(f"measurement coverage metrics differ at seed {seed}")
    return sorted(rows, key=lambda row: (row["seed"], SYSTEM_ORDER.index(row["system"])))


def bootstrap_interval(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"mean": None, "lower": None, "upper": None, "values": []}
    rng = random.Random(BOOTSTRAP_SEED)
    samples = [
        statistics.mean(values[rng.randrange(len(values))] for _ in values)
        for _ in range(BOOTSTRAP_SAMPLES)
    ]
    ordered = sorted(samples)
    return {
        "mean": statistics.mean(values),
        "lower": ordered[int(0.025 * len(ordered))],
        "upper": ordered[int(0.975 * len(ordered))],
        "values": values,
    }


def _paired(rows: list[dict[str, Any]], baseline: str) -> dict[str, Any]:
    grouped: dict[int, dict[str, dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault(row["seed"], {})[row["system"]] = row
    success_diff: list[float] = []
    mean_ratios: list[float] = []
    p95_ratios: list[float] = []
    for seed in sorted(grouped):
        treatment = grouped[seed]["ndnsf"]
        control = grouped[seed][baseline]
        success_diff.append(treatment["success_rate"] - control["success_rate"])
        if treatment["mean_ms"] is not None and control["mean_ms"] is not None:
            mean_ratios.append(treatment["mean_ms"] / control["mean_ms"])
        if treatment["p95_ms"] is not None and control["p95_ms"] is not None:
            p95_ratios.append(treatment["p95_ms"] / control["p95_ms"])
    return {
        "success_difference": bootstrap_interval(success_diff),
        "mean_latency_ratio": bootstrap_interval(mean_ratios),
        "p95_latency_ratio": bootstrap_interval(p95_ratios),
    }


def _coverage(rows: list[dict[str, Any]]) -> dict[str, Any]:
    by_seed = {row["seed"]: row["trace_metrics"]["measurement_window"]
               for row in rows if row["system"] == "ndnsf"}
    keys = ("at_least_one_fraction", "all_unreachable_fraction", "at_least_two_fraction")
    result = {key: statistics.mean(float(item[key]) for item in by_seed.values())
              for key in keys}
    result["per_seed"] = by_seed
    return result


def summarize(rows: list[dict[str, Any]], *, aggregate_path: Path) -> dict[str, Any]:
    systems: dict[str, dict[str, Any]] = {}
    for system in SYSTEM_ORDER:
        selected = [row for row in rows if row["system"] == system]
        total_sent = sum(row["sent"] for row in selected)
        total_success = sum(row["success"] for row in selected)
        successful = [row for row in selected if row["success"] > 0]
        systems[system] = {
            "label": SYSTEM_LABELS[system],
            "requests": total_sent,
            "success": total_success,
            "success_rate": total_success / total_sent,
            "mean_success_latency_ms": (
                sum(row["mean_ms"] * row["success"] for row in successful) / total_success
                if total_success else None),
            "mean_p50_ms": statistics.mean(row["p50_ms"] for row in successful) if successful else None,
            "mean_p95_ms": statistics.mean(row["p95_ms"] for row in successful) if successful else None,
            "mean_p99_ms": statistics.mean(row["p99_ms"] for row in successful) if successful else None,
            "attempts_per_request": sum(row["attempts"] for row in selected) / total_sent,
            "failovers_per_request": sum(row["failovers"] for row in selected) / total_sent,
            "provider_executions_per_request": (
                sum(row["provider_executions"] for row in selected) / total_sent),
            "per_seed": selected,
        }
    paired = {
        "grpc-single": _paired(rows, "grpc-single"),
        "grpc": _paired(rows, "grpc"),
    }
    gate = {
        "control_success_lower_positive": (
            paired["grpc-single"]["success_difference"]["lower"] is not None and
            paired["grpc-single"]["success_difference"]["lower"] > 0),
        "sequential_success_noninferior": (
            paired["grpc"]["success_difference"]["lower"] is not None and
            paired["grpc"]["success_difference"]["lower"] >= -0.05),
        "mean_latency_ratio_upper_below_one": (
            paired["grpc"]["mean_latency_ratio"]["upper"] is not None and
            paired["grpc"]["mean_latency_ratio"]["upper"] < 1.0),
        "p95_latency_ratio_upper_below_one": (
            paired["grpc"]["p95_latency_ratio"]["upper"] is not None and
            paired["grpc"]["p95_latency_ratio"]["upper"] < 1.0),
    }
    gate["confirmed_conditional_advantage"] = all(gate.values())
    registration = aggregate_path.parent / "registration.json"
    return {
        "schema": "ndnsf-mobility-comparison-analysis-v1",
        "aggregate": str(aggregate_path.resolve()),
        "aggregate_sha256": sha256_file(aggregate_path),
        "registration": str(registration.resolve()) if registration.is_file() else None,
        "registration_sha256": sha256_file(registration) if registration.is_file() else None,
        "condition": rows[0]["condition"],
        "systems": systems,
        "paired": paired,
        "coverage": _coverage(rows),
        "claim_gate": gate,
        "claim_verdict": (
            "CONFIRMED_CONDITIONAL_ADVANTAGE"
            if gate["confirmed_conditional_advantage"]
            else "NO_HOLDOUT_CONFIRMATION"),
        "rows": rows,
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    columns = [
        "seed", "condition", "system", "sent", "success", "success_rate",
        "deadline_failures", "mean_ms", "p50_ms", "p95_ms", "p99_ms",
        "attempts", "failovers", "provider_executions",
        "traffic_launch_offset_s", "measurement_start_lateness_ms", "trace_sha256",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key) for key in columns})


def write_markdown(path: Path, result: dict[str, Any]) -> None:
    systems = result["systems"]
    paired = result["paired"]
    lines = [
        "# Mobility comparison analysis",
        "",
        f"- Condition: `{result['condition']}`",
        f"- Claim verdict: `{result['claim_verdict']}`",
        f"- Aggregate SHA-256: `{result['aggregate_sha256']}`",
        "",
        "| System | Success | Mean successful latency (ms) | Mean p95 (ms) | Attempts/executions per request |",
        "|---|---:|---:|---:|---:|",
    ]
    for system in SYSTEM_ORDER:
        item = systems[system]
        cost = (item["provider_executions_per_request"]
                if system == "ndnsf" else item["attempts_per_request"])
        lines.append(
            f"| {item['label']} | {item['success_rate'] * 100:.2f}% | "
            f"{item['mean_success_latency_ms']:.2f} | {item['mean_p95_ms']:.2f} | {cost:.3f} |")
    lines.extend([
        "",
        "Successful-response latency is conditional on a response; success rate and deadline misses are reported separately.",
        "",
        "## Seed-level paired success",
        "",
        "The 300 requests within a seed share one mobility/coverage trace; they are not treated as independent seed replicates.",
        "",
        "| Seed | NDNSF | gRPC-SEQ-4 | NDNSF minus gRPC (pp) |",
        "|---:|---:|---:|---:|",
    ])
    grpc_by_seed = {row["seed"]: row for row in systems["grpc"]["per_seed"]}
    ndnsf_by_seed = {row["seed"]: row for row in systems["ndnsf"]["per_seed"]}
    for seed in sorted(ndnsf_by_seed):
        ndnsf_rate = ndnsf_by_seed[seed]["success_rate"] * 100
        grpc_rate = grpc_by_seed[seed]["success_rate"] * 100
        lines.append(
            f"| {seed} | {ndnsf_rate:.2f}% | {grpc_rate:.2f}% | "
            f"{ndnsf_rate - grpc_rate:+.2f} |")
    lines.extend([
        "",
        "## Paired gates",
        "",
        "| Gate | Result |",
        "|---|---|",
    ])
    for key, value in result["claim_gate"].items():
        lines.append(f"| `{key}` | `{value}` |")
    lines.extend([
        "",
        "## Paired bootstrap intervals",
        "",
        "| Baseline | Success difference mean [95% interval] | Mean latency ratio mean [95% interval] | p95 ratio mean [95% interval] |",
        "|---|---:|---:|---:|",
    ])
    for baseline in ("grpc-single", "grpc"):
        item = paired[baseline]
        def fmt(name: str) -> str:
            value = item[name]
            return f"{value['mean']:.4f} [{value['lower']:.4f}, {value['upper']:.4f}]"
        lines.append(f"| {SYSTEM_LABELS[baseline]} | {fmt('success_difference')} | "
                     f"{fmt('mean_latency_ratio')} | {fmt('p95_latency_ratio')} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_figure(paths: Iterable[Path], result: dict[str, Any]) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    systems = list(SYSTEM_ORDER)
    labels = [SYSTEM_LABELS[item] for item in systems]
    colors = ["#6b7280", "#2563eb", "#e07a16"]
    summaries = result["systems"]
    fig, axes = plt.subplots(1, 3, figsize=(13.2, 4.4), constrained_layout=True)

    def panel_bars(ax, values, title, ylabel, *, log=False):
        x = list(range(len(systems)))
        ax.bar(x, values, color=colors, width=0.62, edgecolor="#1f2937", linewidth=0.5)
        for index, system in enumerate(systems):
            per_seed = [row[values_key] for row in summaries[system]["per_seed"]
                        if row[values_key] is not None]
            if per_seed:
                jitter = [index + (offset - (len(per_seed) - 1) / 2) * 0.055
                          for offset in range(len(per_seed))]
                ax.scatter(jitter, per_seed, color="#111827", s=18, zorder=3)
        ax.set_title(title, fontsize=10)
        ax.set_ylabel(ylabel, fontsize=9)
        ax.set_xticks(x, labels, rotation=18, ha="right", fontsize=8)
        ax.grid(axis="y", color="#d1d5db", linewidth=0.6, alpha=0.8)
        ax.set_axisbelow(True)
        if log:
            ax.set_yscale("log")

    values_key = "success_rate"
    values = [summaries[item][values_key] * 100 for item in systems]
    panel_bars(axes[0], values, "A  Logical success", "Success rate (%)")
    axes[0].set_ylim(0, 105)
    # Link the paired NDNSF and sequential-gRPC observations for each seed.
    # The pooled bars are descriptive; these links make trace-to-trace
    # heterogeneity visible instead of implying 1,500 independent trials.
    by_seed = {}
    for system in systems:
        for row in summaries[system]["per_seed"]:
            by_seed.setdefault(row["seed"], {})[system] = row["success_rate"] * 100
    for offset, seed in enumerate(sorted(by_seed)):
        jitter = (offset - (len(by_seed) - 1) / 2) * 0.055
        pair = by_seed[seed]
        axes[0].plot(
            [1 + jitter, 2 + jitter],
            [pair["grpc"], pair["ndnsf"]],
            color="#9ca3af", linewidth=0.9, alpha=0.75, zorder=2,
        )
    paired_success = result["paired"]["grpc"]["success_difference"]
    axes[0].text(
        0.02, 0.98,
        "paired NDNSF−gRPC: "
        f"{paired_success['mean'] * 100:+.2f} pp mean\n"
        f"range {min(paired_success['values']) * 100:+.2f}…"
        f"{max(paired_success['values']) * 100:+.2f} pp",
        transform=axes[0].transAxes, va="top", fontsize=7.5,
        color="#374151", bbox={"facecolor": "white", "alpha": 0.8,
                                "edgecolor": "none", "pad": 2.0},
    )

    values_key = "mean_ms"
    values = [summaries[item]["mean_success_latency_ms"] for item in systems]
    panel_bars(axes[1], values, "B  Mean successful latency", "Latency (ms; log scale)", log=True)

    values_key = "p95_ms"
    p95_systems = ("grpc", "ndnsf")
    p95_labels = [SYSTEM_LABELS[item] for item in p95_systems]
    p95_colors = [colors[1], colors[2]]
    p95_values = [summaries[item]["mean_p95_ms"] for item in p95_systems]
    axes[2].bar(range(2), p95_values, color=p95_colors, width=0.62,
                edgecolor="#1f2937", linewidth=0.5)
    for index, system in enumerate(p95_systems):
        per_seed = [row["p95_ms"] for row in summaries[system]["per_seed"]
                    if row["p95_ms"] is not None]
        jitter = [index + (offset - (len(per_seed) - 1) / 2) * 0.055
                  for offset in range(len(per_seed))]
        axes[2].scatter(jitter, per_seed, color="#111827", s=18, zorder=3)
    axes[2].set_title("C  Tail latency (p95)", fontsize=10)
    axes[2].set_ylabel("Latency (ms; log scale)", fontsize=9)
    axes[2].set_xticks(range(2), p95_labels, rotation=18, ha="right", fontsize=8)
    axes[2].set_yscale("log")
    axes[2].grid(axis="y", color="#d1d5db", linewidth=0.6, alpha=0.8)
    axes[2].set_axisbelow(True)

    fig.suptitle("Four-provider mobility: native selection versus endpoint failover",
                 fontsize=13, fontweight="bold")
    fig.text(0.5, -0.025,
             "Bars are pooled across holdout seeds; dots are per-seed values. "
             "Lines pair NDNSF with gRPC-SEQ-4 by seed. Latency uses successful "
             "responses; gRPC-1 is a fixed-endpoint diagnostic.",
             ha="center", fontsize=8.5, color="#374151")
    for path in paths:
        fig.savefig(path, dpi=220, bbox_inches="tight")
    plt.close(fig)


def write_outputs(result: dict[str, Any], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "analysis.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_csv(output_dir / "per-seed.csv", result["rows"])
    write_markdown(output_dir / "README.md", result)
    write_figure(
        (output_dir / "mobility-comparison.png",
         output_dir / "mobility-comparison.svg",
         output_dir / "mobility-comparison.pdf"),
        result,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--aggregate", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--condition", default=None)
    args = parser.parse_args()
    aggregate = read_json(args.aggregate)
    rows = validate_records(aggregate, condition=args.condition)
    result = summarize(rows, aggregate_path=args.aggregate)
    write_outputs(result, args.output_dir)
    print(json.dumps({
        "claim_verdict": result["claim_verdict"],
        "condition": result["condition"],
        "seeds": sorted({row["seed"] for row in rows}),
        "output_dir": str(args.output_dir.resolve()),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
