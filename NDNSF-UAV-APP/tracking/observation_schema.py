"""Strict Spec210 worker-side observation and verified input contracts."""

from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import json
import math
from pathlib import Path
import re
from typing import Any, Mapping


OBSERVATION_SCHEMA = "uav-observation-v2"
VERIFIED_INPUT_SCHEMA = "uav-verified-inputs-v2"
CAMERAS = ("UAV1", "UAV2", "UAV3")
MAX_OBSERVATION_BYTES = 64 * 1024
MAX_IMAGE_BYTES = 8 * 1024 * 1024


def _object(value: Any, label: str, required: set[str], optional: set[str] = frozenset()) -> dict[str, Any]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{label} must be an object")
    if not required <= set(value) or set(value) - required - optional:
        raise ValueError(f"{label} has missing or unknown fields")
    return value


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"JSON object contains duplicate field: {key}")
        result[key] = value
    return result


def _json_loads(data: bytes | str, label: str) -> Any:
    try:
        text = data.decode("utf-8") if isinstance(data, bytes) else data
        return json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{label} is invalid JSON") from error


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} must be a non-empty string")
    return value


def _identity_string(value: Any, label: str) -> str:
    result = _string(value, label)
    try:
        size = len(result.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ValueError(f"{label} must be valid UTF-8") from error
    if size > 128:
        raise ValueError(f"{label} exceeds 128 UTF-8 bytes")
    return result


def _digest(value: Any, label: str) -> str:
    value = _string(value, label)
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", value):
        raise ValueError(f"{label} must be a lowercase SHA-256 digest")
    return value


def _data_ref(value: Any, label: str) -> dict[str, str]:
    value = _object(value, label, {"exactName", "contentSha256", "producerIdentity"})
    return {
        "exactName": _string(value["exactName"], f"{label}.exactName"),
        "contentSha256": _digest(value["contentSha256"], f"{label}.contentSha256"),
        "producerIdentity": _string(value["producerIdentity"], f"{label}.producerIdentity"),
    }


def _integer(value: Any, label: str, *, positive: bool = False) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{label} must be an integer")
    try:
        if isinstance(value, str) and value.isdecimal():
            result = int(value)
        elif isinstance(value, int):
            result = value
        else:
            raise ValueError
    except ValueError as error:
        raise ValueError(f"{label} must be an integer") from error
    if result < (1 if positive else 0) or result > (1 << 63) - 1:
        raise ValueError(f"{label} is outside its range")
    return result


def _number(value: Any, label: str) -> float:
    if isinstance(value, bool):
        raise ValueError(f"{label} must be numeric")
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{label} must be numeric") from error
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    return result


def _vector(value: Any, length: int, label: str) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != length:
        raise ValueError(f"{label} must contain {length} values")
    return tuple(_number(item, label) for item in value)


def _matrix(value: Any, size: int, label: str) -> tuple[float, ...] | None:
    if value is None:
        return None
    if not isinstance(value, list):
        raise ValueError(f"{label} must be a matrix or null")
    if len(value) == size * size and all(not isinstance(item, list) for item in value):
        flat = tuple(_number(item, label) for item in value)
    elif len(value) == size and all(isinstance(row, list) and len(row) == size for row in value):
        flat = tuple(_number(item, label) for row in value for item in row)
    else:
        raise ValueError(f"{label} must be {size} by {size}")
    for row in range(size):
        for col in range(size):
            if abs(flat[row * size + col] - flat[col * size + row]) > 1e-8:
                raise ValueError(f"{label} must be symmetric")
        if flat[row * size + row] < -1e-9:
            raise ValueError(f"{label} must have nonnegative diagonal")
    matrix = [list(flat[row * size:(row + 1) * size]) for row in range(size)]
    # Jacobi diagonalization is small and deterministic for the 4x4, 6x6, and
    # 9x9 source covariance matrices. A positive diagonal alone is not enough
    # to establish positive semidefiniteness.
    for _ in range(size * size * 20):
        p, q, largest = 0, 1, 0.0
        for row in range(size):
            for col in range(row + 1, size):
                magnitude = abs(matrix[row][col])
                if magnitude > largest:
                    p, q, largest = row, col, magnitude
        scale = max(1.0, max(abs(matrix[i][i]) for i in range(size)))
        if largest <= 1e-13 * scale:
            break
        angle = 0.5 * math.atan2(2.0 * matrix[p][q], matrix[q][q] - matrix[p][p])
        cosine, sine = math.cos(angle), math.sin(angle)
        app, aqq, apq = matrix[p][p], matrix[q][q], matrix[p][q]
        matrix[p][p] = cosine * cosine * app - 2.0 * sine * cosine * apq + sine * sine * aqq
        matrix[q][q] = sine * sine * app + 2.0 * sine * cosine * apq + cosine * cosine * aqq
        matrix[p][q] = matrix[q][p] = 0.0
        for index in range(size):
            if index in (p, q):
                continue
            aip, aiq = matrix[index][p], matrix[index][q]
            matrix[index][p] = matrix[p][index] = cosine * aip - sine * aiq
            matrix[index][q] = matrix[q][index] = sine * aip + cosine * aiq
    if min(matrix[index][index] for index in range(size)) < -1e-9:
        raise ValueError(f"{label} must be positive semidefinite")
    return flat


def _validate_roi(vertices: list[Any], normal: tuple[float, ...]) -> None:
    points = [tuple(_number(value, "ground ROI vertex") for value in vertex)
              for vertex in vertices]
    origin = points[0]
    edge = tuple(points[1][axis] - origin[axis] for axis in range(3))
    edge_length = math.sqrt(sum(value * value for value in edge))
    if edge_length <= 1e-9:
        raise ValueError("ground ROI contains a degenerate edge")
    u = tuple(value / edge_length for value in edge)
    v = (normal[1] * u[2] - normal[2] * u[1],
         normal[2] * u[0] - normal[0] * u[2],
         normal[0] * u[1] - normal[1] * u[0])
    points_2d = []
    for point in points:
        delta = tuple(point[axis] - origin[axis] for axis in range(3))
        if abs(sum(delta[axis] * normal[axis] for axis in range(3))) > 1e-4:
            raise ValueError("ground ROI vertices must lie on one plane")
        points_2d.append((sum(delta[axis] * u[axis] for axis in range(3)),
                          sum(delta[axis] * v[axis] for axis in range(3))))
    signs = []
    for index, a in enumerate(points_2d):
        b, c = points_2d[(index + 1) % len(points_2d)], points_2d[(index + 2) % len(points_2d)]
        cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])
        if abs(cross) <= 1e-9:
            raise ValueError("ground ROI must be strictly convex")
        signs.append(cross > 0)
    if any(sign != signs[0] for sign in signs):
        raise ValueError("ground ROI must be convex")


