"""Source-time, homography-based vehicle speed estimation for Spec191."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
from typing import Any, Mapping

from motion_schema import CalibrationProfile, TelemetrySample


@dataclass(frozen=True)
class VehicleSpeedEstimate:
    vehicle_id: str
    global_id: int
    from_pts_us: int
    to_pts_us: int
    pixel_anchors: tuple[tuple[float, float], tuple[float, float]]
    ground_points_mm: tuple[tuple[float, float], tuple[float, float]]
    distance_mm: float | None
    vehicle_speed_mmps: int | None
    provenance: str
    calibration_digest: str
    ego_motion_compensation: str
    uncertainty: str
    status: str

    def to_dict(self) -> dict[str, Any]:
        return {
            "vehicleId": self.vehicle_id,
            "globalId": self.global_id,
            "fromPtsUs": self.from_pts_us,
            "toPtsUs": self.to_pts_us,
            "pixelAnchors": [list(point) for point in self.pixel_anchors],
            "groundPointsMm": [list(point) for point in self.ground_points_mm],
            "distanceMm": self.distance_mm,
            "vehicleSpeedMmps": self.vehicle_speed_mmps,
            "unit": "mm/s",
            "provenance": self.provenance,
            "calibrationDigest": self.calibration_digest,
            "egoMotionCompensation": self.ego_motion_compensation,
            "uncertainty": self.uncertainty,
            "status": self.status,
        }


def _anchor(track: Mapping[str, Any]) -> tuple[float, float]:
    box = track.get("box")
    if not isinstance(box, (list, tuple)) or len(box) != 4:
        raise ValueError("track box must contain four coordinates")
    x1, y1, x2, y2 = (float(value) for value in box)
    values = (x1, y1, x2, y2)
    if any(not math.isfinite(value) for value in values) or x2 < x1 or y2 < y1:
        raise ValueError("track box is invalid")
    return ((x1 + x2) / 2.0, y2)


def _project(profile: CalibrationProfile, pixel: tuple[float, float]) -> tuple[float, float]:
    h = profile.homography
    x, y = pixel
    denominator = h[2][0] * x + h[2][1] * y + h[2][2]
    if abs(denominator) < 1e-12:
        raise ValueError("homography projection has zero denominator")
    return ((h[0][0] * x + h[0][1] * y + h[0][2]) / denominator,
            (h[1][0] * x + h[1][1] * y + h[1][2]) / denominator)


def _gps_mm(sample: TelemetrySample, reference: TelemetrySample) -> tuple[float, float]:
    # Equirectangular local conversion is adequate for the small deterministic
    # fixture.  It documents the frame instead of treating GPS degrees as mm.
    meters_lat = 111_320.0
    meters_lon = meters_lat * math.cos(math.radians(reference.latitude_e7 / 1e7))
    return ((sample.longitude_e7 - reference.longitude_e7) / 1e7 * meters_lon * 1000.0,
            (sample.latitude_e7 - reference.latitude_e7) / 1e7 * meters_lat * 1000.0)


def _world_point(local: tuple[float, float], sample: TelemetrySample,
                 reference: TelemetrySample) -> tuple[float, float]:
    position = _gps_mm(sample, reference)
    theta = math.radians(sample.heading_cdeg / 100.0)
    rotated = (local[0] * math.cos(theta) - local[1] * math.sin(theta),
               local[0] * math.sin(theta) + local[1] * math.cos(theta))
    return position[0] + rotated[0], position[1] + rotated[1]


def _unknown(vehicle_id: str, global_id: int, from_pts: int, to_pts: int,
             anchors: tuple[tuple[float, float], tuple[float, float]],
             profile: CalibrationProfile | None, reason: str) -> VehicleSpeedEstimate:
    digest = profile.digest() if profile is not None else ""
    return VehicleSpeedEstimate(vehicle_id, global_id, from_pts, to_pts, anchors,
                                ((0.0, 0.0), (0.0, 0.0)), None, None,
                                "unknown", digest, "not-applied", reason, "unknown")


def estimate_vehicle_speed(previous_track: Mapping[str, Any], current_track: Mapping[str, Any],
                           previous_telemetry: TelemetrySample,
                           current_telemetry: TelemetrySample,
                           profile: CalibrationProfile,
                           *, require_ego_compensation: bool = True) -> VehicleSpeedEstimate:
    """Estimate one object's speed; invalid inputs return an explicit unknown."""
    anchors = (_anchor(previous_track), _anchor(current_track))
    vehicle_id = str(current_track.get("vehicleId", current_track.get("globalId", "unknown")))
    global_id = int(current_track.get("globalId", 0))
    from_pts = int(previous_track.get("ptsUs", 0))
    to_pts = int(current_track.get("ptsUs", 0))
    try:
        profile.validate()
        previous_telemetry.validate()
        current_telemetry.validate()
        if previous_telemetry.camera_id != current_telemetry.camera_id:
            raise ValueError("telemetry camera mismatch")
        if previous_telemetry.source_epoch != current_telemetry.source_epoch:
            raise ValueError("telemetry epoch mismatch")
        if previous_telemetry.freshness != "fresh" or current_telemetry.freshness != "fresh":
            raise ValueError("telemetry is stale")
        if to_pts <= from_pts or current_telemetry.source_timestamp_us <= previous_telemetry.source_timestamp_us:
            raise ValueError("source PTS is not strictly increasing")
        if current_telemetry.source_timestamp_us != to_pts:
            raise ValueError("current telemetry is not source-time aligned")
        if previous_telemetry.source_timestamp_us != from_pts:
            raise ValueError("previous telemetry is not source-time aligned")
        if not require_ego_compensation:
            raise ValueError("UAV ego motion compensation is disabled")
        local_a = _project(profile, anchors[0])
        local_b = _project(profile, anchors[1])
        reference = previous_telemetry
        world_a = _world_point(local_a, previous_telemetry, reference)
        world_b = _world_point(local_b, current_telemetry, reference)
        uav_a = _gps_mm(previous_telemetry, reference)
        uav_b = _gps_mm(current_telemetry, reference)
        ego_dx, ego_dy = uav_b[0] - uav_a[0], uav_b[1] - uav_a[1]
        if require_ego_compensation and (not math.isfinite(ego_dx) or not math.isfinite(ego_dy)):
            raise ValueError("UAV ego motion is unavailable")
        corrected = (world_b[0] - world_a[0] - ego_dx,
                     world_b[1] - world_a[1] - ego_dy)
        distance = math.hypot(*corrected)
        delta_us = to_pts - from_pts
        speed = int(round(distance * 1_000_000.0 / delta_us))
        return VehicleSpeedEstimate(vehicle_id, global_id, from_pts, to_pts, anchors,
                                    (world_a, world_b), distance, speed,
                                    "homography-estimated", profile.digest(),
                                    "applied",
                                    "fixture-calibrated", "ok")
    except (ValueError, OverflowError, ZeroDivisionError) as error:
        return _unknown(vehicle_id, global_id, from_pts, to_pts, anchors, profile, str(error))
