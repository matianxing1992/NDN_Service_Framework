#!/usr/bin/env python3
"""Generate real YOLO 2x2 ONNX shards and NDNSF-DI policy."""

from __future__ import annotations

from pathlib import Path

from yolo_2x2_lib import (
    load_provider_profiles,
    parse_args_with_common,
    split_model,
    yolo_dynamic_splitter_output,
    yolo_splitter_output,
)


def main() -> int:
    parser = parse_args_with_common("Split the real YOLO 2x2 demo model")
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--input-size", type=int, default=32)
    parser.add_argument("--out-dir", default="/tmp/ndnsf-yolo-2x2")
    parser.add_argument("--policy", default="")
    parser.add_argument("--provider-profile", default="",
                        help="optional experimental JSON provider profile list; "
                             "default assumes homogeneous providers")
    parser.add_argument("--auto-split", action="store_true",
                        help="select the pipeline stage boundary from ONNX planner output")
    parser.add_argument("--dynamic-provisioning", action="store_true")
    parser.add_argument("--trust-anchor-file", default="")
    args = parser.parse_args()

    profiles = load_provider_profiles(args.provider_profile) if args.provider_profile else None
    split = split_model(
        args.out_dir,
        args.model,
        args.input_size,
        provider_profiles=profiles,
        auto_split=args.auto_split,
    )
    if args.dynamic_provisioning:
        output = yolo_dynamic_splitter_output(
            split,
            trust_anchor_file=args.trust_anchor_file,
        )
    else:
        output = yolo_splitter_output(split)
    policy = Path(args.policy) if args.policy else Path(args.out_dir) / "yolo_policy.yaml"
    output.write_policy_config(policy)
    print("YOLO_2X2_POLICY", policy)
    if split.get("onnx_graph_summary"):
        print("YOLO_2X2_ONNX_GRAPH_SUMMARY", split["onnx_graph_summary"])
    candidates = split.get("onnx_split_candidates") or []
    print("YOLO_2X2_ONNX_SPLIT_CANDIDATES", len(candidates))
    for candidate in candidates[:5]:
        print(
            "YOLO_2X2_ONNX_SPLIT_CANDIDATE",
            f"cut_after_node={candidate.cut_after_node}",
            f"boundary_tensors={len(candidate.boundary_tensors)}",
            f"known_boundary_bytes={candidate.known_boundary_bytes}",
            f"unknown_size_tensors={len(candidate.unknown_size_tensors)}",
        )
    recommendations = split.get("planner_recommendations") or []
    print("YOLO_2X2_PLANNER_RECOMMENDATIONS", len(recommendations))
    for item in recommendations[:5]:
        print(
            "YOLO_2X2_PLANNER_RECOMMENDATION",
            f"cut_after_node={item.candidate.cut_after_node}",
            f"left={item.left_provider.name}",
            f"right={item.right_provider.name}",
            f"transfer_ms={item.transfer_ms:.3f}",
            f"compute_imbalance={item.compute_imbalance:.3f}",
            f"score={item.score:.3f}",
        )
    for artifact in output.service("/AI/YOLO/2x2Inference").artifacts:
        print("YOLO_2X2_ARTIFACT", artifact.role, artifact.path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
