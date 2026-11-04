"""Strict portable configuration for the person model adapter, without execution state."""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
from types import MappingProxyType
from typing import Mapping, Tuple

MAX_PROFILE_BYTES = 16384
CAMERAS = ("UAV1", "UAV2", "UAV3")
LIMIT_BOUNDS = {
    "sampleHz": (2, 2),
    "maxPersonsPerFrame": (1, 64),
    "maxLocalKeys": (1, 1024),
    "maxBackendTracksPerCamera": (1, 512),
    "maxGlobalIdentities": (1, 256),
    "samplesPerIdentity": (1, 8),
    "galleryTtlUs": (1, 30_000_000),
    "minTransitionUs": (0, 30_000_000),
    "maxTransitionUs": (0, 30_000_000),
    "maxClockSkewUs": (0, 100_000),
    "maxCosineDistancePpm": (0, 1_000_000),
    "ambiguityMarginPpm": (0, 1_000_000),
    "reidBatchSize": (1, 32),
    "maxFrameBytes": (1, 4 * 1024 * 1024),
    "maxFrameWidth": (1, 1920),
    "maxFrameHeight": (1, 1080),
    "maxSnapshotBytes": (1, 8 * 1024 * 1024),
    "maxResultBytes": (1, 4 * 1024 * 1024),
    "maxQueuedWindows": (1, 2),
    "maxInFlightWindows": (1, 1),
    "workerTimeoutMs": (1, 30_000),
    "runDeadlineMs": (1, 900_000),
    "maxChildLogBytes": (1, 16 * 1024 * 1024),
    "maxRetainedTransactionWindows": (1, 120),
}
ROOT_KEYS = frozenset(("schema", "trackingMode", "cameras", "cameraRelations",
                       "detectorDigest", "reidDigest", "architecture", "featureDimension",
                       "preprocessId", "requestedDevice", "limits"))
LEGACY_DETECTOR_PREPROCESS_ID = "yolo-default640-stock-box-v1"
RAW_DETECTOR_PREPROCESS_ID = "yolo-letterbox1280-raw-box-v1"
NMS50_DETECTOR_PREPROCESS_ID = "yolo-letterbox1280-raw-box-nms50-v1"
GAP_REID_PREPROCESS_ID = "osnet-rgb-256x128-imagenet-l2-v1"
GEM_REID_PREPROCESS_ID = "osnet-rgb-256x128-imagenet-gem-p3-l2-v1"


@dataclass(frozen=True)
class PersonProfile:
    canonical_bytes: bytes
    digest: str
    detector_digest: str
    reid_digest: str
    requested_device: str
    limits: Mapping[str, int]
    overlapping_cameras: Mapping[Tuple[str, str], bool]
    detector_preprocess_id: str = LEGACY_DETECTOR_PREPROCESS_ID
    reid_preprocess_id: str = GAP_REID_PREPROCESS_ID


def _unsigned_integer(text):
    if text.startswith("-"):
        raise ValueError("profile integers must be unsigned")
    value = int(text)
    if value > 18446744073709551615:
        raise ValueError("profile integer overflow")
    return value


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate person profile field: " + key)
        result[key] = value
    return result


def _ascii_string(value):
    if not isinstance(value, str) or not value or len(value) > 128:
        raise ValueError("invalid profile string")
    if any(not 0x20 <= ord(ch) <= 0x7E for ch in value):
        raise ValueError("profile strings must be printable ASCII")
    return value


def _exact_keys(value, keys):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError("person profile fields do not match schema")


