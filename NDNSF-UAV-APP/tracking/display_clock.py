"""Capture-age calculation in an explicitly bound source or playback clock.

Playback advances only on compute's verified input admission. Its rate is zero
between admissions: this age is not wall-clock processing latency. A measured
source may supply a synchronized ClockSample through the same renderer seam.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import time
from typing import Any, Callable

from observation_schema import (MAX_OBSERVATION_BYTES, _identity_string, _integer,
                                _number, _object, load_verified_observation_bundle)


def source_identity(value: Any) -> tuple[str, str, str, int, str, str, str]:
    """Normalize every identity field, never matching a clock by camera alone."""
    value = _object(value, "source identity", {
        "sourceId", "droneId", "cameraId", "epoch", "worldFrameId",
        "acquisitionKind", "deliveryMode"})
    if value["cameraId"] not in {"UAV1", "UAV2", "UAV3"} or \
            value["acquisitionKind"] not in {"simulated", "measured"} or \
            value["deliveryMode"] not in {"generated", "replay", "live-push"}:
        raise ValueError("invalid clock source identity")
    return (_identity_string(value["sourceId"], "sourceId"),
            _identity_string(value["droneId"], "droneId"), value["cameraId"],
            _integer(value["epoch"], "source epoch", positive=True),
            _identity_string(value["worldFrameId"], "worldFrameId"),
            value["acquisitionKind"], value["deliveryMode"])


@dataclass(frozen=True)
class ClockBinding:
    revision: int
    clock_id: str
    epoch: int
    scale: float
    offset_us: float
    valid_from_us: int
    valid_until_us: int
    uncertainty_us: int

    @classmethod
    def parse(cls, value: Any) -> "ClockBinding":
        value = _object(value, "clockBinding", {
            "revision", "clockId", "clockEpoch", "a", "bUs", "validFromUs",
            "validUntilUs", "uncertaintyUs"})
        binding = cls(_integer(value["revision"], "revision", positive=True),
                      _identity_string(value["clockId"], "clockId"),
                      _integer(value["clockEpoch"], "clockEpoch", positive=True),
                      _number(value["a"], "clock scale"), _number(value["bUs"], "clock offset"),
                      _integer(value["validFromUs"], "validFromUs"),
                      _integer(value["validUntilUs"], "validUntilUs"),
                      _integer(value["uncertaintyUs"], "uncertaintyUs"))
        if binding.scale <= 0 or binding.valid_from_us > binding.valid_until_us:
            raise ValueError("invalid clock mapping or validity interval")
        return binding

    def contains(self, mapped_time_us: float) -> bool:
        # Record validity is expressed in device time; capture/current time is
        # mapped into the run-relative domain by a*tDevice+b.
        device_time = (mapped_time_us - self.offset_us) / self.scale
        return self.valid_from_us <= device_time <= self.valid_until_us


@dataclass(frozen=True)
class ClockSample:
    identity: tuple[str, str, str, int, str, str, str]
    binding: ClockBinding
    current_time_us: float
    uncertainty_us: int
    sampled_at_steady_us: int
    rate: float
    basis: str

    @classmethod
    def parse(cls, value: Any) -> "ClockSample":
        value = _object(value, "age clock", {
            "identity", "clockBinding", "currentTimeUs", "uncertaintyUs",
            "sampledAtSteadyUs", "rate", "basis"})
        sample = cls(source_identity(value["identity"]), ClockBinding.parse(value["clockBinding"]),
                     _number(value["currentTimeUs"], "currentTimeUs"),
                     _integer(value["uncertaintyUs"], "current uncertainty"),
                     _integer(value["sampledAtSteadyUs"], "clock sample time"),
                     _number(value["rate"], "clock rate"), str(value["basis"]))
        if sample.current_time_us < 0 or not 0 <= sample.rate <= 4 or \
                sample.basis not in {"simulation", "playback", "source"}:
            raise ValueError("invalid current clock sample")
        if (sample.basis == "source" and sample.identity[5:] != ("measured", "live-push")) or \
                (sample.basis == "playback" and sample.identity[6] != "replay") or \
                (sample.basis == "simulation" and sample.identity[5] != "simulated"):
            raise ValueError("clock basis does not match source provenance")
        return sample


@dataclass(frozen=True)
class AgeEstimate:
    age_ms: float | None = None
    uncertainty_ms: float | None = None
    freshness: str = "unknown"
    basis: str = "unknown"

    def label(self) -> str:
        age = "?" if self.age_ms is None else f"{self.age_ms:.0f}"
        return f"{self.basis} age={age}ms {self.freshness}"


def capture_age(identity: Any, binding: Any, capture_time_us: int,
                uncertainty_us: Any, sample: ClockSample | None, steady_now_us: int) -> AgeEstimate:
    """Fail closed when current and capture clocks cannot be compared safely."""
    try:
        capture_identity = source_identity(identity)
        clock = ClockBinding.parse(binding)
        capture = _integer(capture_time_us, "capture time")
        uncertainty = _integer(uncertainty_us, "capture uncertainty")
        now = _integer(steady_now_us, "steady time")
        if not isinstance(sample, ClockSample) or sample.identity != capture_identity or sample.binding != clock or \
                now < sample.sampled_at_steady_us:
            return AgeEstimate()
        rate = _number(sample.rate, "clock rate")
        if not 0 <= rate <= 4 or sample.basis not in {"simulation", "playback", "source"} or \
                (sample.basis == "source" and capture_identity[5:] != ("measured", "live-push")) or \
                (sample.basis == "playback" and capture_identity[6] != "replay") or \
                (sample.basis == "simulation" and capture_identity[5] != "simulated"):
            return AgeEstimate()
        current = _number(sample.current_time_us, "current time") + \
            (now - _integer(sample.sampled_at_steady_us, "sample time")) * rate
        if not clock.contains(capture) or not clock.contains(current) or current < capture:
            return AgeEstimate()
        age = (current - capture) / 1000.0
        uncertainty_ms = (uncertainty + max(clock.uncertainty_us, _integer(
            sample.uncertainty_us, "current uncertainty"))) / 1000.0
        freshness = ("fresh" if age + uncertainty_ms <= 1000.0 else
                     "stale" if age - uncertainty_ms > 1000.0 else "unknown")
        return AgeEstimate(age, uncertainty_ms, freshness, sample.basis)
    except (TypeError, ValueError, KeyError, OverflowError):
        return AgeEstimate()


class VerifiedPlaybackClock:
    """Bounded current playback cursor from the existing verified-bundle parser."""

    def __init__(self, input_dir: Path, manifest_path: Path,
                 steady_now_us: Callable[[], int] | None = None) -> None:
        self.input_dir = input_dir
        self.manifest_path = manifest_path
        self.steady_now_us = steady_now_us or (lambda: time.monotonic_ns() // 1000)
        self._digest: bytes | None = None
        self._samples: dict[str, ClockSample] = {}
        self._witnesses: dict[str, tuple[Any, ...]] = {}

    def _manifest_digest(self) -> bytes:
        with self.manifest_path.open("rb") as stream:
            data = stream.read(MAX_OBSERVATION_BYTES + 1)
        if len(data) > MAX_OBSERVATION_BYTES:
            raise ValueError("playback manifest exceeds its bound")
        return hashlib.sha256(data).digest()

    def __call__(self) -> dict[str, ClockSample]:
        try:
            digest = self._manifest_digest()
            if digest != self._digest:
                observations = load_verified_observation_bundle(self.input_dir, self.manifest_path)
                if digest != self._manifest_digest():
                    raise ValueError("playback manifest changed during verification")
                now = self.steady_now_us()
                samples = {}
                witnesses = {}
                for camera, observation in observations.items():
                    identity = source_identity(observation.raw["identity"])
                    basis = "playback" if identity[6] == "replay" else "simulation"
                    # Measured live input needs its own synchronized source
                    # clock. A received capture cannot substitute for "now".
                    if identity[5] != "simulated" and identity[6] != "replay":
                        continue
                    binding = ClockBinding.parse(observation.raw["clockBinding"])
                    witness = (identity, binding, observation.sequence,
                               observation.capture_time_us, observation.sidecar_digest)
                    previous = self._witnesses.get(camera)
                    if previous is not None and previous[:2] == witness[:2]:
                        if witness[2] < previous[2] or witness[3] < previous[3] or \
                                (witness[2] == previous[2] and witness != previous):
                            raise ValueError("playback witness moved backwards or changed identity")
                    witnesses[camera] = witness
                    samples[camera] = ClockSample(
                        identity, binding, observation.capture_time_us,
                        max(binding.uncertainty_us, int(observation.raw["timeUncertaintyUs"])),
                        now, 0.0, basis)
                self._samples, self._digest = samples, digest
                self._witnesses = witnesses
            return dict(self._samples)
        except (OSError, ValueError, TypeError, KeyError, OverflowError):
            self._samples, self._digest = {}, None
            return {}