@dataclass(frozen=True)
class FrameObservation:
    raw: dict[str, Any]
    source_id: str
    camera_id: str
    source_epoch: int
    sequence: int
    capture_time_us: int
    time_uncertainty_us: int
    width: int
    height: int
    sidecar_digest: str = ""
    image_digest: str = ""
    sidecar_name: str = ""
    image_name: str = ""
    producer_identity: str = ""

    @classmethod
    def parse(cls, raw: bytes | str | Mapping[str, Any]) -> "FrameObservation":
        if isinstance(raw, bytes):
            if not 1 <= len(raw) <= MAX_OBSERVATION_BYTES:
                raise ValueError("observation sidecar is empty or too large")
            value = _json_loads(raw, "observation")
        elif isinstance(raw, str):
            if not 1 <= len(raw.encode("utf-8")) <= MAX_OBSERVATION_BYTES:
                raise ValueError("observation sidecar is empty or too large")
            value = _json_loads(raw, "observation")
        else:
            value = dict(raw)
        value = _object(value, "observation", {
            "schema", "identity", "key", "captureTimeUs", "timeUncertaintyUs", "width",
            "height", "image", "clockBinding", "calibration", "ground", "poseStatus",
            "positionW", "qWC", "poseCovariance", "anchorSigmaPixels", "geometryValidity",
            "reason",
        }, {
            "imageRef", "clockRef", "calibrationRef", "groundRef", "poseSampleRefs",
            "poseSamples",
        })
        if value["schema"] != OBSERVATION_SCHEMA:
            raise ValueError("unsupported observation schema")
        identity = _object(value["identity"], "identity", {
            "sourceId", "droneId", "cameraId", "epoch", "worldFrameId",
            "acquisitionKind", "deliveryMode",
        })
        key = _object(value["key"], "frame key", {"sourceId", "cameraId", "epoch", "sequence"})
        camera = _identity_string(identity["cameraId"], "cameraId")
        if camera not in CAMERAS or key["cameraId"] != camera:
            raise ValueError("observation camera identity does not match its frame key")
        source_id = _identity_string(identity["sourceId"], "sourceId")
        _identity_string(identity["droneId"], "droneId")
        _identity_string(identity["worldFrameId"], "worldFrameId")
        if identity["acquisitionKind"] not in {"simulated", "measured"}:
            raise ValueError("acquisitionKind is invalid")
        if identity["deliveryMode"] not in {"generated", "replay", "live-push"}:
            raise ValueError("deliveryMode is invalid")
        source_epoch = _integer(identity["epoch"], "source epoch", positive=True)
        if key["sourceId"] != source_id or _integer(key["epoch"], "frame epoch", positive=True) != source_epoch:
            raise ValueError("observation source identity does not match its frame key")
        sequence = _integer(key["sequence"], "frame sequence", positive=True)
        capture_time = _integer(value["captureTimeUs"], "captureTimeUs")
        uncertainty = _integer(value["timeUncertaintyUs"], "timeUncertaintyUs")
        width = _integer(value["width"], "frame width", positive=True)
        height = _integer(value["height"], "frame height", positive=True)
        image = _object(value["image"], "image", {"encoding", "contentSha256"})
        if image["encoding"] != "image/jpeg":
            raise ValueError("observation image identity is invalid")
        _digest(image["contentSha256"], "observation image digest")
        clock = _object(value["clockBinding"], "clockBinding", {
            "revision", "clockId", "clockEpoch", "a", "bUs", "validFromUs",
            "validUntilUs", "uncertaintyUs",
        })
        _string(clock["clockId"], "clockId")
        _integer(clock["revision"], "clock revision", positive=True)
        _integer(clock["clockEpoch"], "clock epoch", positive=True)
        if _number(clock["a"], "clock scale") <= 0:
            raise ValueError("clock scale must be positive")
        _number(clock["bUs"], "clock offset")
        if _integer(clock["validFromUs"], "clock validFromUs") > _integer(
                clock["validUntilUs"], "clock validUntilUs"):
            raise ValueError("clock validity interval is inverted")

        calibration = value["calibration"]
        if calibration is not None:
            calibration = _object(calibration, "calibration", {
                "schema", "cameraId", "revision", "width", "height", "fx", "fy", "cx",
                "cy", "distortion", "imageTransform", "validFromUs", "validUntilUs",
                "anchorSigmaPixels", "parameterCovariance",
            })
            if calibration["schema"] != "uav-calibration-v2" or calibration["cameraId"] != camera:
                raise ValueError("calibration identity does not match frame camera")
            if _integer(calibration["width"], "calibration width", positive=True) != width or \
                    _integer(calibration["height"], "calibration height", positive=True) != height:
                raise ValueError("calibration dimensions do not match frame")
            for name in ("fx", "fy"):
                if _number(calibration[name], name) <= 0:
                    raise ValueError(f"{name} must be positive")
            for name in ("cx", "cy"):
                _number(calibration[name], name)
            _integer(calibration["revision"], "calibration revision", positive=True)
            _vector(calibration["distortion"], 5, "distortion")
            if calibration["imageTransform"] not in {"raw", "rectified"}:
                raise ValueError("unsupported calibration image transform")
            if _integer(calibration["validFromUs"], "calibration validFromUs") > _integer(
                    calibration["validUntilUs"], "calibration validUntilUs"):
                raise ValueError("calibration validity interval is inverted")
            _matrix(calibration["parameterCovariance"], 9, "calibration covariance")
            if calibration["anchorSigmaPixels"] is not None and \
                    _number(calibration["anchorSigmaPixels"], "anchorSigmaPixels") < 0:
                raise ValueError("anchorSigmaPixels cannot be negative")

        ground = value["ground"]
        if ground is not None:
            ground = _object(ground, "ground", {
                "schema", "revision", "worldFrameId", "normal", "offsetM", "roiVertices",
                "geometryValidity", "parameterCovariance",
            })
            if ground["schema"] != "uav-ground-v2" or ground["worldFrameId"] != identity["worldFrameId"]:
                raise ValueError("ground reference identity mismatch")
            if ground["geometryValidity"] not in {
                    "illustrative", "calibrated-plane", "analytic-fixture"}:
                raise ValueError("ground geometryValidity is invalid")
            _integer(ground["revision"], "ground revision", positive=True)
            normal = _vector(ground["normal"], 3, "ground normal")
            if abs(math.sqrt(sum(item * item for item in normal)) - 1.0) > 1e-3:
                raise ValueError("ground normal must be unit length")
            _number(ground["offsetM"], "ground offset")
            vertices = ground["roiVertices"]
            if not isinstance(vertices, list) or not 3 <= len(vertices) <= 32:
                raise ValueError("ground ROI requires 3..32 vertices")
            for vertex in vertices:
                _vector(vertex, 3, "ground ROI vertex")
            _validate_roi(vertices, normal)
            _matrix(ground["parameterCovariance"], 4, "ground covariance")

        if value["poseStatus"] not in {"aligned", "unknown"}:
            raise ValueError("poseStatus is invalid")
        if value["poseStatus"] == "aligned":
            _vector(value["positionW"], 3, "positionW")
            q = _vector(value["qWC"], 4, "qWC")
            if abs(math.sqrt(sum(item * item for item in q)) - 1.0) > 1e-3:
                raise ValueError("qWC must be unit length")
        elif value["positionW"] is not None or value["qWC"] is not None:
            raise ValueError("unknown pose cannot contain a usable position or quaternion")
        _matrix(value["poseCovariance"], 6, "pose covariance")
        if value["poseStatus"] == "unknown" and value["poseCovariance"] is not None:
            raise ValueError("unknown pose cannot carry an applicable pose covariance")
        if value["anchorSigmaPixels"] is not None and \
                _number(value["anchorSigmaPixels"], "anchorSigmaPixels") < 0:
            raise ValueError("anchorSigmaPixels cannot be negative")
        geometry_validity = _string(value["geometryValidity"], "geometryValidity")
        if geometry_validity not in {"illustrative", "calibrated-plane", "analytic-fixture"}:
            raise ValueError("geometryValidity is invalid")
        if ground is not None and ground["geometryValidity"] != geometry_validity:
            raise ValueError("ground and observation geometryValidity do not match")
        if not isinstance(value["reason"], str):
            raise ValueError("observation reason must be a string")
        reference_fields = {"imageRef", "clockRef", "calibrationRef", "groundRef", "poseSampleRefs"}
        has_references = bool(reference_fields & set(value))
        if has_references:
            if not reference_fields <= set(value):
                raise ValueError("observation DataRef closure is incomplete")
            refs = {
                "imageRef": _data_ref(value["imageRef"], "imageRef"),
                "clockRef": _data_ref(value["clockRef"], "clockRef"),
            }
            if refs["imageRef"]["contentSha256"] != image["contentSha256"]:
                raise ValueError("imageRef digest does not match image metadata")
            for ref_name, record_name in (("calibrationRef", "calibration"),
                                          ("groundRef", "ground")):
                raw_ref = value[ref_name]
                record = value[record_name]
                if (raw_ref is None) != (record is None):
                    raise ValueError(f"{ref_name} and {record_name} must both be present or null")
                if raw_ref is not None:
                    refs[ref_name] = _data_ref(raw_ref, ref_name)
            if not isinstance(value["poseSamples"], list) or not isinstance(value["poseSampleRefs"], list):
                raise ValueError("poseSamples and poseSampleRefs must be arrays")
            if len(value["poseSamples"]) != len(value["poseSampleRefs"]):
                raise ValueError("poseSamples and poseSampleRefs have different lengths")
            if value["poseStatus"] == "aligned" and len(value["poseSamples"]) not in {1, 2}:
                raise ValueError("aligned pose requires one sample or two interpolation endpoints")
            if value["poseStatus"] == "unknown" and value["poseSamples"]:
                raise ValueError("unknown pose cannot carry pose sample references")
            refs["poseSampleRefs"] = [
                _data_ref(item, f"poseSampleRefs[{index}]")
                for index, item in enumerate(value["poseSampleRefs"])
            ]
            all_refs = [ref for ref in refs.values() if isinstance(ref, dict)]
            all_refs.extend(refs["poseSampleRefs"])
            producers = {ref["producerIdentity"] for ref in all_refs}
            if len(producers) != 1:
                raise ValueError("observation DataRefs must have one producer identity")
        return cls(value, source_id, camera, source_epoch, sequence, capture_time,
                   uncertainty, width, height)


