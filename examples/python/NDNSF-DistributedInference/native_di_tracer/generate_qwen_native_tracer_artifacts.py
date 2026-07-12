#!/usr/bin/env python3
"""Export the fixed three-stage Qwen ONNX/KV artifacts for Spec 105."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


DEFAULT_MODEL = "Qwen/Qwen2.5-0.5B-Instruct"
ROOT = Path(__file__).resolve().parent
PIPELINE_DIR = ROOT.parent / "llm_pipeline"
DEFAULT_OUT = ROOT / "artifacts"


def generate(model_name: str, out_dir: Path, local_files_only: bool,
             opset: int = 17, prompt: str = "NDNSF deployment pilot") -> dict[str, Any]:
    if opset != 17:
        raise ValueError("the frozen Qwen pilot exporter requires ONNX opset 17")
    sys.path.insert(0, str(PIPELINE_DIR))
    try:
        from llm_pipeline_lib import role_name, write_qwen_onnx_stage_artifacts

        artifacts = write_qwen_onnx_stage_artifacts(
            out_dir,
            roles=[role_name(index) for index in range(3)],
            stages=3,
            model_name=model_name,
            prompt=prompt,
            allow_download=not local_files_only,
            dtype="float32",
        )
    finally:
        sys.path.pop(0)
    manifest_path = out_dir / "qwen-onnx-service-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if len(artifacts) != 3 or len(manifest.get("stages", [])) != 3:
        raise RuntimeError("Qwen pilot exporter did not produce exactly three stages")
    summary = {
        "schema": "ndnsf-di-qwen-three-stage-onnx-artifacts-v1",
        "model": model_name,
        "opset": opset,
        "manifest": str(manifest_path),
        "stages": manifest["stages"],
        "expectedTopToken": manifest.get("expectedTopToken"),
        "stagedValidation": manifest.get("stagedValidation"),
    }
    (out_dir / "qwen-native-tracer-artifacts-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--prompt", default="NDNSF deployment pilot")
    parser.add_argument("--allow-download", action="store_true")
    args = parser.parse_args()
    summary = generate(
        args.model,
        Path(args.out),
        local_files_only=not args.allow_download,
        opset=args.opset,
        prompt=args.prompt,
    )
    print("NDNSF_DI_QWEN_THREE_STAGE_ARTIFACTS_OK")
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
