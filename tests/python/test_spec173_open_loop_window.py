from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]
APP_USER = REPO / "examples" / "App_User.cpp"


class Spec173OpenLoopWindowTests(unittest.TestCase):
    def test_admission_modes_share_absolute_generation_and_measurement_boundaries(self):
        source = APP_USER.read_text()
        self.assertIn("const bool generating = now < *stopSendingAt;", source)
        self.assertIn("state->measured = now >= *measurementStartAt;", source)
        self.assertNotIn("noAdmissionTargetRequests", source)
        self.assertNotIn("noAdmissionWarmupRequests", source)
        self.assertNotIn("noAdmissionMeasuredRequests", source)


if __name__ == "__main__":
    unittest.main()
