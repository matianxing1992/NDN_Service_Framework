#!/usr/bin/env python3
"""Generate the smallest Qwen-derived ONNX artifacts for NativeTracer.

The current C++ native runner executes float32 ONNX tensors.  Full Qwen
execution normally starts from int64 token IDs, so this generator extracts small
real Qwen weight slices and exports four float32 stage models that match the
NativeTracer dataflow:

    images -> Backbone -> Head0/Head1 -> Merge -> predictions

The artifacts are intentionally tiny, but their weights come from the requested
Qwen checkpoint and the provider executes them through real ONNX Runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

import torch


DEFAULT_MODEL = "Qwen/Qwen2.5-0.5B-Instruct"
ROOT = Path(__file__).resolve().parent
DEFAULT_OUT = ROOT / "artifacts"


class BackboneStage(torch.nn.Module):
    def __init__(self, weight: torch.Tensor, bias: torch.Tensor):
        super().__init__()
        self.register_buffer("weight", weight)
        self.register_buffer("bias", bias)

    def forward(self, images: torch.Tensor) -> torch.Tensor:
        x = images.reshape(images.shape[0], -1)
        return torch.tanh(torch.nn.functional.linear(x, self.weight, self.bias))


class HeadStage(torch.nn.Module):
    def __init__(self, weight: torch.Tensor, bias: torch.Tensor):
        super().__init__()
        self.register_buffer("weight", weight)
        self.register_buffer("bias", bias)

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        return torch.tanh(torch.nn.functional.linear(features, self.weight, self.bias))


class MergeStage(torch.nn.Module):
    def __init__(self, weight: torch.Tensor, bias: torch.Tensor):
        super().__init__()
        self.register_buffer("weight", weight)
        self.register_buffer("bias", bias)

    def forward(self, detections0: torch.Tensor, detections1: torch.Tensor) -> torch.Tensor:
        merged = torch.cat([detections0, detections1], dim=1)
        return torch.nn.functional.linear(merged, self.weight, self.bias)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_sha256(path: Path) -> str:
    digest = sha256_file(path)
    path.with_suffix(path.suffix + ".sha256").write_text(digest + "\n", encoding="utf-8")
    return digest


def load_qwen(model_name: str, local_files_only: bool) -> Any:
    from transformers import AutoModelForCausalLM

    model = AutoModelForCausalLM.from_pretrained(
        model_name,
        torch_dtype=torch.float32,
        local_files_only=local_files_only,
    )
    model.eval()
    return model


def qwen_weight_matrix(model: Any) -> torch.Tensor:
    base = getattr(model, "model", None)
    embed = getattr(base, "embed_tokens", None) if base is not None else None
    if embed is None or getattr(embed, "weight", None) is None:
        raise RuntimeError("Qwen model must expose model.embed_tokens.weight")
    return embed.weight.detach().cpu().float()


def qwen_lm_head_matrix(model: Any, fallback: torch.Tensor) -> torch.Tensor:
    head = getattr(model, "lm_head", None)
    if head is None or getattr(head, "weight", None) is None:
        return fallback
    return head.weight.detach().cpu().float()


def slice_rows(matrix: torch.Tensor, start: int, rows: int, cols: int) -> torch.Tensor:
    if matrix.shape[0] < start + rows or matrix.shape[1] < cols:
        raise RuntimeError(
            f"Qwen weight matrix too small for rows={start}:{start + rows}, cols={cols}")
    return matrix[start:start + rows, :cols].contiguous()


def export_onnx(module: torch.nn.Module,
                path: Path,
                inputs: tuple[torch.Tensor, ...],
                input_names: list[str],
                output_names: list[str],
                opset: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with torch.no_grad():
        torch.onnx.export(
            module,
            inputs,
            str(path),
            input_names=input_names,
            output_names=output_names,
            opset_version=opset,
            do_constant_folding=True,
        )


def generate(model_name: str, out_dir: Path, local_files_only: bool, opset: int) -> dict[str, Any]:
    model = load_qwen(model_name, local_files_only)
    embed = qwen_weight_matrix(model)
    lm_head = qwen_lm_head_matrix(model, embed)

    # Small float32 contract used by the C++ native runner.
    input_shape = (1, 3, 2, 2)
    feature_dim = 16
    head_dim = 8
    output_dim = 4

    backbone = BackboneStage(
        slice_rows(embed, 0, feature_dim, 12),
        torch.zeros(feature_dim, dtype=torch.float32),
    )
    head0 = HeadStage(
        slice_rows(embed, feature_dim, head_dim, feature_dim),
        torch.zeros(head_dim, dtype=torch.float32),
    )
    head1 = HeadStage(
        slice_rows(embed, feature_dim + head_dim, head_dim, feature_dim),
        torch.zeros(head_dim, dtype=torch.float32),
    )
    merge = MergeStage(
        slice_rows(lm_head, 0, output_dim, head_dim * 2),
        torch.zeros(output_dim, dtype=torch.float32),
    )

    image_sample = torch.arange(12, dtype=torch.float32).reshape(input_shape) / 10.0
    feature_sample = torch.zeros((1, feature_dim), dtype=torch.float32)
    detection_sample = torch.zeros((1, head_dim), dtype=torch.float32)

    artifacts = {
        "backbone": out_dir / "qwen-native-tracer-backbone.onnx",
        "head0": out_dir / "qwen-native-tracer-head0.onnx",
        "head1": out_dir / "qwen-native-tracer-head1.onnx",
        "merge": out_dir / "qwen-native-tracer-merge.onnx",
    }
    export_onnx(backbone, artifacts["backbone"], (image_sample,), ["images"], ["features"], opset)
    export_onnx(head0, artifacts["head0"], (feature_sample,), ["features"], ["detections0"], opset)
    export_onnx(head1, artifacts["head1"], (feature_sample,), ["features"], ["detections1"], opset)
    export_onnx(
        merge,
        artifacts["merge"],
        (detection_sample, detection_sample),
        ["detections0", "detections1"],
        ["predictions"],
        opset,
    )

    summary = {
        "model": model_name,
        "schema": "ndnsf-di-native-tracer-qwen-onnx-artifacts-v1",
        "inputShape": list(input_shape),
        "featureDim": feature_dim,
        "headDim": head_dim,
        "outputDim": output_dim,
        "artifacts": {
            name: {
                "path": str(path),
                "sha256": write_sha256(path),
                "bytes": path.stat().st_size,
            }
            for name, path in artifacts.items()
        },
    }
    (out_dir / "qwen-native-tracer-artifacts-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--allow-download", action="store_true",
                        help="Allow HuggingFace download if the Qwen checkpoint is not cached")
    args = parser.parse_args()

    summary = generate(
        args.model,
        Path(args.out),
        local_files_only=not args.allow_download,
        opset=args.opset,
    )
    print("NDNSF_DI_QWEN_NATIVE_TRACER_ARTIFACTS_OK")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