def _wire_observation(raw: bytes) -> dict[str, Any]:
    if not 1 <= len(raw) <= MAX_OBSERVATION_BYTES:
        raise ValueError("observation sidecar is empty or too large")
    value = _object(_json_loads(raw, "observation sidecar"), "FrameObservation", {
        "schema", "identity", "key", "captureTimeUs", "timeUncertaintyUs", "width", "height",
        "imageRef", "clockRef", "poseStatus", "poseSampleRefs", "geometryValidity", "reason",
    }, {"calibrationRef", "groundRef", "positionW", "qWC", "poseCovariance"})
    if value["schema"] != OBSERVATION_SCHEMA:
        raise ValueError("unsupported observation schema")
    identity = _object(value["identity"], "identity", {
        "sourceId", "droneId", "cameraId", "epoch", "worldFrameId", "acquisitionKind", "deliveryMode",
    })
    key = _object(value["key"], "frame key", {"sourceId", "cameraId", "epoch", "sequence"})
    camera = _string(identity["cameraId"], "cameraId")
    source_id = _string(identity["sourceId"], "sourceId")
    if camera not in CAMERAS or key["cameraId"] != camera or key["sourceId"] != source_id:
        raise ValueError("observation identity does not match its frame key")
    if identity["acquisitionKind"] not in {"simulated", "measured"}:
        raise ValueError("acquisitionKind is invalid")
    if identity["deliveryMode"] not in {"generated", "replay", "live-push"}:
        raise ValueError("deliveryMode is invalid")
    epoch = _integer(identity["epoch"], "source epoch", positive=True)
    if _integer(key["epoch"], "frame epoch", positive=True) != epoch:
        raise ValueError("observation epoch does not match its frame key")
    _integer(key["sequence"], "frame sequence", positive=True)
    _string(identity["droneId"], "droneId")
    _string(identity["worldFrameId"], "worldFrameId")
    _integer(value["captureTimeUs"], "captureTimeUs")
    _integer(value["timeUncertaintyUs"], "timeUncertaintyUs")
    _integer(value["width"], "frame width", positive=True)
    _integer(value["height"], "frame height", positive=True)
    _data_ref(value["imageRef"], "imageRef")
    _data_ref(value["clockRef"], "clockRef")
    for name in ("calibrationRef", "groundRef"):
        if name in value:
            _data_ref(value[name], name)
    references = value["poseSampleRefs"]
    if not isinstance(references, list):
        raise ValueError("poseSampleRefs must be an array")
    for index, reference in enumerate(references):
        _data_ref(reference, f"poseSampleRefs[{index}]")
    if value["poseStatus"] == "aligned":
        if not {"positionW", "qWC"} <= set(value) or len(references) not in {1, 2}:
            raise ValueError("aligned pose requires an estimate and one or two pose references")
        _vector(value["positionW"], 3, "positionW")
        quaternion = _vector(value["qWC"], 4, "qWC")
        if abs(math.sqrt(sum(component * component for component in quaternion)) - 1.0) > 1e-3:
            raise ValueError("qWC must be unit length")
        if "poseCovariance" in value:
            if value["poseCovariance"] is None:
                raise ValueError("pose covariance must be omitted when unknown")
            _matrix(value["poseCovariance"], 6, "pose covariance")
    elif value["poseStatus"] == "unknown":
        if references or {"positionW", "qWC", "poseCovariance"} & set(value):
            raise ValueError("unknown pose cannot carry a pose estimate or pose references")
        if not _string(value["reason"], "unknown pose reason").strip():
            raise ValueError("unknown pose requires a reason")
    else:
        raise ValueError("poseStatus is invalid")
    if value["geometryValidity"] not in {"illustrative", "calibrated-plane", "analytic-fixture"}:
        raise ValueError("geometryValidity is invalid")
    if not isinstance(value["reason"], str):
        raise ValueError("observation reason must be a string")
    return value


