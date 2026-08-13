#!/usr/bin/env python3
"""Render the scoped Spec 171 Provider-discovery and switching evidence."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


SYSTEM_ORDER = [
    "ndnsf",
    "grpc-static-3",
    "grpc-preregistered-4",
    "nsc-static-3",
    "nsc-preregistered-4",
]
SYSTEM_LABELS = {
    "ndnsf": "NDNSF\nservice only",
    "grpc-static-3": "gRPC\nstatic 3",
    "grpc-preregistered-4": "gRPC\npre-registered 4",
    "nsc-static-3": "NSC\nstatic 3",
    "nsc-preregistered-4": "NSC\npre-registered 4",
}
SYSTEM_COLORS = {
    "ndnsf": "#225ea8",
    "grpc-static-3": "#f3b562",
    "grpc-preregistered-4": "#d97706",
    "nsc-static-3": "#a8adb5",
    "nsc-preregistered-4": "#60656f",
}


def load_evidence(transition_path: Path, opportunity_path: Path) -> dict:
    transition = json.loads(transition_path.read_text(encoding="utf-8"))
    opportunity = json.loads(opportunity_path.read_text(encoding="utf-8"))
    if not transition.get("sc014_passed") or len(transition["replays"]) < 3:
        raise ValueError("Provider-transition evidence has not passed SC-014")
    if opportunity.get("verdict") != "CONDITIONAL_SWITCHING_COST_REDUCTION":
        raise ValueError("unexpected opportunity-analysis verdict")

    systems = {}
    for system in SYSTEM_ORDER:
        phases = [
            replay["cells"][system]["phases"]["post_retirement"]
            for replay in transition["replays"]
        ]
        configured = {
            replay["cells"][system]["configured_provider_count"]
            for replay in transition["replays"]
        }
        if len(configured) != 1:
            raise ValueError(f"configured Provider count changed for {system}")
        requests = sum(int(phase["requests"]) for phase in phases)
        success = sum(int(phase["success"]) for phase in phases)
        p95_values = [
            float(phase["p95_success_latency_ms"])
            for phase in phases if phase["p95_success_latency_ms"] is not None
        ]
        systems[system] = {
            "configured_provider_count": configured.pop(),
            "requests": requests,
            "success": success,
            "success_rate_pct": 100.0 * success / requests,
            "provider_d_successes": sum(
                int(phase["provider_d_successes"]) for phase in phases),
            "attempts_or_executions": sum(
                int(phase["attempts_or_executions"]) for phase in phases),
            "median_replay_p95_latency_ms": (
                statistics.median(p95_values) if p95_values else None),
        }

    per_seed = []
    for item in opportunity["per_seed"]:
        per_seed.append({
            "seed": int(item["seed"]),
            "switch_required": int(item["switch_required"]),
            "ndnsf_selection_cost_p95_ms": float(
                item["ndnsf_selection_cost_p95_ms"]),
            "grpc_failed_attempt_cost_p95_ms": float(
                item["grpc_failed_attempt_cost_p95_ms"]),
        })
    paired = opportunity["paired_seed_p95_reduction_ms"]
    return {
        "schema": "spec171-provider-evidence-figure-data-v1",
        "transition_replays": len(transition["replays"]),
        "post_retirement_systems": systems,
        "opportunity_seeds": per_seed,
        "switch_required_requests": sum(item["switch_required"] for item in per_seed),
        "paired_seed_p95_reduction_ms": paired,
        "opportunity_metric_note": opportunity["switching_cost_definition"],
        "ndnsf_user_latency_available": opportunity["ndnsf_user_latency_available"],
    }


def write_figure(data: dict, output_dir: Path) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "figure-data.json").write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    fig, axes = plt.subplots(1, 2, figsize=(12.8, 5.25),
                             gridspec_kw={"width_ratios": [1.15, 1]})
    fig.subplots_adjust(left=0.07, right=0.98, top=0.83, bottom=0.24, wspace=0.29)
    fig.suptitle("Provider discovery and conditional switching cost",
                 fontsize=17, fontweight="bold")

    ax = axes[0]
    systems = data["post_retirement_systems"]
    x = list(range(len(SYSTEM_ORDER)))
    values = [systems[name]["success_rate_pct"] for name in SYSTEM_ORDER]
    bars = ax.bar(
        x, values, width=0.68,
        color=[SYSTEM_COLORS[name] for name in SYSTEM_ORDER],
        edgecolor="#222222", linewidth=0.6)
    ax.set_title("A. After only previously omitted Provider D remains",
                 fontsize=11.5, fontweight="bold")
    ax.set_ylabel("Request success rate (%)")
    ax.set_ylim(0, 112)
    ax.set_yticks([0, 25, 50, 75, 100])
    ax.set_xticks(x, [SYSTEM_LABELS[name] for name in SYSTEM_ORDER], fontsize=8.5)
    ax.grid(axis="y", color="#d8dce3", linewidth=0.7)
    ax.set_axisbelow(True)
    for bar, name, value in zip(bars, SYSTEM_ORDER, values):
        cell = systems[name]
        value_y = value + 2 if value > 0 else 6
        ax.text(bar.get_x() + bar.get_width() / 2, value_y,
                f"{cell['success']}/{cell['requests']}",
                ha="center", va="bottom", fontsize=8.5, fontweight="bold")
        p95 = cell["median_replay_p95_latency_ms"]
        annotation = f"endpoints={cell['configured_provider_count']}"
        if p95 is not None:
            annotation += f"\np95={p95:.0f} ms"
        annotation_y = 4 if value > 0 else 14
        ax.text(bar.get_x() + bar.get_width() / 2, annotation_y, annotation,
                ha="center", va="bottom", fontsize=7.5,
                color="white" if value >= 40 else "#333333")

    ax = axes[1]
    seed_rows = data["opportunity_seeds"]
    for row in seed_rows:
        ndnsf = row["ndnsf_selection_cost_p95_ms"]
        grpc = row["grpc_failed_attempt_cost_p95_ms"]
        color = "#c2410c" if grpc > ndnsf else "#6b7280"
        ax.plot([0, 1], [ndnsf, grpc], color=color, alpha=0.62, linewidth=1.3)
        ax.scatter([0, 1], [ndnsf, grpc], color=["#225ea8", "#d97706"],
                   s=28, zorder=3, edgecolor="white", linewidth=0.4)
    ax.set_title("B. Natural-mobility SWITCH_REQUIRED requests",
                 fontsize=11.5, fontweight="bold")
    ax.set_ylabel("Seed p95 pre-execution switching cost (ms)")
    ax.set_yscale("log")
    ax.set_ylim(0.4, 3000)
    ax.set_xticks([0, 1], ["NDNSF\nrequest→selection",
                           "gRPC\nfailed attempts before success"])
    ax.grid(axis="y", which="both", color="#d8dce3", linewidth=0.7)
    ax.set_axisbelow(True)
    paired = data["paired_seed_p95_reduction_ms"]
    ax.text(
        0.03, 0.97,
        f"n={len(seed_rows)} mobility seeds; "
        f"{data['switch_required_requests']:,} switch windows\n"
        f"Mean paired reduction: {paired['mean']:.1f} ms\n"
        f"95% bootstrap CI: [{paired['ci95_low']:.1f}, "
        f"{paired['ci95_high']:.1f}] ms",
        transform=ax.transAxes, ha="left", va="top", fontsize=8.5,
        bbox={"boxstyle": "round,pad=0.35", "facecolor": "white",
              "edgecolor": "#c7cbd1", "alpha": 0.92})

    fig.text(
        0.5, 0.055,
        f"A: three independent 60 s replays, "
        f"{systems['ndnsf']['requests']} steady post-retirement requests per system; "
        "health/resolver routing disabled.  B: paired seed-level p95 stage metrics; "
        "gRPC fast-fail seeds can be faster.",
        ha="center", va="bottom", fontsize=8.5, color="#3f4650")

    paths = []
    for suffix in ("png", "pdf", "svg"):
        path = output_dir / f"provider-discovery-and-switching.{suffix}"
        fig.savefig(path, dpi=300 if suffix == "png" else None,
                    bbox_inches="tight", facecolor="white")
        paths.append(path)
    plt.close(fig)
    return paths


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--transition-summary", type=Path, required=True)
    parser.add_argument("--opportunity-summary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    data = load_evidence(args.transition_summary, args.opportunity_summary)
    paths = write_figure(data, args.output_dir)
    print(json.dumps({
        "outputs": [str(path.resolve()) for path in paths],
        "figure_data": str((args.output_dir / "figure-data.json").resolve()),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
