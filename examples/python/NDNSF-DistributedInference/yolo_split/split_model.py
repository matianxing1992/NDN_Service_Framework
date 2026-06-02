#!/usr/bin/env python3
"""Split YOLO and emit NDNSF-DistributedInference deployment policy."""

from __future__ import annotations

import argparse
from pathlib import Path

from yolo_split_lib import (
    DEFAULT_INPUT_SIZE,
    export_yolo_split_onnx,
    load_provider_profiles,
    yolo_splitter_output,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--out-dir", default="/tmp/ndnsf-yolo-split")
    parser.add_argument("--policy", default="",
                        help="Generated yolo_policy.yaml path")
    parser.add_argument("--auto-split", action="store_true",
                        help="Select the 2-stage split with the ONNX planner")
    parser.add_argument("--provider-profile", default="",
                        help="optional experimental JSON provider profile list; "
                             "default assumes homogeneous providers")
    args = parser.parse_args()

    output_dir = Path(args.out_dir)
    profiles = load_provider_profiles(args.provider_profile) if args.provider_profile else None
    exported = export_yolo_split_onnx(
        args.model,
        output_dir,
        args.input_size,
        auto_split=args.auto_split,
        provider_profiles=profiles,
    )
    splitter_output = yolo_splitter_output(exported)
    policy_path = Path(args.policy) if args.policy else output_dir / "yolo_policy.yaml"
    splitter_output.write_policy_config(policy_path)

    service = splitter_output.service("/AI/YOLO/SplitInference")
    print("YOLO_SPLIT_POLICY", policy_path)
    print(
        "YOLO_SPLIT_SELECTED",
        f"source={exported.get('split_source', 'yolo-fixed')}",
        f"split={exported['split']}",
        f"saved_indices={exported['saved_indices']}",
    )
    if exported.get("onnx_graph_summary"):
        print("YOLO_SPLIT_ONNX_GRAPH_SUMMARY", exported["onnx_graph_summary"])
        print(
            "YOLO_SPLIT_PLANNER_SELECTED",
            f"cut_after_node={exported.get('planner_selected_cut_after_node')}",
            f"node={exported.get('planner_selected_node')}",
            f"score={float(exported.get('planner_selected_score', 0.0)):.3f}",
            f"compute_imbalance={float(exported.get('planner_selected_compute_imbalance', 0.0)):.3f}",
        )
    for artifact in service.artifacts:
        print(
            "YOLO_SPLIT_ARTIFACT",
            artifact.role,
            artifact.artifact_name,
            artifact.path,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
