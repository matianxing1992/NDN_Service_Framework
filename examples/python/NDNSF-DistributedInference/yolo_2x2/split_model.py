#!/usr/bin/env python3
"""Generate real YOLO ONNX layout shards and NDNSF-DI policy."""

from __future__ import annotations

from pathlib import Path

from yolo_2x2_lib import (
    DEFAULT_LAYOUT,
    YOLO_PARALLEL_OUTPUT_SEMANTICS,
    load_provider_profiles,
    make_input,
    parse_args_with_common,
    roles_for_layout,
    run_local_onnx_pipeline,
    run_local_parallel_output_pipeline,
    split_model,
    full_forward,
    yolo_dynamic_splitter_output,
    service_name_for_layout,
    yolo_splitter_output,
)


def main() -> int:
    parser = parse_args_with_common("Split the real YOLO demo model into a custom layout")
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--input-size", type=int, default=32)
    parser.add_argument("--out-dir", default="/tmp/ndnsf-yolo-2x2")
    parser.add_argument("--layout", default=DEFAULT_LAYOUT,
                        help="stage-by-shard layout such as 1x3, 2x3, 3x2, or 3x3")
    parser.add_argument("--policy", default="")
    parser.add_argument("--provider-profile", default="",
                        help="optional experimental JSON provider profile list; "
                             "default assumes homogeneous providers")
    parser.add_argument("--auto-split", action="store_true",
                        help="select the pipeline stage boundary from ONNX planner output")
    parser.add_argument("--parallel-output-shards", action="store_true",
                        help="export a verifiable true-NxM output-channel shard "
                             "prototype with parallel stage shards and a merge role")
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
        layout=args.layout,
        parallel_output_shards=args.parallel_output_shards,
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
    layout = str(split.get("layout", args.layout))
    print("YOLO_LAYOUT_POLICY", f"layout={layout}", policy)
    if layout == "2x2":
        print("YOLO_2X2_POLICY", policy)
    if split.get("onnx_graph_summary"):
        print("YOLO_LAYOUT_ONNX_GRAPH_SUMMARY", f"layout={layout}", split["onnx_graph_summary"])
    candidates = split.get("onnx_split_candidates") or []
    print("YOLO_LAYOUT_ONNX_SPLIT_CANDIDATES", f"layout={layout}", len(candidates))
    for candidate in candidates[:5]:
        print(
            "YOLO_LAYOUT_ONNX_SPLIT_CANDIDATE",
            f"cut_after_node={candidate.cut_after_node}",
            f"boundary_tensors={len(candidate.boundary_tensors)}",
            f"known_boundary_bytes={candidate.known_boundary_bytes}",
            f"unknown_size_tensors={len(candidate.unknown_size_tensors)}",
        )
    recommendations = split.get("planner_recommendations") or []
    print("YOLO_LAYOUT_PLANNER_RECOMMENDATIONS", f"layout={layout}", len(recommendations))
    for item in recommendations[:5]:
        print(
            "YOLO_LAYOUT_PLANNER_RECOMMENDATION",
            f"cut_after_node={item.candidate.cut_after_node}",
            f"left={item.left_provider.name}",
            f"right={item.right_provider.name}",
            f"transfer_ms={item.transfer_ms:.3f}",
            f"compute_imbalance={item.compute_imbalance:.3f}",
            f"score={item.score:.3f}",
        )
    service_name = service_name_for_layout(layout)
    service = output.service(service_name)
    print(
        "YOLO_LAYOUT_SEMANTICS",
        f"layout={layout}",
        f"semantics={service.metadata.get('layout_semantics', '')}",
        f"stage_shards_parallel={str(service.metadata.get('stage_shards_parallel', False)).lower()}",
    )
    for artifact in output.service(service_name).artifacts:
        print("YOLO_LAYOUT_ARTIFACT", f"layout={layout}", artifact.role, artifact.path)
        if layout == "2x2":
            print("YOLO_2X2_ARTIFACT", artifact.role, artifact.path)
    image = make_input(args.input_size)
    expected = full_forward(args.model, image)
    if split.get("layout_semantics") == YOLO_PARALLEL_OUTPUT_SEMANTICS:
        actual = run_local_parallel_output_pipeline(split["paths"], image, layout)
    else:
        actual = run_local_onnx_pipeline(split["paths"], image, roles_for_layout(layout))
    diff = abs(actual - expected)
    max_diff = float(diff.max())
    mean_diff = float(diff.mean())
    ok = max_diff < 1e-4
    print(
        "YOLO_LAYOUT_LOCAL_VERIFY",
        f"layout={layout}",
        f"chunks={len(split['paths'])}",
        f"max_abs_diff={max_diff:.8f}",
        f"mean_abs_diff={mean_diff:.8f}",
        f"ok={str(ok).lower()}",
    )
    if layout == "2x2":
        print(
            "YOLO_2X2_LOCAL_VERIFY",
            f"max_abs_diff={max_diff:.8f}",
            f"mean_abs_diff={mean_diff:.8f}",
            f"ok={str(ok).lower()}",
        )
    if not ok:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
