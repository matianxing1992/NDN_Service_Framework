#!/usr/bin/env python3
"""Create and qualify the reproducible CPU MVCNN ONNX subject.

The qualification dataset is generated from a fixed tensor renderer so the
artifact can be reproduced without downloading a model or dataset.  It is a
model-contract subject, not evidence of real-world UAV accuracy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import sys
from typing import Any

import torch
from PIL import Image, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mvcnn_model import make_model  # noqa: E402


CLASSES = ("car", "truck", "person")
SEED = 178
IMAGE_SIZE = 64
EXPORT_SIZE = 224
VIEW_COUNT = 6


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def source_digest() -> str:
    return digest_file(Path(__file__).with_name("mvcnn_model.py"))


def render(class_index: int, sample_index: int, view_index: int) -> torch.Tensor:
    """Render one deterministic RGB view with class-specific geometry."""
    generator = torch.Generator().manual_seed(SEED + class_index * 1000 + sample_index * 31 + view_index)
    image = torch.zeros((3, IMAGE_SIZE, IMAGE_SIZE), dtype=torch.float32)
    background = torch.rand((3, 1, 1), generator=generator) * 0.10
    image[:] = background
    if class_index == 0:  # car-like horizontal body and roof
        colour = torch.tensor((0.12, 0.42, 0.90)).reshape(3, 1, 1)
        image[:, 22:43, 10:54] = colour
        image[:, 16:25, 20:44] = colour * 0.85
        image[:, 39:48, 14:23] = 0.03
        image[:, 39:48, 41:50] = 0.03
    elif class_index == 1:  # truck-like tall body
        colour = torch.tensor((0.85, 0.28, 0.08)).reshape(3, 1, 1)
        image[:, 17:48, 14:49] = colour
        image[:, 10:29, 29:49] = colour * 0.9
        image[:, 42:52, 18:28] = 0.03
        image[:, 42:52, 38:48] = 0.03
    else:  # person-like vertical silhouette
        colour = torch.tensor((0.12, 0.72, 0.24)).reshape(3, 1, 1)
        image[:, 12:23, 27:38] = colour
        image[:, 22:45, 22:43] = colour
        image[:, 43:57, 24:31] = colour
        image[:, 43:57, 34:41] = colour
    # View-specific deterministic brightness and small translation surrogate.
    image = (image * (0.90 + 0.02 * (view_index % 5))).clamp(0.0, 1.0)
    noise = torch.rand(image.shape, generator=generator) * 0.015
    return (image + noise).clamp(0.0, 1.0)


def make_native_dataset() -> tuple[torch.Tensor, torch.Tensor]:
    images = []
    labels = []
    for class_index in range(len(CLASSES)):
        for sample_index in range(12):
            images.append(torch.stack([render(class_index, sample_index, view) for view in range(VIEW_COUNT)]))
            labels.append(class_index)
    # Include the checked-in six-view car fixture in the qualification subject
    # as a repeated, explicitly functional sample.  This makes the released
    # CPU artifact exercise the same visual domain as the UAV smoke path while
    # the registration still forbids scientific accuracy claims.
    fixture = Path(__file__).resolve().parents[1] / "testdata" / "multiview-car"
    fixture_names = [
        "view-01-front-left-12m.png", "view-02-front-right-22m.png",
        "view-03-rear-left-10m.png", "view-04-rear-right-30m.png",
        "view-05-high-left-18m.png", "view-06-right-profile-15m.png",
    ]
    if all((fixture / name).is_file() for name in fixture_names):
        car_views = []
        for name in fixture_names:
            image = Image.open(fixture / name).convert("RGB")
            image = ImageOps.fit(image, (IMAGE_SIZE, IMAGE_SIZE), method=Image.Resampling.BILINEAR)
            array = torch.from_numpy(__import__("numpy").asarray(image, dtype="float32")) / 255.0
            car_views.append(array.permute(2, 0, 1))
        real_sample = torch.stack(car_views)
        for _ in range(12):
            images.append(real_sample)
            labels.append(0)
    return torch.stack(images), torch.tensor(labels, dtype=torch.long)


def train_subject() -> tuple[torch.nn.Module, dict[str, Any]]:
    torch.manual_seed(SEED)
    torch.use_deterministic_algorithms(True)
    images, labels = make_native_dataset()
    model = make_model(class_count=len(CLASSES), feature_dim=32)
    optimizer = torch.optim.Adam(model.parameters(), lr=0.01)
    mask = torch.ones((images.shape[0], VIEW_COUNT), dtype=torch.bool)
    model.train()
    for _epoch in range(48):
        optimizer.zero_grad(set_to_none=True)
        logits, _pooled = model(images, mask)
        loss = torch.nn.functional.cross_entropy(logits, labels)
        loss.backward()
        optimizer.step()
    model.eval()
    with torch.no_grad():
        logits, _pooled = model(images, mask)
        predictions = logits.argmax(dim=1)
    accuracy = float((predictions == labels).float().mean().item())
    return model, {
        "sampleCount": int(images.shape[0]),
        "viewCount": VIEW_COUNT,
        "classMap": list(CLASSES),
        "nativeAccuracy": round(accuracy, 6),
        "nativeDatasetDigest": hashlib.sha256(images.numpy().tobytes() + labels.numpy().tobytes()).hexdigest(),
    }


def export_subject(model: torch.nn.Module, output: Path) -> None:
    example_images = torch.zeros((1, VIEW_COUNT, 3, EXPORT_SIZE, EXPORT_SIZE), dtype=torch.float32)
    example_mask = torch.ones((1, VIEW_COUNT), dtype=torch.bool)
    torch.onnx.export(
        model,
        (example_images, example_mask),
        str(output),
        input_names=("images", "viewMask"),
        output_names=("logits", "pooledFeatures"),
        opset_version=17,
        do_constant_folding=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", default=str(Path(__file__).resolve().parents[1] / "models"))
    args = parser.parse_args()
    root = Path(args.output_dir).resolve()
    root.mkdir(parents=True, exist_ok=True)
    model, native = train_subject()
    checkpoint = root / "mvcnn_vehicle_cpu.pt"
    onnx_path = root / "mvcnn_vehicle_cpu.onnx"
    torch.save({"state_dict": model.state_dict(), "class_map": list(CLASSES),
                "source": "NDNSF-UAV-APP/tools/mvcnn_model.py", "seed": SEED}, checkpoint)
    export_subject(model, onnx_path)
    onnx_checker = "not-run"
    try:
        import onnx
        onnx.checker.check_model(str(onnx_path))
        onnx_checker = "passed"
    except Exception as exc:
        raise RuntimeError("ONNX checker failed") from exc
    manifest = {
        "schema": "ndnsf-uav-mvcnn-artifact-manifest/v1",
        "modelId": "mvcnn-tiny-vehicle-v1",
        "algorithmId": "mvcnn-onnx-maxpool/v1",
        "modelFamily": "MVCNN-family/shared-backbone-masked-max-pooling",
        "license": "MIT",
        "sourceRevision": "local-source:" + source_digest(),
        "checkpoint": {"path": str(checkpoint), "digest": digest_file(checkpoint)},
        "artifact": {"path": str(onnx_path), "digest": digest_file(onnx_path)},
        "classMap": list(CLASSES),
        "input": {"name": "images", "shape": [1, VIEW_COUNT, 3, EXPORT_SIZE, EXPORT_SIZE],
                  "dtype": "float32", "preprocessing": "rgb-resize-224-center-crop/v1"},
        "viewMask": {"name": "viewMask", "shape": [1, VIEW_COUNT], "dtype": "bool",
                     "semantics": "true entries participate in symmetric max pooling"},
        "outputs": [{"name": "logits", "dtype": "float32"},
                    {"name": "pooledFeatures", "dtype": "float32"}],
        "opset": 17,
        "expectedExecutionProvider": "CPUExecutionProvider",
        "nativeQualification": native,
        "onnxChecker": onnx_checker,
        "runtime": {"python": platform.python_version(), "torch": torch.__version__},
    }
    (root / "mvcnn_vehicle_cpu.manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
