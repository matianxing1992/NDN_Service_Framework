#!/usr/bin/env python3
"""Deterministic, application-neutral workloads and evidence primitives."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import random
from typing import Dict, Iterable, List, Mapping, Tuple


WORKLOAD_SEED = 12720260720


@dataclass(frozen=True)
class SampleClassProfile:
    class_id: str
    seed_source_items: int
    hard_max_source_items: int
    actual_extents: Tuple[int, ...]


@dataclass(frozen=True)
class WorkloadManifest:
    workload_id: str
    seed: int
    period_ms: int
    warmup_seconds: int
    measurement_seconds: int
    expected_measured_samples: int
    segment_payload_bytes: int
    fec_rule: str
    class_profiles: Tuple[SampleClassProfile, ...]

    def __post_init__(self) -> None:
        if not self.workload_id:
            raise ValueError("workload ID must be nonempty")
        if self.seed < 0:
            raise ValueError("seed must be nonnegative")
        if self.period_ms <= 0:
            raise ValueError("period must be positive")
        if self.warmup_seconds <= 0 or self.measurement_seconds <= 0:
            raise ValueError("warm-up and measurement windows must be positive")
        measurement_ms = self.measurement_seconds * 1000
        if measurement_ms % self.period_ms or \
                self.expected_measured_samples != measurement_ms // self.period_ms:
            raise ValueError("measured sample count must match period and window")
        if self.segment_payload_bytes <= 0:
            raise ValueError("segment payload size must be positive")
        if self.fec_rule not in {"none", "xor-one-repair"}:
            raise ValueError("unsupported FEC rule")
        if not self.class_profiles:
            raise ValueError("at least one class profile is required")
        class_ids = [profile.class_id for profile in self.class_profiles]
        if any(not class_id for class_id in class_ids) or \
                len(set(class_ids)) != len(class_ids):
            raise ValueError("class profiles require unique class IDs")
        for profile in self.class_profiles:
            if not 1 <= profile.seed_source_items <= \
                    profile.hard_max_source_items:
                raise ValueError("seed source items exceed signed class bounds")
            if not profile.actual_extents or any(
                    extent < 1 or extent > profile.hard_max_source_items
                    for extent in profile.actual_extents):
                raise ValueError("actual extent exceeds signed class bounds")

    def to_dict(self) -> Dict[str, object]:
        return {
            "schemaVersion": "spec127-workload-v1",
            "workloadId": self.workload_id,
            "seed": self.seed,
            "periodMs": self.period_ms,
            "warmupSeconds": self.warmup_seconds,
            "measurementSeconds": self.measurement_seconds,
            "expectedMeasuredSamples": self.expected_measured_samples,
            "segmentPayloadBytes": self.segment_payload_bytes,
            "fecRule": self.fec_rule,
            "classProfiles": [
                {
                    "classId": profile.class_id,
                    "seedSourceItems": profile.seed_source_items,
                    "hardMaxSourceItems": profile.hard_max_source_items,
                    "actualExtents": list(profile.actual_extents),
                }
                for profile in self.class_profiles
            ],
        }

    @classmethod
    def from_dict(cls, encoded: Mapping[str, object]) -> "WorkloadManifest":
        expected_keys = {
            "schemaVersion", "workloadId", "seed", "periodMs",
            "warmupSeconds", "measurementSeconds", "expectedMeasuredSamples",
            "segmentPayloadBytes", "fecRule", "classProfiles",
        }
        if set(encoded) != expected_keys:
            raise ValueError("manifest fields do not match schemaVersion contract")
        if encoded["schemaVersion"] != "spec127-workload-v1":
            raise ValueError("unsupported schemaVersion")
        raw_profiles = encoded["classProfiles"]
        if not isinstance(raw_profiles, list):
            raise ValueError("classProfiles must be a list")
        profiles = []
        profile_keys = {
            "classId", "seedSourceItems", "hardMaxSourceItems",
            "actualExtents",
        }
        for raw_profile in raw_profiles:
            if not isinstance(raw_profile, dict) or \
                    set(raw_profile) != profile_keys:
                raise ValueError("class profile fields do not match contract")
            raw_extents = raw_profile["actualExtents"]
            if not isinstance(raw_extents, list):
                raise ValueError("actualExtents must be a list")
            profiles.append(SampleClassProfile(
                class_id=raw_profile["classId"],
                seed_source_items=raw_profile["seedSourceItems"],
                hard_max_source_items=raw_profile["hardMaxSourceItems"],
                actual_extents=tuple(raw_extents),
            ))
        return cls(
            workload_id=encoded["workloadId"], seed=encoded["seed"],
            period_ms=encoded["periodMs"],
            warmup_seconds=encoded["warmupSeconds"],
            measurement_seconds=encoded["measurementSeconds"],
            expected_measured_samples=encoded["expectedMeasuredSamples"],
            segment_payload_bytes=encoded["segmentPayloadBytes"],
            fec_rule=encoded["fecRule"], class_profiles=tuple(profiles),
        )

    @property
    def digest(self) -> str:
        encoded = json.dumps(
            self.to_dict(),
            sort_keys=True, separators=(",", ":"),
        ).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()


@dataclass(frozen=True)
class OpaqueWorkloadSample:
    sample_id: int
    class_id: str
    actual_source_items: int
    phase: str


@dataclass(frozen=True)
class CompleteSampleReceipt:
    sample_id: int
    class_id: str
    source_digests: Tuple[str, ...]
    completed_timestamp_us: int
    recovered_items: int
    phase: str


@dataclass(frozen=True)
class TrafficUtilityReport:
    """Application-neutral Interest utility counters and derived ratios."""

    payload_interests: int
    necessary_source_repair_items: int
    mapping_interests: int
    mapping_data_responses: int
    mapping_new_data_responses: int
    mapping_bytes: int
    retry_attempts: int
    timeouts: int
    nacks: int
    provider_future_interests: int
    provider_future_hits: int
    initial_payload_interests: int = 0
    retry_payload_interests: int = 0
    initial_future_payload_interests: int = 0
    retry_future_payload_interests: int = 0
    retry_successes: int = 0
    retry_exhaustions: int = 0
    retry_suppressions: int = 0
    declared_recovery_capacity: int = 0
    recovery_attempts: int = 0
    recovered_sources: int = 0
    recovery_exhaustions: int = 0
    provider_initial_future_interests: int = 0
    provider_initial_future_hits: int = 0
    provider_retry_future_interests: int = 0
    provider_retry_future_hits: int = 0

    def __post_init__(self) -> None:
        for name, value in asdict(self).items():
            if value < 0:
                raise ValueError(f"{name} must be nonnegative")
        if self.mapping_new_data_responses > self.mapping_data_responses:
            raise ValueError(
                "new Mapping Data responses exceed Mapping Data responses")
        if self.provider_future_hits > self.provider_future_interests:
            raise ValueError("future hits exceed future interests")
        if (self.initial_payload_interests + self.retry_payload_interests and
                self.initial_payload_interests + self.retry_payload_interests !=
                self.payload_interests):
            raise ValueError("initial and retry Payload Interests do not sum to total")

    def to_dict(self) -> Dict[str, object]:
        result: Dict[str, object] = {
            "payloadInterests": self.payload_interests,
            "necessarySourceRepairItems": self.necessary_source_repair_items,
            "mappingInterests": self.mapping_interests,
            "mappingDataResponses": self.mapping_data_responses,
            "mappingNewDataResponses": self.mapping_new_data_responses,
            "mappingBytes": self.mapping_bytes,
            "retryAttempts": self.retry_attempts,
            "timeouts": self.timeouts,
            "nacks": self.nacks,
            "providerFutureInterests": self.provider_future_interests,
            "providerFutureHits": self.provider_future_hits,
            "initialPayloadInterests": self.initial_payload_interests,
            "retryPayloadInterests": self.retry_payload_interests,
            "initialFuturePayloadInterests": self.initial_future_payload_interests,
            "retryFuturePayloadInterests": self.retry_future_payload_interests,
            "retrySuccesses": self.retry_successes,
            "retryExhaustions": self.retry_exhaustions,
            "retrySuppressions": self.retry_suppressions,
            "declaredRecoveryCapacity": self.declared_recovery_capacity,
            "recoveryAttempts": self.recovery_attempts,
            "recoveredSources": self.recovered_sources,
            "recoveryExhaustions": self.recovery_exhaustions,
            "providerInitialFutureInterests": self.provider_initial_future_interests,
            "providerInitialFutureHits": self.provider_initial_future_hits,
            "providerRetryFutureInterests": self.provider_retry_future_interests,
            "providerRetryFutureHits": self.provider_retry_future_hits,
        }
        if self.necessary_source_repair_items:
            result["payloadInterestOverheadRatio"] = (
                self.payload_interests - self.necessary_source_repair_items
            ) / self.necessary_source_repair_items
        else:
            result["payloadInterestOverheadRatio"] = None
            result["payloadInterestOverheadRatioUnavailableReason"] = (
                "no-necessary-source-repair-items")
        if self.mapping_data_responses:
            result["mappingNewDataRatio"] = (
                self.mapping_new_data_responses / self.mapping_data_responses)
        else:
            result["mappingNewDataRatio"] = None
            result["mappingNewDataRatioUnavailableReason"] = (
                "no-mapping-data-responses")
        if self.provider_future_interests:
            result["providerFutureHitRatio"] = (
                self.provider_future_hits / self.provider_future_interests)
        else:
            result["providerFutureHitRatio"] = None
            result["providerFutureHitRatioUnavailableReason"] = (
                "no-provider-future-interests")
        return result


def build_semantic_prefix(provider_name: str, workload_id: str,
                          mapping_version: int,
                          campaign_label: str = "spec127") -> str:
    if not provider_name.startswith("/") or mapping_version <= 0:
        raise ValueError("invalid semantic prefix context")
    if workload_id not in {"periodic-sensor", "variable-multisegment"}:
        raise ValueError("unsupported workload semantic prefix")
    if campaign_label not in {"spec127", "spec128"}:
        raise ValueError("unsupported campaign label")
    return (f"{provider_name.rstrip('/')}/{campaign_label}/{workload_id}/"
            f"v={int(mapping_version)}")


def build_workload_manifest(workload_id: str) -> WorkloadManifest:
    common = dict(
        workload_id=workload_id, seed=WORKLOAD_SEED, period_ms=100,
        warmup_seconds=5, measurement_seconds=60,
        expected_measured_samples=600,
    )
    if workload_id == "periodic-sensor":
        return WorkloadManifest(
            **common, segment_payload_bytes=256, fec_rule="none",
            class_profiles=(SampleClassProfile("opaque-small", 1, 1, (1,)),),
        )
    if workload_id == "variable-multisegment":
        return WorkloadManifest(
            **common, segment_payload_bytes=4096, fec_rule="xor-one-repair",
            class_profiles=(
                SampleClassProfile("cap-1", 1, 1, (1, 1, 1)),
                SampleClassProfile("cap-2", 2, 2, (2, 2, 1)),
                SampleClassProfile("cap-4", 4, 4, (4, 4, 3)),
                SampleClassProfile("cap-8", 8, 8, (8, 8, 7)),
            ),
        )
    raise ValueError("unsupported workload: " + workload_id)


def generate_sample_sequence(manifest: WorkloadManifest) -> Tuple[OpaqueWorkloadSample, ...]:
    warmup_count = manifest.warmup_seconds * 1000 // manifest.period_ms
    total = warmup_count + manifest.expected_measured_samples
    if len(manifest.class_profiles) == 1:
        profile = manifest.class_profiles[0]
        pairs = [(profile.class_id, profile.actual_extents[0])] * total
    else:
        pairs = []
        block_number = 0
        while len(pairs) < total:
            block = [
                (profile.class_id, extent)
                for profile in manifest.class_profiles
                for extent in profile.actual_extents
            ]
            random.Random(manifest.seed + block_number).shuffle(block)
            pairs.extend(block)
            block_number += 1
    return tuple(
        OpaqueWorkloadSample(
            sample_id=sample_id, class_id=class_id,
            actual_source_items=actual_source_items,
            phase="warmup" if sample_id < warmup_count else "measured",
        )
        for sample_id, (class_id, actual_source_items) in
        enumerate(pairs[:total])
    )


def opaque_source_payload(
        manifest: WorkloadManifest,
        sample: OpaqueWorkloadSample,
        segment_index: int) -> bytes:
    """Generate opaque bytes without exposing their contents to scheduling."""
    if segment_index < 0 or segment_index >= sample.actual_source_items:
        raise ValueError("segment index outside actual sample extent")
    identity = (
        f"{manifest.digest}:{manifest.seed}:{sample.sample_id}:{segment_index}"
    ).encode("ascii")
    output = bytearray()
    counter = 0
    while len(output) < manifest.segment_payload_bytes:
        output.extend(hashlib.sha256(
            identity + b":" + str(counter).encode("ascii")).digest())
        counter += 1
    return bytes(output[:manifest.segment_payload_bytes])


class CompleteSampleTracker:
    """Fail-closed ordered completion over authenticated opaque segments."""

    def __init__(self, manifest: WorkloadManifest,
                 expected_samples: Iterable[OpaqueWorkloadSample]) -> None:
        self._manifest = manifest
        ordered = tuple(expected_samples)
        if not ordered or len({sample.sample_id for sample in ordered}) != len(ordered):
            raise ValueError("expected samples must be nonempty and uniquely identified")
        self._order = tuple(sample.sample_id for sample in ordered)
        self._expected = {sample.sample_id: sample for sample in ordered}
        self._received: Dict[int, Dict[int, str]] = {
            sample.sample_id: {} for sample in ordered
        }
        self._recovered: Dict[int, int] = {sample.sample_id: 0 for sample in ordered}
        self._ready: Dict[int, CompleteSampleReceipt] = {}
        self._failed: Dict[int, str] = {}
        self._emitted: List[CompleteSampleReceipt] = []
        self._next_index = 0
        self._stopped = False
        self.duplicates = 0
        self.invalid_segments = 0
        self.partial_samples = 0
        self.post_stop_ignored = 0

    def accept(self, sample_id: int, segment_index: int, content: bytes, *,
               completed_timestamp_us: int,
               recovered: bool = False) -> Tuple[CompleteSampleReceipt, ...]:
        if self._stopped:
            self.post_stop_ignored += 1
            return ()
        sample = self._expected.get(sample_id)
        if sample is None or segment_index < 0 or segment_index >= sample.actual_source_items:
            self.invalid_segments += 1
            return ()
        if sample_id in {receipt.sample_id for receipt in self._emitted} or \
                segment_index in self._received[sample_id]:
            self.duplicates += 1
            return ()
        expected = opaque_source_payload(self._manifest, sample, segment_index)
        digest = hashlib.sha256(content).hexdigest()
        if digest != hashlib.sha256(expected).hexdigest():
            self.invalid_segments += 1
            return ()
        self._received[sample_id][segment_index] = digest
        if recovered:
            self._recovered[sample_id] += 1
        if len(self._received[sample_id]) == sample.actual_source_items:
            self._ready[sample_id] = CompleteSampleReceipt(
                sample_id=sample_id,
                class_id=sample.class_id,
                source_digests=tuple(
                    self._received[sample_id][index]
                    for index in range(sample.actual_source_items)),
                completed_timestamp_us=completed_timestamp_us,
                recovered_items=self._recovered[sample_id],
                phase=sample.phase,
            )
        return self._drain_ready()

    def _drain_ready(self) -> Tuple[CompleteSampleReceipt, ...]:
        emitted = []
        while self._next_index < len(self._order):
            next_sample_id = self._order[self._next_index]
            if next_sample_id in self._failed:
                self._next_index += 1
                continue
            receipt = self._ready.get(next_sample_id)
            if receipt is None:
                break
            emitted.append(receipt)
            self._emitted.append(receipt)
            del self._ready[next_sample_id]
            self._next_index += 1
        return tuple(emitted)

    def fail_sample(self, sample_id: int, reason: str) -> \
            Tuple[CompleteSampleReceipt, ...]:
        if self._stopped:
            raise RuntimeError("complete-sample tracker is stopped")
        if not reason:
            raise ValueError("sample failure reason must be nonempty")
        if sample_id not in self._expected:
            raise ValueError("cannot fail an unknown sample")
        if sample_id in self._failed or any(
                receipt.sample_id == sample_id for receipt in self._emitted):
            raise ValueError("sample is already terminal")
        if sample_id in self._ready:
            raise ValueError("cannot fail a complete sample")
        self._failed[sample_id] = reason
        self._received[sample_id].clear()
        self._recovered[sample_id] = 0
        return self._drain_ready()

    def terminalize_incomplete(self, reason: str) -> \
            Tuple[CompleteSampleReceipt, ...]:
        """Fail every incomplete sample and release complete successors.

        This is an application-side end-of-window fence. It does not infer a
        workload-specific reason from native item cursors; it records one
        explicit generic terminal reason for every sample that never became
        complete before the frozen deadline.
        """
        emitted: List[CompleteSampleReceipt] = []
        terminal = {receipt.sample_id for receipt in self._emitted}
        terminal.update(self._failed)
        for sample_id in self._order:
            if sample_id in terminal or sample_id in self._ready:
                continue
            newly_emitted = self.fail_sample(sample_id, reason)
            emitted.extend(newly_emitted)
            terminal.add(sample_id)
            terminal.update(receipt.sample_id for receipt in newly_emitted)
        emitted.extend(self._drain_ready())
        return tuple(emitted)

    @property
    def skips_by_reason(self) -> Dict[str, int]:
        result: Dict[str, int] = {}
        for reason in self._failed.values():
            result[reason] = result.get(reason, 0) + 1
        return result

    def stop(self) -> None:
        if self._stopped:
            return
        self.partial_samples = sum(
            0 < len(segments) < self._expected[sample_id].actual_source_items
            for sample_id, segments in self._received.items()
        )
        self._stopped = True

    def snapshot(self) -> Tuple[object, ...]:
        """Return delivery state; ignored post-stop callbacks cannot change it."""
        return (
            tuple(receipt.sample_id for receipt in self._emitted),
            tuple((sample_id, tuple(sorted(segments)))
                  for sample_id, segments in sorted(self._received.items())),
            tuple(sorted(self._ready)), tuple(sorted(self._failed.items())),
            self._next_index, self.partial_samples,
            self._stopped,
        )
