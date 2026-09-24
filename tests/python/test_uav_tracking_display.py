from __future__ import annotations

import base64
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))
from tracking_display import DisplayFrame, DisplayMailbox  # noqa: E402


class TrackingDisplayTest(unittest.TestCase):
    def frame(self, camera: str, pts: int) -> DisplayFrame:
        return DisplayFrame("run", "session", 1, 0, camera, pts, pts,
                            b"jpeg", ({"box": [1, 2, 10, 11], "class": "car",
                                       "confidence": .9, "localId": 1, "globalId": 7},))

    def test_three_camera_slots_are_bounded_and_count_presentation_drops(self) -> None:
        mailbox = DisplayMailbox()
        for camera in ("UAV1", "UAV2", "UAV3"):
            mailbox.put(self.frame(camera, 1))
            mailbox.put(self.frame(camera, 2))
        self.assertEqual(mailbox.presentation_drops, {"UAV1": 1, "UAV2": 1, "UAV3": 1})
        self.assertEqual(mailbox.get_latest("UAV1").pts_us, 2)

    def test_rejects_unknown_camera_and_backwards_pts(self) -> None:
        mailbox = DisplayMailbox()
        with self.assertRaises(ValueError):
            mailbox.put(self.frame("other", 1))
        mailbox.put(self.frame("UAV1", 2))
        with self.assertRaises(ValueError):
            mailbox.put(self.frame("UAV1", 1))


if __name__ == "__main__":
    unittest.main()
