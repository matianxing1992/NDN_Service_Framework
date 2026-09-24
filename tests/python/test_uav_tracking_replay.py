from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
from replay_source import synchronized  # noqa: E402


class TrackingReplayTest(unittest.TestCase):
    def test_requires_three_named_sources(self) -> None:
        with self.assertRaises(ValueError):
            list(synchronized({"UAV1": Path("a")}, source_seconds=0.1))

    def test_replay_uses_source_pts_and_camera_order(self) -> None:
        video = ROOT / "NDNSF-UAV-APP/videos/drone.mp4"
        if not video.is_file():
            self.skipTest("checked-in smoke video is unavailable")
        frames = list(synchronized({"UAV1": video, "UAV2": video, "UAV3": video},
                                   sample_fps=2.0, source_seconds=1.0))
        self.assertTrue(frames)
        for index, frame_set in enumerate(frames):
            self.assertEqual(tuple(frame_set), ("UAV1", "UAV2", "UAV3"))
            self.assertEqual([frame_set[camera].pts_us for camera in frame_set],
                             [index * 500_000] * 3)


if __name__ == "__main__":
    unittest.main()
