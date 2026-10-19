"""Independent timing boundaries using existing verified source fixtures."""

import copy
from pathlib import Path
import sys

import cv2
import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-UAV-APP/tracking"))

import display_clock
from display_clock import ClockSample, VerifiedPlaybackClock, capture_age
from test_uav_metric_observation import _observation, _write_verified_observation_bundle


def _timing(*, measured: bool = False):
    raw = _observation("UAV1", 1, 0, 0.0, "sha256:" + "a" * 64,
                       acquisition_kind="measured" if measured else "simulated",
                       delivery_mode="live-push" if measured else "replay").raw
    clock = {"identity": copy.deepcopy(raw["identity"]),
             "clockBinding": copy.deepcopy(raw["clockBinding"]),
             "currentTimeUs": 0, "uncertaintyUs": 0,
             "sampledAtSteadyUs": 100, "rate": 1.0 if measured else 0.0,
             "basis": "source" if measured else "playback"}
    return raw, clock


def _age(raw, clock, now=100):
    return capture_age(raw["identity"], raw["clockBinding"], raw["captureTimeUs"],
                       raw["timeUncertaintyUs"], ClockSample.parse(clock), now)


def test_paused_playback_and_later_witness_age_the_cached_capture():
    raw, clock = _timing()
    assert _age(raw, clock, now=900_000_000).age_ms == 0
    assert _age(raw, clock, now=900_000_000).freshness == "fresh"
    clock["currentTimeUs"] = 1_000_000
    assert _age(raw, clock).freshness == "fresh"
    clock["currentTimeUs"] = 1_000_001
    assert _age(raw, clock).freshness == "stale"
    clock["uncertaintyUs"] = 2
    assert _age(raw, clock).freshness == "unknown"
    clock["currentTimeUs"] = 1_200_000
    assert _age(raw, clock).age_ms == 1200
    assert _age(raw, clock).basis == "playback"


def test_capture_and_clock_uncertainty_are_added_conservatively():
    raw, clock = _timing()
    raw["timeUncertaintyUs"] = 40_000
    clock.update(currentTimeUs=950_000, uncertaintyUs=20_000)
    age = _age(raw, clock)
    assert age.age_ms == 950
    assert age.uncertainty_ms == 60
    assert age.freshness == "unknown"


@pytest.mark.parametrize("field,value", [
    ("sourceId", "other"), ("droneId", "other"), ("cameraId", "UAV2"),
    ("epoch", 8), ("worldFrameId", "other"), ("acquisitionKind", "measured"),
    ("deliveryMode", "live-push"),
])
def test_every_source_identity_field_must_match(field, value):
    raw, clock = _timing()
    clock["identity"][field] = value
    if field == "deliveryMode":
        clock["basis"] = "simulation"
    assert _age(raw, clock).freshness == "unknown"


@pytest.mark.parametrize("field,value", [
    ("revision", 2), ("clockId", "other"), ("clockEpoch", 8),
    ("a", 2.0), ("bUs", 1.0), ("validFromUs", 1),
    ("validUntilUs", 8_000_000_000), ("uncertaintyUs", 1),
])
def test_every_clock_binding_field_must_match(field, value):
    raw, clock = _timing()
    clock["clockBinding"][field] = value
    assert _age(raw, clock).freshness == "unknown"


def test_source_clock_api_extrapolates_only_a_valid_synchronized_sample():
    raw, clock = _timing(measured=True)
    age = _age(raw, clock, now=1_200_100)
    assert (age.age_ms, age.freshness, age.basis) == (1200, "stale", "source")
    assert _age(raw, clock, now=99).freshness == "unknown"
    assert capture_age(raw["identity"], raw["clockBinding"], 0, 0, None, 100).freshness == "unknown"
    raw["captureTimeUs"] = 1
    assert _age(raw, clock).freshness == "unknown"
    raw["captureTimeUs"] = 0
    for binding in (raw["clockBinding"], clock["clockBinding"]):
        binding.update(a=2.0, bUs=100.0, validUntilUs=200)
    raw["captureTimeUs"] = 100
    clock["currentTimeUs"] = 500
    assert _age(raw, clock).freshness == "fresh"
    clock["currentTimeUs"] = 501
    assert _age(raw, clock).freshness == "unknown"


@pytest.mark.parametrize("field,value", [
    ("rate", -1), ("rate", float("nan")), ("rate", float("inf")), ("rate", 5),
    ("rate", True), ("currentTimeUs", -1), ("basis", "arrival"),
    ("uncertaintyUs", -1), ("sampledAtSteadyUs", True),
])
def test_invalid_local_clock_samples_reject(field, value):
    _, clock = _timing()
    clock[field] = value
    with pytest.raises(ValueError):
        ClockSample.parse(clock)


def test_verified_playback_provider_pauses_advances_and_rejects_regression(tmp_path):
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    image = bytes(encoded)
    _, manifest = _write_verified_observation_bundle(tmp_path, image)
    provider = VerifiedPlaybackClock(tmp_path, manifest, lambda: 100)
    first = provider()
    assert set(first) == {"UAV1", "UAV2", "UAV3"}
    assert first["UAV1"].rate == 0
    assert provider() == first
    _write_verified_observation_bundle(tmp_path, image, sequence=2, capture_time_us=1_200_000)
    later = provider()
    assert later["UAV1"].current_time_us == 1_200_000
    sample = first["UAV1"]
    raw, clock = _timing()
    # Both fixtures use the same verified source/clock identity.
    assert later["UAV1"].identity == sample.identity
    assert capture_age(raw["identity"], raw["clockBinding"], 0, 0,
                       later["UAV1"], 100).freshness == "stale"
    _write_verified_observation_bundle(tmp_path, image)
    assert provider() == {}
    _write_verified_observation_bundle(tmp_path, image, sequence=3, capture_time_us=1_400_000)
    assert provider()["UAV1"].current_time_us == 1_400_000


def test_verified_playback_provider_clears_samples_on_partial_or_racing_manifest(tmp_path, monkeypatch):
    ok, encoded = cv2.imencode(".jpg", np.zeros((100, 100, 3), dtype=np.uint8))
    assert ok
    _, manifest = _write_verified_observation_bundle(tmp_path, bytes(encoded))
    provider = VerifiedPlaybackClock(tmp_path, manifest, lambda: 100)
    assert provider()
    original = manifest.read_bytes()
    manifest.write_bytes(b"{")
    assert provider() == {}
    manifest.write_bytes(original)
    assert provider()
    parse = display_clock.load_verified_observation_bundle

    def racing_parse(*args):
        observations = parse(*args)
        manifest.write_bytes(manifest.read_bytes() + b" ")
        return observations

    manifest.write_bytes(original + b"\n")
    monkeypatch.setattr(display_clock, "load_verified_observation_bundle", racing_parse)
    assert provider() == {}