def _component_suffix(name: str, producer: str, category: str) -> list[str]:
    prefix = producer.rstrip("/") + "/"
    if not producer.startswith("/") or not name.startswith(prefix):
        raise ValueError(f"{category} exact name is outside its declared producer namespace")
    suffix = name[len(prefix):].split("/")
    if any(not component for component in suffix):
        raise ValueError(f"{category} exact name has an empty component")
    return suffix


def _positive_name_uint64(component: str) -> bool:
    return (len(component) <= 20 and re.fullmatch(r"[1-9][0-9]*", component) is not None
            and int(component) <= (1 << 64) - 1)


def _round_away_from_zero(value: float) -> int:
    return math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)


def _slerp(left: tuple[float, ...], right: tuple[float, ...], amount: float) -> tuple[float, ...]:
    dot = sum(a * b for a, b in zip(left, right))
    if dot < 0:
        right = tuple(-value for value in right)
        dot = -dot
    dot = min(1.0, max(-1.0, dot))
    if dot > 0.9995:
        result = tuple(a + amount * (b - a) for a, b in zip(left, right))
    else:
        angle = math.acos(dot)
        denominator = math.sin(angle)
        a_weight = math.sin((1.0 - amount) * angle) / denominator
        b_weight = math.sin(amount * angle) / denominator
        result = tuple(a_weight * a + b_weight * b for a, b in zip(left, right))
    norm = math.sqrt(sum(value * value for value in result))
    return tuple(value / norm for value in result)


