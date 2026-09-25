"""Canonical motion-input records for the Spec191 replay boundary.

The runner is allowed to accept human-friendly ``groundspeed_mps`` values,
but the application boundary carries one canonical integer unit: mm/s.  This
module deliberately contains no NDN or detector code; it is the small,
deterministic schema shared by the fixture, worker, and their tests.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from typing import Any, Mapping


def _finite(value: Any, name: str) -> float:
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{name} must be finite")
    return result


def canonical_speed_mmps(value: Any = None, *, groundspeed_mps: Any = None) -> int:
    """Normalize one speed and reject ambiguous or negative units."""
    if value is None and groundspeed_mps is None:
        raise ValueError("one of groundSpeedMmps or groundspeed_mps is required")
    if value is None:
        mmps = None
    else:
        if isinstance(value, bool):
            raise ValueError("groundSpeedMmps must be numeric, not boolean")
        numeric = _finite(value, "groundSpeedMmps")
        if numeric != int(numeric):
            raise ValueError("groundSpeedMmps must be an integer")
        mmps = int(numeric)
    if mmps is not None and mmps < 0:
        raise ValueError("groundSpeedMmps must be non-negative")
    if groundspeed_mps is not None:
        metres = _finite(groundspeed_mps, "groundspeed_mps")
        if metres < 0:
            raise ValueError("groundspeed_mps must be non-negative")
        converted = int(round(metres * 1000.0))
        if mmps is not None and abs(mmps - converted) > 1:
            raise ValueError("groundSpeedMmps and groundspeed_mps disagree")
        mmps = converted
    assert mmps is not None
    return mmps


@dataclass(frozen=True)
class TelemetrySample:
    drone_id: str
    camera_id: str
    source_epoch: int
    sequence: int
    source_timestamp_us: int
    latitude_e7: int
    longitude_e7: int
    altitude_mm: int
    heading_cdeg: int
    ground_speed_mmps: int
    freshness: str = "fresh"
    provenance: str = "simulated-input"

    def validate(self) -> None:
        if not self.drone_id or not self.camera_id:
            raise ValueError("telemetry identity is required")
        if self.source_epoch < 1 or self.sequence < 1 or self.source_timestamp_us < 0:
            raise ValueError("telemetry epoch, sequence, and timestamp are invalid")
        if not -900_000_000 <= self.latitude_e7 <= 900_000_000:
            raise ValueError("telemetry latitude is out of range")
        if not -1_800_000_000 <= self.longitude_e7 <= 1_800_000_000:
            raise ValueError("telemetry longitude is out of range")
        if self.altitude_mm < 0 or self.ground_speed_mmps < 0:
            raise ValueError("telemetry altitude and speed must be non-negative")
        if not 0 <= self.heading_cdeg < 36_000:
            raise ValueError("telemetry heading must be in [0, 36000)")
        if self.freshness not in {"fresh", "stale", "missing"}:
            raise ValueError("unsupported telemetry freshness")
        if not self.provenance:
            raise ValueError("telemetry provenance is required")

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "TelemetrySample":
        sample = cls(
            drone_id=str(value.get("droneId", "")),
            camera_id=str(value.get("cameraId", "")),
            source_epoch=int(value.get("sourceEpoch", 0)),
            sequence=int(value.get("sequence", 0)),
            source_timestamp_us=int(value.get("sourceTimestampUs", 0)),
            latitude_e7=int(value.get("latitudeE7", 0)),
            longitude_e7=int(value.get("longitudeE7", 0)),
            altitude_mm=int(value.get("altitudeMm", 0)),
            heading_cdeg=int(value.get("headingCdeg", 0)),
            ground_speed_mmps=canonical_speed_mmps(
                value.get("groundSpeedMmps"),
                groundspeed_mps=value.get("groundspeed_mps")),
            freshness=str(value.get("freshness", "fresh")),
            provenance=str(value.get("provenance", "simulated-input")),
        )
        sample.validate()
        return sample

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        return {
            "droneId": self.drone_id,
            "cameraId": self.camera_id,
            "sourceEpoch": self.source_epoch,
            "sequence": self.sequence,
            "sourceTimestampUs": self.source_timestamp_us,
            "latitudeE7": self.latitude_e7,
            "longitudeE7": self.longitude_e7,
            "altitudeMm": self.altitude_mm,
            "headingCdeg": self.heading_cdeg,
            "groundSpeedMmps": self.ground_speed_mmps,
            "freshness": self.freshness,
            "provenance": self.provenance,
        }


@dataclass(frozen=True)
class CalibrationProfile:
    camera_id: str
    image_width: int
    image_height: int
    homography: tuple[tuple[float, float, float],
                      tuple[float, float, float],
                      tuple[float, float, float]]
    coordinate_frame: str = "ground-mm-ned"
    distortion: str = "ignored-after-calibration"

    def validate(self) -> None:
        if not self.camera_id or self.image_width < 1 or self.image_height < 1:
            raise ValueError("calibration identity and image dimensions are required")
        if self.coordinate_frame != "ground-mm-ned":
            raise ValueError("unsupported calibration coordinate frame")
        if len(self.homography) != 3 or any(len(row) != 3 for row in self.homography):
            raise ValueError("homography must be 3x3")
        values = [item for row in self.homography for item in row]
        if any(not math.isfinite(item) for item in values):
            raise ValueError("homography must contain finite values")
        determinant = (
            self.homography[0][0] * (self.homography[1][1] * self.homography[2][2] -
                                     self.homography[1][2] * self.homography[2][1]) -
            self.homography[0][1] * (self.homography[1][0] * self.homography[2][2] -
                                     self.homography[1][2] * self.homography[2][0]) +
            self.homography[0][2] * (self.homography[1][0] * self.homography[2][1] -
                                     self.homography[1][1] * self.homography[2][0]))
        if determinant <= 1e-12:
            raise ValueError("homography is degenerate or reverses the ground frame")

    def digest(self) -> str:
        self.validate()
        payload = json.dumps(self.to_dict(include_digest=False), sort_keys=True,
                             separators=(",", ":")).encode()
        return "sha256:" + hashlib.sha256(payload).hexdigest()

    def to_dict(self, *, include_digest: bool = True) -> dict[str, Any]:
        self.validate()
        result: dict[str, Any] = {
            "cameraId": self.camera_id,
            "imageWidth": self.image_width,
            "imageHeight": self.image_height,
            "homography": [list(row) for row in self.homography],
            "coordinateFrame": self.coordinate_frame,
            "distortion": self.distortion,
        }
        if include_digest:
            result["calibrationDigest"] = self.digest()
        return result

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "CalibrationProfile":
        matrix = tuple(tuple(float(item) for item in row)
                       for row in value.get("homography", ()))
        profile = cls(str(value.get("cameraId", "")), int(value.get("imageWidth", 0)),
                      int(value.get("imageHeight", 0)), matrix,  # type: ignore[arg-type]
                      str(value.get("coordinateFrame", "ground-mm-ned")),
                      str(value.get("distortion", "ignored-after-calibration")))
        profile.validate()
        supplied = value.get("calibrationDigest")
        if supplied is not None and str(supplied) != profile.digest():
            raise ValueError("calibration digest mismatch")
        return profile


def load_motion_window(value: Mapping[str, Any]) -> tuple[list[TelemetrySample], dict[str, CalibrationProfile], dict[str, Any]]:
    """Validate one request-scoped window and return records plus oracle metadata."""
    if value.get("schema") != "spec191-motion-window-v1":
        raise ValueError("unsupported motion window schema")
    telemetry = [TelemetrySample.from_dict(item) for item in value.get("telemetry", [])]
    if len(telemetry) != 3:
        raise ValueError("one telemetry sample per UAV camera is required")
    cameras = tuple(sample.camera_id for sample in telemetry)
    if cameras != ("UAV1", "UAV2", "UAV3") or len(set(cameras)) != 3:
        raise ValueError("motion telemetry camera order is not canonical")
    timestamps = {sample.source_timestamp_us for sample in telemetry}
    if len(timestamps) != 1:
        raise ValueError("motion telemetry must share one source timestamp")
    calibrations = {
        str(item["cameraId"]): CalibrationProfile.from_dict(item)
        for item in value.get("calibrations", [])
    }
    if set(calibrations) != set(cameras):
        raise ValueError("one calibration profile per camera is required")
    oracle = dict(value.get("oracle", {}))
    oracle["provenance"] = "simulated-input"
    return telemetry, calibrations, oracle


def canonical_window_json(value: Mapping[str, Any]) -> str:
    """Return the digest-stable representation carried in the request."""
    load_motion_window(value)
    return json.dumps(value, sort_keys=True, separators=(",", ":"))
