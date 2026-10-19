"""Source-time, homography-based vehicle speed estimation for Spec191."""

from __future__ import annotations

from dataclasses import dataclass
from copy import deepcopy
import hashlib
import math
from typing import Any, Mapping, Sequence

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


@dataclass(frozen=True)
class GroundEstimate:
    """World-ground anchor and range from the camera optical center."""

    position_w: tuple[float, float, float]
    camera_position_w: tuple[float, float, float]
    horizontal_range_m: float
    slant_range_m: float
    pixel_covariance: tuple[float, ...] | None
    system_covariance: tuple[float, ...] | None
    uncertainty_kind: str
    range_sigma_m: float | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "positionW": list(self.position_w),
            "horizontalRangeM": self.horizontal_range_m,
            "slantRangeM": self.slant_range_m,
            "uncertaintyKind": self.uncertainty_kind,
            "sigmaRangeM": self.range_sigma_m,
        }


def _cross(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _rotate(q: Sequence[float], vector: Sequence[float]) -> tuple[float, float, float]:
    w, x, y, z = q
    qv = (x, y, z)
    uv = _cross(qv, vector)
    uuv = _cross(qv, uv)
    return tuple(vector[i] + 2.0 * (w * uv[i] + uuv[i]) for i in range(3))


def _multiply_quaternion(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float, float]:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw)


