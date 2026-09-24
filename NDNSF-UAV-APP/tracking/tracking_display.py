#!/usr/bin/env python3
"""Compute-owned live renderer for Spec191.

The renderer consumes only annotated frames produced after inference. It never
opens a source video path. ``DisplayFrame`` is JSON-friendly so tests can use
the same contract without a GUI; the default runner uses OpenCV HighGUI.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import queue
import socket
import threading
import time
from typing import Any, Iterable


CAMERAS = ("UAV1", "UAV2", "UAV3")
MAX_FRAME_BYTES = 8 * 1024 * 1024
MAX_QUEUE_PER_CAMERA = 1
# Keep the three operator windows visible on a normal 1920x1080 desktop.  The
# source images are 3840x2160, so drawing directly at source resolution makes
# labels unreadable after HighGUI scales the image into its window.
DISPLAY_WINDOW_SIZE = (600, 400)
DISPLAY_WINDOW_POSITIONS = {
    "UAV1": (10, 60),
    "UAV2": (660, 60),
    "UAV3": (1310, 60),
}


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

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "DisplayFrame":
        image = base64.b64decode(str(value.get("imageJpeg", "")), validate=True)
        frame = cls(str(value["runId"]), str(value["sessionId"]), int(value["epoch"]),
                    int(value["window"]), str(value["cameraId"]), int(value["sequence"]),
                    int(value["ptsUs"]), image,
                    tuple(value.get("tracks", ())), str(value.get("resultState", "computed")))
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

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        return {"runId": self.run_id, "sessionId": self.session_id, "epoch": self.epoch,
                "window": self.window, "cameraId": self.camera_id, "sequence": self.sequence,
                "ptsUs": self.pts_us, "imageJpeg": base64.b64encode(self.image_jpeg).decode("ascii"),
                "tracks": list(self.tracks), "resultState": self.result_state}


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
    def __init__(self, mode: str = "windows", hold_seconds: float = 10.0) -> None:
        if mode not in {"windows", "headless"}:
            raise ValueError("mode must be windows or headless")
        self.mode = mode
        self.hold_seconds = max(0.0, min(60.0, hold_seconds))
        self.mailbox = DisplayMailbox()
        self.status: dict[str, Any] = {"mode": mode, "ready": False, "shownFrames": 0,
                                       "presentationDrops": self.mailbox.presentation_drops}
        self._last: dict[str, DisplayFrame] = {}

    @staticmethod
    def _emit_frame_shown(frame: DisplayFrame) -> None:
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
        }, sort_keys=True), flush=True)

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
        if self.mode == "headless":
            for camera in CAMERAS:
                frame = self.mailbox.get_latest(camera)
                if frame is not None:
                    self._last[camera] = frame
                    self.status["shownFrames"] += 1
                    self._emit_frame_shown(frame)
            return True
        import cv2
        np = __import__("numpy")
        for camera in CAMERAS:
            frame = self.mailbox.get_latest(camera)
            if frame is not None:
                self._last[camera] = frame
                self._emit_frame_shown(frame)
                image = cv2.imdecode(np.frombuffer(frame.image_jpeg, dtype="uint8"), cv2.IMREAD_COLOR)
                if image is None:
                    raise RuntimeError(f"invalid display JPEG for {camera}")
                # Render a readable operator view instead of relying on Qt to
                # shrink a 4K source image.  The worker's annotated JPEG is
                # still the evidence artifact; this canvas is display-only.
                window_width, window_height = DISPLAY_WINDOW_SIZE
                banner_height = 52
                available_height = window_height - banner_height
                source_height, source_width = image.shape[:2]
                scale = min(window_width / source_width, available_height / source_height)
                display_width = max(1, int(round(source_width * scale)))
                display_height = max(1, int(round(source_height * scale)))
                resized = cv2.resize(image, (display_width, display_height),
                                     interpolation=cv2.INTER_AREA)
                canvas = np.zeros((window_height, window_width, 3), dtype=image.dtype)
                offset_x = (window_width - display_width) // 2
                offset_y = banner_height + (available_height - display_height) // 2
                canvas[offset_y:offset_y + display_height,
                       offset_x:offset_x + display_width] = resized
                cv2.rectangle(canvas, (0, 0), (window_width - 1, banner_height - 1),
                              (24, 24, 24), cv2.FILLED)
                cv2.putText(canvas,
                            f"{camera}  PTS={frame.pts_us / 1e6:.3f}s  tracks={len(frame.tracks)}",
                            (8, 21), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 0), 1,
                            cv2.LINE_AA)
                summary = " ".join(
                    f"{track.get('class', 'object')} L{track.get('localId', '?')}"
                    f"/G{track.get('globalId', '?')}"
                    for track in frame.tracks[:3])
                if len(frame.tracks) > 3:
                    summary += " ..."
                cv2.putText(canvas, summary[:100], (8, 43), cv2.FONT_HERSHEY_SIMPLEX,
                            0.42, (230, 230, 230), 1, cv2.LINE_AA)
                for track in frame.tracks:
                    box = track.get("box")
                    if isinstance(box, (list, tuple)) and len(box) == 4:
                        x1, y1, x2, y2 = (int(v) for v in box)
                        x1 = int(round(x1 * scale)) + offset_x
                        y1 = int(round(y1 * scale)) + offset_y
                        x2 = int(round(x2 * scale)) + offset_x
                        y2 = int(round(y2 * scale)) + offset_y
                        cv2.rectangle(canvas, (x1, y1), (x2, y2), (0, 32, 255), 2)
                        label = f"{track.get('class', 'object')} {track.get('confidence', 0):.2f} " \
                                f"L{track.get('localId', '?')} G{track.get('globalId', '?')}"
                        cv2.putText(canvas, label, (x1, max(offset_y + 14, y1 - 4)),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.42, (0, 32, 255), 1,
                                    cv2.LINE_AA)
                cv2.imshow(f"{camera} — Tracking", canvas)
                self.status["shownFrames"] += 1
        key = cv2.waitKey(wait_ms) & 0xff
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
          fault: str = "none") -> int:
    if mode == "windows" and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise RuntimeError("DISPLAY_UNAVAILABLE")
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass
    renderer = ComputeRenderer(mode, hold_seconds)
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
        pending: list[DisplayFrame] = []
        for item in frames:
            if not isinstance(item, dict) or item.get("cameraId") not in CAMERAS:
                raise ValueError("tracking result contains an unknown display camera")
            image_path = Path(str(item.get("imagePath", "")))
            if not image_path.is_absolute() and replay_dir is not None:
                image_path = replay_dir / image_path
            if not image_path.is_file():
                return
            pending.append(DisplayFrame(
                run_id=str(value.get("runId", "")),
                session_id=str(value.get("missionId", "")),
                epoch=1,
                window=window,
                camera_id=str(item["cameraId"]),
                sequence=int(item.get("sequence", 1)),
                pts_us=int(item.get("ptsUs", 0)),
                image_jpeg=image_path.read_bytes(),
                tracks=tuple(item.get("tracks", ())),
                result_state="computed"))
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
                renderer.pump_once()
                check_window_close_fault()
                emit_eos_if_ready()
                continue
            with conn:
                buffer = b""
                while True:
                    chunk = conn.recv(1024 * 1024)
                    if not chunk:
                        break
                    buffer += chunk
                    while b"\n" in buffer:
                        raw, buffer = buffer.split(b"\n", 1)
                        if not raw:
                            continue
                        message = json.loads(raw.decode("utf-8"))
                        if message.get("event") == "STOP":
                            stop = True
                            break
                        renderer.submit(DisplayFrame.from_dict(message))
                        renderer.pump_once()
                        check_window_close_fault()
            renderer.pump_once()
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
    parser.add_argument("--fault", default="none")
    args = parser.parse_args(argv)
    if args.socket is None:
        parser.error("--socket is required")
    try:
        return serve(args.socket, args.mode, args.display_hold_seconds,
                     args.replay_json, args.replay_dir, args.fault)
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
