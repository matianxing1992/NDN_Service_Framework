#!/usr/bin/env python3
"""Offline static CPU ONNX export for the Spec 261 YOLO profiles.

The export runs with the pinned offline interpreter and never inside a runtime
container. It keeps the model's own end-to-end detection tail (including any
graph-embedded NMS) and always externalizes initializers so the caller owns an
explicit weight object.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

EXPECTED_CHECKPOINT_SHA256 = {
    "yolo26n": "9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def first_tensor(value):
    import torch

    if torch.is_tensor(value):
        return value
    if isinstance(value, (list, tuple)):
        for item in value:
            tensor = first_tensor(item)
            if tensor is not None:
                return tensor
    return None


def export(checkpoint: Path, out_dir: Path, name: str, input_size: int, opset: int) -> Path:
    import onnx
    import torch
    from ultralytics import YOLO

    expected = EXPECTED_CHECKPOINT_SHA256.get(name)
    if expected is not None and sha256_file(checkpoint) != expected:
        raise SystemExit(f"checkpoint digest differs from the pinned {name} asset")

    class Full(torch.nn.Module):
        def __init__(self, model):
            super().__init__()
            self.model = model

        def forward(self, x):
            return first_tensor(self.model(x))

    model = Full(YOLO(str(checkpoint)).model.eval().cpu())
    out_dir.mkdir(parents=True, exist_ok=True)
    graph = out_dir / f"{name}.onnx"
    sample = torch.zeros(1, 3, input_size, input_size, dtype=torch.float32)
    temporary = graph.with_suffix(".embedded.onnx")
    torch.onnx.export(model, sample, str(temporary), input_names=["images"],
                      output_names=["predictions"], opset_version=opset,
                      do_constant_folding=True)
    loaded = onnx.load(str(temporary))
    onnx.checker.check_model(loaded)
    from onnxruntime.tools.symbolic_shape_infer import SymbolicShapeInference
    inferred = SymbolicShapeInference.infer_shapes(loaded, auto_merge=True)
    onnx.checker.check_model(inferred)
    onnx.save_model(inferred, str(graph), save_as_external_data=True,
                    all_tensors_to_one_file=True,
                    location=graph.stem + ".weights", size_threshold=0)
    temporary.unlink(missing_ok=True)
    return graph


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--name", default="yolo26n")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--input-size", type=int, default=640)
    parser.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()
    graph = export(args.model, args.out_dir, args.name, args.input_size, args.opset)
    weights = graph.with_suffix(".weights")
    print(f"GRAPH={graph} BYTES={graph.stat().st_size} SHA256={sha256_file(graph)}")
    print(f"WEIGHTS={weights} BYTES={weights.stat().st_size} SHA256={sha256_file(weights)}")


if __name__ == "__main__":
    main()
