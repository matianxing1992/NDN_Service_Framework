#!/usr/bin/env python3
"""Compute-owned live renderer for Spec191.

The renderer consumes only annotated frames produced after inference. It never
opens a source video path. ``DisplayFrame`` is JSON-friendly so tests can use
the same contract without a GUI; the default runner uses OpenCV HighGUI.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import asdict, dataclass, field
import json
import math
import os
from pathlib import Path
import queue
import socket
import threading
import time
from typing import Any, Callable, Iterable

from display_clock import (AgeEstimate, ClockSample, VerifiedPlaybackClock,
                           capture_age, source_identity)
from observation_source import (_identity as profile_source_identity,
                                load_observation_profile, load_source_generation_manifest,
                                source_generation_for_window)
from observation_schema import _integer


CAMERAS = ("UAV1", "UAV2", "UAV3")
MAX_FRAME_BYTES = 8 * 1024 * 1024
MAX_MESSAGE_BYTES = 16 * 1024 * 1024
MAX_QUEUE_PER_CAMERA = 1
# Keep the three operator windows visible on a normal 1920x1080 desktop. The
# lower panel gives each track a stable row instead of drawing long metric
# strings over neighboring boxes in the 4K source image.
DISPLAY_WINDOW_SIZE = (600, 700)
DISPLAY_WINDOW_POSITIONS = {
    "UAV1": (10, 60),
    "UAV2": (660, 60),
    "UAV3": (1310, 60),
}
METRICS_PANEL_HEIGHT = 305
METRICS_ROW_HEIGHT = 34
METRICS_PAGE_SECONDS = 2.0
TRACK_COLORS = (
    (60, 220, 255), (100, 255, 100), (255, 170, 70), (255, 100, 220),
    (80, 230, 210), (220, 190, 80), (180, 130, 255), (120, 210, 255),
    (255, 130, 120), (170, 255, 160), (255, 210, 120), (200, 160, 220),
)


def _display_value(value: Any, precision: int) -> str:
    if not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(float(value)):
        return f"{float(value):.{precision}f}"
    return "?"


def _track_metric_label(track: dict[str, Any]) -> str:
    """Return compact per-track values for the dedicated display table."""
    local_id = track.get("localId", "?")
    global_id = track.get("globalId", "?")
    measurement = track.get("measurement")
    if not isinstance(measurement, dict) or measurement.get("schema") != "uav-motion-result-v2":
        legacy_speed = track.get("legacySpeedMmps")
        speed = _display_value(legacy_speed * 0.0036 if not isinstance(legacy_speed, bool) and
                               isinstance(legacy_speed, (int, float)) and legacy_speed >= 0 else None, 2)
        return f"L{local_id}/G{global_id}  {speed}km/h"
    speed_mps = measurement.get("speedMps")
    speed = _display_value(speed_mps * 3.6 if not isinstance(speed_mps, bool) and
                           isinstance(speed_mps, (int, float)) else None, 2)
    course = _display_value(measurement.get("courseDeg"), 1)
    horizontal = _display_value(measurement.get("horizontalRangeM"), 1)
    slant = _display_value(measurement.get("slantRangeM"), 1)
    return f"L{local_id}/G{global_id}  {speed}km/h  {course}degT  H{horizontal}/S{slant}m"


def _track_status_label(track: dict[str, Any]) -> str:
    """Keep quality and the primary cause ahead of the numeric metric line."""
    measurement = track.get("measurement")
    if not isinstance(measurement, dict) or measurement.get("schema") != "uav-motion-result-v2":
        return "legacy estimate / unqualified"
    quality = str(measurement.get("quality") or "unknown")
    reasons = measurement.get("reasonCodes", ())
    reasons = [reason for reason in reasons if isinstance(reason, str) and reason] \
        if isinstance(reasons, (list, tuple)) else []
    if not reasons:
        return quality
    more = f" (+{len(reasons) - 1})" if len(reasons) > 1 else ""
    return f"{quality} {reasons[0]}{more}"


def _legacy_camera_speeds(estimates: Iterable[Any]) -> dict[tuple[str, int], int]:
    """Never infer a camera or overwrite an ambiguous legacy track estimate."""
    speeds: dict[tuple[str, int], int] = {}
    seen: set[tuple[str, int]] = set()
    for item in estimates:
        if not isinstance(item, dict) or item.get("cameraId") not in CAMERAS:
            continue
        try:
            key = (item["cameraId"], _integer(item.get("globalId"), "legacy globalId", positive=True))
            if key in seen:
                speeds.pop(key, None)
                continue
            seen.add(key)
            if item.get("status") == "ok" and item.get("vehicleSpeedMmps") is not None:
                speeds[key] = _integer(item["vehicleSpeedMmps"], "legacy speed")
        except ValueError:
            continue
    return speeds


def _fit_text(label: str, max_width: int, measure_width: Callable[[str], int]) -> str:
    """Trim a metric row monotonically until it fits, preserving an ellipsis."""
    if measure_width(label) <= max_width:
        return label
    suffix = "..."
    if measure_width(suffix) > max_width:
        return ""
    prefix = label.rstrip()
    while prefix and measure_width(prefix + suffix) > max_width:
        prefix = prefix[:-1].rstrip()
    return prefix + suffix


def _track_color(track: dict[str, Any]) -> tuple[int, int, int]:
    try:
        local_id = int(track.get("localId", 1))
    except (TypeError, ValueError):
        local_id = 1
    return TRACK_COLORS[(max(1, local_id) - 1) % len(TRACK_COLORS)]


def _matches_observation_scope(camera: str, window: int, sequence: int, identity: Any,
                               profile: dict[str, Any] | None,
                               generations: dict[str, Any] | None) -> bool:
    """Match this invocation, including scheduled source replacement, before display."""
    if profile is None:
        return True
    try:
        if sequence != window + 1:
            return False
        generation = source_generation_for_window(generations, window)
        expected = profile_source_identity(profile, camera, generation["sourceIds"][camera],
                                           generation["sourceEpoch"])
        return source_identity(identity) == source_identity(expected)
    except (ValueError, TypeError, KeyError):
        return False


@dataclass(frozen=True)
class DisplayFrame:
    run_id: str
    session_id: str
    epoch: int
    window: int
    camera_id: str
    sequence: int
    pts_us: int
    image_jpeg: bytes
    tracks: tuple[dict[str, Any], ...] = ()
    result_state: str = "computed"
    motion_status: str = "unknown"
    speed_mmps: tuple[int, ...] = ()
    measurements: tuple[dict[str, Any], ...] = ()
    provenance: str = "legacy"
    accuracy_qualification: str = "unvalidated"
    source_identity: dict[str, Any] | None = None
    observation_ref: dict[str, Any] | None = None
    geometry_validity: str = "unknown"
    clock_binding: dict[str, Any] | None = None
    time_uncertainty_us: int | None = None
    anchor_approximation: bool | None = None
    age_clock: dict[str, Any] | None = None

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "DisplayFrame":
        image = base64.b64decode(str(value.get("imageJpeg", "")), validate=True)
        frame = cls(str(value["runId"]), str(value["sessionId"]), int(value["epoch"]),
                    int(value["window"]), str(value["cameraId"]), int(value["sequence"]),
                    int(value["ptsUs"]), image,
                    tuple(value.get("tracks", ())), str(value.get("resultState", "computed")),
                    str(value.get("motionStatus", "unknown")),
                    tuple(int(item) for item in value.get("speedMmps", ())),
                    tuple(value.get("measurements", ())),
                    str(value.get("provenance", "legacy")),
                    str(value.get("accuracyQualification", "unvalidated")),
                    source_identity=value.get("sourceIdentity"),
                    observation_ref=value.get("observationRef"),
                    geometry_validity=str(value.get("geometryValidity", "unknown")),
                    clock_binding=value.get("clockBinding"),
                    time_uncertainty_us=value.get("timeUncertaintyUs"),
                    anchor_approximation=value.get("anchorApproximation"),
                    age_clock=value.get("ageClock"))
        frame.validate()
        return frame

    def validate(self) -> None:
        if self.camera_id not in CAMERAS:
            raise ValueError(f"unknown camera: {self.camera_id}")
        if self.epoch < 1 or self.window < 0 or self.sequence < 0 or self.pts_us < 0:
            raise ValueError("negative display sequence/epoch/time")
        if not self.run_id or not self.session_id or not self.image_jpeg:
            raise ValueError("display identity and image are required")
        if len(self.image_jpeg) > MAX_FRAME_BYTES:
            raise ValueError("display image exceeds 8 MiB")
        if len(self.tracks) > 5000 or len(self.measurements) > 5000:
            raise ValueError("display track/measurement count exceeds its bound")

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        return {"runId": self.run_id, "sessionId": self.session_id, "epoch": self.epoch,
                "window": self.window, "cameraId": self.camera_id, "sequence": self.sequence,
                "ptsUs": self.pts_us, "imageJpeg": base64.b64encode(self.image_jpeg).decode("ascii"),
                "tracks": list(self.tracks), "resultState": self.result_state,
                "motionStatus": self.motion_status, "speedMmps": list(self.speed_mmps),
                "measurements": list(self.measurements), "provenance": self.provenance,
                "accuracyQualification": self.accuracy_qualification,
                "sourceIdentity": self.source_identity, "observationRef": self.observation_ref,
                "geometryValidity": self.geometry_validity, "clockBinding": self.clock_binding,
                "timeUncertaintyUs": self.time_uncertainty_us,
                "anchorApproximation": self.anchor_approximation, "ageClock": self.age_clock}


class DisplayMailbox:
    """One bounded latest-frame slot per camera; presentation drops are counted."""

    def __init__(self) -> None:
        self._queues = {camera: queue.Queue(maxsize=MAX_QUEUE_PER_CAMERA) for camera in CAMERAS}
        self.presentation_drops = {camera: 0 for camera in CAMERAS}
        self._last_pts = {camera: -1 for camera in CAMERAS}

    def put(self, frame: DisplayFrame) -> None:
        frame.validate()
        if frame.pts_us < self._last_pts[frame.camera_id]:
            raise ValueError("display PTS moved backwards")
        self._last_pts[frame.camera_id] = frame.pts_us
        slot = self._queues[frame.camera_id]
        if slot.full():
            slot.get_nowait()
            self.presentation_drops[frame.camera_id] += 1
        slot.put_nowait(frame)

    def get_latest(self, camera: str) -> DisplayFrame | None:
        latest = None
        slot = self._queues[camera]
        while True:
            try:
                latest = slot.get_nowait()
            except queue.Empty:
                return latest


class ComputeRenderer:
    def __init__(self, mode: str = "windows", hold_seconds: float = 10.0, *,
                 clock_provider: Callable[[], dict[str, ClockSample]] | None = None,
                 steady_now_us: Callable[[], int] | None = None) -> None:
        if mode not in {"windows", "headless"}:
            raise ValueError("mode must be windows or headless")
        self.mode = mode
        self.hold_seconds = max(0.0, min(60.0, hold_seconds))
        self.mailbox = DisplayMailbox()
        self.status: dict[str, Any] = {"mode": mode, "ready": False, "shownFrames": 0,
                                       "presentationDrops": self.mailbox.presentation_drops}
        self._last: dict[str, DisplayFrame] = {}
        self._clock_provider = clock_provider
        self._steady_now_us = steady_now_us or (lambda: time.monotonic_ns() // 1000)
        self._ages: dict[str, AgeEstimate] = {}
        self._bases: dict[str, tuple[Any, ...]] = {}
        self._view_stamps: dict[str, tuple[Any, ...]] = {}

    def _emit_frame_shown(self, frame: DisplayFrame) -> None:
        print(json.dumps({
            "event": "DISPLAY_FRAME_SHOWN",
            "cameraId": frame.camera_id,
            "runId": frame.run_id,
            "sessionId": frame.session_id,
            "epoch": frame.epoch,
            "window": frame.window,
            "sequence": frame.sequence,
            "ptsUs": frame.pts_us,
            "resultState": frame.result_state,
            "motionStatus": frame.motion_status,
            "speedMmps": list(frame.speed_mmps),
            "measurementCount": len(frame.measurements),
            "provenance": frame.provenance,
            "accuracyQualification": frame.accuracy_qualification,
            "geometryValidity": frame.geometry_validity,
            "sourceIdentity": frame.source_identity,
            "observationRef": frame.observation_ref,
            "anchorApproximation": frame.anchor_approximation,
            "speedDisplayUnit": "km/h",
            "captureAge": asdict(self._ages.get(frame.camera_id, AgeEstimate())),
        }, sort_keys=True), flush=True)

    def _update_age(self, frame: DisplayFrame, samples: dict[str, ClockSample], now: int) -> None:
        sample = samples.get(frame.camera_id)
        try:
            if self._clock_provider is None and frame.age_clock is not None:
                sample = ClockSample.parse(frame.age_clock)
            if not isinstance(frame.source_identity, dict) or \
                    frame.source_identity.get("cameraId") != frame.camera_id:
                sample = None
            age = capture_age(frame.source_identity, frame.clock_binding, frame.pts_us,
                              frame.time_uncertainty_us, sample, now)
        except (TypeError, ValueError, KeyError, OverflowError):
            age = AgeEstimate()
        previous = self._ages.get(frame.camera_id)
        self._ages[frame.camera_id] = age
        self.status.setdefault("captureAge", {})[frame.camera_id] = asdict(age)
        if previous is not None and previous.freshness != age.freshness:
            print(json.dumps({"event": "DISPLAY_AGE_CHANGED", "cameraId": frame.camera_id,
                              "sequence": frame.sequence, "captureAge": asdict(age)},
                             sort_keys=True), flush=True)

    def start(self) -> None:
        if self.mode == "windows":
            if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
                raise RuntimeError("DISPLAY_UNAVAILABLE")
            import cv2
            for camera in CAMERAS:
                title = f"{camera} — Tracking"
                cv2.namedWindow(title, cv2.WINDOW_NORMAL)
                cv2.resizeWindow(title, *DISPLAY_WINDOW_SIZE)
                cv2.moveWindow(title, *DISPLAY_WINDOW_POSITIONS[camera])
        self.status["ready"] = True

    def submit(self, frame: DisplayFrame) -> None:
        self.mailbox.put(frame)

    def pump_once(self, wait_ms: int = 1) -> bool:
        if not self.status["ready"]:
            raise RuntimeError("renderer is not ready")
        try:
            samples = self._clock_provider() if self._clock_provider is not None else {}
            if not isinstance(samples, dict):
                samples = {}
        except Exception:
            # Clock adapters are optional; a failed adapter invalidates age,
            # while inference-owned frames and the operator UI remain usable.
            samples = {}
        now = self._steady_now_us()
        if self.mode == "headless":
            for camera in CAMERAS:
                frame = self.mailbox.get_latest(camera)
                if frame is not None:
                    self._last[camera] = frame
                    self._update_age(frame, samples, now)
                    self.status["shownFrames"] += 1
                    self._emit_frame_shown(frame)
                elif camera in self._last:
                    self._update_age(self._last[camera], samples, now)
            return True
        import cv2
        np = __import__("numpy")
        presented = []
        for camera in CAMERAS:
            new_frame = self.mailbox.get_latest(camera)
            frame = new_frame or self._last.get(camera)
            if frame is not None:
                self._last[camera] = frame
                self._update_age(frame, samples, now)
                window_width, window_height = DISPLAY_WINDOW_SIZE
                banner_height = 94
                panel_height = min(METRICS_PANEL_HEIGHT, max(120, window_height // 2))
                panel_top = window_height - panel_height
                image_area_height = max(1, panel_top - banner_height - 8)
                rows_per_column = max(1, (panel_height - 50) // METRICS_ROW_HEIGHT)
                page_capacity = rows_per_column * 2
                page_count = max(1, (len(frame.tracks) + page_capacity - 1) // page_capacity)
                page_index = int(time.monotonic() / METRICS_PAGE_SECONDS) % page_count
                view_stamp = (self._ages[camera].label(), page_index)
                if new_frame is None and self._view_stamps.get(camera) == view_stamp:
                    continue
                if new_frame is not None or camera not in self._bases:
                    image = cv2.imdecode(np.frombuffer(frame.image_jpeg, dtype="uint8"), cv2.IMREAD_COLOR)
                    if image is None:
                        raise RuntimeError(f"invalid display JPEG for {camera}")
                    source_height, source_width = image.shape[:2]
                    scale = min(window_width / source_width, image_area_height / source_height)
                    display_width = max(1, int(round(source_width * scale)))
                    display_height = max(1, int(round(source_height * scale)))
                    resized = cv2.resize(image, (display_width, display_height),
                                         interpolation=cv2.INTER_AREA)
                    canvas = np.zeros((window_height, window_width, 3), dtype=image.dtype)
                    offset_x = (window_width - display_width) // 2
                    offset_y = banner_height + (image_area_height - display_height) // 2
                    canvas[offset_y:offset_y + display_height,
                           offset_x:offset_x + display_width] = resized
                    self._bases[camera] = (canvas, scale, offset_x, offset_y, display_width, display_height)
                base, scale, offset_x, offset_y, display_width, display_height = self._bases[camera]
                canvas = base.copy()
                cv2.rectangle(canvas, (0, 0), (window_width - 1, banner_height - 1),
                              (24, 24, 24), cv2.FILLED)
                cv2.putText(canvas,
                            f"{camera}  capture={frame.pts_us / 1e6:.3f}s  tracks={len(frame.tracks)}",
                            (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (255, 255, 0), 1,
                            cv2.LINE_AA)
                qualification = (f"{frame.provenance} / {frame.accuracy_qualification}"
                                 if frame.source_identity is not None else "legacy estimate / unqualified")
                cv2.putText(canvas, f"{qualification} | {frame.geometry_validity}"[:100],
                            (8, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.34,
                            (0, 190, 255), 1, cv2.LINE_AA)
                age = self._ages[camera]
                cv2.putText(canvas, age.label(), (8, 59), cv2.FONT_HERSHEY_SIMPLEX,
                            0.42, (60, 80, 255) if age.freshness == "stale" else (200, 210, 210),
                            1, cv2.LINE_AA)
                anchor_label = ("bbox bottom-center: approximate ground contact"
                                if frame.anchor_approximation is True else "ground anchor: unqualified")
                cv2.putText(canvas, anchor_label, (8, 79), cv2.FONT_HERSHEY_SIMPLEX,
                            0.36, (180, 190, 190), 1, cv2.LINE_AA)
                for track in frame.tracks:
                    box = track.get("box")
                    if isinstance(box, (list, tuple)) and len(box) == 4:
                        x1, y1, x2, y2 = (int(v) for v in box)
                        x1 = int(round(x1 * scale)) + offset_x
                        y1 = int(round(y1 * scale)) + offset_y
                        x2 = int(round(x2 * scale)) + offset_x
                        y2 = int(round(y2 * scale)) + offset_y
                        color = _track_color(track)
                        cv2.rectangle(canvas, (x1, y1), (x2, y2), color, 2)
                        badge = f"L{track.get('localId', '?')}"
                        badge_width, badge_height = cv2.getTextSize(
                            badge, cv2.FONT_HERSHEY_SIMPLEX, 0.32, 1)[0]
                        badge_x = min(max(0, x1), window_width - badge_width - 6)
                        badge_y = max(offset_y + badge_height + 4, min(y1, offset_y + display_height - 2))
                        cv2.rectangle(canvas, (badge_x, badge_y - badge_height - 4),
                                      (badge_x + badge_width + 6, badge_y + 2), color, cv2.FILLED)
                        cv2.putText(canvas, badge, (badge_x + 3, badge_y - 1),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.32, (10, 10, 10), 1, cv2.LINE_AA)
                        measurement = track.get("measurement")
                        if isinstance(measurement, dict) and measurement.get("schema") == "uav-motion-result-v2":
                            arrow = measurement.get("imageArrow")
                            if measurement.get("bearingStatus") == "valid" and isinstance(arrow, dict):
                                origin_px = arrow.get("originPx")
                                direction_px = arrow.get("directionPx")
                                if (isinstance(origin_px, (list, tuple)) and len(origin_px) == 2 and
                                        isinstance(direction_px, (list, tuple)) and len(direction_px) == 2):
                                    ox, oy = float(origin_px[0]), float(origin_px[1])
                                    dx, dy = float(direction_px[0]), float(direction_px[1])
                                    length = math.hypot(dx, dy)
                                    if all(math.isfinite(value) for value in (ox, oy, dx, dy, length)) and length > 1e-6:
                                        tip_px = (ox + dx * 28.0 / length,
                                                  oy + dy * 28.0 / length)
                                        origin = (int(round(ox * scale)) + offset_x,
                                                  int(round(oy * scale)) + offset_y)
                                        tip = (int(round(tip_px[0] * scale)) + offset_x,
                                               int(round(tip_px[1] * scale)) + offset_y)
                                        cv2.arrowedLine(canvas, origin, tip, (0, 255, 255), 2,
                                                        tipLength=0.35)
                ordered_tracks = sorted(
                    frame.tracks,
                    key=lambda item: (str(item.get("globalId", "")), str(item.get("localId", ""))))
                cv2.rectangle(canvas, (0, panel_top), (window_width - 1, window_height - 1),
                              (18, 18, 18), cv2.FILLED)
                page_tracks = ordered_tracks[page_index * page_capacity:(page_index + 1) * page_capacity]
                cv2.putText(canvas,
                            f"PER-TRACK METRICS  page {page_index + 1}/{page_count}  (auto every 2s)",
                            (8, panel_top + 17), cv2.FONT_HERSHEY_SIMPLEX, 0.36,
                            (255, 255, 255), 1, cv2.LINE_AA)
                cv2.putText(canvas, "Quality / reason; then ID, km/h, degT, camera-center range H/S m",
                            (8, panel_top + 34), cv2.FONT_HERSHEY_SIMPLEX, 0.30,
                            (170, 190, 190), 1, cv2.LINE_AA)
                column_width = window_width // 2
                for index, track in enumerate(page_tracks):
                    column = index // rows_per_column
                    row = index % rows_per_column
                    base_x = column * column_width + 8
                    baseline = panel_top + 49 + row * METRICS_ROW_HEIGHT
                    color = _track_color(track)
                    cv2.rectangle(canvas, (base_x, baseline - 8), (base_x + 8, baseline), color,
                                  cv2.FILLED)
                    max_text_width = column_width - 30
                    for line, label in enumerate((_track_status_label(track), _track_metric_label(track))):
                        label = _fit_text(
                            label, max_text_width,
                            lambda value: cv2.getTextSize(
                                value, cv2.FONT_HERSHEY_SIMPLEX, 0.33, 1)[0][0])
                        cv2.putText(canvas, label, (base_x + 13, baseline + line * 17),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.33, (235, 235, 235), 1, cv2.LINE_AA)
                cv2.imshow(f"{camera} — Tracking", canvas)
                self._view_stamps[camera] = view_stamp
                if new_frame is not None:
                    presented.append(frame)
        key = cv2.waitKey(wait_ms) & 0xff
        # GUI work is flushed by waitKey; observers may now capture the frame.
        for frame in presented:
            self.status["shownFrames"] += 1
            self._emit_frame_shown(frame)
        if key in (ord("q"), 27):
            self.status["stoppedByUser"] = True
            return False
        return True

    def close(self) -> None:
        if self.mode == "windows":
            import cv2
            for camera in CAMERAS:
                cv2.destroyWindow(f"{camera} — Tracking")
        self.status["ready"] = False
        self.status["presentationDrops"] = dict(self.mailbox.presentation_drops)


def serve(socket_path: Path, mode: str, hold_seconds: float,
          replay_json: Path | None = None, replay_dir: Path | None = None,
          fault: str = "none", playback_clock_manifest: Path | None = None,
          playback_clock_input_dir: Path | None = None,
          observation_profile: Path | None = None, source_generations: Path | None = None) -> int:
    if (playback_clock_manifest is None) != (playback_clock_input_dir is None):
        raise ValueError("playback clock requires both manifest and input directory")
    if (observation_profile is None) != (source_generations is None):
        raise ValueError("display scope requires both profile and source generation manifest")
    profile = load_observation_profile(observation_profile) if observation_profile is not None else None
    generations = (load_source_generation_manifest(source_generations, int(profile["frameCount"]))
                   if profile is not None else None)
    if mode == "windows" and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("DISPLAY_UNAVAILABLE")
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass
    clock_provider = (VerifiedPlaybackClock(playback_clock_input_dir, playback_clock_manifest)
                      if playback_clock_manifest is not None else None)
    renderer = ComputeRenderer(mode, hold_seconds, clock_provider=clock_provider)
    renderer.start()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o600)
    server.listen(1)
    server.settimeout(0.2)
    renderer.status["socket"] = str(socket_path)
    print(json.dumps({"event": "DISPLAY_READY", **renderer.status}), flush=True)
    if fault == "display-failure":
        print(json.dumps({"event": "DISPLAY_FAILED", "error": "injected display failure"}),
             flush=True)
        server.close()
        socket_path.unlink(missing_ok=True)
        return 2
    if fault == "display-loss-after-ready":
        print(json.dumps({"event": "DISPLAY_LOST", "error": "injected display loss after ready"}),
             flush=True)
        server.close()
        socket_path.unlink(missing_ok=True)
        return 2
    stop = False
    replay_stamp: tuple[int, int] | None = None
    eos_emitted = False

    def check_window_close_fault() -> None:
        if fault == "window-close" and renderer.status["shownFrames"] > 0:
            print(json.dumps({"event": "DISPLAY_WINDOW_CLOSED",
                              "error": "injected window close"}), flush=True)
            raise RuntimeError("injected window close")

    def emit_eos_if_ready() -> None:
        nonlocal eos_emitted
        if fault == "eos" and not eos_emitted and renderer.status["shownFrames"] > 0:
            eos_emitted = True
            print(json.dumps({"event": "DISPLAY_EOS",
                              "shownFrames": renderer.status["shownFrames"]}), flush=True)

    def replay_result_if_ready() -> None:
        nonlocal replay_stamp
        if replay_json is None or not replay_json.is_file():
            return
        stat = replay_json.stat()
        stamp = (stat.st_mtime_ns, stat.st_size)
        if replay_stamp == stamp:
            return
        value = json.loads(replay_json.read_text(encoding="utf-8"))
        frames = value.get("frames")
        if not isinstance(frames, list) or len(frames) != len(CAMERAS):
            raise ValueError("tracking result does not contain three display frames")
        window_id = str(value.get("windowId", "window-0"))
        if not window_id.startswith("window-"):
            raise ValueError("tracking result has an invalid window id")
        try:
            window = int(window_id[len("window-"):])
        except ValueError as error:
            raise ValueError("tracking result has an invalid window id") from error
        if window < 0:
            raise ValueError("tracking result has a negative window id")
        for item in frames:
            observation = item.get("observation", {}) if isinstance(item, dict) else {}
            identity = observation.get("identity") if isinstance(observation, dict) else None
            if not isinstance(item, dict) or not _matches_observation_scope(
                    item.get("cameraId"), window, item.get("sequence"), identity, profile, generations):
                replay_stamp = stamp
                print(json.dumps({"event": "DISPLAY_RESULT_IGNORED", "reason": "source scope mismatch",
                                  "window": window}, sort_keys=True), flush=True)
                return
        motion = value.get("motion", {})
        if not isinstance(motion, dict):
            raise ValueError("tracking result motion metadata is not an object")
        estimates = motion.get("estimates", [])
        legacy_speeds = (_legacy_camera_speeds(estimates)
                         if motion.get("schema") == "spec191-motion-result-v1" else {})
        pending: list[DisplayFrame] = []
        for item in frames:
            if not isinstance(item, dict) or item.get("cameraId") not in CAMERAS:
                raise ValueError("tracking result contains an unknown display camera")
            image_path = Path(str(item.get("imagePath", "")))
            if not image_path.is_absolute() and replay_dir is not None:
                image_path = replay_dir / image_path
            if not image_path.is_file():
                return
            frame_speeds = tuple(
                legacy_speeds[item["cameraId"], int(track["globalId"])]
                for track in item.get("tracks", ())
                if isinstance(track, dict) and str(track.get("globalId", "")).isdigit()
                and (item["cameraId"], int(track["globalId"])) in legacy_speeds)
            frame_measurements = item.get("measurements", ())
            if not frame_measurements:
                frame_measurements = tuple(
                    track["measurement"] for track in item.get("tracks", ())
                    if isinstance(track, dict) and
                    isinstance(track.get("measurement"), dict))
            provenance = str(item.get("acquisitionKind", motion.get("inputProvenance", "legacy")))
            delivery = str(item.get("deliveryMode", ""))
            if delivery:
                provenance += "/" + delivery
            qualification = str(motion.get("accuracyQualification", "unvalidated"))
            qualities = [str(value.get("quality", "unknown"))
                         for value in frame_measurements if isinstance(value, dict)]
            motion_status = ("estimated" if any(value == "estimated" for value in qualities)
                             else "unknown" if not qualities else qualities[-1])
            display_tracks = tuple(dict(track) for track in item.get("tracks", ()))
            if motion.get("schema") == "spec191-motion-result-v1":
                for track in display_tracks:
                    global_id = str(track.get("globalId", ""))
                    if global_id.isdigit() and (item["cameraId"], int(global_id)) in legacy_speeds:
                        track["legacySpeedMmps"] = legacy_speeds[item["cameraId"], int(global_id)]
            observation = item.get("observation")
            if not isinstance(observation, dict):
                observation = {}
            pending.append(DisplayFrame(
                run_id=str(value.get("runId", "")),
                session_id=str(value.get("missionId", "")),
                epoch=1,
                window=window,
                camera_id=str(item["cameraId"]),
                sequence=int(item.get("sequence", 1)),
                pts_us=int(item.get("ptsUs", 0)),
                image_jpeg=image_path.read_bytes(),
                tracks=display_tracks,
                result_state="computed",
                motion_status=motion_status,
                speed_mmps=frame_speeds,
                measurements=tuple(frame_measurements),
                provenance=provenance,
                accuracy_qualification=qualification,
                source_identity=observation.get("identity"),
                observation_ref=observation.get("observationRef"),
                geometry_validity=str(observation.get("geometryValidity", "unknown")),
                clock_binding=observation.get("clockBinding"),
                time_uncertainty_us=observation.get("timeUncertaintyUs"),
                anchor_approximation=observation.get("anchorApproximation")))
        for frame in pending:
            renderer.submit(frame)
        replay_stamp = stamp
        print(json.dumps({"event": "DISPLAY_FRAMES_READY", "frames": len(pending)},
                         sort_keys=True), flush=True)

    try:
        while not stop:
            replay_result_if_ready()
            try:
                conn, _ = server.accept()
            except socket.timeout:
                stop = not renderer.pump_once()
                check_window_close_fault()
                emit_eos_if_ready()
                continue
            with conn:
                conn.settimeout(0.2)
                buffer = b""
                while not stop:
                    try:
                        chunk = conn.recv(1024 * 1024)
                    except socket.timeout:
                        replay_result_if_ready()
                        stop = not renderer.pump_once()
                        check_window_close_fault()
                        emit_eos_if_ready()
                        continue
                    if not chunk:
                        break
                    buffer += chunk
                    while b"\n" in buffer:
                        raw, buffer = buffer.split(b"\n", 1)
                        if len(raw) > MAX_MESSAGE_BYTES:
                            raise ValueError("display message exceeds 16 MiB")
                        if not raw:
                            continue
                        message = json.loads(raw.decode("utf-8"))
                        if message.get("event") == "STOP":
                            stop = True
                            break
                        frame = DisplayFrame.from_dict(message)
                        if not _matches_observation_scope(frame.camera_id, frame.window, frame.sequence,
                                                          frame.source_identity, profile, generations):
                            print(json.dumps({"event": "DISPLAY_FRAME_IGNORED",
                                              "reason": "source scope mismatch"}, sort_keys=True), flush=True)
                            continue
                        renderer.submit(frame)
                        stop = not renderer.pump_once()
                        check_window_close_fault()
                        if stop:
                            break
                    if len(buffer) > MAX_MESSAGE_BYTES:
                        raise ValueError("display message exceeds 16 MiB")
            stop = not renderer.pump_once() or stop
            check_window_close_fault()
            emit_eos_if_ready()
    finally:
        renderer.close()
        server.close()
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass
        print(json.dumps({"event": "DISPLAY_EXIT", **renderer.status}), flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", type=Path)
    parser.add_argument("--mode", choices=("windows", "headless"), default="windows")
    parser.add_argument("--display-hold-seconds", type=float, default=10.0)
    parser.add_argument("--replay-json", type=Path)
    parser.add_argument("--replay-dir", type=Path)
    parser.add_argument("--playback-clock-manifest", type=Path)
    parser.add_argument("--playback-clock-input-dir", type=Path)
    parser.add_argument("--observation-profile", type=Path)
    parser.add_argument("--source-generations", type=Path)
    parser.add_argument("--fault", default="none")
    args = parser.parse_args(argv)
    if args.socket is None:
        parser.error("--socket is required")
    if (args.playback_clock_manifest is None) != (args.playback_clock_input_dir is None):
        parser.error("playback clock requires both --playback-clock-manifest and --playback-clock-input-dir")
    if (args.observation_profile is None) != (args.source_generations is None):
        parser.error("display scope requires both --observation-profile and --source-generations")
    try:
        return serve(args.socket, args.mode, args.display_hold_seconds,
                     args.replay_json, args.replay_dir, args.fault,
                     args.playback_clock_manifest, args.playback_clock_input_dir,
                     args.observation_profile, args.source_generations)
    except KeyboardInterrupt:
        # The launcher deliberately stops the compute-local renderer after a
        # terminal result.  Treat that bounded shutdown as a clean exit after
        # serve() has emitted its DISPLAY_EXIT evidence, rather than leaking a
        # traceback into an otherwise successful run.
        return 0
    except Exception as exc:
        print(json.dumps({"event": "DISPLAY_FAILED", "error": str(exc)}), flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
