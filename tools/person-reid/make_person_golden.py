#!/usr/bin/env python3
"""Produce deterministic detector/ReID goldens from the frozen ONNX assets.

This is offline model preparation, not production execution.  The raw tensors
and decoded boxes let the native C++ backend compare preprocessing, inference
and NMS without a Python runtime in the tracking path.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from importlib.metadata import version
from pathlib import Path

import cv2
import numpy as np


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def letterbox(image: np.ndarray, size: int = 640) -> tuple[np.ndarray, float, tuple[int, int]]:
    height, width = image.shape[:2]
    scale = min(size / width, size / height)
    new_width, new_height = round(width * scale), round(height * scale)
    resized = cv2.resize(image, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
    pad_width, pad_height = size - new_width, size - new_height
    left, top = round(pad_width / 2 - 0.1), round(pad_height / 2 - 0.1)
    canvas = np.full((size, size, 3), 114, dtype=np.uint8)
    canvas[top:top + new_height, left:left + new_width] = resized
    return canvas, scale, (left, top)


def nms(boxes: np.ndarray, scores: np.ndarray, iou_threshold: float) -> list[int]:
    order = scores.argsort()[::-1]
    keep: list[int] = []
    while order.size:
        index = int(order[0])
        keep.append(index)
        if order.size == 1:
            break
        rest = order[1:]
        xx1 = np.maximum(boxes[index, 0], boxes[rest, 0])
        yy1 = np.maximum(boxes[index, 1], boxes[rest, 1])
        xx2 = np.minimum(boxes[index, 2], boxes[rest, 2])
        yy2 = np.minimum(boxes[index, 3], boxes[rest, 3])
        inter = np.maximum(0, xx2 - xx1) * np.maximum(0, yy2 - yy1)
        area_i = (boxes[index, 2] - boxes[index, 0]) * (boxes[index, 3] - boxes[index, 1])
        area_r = (boxes[rest, 2] - boxes[rest, 0]) * (boxes[rest, 3] - boxes[rest, 1])
        iou = inter / (area_i + area_r - inter + 1e-9)
        order = rest[iou <= iou_threshold]
    return keep


def stock_crops(image_path: Path, checkpoint: Path, boxes_path: Path, output: Path) -> int:
    """Freeze the checkpoint/PIL reference independently of native preprocessing."""
    import torch
    from PIL import Image
    from torchvision import transforms
    from export_osnet_reid import build

    image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError("cannot decode stock reference image")
    height, width = image.shape[:2]
    boxes = json.loads(boxes_path.read_text())
    if not isinstance(boxes, list) or not 1 <= len(boxes) <= 64:
        raise ValueError("stock reference needs 1..64 crop boxes")
    transform = transforms.Compose([
        transforms.Resize((256, 128), interpolation=transforms.InterpolationMode.BILINEAR),
        transforms.ToTensor(),
        transforms.Normalize((0.485, 0.456, 0.406), (0.229, 0.224, 0.225)),
    ])
    crops, rows = [], []
    for box in boxes:
        if (not isinstance(box, list) or len(box) != 4
                or any(isinstance(v, bool) or not isinstance(v, (int, float))
                       or not math.isfinite(v) for v in box)):
            raise ValueError("invalid stock reference box")
        rounded = [int(round(float(value))) for value in box]
        rounded = [max(0, min(width if i % 2 == 0 else height, value))
                   for i, value in enumerate(rounded)]
        x1, y1, x2, y2 = rounded
        if x1 >= x2 or y1 >= y2:
            raise ValueError("empty stock reference box")
        row = {"observationBox": box, "box": rounded, "tensorIndex": None}
        if x2 - x1 >= 16 and y2 - y1 >= 32:
            rgb = cv2.cvtColor(image[y1:y2, x1:x2], cv2.COLOR_BGR2RGB)
            crop = transform(Image.fromarray(rgb))
            row["tensorIndex"] = len(crops)
            row["tensorSha256"] = hashlib.sha256(
                crop.numpy().astype("<f4").tobytes()).hexdigest()
            crops.append(crop)
        rows.append(row)
    torch.set_num_threads(1)
    model = build(checkpoint)
    reference = []
    with torch.inference_mode():
        for offset in range(0, len(crops), 32):
            features = model(torch.stack(crops[offset:offset + 32]))
            norms = torch.linalg.vector_norm(features, dim=1, keepdim=True)
            if (tuple(features.shape) != (min(32, len(crops) - offset), 512)
                    or not torch.isfinite(features).all() or not torch.isfinite(norms).all()
                    or not (norms > 1e-12).all()):
                raise ValueError("invalid stock reference embedding")
            reference.append(features / norms)
    (output / "image.rgb").write_bytes(cv2.cvtColor(image, cv2.COLOR_BGR2RGB).tobytes())
    tensor = (torch.stack(crops).numpy() if crops else
              np.empty((0, 3, 256, 128), dtype=np.float32))
    embedding = (torch.cat(reference).numpy() if reference else
                 np.empty((0, 512), dtype=np.float32))
    (output / "reid-input.f32").write_bytes(tensor.astype("<f4").tobytes())
    (output / "reid-output.f32").write_bytes(embedding.astype("<f4").tobytes())
    receipt = {
        "schema": "ndnsf-person-stock-crops-v1",
        "image": {"path": str(image_path), "sha256": sha256(image_path),
                  "width": width, "height": height},
        "checkpoint": {"path": str(checkpoint), "sha256": sha256(checkpoint)},
        "boxes": rows, "cropCount": len(crops), "featureDimension": 512,
        "preprocessId": "osnet-rgb-256x128-imagenet-l2-v1",
        "versions": {name: version(name) for name in
                     ("torch", "torchvision", "pillow", "torchreid", "numpy")},
    }
    (output / "golden.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps(receipt, sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--detector")
    parser.add_argument("--stock-crops", action="store_true",
                        help="use checkpoint/PIL reference instead of the raw ONNX detector")
    parser.add_argument("--boxes", type=Path, help="JSON observation boxes for --stock-crops")
    parser.add_argument("--reid", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    image_path = Path(args.image).resolve(strict=True)
    if args.stock_crops:
        if args.detector or args.boxes is None:
            parser.error("--stock-crops requires --boxes and does not use --detector")
        return stock_crops(image_path, Path(args.reid).resolve(strict=True),
                           args.boxes.resolve(strict=True), output)
    if not args.detector or args.boxes is not None:
        parser.error("the raw ONNX reference requires --detector and does not use --boxes")
    import onnxruntime as ort

    image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError("cannot decode test image")

    canvas, scale, (pad_x, pad_y) = letterbox(image, 640)
    tensor = canvas[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32) / 255.0
    detector = ort.InferenceSession(args.detector, providers=["CPUExecutionProvider"])
    raw = detector.run(None, {"images": tensor})[0]
    if raw.shape != (1, 84, 8400):
        raise ValueError("unexpected detector output shape")
    predictions = raw[0].T
    person = predictions[:, 4]
    candidates = person >= 0.25
    boxes_cxcywh = predictions[candidates, :4]
    scores = person[candidates]
    boxes = np.empty_like(boxes_cxcywh)
    boxes[:, 0] = boxes_cxcywh[:, 0] - boxes_cxcywh[:, 2] / 2
    boxes[:, 1] = boxes_cxcywh[:, 1] - boxes_cxcywh[:, 3] / 2
    boxes[:, 2] = boxes_cxcywh[:, 0] + boxes_cxcywh[:, 2] / 2
    boxes[:, 3] = boxes_cxcywh[:, 1] + boxes_cxcywh[:, 3] / 2
    keep = nms(boxes, scores, 0.7)
    height, width = image.shape[:2]
    decoded = []
    for index in keep:
        x1, y1, x2, y2 = boxes[index]
        x1 = int(round((x1 - pad_x) / scale))
        y1 = int(round((y1 - pad_y) / scale))
        x2 = int(round((x2 - pad_x) / scale))
        y2 = int(round((y2 - pad_y) / scale))
        x1, y1 = max(0, min(width, x1)), max(0, min(height, y1))
        x2, y2 = max(0, min(width, x2)), max(0, min(height, y2))
        if x1 >= x2 or y1 >= y2:
            continue
        decoded.append({"box": [x1, y1, x2, y2], "confidence": float(scores[index])})
    if not decoded:
        raise ValueError("test image produced no person detections")

    from PIL import Image

    crops = []
    for row in decoded:
        x1, y1, x2, y2 = row["box"]
        if x2 - x1 < 16 or y2 - y1 < 32:
            continue
        crop = cv2.cvtColor(image[y1:y2, x1:x2], cv2.COLOR_BGR2RGB)
        crop = np.asarray(Image.fromarray(crop).resize((128, 256), Image.Resampling.BILINEAR))
        normalized = crop.astype(np.float32) / np.float32(255)
        normalized = (normalized - np.array([0.485, 0.456, 0.406], dtype=np.float32)) / \
            np.array([0.229, 0.224, 0.225], dtype=np.float32)
        crops.append(normalized.transpose(2, 0, 1))
    reid_tensor = np.stack(crops).astype(np.float32)
    reid = ort.InferenceSession(args.reid, providers=["CPUExecutionProvider"])
    embedding = reid.run(None, {"images": reid_tensor})[0]
    norms = np.linalg.norm(embedding, axis=1, keepdims=True)
    if not np.isfinite(embedding).all() or np.any(norms <= 1e-12):
        raise ValueError("non-finite or zero ReID embedding")
    normalized_embedding = embedding / norms

    (output / "detector-input.f32").write_bytes(tensor.astype("<f4").tobytes())
    (output / "detector-output.f32").write_bytes(raw.astype("<f4").tobytes())
    (output / "reid-input.f32").write_bytes(reid_tensor.astype("<f4").tobytes())
    (output / "reid-output.f32").write_bytes(normalized_embedding.astype("<f4").tobytes())
    receipt = {
        "schema": "ndnsf-person-golden-v1",
        "image": {"path": str(image_path), "sha256": sha256(image_path),
                  "width": width, "height": height},
        "detector": {"path": str(Path(args.detector).resolve()),
                     "sha256": sha256(Path(args.detector).resolve()),
                     "letterbox": {"size": 640, "scale": scale, "padX": pad_x, "padY": pad_y},
                     "confidence": 0.25, "nmsIou": 0.7,
                     "detections": decoded},
        "reid": {"path": str(Path(args.reid).resolve()),
                 "sha256": sha256(Path(args.reid).resolve()),
                 "cropCount": len(crops), "featureDimension": 512,
                 "preprocessId": "osnet-rgb-256x128-imagenet-l2-v1"},
    }
    (output / "golden.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
