#!/usr/bin/env python3
"""Prepare the dynamic raw ONNX detector used by native vehicle tracking."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from tempfile import TemporaryDirectory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--checkpoint-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (args.output.suffix != ".onnx" or args.output.exists() or args.output.is_symlink()
            or not args.output.parent.is_dir()):
        parser.error("--output must be a new .onnx file in an existing directory")
    if not re.fullmatch(r"[0-9a-f]{64}", args.checkpoint_sha256):
        parser.error("--checkpoint-sha256 must contain 64 lowercase hex digits")
    os.environ["YOLO_AUTOINSTALL"] = "false"
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from ultralytics import YOLO, __version__ as ultralytics_version

    torch.set_num_threads(1)
    with TemporaryDirectory(prefix=".vehicle-onnx-", dir=args.output.parent) as scratch:
        checkpoint = Path(scratch) / "detector.pt"
        shutil.copyfile(args.checkpoint, checkpoint)
        digest = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        if digest != args.checkpoint_sha256:
            parser.error("checkpoint SHA-256 mismatch")
        model = YOLO(str(checkpoint), task="detect")
        names = model.names
        if not 1 <= len(names) <= 256 or set(names) != set(range(len(names))):
            parser.error("checkpoint must provide a contiguous zero-based class index")
        labels = [names[i] for i in range(len(names))]
        if any(not isinstance(name, str) or not 0 < len(name.encode()) <= 256
               or any(ord(ch) < 32 or ord(ch) == 127 for ch in name) for name in labels):
            parser.error("checkpoint class label outside supported bounds")
        exported = Path(model.export(format="onnx", imgsz=640, batch=1, dynamic=True,
                                     simplify=False, opset=17, nms=None, device="cpu"))
        graph = onnx.load(exported)
        class_index = graph.metadata_props.add()
        class_index.key = "ndnsf.uav.class-names.v1"
        class_index.value = json.dumps(labels, separators=(",", ":"), ensure_ascii=False)
        onnx.checker.check_model(graph)
        onnx.save(graph, exported)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        session = ort.InferenceSession(str(exported), options, providers=["CPUExecutionProvider"])
        inputs, outputs = session.get_inputs(), session.get_outputs()
        if len(inputs) != 1 or len(outputs) != 1:
            parser.error("raw detector export must have one input and one output")
        for height, width in ((640, 480), (384, 640)):
            image = np.zeros((1, 3, height, width), dtype=np.float32)
            result, = session.run(None, {inputs[0].name: image})
            anchors = sum((height // stride) * (width // stride) for stride in (8, 16, 32))
            if (result.dtype != np.float32 or result.shape != (1, 4 + len(labels), anchors)
                    or not np.isfinite(result).all()):
                parser.error("ONNX export did not produce a finite dynamic raw YOLO head")
        artifact_digest = hashlib.sha256(exported.read_bytes()).hexdigest()
        os.link(exported, args.output)
        print(json.dumps({"checkpointSha256": digest, "onnxSha256": artifact_digest,
                          "classNames": labels, "ultralytics": ultralytics_version,
                          "torch": torch.__version__, "onnx": onnx.__version__,
                          "onnxruntime": ort.__version__}, sort_keys=True))


if __name__ == "__main__":
    main()
