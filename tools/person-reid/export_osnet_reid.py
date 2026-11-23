#!/usr/bin/env python3
"""Export a torchreid OSNet x1.0 Market1501 or MSMT17 checkpoint to ONNX.

Offline preparation only: production tracking consumes the resulting ONNX and
its receipt.  The preprocessing contract matches the maintained person profile
(`osnet-rgb-256x128-imagenet-l2-v1`): BGR->RGB, bilinear resize to 256x128,
ImageNet normalization, 512-d L2-normalized embedding.
"""
from __future__ import annotations

import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
from torch import nn
from torchreid.reid.models import build_model


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build(checkpoint: Path) -> nn.Module:
    model = build_model("osnet_x1_0", num_classes=1, pretrained=False, use_gpu=False)
    payload = torch.load(str(checkpoint), map_location="cpu", weights_only=True)
    state = payload.get("state_dict", payload)
    if not isinstance(state, dict):
        raise ValueError("checkpoint must contain a tensor state_dict")
    body = {}
    for name, tensor in state.items():
        if name.startswith("module."):
            name = name[len("module."):]
        if not name.startswith("classifier."):
            body[name] = tensor
    loaded = model.load_state_dict(body, strict=False)
    if loaded.unexpected_keys or set(loaded.missing_keys) != {
            "classifier.weight", "classifier.bias"}:
        raise ValueError("checkpoint does not contain the complete OSNet body")
    return model.eval()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--receipt", required=True)
    parser.add_argument("--dataset", choices=("market1501", "msmt17"), default="market1501",
                        help="training dataset of the supplied checkpoint")
    args = parser.parse_args()

    checkpoint = Path(args.checkpoint).resolve(strict=True)
    output = Path(args.output).resolve()
    receipt_path = Path(args.receipt).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.parent.mkdir(parents=True, exist_ok=True)

    model = build(checkpoint)

    class Embedding(nn.Module):
        def __init__(self, inner: nn.Module) -> None:
            super().__init__()
            self.inner = inner

        def forward(self, images: torch.Tensor) -> torch.Tensor:
            return self.inner(images)

    wrapper = Embedding(model).eval()
    sample = torch.randn(1, 3, 256, 128, generator=torch.Generator().manual_seed(226))
    with torch.inference_mode():
        reference = wrapper(sample)
    if tuple(reference.shape) != (1, 512) or not torch.isfinite(reference).all():
        raise ValueError("OSNet export did not produce a finite 512-d embedding")

    torch.onnx.export(
        wrapper, sample, str(output), opset_version=17,
        input_names=["images"], output_names=["embedding"],
        dynamic_axes={"images": {0: "batch"}, "embedding": {0: "batch"}},
        do_constant_folding=True,
    )
    onnx.checker.check_model(onnx.load(str(output)))

    session = ort.InferenceSession(str(output), providers=["CPUExecutionProvider"])
    batch = torch.randn(4, 3, 256, 128, generator=torch.Generator().manual_seed(7))
    with torch.inference_mode():
        expected = wrapper(batch).numpy()
    actual = session.run(["embedding"], {"images": batch.numpy()})[0]
    max_abs = float(np.max(np.abs(expected - actual)))
    if max_abs > 1e-4:
        raise ValueError(f"ONNX/torch embedding mismatch: {max_abs}")

    receipt = {
        "schema": "ndnsf-person-reid-export-v1",
        "architecture": "osnet_x1_0",
        "dataset": args.dataset,
        "preprocessId": "osnet-rgb-256x128-imagenet-l2-v1",
        "input": {"name": "images", "shape": ["batch", 3, 256, 128], "dtype": "float32"},
        "output": {"name": "embedding", "shape": ["batch", 512], "dtype": "float32"},
        "normalization": {"mean": [0.485, 0.456, 0.406], "std": [0.229, 0.224, 0.225]},
        "checkpoint": {"path": str(checkpoint), "sha256": sha256(checkpoint)},
        "onnx": {"path": str(output), "sha256": sha256(output), "opset": 17},
        "versions": {"torch": torch.__version__, "torchreid": version("torchreid"),
                     "onnx": onnx.__version__, "onnxruntime": ort.__version__},
        "maxAbsTorchOnnx": max_abs,
    }
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
