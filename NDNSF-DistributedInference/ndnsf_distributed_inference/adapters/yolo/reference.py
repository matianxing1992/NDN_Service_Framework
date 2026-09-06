"""Spec180 fixed-input numerical oracle; NumPy only, no model execution.

Hashes bind this component to package bytes, not to an independent trust root.
The qualification collector must additionally bind the package to its candidate.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import io
import json
from pathlib import Path

import numpy as np


FIXTURE_PATH = "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm"
ATOL = 1e-3
RTOL = 1e-4


def _digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _fixture_array(data: bytes, input_size: int) -> np.ndarray:
    # P3 comments extend to end of line (not merely to the next token).
    tokens = " ".join(line.split("#", 1)[0] for line in data.decode("ascii").splitlines()).split()
    if len(tokens) < 4 or tokens[0] != "P3" or input_size <= 0:
        raise ValueError("INVALID_FIXED_FIXTURE")
    width, height, maximum = map(int, tokens[1:4])
    pixels = np.asarray([int(value) for value in tokens[4:]], dtype=np.float32)
    if (width <= 0 or height <= 0 or maximum != 255
            or pixels.size != width * height * 3
            or np.any(pixels < 0) or np.any(pixels > 255)):
        raise ValueError("INVALID_FIXED_FIXTURE")
    image = pixels.reshape(height, width, 3) / np.float32(255)

    def coordinates(length):
        # PyTorch bilinear align_corners=False, half-pixel centers, edge clamp.
        position = (np.arange(input_size, dtype=np.float32) + np.float32(.5)) * np.float32(length / input_size) - np.float32(.5)
        position = np.maximum(position, np.float32(0))
        lower = position.astype(np.int64)
        return lower, np.minimum(lower + 1, length - 1), (position - lower).astype(np.float32)

    y0, y1, wy = coordinates(height)
    x0, x1, wx = coordinates(width)
    top = image[y0[:, None], x0] * (1 - wx)[None, :, None] + image[y0[:, None], x1] * wx[None, :, None]
    bottom = image[y1[:, None], x0] * (1 - wx)[None, :, None] + image[y1[:, None], x1] * wx[None, :, None]
    resized = top * (1 - wy)[:, None, None] + bottom * wy[:, None, None]
    return np.ascontiguousarray(resized.transpose(2, 0, 1)[None], dtype=np.float32)


def fixture_tensor(path: Path, input_size: int) -> np.ndarray:
    return _fixture_array(Path(path).read_bytes(), input_size)


def _canonical_rows(array: np.ndarray) -> np.ndarray:
    values = np.asarray(array)
    if (values.dtype != np.float32 or values.ndim != 3
            or values.shape[0] != 1 or values.shape[2] != 6
            or not np.isfinite(values).all()):
        raise ValueError("INVALID_DETECTION_TENSOR")
    confidence, classes = values[0, :, 4], values[0, :, 5]
    if (np.any(confidence < 0) or np.any(confidence > 1)
            or np.any(classes < 0) or np.any(classes >= 80)
            or np.any(classes != np.floor(classes))):
        raise ValueError("INVALID_DETECTION_ROWS")
    values = values[:, confidence >= .001, :]
    order = np.lexsort((values[0, :, 3], values[0, :, 2], values[0, :, 1],
                       values[0, :, 0], values[0, :, 5], -values[0, :, 4]))
    return values[:, order, :]


@dataclass(frozen=True)
class YoloReference:
    input_tensor: np.ndarray
    expected: np.ndarray
    manifest_digest: str
    oracle_digest: str
    fixture_digest: str


def load_reference(package: Path, repository: Path, input_size: int) -> YoloReference:
    package = Path(package).resolve()
    manifest_bytes = (package / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    fixture, oracle = manifest["fixture"], manifest["oracle"]
    shape = [1, 3, 640, 640]
    if (input_size != 640 or fixture["path"] != FIXTURE_PATH
            or fixture["revision"] != "spec180-fixed-fixture-v1"
            or oracle["fixtureRevision"] != fixture["revision"]
            or oracle["fixtureSha256"] != fixture["sha256"]
            or oracle["inputShape"] != shape or oracle["outputDtype"] != "float32"
            or oracle["outputPath"] != "full-model-output.npy"
            or manifest["preprocessing"] != {
                "identity": "float32-NCHW-RGB-0-to-1", "inputName": "images", "shape": shape}
            or manifest["postprocessing"] != {
                "identity": "YOLO26n-canonical-detection-rows", "outputName": "predictions",
                "confidenceThreshold": .001, "sort": "confidence-desc,class-asc,xyxy-asc"}):
        raise ValueError("REFERENCE_SUBJECT_MISMATCH")
    oracle_path = (package / "oracle" / oracle["outputPath"]).resolve()
    if package not in oracle_path.parents:
        raise ValueError("REFERENCE_PATH_ESCAPE")
    fixture_path = (Path(repository).resolve() / FIXTURE_PATH).resolve()
    if Path(repository).resolve() not in fixture_path.parents:
        raise ValueError("FIXTURE_PATH_ESCAPE")
    fixture_bytes, oracle_bytes = fixture_path.read_bytes(), oracle_path.read_bytes()
    if (_digest(fixture_bytes) != "sha256:" + fixture["sha256"]
            or _digest(oracle_bytes) != oracle["outputDigest"]):
        raise ValueError("REFERENCE_DIGEST_MISMATCH")
    expected = np.load(io.BytesIO(oracle_bytes), allow_pickle=False)
    canonical = _canonical_rows(expected)
    if (list(expected.shape) != oracle["outputShape"] or expected.shape[1] == 0
            or not np.array_equal(expected, canonical)):
        raise ValueError("INVALID_CANONICAL_ORACLE")
    tensor = _fixture_array(fixture_bytes, input_size)
    tensor.setflags(write=False)
    canonical.setflags(write=False)
    return YoloReference(tensor, canonical, _digest(manifest_bytes),
                         _digest(oracle_bytes), _digest(fixture_bytes))


def compare_reference(reference: YoloReference, actual: np.ndarray) -> dict:
    actual = _canonical_rows(actual)
    expected = reference.expected
    same_shape = actual.shape == expected.shape
    # Float64 arithmetic prevents overflow in error reporting for finite float32.
    errors = np.abs(actual.astype(np.float64) - expected.astype(np.float64)) if same_shape else None
    return {
        "matched": bool(same_shape and np.array_equal(actual[..., 5], expected[..., 5])
                        and np.allclose(actual, expected, atol=ATOL, rtol=RTOL)),
        "shape": list(actual.shape), "expectedShape": list(expected.shape),
        "maxAbsError": float(errors.max()) if same_shape else None,
        "atol": ATOL, "rtol": RTOL,
    }
