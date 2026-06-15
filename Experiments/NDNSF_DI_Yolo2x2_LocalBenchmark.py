#!/usr/bin/env python3
"""Local ONNX benchmark for the YOLO layout used by NDNSF-DI.

This runs the exported ONNX chunks sequentially in one local process. It does
not start NFD, MiniNDN, NDNSF, repo, or provider processes.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np


REPO = Path(__file__).resolve().parents[1]
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"
sys.path.insert(0, str(PY_DIR))

from yolo_2x2_lib import (  # noqa: E402
    DEFAULT_INPUT_SIZE,
    ROLE_MERGE,
    ROLE_BACKBONE,
    make_input,
    make_ort_session,
    parallel_detect_replicated_backbone_roles_for_layout,
    parallel_detect_scale_roles_for_layout,
    roles_for_layout,
    _value_for_input,
)


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * (p / 100.0)
    lo = int(rank)
    hi = min(lo + 1, len(ordered) - 1)
    weight = rank - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def chunk_path(model_dir: Path,
               role: str,
               input_size: int,
               *,
               layout: str,
               parallel_detect_scale_shards: bool,
               parallel_detect_replicated_backbone_shards: bool) -> Path:
    if ((parallel_detect_scale_shards or parallel_detect_replicated_backbone_shards) and
            role == ROLE_MERGE):
        path = model_dir / f"yolo26n-DetectMerge-{layout}-{input_size}.onnx"
        if not path.exists():
            raise FileNotFoundError(f"missing ONNX chunk for {role}: {path}")
        return path
    suffix = role.strip("/").replace("/", "-")
    path = model_dir / f"yolo26n-{suffix}-{input_size}.onnx"
    if not path.exists():
        raise FileNotFoundError(f"missing ONNX chunk for {role}: {path}")
    return path


def resolve_auto_parallel_detect_plan(model_dir: Path) -> tuple[Path, bool, bool, dict]:
    selection_path = model_dir / "planner-selection.json"
    if not selection_path.exists():
        raise FileNotFoundError(
            "--auto-parallel-detect-plan expects planner-selection.json in "
            f"{model_dir}; generate the model with split_model.py --auto-parallel-detect-plan")
    selection = json.loads(selection_path.read_text(encoding="utf-8"))
    mode = str(selection.get("mode", ""))
    candidate_dir = model_dir / "planner-candidates" / mode
    if mode == "shared-backbone":
        return candidate_dir, True, False, selection
    if mode == "replicated-backbone":
        return candidate_dir, False, True, selection
    raise ValueError(f"unknown planner-selected mode in {selection_path}: {mode!r}")


def run_cached_pipeline(sessions: dict[str, object],
                        roles: list[str],
                        image: np.ndarray) -> np.ndarray:
    values: dict[str, np.ndarray] = {"images": image.astype(np.float32)}
    for role in roles:
        session = sessions[role]
        feed = {
            input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
            for input_info in session.get_inputs()
        }
        outputs = session.run(None, feed)
        values = {
            output.name: np.asarray(value, dtype=np.float32)
            for output, value in zip(session.get_outputs(), outputs)
        }
    return values.get("predictions", next(iter(values.values()))).astype(np.float32)


def run_cached_parallel_detect_scale_pipeline(sessions: dict[str, object],
                                              roles: list[str],
                                              image: np.ndarray) -> np.ndarray:
    values: dict[str, np.ndarray] = {"images": image.astype(np.float32)}
    if ROLE_BACKBONE in sessions:
        backbone = sessions[ROLE_BACKBONE]
        feed = {
            input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
            for input_info in backbone.get_inputs()
        }
        outputs = backbone.run(None, feed)
        values.update({
            output.name: np.asarray(value, dtype=np.float32)
            for output, value in zip(backbone.get_outputs(), outputs)
        })

    for role in roles:
        if not role.startswith("/Head/Shard/"):
            continue
        session = sessions[role]
        feed = {
            input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
            for input_info in session.get_inputs()
        }
        outputs = session.run(None, feed)
        values.update({
            output.name: np.asarray(value, dtype=np.float32)
            for output, value in zip(session.get_outputs(), outputs)
        })

    merge = sessions[ROLE_MERGE]
    feed = {
        input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
        for input_info in merge.get_inputs()
    }
    outputs = merge.run(None, feed)
    merged = {
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(merge.get_outputs(), outputs)
    }
    return merged.get("predictions", next(iter(merged.values()))).astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", default="2x2")
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--model-dir", default="")
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--output", default="")
    parser.add_argument("--parallel-detect-scale-shards", action="store_true")
    parser.add_argument("--parallel-detect-replicated-backbone-shards", action="store_true")
    parser.add_argument("--auto-parallel-detect-plan", action="store_true",
                        help="Read planner-selection.json from --model-dir and benchmark the selected plan")
    args = parser.parse_args()
    selected_modes = sum([
        bool(args.parallel_detect_scale_shards),
        bool(args.parallel_detect_replicated_backbone_shards),
        bool(args.auto_parallel_detect_plan),
    ])
    if selected_modes > 1:
        raise SystemExit(
            "--parallel-detect-scale-shards, "
            "--parallel-detect-replicated-backbone-shards, and "
            "--auto-parallel-detect-plan are mutually exclusive")

    layout = args.layout.strip().lower().replace("*", "x")
    model_dir = Path(args.model_dir) if args.model_dir else (
        REPO / f"results/yolo_{layout}_minindn_quick/model")
    planner_selection = {}
    if args.auto_parallel_detect_plan:
        model_dir, args.parallel_detect_scale_shards, args.parallel_detect_replicated_backbone_shards, planner_selection = (
            resolve_auto_parallel_detect_plan(model_dir)
        )
    if args.parallel_detect_replicated_backbone_shards:
        roles = parallel_detect_replicated_backbone_roles_for_layout(layout)
    elif args.parallel_detect_scale_shards:
        roles = parallel_detect_scale_roles_for_layout(layout)
    else:
        roles = roles_for_layout(layout)
    paths = {
        role: chunk_path(model_dir, role, args.input_size,
                         layout=layout,
                         parallel_detect_scale_shards=args.parallel_detect_scale_shards,
                         parallel_detect_replicated_backbone_shards=(
                             args.parallel_detect_replicated_backbone_shards))
        for role in roles
    }
    image = make_input(args.input_size)

    session_start = time.perf_counter()
    sessions = {role: make_ort_session(path) for role, path in paths.items()}
    session_init_ms = (time.perf_counter() - session_start) * 1000.0

    for _ in range(max(0, args.warmup)):
        if args.parallel_detect_scale_shards or args.parallel_detect_replicated_backbone_shards:
            run_cached_parallel_detect_scale_pipeline(sessions, roles, image)
        else:
            run_cached_pipeline(sessions, roles, image)

    samples = []
    for _ in range(max(1, args.iterations)):
        started = time.perf_counter()
        if args.parallel_detect_scale_shards or args.parallel_detect_replicated_backbone_shards:
            result = run_cached_parallel_detect_scale_pipeline(sessions, roles, image)
        else:
            result = run_cached_pipeline(sessions, roles, image)
        samples.append((time.perf_counter() - started) * 1000.0)

    summary = {
        "layout": layout,
        "parallelDetectScaleShards": bool(args.parallel_detect_scale_shards),
        "parallelDetectReplicatedBackboneShards": bool(
            args.parallel_detect_replicated_backbone_shards),
        "autoParallelDetectPlan": bool(args.auto_parallel_detect_plan),
        "plannerSelection": planner_selection,
        "modelDir": str(model_dir),
        "roles": roles,
        "sessionInitMs": session_init_ms,
        "warmup": max(0, args.warmup),
        "iterations": max(1, args.iterations),
        "samplesMs": samples,
        "minMs": min(samples),
        "p50Ms": percentile(samples, 50),
        "p95Ms": percentile(samples, 95),
        "maxMs": max(samples),
        "meanMs": sum(samples) / len(samples),
        "outputShape": list(result.shape),
    }
    if args.output:
        output_path = Path(args.output)
    else:
        output_path = model_dir.parent / "local-onnx-benchmark.json"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    print(
        "YOLO_LAYOUT_LOCAL_BENCHMARK "
        f"layout={layout} iterations={summary['iterations']} warmup={summary['warmup']} "
        f"session_init_ms={session_init_ms:.2f} "
        f"min_ms={summary['minMs']:.2f} p50_ms={summary['p50Ms']:.2f} "
        f"p95_ms={summary['p95Ms']:.2f} max_ms={summary['maxMs']:.2f} "
        f"mean_ms={summary['meanMs']:.2f} output_shape={summary['outputShape']} "
        f"path={output_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