def parse_person_profile(payload: bytes) -> PersonProfile:
    """Parse the same bounded, integer-only schema used by the native authority."""
    if not payload or len(payload) > MAX_PROFILE_BYTES:
        raise ValueError("person profile is empty or oversized")
    try:
        value = json.loads(payload.decode("utf-8"), object_pairs_hook=_object,
                           parse_int=_unsigned_integer)
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid person profile JSON encoding/nesting") from error
    version_two = isinstance(value, dict) and value.get("schema") == "uav-person-tracking-profile-v2"
    _exact_keys(value, ROOT_KEYS | {"detectorPreprocessId"} if version_two else ROOT_KEYS)
    if _ascii_string(value["schema"]) not in ("uav-person-tracking-profile-v1",
                                             "uav-person-tracking-profile-v2"):
        raise ValueError("unsupported person profile: schema")
    detector_preprocess = LEGACY_DETECTOR_PREPROCESS_ID
    if version_two:
        detector_preprocess = _ascii_string(value["detectorPreprocessId"])
        if detector_preprocess not in (RAW_DETECTOR_PREPROCESS_ID, NMS50_DETECTOR_PREPROCESS_ID):
            raise ValueError("unsupported person profile: detectorPreprocessId")
    expected = {"trackingMode": "indoor-person-reid", "architecture": "osnet_x1_0"}
    for key, text in expected.items():
        if _ascii_string(value[key]) != text:
            raise ValueError("unsupported person profile: " + key)
    reid_preprocess = _ascii_string(value["preprocessId"])
    if reid_preprocess not in (GAP_REID_PREPROCESS_ID, GEM_REID_PREPROCESS_ID):
        raise ValueError("unsupported person profile: preprocessId")
    if type(value["featureDimension"]) is not int or value["featureDimension"] != 512:
        raise ValueError("OSNet feature dimension must be 512")
    for key in ("detectorDigest", "reidDigest"):
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", _ascii_string(value[key])):
            raise ValueError("invalid canonical model digest: " + key)
    device = _ascii_string(value["requestedDevice"])
    if device not in ("cpu", "cuda:0"):
        raise ValueError("requestedDevice must be cpu or cuda:0")
    if value["cameras"] != list(CAMERAS):
        raise ValueError("cameras must be UAV1,UAV2,UAV3 in order")
    relations = value["cameraRelations"]
    if not isinstance(relations, list) or len(relations) != 3:
        raise ValueError("three unordered camera relations are required")
    overlap = {}
    for relation in relations:
        _exact_keys(relation, ("cameraA", "cameraB", "relation"))
        a, b = _ascii_string(relation["cameraA"]), _ascii_string(relation["cameraB"])
        kind = _ascii_string(relation["relation"])
        if (a not in CAMERAS or b not in CAMERAS or a >= b or (a, b) in overlap
                or kind not in ("overlap", "non-overlap")):
            raise ValueError("invalid or duplicate camera pair")
        overlap[a, b] = kind == "overlap"
    limits = value["limits"]
    _exact_keys(limits, LIMIT_BOUNDS)
    for key, (minimum, maximum) in LIMIT_BOUNDS.items():
        number = limits[key]
        if type(number) is not int or not minimum <= number <= maximum:
            raise ValueError("person profile limit out of range: " + key)
    if (not limits["minTransitionUs"] <= limits["maxTransitionUs"] <= limits["galleryTtlUs"]
            or limits["workerTimeoutMs"] > limits["runDeadlineMs"]
            or limits["maxPersonsPerFrame"] > limits["maxBackendTracksPerCamera"]
            or limits["maxPersonsPerFrame"] > limits["maxLocalKeys"]):
        raise ValueError("inconsistent person profile time/capacity bounds")
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True, allow_nan=False).encode("ascii")
    return PersonProfile(canonical, "sha256:" + hashlib.sha256(canonical).hexdigest(),
                         value["detectorDigest"], value["reidDigest"], device,
                         MappingProxyType(dict(limits)), MappingProxyType(overlap),
                         detector_preprocess, reid_preprocess)


def load_person_profile(path: Path) -> PersonProfile:
    """Bound bytes before parsing rather than reading an arbitrary-size config file."""
    with Path(path).open("rb") as stream:
        payload = stream.read(MAX_PROFILE_BYTES + 1)
    return parse_person_profile(payload)


def _model_digest(path: Path) -> str:
    path = Path(path)
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError("model file must be a nonempty regular file: " + str(path))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def verify_person_model_files(profile: PersonProfile, detector: Path, reid: Path) -> None:
    """Check local model identity before backend loading, without running either model."""
    if (_model_digest(detector) != profile.detector_digest
            or _model_digest(reid) != profile.reid_digest):
        raise ValueError("person profile model digest mismatch")


def prepare_person_profile(template: Path, detector: Path, reid: Path) -> PersonProfile:
    """Bind the operator template to existing weight bytes; no model download or inference."""
    base = load_person_profile(template)
    value = json.loads(base.canonical_bytes)
    value["detectorDigest"] = _model_digest(detector)
    value["reidDigest"] = _model_digest(reid)
    return parse_person_profile(json.dumps(value).encode("ascii"))


def main(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--detector", type=Path, required=True)
    parser.add_argument("--reid", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        profile = prepare_person_profile(args.template, args.detector, args.reid)
        # The operator chooses an output file; refuse to overwrite an existing profile.
        with args.output.open("xb") as stream:
            stream.write(profile.canonical_bytes)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps({"profileDigest": profile.digest, "output": str(args.output)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
