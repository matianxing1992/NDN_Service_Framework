"""Spec 210 source-event writer shared by simulation, replay, and live push.

The writer owns only the local JSONL boundary. It never estimates motion or
promotes simulated values to measured provenance. Device adapters can emit the
same event records without changing tracking or geometry code.
"""

from __future__ import annotations

import argparse
import base64
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, BinaryIO, Iterable, Mapping


EVENT_SCHEMA = "uav-source-event-v2"
PROFILE_SCHEMA = "uav-observation-profile-v2"
LEGACY_PROFILE_SCHEMA = "uav-observation-demo-profile-v1"
SOURCE_GENERATION_SCHEMA = "uav-source-generations-v1"
CAMERAS = ("UAV1", "UAV2", "UAV3")
MAX_LINE_BYTES = 12 * 1024 * 1024
MAX_METADATA_BYTES = 64 * 1024
MAX_IMAGE_BYTES = 8 * 1024 * 1024
MAX_TIME_US = (1 << 63) - 1
MAX_NDN_VERSION = (1 << 64) - 1
_NDN_DECIMAL_URI_TYPES = {"seg": 50, "off": 52, "v": 54, "t": 56, "seq": 58}
_NDN_DIGEST_URI_TYPES = {"sha256digest": 1, "params-sha256": 2}
_NDN_DECIMAL_COMPONENT_TYPES = frozenset(_NDN_DECIMAL_URI_TYPES.values())

_INT_FIELDS = frozenset({
    "epoch", "sequence", "revision", "clockEpoch", "validFromUs", "validUntilUs",
    "uncertaintyUs", "width", "height", "sampleSequence", "deviceTimeUs",
    "calibrationRevision", "maxPoseGapUs", "maxClockUncertaintyUs", "maxAlignWaitMs",
    "poseRetentionUs", "maxPoseSamples", "maxQueuedFrames", "maxQueuedBytes",
    "maxSourceAgeMs", "firstSequence", "lastSequence",
})
_FLOAT_FIELDS = frozenset({
    "a", "bUs", "fx", "fy", "cx", "cy", "offsetM", "anchorSigmaPixels",
})
_NUMERIC_ARRAY_FIELDS = frozenset({
    "distortion", "normal", "roiVertices", "positionW", "qWC",
    "parameterCovariance", "poseCovariance",
})
_EVENT_FIELDS = {
    "gap": {
        "required": {"schema", "identity", "firstSequence", "lastSequence", "reason"},
        "optional": set(),
    },
    "calibration": {
        "required": {"schema", "cameraId", "revision", "width", "height", "fx", "fy",
                     "cx", "cy", "distortion", "imageTransform", "validFromUs", "validUntilUs"},
        "optional": {"parameterCovariance", "anchorSigmaPixels", "externalDataRef"},
    },
    "ground": {
        "required": {"schema", "revision", "worldFrameId", "normal", "offsetM",
                     "roiVertices", "geometryValidity"},
        "optional": {"parameterCovariance"},
    },
    "pose": {
        "required": {"schema", "identity", "sampleSequence", "deviceTimeUs", "clockBinding",
                     "positionW", "qWC", "calibrationRevision"},
        "optional": {"poseCovariance"},
    },
    "frame": {
        "required": {"key", "deviceTimeUs", "clockBinding", "width", "height", "encoding",
                     "calibrationRevision"},
        "optional": {"imageBytes", "imageBase64"},
    },
}