def _validate_pose_alignment(wire: Mapping[str, Any], poses: list[dict[str, Any]],
                             clock: Mapping[str, Any], camera: str,
                             calibration: Mapping[str, Any] | None) -> None:
    if wire["poseStatus"] == "unknown":
        return
    identity = wire["identity"]
    capture_time = _integer(wire["captureTimeUs"], "captureTimeUs")
    frame_calibration = None
    mapped: list[tuple[int, dict[str, Any]]] = []
    for index, pose in enumerate(poses):
        pose = _object(pose, f"pose sample {index}", {
            "schema", "identity", "sampleSequence", "deviceTimeUs", "clockBinding",
            "positionW", "qWC", "calibrationRevision",
        }, {"poseCovariance"})
        pose_identity = _object(pose["identity"], f"pose sample {index} identity", {
            "sourceId", "droneId", "cameraId", "epoch", "worldFrameId", "acquisitionKind", "deliveryMode",
        })
        identity_mismatch = any(
            pose_identity[key] != identity[key] for key in identity if key != "epoch")
        identity_mismatch = identity_mismatch or _integer(
            pose_identity["epoch"], "pose source epoch", positive=True) != _integer(
                identity["epoch"], "observation source epoch", positive=True)
        if pose["schema"] != "uav-pose-v2" or identity_mismatch or pose_identity["cameraId"] != camera:
            raise ValueError(f"pose sample {index} identity does not match {camera} observation")
        _integer(pose["sampleSequence"], "pose sample sequence", positive=True)
        device_time = _integer(pose["deviceTimeUs"], "pose deviceTimeUs")
        if not (_integer(clock["validFromUs"], "clock validFromUs") <= device_time <=
                _integer(clock["validUntilUs"], "clock validUntilUs")):
            raise ValueError(f"pose sample {index} is outside its clock validity interval")
        pose_clock = _object(pose["clockBinding"], f"pose sample {index} clockBinding", {
            "revision", "clockId", "clockEpoch", "a", "bUs", "validFromUs", "validUntilUs", "uncertaintyUs",
        })
        if pose_clock != clock:
            raise ValueError(f"pose sample {index} uses a different clock binding")
        a = _number(clock["a"], "clock scale")
        b = _number(clock["bUs"], "clock offset")
        run_time = _round_away_from_zero(a * device_time + b)
        mapped.append((run_time, pose))
        _vector(pose["positionW"], 3, f"pose sample {index} positionW")
        q = _vector(pose["qWC"], 4, f"pose sample {index} qWC")
        if abs(math.sqrt(sum(value * value for value in q)) - 1.0) > 1e-3:
            raise ValueError(f"pose sample {index} qWC must be unit length")
        _integer(pose["calibrationRevision"], "pose calibration revision", positive=True)
        pose_calibration_revision = _integer(
            pose["calibrationRevision"], "pose calibration revision", positive=True)
        if calibration is not None:
            if pose_calibration_revision != _integer(
                    calibration["revision"], "calibration revision", positive=True):
                raise ValueError(f"pose sample {index} calibration revision does not match observation")
        if "poseCovariance" in pose:
            if pose["poseCovariance"] is None:
                raise ValueError(f"pose sample {index} covariance must be omitted when unknown")
            _matrix(pose["poseCovariance"], 6, "pose sample covariance")
    if _integer(clock["uncertaintyUs"], "clock uncertainty") > 20_000:
        raise ValueError("pose clock uncertainty exceeds 20 ms")
    if len(mapped) == 1:
        if mapped[0][0] != capture_time:
            raise ValueError("exact pose sample does not match observation capture time")
        expected_position = _vector(mapped[0][1]["positionW"], 3, "pose positionW")
        expected_quaternion = _vector(mapped[0][1]["qWC"], 4, "pose qWC")
    else:
        (left_time, left), (right_time, right) = mapped
        if not left_time < capture_time < right_time or right_time - left_time > 100_000:
            raise ValueError("pose references do not bracket capture time within 100 ms")
        if left["calibrationRevision"] != right["calibrationRevision"]:
            raise ValueError("pose interpolation crosses a calibration revision")
        amount = (capture_time - left_time) / (right_time - left_time)
        left_position = _vector(left["positionW"], 3, "left pose positionW")
        right_position = _vector(right["positionW"], 3, "right pose positionW")
        expected_position = tuple(a + amount * (b - a) for a, b in zip(left_position, right_position))
        expected_quaternion = _slerp(
            _vector(left["qWC"], 4, "left pose qWC"),
            _vector(right["qWC"], 4, "right pose qWC"), amount)
    actual_position = _vector(wire["positionW"], 3, "positionW")
    actual_quaternion = _vector(wire["qWC"], 4, "qWC")
    if any(abs(a - b) > 1e-6 for a, b in zip(actual_position, expected_position)):
        raise ValueError("observation positionW does not match its pose references")
    quaternion_dot = abs(sum(a * b for a, b in zip(actual_quaternion, expected_quaternion)))
    if 1.0 - quaternion_dot > 1e-7:
        raise ValueError("observation qWC does not match its pose references")


