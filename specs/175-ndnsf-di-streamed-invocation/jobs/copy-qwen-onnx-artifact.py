#!/usr/bin/env python3
"""Promote only the runtime ONNX artifact from an exporter work directory.

The exporter also emits large ``stage-pt`` files for offline diagnostics.
Those files contain the Transformers implementation and are not part of the
deployed ONNX artifact.  Copying the whole output tree can exhaust the
artifact quota before the immutable manifest is promoted.
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: copy-qwen-onnx-artifact.py OUTPUT ARTIFACT")
    output = Path(sys.argv[1]).resolve()
    artifact = Path(sys.argv[2]).resolve()
    required = (
        output / "qwen-onnx-stage-artifacts",
        output / "qwen-onnx-tokenizer",
        output / "qwen-onnx-service-manifest.json",
    )
    if not all(path.exists() for path in required):
        missing = ", ".join(str(path) for path in required if not path.exists())
        raise SystemExit(f"missing ONNX export assets: {missing}")

    stage_target = artifact / "qwen-onnx-stage-artifacts"
    stage_target.mkdir(parents=True, exist_ok=True)
    for source in required[0].iterdir():
        target = stage_target / source.name
        if source.is_dir():
            shutil.copytree(source, target, dirs_exist_ok=True)
        else:
            shutil.copy2(source, target)
    shutil.copytree(required[1], artifact / required[1].name, dirs_exist_ok=True)
    for name in ("qwen-onnx-service-manifest.json", "model-source.sha256",
                 "exporterSifSha256"):
        source = output / name
        if source.exists():
            shutil.copy2(source, artifact / name)
    # The exporter manifest is produced under the node-local output directory.
    # Rewrite every promoted path to the final artifact root so an atomic
    # partial->final rename cannot leave dangling ``.partial`` references.
    manifest_path = artifact / "qwen-onnx-service-manifest.json"
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    document["artifactRoot"] = str(artifact)
    for stage in document.get("stages", []):
        stage["path"] = str(
            artifact / "qwen-onnx-stage-artifacts" / Path(stage["path"]).name)
    manifest_path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    if any(not Path(stage["path"]).is_file()
           for stage in document.get("stages", [])):
        raise SystemExit("promoted ONNX manifest contains a dangling stage path")
    if any(path.name.endswith(".pt") for path in artifact.rglob("*")):
        raise SystemExit("Transformers stage package leaked into ONNX artifact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
