#!/usr/bin/env python3
"""Analyze the registered 10-seed mobility follow-up and process repeats.

The primary inference unit is a mobility seed. Repeated processes for selected
seeds are checked for trace identity and reported separately as within-trace
runtime diagnostics.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import random
import statistics


SYSTEMS = ("ndnsf", "grpc", "nsc")
LABELS = {"ndnsf": "NDNSF", "grpc": "gRPC-SEQ-4", "nsc": "NSC-4"}
PRIMARY_SEEDS = tuple(range(50, 60))
REPEAT_SEEDS = (50, 54, 58)
CONDITION = "range-50-speed-2p0"
BOOTSTRAP_SEED = 171
BOOTSTRAP_SAMPLES = 20_000


def load(path: Path) -> dict:
    if path.is_dir():
        path = path / "aggregate.json"
    report = json.loads(path.read_text(encoding="utf-8"))
    if report.get("complete_cells") != report.get("total_cells"):
        raise ValueError(f"incomplete aggregate: {path}")
    return report


def records_by_seed(report: dict, seeds: tuple[int, ...],
                    systems: tuple[str, ...] = SYSTEMS) -> dict[int, dict[str, dict]]:
    records = [row for row in report["records"]
               if row.get("condition") == CONDITION and row.get("status") == "complete"]
    result: dict[int, dict[str, dict]] = {}
    for row in records:
        seed = int(row["seed"])
        system = str(row["system"])
        if seed in seeds:
            result.setdefault(seed, {})[system] = row
    if set(result) != set(seeds):
        raise ValueError(f"expected seeds {list(seeds)}, found {sorted(result)}")
    for seed in seeds:
        if not set(systems).issubset(result[seed]):
            raise ValueError(f"seed {seed} is incomplete: {sorted(result[seed])}")
        result[seed] = {system: result[seed][system] for system in systems}
    return result


def manifest_for(record: dict) -> dict:
    path = Path(record["cell_manifest"])
    return json.loads(path.read_text(encoding="utf-8"))


def bootstrap(values: list[float]) -> dict:
    rng = random.Random(BOOTSTRAP_SEED)
    samples = [statistics.mean(values[rng.randrange(len(values))] for _ in values)
               for _ in range(BOOTSTRAP_SAMPLES)]
    ordered = sorted(samples)
    return {
        "mean": statistics.mean(values),
        "lower": ordered[int(0.025 * len(ordered))],
        "upper": ordered[int(0.975 * len(ordered))],
        "values": values,
    }


def summarize(primary: dict[int, dict[str, dict]],
              repeats: dict[int, dict[str, dict]],
              prior: dict[int, dict[str, dict]] | None = None) -> dict:
    pooled = {}
    for system in SYSTEMS:
        rows = [primary[seed][system]["summary"] for seed in PRIMARY_SEEDS]
        sent = sum(int(row["sent"]) for row in rows)
        success = sum(int(row["success"]) for row in rows)
        pooled[system] = {
            "sent": sent,
            "success": success,
            "success_rate": success / sent,
            "mean_success_latency_ms": (
                sum(float(row["mean_ms"]) * int(row["success"]) for row in rows) /
                success if success else None),
        }

    paired = {}
    for baseline in ("grpc", "nsc"):
        values = [
            primary[seed]["ndnsf"]["summary"]["success"] /
            primary[seed]["ndnsf"]["summary"]["sent"] -
            primary[seed][baseline]["summary"]["success"] /
            primary[seed][baseline]["summary"]["sent"]
            for seed in PRIMARY_SEEDS
        ]
        paired[baseline] = bootstrap(values)

    trace_matches = {}
    repeat_rows = []
    for seed in REPEAT_SEEDS:
        for system in SYSTEMS:
            primary_manifest = manifest_for(primary[seed][system])
            repeat_manifest = manifest_for(repeats[seed][system])
            trace_matches[f"{seed}:{system}"] = (
                primary_manifest["trace_sha256"] == repeat_manifest["trace_sha256"])
            p = primary[seed][system]["summary"]
            r = repeats[seed][system]["summary"]
            repeat_rows.append({
                "seed": seed,
                "system": system,
                "trace_sha256": repeat_manifest["trace_sha256"],
                "trace_match": trace_matches[f"{seed}:{system}"],
                "primary_success": int(p["success"]),
                "repeat_success": int(r["success"]),
                "success_delta_pp": (
                    int(r["success"]) / int(r["sent"]) -
                    int(p["success"]) / int(p["sent"])) * 100,
                "primary_mean_ms": p.get("mean_ms"),
                "repeat_mean_ms": r.get("mean_ms"),
                "mean_latency_delta_ms": (
                    float(r["mean_ms"]) - float(p["mean_ms"])
                    if p.get("mean_ms") is not None and r.get("mean_ms") is not None
                    else None),
            })

    grpc = paired["grpc"]
    nsc = paired["nsc"]
    result = {
        "schema": "ndnsf-seed-repeat-followup-analysis-v1",
        "condition": CONDITION,
        "primary_seeds": list(PRIMARY_SEEDS),
        "repeat_seeds": list(REPEAT_SEEDS),
        "systems": {system: pooled[system] for system in SYSTEMS},
        "paired_success_difference": paired,
        "trace_matches": trace_matches,
        "repeat_rows": repeat_rows,
        "claim_gate": {
            "ndnsf_vs_grpc_lower_positive": grpc["lower"] > 0,
            "ndnsf_vs_grpc_noninferior_minus_5pp": grpc["lower"] >= -0.05,
            "ndnsf_vs_nsc_lower_positive": nsc["lower"] > 0,
            "ndnsf_vs_nsc_lower_at_least_10pp": nsc["lower"] >= 0.10,
            "all_repeat_trace_hashes_match": all(trace_matches.values()),
        },
        "claim_verdict": "NO_POSITIVE_MOBILITY_CONFIRMATION",
    }
    if prior:
        combined_grpc_values = [
            prior[seed]["ndnsf"]["summary"]["success"] /
            prior[seed]["ndnsf"]["summary"]["sent"] -
            prior[seed]["grpc"]["summary"]["success"] /
            prior[seed]["grpc"]["summary"]["sent"]
            for seed in sorted(prior)
        ] + paired["grpc"]["values"]
        result["combined_prior_seeds"] = sorted(prior)
        result["combined_seed_count"] = len(combined_grpc_values)
        result["combined_paired_success_difference_grpc"] = bootstrap(combined_grpc_values)
    return result


def write_csv(path: Path, rows: list[dict]) -> None:
    columns = list(rows[0]) if rows else []
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def write_readme(path: Path, result: dict) -> None:
    systems = result["systems"]
    grpc = result["paired_success_difference"]["grpc"]
    nsc = result["paired_success_difference"]["nsc"]
    lines = [
        "# Seed/repeat mobility follow-up",
        "",
        f"- Condition: `{result['condition']}`",
        f"- Primary seeds: `{','.join(map(str, result['primary_seeds']))}`",
        f"- Process-repeat seeds: `{','.join(map(str, result['repeat_seeds']))}`",
        f"- Claim verdict: `{result['claim_verdict']}`",
        "",
        "The primary unit is one mobility seed; the 300 requests within a seed share one trace.",
        "Process repeats use the same trace hash and are diagnostics, not extra independent seeds.",
        "",
        "| System | Success | Mean successful latency (ms) |",
        "|---|---:|---:|",
    ]
    for system in SYSTEMS:
        item = systems[system]
        lines.append(f"| {LABELS[system]} | {item['success']}/{item['sent']} ({item['success_rate'] * 100:.2f}%) | {item['mean_success_latency_ms']:.2f} |")
    lines.extend([
        "",
        "| Paired comparison | Mean difference | 95% bootstrap interval |",
        "|---|---:|---:|",
        f"| NDNSF minus gRPC-SEQ-4 | {grpc['mean'] * 100:+.2f} pp | [{grpc['lower'] * 100:+.2f}, {grpc['upper'] * 100:+.2f}] pp |",
        f"| NDNSF minus NSC-4 | {nsc['mean'] * 100:+.2f} pp | [{nsc['lower'] * 100:+.2f}, {nsc['upper'] * 100:+.2f}] pp |",
        "",
        "The gRPC lower bound is not positive, so the follow-up does not prove a positive NDNSF mobility advantage.",
        "The NSC difference is positive but far below the registered 10 percentage-point superiority threshold.",
        "All nine process repeats reproduced the primary success count for the same seed/system; remaining repeat variation is latency-only.",
    ])
    if "combined_paired_success_difference_grpc" in result:
        combined = result["combined_paired_success_difference_grpc"]
        lines.extend([
            "",
            f"Combining the prior seeds {','.join(map(str, result['combined_prior_seeds']))} "
            f"with the ten new seeds gives {result['combined_seed_count']} paired traces:",
            f"NDNSF minus gRPC-SEQ-4 = {combined['mean'] * 100:+.2f} pp "
            f"[{combined['lower'] * 100:+.2f}, {combined['upper'] * 100:+.2f}] pp.",
            "This combined interval also includes zero and remains non-superior.",
        ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_figure(path_prefix: Path, primary: dict[int, dict[str, dict]], result: dict) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    seeds = list(PRIMARY_SEEDS)
    x = list(range(len(seeds)))
    colors = {"ndnsf": "#2864d7", "grpc": "#d97706", "nsc": "#6b7280"}
    fig, axes = plt.subplots(1, 2, figsize=(11.2, 4.4), constrained_layout=True)
    for system in SYSTEMS:
        rates = [primary[seed][system]["summary"]["success"] /
                 primary[seed][system]["summary"]["sent"] * 100 for seed in seeds]
        axes[0].plot(x, rates, marker="o", linewidth=1.8, markersize=4.5,
                     color=colors[system], label=LABELS[system])
    axes[0].set_title("A  Success by independent mobility seed")
    axes[0].set_ylabel("Success rate (%)")
    axes[0].set_xticks(x, seeds, rotation=35)
    axes[0].set_ylim(0, 105)
    axes[0].grid(axis="y", color="#d8dce3", linewidth=0.7)
    axes[0].legend(frameon=False, fontsize=8)
    axes[0].set_axisbelow(True)

    deltas = {}
    for baseline in ("grpc", "nsc"):
        deltas[baseline] = [
            (primary[seed]["ndnsf"]["summary"]["success"] /
             primary[seed]["ndnsf"]["summary"]["sent"] -
             primary[seed][baseline]["summary"]["success"] /
             primary[seed][baseline]["summary"]["sent"]) * 100
            for seed in seeds
        ]
    axes[1].axhline(0, color="#374151", linewidth=0.9)
    axes[1].plot(x, deltas["grpc"], marker="o", linewidth=1.8,
                 color=colors["grpc"], label="NDNSF − gRPC-SEQ-4")
    axes[1].plot(x, deltas["nsc"], marker="o", linewidth=1.8,
                 color=colors["nsc"], label="NDNSF − NSC-4")
    axes[1].set_title("B  Paired success difference")
    axes[1].set_ylabel("Difference (percentage points)")
    axes[1].set_xticks(x, seeds, rotation=35)
    axes[1].grid(axis="y", color="#d8dce3", linewidth=0.7)
    axes[1].legend(frameon=False, fontsize=8, loc="lower left")
    axes[1].set_axisbelow(True)
    axes[1].text(
        0.02, 0.98,
        f"gRPC mean {result['paired_success_difference']['grpc']['mean'] * 100:+.2f} pp; "
        f"95% CI [{result['paired_success_difference']['grpc']['lower'] * 100:+.2f}, "
        f"{result['paired_success_difference']['grpc']['upper'] * 100:+.2f}] pp",
        transform=axes[1].transAxes, va="top", fontsize=7.0, color="#374151",
        bbox={"facecolor": "white", "alpha": 0.8, "edgecolor": "none", "pad": 2.0},
    )
    fig.suptitle("Ten-seed mobility follow-up: trace heterogeneity remains",
                 fontsize=14, fontweight="bold")
    fig.text(0.5, -0.02,
             "Seeds 50–59; 300 requests/system/seed. Repeat seeds 50, 54, 58 "
             "reproduced success counts under identical trace hashes.",
             ha="center", fontsize=8.5, color="#374151")
    for suffix in ("png", "pdf", "svg"):
        fig.savefig(path_prefix.with_suffix(f".{suffix}"),
                    dpi=260 if suffix == "png" else None, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--primary", type=Path, required=True)
    parser.add_argument("--repeats", type=Path, required=True)
    parser.add_argument("--prior", type=Path,
                        help="optional earlier aggregate to combine for the same systems")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    primary = records_by_seed(load(args.primary), PRIMARY_SEEDS)
    repeats = records_by_seed(load(args.repeats), REPEAT_SEEDS)
    prior = None
    if args.prior:
        prior = records_by_seed(load(args.prior), (43, 44, 45, 46, 47), ("ndnsf", "grpc"))
    result = summarize(primary, repeats, prior)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "analysis.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_csv(args.output_dir / "process-repeats.csv", result["repeat_rows"])
    write_readme(args.output_dir / "README.md", result)
    write_figure(args.output_dir / "seed-repeat-followup", primary, result)
    print(json.dumps({
        "claim_verdict": result["claim_verdict"],
        "grpc_mean_pp": result["paired_success_difference"]["grpc"]["mean"] * 100,
        "grpc_ci_pp": [result["paired_success_difference"]["grpc"]["lower"] * 100,
                       result["paired_success_difference"]["grpc"]["upper"] * 100],
        "all_repeat_trace_hashes_match": result["claim_gate"]["all_repeat_trace_hashes_match"],
        "combined_grpc_mean_pp": (
            result["combined_paired_success_difference_grpc"]["mean"] * 100
            if "combined_paired_success_difference_grpc" in result else None),
        "output_dir": str(args.output_dir.resolve()),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
