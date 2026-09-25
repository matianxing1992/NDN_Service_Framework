#!/usr/bin/env python3
"""Bounded, source-time driven replay for the three UAV videos."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import cv2


@dataclass(frozen=True)
class ReplayFrame:
    camera_id: str
    sequence: int
    pts_us: int
    image_bgr: object


def iter_video(path: Path, camera_id: str, sample_fps: float = 2.0,
               source_seconds: float = 1.0) -> Iterator[ReplayFrame]:
    if sample_fps <= 0 or source_seconds <= 0:
        raise ValueError("sample_fps and source_seconds must be positive")
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise RuntimeError(f"cannot open video: {path}")
    source_fps = float(capture.get(cv2.CAP_PROP_FPS) or 0.0)
    if source_fps <= 0:
        capture.release()
        raise RuntimeError(f"video has no usable FPS: {path}")
    count = max(1, int(source_seconds * sample_fps))
    try:
        for sequence in range(count):
            target_pts = sequence / sample_fps
            capture.set(cv2.CAP_PROP_POS_MSEC, target_pts * 1000.0)
            ok, image = capture.read()
            if not ok:
                break
            yield ReplayFrame(camera_id, sequence, int(round(target_pts * 1_000_000)), image)
    finally:
        capture.release()


def synchronized(paths: dict[str, Path], sample_fps: float = 2.0,
                 source_seconds: float = 1.0) -> Iterator[dict[str, ReplayFrame]]:
    required = {"UAV1", "UAV2", "UAV3"}
    if set(paths) != required:
        raise ValueError(f"paths must contain exactly {sorted(required)}")
    streams = {camera: iter_video(path, camera, sample_fps, source_seconds)
               for camera, path in paths.items()}
    for _ in range(max(1, int(source_seconds * sample_fps))):
        frame_set: dict[str, ReplayFrame] = {}
        for camera in ("UAV1", "UAV2", "UAV3"):
            try:
                frame_set[camera] = next(streams[camera])
            except StopIteration:
                return
        yield frame_set