def _mapping(value: Any, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{label} must be a string-keyed object")
    return value


def _check_fields(value: Mapping[str, Any], required: set[str], optional: set[str], label: str) -> None:
    keys = set(value)
    if not required <= keys or keys - required - optional:
        raise ValueError(f"{label} has missing or unknown fields")


def _decode_canonical_ndn_component(component: str, label: str) -> bytes:
    unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"
    decoded = bytearray()
    index = 0
    while index < len(component):
        char = component[index]
        if char in unreserved:
            decoded.append(ord(char))
            index += 1
            continue
        if char != "%" or index + 2 >= len(component):
            raise ValueError(f"{label} contains an unescaped or malformed URI character")
        escaped = component[index + 1:index + 3]
        if any(digit not in "0123456789ABCDEF" for digit in escaped):
            raise ValueError(f"{label} contains a noncanonical percent escape")
        byte = int(escaped, 16)
        if chr(byte) in unreserved:
            raise ValueError(f"{label} percent-escapes an unreserved character")
        decoded.append(byte)
        index += 3
    if all(byte == ord(".") for byte in decoded):
        if len(decoded) < 3:
            raise ValueError(f"{label} contains a noncanonical dot component")
        del decoded[:3]
    return bytes(decoded)


def _validate_canonical_ndn_component(component: str, label: str) -> None:
    if "=" not in component:
        _decode_canonical_ndn_component(component, label)
        return

    prefix, encoded_value = component.split("=", 1)
    if prefix in _NDN_DECIMAL_URI_TYPES:
        number = encoded_value
        if not number.isascii() or not number.isdecimal() or \
                (len(number) > 1 and number.startswith("0")) or \
                len(number) > 20 or int(number) > MAX_NDN_VERSION:
            raise ValueError(f"{label} contains a noncanonical decimal typed component")
        return
    if prefix in _NDN_DIGEST_URI_TYPES:
        if len(encoded_value) != 64 or any(char not in "0123456789abcdef"
                                           for char in encoded_value):
            raise ValueError(f"{label} contains a noncanonical digest component")
        return

    if not prefix.isascii() or not prefix.isdecimal() or \
            (len(prefix) > 1 and prefix.startswith("0")) or len(prefix) > 5:
        raise ValueError(f"{label} contains an unsupported typed component")
    component_type = int(prefix)
    if not 1 <= component_type <= 65535 or component_type in (1, 2, 8):
        raise ValueError(f"{label} contains a noncanonical typed component")
    decoded = _decode_canonical_ndn_component(encoded_value, label)
    if component_type in _NDN_DECIMAL_COMPONENT_TYPES and len(decoded) in (1, 2, 4, 8):
        raise ValueError(f"{label} uses a noncanonical numeric typed component alias")


def _validate_canonical_ndn_name(value: str, label: str) -> None:
    if value == "/":
        raise ValueError(f"{label} must identify a non-empty NDN name")
    if not value.startswith("/") or value.endswith("/") or "//" in value:
        raise ValueError(f"{label} is not a canonical absolute NDN name")
    for component in value[1:].split("/"):
        _validate_canonical_ndn_component(component, label)


def _validate_external_data_ref(value: Any, *, camera_id: Any, revision: Any) -> None:
    reference = _mapping(value, "externalDataRef")
    _check_fields(reference, {"exactName", "contentSha256", "producerIdentity"}, set(),
                  "externalDataRef")
    exact_name = reference["exactName"]
    producer = reference["producerIdentity"]
    digest = reference["contentSha256"]
    for field, item in (("exactName", exact_name), ("producerIdentity", producer)):
        try:
            size = len(item.encode("utf-8")) if isinstance(item, str) else 0
        except UnicodeEncodeError as error:
            raise ValueError(f"externalDataRef.{field} must be valid UTF-8") from error
        if not isinstance(item, str) or not item.startswith("/") or "\0" in item or size > 4096:
            raise ValueError(f"externalDataRef.{field} must be a bounded absolute NDN name")
    _validate_canonical_ndn_name(producer, "externalDataRef.producerIdentity")
    if camera_id not in CAMERAS or type(revision) is not int or \
            not 1 <= revision <= MAX_TIME_US:
        raise ValueError("external calibration reference requires a valid camera and revision")
    producer_prefix = "" if producer == "/" else producer
    name_prefix = f"{producer_prefix}/UAV/CALIBRATION/{camera_id}/{revision}/v="
    if not exact_name.startswith(name_prefix):
        raise ValueError("externalDataRef.exactName must name this camera and calibration revision")
    version = exact_name[len(name_prefix):]
    if not version.isascii() or not version.isdecimal() or version.startswith("0") or \
            not 1 <= int(version) <= MAX_NDN_VERSION:
        raise ValueError("externalDataRef.exactName requires a canonical typed version")
    if not isinstance(digest, str) or len(digest) != 71 or not digest.startswith("sha256:") or \
            any(char not in "0123456789abcdef" for char in digest[7:]):
        raise ValueError("externalDataRef.contentSha256 must be a lowercase SHA-256 digest")


def _numeric_string(value: Any, *, integer: bool) -> str:
    if isinstance(value, bool):
        raise ValueError("boolean is not a numeric observation value")
    if integer:
        if not isinstance(value, int) or value < 0 or value > MAX_TIME_US:
            raise ValueError("integer observation value is outside its range")
        return str(value)
    if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise ValueError("observation decimal must be finite")
    return format(float(value), ".17g")


def _encode_value(value: Any, field: str = "") -> Any:
    if field in _INT_FIELDS:
        return _numeric_string(value, integer=True)
    if field in _FLOAT_FIELDS:
        return _numeric_string(value, integer=False)
    if field in _NUMERIC_ARRAY_FIELDS:
        def encode_array(item: Any) -> Any:
            if isinstance(item, list) or isinstance(item, tuple):
                return [encode_array(part) for part in item]
            return _numeric_string(item, integer=False)
        if not isinstance(value, (list, tuple)):
            raise ValueError(f"{field} must be an array")
        return encode_array(value)
    if isinstance(value, Mapping):
        obj = _mapping(value, "nested observation record")
        return {key: _encode_value(item, key) for key, item in obj.items()}
    if isinstance(value, (list, tuple)):
        return [_encode_value(item) for item in value]
    if isinstance(value, bool):
        raise ValueError("boolean leaves are not part of the observation wire schema")
    if isinstance(value, (int, float)):
        raise ValueError("numeric leaves require a typed observation field")
    if value is None:
        raise ValueError("null is not an observation source value")
    if not isinstance(value, str):
        raise ValueError("unsupported observation value")
    return value


def encode_event(kind: str, payload: Mapping[str, Any]) -> bytes:
    """Return one canonical UTF-8 JSONL record, including its newline."""
    if kind not in _EVENT_FIELDS:
        raise ValueError(f"unknown observation event kind: {kind}")
    value = _mapping(payload, "payload")
    event_fields = _EVENT_FIELDS[kind]
    _check_fields(value, event_fields["required"], event_fields["optional"], kind)

    normalized = dict(value)
    if kind == "calibration" and "externalDataRef" in normalized:
        _validate_external_data_ref(normalized["externalDataRef"],
                                    camera_id=normalized["cameraId"],
                                    revision=normalized["revision"])
    if kind == "gap":
        first, last = normalized["firstSequence"], normalized["lastSequence"]
        if normalized["schema"] != "uav-frame-gap-v2" or \
                normalized["reason"] not in ("CAPTURE_DROPPED", "QUEUE_FULL") or \
                type(first) is not int or type(last) is not int or not 1 <= first <= last <= MAX_TIME_US:
            raise ValueError("invalid source frame gap")
        identity = _mapping(normalized["identity"], "gap identity")
        _check_fields(identity, {"sourceId", "droneId", "cameraId", "epoch", "worldFrameId",
                                 "acquisitionKind", "deliveryMode"}, set(), "gap identity")
        for field in ("sourceId", "droneId", "cameraId", "worldFrameId"):
            text = identity[field]
            if not isinstance(text, str) or not text or "\0" in text or len(text.encode("utf-8")) > 128:
                raise ValueError("invalid gap identity")
        if type(identity["epoch"]) is not int or not 1 <= identity["epoch"] <= MAX_TIME_US or \
                identity["acquisitionKind"] not in ("simulated", "measured") or \
                identity["deliveryMode"] not in ("generated", "replay", "live-push"):
            raise ValueError("invalid gap identity")
    if kind == "frame":
        image_bytes = normalized.pop("imageBytes", None)
        image_base64 = normalized.get("imageBase64")
        if image_bytes is not None and image_base64 is not None:
            raise ValueError("frame must supply imageBytes or imageBase64, not both")
        if image_bytes is not None:
            if not isinstance(image_bytes, (bytes, bytearray, memoryview)):
                raise ValueError("imageBytes must be bytes")
            image_bytes = bytes(image_bytes)
            if not image_bytes or len(image_bytes) > MAX_IMAGE_BYTES:
                raise ValueError("imageBytes is empty or exceeds 8 MiB")
            normalized["imageBase64"] = base64.b64encode(image_bytes).decode("ascii")
        elif not isinstance(image_base64, str):
            raise ValueError("frame requires imageBytes or imageBase64")
        if "imageBase64" not in normalized:
            raise ValueError("frame image is missing")

    normalized = {key: _encode_value(item, key) for key, item in normalized.items()}
    if kind == "frame":
        encoded_image = normalized["imageBase64"]
        if len(encoded_image) % 4 != 0 or len(encoded_image) > 4 * ((MAX_IMAGE_BYTES + 2) // 3):
            raise ValueError("imageBase64 has an invalid or oversized length")
        try:
            decoded = base64.b64decode(encoded_image, validate=True)
        except (ValueError, base64.binascii.Error) as error:
            raise ValueError("imageBase64 is not canonical base64") from error
        if not decoded or len(decoded) > MAX_IMAGE_BYTES or base64.b64encode(decoded).decode("ascii") != encoded_image:
            raise ValueError("imageBase64 is not canonical or exceeds 8 MiB")
        metadata = {key: item for key, item in normalized.items() if key != "imageBase64"}
        metadata_size = len(json.dumps(metadata, ensure_ascii=False, sort_keys=True,
                                       separators=(",", ":")).encode("utf-8"))
        if metadata_size > MAX_METADATA_BYTES:
            raise ValueError("frame metadata exceeds 64 KiB")

    event = {"schema": EVENT_SCHEMA, "kind": kind, "payload": normalized}
    line = (json.dumps(event, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
                       allow_nan=False) + "\n").encode("utf-8")
    if len(line) > MAX_LINE_BYTES:
        raise ValueError("observation event exceeds 12 MiB")
    return line


def write_event(stream: BinaryIO, kind: str, payload: Mapping[str, Any]) -> None:
    """Write one validated event to a pipe or file and flush the live-push boundary."""
    line = encode_event(kind, payload)
    view = memoryview(line)
    offset = 0
    while offset < len(view):
        written = stream.write(view[offset:])
        if written is None:
            offset = len(view)
        elif written <= 0:
            raise OSError("short write to observation stream")
        else:
            offset += written
    flush = getattr(stream, "flush", None)
    if flush is not None:
        flush()


def load_observation_profile(path: Path) -> dict[str, Any]:
    """Load a common source profile for simulated or externally measured input."""
    return parse_observation_profile(path.read_bytes())


def parse_observation_profile(raw: bytes) -> dict[str, Any]:
    """Validate and normalize exactly the bytes used for configuration identity."""
    value = json.loads(raw)
    if not isinstance(value, dict) or value.get("schema") not in (
            PROFILE_SCHEMA, LEGACY_PROFILE_SCHEMA):
        raise ValueError("unsupported UAV observation profile")
    # Normalize only the owned parsed value; callers retain the original file
    # bytes and digest as the configuration provenance.
    value["schema"] = PROFILE_SCHEMA
    if value.get("acquisitionKind") not in ("simulated", "measured"):
        raise ValueError("acquisitionKind must be simulated or measured")
    if value.get("accuracyQualification") != "unvalidated":
        raise ValueError("source profile cannot claim an unverified accuracy qualification")
    if value.get("geometryValidity") not in ("illustrative", "calibrated-plane", "analytic-fixture"):
        raise ValueError("profile geometryValidity must be illustrative, calibrated-plane, or analytic-fixture")
    if not isinstance(value.get("cameras"), dict) or set(value["cameras"]) != {"UAV1", "UAV2", "UAV3"}:
        raise ValueError("profile must define exactly the three demo cameras")
    for camera_id, camera_profile in value["cameras"].items():
        if isinstance(camera_profile, Mapping):
            calibration = camera_profile.get("calibration")
            if isinstance(calibration, Mapping) and "externalDataRef" in calibration:
                _validate_external_data_ref(
                    calibration["externalDataRef"], camera_id=camera_id,
                    revision=calibration.get("revision", 1))
    if "frameCount" in value:
        count = value["frameCount"]
        if type(count) is not int or not 1 <= count <= 600:
            raise ValueError("profile frameCount must be an integer in 1..600")
    if "sampleRateHz" in value:
        rate = value["sampleRateHz"]
        if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not 1 <= rate <= 30:
            raise ValueError("profile sampleRateHz must be a number in 1..30")
    source = value.get("source")
    if not isinstance(source, dict) or set(source) != {"kind"} or \
            source.get("kind") not in ("video-reference", "external-fd"):
        raise ValueError("the profile must select video-reference or external-fd input")
    return value


def load_demo_profile(path: Path) -> dict[str, Any]:
    """Load the explicitly simulated, unqualified demonstration profile."""
    value = load_observation_profile(path)
    if value.get("acquisitionKind") != "simulated" or \
            value.get("geometryValidity") not in {"illustrative", "analytic-fixture"}:
        raise ValueError("the demo profile must remain simulated with illustrative geometry")
    return value


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("source generation manifest contains a duplicate field")
        result[key] = value
    return result


def load_source_generation_manifest(path: Path, window_count: int) -> dict[str, Any]:
    """Load a complete bounded source identity schedule for one run."""
    if not 1 <= window_count <= 600:
        raise ValueError("source generation window count must be in 1..600")
    if path.stat().st_size > MAX_METADATA_BYTES:
        raise ValueError("source generation manifest exceeds 64 KiB")
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=_unique_json_object)
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read source generation manifest: {error}") from error
    if not isinstance(value, dict) or set(value) != {"schema", "windowCount", "generations"} or \
            value.get("schema") != SOURCE_GENERATION_SCHEMA:
        raise ValueError("unsupported or malformed source generation manifest")
    if type(value.get("windowCount")) is not int or value["windowCount"] != window_count:
        raise ValueError("source generation manifest windowCount does not match the run")
    generations = value.get("generations")
    if not isinstance(generations, list) or not 1 <= len(generations) <= window_count:
        raise ValueError("source generation manifest has no bounded generation schedule")
    previous: dict[str, Any] | None = None
    for position, generation in enumerate(generations):
        if not isinstance(generation, dict) or set(generation) != {
                "firstWindowIndex", "sourceEpoch", "sourceIds"}:
            raise ValueError("source generation has missing or unknown fields")
        first = generation["firstWindowIndex"]
        epoch = generation["sourceEpoch"]
        source_ids = generation["sourceIds"]
        if type(first) is not int or not 0 <= first < window_count or \
                type(epoch) is not int or not 1 <= epoch <= (1 << 64) - 1 or \
                not isinstance(source_ids, dict) or set(source_ids) != set(CAMERAS):
            raise ValueError("source generation boundary, epoch, or camera set is invalid")
        if position == 0:
            if first != 0:
                raise ValueError("first source generation must start at window index zero")
        else:
            assert previous is not None
            if first <= previous["firstWindowIndex"] or \
                    previous["sourceEpoch"] == (1 << 64) - 1 or \
                    epoch != previous["sourceEpoch"] + 1:
                raise ValueError("source generation boundaries and epochs must advance by one")
        values = []
        for camera in CAMERAS:
            source_id = source_ids[camera]
            if not isinstance(source_id, str) or not source_id or len(source_id.encode("utf-8")) > 128 or \
                    any(ord(char) < 0x20 or ord(char) == 0x7f for char in source_id):
                raise ValueError("source generation contains an invalid sourceId")
            values.append(source_id)
        if len(set(values)) != len(values):
            raise ValueError("sourceIds must be unique per camera within a generation")
        if previous is not None and any(
                source_ids[camera] == previous["sourceIds"][camera] for camera in CAMERAS):
            raise ValueError("sourceId must change for every camera at a generation boundary")
        previous = generation
    return value


def source_generation_for_window(manifest: Mapping[str, Any], window_index: int) -> Mapping[str, Any]:
    """Return the generation active at one zero-based window index."""
    if type(window_index) is not int or not 0 <= window_index < int(manifest["windowCount"]):
        raise ValueError("window index is outside the source generation manifest")
    for generation in reversed(manifest["generations"]):
        if window_index >= int(generation["firstWindowIndex"]):
            return generation
    raise ValueError("source generation manifest has no initial generation")


def _identity(profile: Mapping[str, Any], camera: str, source_id: str, epoch: int) -> dict[str, Any]:
    camera_profile = profile["cameras"][camera]
    return {
        "sourceId": source_id,
        "droneId": camera_profile["droneId"],
        "cameraId": camera,
        "epoch": epoch,
        "worldFrameId": profile["worldFrameId"],
        "acquisitionKind": profile["acquisitionKind"],
        "deliveryMode": profile["deliveryMode"],
    }


def profile_events(profile: Mapping[str, Any], camera: str, sequence: int, capture_time_us: int,
                   image_bytes: bytes, *, source_id: str, epoch: int = 1,
                   source_time_origin_us: int = 0,
                   image_width: int | None = None,
                   image_height: int | None = None) -> Iterable[tuple[str, dict[str, Any]]]:
    """Build one source-owned frame bundle from simulated parameters and a decoded frame."""
    if camera not in profile.get("cameras", {}) or sequence <= 0 or epoch <= 0:
        raise ValueError("invalid source identity or frame sequence")
    camera_profile = profile["cameras"][camera]
    source_width = int(camera_profile["width"])
    source_height = int(camera_profile["height"])
    width = source_width if image_width is None else image_width
    height = source_height if image_height is None else image_height
    if width <= 0 or height <= 0:
        raise ValueError("decoded image dimensions must be positive")
    if capture_time_us < source_time_origin_us:
        raise ValueError("source PTS precedes its declared run-time origin")
    clock = dict(profile["clock"])
    clock["bUs"] = float(clock["bUs"]) - source_time_origin_us
    identity = _identity(profile, camera, source_id, epoch)
    calibration = dict(camera_profile["calibration"])
    scale_x = width / source_width
    scale_y = height / source_height
    calibration["fx"] = float(calibration["fx"]) * scale_x
    calibration["fy"] = float(calibration["fy"]) * scale_y
    calibration["cx"] = float(calibration["cx"]) * scale_x
    calibration["cy"] = float(calibration["cy"]) * scale_y
    covariance = calibration.get("parameterCovariance")
    if covariance is not None:
        scales = [scale_x, scale_y, scale_x, scale_y, 1.0, 1.0, 1.0, 1.0, 1.0]
        if len(covariance) != 9 or any(len(row) != 9 for row in covariance):
            raise ValueError("calibration parameterCovariance must be 9 by 9")
        calibration["parameterCovariance"] = [
            [float(covariance[i][j]) * scales[i] * scales[j] for j in range(9)]
            for i in range(9)
        ]
    if "anchorSigmaPixels" in profile:
        calibration["anchorSigmaPixels"] = float(profile["anchorSigmaPixels"])
    calibration.update({"schema": "uav-calibration-v2", "cameraId": camera})
    calibration["width"] = width
    calibration["height"] = height
    calibration.setdefault("revision", 1)
    calibration.setdefault("validFromUs", 0)
    calibration.setdefault("validUntilUs", MAX_TIME_US)
    if "externalDataRef" in calibration:
        _validate_external_data_ref(calibration["externalDataRef"], camera_id=camera,
                                    revision=calibration["revision"])
    ground = dict(profile["ground"])
    ground.setdefault("schema", "uav-ground-v2")
    ground.setdefault("revision", 1)
    ground.setdefault("worldFrameId", profile["worldFrameId"])
    yield "calibration", calibration
    yield "ground", ground

    run_time_us = capture_time_us - source_time_origin_us
    seconds = run_time_us / 1_000_000.0
    base = camera_profile["positionW"]
    velocity = camera_profile.get("velocityWmps", [0.0, 0.0, 0.0])
    position = [float(base[i]) + float(velocity[i]) * seconds for i in range(3)]
    pose = {
        "schema": "uav-pose-v2",
        "identity": identity,
        "sampleSequence": sequence,
        "deviceTimeUs": capture_time_us,
        "clockBinding": clock,
        "positionW": position,
        "qWC": list(camera_profile["qWC"]),
        "calibrationRevision": calibration["revision"],
    }
    if "poseCovariance" in camera_profile:
        pose["poseCovariance"] = camera_profile["poseCovariance"]
    yield "pose", pose

    frame = {
        "key": {"sourceId": source_id, "cameraId": camera, "epoch": epoch,
                "sequence": sequence},
        "deviceTimeUs": capture_time_us,
        "clockBinding": clock,
        "width": width,
        "height": height,
        "encoding": "image/jpeg",
        "calibrationRevision": calibration["revision"],
        "imageBytes": image_bytes,
    }
    yield "frame", frame


def write_profile_frame(stream: BinaryIO, profile: Mapping[str, Any], camera: str, sequence: int,
                        capture_time_us: int, image_bytes: bytes, *, source_id: str,
                        epoch: int = 1, source_time_origin_us: int = 0,
                        image_width: int | None = None,
                        image_height: int | None = None) -> None:
    for kind, payload in profile_events(profile, camera, sequence, capture_time_us,
                                        image_bytes, source_id=source_id, epoch=epoch,
                                        source_time_origin_us=source_time_origin_us,
                                        image_width=image_width, image_height=image_height):
        write_event(stream, kind, payload)


def push_directory(event_directory: Path, fifo_directory: Path, *,
                   camera: str, window_count: int) -> None:
    """Push canonical JSONL events into per-frame FIFOs with pipe backpressure."""
    if camera not in {"UAV1", "UAV2", "UAV3"} or not 1 <= window_count <= 600:
        raise ValueError("invalid camera or bounded push window count")
    print(f"UAV_OBSERVATION_PUSHER_READY camera={camera} windows={window_count}",
          flush=True)
    for window in range(window_count):
        source = event_directory / f"window-{window}.jsonl"
        target = fifo_directory / f"window-{window}.jsonl"
        with source.open("rb") as input_stream, target.open("wb", buffering=0) as output_stream:
            while True:
                line = input_stream.readline(MAX_LINE_BYTES + 1)
                if not line:
                    break
                if len(line) > MAX_LINE_BYTES or not line.endswith(b"\n"):
                    raise ValueError(f"invalid or oversized push event in {source}")
                offset = 0
                while offset < len(line):
                    written = output_stream.write(line[offset:])
                    if written is None or written <= 0:
                        raise OSError("observation FIFO closed during push")
                    offset += written


def _video_pts(video: Path, *, through_time_s: float) -> list[int]:
    if not math.isfinite(through_time_s) or through_time_s <= 0:
        raise ValueError("reference video probe interval must be finite and positive")
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-read_intervals", f"%+{through_time_s:.6f}",
         "-select_streams", "v:0", "-show_frames",
         "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(video)],
        check=False, capture_output=True, text=True)
    if probe.returncode != 0:
        raise ValueError(f"ffprobe failed for live reference source: {probe.stderr.strip()}")
    try:
        frames = json.loads(probe.stdout).get("frames", [])
        values = [
            int((Decimal(str(frame["best_effort_timestamp_time"])) * Decimal(1_000_000))
                .quantize(Decimal("1"), rounding=ROUND_HALF_UP))
            for frame in frames
        ]
    except (json.JSONDecodeError, KeyError, InvalidOperation, TypeError, ValueError) as error:
        raise ValueError("live reference video has missing or malformed frame PTS") from error
    if not values or any(right <= left for left, right in zip(values, values[1:])):
        raise ValueError("live reference video PTS is empty or non-monotonic")
    return values


