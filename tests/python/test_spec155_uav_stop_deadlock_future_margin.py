#!/usr/bin/env python3
"""Source-boundary regressions for Spec 155."""

from pathlib import Path
import importlib.util
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRONE = ROOT / "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp"
RUNNER = ROOT / "Experiments/run_spec155_uav_stop_deadlock_future_margin.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("spec155_runner", RUNNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec155SourceBoundaryTest(unittest.TestCase):
    def test_video_stop_joins_detection_loop_after_container_lock(self) -> None:
        source = DRONE.read_text(encoding="utf-8")
        stop = source.index('if (action == "stop")')
        end = source.index('return makeResponse(false, encodeFields({', stop)
        body = source[stop:end]

        lock = body.index("std::lock_guard<std::mutex> guard(m_containerMutex)")
        lock_end = body.index(
            "\n            }\n"
            "            // Joining while holding m_containerMutex",
            lock,
        )
        join = body.index("stopObjectDetectionLoop();")
        status = body.index('publishStatus(alreadyStopped ?')
        self.assertLess(lock_end, join)
        self.assertLess(join, status)

    def test_efficiency_gates_are_fixed_at_two_percent(self) -> None:
        runner = load_runner()
        accepted = runner.apply_efficiency_gates({
            "metrics": {
                "payloadInterests": 10_000,
                "retryAttempts": 200,
                "timeouts": 199,
            },
            "checks": {"baseline": True},
        })
        self.assertTrue(accepted["accepted"])
        rejected = runner.apply_efficiency_gates({
            "metrics": {
                "payloadInterests": 10_000,
                "retryAttempts": 201,
                "timeouts": 0,
            },
            "checks": {"baseline": True},
        })
        self.assertFalse(rejected["accepted"])


if __name__ == "__main__":
    unittest.main()
