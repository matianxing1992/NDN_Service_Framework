#!/usr/bin/env python3
"""Regenerate the paper's two-layer mobility figure from frozen Spec 171 data."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402


REPO = Path(__file__).resolve().parents[1]
FIFTY_M = REPO / "results/spec171-burnin300-50m-2ms-seeds62-71-20260808/aggregate.json"
HIGHER_RANGES = REPO / "results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808/aggregate.json"
HOLDOUT = REPO / (
    "specs/171-four-provider-mobility-advantage/evidence/"
    "opportunity-holdout-results-20260809/holdout-summary.json"
)
EXPECTED_SHA256 = {
    FIFTY_M: "b42756f10b87731072f2ae385007771410a249f4f9dbfa44dc078e764d91d9b0",
    HIGHER_RANGES: "a855b1e145d3fa3b74605d7f87644b50601e7368ed4798a208f9352848d0279e",
    HOLDOUT: "efd9dec6c4a8e5c78b386f1bab12323c47beb09c660c123f2e090f178412bd38",
}
SYSTEMS = ("ndnsf", "grpc", "nsc")
LABELS = {"ndnsf": "NDNSF", "grpc": "gRPC-SEQ-4", "nsc": "NSC-SEQ-4"}
COLORS = {"ndnsf": "#2b83ba", "grpc": "#f28e52", "nsc": "#8d8d8d"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_frozen(path: Path) -> dict:
    actual = sha256(path)
    expected = EXPECTED_SHA256[path]
    if actual != expected:
        raise RuntimeError(f"frozen input hash mismatch: {path}: {actual} != {expected}")
    return json.loads(path.read_text())


def success_rates(records: list[dict], radius: int, system: str) -> np.ndarray:
    selected = sorted(
        (
            record for record in records
            if record["status"] == "complete"
            and int(record["range_m"]) == radius
            and record["system"] == system
        ),
        key=lambda record: int(record["seed"]),
    )
    if len(selected) != 10:
        raise RuntimeError(f"expected ten cells for {radius} m/{system}, got {len(selected)}")
    return np.asarray([
        float(record["summary"]["success"]) / float(record["summary"]["sent"])
        for record in selected
    ])


def bootstrap_ci(values: np.ndarray) -> tuple[float, float]:
    rng = np.random.default_rng(171)
    sample_indices = rng.integers(0, len(values), size=(20_000, len(values)))
    means = values[sample_indices].mean(axis=1)
    low, high = np.percentile(means, (2.5, 97.5))
    return float(low), float(high)


def build_figure(output_png: Path, output_pdf: Path) -> None:
    records = load_frozen(FIFTY_M)["records"] + load_frozen(HIGHER_RANGES)["records"]
    holdout = load_frozen(HOLDOUT)["paired_ndnsf_minus_baseline_p95_ms"]

    values = {
        radius: {system: success_rates(records, radius, system) for system in SYSTEMS}
        for radius in (50, 100, 150)
    }
    expected_means = {
        (50, "ndnsf"): 54.5667, (50, "grpc"): 53.8, (50, "nsc"): 55.4667,
        (100, "ndnsf"): 97.9667, (100, "grpc"): 97.9667, (100, "nsc"): 98.3333,
        (150, "ndnsf"): 100.0, (150, "grpc"): 100.0, (150, "nsc"): 100.0,
    }
    for key, expected in expected_means.items():
        actual = 100.0 * float(values[key[0]][key[1]].mean())
        if abs(actual - expected) > 0.005:
            raise RuntimeError(f"unexpected plotted mean for {key}: {actual}")

    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "font.size": 9,
        "axes.titlesize": 10,
        "axes.labelsize": 9,
        "legend.fontsize": 8,
    })
    fig, (left, right) = plt.subplots(
        1, 2, figsize=(12.75, 5.08), gridspec_kw={"width_ratios": (1.18, 1.0)}
    )
    fig.suptitle(
        "All-request controls and the registered switching-opportunity holdout",
        fontsize=14, fontweight="bold", y=0.965,
    )

    x = np.arange(3)
    width = 0.22
    offsets = (-width, 0.0, width)
    jitter = np.linspace(-0.035, 0.035, 10)
    for system, offset in zip(SYSTEMS, offsets):
        means = []
        lower = []
        upper = []
        for radius in (50, 100, 150):
            samples = values[radius][system]
            mean = 100.0 * float(samples.mean())
            low, high = bootstrap_ci(samples)
            means.append(mean)
            lower.append(mean - 100.0 * low)
            upper.append(100.0 * high - mean)
        bars = left.bar(
            x + offset, means, width, color=COLORS[system], label=LABELS[system],
            edgecolor="none", zorder=2,
        )
        left.errorbar(
            x + offset, means, yerr=np.asarray((lower, upper)), fmt="none",
            ecolor="#4f4f4f", elinewidth=1.0, capsize=3, zorder=4,
        )
        for index, radius in enumerate((50, 100, 150)):
            left.scatter(
                np.full(10, x[index] + offset) + jitter,
                100.0 * values[radius][system], s=8, color="#555555",
                alpha=0.72, linewidths=0, zorder=5,
            )
        for bar, mean in zip(bars, means):
            left.text(
                bar.get_x() + bar.get_width() / 2.0, min(104.2, mean + 2.0),
                f"{mean:.2f}", ha="center", va="bottom", fontsize=7,
            )

    left.set_title("All requests: coverage controls (seeds 62–71)", pad=8)
    left.set_ylabel("Logical success (%)")
    left.set_xlabel("AP coverage radius")
    left.set_xticks(x, ("50 m\nlow coverage", "100 m\npartial coverage", "150 m\nhigh-coverage control"))
    left.set_ylim(0, 108)
    left.grid(axis="y", color="#d9d9d9", linewidth=0.7, zorder=0)
    left.spines[["top", "right"]].set_visible(False)
    left.legend(loc="lower right", frameon=True)

    baselines = ("grpc", "nsc")
    y = np.asarray((1.0, 0.0))
    right.axvspan(-3300, 0, color="#e8f2ea", alpha=0.75, zorder=0)
    right.axvline(0, color="#333333", linestyle="--", linewidth=1.0)
    for baseline, ypos in zip(baselines, y):
        item = holdout[baseline]
        mean = float(item["mean"])
        low = float(item["ci95_low"])
        high = float(item["ci95_high"])
        right.errorbar(
            mean, ypos, xerr=[[mean - low], [high - mean]], fmt="o",
            color=COLORS[baseline], ecolor=COLORS[baseline], markersize=6,
            elinewidth=2.2, capsize=5, zorder=3,
        )
        right.text(
            mean, ypos + 0.18,
            f"{mean:,.0f} ms [{low:,.0f}, {high:,.0f}]",
            ha="center", va="bottom", fontsize=8, fontweight="bold",
        )
    right.text(-3090, 1.32, "NDNSF faster", color="#26734d", fontweight="bold")
    right.set_title("SWITCH_REQUIRED: paired tail latency (seeds 72–81)", pad=8)
    right.set_yticks(y, (LABELS["grpc"], LABELS["nsc"]))
    right.set_xlim(-3300, 450)
    right.set_ylim(-0.45, 1.55)
    right.set_xlabel("NDNSF minus baseline seed-p95 latency (ms)")
    right.grid(axis="x", color="#d9d9d9", linewidth=0.7, zorder=0)
    right.spines[["top", "right"]].set_visible(False)

    fig.text(
        0.5, 0.025,
        "Left: seed means, bootstrap 95% CI, and independent traces.  "
        "Right: registered seed-paired p95 difference across 1,312 agreed switching requests; "
        "negative favors NDNSF.",
        ha="center", va="bottom", fontsize=8, color="#555555",
    )
    fig.tight_layout(rect=(0.025, 0.075, 0.985, 0.92), w_pad=3.0)
    output_png.parent.mkdir(parents=True, exist_ok=True)
    output_pdf.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_png, dpi=300, bbox_inches="tight", metadata={"Software": "Matplotlib"})
    fig.savefig(output_pdf, bbox_inches="tight", metadata={"Creator": "Matplotlib"})
    plt.close(fig)


def main() -> int:
    default_dir = REPO / "docs/PAPER/named-data-network-service-framework-paper/figures"
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-png", type=Path, default=default_dir / "mobility-two-layer-evidence.png")
    parser.add_argument("--output-pdf", type=Path, default=default_dir / "mobility-two-layer-evidence.pdf")
    args = parser.parse_args()
    build_figure(args.output_png.resolve(), args.output_pdf.resolve())
    print(f"wrote {args.output_png.resolve()}")
    print(f"wrote {args.output_pdf.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