def _verify_reference_closure(input_dir: Path, camera: str, producer: str,
                              wire: Mapping[str, Any], entry: Mapping[str, Any],
                              image_digest: str) -> FrameObservation:
    """Verify exact sidecar refs and raw component bytes, then build local worker input."""
    epoch = _integer(wire["identity"]["epoch"], "source epoch", positive=True)
    sequence = _integer(wire["key"]["sequence"], "frame sequence", positive=True)
    prefix = f"{producer}/UAV/OBS/{camera}/{epoch}/{sequence}"
    expected_observation_name = f"{prefix}/OBSERVATION/v=1"
    expected_image_name = f"{prefix}/FRAME/v=1"
    if entry["observationName"] != expected_observation_name or entry["imageName"] != expected_image_name:
        raise ValueError(f"observation or image exact name mismatch for {camera}")
    image_ref = wire["imageRef"]
    if (image_ref["exactName"] != expected_image_name or
            image_ref["contentSha256"] != image_digest or image_ref["producerIdentity"] != producer):
        raise ValueError(f"image DataRef does not match its producer object for {camera}")

    expected: list[tuple[str, dict[str, str]]] = [("CLOCK", wire["clockRef"])]
    if "calibrationRef" in wire:
        expected.append(("CALIBRATION", wire["calibrationRef"]))
    if "groundRef" in wire:
        expected.append(("GROUND", wire["groundRef"]))
    expected.extend(("POSE", reference) for reference in wire["poseSampleRefs"])
    references = entry["references"]
    if not isinstance(references, list) or len(references) != len(expected):
        raise ValueError(f"verified reference closure is incomplete for {camera}")

    seen_names = {expected_image_name, expected_observation_name}
    category_indexes: dict[str, int] = {}
    decoded_records: dict[str, list[dict[str, Any]]] = {}
    for index, (item, (category, data_ref)) in enumerate(zip(references, expected)):
        item = _object(item, f"{camera} reference {index}", {
            "category", "exactName", "contentSha256", "producerIdentity", "fileName",
        })
        if item["category"] != category:
            raise ValueError(f"reference category/order mismatch for {camera}")
        exact_name = _string(item["exactName"], "reference exactName")
        content_digest = _digest(item["contentSha256"], "reference contentSha256")
        reference_producer = _string(item["producerIdentity"], "reference producerIdentity")
        if (exact_name != data_ref["exactName"] or content_digest != data_ref["contentSha256"] or
                reference_producer != data_ref["producerIdentity"]):
            raise ValueError(f"manifest does not match {category} DataRef for {camera}")
        if category != "CALIBRATION" and reference_producer != producer:
            raise ValueError(f"{category} reference must be owned by {camera} source")
        suffix = _component_suffix(exact_name, reference_producer, category)
        if category in {"CLOCK", "CALIBRATION", "GROUND"}:
            semantic_prefix = (["UAV", category, camera] if category == "CALIBRATION"
                               else ["UAV", category])
            valid_shape = (
                len(suffix) == len(semantic_prefix) + 2 and
                suffix[:-2] == semantic_prefix and
                _positive_name_uint64(suffix[-2]) and
                suffix[-1].startswith("v=") and
                _positive_name_uint64(suffix[-1][2:])
            )
        else:
            valid_shape = (len(suffix) == 7 and suffix[:5] == ["UAV", "OBS", camera, str(epoch), str(sequence)]
                           and suffix[5] == "POSE" and suffix[6].startswith("v="))
        if not valid_shape:
            raise ValueError(f"{category} exact name has invalid typed structure for {camera}")
        category_index = category_indexes.get(category, 0)
        category_indexes[category] = category_index + 1
        expected_file = f"{camera}.reference-{category}-{category_index}.json"
        if item["fileName"] != expected_file:
            raise ValueError(f"unsafe or unexpected reference filename for {camera}")
        if exact_name in seen_names:
            raise ValueError(f"duplicate exact Data name in {camera} reference closure")
        seen_names.add(exact_name)
        payload = (input_dir / expected_file).read_bytes()
        if not 1 <= len(payload) <= MAX_OBSERVATION_BYTES:
            raise ValueError(f"{category} reference size is invalid for {camera}")
        actual_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        if actual_digest != content_digest:
            raise ValueError(f"{category} reference digest mismatch for {camera}")
        decoded = _json_loads(payload, f"{category} reference")
        if not isinstance(decoded, dict) or any(not isinstance(key, str) for key in decoded):
            raise ValueError(f"{category} reference must be a JSON object for {camera}")
        decoded_records.setdefault(category, []).append(decoded)
        if category == "CLOCK":
            revision = _integer(decoded.get("revision"), "clock revision", positive=True)
            expected_name = f"{reference_producer}/UAV/CLOCK/{revision}/{suffix[-1]}"
        elif category == "CALIBRATION":
            revision = _integer(decoded.get("revision"), "calibration revision", positive=True)
            expected_name = f"{reference_producer}/UAV/CALIBRATION/{camera}/{revision}/{suffix[-1]}"
        elif category == "GROUND":
            revision = _integer(decoded.get("revision"), "ground revision", positive=True)
            expected_name = f"{reference_producer}/UAV/GROUND/{revision}/{suffix[-1]}"
        else:
            sample_sequence = _integer(decoded.get("sampleSequence"), "pose sample sequence", positive=True)
            expected_name = f"{reference_producer}/UAV/OBS/{camera}/{epoch}/{sequence}/POSE/v={sample_sequence}"
        if exact_name != expected_name:
            raise ValueError(f"{category} exact name does not match its record identity for {camera}")

    clock_records = decoded_records.get("CLOCK", [])
    if len(clock_records) != 1:
        raise ValueError(f"clock reference closure must contain one record for {camera}")
    calibration_records = decoded_records.get("CALIBRATION", [])
    ground_records = decoded_records.get("GROUND", [])
    pose_records = decoded_records.get("POSE", [])
    if len(calibration_records) != (1 if "calibrationRef" in wire else 0) or \
            len(ground_records) != (1 if "groundRef" in wire else 0):
        raise ValueError(f"optional reference count mismatch for {camera}")
    calibration = calibration_records[0] if calibration_records else None
    ground = ground_records[0] if ground_records else None
    _validate_pose_alignment(wire, pose_records, clock_records[0], camera, calibration)
    reason = wire["reason"]
    if calibration is None and "CALIBRATION_MISSING" not in reason:
        raise ValueError(f"observation is missing its CALIBRATION_MISSING reason for {camera}")
    if ground is None and "GROUND_MISSING" not in reason:
        raise ValueError(f"observation is missing its GROUND_MISSING reason for {camera}")
    internal = {key: value for key, value in wire.items() if not key.endswith("Ref") and key != "poseSampleRefs"}
    internal.update({
        "image": {"encoding": "image/jpeg", "contentSha256": image_digest},
        "clockBinding": clock_records[0], "calibration": calibration, "ground": ground,
        "poseSamples": pose_records,
        "positionW": wire.get("positionW"), "qWC": wire.get("qWC"),
        "poseCovariance": wire.get("poseCovariance"),
        "anchorSigmaPixels": calibration.get("anchorSigmaPixels") if calibration else None,
    })
    return FrameObservation.parse(internal)


