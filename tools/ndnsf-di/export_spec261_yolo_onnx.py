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
    "yolo11n": "0ebbc80d4a7680d14987a577cd21342b65ecfd94632bd9a8da63ae6417644ee1",
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


def export(checkpoint: Path, out_dir: Path, name: str, input_size: int, opset: int,
           nms: bool = False) -> Path:
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
    if nms:
      # Graph-embedded NMS is produced by the Ultralytics exporter, not by a
      # raw torch.onnx.export of the module; a raw export silently returns the
      # raw head and would change the detection ABI.
      produced = Path(YOLO(str(checkpoint)).export(
        format="onnx", imgsz=input_size, opset=opset, nms=True, simplify=False,
        dynamic=False, half=False, device="cpu", project=str(out_dir.parent),
        name=out_dir.name + "-nms"))
      loaded = onnx.load(str(produced), load_external_data=True)
      onnx.checker.check_model(loaded)
      onnx.save_model(loaded, str(graph), save_as_external_data=True,
                      all_tensors_to_one_file=True,
                      location=graph.stem + ".weights", size_threshold=0)
      produced.unlink(missing_ok=True)
      return graph
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
    parser.add_argument("--nms", action="store_true",
                        help="use the Ultralytics exporter and keep graph-embedded NMS")
    args = parser.parse_args()
    graph = export(args.model, args.out_dir, args.name, args.input_size, args.opset,
                   nms=args.nms)
    weights = graph.with_suffix(".weights")
    print(f"GRAPH={graph} BYTES={graph.stat().st_size} SHA256={sha256_file(graph)}")
    print(f"WEIGHTS={weights} BYTES={weights.stat().st_size} SHA256={sha256_file(weights)}")


if __name__ == "__main__":
    main()
