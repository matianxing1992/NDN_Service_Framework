#!/usr/bin/env python3
"""Generate deterministic multi-sample cases for a frozen YOLO asset directory.

Each case is a small perturbation of the frozen input plus an independent
prediction file from the native C++ ONNX Runtime oracle. The Provider never
sees these files; they exist so a single session can be exercised with several
distinct inputs without re-exporting or re-signing the model.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path, required=True,
                        help="directory holding model.onnx and input.f32")
    parser.add_argument("--output", type=Path, required=True,
                        help="cases directory to create")
    parser.add_argument("--oracle", type=Path, required=True,
                        help="native spec261-ort-detection-oracle executable")
    parser.add_argument("--shape", default="1,3,640,640")
    parser.add_argument("--extra-cases", type=int, default=8)
    parser.add_argument("--no-edge-cases", action="store_true",
                        help="emit only the frozen input and perturbations")
    parser.add_argument("--min-detecting", type=int, default=9,
                        help="generate perturbations until this many cases have a "
                             "non-degenerate oracle (default 9)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    shape = tuple(int(part) for part in args.shape.split(","))
    frozen_path = args.model_dir / "input.f32"
    if not frozen_path.is_file():
        raise SystemExit("missing frozen input: " + str(frozen_path))
    graph = next(args.model_dir.glob("*.onnx"), None)
    if graph is None:
        raise SystemExit("missing model: " + str(args.model_dir))
    frozen = np.frombuffer(frozen_path.read_bytes(), dtype="<f4").reshape(shape)
    cases = [frozen.astype("float32")]
    # Perturbations stay inside [0, 1] so every case is a plausible image tensor.
    perturbations = [(1, 0.95, 0.02), (2, 1.05, 0.02), (3, 1.0, 0.05), (4, 0.9, 0.0),
                     (11, 0.98, 0.01), (12, 1.02, 0.01), (13, 0.90, 0.03), (14, 1.10, 0.03)]
    for seed, gain, noise in perturbations:
        if len(cases) > args.extra_cases:
            break
        rng = np.random.default_rng(seed)
        value = np.clip(frozen * gain + rng.normal(0, noise, frozen.shape).astype("float32"), 0, 1)
        cases.append(value.astype("float32"))
    if not args.no_edge_cases:
        # Edge inputs that are not perturbations of the image: they exercise the
        # empty-detection tail, saturated activations and structure-free noise.
        cases.append(np.zeros(shape, dtype="float32"))
        cases.append(np.ones(shape, dtype="float32"))
        ramp = np.linspace(0.0, 1.0, shape[-1], dtype="float32")
        row = np.repeat(ramp[None, :], shape[-2], axis=0)
        cases.append(np.broadcast_to(row, shape).astype("float32").copy())
        cases.append(np.random.default_rng(7).random(shape, dtype=np.float32))
    args.output.mkdir(parents=True, exist_ok=True)
    import subprocess
    manifest = {"schema": "spec261-detection-cases-v1", "cases": []}
    for index, value in enumerate(cases):
        case = args.output / f"case-{index:02d}"
        case.mkdir(exist_ok=True)
        (case / "input.f32").write_bytes(value.astype("<f4").tobytes())
        npy = args.output / f"case-{index:02d}-input.npy"
        prediction = args.output / f"case-{index:02d}-oracle.npy"
        np.save(npy, value.astype("<f4"))
        subprocess.run([str(args.oracle), "--graph", str(graph), "--input", str(npy),
                        "--write-predictions", str(prediction)], check=True)
        values = np.load(prediction).astype("<f4").reshape(-1)
        (case / "oracle.json").write_text(json.dumps({"float32": [float(v) for v in values]}))
        npy.unlink()
        prediction.unlink()
        # A synthetic input can drive the model to an all-zero output, which only
        # exercises the padding path. Record that per case so a caller can select
        # a detection-comparing set instead of assuming every case compares.
        scores = values.reshape(-1, 6)[:, 4]
        manifest["cases"].append({
            "name": case.name,
            "nonzero": int(np.count_nonzero(values)),
            "maxScore": float(scores.max()) if scores.size else 0.0,
            "comparing": bool(np.count_nonzero(values)),
        })
    comparing = sum(1 for item in manifest["cases"] if item["comparing"])
    manifest["detectionComparing"] = comparing
    manifest["degenerate"] = [item["name"] for item in manifest["cases"] if not item["comparing"]]
    if comparing < args.min_detecting:
        raise SystemExit(f"only {comparing} detection-comparing cases; "
                         f"requested {args.min_detecting}")
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True))
    print(f"wrote {len(cases)} cases to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