def _number(value: Any, label: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{label} is not numeric") from error
    if not math.isfinite(result):
        raise ValueError(f"{label} is not finite")
    return result


def _undistort(pixel: tuple[float, float], calibration: Mapping[str, Any]) -> tuple[float, float]:
    fx, fy = _number(calibration["fx"], "fx"), _number(calibration["fy"], "fy")
    cx, cy = _number(calibration["cx"], "cx"), _number(calibration["cy"], "cy")
    xd, yd = (pixel[0] - cx) / fx, (pixel[1] - cy) / fy
    if calibration.get("imageTransform", "raw") == "rectified":
        return xd, yd
    coefficients = [_number(value, "distortion") for value in calibration["distortion"]]
    k1, k2, p1, p2, k3 = coefficients
    x, y = xd, yd
    for _ in range(8):
        r2 = x * x + y * y
        radial = 1.0 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2
        if not math.isfinite(radial) or abs(radial) < 1e-12:
            raise ValueError("DISTORTION_SINGULAR")
        dx = 2.0 * p1 * x * y + p2 * (r2 + 2.0 * x * x)
        dy = p1 * (r2 + 2.0 * y * y) + 2.0 * p2 * x * y
        x, y = (xd - dx) / radial, (yd - dy) / radial
    return x, y


def project_world_point_to_pixel(point_w: Sequence[float],
                                observation: Mapping[str, Any]) -> tuple[float, float]:
    """Project one ENU point into the current camera image using its pose/model."""
    calibration = observation.get("calibration")
    if not isinstance(calibration, Mapping):
        raise ValueError("CALIBRATION_MISSING")
    position = tuple(_number(value, "positionW") for value in observation["positionW"])
    quaternion = tuple(_number(value, "qWC") for value in observation["qWC"])
    point = tuple(_number(value, "world point") for value in point_w)
    if len(point) != 3 or len(position) != 3 or len(quaternion) != 4 or \
            not math.isclose(sum(value * value for value in quaternion), 1.0,
                             rel_tol=0.0, abs_tol=1e-6):
        raise ValueError("INVALID_QUATERNION_OR_POSITION")
    delta = tuple(point[index] - position[index] for index in range(3))
    inverse = (quaternion[0], -quaternion[1], -quaternion[2], -quaternion[3])
    camera = _rotate(inverse, delta)
    if camera[2] <= 1e-9:
        raise ValueError("PROJECTED_POINT_BEHIND_CAMERA")
    x, y = camera[0] / camera[2], camera[1] / camera[2]
    if calibration.get("imageTransform", "raw") == "raw":
        coefficients = [_number(value, "distortion") for value in calibration["distortion"]]
        if len(coefficients) != 5:
            raise ValueError("INVALID_DISTORTION")
        k1, k2, p1, p2, k3 = coefficients
        radius2 = x * x + y * y
        radial = 1.0 + k1 * radius2 + k2 * radius2**2 + k3 * radius2**3
        x, y = (x * radial + 2.0 * p1 * x * y + p2 * (radius2 + 2.0 * x * x),
                y * radial + p1 * (radius2 + 2.0 * y * y) + 2.0 * p2 * x * y)
    fx, fy = _number(calibration["fx"], "fx"), _number(calibration["fy"], "fy")
    cx, cy = _number(calibration["cx"], "cx"), _number(calibration["cy"], "cy")
    pixel = (fx * x + cx, fy * y + cy)
    if not all(math.isfinite(value) for value in pixel):
        raise ValueError("PROJECTED_PIXEL_INVALID")
    return pixel


def _roi_contains(point: Sequence[float], ground: Mapping[str, Any]) -> bool:
    vertices = ground["roiVertices"]
    normal = tuple(_number(value, "ground normal") for value in ground["normal"])
    origin = tuple(_number(value, "ROI vertex") for value in vertices[0])
    edge = tuple(_number(vertices[1][i], "ROI vertex") - origin[i] for i in range(3))
    edge_norm = math.sqrt(sum(value * value for value in edge))
    if edge_norm <= 1e-12:
        raise ValueError("ROI_DEGENERATE")
    u = tuple(value / edge_norm for value in edge)
    v_raw = _cross(normal, u)
    v_norm = math.sqrt(sum(value * value for value in v_raw))
    if v_norm <= 1e-12:
        raise ValueError("ROI_DEGENERATE")
    v = tuple(value / v_norm for value in v_raw)

    def project(value: Sequence[float]) -> tuple[float, float]:
        delta = tuple(_number(value[i], "world point") - origin[i] for i in range(3))
        return sum(delta[i] * u[i] for i in range(3)), sum(delta[i] * v[i] for i in range(3))

    polygon = [project(vertex) for vertex in vertices]
    candidate = project(point)
    sign = 0
    for i, a in enumerate(polygon):
        b = polygon[(i + 1) % len(polygon)]
        cross = (b[0] - a[0]) * (candidate[1] - a[1]) - \
                (b[1] - a[1]) * (candidate[0] - a[0])
        if abs(cross) <= 1e-8:
            continue
        current = 1 if cross > 0 else -1
        if sign and current != sign:
            return False
        sign = current
    return True


def _project_raw(pixel: tuple[float, float], observation: Mapping[str, Any], *,
                 check_roi: bool = True) -> tuple[float, float, float]:
    calibration = observation["calibration"]
    ground = observation["ground"]
    position = tuple(_number(value, "positionW") for value in observation["positionW"])
    quaternion = tuple(_number(value, "qWC") for value in observation["qWC"])
    if len(position) != 3 or len(quaternion) != 4 or \
            not math.isclose(sum(value * value for value in quaternion), 1.0,
                             rel_tol=0.0, abs_tol=1e-6):
        raise ValueError("INVALID_QUATERNION_OR_POSITION")
    x, y = _undistort(pixel, calibration)
    ray_c = (x, y, 1.0)
    ray_norm = math.sqrt(sum(value * value for value in ray_c))
    ray_c = tuple(value / ray_norm for value in ray_c)
    ray_w = _rotate(quaternion, ray_c)
    normal = tuple(_number(value, "ground normal") for value in ground["normal"])
    if len(normal) != 3 or not math.isclose(
            sum(value * value for value in normal), 1.0, rel_tol=0.0, abs_tol=1e-6):
        raise ValueError("INVALID_GROUND_NORMAL")
    offset = _number(ground["offsetM"], "ground offset")
    denominator = sum(normal[i] * ray_w[i] for i in range(3))
    if abs(denominator) < 0.1:
        raise ValueError("RAY_NEAR_HORIZON")
    distance = -(sum(normal[i] * position[i] for i in range(3)) + offset) / denominator
    if not math.isfinite(distance) or distance <= 0 or distance > 500.0:
        raise ValueError("GROUND_INTERSECTION_BEHIND_CAMERA")
    point = tuple(position[i] + distance * ray_w[i] for i in range(3))
    if any(not math.isfinite(value) or abs(value) > 1e7 for value in point):
        raise ValueError("GROUND_INTERSECTION_INVALID")
    if check_roi and not _roi_contains(point, ground):
        raise ValueError("GROUND_INTERSECTION_OUTSIDE_ROI")
    return point


def _covariance_matrix(flat: Sequence[Any] | None, size: int) -> list[list[float]] | None:
    if flat is None:
        return None
    values = [_number(value, "covariance") for value in flat]
    if len(values) != size * size:
        raise ValueError("covariance dimension mismatch")
    matrix = [values[row * size:(row + 1) * size] for row in range(size)]
    scale = max(1.0, max(abs(value) for value in values))
    tolerance = 1e-10 * scale
    for row in range(size):
        for col in range(row):
            if abs(matrix[row][col] - matrix[col][row]) > tolerance:
                return None
            average = (matrix[row][col] + matrix[col][row]) / 2.0
            matrix[row][col] = average
            matrix[col][row] = average

    # LDL^T accepts positive-semidefinite matrices with zero pivots while
    # rejecting indefinite matrices and covariance values with negative
    # variance. Invalid uncertainty must degrade to unknown, never a clipped
    # or apparently qualified estimate.
    lower = [[0.0] * size for _ in range(size)]
    diagonal = [0.0] * size
    for col in range(size):
        pivot = matrix[col][col] - sum(
            lower[col][index] * lower[col][index] * diagonal[index]
            for index in range(col))
        if pivot < -tolerance:
            return None
        if pivot <= tolerance:
            diagonal[col] = 0.0
            for row in range(col + 1, size):
                residual = matrix[row][col] - sum(
                    lower[row][index] * lower[col][index] * diagonal[index]
                    for index in range(col))
                if abs(residual) > tolerance:
                    return None
                lower[row][col] = 0.0
            continue
        diagonal[col] = pivot
        for row in range(col + 1, size):
            residual = matrix[row][col] - sum(
                lower[row][index] * lower[col][index] * diagonal[index]
                for index in range(col))
            lower[row][col] = residual / pivot
    return matrix


def _covariance_from_jacobian(jacobian: list[list[float]],
                              covariance: list[list[float]]) -> tuple[float, ...]:
    size = len(covariance)
    result = [[0.0] * 3 for _ in range(3)]
    for row in range(3):
        for col in range(3):
            result[row][col] = sum(
                jacobian[row][i] * covariance[i][j] * jacobian[col][j]
                for i in range(size) for j in range(size))
    return tuple(result[row][col] for row in range(3) for col in range(3))


def _numeric_jacobian(function: Any, values: Sequence[float], steps: Sequence[float]) -> list[list[float]]:
    columns: list[tuple[float, float, float]] = []
    for index, step in enumerate(steps):
        before, after = list(values), list(values)
        before[index] -= step
        after[index] += step
        low, high = function(before), function(after)
        columns.append(tuple((high[axis] - low[axis]) / (2.0 * step) for axis in range(3)))
    return [[columns[column][row] for column in range(len(columns))] for row in range(3)]


def _add_covariance(a: tuple[float, ...] | None,
                    b: tuple[float, ...] | None) -> tuple[float, ...] | None:
    if a is None or b is None:
        return None
    return tuple(a[index] + b[index] for index in range(9))


def project_ground_point(pixel: tuple[float, float], observation: Mapping[str, Any]) -> GroundEstimate:
    """Undistort and intersect a box-bottom pixel ray with the observed plane.

    qWC rotates camera-frame rays into ENU world coordinates. The resulting
    point already includes camera translation; callers must not subtract ego
    displacement again when fitting vehicle ground motion.
    """
    if observation.get("poseStatus") != "aligned" or observation.get("positionW") is None or \
            observation.get("qWC") is None:
        raise ValueError(str(observation.get("reason") or "POSE_UNAVAILABLE"))
    if observation.get("calibration") is None or observation.get("ground") is None:
        raise ValueError("CALIBRATION_OR_GROUND_MISSING")
    if int(observation.get("timeUncertaintyUs", 0)) != 0 and \
            observation.get("poseCovariance") is None:
        raise ValueError("TIME_UNCERTAINTY_UNBOUNDED")
    point = _project_raw(pixel, observation)
    camera = tuple(_number(value, "positionW") for value in observation["positionW"])
    delta = tuple(point[i] - camera[i] for i in range(3))
    horizontal_range = math.hypot(delta[0], delta[1])
    slant_range = math.sqrt(sum(value * value for value in delta))

    raw = observation
    pixel_sigma = raw.get("anchorSigmaPixels")
    pixel_covariance = None
    system_covariances: list[tuple[float, ...]] = []
    complete = pixel_sigma is not None
    if pixel_sigma is not None:
        sigma = _number(pixel_sigma, "anchorSigmaPixels")
        pixel_values = (float(pixel[0]), float(pixel[1]))
        pixel_jacobian = _numeric_jacobian(
            lambda values: _project_raw((values[0], values[1]), raw, check_roi=False),
            pixel_values, (0.05, 0.05))
        pixel_covariance = _covariance_from_jacobian(
            pixel_jacobian, [[sigma * sigma, 0.0], [0.0, sigma * sigma]])
    else:
        complete = False

    pose_cov = _covariance_matrix(raw.get("poseCovariance"), 6)
    if pose_cov is not None:
        pose_values = [*_number_list(raw["positionW"], "positionW"), 0.0, 0.0, 0.0]

        def pose_projection(values: Sequence[float]) -> tuple[float, float, float]:
            edited = deepcopy(raw)
            edited["positionW"] = values[:3]
            delta = tuple(values[3 + axis] / 2.0 for axis in range(3))
            angle = math.sqrt(sum(value * value for value in delta))
            dq = ((math.cos(angle),) + tuple((value / angle) * math.sin(angle)
                                             for value in delta) if angle > 1e-15
                  else (1.0, 0.0, 0.0, 0.0))
            quaternion = tuple(_number(value, "qWC") for value in raw["qWC"])
            edited["qWC"] = _multiply_quaternion(quaternion, dq)
            return _project_raw(pixel, edited, check_roi=False)

        pose_steps = (1e-4, 1e-4, 1e-4, 1e-5, 1e-5, 1e-5)
        system_covariances.append(_covariance_from_jacobian(
            _numeric_jacobian(pose_projection, pose_values, pose_steps), pose_cov))
    else:
        complete = False

    cal_cov = _covariance_matrix(raw["calibration"].get("parameterCovariance"), 9)
    if cal_cov is not None:
        cal = raw["calibration"]
        values = [_number(cal[name], name) for name in ("fx", "fy", "cx", "cy")]
        values.extend(_number(item, "distortion") for item in cal["distortion"])

        def calibration_projection(parameters: Sequence[float]) -> tuple[float, float, float]:
            edited = deepcopy(raw)
            edited_cal = edited["calibration"]
            for name, value in zip(("fx", "fy", "cx", "cy"), parameters[:4]):
                edited_cal[name] = value
            edited_cal["distortion"] = list(parameters[4:])
            return _project_raw(pixel, edited, check_roi=False)

        cal_steps = tuple(max(abs(value) * 1e-6, 1e-8) for value in values)
        system_covariances.append(_covariance_from_jacobian(
            _numeric_jacobian(calibration_projection, values, cal_steps), cal_cov))
    else:
        complete = False

    ground_cov = _covariance_matrix(raw["ground"].get("parameterCovariance"), 4)
    if ground_cov is not None:
        ground = raw["ground"]
        values = [*_number_list(ground["normal"], "normal"), _number(ground["offsetM"], "offsetM")]

        def ground_projection(parameters: Sequence[float]) -> tuple[float, float, float]:
            edited = deepcopy(raw)
            # A plane is invariant to a common scale on its normal and offset.
            # Normalize both after perturbation so finite differences stay on
            # the unit-normal representation accepted by the observation API.
            norm = math.sqrt(sum(value * value for value in parameters[:3]))
            if norm <= 1e-12:
                raise ValueError("INVALID_GROUND_NORMAL")
            edited["ground"]["normal"] = [value / norm for value in parameters[:3]]
            edited["ground"]["offsetM"] = parameters[3] / norm
            return _project_raw(pixel, edited, check_roi=False)

        ground_steps = (1e-6, 1e-6, 1e-6, 1e-5)
        system_covariances.append(_covariance_from_jacobian(
            _numeric_jacobian(ground_projection, values, ground_steps), ground_cov))
    else:
        complete = False

    if complete:
        system_covariance = tuple(sum(matrix[i] for matrix in system_covariances)
                                  for i in range(9))
        uncertainty_kind = "linearized-assumed-independent"
    else:
        system_covariance = None
        pixel_covariance = None
        uncertainty_kind = "unknown"
    range_sigma = None
    if complete and pixel_covariance is not None and system_covariance is not None and slant_range > 1e-12:
        delta = tuple(point[index] - camera[index] for index in range(3))
        direction = tuple(value / slant_range for value in delta)
        covariance = tuple(pixel_covariance[index] + system_covariance[index] for index in range(9))
        variance = sum(direction[row] * covariance[row * 3 + col] * direction[col]
                       for row in range(3) for col in range(3))
        range_sigma = math.sqrt(max(0.0, variance))
    return GroundEstimate(point, camera, horizontal_range, slant_range,
                          pixel_covariance, system_covariance, uncertainty_kind, range_sigma)


def _number_list(values: Sequence[Any], label: str) -> list[float]:
    return [_number(value, label) for value in values]


@dataclass(frozen=True)
class MotionPolicy:
    minimum_points: int = 3
    maximum_points: int = 32
    maximum_gap_us: int = 1_000_000
    history_window_us: int = 1_000_000
    minimum_time_span_us: int = 400_000
    maximum_speed_mps: float = 60.0
    stationary_threshold_mps: float = 0.5
    maximum_bearing_sigma_degrees: float = 30.0


class MotionEstimator:
    """Finite per-track weighted least-squares estimator in ENU world meters."""

    def __init__(self, policy: MotionPolicy = MotionPolicy()) -> None:
        if policy.minimum_points < 3 or policy.maximum_points < policy.minimum_points or \
                policy.maximum_gap_us <= 0 or policy.history_window_us <= 0 or \
                policy.minimum_time_span_us <= 0 or \
                policy.minimum_time_span_us > policy.history_window_us or \
                policy.maximum_speed_mps <= 0 or \
                policy.stationary_threshold_mps <= 0:
            raise ValueError("motion policy bounds are invalid")
        self.policy = policy
        self._history: dict[tuple[Any, ...], list[tuple[int, GroundEstimate]]] = {}

    def reset(self, camera_id: str | None = None, source_epoch: int | None = None) -> None:
        if camera_id is None:
            self._history.clear()
            return
        self._history = {
            key: history for key, history in self._history.items()
            if not (len(key) >= 2 and key[1] == camera_id and
                    (source_epoch is None or (len(key) >= 3 and key[2] == source_epoch)))
        }

    def reset_track(self, track_key: tuple[Any, ...]) -> None:
        self._history.pop(tuple(track_key), None)

    def retain_tracks(self, active: set[tuple[Any, ...]]) -> None:
        """Drop histories for tracks not observed in this source-time window."""
        self._history = {key: samples for key, samples in self._history.items()
                         if key in active}

    def export_state(self) -> dict[str, Any]:
        return {
            "policy": {
                "minimumPoints": self.policy.minimum_points,
                "maximumPoints": self.policy.maximum_points,
                "maximumGapUs": self.policy.maximum_gap_us,
                "historyWindowUs": self.policy.history_window_us,
                "minimumTimeSpanUs": self.policy.minimum_time_span_us,
                "maximumSpeedMps": self.policy.maximum_speed_mps,
                "stationaryThresholdMps": self.policy.stationary_threshold_mps,
                "maximumBearingSigmaDegrees": self.policy.maximum_bearing_sigma_degrees,
            },
            "tracks": [
                {"key": list(key), "samples": [
                    {"timeUs": time_us, "positionW": list(ground.position_w),
                     "cameraPositionW": list(ground.camera_position_w),
                     "horizontalRangeM": ground.horizontal_range_m,
                     "slantRangeM": ground.slant_range_m,
                     "pixelCovariance": list(ground.pixel_covariance) if ground.pixel_covariance else None,
                     "systemCovariance": list(ground.system_covariance) if ground.system_covariance else None,
                     "uncertaintyKind": ground.uncertainty_kind,
                     "rangeSigmaM": ground.range_sigma_m}
                    for time_us, ground in history]}
                for key, history in sorted(self._history.items(), key=lambda item: repr(item[0]))
            ],
        }

    def restore_state(self, state: Mapping[str, Any]) -> None:
        expected_policy = {
            "minimumPoints": self.policy.minimum_points,
            "maximumPoints": self.policy.maximum_points,
            "maximumGapUs": self.policy.maximum_gap_us,
            "historyWindowUs": self.policy.history_window_us,
            "minimumTimeSpanUs": self.policy.minimum_time_span_us,
            "maximumSpeedMps": self.policy.maximum_speed_mps,
            "stationaryThresholdMps": self.policy.stationary_threshold_mps,
            "maximumBearingSigmaDegrees": self.policy.maximum_bearing_sigma_degrees,
        }
        if state.get("policy") != expected_policy:
            raise ValueError("motion state policy does not match this worker")
        tracks = state.get("tracks", [])
        if not isinstance(tracks, list) or len(tracks) > 5000:
            raise ValueError("motion state track count is invalid")
        history: dict[tuple[Any, ...], list[tuple[int, GroundEstimate]]] = {}
        for track in tracks:
            key = tuple(track["key"])
            samples = track["samples"]
            if len(key) != 5 or not isinstance(samples, list) or len(samples) > self.policy.maximum_points:
                raise ValueError("motion track state is outside its bounds")
            if (not isinstance(key[0], str) or not key[0] or key[1] not in {"UAV1", "UAV2", "UAV3"}
                    or any(isinstance(value, bool) or not isinstance(value, int) or value < 1
                           for value in key[2:])):
                raise ValueError("motion track state identity is invalid")
            if key in history:
                raise ValueError("motion state contains a duplicate track key")
            restored: list[tuple[int, GroundEstimate]] = []
            previous_time = -1
            for item in samples:
                time_us = int(item["timeUs"])
                if time_us <= previous_time:
                    raise ValueError("motion state timestamps must increase")
                previous_time = time_us
                position = tuple(float(value) for value in item["positionW"])
                camera_position = tuple(float(value) for value in item["cameraPositionW"])
                ranges = (float(item["horizontalRangeM"]), float(item["slantRangeM"]))
                if (len(position) != 3 or len(camera_position) != 3 or
                        any(not math.isfinite(value) for value in (*position, *camera_position, *ranges)) or
                        ranges[0] < 0 or ranges[1] < ranges[0]):
                    raise ValueError("motion state ground sample is invalid")
                pixel_covariance = (tuple(float(value) for value in item["pixelCovariance"])
                                    if item.get("pixelCovariance") is not None else None)
                system_covariance = (tuple(float(value) for value in item["systemCovariance"])
                                     if item.get("systemCovariance") is not None else None)
                if any(covariance is not None and
                       (len(covariance) != 9 or any(not math.isfinite(value) for value in covariance))
                       for covariance in (pixel_covariance, system_covariance)):
                    raise ValueError("motion state covariance is invalid")
                range_sigma = (float(item["rangeSigmaM"])
                               if item.get("rangeSigmaM") is not None else None)
                if range_sigma is not None and (not math.isfinite(range_sigma) or range_sigma < 0):
                    raise ValueError("motion state range uncertainty is invalid")
                restored.append((time_us, GroundEstimate(
                    position, camera_position, ranges[0], ranges[1],
                    pixel_covariance, system_covariance,
                    str(item["uncertaintyKind"]),
                    range_sigma)))
            if restored:
                history[key] = restored
        self._history = history

    def update(self, track_key: tuple[Any, ...], capture_time_us: int,
               ground: GroundEstimate) -> dict[str, Any]:
        if len(track_key) != 5 or capture_time_us < 0:
            raise ValueError("motion track identity or capture time is invalid")
        history = self._history.setdefault(tuple(track_key), [])
        if history and (capture_time_us <= history[-1][0] or
                        capture_time_us - history[-1][0] > self.policy.maximum_gap_us):
            history.clear()
        outlier = False
        if history:
            previous_time, previous_ground = history[-1]
            delta_seconds = (capture_time_us - previous_time) / 1_000_000.0
            displacement = math.dist(ground.position_w[:2], previous_ground.position_w[:2])
            error_margin = 3.0 * (self._position_sigma(previous_ground) +
                                  self._position_sigma(ground))
            if displacement > self.policy.maximum_speed_mps * delta_seconds + error_margin:
                history.clear()
                outlier = True
        history.append((capture_time_us, ground))
        cutoff_us = capture_time_us - self.policy.history_window_us
        while history and history[0][0] < cutoff_us:
            history.pop(0)
        del history[:-self.policy.maximum_points]
        span_us = history[-1][0] - history[0][0]
        estimate_time_us = ((history[0][0] + history[-1][0]) // 2
                            if len(history) >= self.policy.minimum_points and
                            span_us >= self.policy.minimum_time_span_us else None)
        base = {
            "trackKey": list(track_key),
            "captureTimeUs": capture_time_us,
            "estimateTimeUs": estimate_time_us,
            "positionW": list(ground.position_w),
            "distanceToCamera": {
                "horizontalM": ground.horizontal_range_m,
                "slantM": ground.slant_range_m,
                "sigmaRangeM": ground.range_sigma_m,
                "status": "valid",
            },
            "source": str(track_key[0]),
            "uncertaintyKind": ground.uncertainty_kind,
            "speedMps": None,
            "speedStatus": "unknown",
            "bearingDegreesTrue": None,
            "bearingStatus": "unknown",
            "quality": "warming-up",
            "reason": "INSUFFICIENT_TRACK_HISTORY",
            "sampleCount": len(history),
        }
        if outlier:
            base["quality"] = "unknown"
            base["reason"] = "OUTLIER"
            return base
        if len(history) < self.policy.minimum_points or \
                span_us < self.policy.minimum_time_span_us:
            return base

        times = [(time_us - history[0][0]) / 1_000_000.0 for time_us, _ in history]
        mean_time = sum(times) / len(times)
        denominator = sum((time_value - mean_time) ** 2 for time_value in times)
        if denominator <= 1e-12:
            base["quality"] = "unknown"
            base["reason"] = "INSUFFICIENT_TIME_SPAN"
            return base
        weights = [(time_value - mean_time) / denominator for time_value in times]
        velocity = tuple(sum(weights[i] * history[i][1].position_w[axis]
                             for i in range(len(history))) for axis in range(3))
        speed = math.hypot(velocity[0], velocity[1])
        pixel_covariances = [sample.pixel_covariance for _, sample in history]
        system_covariances = [sample.system_covariance for _, sample in history]
        sigma_speed: float | None = None
        sigma_bearing: float | None = None
        if all(covariance is not None for covariance in pixel_covariances + system_covariances):
            pixel_velocity_cov = [0.0] * 9
            for weight, covariance in zip(weights, pixel_covariances):
                assert covariance is not None
                for i in range(9):
                    pixel_velocity_cov[i] += weight * weight * covariance[i]
            system_sigma = sum(abs(weight) * math.sqrt(max(
                0.0, sum(covariance[i] for i in (0, 4, 8))))
                               for weight, covariance in zip(weights, system_covariances)
                               if covariance is not None)
            covariance = list(pixel_velocity_cov)
            for diagonal in (0, 4, 8):
                covariance[diagonal] += system_sigma * system_sigma
            if speed > 1e-9:
                sigma_speed = math.sqrt(max(0.0,
                    (velocity[0] ** 2 * covariance[0] +
                     velocity[1] ** 2 * covariance[4] +
                     2.0 * velocity[0] * velocity[1] * covariance[1]) / (speed * speed)))
                derivative_e = velocity[1] / (speed * speed)
                derivative_n = -velocity[0] / (speed * speed)
                sigma_bearing = math.degrees(math.sqrt(max(0.0,
                    derivative_e ** 2 * covariance[0] + derivative_n ** 2 * covariance[4] +
                    2.0 * derivative_e * derivative_n * covariance[1])))
            else:
                sigma_speed = math.sqrt(max(0.0, covariance[0] + covariance[4]))
                sigma_bearing = None
        base.update({
            "speedMps": speed,
            "speedStatus": "valid" if math.isfinite(speed) else "unknown",
            "velocityENU_mps": list(velocity),
            "sigmaSpeedMps": sigma_speed,
            "sigmaBearingDegrees": sigma_bearing,
            "uncertaintyKind": "linearized-assumed-independent"
                if sigma_speed is not None else "unknown",
            "sampleCount": len(history),
        })
        if sigma_speed is not None and speed + 2.0 * sigma_speed < self.policy.stationary_threshold_mps:
            base.update({"quality": "stationary", "reason": "STATIONARY",
                         "bearingDegreesTrue": None, "bearingStatus": "unknown"})
        elif speed < self.policy.stationary_threshold_mps:
            base.update({"quality": "degraded", "reason": "LOW_SPEED_DIRECTION_UNKNOWN",
                         "bearingDegreesTrue": None, "bearingStatus": "unknown"})
        else:
            base["quality"] = "estimated"
            base["reason"] = "OK" if sigma_speed is not None else "UNCERTAINTY_UNKNOWN"
            if sigma_speed is not None and speed > 2.0 * sigma_speed and \
                    sigma_bearing is not None and sigma_bearing <= self.policy.maximum_bearing_sigma_degrees:
                base["bearingDegreesTrue"] = math.degrees(math.atan2(velocity[0], velocity[1])) % 360.0
                base["bearingStatus"] = "valid"
            else:
                base["reason"] = "BEARING_UNCERTAINTY"
        return base

    @staticmethod
    def _position_sigma(ground: GroundEstimate) -> float:
        covariances = (ground.pixel_covariance, ground.system_covariance)
        if any(covariance is None for covariance in covariances):
            return 0.0
        variance = sum(covariance[index]
                       for covariance in covariances if covariance is not None
                       for index in (0, 4, 8))
        return math.sqrt(max(0.0, variance))
