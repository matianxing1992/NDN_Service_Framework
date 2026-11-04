"""Shared stock ByteTrack state translation; no application identities or lifecycle."""

from __future__ import annotations

from collections import OrderedDict
import math
from types import SimpleNamespace
from typing import Any

import numpy as np

STOCK_REMOVED_BUFFER = 1000
MAX_STOCK_TRACK_ID = (1 << 24) - 1


class ByteTrackStateCodec:
    @staticmethod
    def _json_scalar(value: Any) -> Any:
        """Convert NumPy scalars to JSON values without accepting non-finite data."""
        if hasattr(value, "item"):
            value = value.item()
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("ByteTrack state contains a non-finite scalar")
        if isinstance(value, (str, int, float, bool)) or value is None:
            return value
        raise ValueError("ByteTrack state contains an unsupported scalar")

    @classmethod
    def _json_array(cls, value: Any, *, shape: tuple[int, ...] | None = None) -> list[Any] | None:
        if value is None:
            return None
        array = np.asarray(value)
        if shape is not None and tuple(array.shape) != shape:
            raise ValueError(f"ByteTrack state array shape {array.shape} != {shape}")
        if not np.isfinite(array).all():
            raise ValueError("ByteTrack state contains a non-finite array value")
        return array.astype(float).tolist()

    @classmethod
    def _serialize_byte_track(cls, track: Any, current_frame_id: int) -> dict[str, Any]:
        """Serialize the state consumed by Ultralytics BYTETracker.update()."""
        tlwh = cls._json_array(getattr(track, "_tlwh", None), shape=(4,))
        mean = cls._json_array(getattr(track, "mean", None), shape=(8,))
        covariance = cls._json_array(getattr(track, "covariance", None), shape=(8, 8))
        if tlwh is None or mean is None or covariance is None:
            raise ValueError("ByteTrack state is missing an initialized Kalman track")
        frame_id = int(track.frame_id)
        if current_frame_id < frame_id:
            raise ValueError("ByteTrack track frame is ahead of its tracker")
        time_since_update = getattr(track, "time_since_update", None)
        if time_since_update is None:
            # Releases without this cached field retain the same age information
            # in the frame IDs already present in the persisted state.
            time_since_update = current_frame_id - frame_id
        return {
            "trackId": int(track.track_id),
            "isActivated": bool(track.is_activated),
            "state": int(track.state),
            "tlwh": tlwh,
            "mean": mean,
            "covariance": covariance,
            "score": float(cls._json_scalar(track.score)),
            "class": cls._json_scalar(track.cls),
            "index": cls._json_scalar(track.idx),
            "angle": cls._json_scalar(track.angle),
            "startFrame": int(track.start_frame),
            "frameId": frame_id,
            "timeSinceUpdate": int(time_since_update),
            "trackletLength": int(track.tracklet_len),
        }

    @classmethod
    def _restore_byte_track(cls, tracker: Any, item: dict[str, Any]) -> Any:
        """Reconstruct one validated Ultralytics STrack for a new process."""
        from ultralytics.trackers.byte_tracker import STrack

        tlwh = np.asarray(item["tlwh"], dtype=np.float32)
        mean = np.asarray(item["mean"], dtype=np.float32)
        covariance = np.asarray(item["covariance"], dtype=np.float32)
        if tlwh.shape != (4,) or mean.shape != (8,) or covariance.shape != (8, 8):
            raise ValueError("invalid ByteTrack state array shape")
        if not np.isfinite(tlwh).all() or not np.isfinite(mean).all() or not np.isfinite(covariance).all():
            raise ValueError("invalid non-finite ByteTrack state")
        if any(int(item[key]) < 0 for key in ("trackId", "startFrame", "frameId",
                                               "timeSinceUpdate", "trackletLength")):
            raise ValueError("invalid negative ByteTrack state counter")
        if int(item["trackId"]) == 0:
            raise ValueError("ByteTrack track IDs must be positive")
        x, y, width, height = (float(value) for value in tlwh)
        track = STrack(np.asarray([x + width / 2.0, y + height / 2.0, width, height,
                                   float(item["index"])], dtype=np.float32),
                       float(item["score"]), item["class"])
        track._tlwh = tlwh
        track.kalman_filter = tracker.kalman_filter
        track.mean = mean
        track.covariance = covariance
        track.track_id = int(item["trackId"])
        track.is_activated = bool(item["isActivated"])
        track.state = int(item["state"])
        track.score = float(item["score"])
        track.cls = item["class"]
        track.idx = item["index"]
        track.angle = item["angle"]
        track.start_frame = int(item["startFrame"])
        track.frame_id = int(item["frameId"])
        # Do not inject a removed counter that stock updates will never maintain.
        if hasattr(track, "time_since_update"):
            track.time_since_update = int(item["timeSinceUpdate"])
        track.tracklet_len = int(item["trackletLength"])
        track.history = OrderedDict()
        track.features = []
        track.curr_feature = None
        return track

    @classmethod
    def _compact_high_watermark(cls, state: dict[str, Any]) -> int:
        """Validate compact history without changing a tracker or its allocator."""
        if set(state) != {"frameId", "tracked", "lost", "removedIds", "idHighWatermark"}:
            raise ValueError("invalid compact ByteTrack state fields")
        frame_id = state["frameId"]
        if type(frame_id) is not int or not 0 <= frame_id < 1 << 64:
            raise ValueError("invalid compact ByteTrack frame ID")
        high = state["idHighWatermark"]
        if type(high) is not int or not 0 <= high < MAX_STOCK_TRACK_ID:
            raise ValueError("invalid or exhausted ByteTrack ID high-watermark")
        removed = state["removedIds"]
        if not isinstance(removed, list) or len(removed) > STOCK_REMOVED_BUFFER:
            raise ValueError("invalid compact ByteTrack removal history")
        for track_id in removed:
            if type(track_id) is not int or not 0 < track_id <= high:
                raise ValueError("invalid compact ByteTrack removed ID")
        for group in ("tracked", "lost"):
            if not isinstance(state[group], list):
                raise ValueError("invalid compact ByteTrack track list")
            for item in state[group]:
                track_id = item.get("trackId") if isinstance(item, dict) else None
                if type(track_id) is not int or not 0 < track_id <= high:
                    raise ValueError("ByteTrack track exceeds its ID high-watermark")
        return high

    @classmethod
    def _serialize_byte_tracker(cls, tracker: Any, *, compact_removed: bool = False) -> dict[str, Any]:
        current_frame_id = int(tracker.frame_id)
        state = {
            "frameId": current_frame_id,
            "tracked": [cls._serialize_byte_track(item, current_frame_id)
                        for item in tracker.tracked_stracks],
            "lost": [cls._serialize_byte_track(item, current_frame_id)
                     for item in tracker.lost_stracks],
        }
        if compact_removed:
            from ultralytics.trackers.basetrack import BaseTrack

            # The sequential owner captures this immediately after its camera update.
            state["idHighWatermark"] = BaseTrack._count
            state["removedIds"] = [int(item.track_id) for item in tracker.removed_stracks]
            cls._compact_high_watermark(state)
        else:
            state["removed"] = [cls._serialize_byte_track(item, current_frame_id)
                                for item in tracker.removed_stracks]
        return state

    @classmethod
    def _restore_byte_tracker_state(cls, tracker: Any, state: dict[str, Any], *,
                                    compact_removed: bool = False) -> None:
        from ultralytics.trackers.basetrack import BaseTrack

        high = cls._compact_high_watermark(state) if compact_removed else None
        frame_id = int(state["frameId"])
        if frame_id < 0:
            raise ValueError("ByteTrack frame ID must be non-negative")
        restored = {
            key: [cls._restore_byte_track(tracker, item) for item in state.get(key, [])]
            for key in (("tracked", "lost") if compact_removed else ("tracked", "lost", "removed"))
        }
        if any(len(items) > 5000 for items in restored.values()):
            raise ValueError("ByteTrack state exceeds bounded track count")
        if compact_removed:
            # Stock only reads track_id from old removals before appending new ones.
            restored["removed"] = [SimpleNamespace(track_id=track_id)
                                   for track_id in state["removedIds"]]
        tracker.frame_id = frame_id
        tracker.tracked_stracks = restored["tracked"]
        tracker.lost_stracks = restored["lost"]
        tracker.removed_stracks = restored["removed"]
        if compact_removed:
            BaseTrack._count = high
        else:
            maximum_id = max((item.track_id for items in restored.values() for item in items), default=0)
            BaseTrack._count = max(int(BaseTrack._count), int(maximum_id))