def load_verified_observation_bundle(input_dir: Path, manifest_path: Path) -> dict[str, FrameObservation]:
    """Verify the native fetch manifest and exact per-camera image/sidecar/reference bytes."""
    manifest_bytes = manifest_path.read_bytes()
    if len(manifest_bytes) > MAX_OBSERVATION_BYTES:
        raise ValueError("verified observation manifest exceeds 64 KiB")
    manifest = _json_loads(manifest_bytes, "verified observation manifest")
    manifest = _object(manifest, "verified input manifest", {"schema", "sourceEpoch", "entries"})
    if manifest["schema"] != VERIFIED_INPUT_SCHEMA:
        raise ValueError("unsupported verified input manifest schema")
    source_epoch = _integer(manifest["sourceEpoch"], "manifest source epoch", positive=True)
    entries = manifest["entries"]
    if not isinstance(entries, list) or len(entries) != len(CAMERAS):
        raise ValueError("verified input manifest must contain exactly three cameras")
    result: dict[str, FrameObservation] = {}
    for entry in entries:
        entry = _object(entry, "verified input entry", {
            "cameraId", "sourceId", "producerIdentity", "imageName", "imageDigest",
            "observationName", "observationDigest",
        }, {"references"})
        camera = _string(entry["cameraId"], "cameraId")
        if camera not in CAMERAS or camera in result:
            raise ValueError("verified input manifest has duplicate/unknown camera")
        producer = f"/example/uav/drone/{camera}"
        if entry["producerIdentity"] != producer:
            raise ValueError("verified input producer identity mismatch")
        for name in ("imageName", "observationName"):
            if not isinstance(entry[name], str) or not entry[name].startswith(producer + "/UAV/"):
                raise ValueError(f"{name} is not in the source producer namespace")
        image_path = input_dir / f"{camera}.jpg"
        image = image_path.read_bytes()
        if not 1 <= len(image) <= MAX_IMAGE_BYTES:
            raise ValueError(f"verified image size is invalid for {camera}")
        image_digest = "sha256:" + hashlib.sha256(image).hexdigest()
        if image_digest != entry["imageDigest"]:
            raise ValueError(f"verified image digest mismatch for {camera}")
        observation_path = input_dir / f"{camera}.observation.json"
        observation_bytes = observation_path.read_bytes()
        observation_digest = "sha256:" + hashlib.sha256(observation_bytes).hexdigest()
        if observation_digest != entry["observationDigest"]:
            raise ValueError(f"verified observation digest mismatch for {camera}")
        wire = _wire_observation(observation_bytes)
        if (wire["identity"]["cameraId"] != camera or
                wire["identity"]["sourceId"] != entry["sourceId"] or
                _integer(wire["identity"]["epoch"], "source epoch", positive=True) != source_epoch):
            raise ValueError(f"observation identity mismatch for {camera}")
        if entry["imageDigest"] != image_digest:
            raise ValueError(f"verified input image manifest digest mismatch for {camera}")
        if "references" not in entry:
            raise ValueError(f"verified input reference closure is missing for {camera}")
        observation = _verify_reference_closure(input_dir, camera, producer, wire, entry, image_digest)
        result[camera] = replace(
            observation, sidecar_digest=observation_digest, image_digest=image_digest,
            sidecar_name=entry["observationName"], image_name=entry["imageName"],
            producer_identity=producer)
    if set(result) != set(CAMERAS):
        raise ValueError("verified input manifest camera set is incomplete")
    return result
