#!/usr/bin/env python3
"""Generate the Spec 157 five-stage UAV latency attribution figure."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt


STAGES = (
    ("capture_to_encoded_ms", "Capture to encoded", "#2F6B9A"),
    ("encoded_to_materialized_ms", "Encoded to materialized", "#E69F00"),
    ("materialized_to_decoder_input_ms", "Materialized to decoder input", "#009E73"),
    ("decoder_input_to_output_ms", "Decoder input to output", "#CC79A7"),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--output-prefix", required=True, type=Path)
    args = parser.parse_args()

    payload = json.loads(args.summary.read_text(encoding="utf-8"))
    cells = sorted(payload["cells"], key=lambda item: float(item["fps"]))
    fps = [float(item["fps"]) for item in cells]

    fig, axis = plt.subplots(figsize=(10.8, 4.8), constrained_layout=True)
    bottoms = [0.0] * len(cells)
    for field, label, color in STAGES:
        values = [float(item["stages"][field]["mean"]) for item in cells]
        axis.bar(fps, values, width=6.3, bottom=bottoms, label=label,
                 color=color, edgecolor="white", linewidth=0.5)
        bottoms = [left + right for left, right in zip(bottoms, values)]

    axis.set_xlabel("Configured video rate (FPS)")
    axis.set_ylabel("Mean per-frame latency (ms)")
    axis.set_xticks(fps)
    axis.grid(axis="y", alpha=0.25, linewidth=0.6)
    axis.set_axisbelow(True)

    segment_axis = axis.twinx()
    segment_means = [
        float(item["sourceSegmentsPerFrame"]["mean"]) for item in cells
    ]
    segment_axis.plot(fps, segment_means, color="#222222", marker="o",
                      linewidth=2.0, linestyle="--",
                      label="Source segments per frame")
    segment_axis.set_ylabel("Mean source segments per frame")
    segment_axis.set_ylim(bottom=0)

    handles, labels = axis.get_legend_handles_labels()
    second_handles, second_labels = segment_axis.get_legend_handles_labels()
    axis.legend(handles + second_handles, labels + second_labels,
                loc="upper right", frameon=False, fontsize=8, ncol=2)
    axis.set_title(
        "Higher FPS reduces pre-network frame work; fetch/reassembly stays flat")

    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output_prefix.with_suffix(".pdf"))
    fig.savefig(args.output_prefix.with_suffix(".png"), dpi=220)
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
