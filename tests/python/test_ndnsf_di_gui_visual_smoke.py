#!/usr/bin/env python3
"""Optional visual smoke for the NDNSF-DI GUI.

This test is intentionally shallow. It verifies that a real window can be
shown and captured when PyAutoGUI is available; core behavior is covered by
the headless and Tk widget tests.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from ndnsf_distributed_inference.gui import DistributedInferenceGui, FakeRuntimeFactory


@unittest.skipUnless(os.environ.get("DISPLAY"), "visual GUI smoke requires DISPLAY/Xvfb")
class DistributedInferenceGuiVisualSmoke(unittest.TestCase):
    def test_window_can_be_shown_and_captured_when_pyautogui_is_available(self) -> None:
        try:
            import pyautogui  # type: ignore
        except Exception as exc:
            self.skipTest(f"PyAutoGUI unavailable: {exc}")

        app = DistributedInferenceGui(factory=FakeRuntimeFactory())
        try:
            app.update()
            app.lift()
            app.update()
            image = pyautogui.screenshot()
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "ndnsf-di-gui-smoke.png"
                image.save(path)
                self.assertGreater(path.stat().st_size, 0)
        finally:
            app.stop_all_roles()
            app.destroy()


if __name__ == "__main__":
    unittest.main()
