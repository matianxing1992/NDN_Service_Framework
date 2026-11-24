import json
from pathlib import Path
import sys

import pytest


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "NDNSF-UAV-APP" / "tracking"))


def test_native_numerical_export_matches_independent_python_oracle():
    """Optional batch oracle consumes values produced by the actual native test executable."""
    import os
    from motion_estimator import MotionEstimator, project_ground_point

    path = os.environ.get("NDNSF_UAV_VEHICLE_ORACLE_OUTPUT")
    if not path:
        pytest.skip("native numerical export must be produced by the B1b C++ selector")
    cases = json.loads(Path(path).read_text(encoding="utf-8"))
    assert [case["case"] for case in cases] == [
        "moving", "moving-camera", "stationary", "missing-covariance",
        "nonzero-covariance", "distortion"]
    comparisons = 0
    for case in cases:
        estimator = MotionEstimator()
        assert len(case["samples"]) == 3
        for sample in case["samples"]:
            raw, native = sample["raw"], sample["native"]
            identity = raw["identity"]
            key = (identity["sourceId"], identity["cameraId"], identity["epoch"], 1, 1)
            ground = project_ground_point(tuple(sample["anchor"]), raw)
            expected = estimator.update(key, raw["captureTimeUs"], ground)
            assert native["speedStatus"] == expected["speedStatus"]
            assert native["bearingStatus"] == expected["bearingStatus"]
            assert native["quality"] == expected["quality"]
            assert native["reasonCodes"] == ([] if expected["reason"] == "OK"
                                              else [expected["reason"]])
            assert native["estimateTimeUs"] == expected["estimateTimeUs"]
            pairs = {
                "speedMps": expected["speedMps"],
                "courseDeg": expected["bearingDegreesTrue"],
                "horizontalRangeM": ground.horizontal_range_m,
                "slantRangeM": ground.slant_range_m,
                "sigmaRangeM": ground.range_sigma_m,
                "sigmaSpeedMps": expected.get("sigmaSpeedMps"),
                "sigmaCourseDeg": expected.get("sigmaBearingDegrees"),
                "velocityEastMps": expected.get("velocityENU_mps", [None, None])[0],
                "velocityNorthMps": expected.get("velocityENU_mps", [None, None])[1],
            }
            for field, value in pairs.items():
                if value is None:
                    assert native[field] is None, (case["case"], field)
                else:
                    assert native[field] == pytest.approx(value, rel=1e-8, abs=1e-9), (
                        case["case"], field)
                comparisons += 1
    assert comparisons == 162
