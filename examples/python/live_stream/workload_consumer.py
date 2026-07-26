#!/usr/bin/env python3
"""Application-neutral complete-sample consumer for Spec 127 workloads."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import time
from typing import Callable, Iterable, Mapping, Tuple

from workload_common import (
    CompleteSampleReceipt,
    CompleteSampleTracker,
    OpaqueWorkloadSample,
    WorkloadManifest,
)


@dataclass(frozen=True)
class ItemDecision:
    accepted: bool
    reason: str
    completed: Tuple[CompleteSampleReceipt, ...] = ()


class WorkloadConsumerSession:
    """Turn Provider-validated opaque items into ordered complete samples."""

    _NATIVE_COUNTERS = (
        "delivered", "recovered", "rejected", "timeouts", "nacks",
        "retry_attempts", "late_arrivals", "deadline_skips",
        "retry_exhaustions", "mapping_interests", "mapping_data_responses",
        "mapping_new_data_responses", "mapping_bytes", "payload_interests",
        "future_payload_interests", "provider_future_interests",
        "provider_future_hits", "initial_payload_interests",
        "retry_payload_interests", "initial_future_payload_interests",
        "retry_future_payload_interests", "retry_successes",
        "retry_suppressions", "declared_recovery_capacity",
        "recovery_attempts", "recovery_exhaustions",
    )

    def __init__(
            self, manifest: WorkloadManifest,
            expected_samples: Iterable[OpaqueWorkloadSample],
            semantic_prefix: str, *, expected_provider: str,
            publication_times_us: Mapping[int, int] | None = None,
            publication_time_lookup: Callable[[int], int | None] | None = None,
            on_complete: Callable[[CompleteSampleReceipt, float | None], None] |
            None = None) -> None:
        if manifest.workload_id not in {
                "periodic-sensor", "variable-multisegment"}:
            raise ValueError("unsupported workload consumer session")
        self.manifest = manifest
        self._samples = tuple(expected_samples)
        self._tracker = CompleteSampleTracker(manifest, self._samples)
        self._prefix = semantic_prefix.rstrip("/")
        self._expected_provider = expected_provider
        self._publication_times_us = dict(publication_times_us or {})
        self._publication_time_lookup = publication_time_lookup
        self._on_complete = on_complete
        self._stopped = False
        self._completed: list[CompleteSampleReceipt] = []
        self._latencies_ms: list[float] = []
        self._measured_latencies_ms: list[float] = []
        self._post_stop_ignored = 0
        self._warmup_native_status: dict[str, int] | None = None
        self._name_pattern = re.compile(
            rf"^{re.escape(self._prefix)}/sample/(\d+)/source/(\d+)$")

    def accept_item(self, item) -> ItemDecision:
        if self._stopped:
            self._post_stop_ignored += 1
            return ItemDecision(False, "consumer-stopped")
        if str(item.verified_provider) != self._expected_provider:
            return ItemDecision(False, "provider-mismatch")
        matched = self._name_pattern.fullmatch(str(item.original_name))
        if matched is None:
            return ItemDecision(False, "semantic-name-mismatch")
        sample_id, segment_index = (int(value) for value in matched.groups())
        invalid_before = self._tracker.invalid_segments
        duplicates_before = self._tracker.duplicates
        completed = self._tracker.accept(
            sample_id, segment_index, bytes(item.content),
            completed_timestamp_us=int(item.received_ms) * 1000,
            recovered=str(item.provenance) == "fec-recovered")
        if self._tracker.invalid_segments != invalid_before:
            return ItemDecision(False, "content-digest-mismatch")
        if self._tracker.duplicates != duplicates_before:
            return ItemDecision(False, "duplicate-item")
        for receipt in completed:
            latency_ms = self.publication_latency_ms(receipt)
            self._completed.append(receipt)
            if latency_ms is not None:
                self._latencies_ms.append(latency_ms)
                if receipt.phase == "measured":
                    self._measured_latencies_ms.append(latency_ms)
            if self._on_complete is not None:
                self._on_complete(receipt, latency_ms)
        return ItemDecision(True, "accepted", completed)

    def stop(self) -> None:
        if self._stopped:
            return
        self._tracker.stop()
        self._stopped = True

    def fail_sample(self, sample_id: int, reason: str) -> \
            Tuple[CompleteSampleReceipt, ...]:
        completed = self._tracker.fail_sample(int(sample_id), str(reason))
        self._record_completed(completed)
        return completed

    def terminalize_incomplete(self, reason: str) -> \
            Tuple[CompleteSampleReceipt, ...]:
        completed = self._tracker.terminalize_incomplete(str(reason))
        self._record_completed(completed)
        return completed

    def _record_completed(
            self, completed: Iterable[CompleteSampleReceipt]) -> None:
        for receipt in completed:
            latency_ms = self.publication_latency_ms(receipt)
            self._completed.append(receipt)
            if latency_ms is not None:
                self._latencies_ms.append(latency_ms)
                if receipt.phase == "measured":
                    self._measured_latencies_ms.append(latency_ms)
            if self._on_complete is not None:
                self._on_complete(receipt, latency_ms)

    def publication_latency_ms(
            self, receipt: CompleteSampleReceipt) -> float | None:
        published_us = self._publication_times_us.get(receipt.sample_id)
        if published_us is None and self._publication_time_lookup is not None:
            published_us = self._publication_time_lookup(receipt.sample_id)
        return None if published_us is None else (
            receipt.completed_timestamp_us - published_us) / 1000.0

    @property
    def complete_measured_samples(self) -> int:
        return sum(receipt.phase == "measured" for receipt in self._completed)

    @property
    def complete_samples(self) -> int:
        return len(self._completed)

    def capture_warmup_native_status(self, native_status) -> None:
        if self._warmup_native_status is None:
            self._warmup_native_status = {
                name: int(getattr(native_status, name))
                for name in self._NATIVE_COUNTERS
            }

    def status(self, native_status=None) -> dict:
        native = {
            name: (None if native_status is None else
                   int(getattr(native_status, name)))
            for name in self._NATIVE_COUNTERS
        }
        result = {
            "schemaVersion": "spec127-workload-consumer-v1",
            "workloadId": self.manifest.workload_id,
            "manifestDigest": self.manifest.digest,
            "expectedSamples": len(self._samples),
            "expectedMeasuredSamples": sum(
                sample.phase == "measured" for sample in self._samples),
            "completeSamples": len(self._completed),
            "completeMeasuredSamples": sum(
                receipt.phase == "measured" for receipt in self._completed),
            "completeSampleIds": [receipt.sample_id
                                  for receipt in self._completed],
            "duplicates": self._tracker.duplicates,
            "invalidItems": self._tracker.invalid_segments,
            "partialSamples": self._tracker.partial_samples,
            "recoveredItems": sum(receipt.recovered_items
                                  for receipt in self._completed),
            "recoveredSamples": sum(receipt.recovered_items > 0
                                    for receipt in self._completed),
            "outOfOrderSamples": 0,
            "postStopIgnored": self._post_stop_ignored,
            "skipsByReason": self._tracker.skips_by_reason,
            "stopped": self._stopped,
            "publicationToDeliveryMs": list(self._latencies_ms),
            "measuredPublicationToDeliveryMs":
                list(self._measured_latencies_ms),
            "receipts": [asdict(receipt) for receipt in self._completed],
            "nativeStatus": native,
            "nativeState": (None if native_status is None else
                            str(native_status.state)),
            "nativeReason": (None if native_status is None else
                             str(native_status.reason)),
            "retrySuppressionReasons": ({} if native_status is None else {
                str(reason): int(count) for reason, count in
                native_status.retry_suppression_reasons.items()
            }),
        }
        if self._warmup_native_status is not None:
            result["warmupNativeStatus"] = dict(self._warmup_native_status)
            result["measuredNativeStatus"] = {
                name: (None if native[name] is None else
                       max(0, native[name] - self._warmup_native_status[name]))
                for name in self._NATIVE_COUNTERS
            }
        if native_status is None:
            result["nativeStatusUnavailableReason"] = "no-live-handle-status"
        return result


def open_workload_consumer(user, descriptor,
                           session: WorkloadConsumerSession, *,
                           aggregate_interest_limit: int = 64,
                           interest_lifetime_ms: int = 500):
    """Open Mapping v2 consumption without selecting an app-specific policy."""
    from ndnsf import LiveStreamItemAdmission

    handle_ref = {}

    def on_item(item):
        decision = session.accept_item(item)
        if not decision.accepted:
            return LiveStreamItemAdmission.reject_item(decision.reason)
        handle = handle_ref.get("handle")
        if handle is not None:
            for receipt in decision.completed:
                latency_ms = session.publication_latency_ms(receipt)
                if latency_ms is not None:
                    handle.observe_accepted_sample(
                        receipt.sample_id,
                        receipt.completed_timestamp_us // 1000,
                        latency_ms,
                        len(receipt.source_digests),
                    )
        return LiveStreamItemAdmission.accept_item()

    handle = user.open_live_stream(
        descriptor, start="beginning",
        aggregate_interest_limit=int(aggregate_interest_limit),
        enable_fec_recovery=session.manifest.fec_rule == "xor-one-repair",
        interest_lifetime_ms=int(interest_lifetime_ms),
        on_item=on_item,
    )
    handle_ref["handle"] = handle
    return handle


# Source-compatible names retained for the independently tested periodic slice.
PeriodicConsumerSession = WorkloadConsumerSession
open_periodic_consumer = open_workload_consumer


class PublicationLogReader:
    """Incrementally join immutable sample IDs with Provider publication time."""

    def __init__(self, path: Path) -> None:
        self._path = path
        self._offset = 0
        self._values: dict[int, int] = {}

    def lookup(self, sample_id: int) -> int | None:
        try:
            with self._path.open("r", encoding="utf-8") as source:
                source.seek(self._offset)
                for line in source:
                    value = json.loads(line)
                    self._values[int(value["sampleId"])] = int(
                        value["publishedTimestampUs"])
                self._offset = source.tell()
        except FileNotFoundError:
            return None
        return self._values.get(int(sample_id))


def _write_json_atomic(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    from ndnsf import LiveStreamDescriptor, ServiceUser
    from workload_common import WorkloadManifest, generate_sample_sequence

    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", default="periodic-sensor",
                        choices=("periodic-sensor", "variable-multisegment"))
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--publication-log", type=Path, required=True)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=75.0)
    parser.add_argument("--minimum-measured-completion-ratio", type=float,
                        default=1.0)
    args = parser.parse_args()
    if not 0.0 < args.minimum_measured_completion_ratio <= 1.0:
        parser.error("minimum measured completion ratio must be in (0, 1]")

    deadline = time.monotonic() + max(1.0, args.timeout_seconds)
    while (not args.descriptor.exists() or not args.manifest.exists()) and \
            time.monotonic() < deadline:
        time.sleep(0.02)
    user = None
    handle = None
    session = None
    native_status = None
    try:
        manifest = WorkloadManifest.from_dict(
            json.loads(args.manifest.read_text(encoding="utf-8")))
        if manifest.workload_id != args.workload:
            raise ValueError("workload manifest identity mismatch")
        descriptor = LiveStreamDescriptor.from_dict(
            json.loads(args.descriptor.read_text(encoding="utf-8")))
        provider_name = "/example/live/provider"
        semantic_prefix = descriptor.semantic_data_prefix
        ledger = PublicationLogReader(args.publication_log)
        session = PeriodicConsumerSession(
            manifest, generate_sample_sequence(manifest), semantic_prefix,
            expected_provider=provider_name,
            publication_time_lookup=ledger.lookup)
        user = ServiceUser(
            group="/example/live/group", controller="/example/live/controller",
            user="/example/live/user", trust_schema="examples/trust-schema.conf",
            serve_certificates=True)
        user.start()
        handle = open_periodic_consumer(user, descriptor, session)
        handle.start()
        warmup_captured = False
        while session.complete_measured_samples < \
                manifest.expected_measured_samples and time.monotonic() < deadline:
            if not warmup_captured and session.complete_samples >= 50:
                session.capture_warmup_native_status(handle.status())
                warmup_captured = True
            time.sleep(0.02)
        native_status = handle.status()
        session.terminalize_incomplete("measurement-window-terminal-skip")
        session.stop()
        result = session.status(native_status)
        measured_ratio = (
            result["completeMeasuredSamples"] /
            manifest.expected_measured_samples)
        result["minimumMeasuredCompletionRatio"] = \
            args.minimum_measured_completion_ratio
        result["measuredCompletionRatio"] = measured_ratio
        result["passed"] = (
            measured_ratio >= args.minimum_measured_completion_ratio
            and result["duplicates"] == 0 and result["partialSamples"] == 0
            and result["outOfOrderSamples"] == 0
            and result["invalidItems"] == 0)
        if len(result["publicationToDeliveryMs"]) != result["completeSamples"]:
            result["passed"] = False
            result["latencyUnavailableReason"] = "publication-ledger-incomplete"
        _write_json_atomic(args.status, result)
        return 0 if result["passed"] else 2
    except Exception as error:
        failure = {
            "schemaVersion": "spec127-workload-consumer-v1",
            "workloadId": args.workload,
            "passed": False,
            "error": f"{type(error).__name__}: {error}",
        }
        _write_json_atomic(args.status, failure)
        return 2
    finally:
        if handle is not None:
            handle.stop()
        if session is not None:
            session.stop()
        if user is not None:
            user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