def push_video(video: Path, profile_path: Path, fifo_directory: Path, *,
               camera: str, source_id: str, epoch: int, window_count: int,
               sample_rate_hz: float,
               source_generations_path: Path | None = None) -> None:
    """Decode and push one reference recording on demand through live FIFOs."""
    if camera not in {"UAV1", "UAV2", "UAV3"} or not 1 <= window_count <= 600:
        raise ValueError("invalid camera or bounded push window count")
    if epoch < 1 or not math.isfinite(sample_rate_hz) or not 1.0 <= sample_rate_hz <= 30.0:
        raise ValueError("invalid live reference source identity or rate")
    profile = load_demo_profile(profile_path)
    if profile["source"]["kind"] != "video-reference":
        raise ValueError("video pusher requires source.kind=video-reference")
    profile["deliveryMode"] = "live-push"
    generations = (load_source_generation_manifest(source_generations_path, window_count)
                   if source_generations_path is not None else None)
    generation_digest = (hashlib.sha256(source_generations_path.read_bytes()).hexdigest()
                         if source_generations_path is not None else "")
    period_us = int((Decimal(1_000_000) / Decimal(str(sample_rate_hz)))
                    .quantize(Decimal("1"), rounding=ROUND_HALF_UP))
    last_target_us = (window_count - 1) * period_us
    # Keep startup work proportional to the requested window horizon. A full
    # one-minute ffprobe frame scan can outlive MiniNDN's pusher-ready deadline
    # even when the demo only needs a few seconds of source frames.
    probe_margin_us = max(1_000_000, 2 * period_us)
    pts = _video_pts(video, through_time_s=(last_target_us + probe_margin_us) / 1_000_000)
    origin_us = pts[0]
    selected = []
    cursor = 0
    for window in range(window_count):
        target = window * period_us
        while cursor < len(pts) and pts[cursor] - origin_us < target:
            cursor += 1
        if cursor >= len(pts):
            raise ValueError(f"reference video is too short for {window_count} windows")
        selected.append(cursor)
        cursor += 1

    import cv2
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise ValueError(f"cannot open live reference video: {video}")
    ready = f"UAV_OBSERVATION_PUSHER_READY camera={camera} windows={window_count}"
    if generation_digest:
        ready += f" generation-manifest-sha256=sha256:{generation_digest}"
    print(ready, flush=True)
    decoded_index = -1
    try:
        for window, frame_index in enumerate(selected):
            frame = None
            while decoded_index < frame_index:
                ok, frame = capture.read()
                if not ok or frame is None:
                    raise ValueError(f"cannot decode reference frame {frame_index}")
                decoded_index += 1
            encoded, data = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            if not encoded or data.nbytes == 0:
                raise ValueError(f"cannot encode reference frame {window}")
            fifo = fifo_directory / f"window-{window}.jsonl"
            generation = (source_generation_for_window(generations, window)
                          if generations is not None else None)
            with fifo.open("wb", buffering=0) as output_stream:
                write_profile_frame(
                    output_stream, profile, camera, window + 1, pts[frame_index] - origin_us,
                    data.tobytes(),
                    source_id=(str(generation["sourceIds"][camera])
                               if generation is not None else source_id),
                    epoch=(int(generation["sourceEpoch"])
                           if generation is not None else epoch),
                    source_time_origin_us=0, image_width=int(frame.shape[1]),
                    image_height=int(frame.shape[0]))
    finally:
        capture.release()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--push-directory", type=Path)
    parser.add_argument("--push-video", type=Path)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--fifo-directory", type=Path, required=True)
    parser.add_argument("--camera", choices=("UAV1", "UAV2", "UAV3"), required=True)
    parser.add_argument("--window-count", type=int, required=True)
    parser.add_argument("--source-id")
    parser.add_argument("--source-epoch", type=int, default=1)
    parser.add_argument("--source-generations", type=Path,
                        help="uav-source-generations-v1 manifest shared by this run")
    parser.add_argument("--sample-rate-hz", type=float, default=5.0)
    args = parser.parse_args(argv)
    try:
        if args.push_video is not None:
            if args.profile is None or not args.source_id:
                raise ValueError("video push requires profile and source-id")
            push_video(args.push_video, args.profile, args.fifo_directory,
                       camera=args.camera, source_id=args.source_id,
                       epoch=args.source_epoch, window_count=args.window_count,
                       sample_rate_hz=args.sample_rate_hz,
                       source_generations_path=args.source_generations)
        elif args.push_directory is not None:
            push_directory(args.push_directory, args.fifo_directory,
                           camera=args.camera, window_count=args.window_count)
        else:
            raise ValueError("select --push-video or --push-directory")
        print(f"UAV_OBSERVATION_PUSHER_COMPLETE camera={args.camera}", flush=True)
        return 0
    except (OSError, ValueError) as error:
        print(f"UAV_OBSERVATION_PUSHER_FAILED camera={args.camera} error={error}",
              file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

