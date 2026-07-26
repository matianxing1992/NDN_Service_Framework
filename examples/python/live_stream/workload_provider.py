#!/usr/bin/env python3
"""Application-neutral Mapping v2 workload publisher for Spec 127."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import time
from typing import Callable, Iterable, Mapping, Tuple

from workload_common import (
    OpaqueWorkloadSample,
    WorkloadManifest,
    build_semantic_prefix,
    opaque_source_payload,
)


def build_live_stream_definition(
        manifest: WorkloadManifest, *, provider_name: str,
        semantic_prefix: str, session_epoch: int, mapping_version: int,
        recovery_scheme: str | None = None,
        campaign_label: str = "spec127"):
    """Translate only generic manifest bounds into the existing public API."""
    from ndnsf import (
        LiveStreamDefinition,
        LiveStreamFecOptions,
        SampleClassProfile,
        STREAM_NAME_MAP_CONTRACT_VERSION_V2,
    )

    if manifest.workload_id not in {"periodic-sensor", "variable-multisegment"}:
        raise ValueError("unsupported workload definition")
    scheme = recovery_scheme or manifest.fec_rule
    maximum_sources = max(profile.hard_max_source_items
                          for profile in manifest.class_profiles)
    if scheme == "none":
        fec = LiveStreamFecOptions.none()
    elif scheme == "xor-one-repair":
        fec = LiveStreamFecOptions.xor_one_repair(
            maximum_sources, manifest.segment_payload_bytes, 500)
    elif scheme == "gf256-two-repair":
        fec = LiveStreamFecOptions.gf256_two_repair(
            maximum_sources, manifest.segment_payload_bytes, 500)
    else:
        raise ValueError("unsupported generic recovery scheme")
    block_capacity = 16
    total_samples = (
        (manifest.warmup_seconds + manifest.measurement_seconds) * 1000 //
        manifest.period_ms)
    return LiveStreamDefinition(
        stream_id=f"{campaign_label}-{manifest.workload_id}",
        provider=provider_name,
        semantic_data_prefix=semantic_prefix,
        session_epoch=int(session_epoch),
        mapping_version=int(mapping_version),
        contract_version=STREAM_NAME_MAP_CONTRACT_VERSION_V2,
        mapping_block_capacity=block_capacity,
        mapping_ahead_blocks=4,
        retained_items=1024,
        max_name_reservations=(total_samples + 1) * block_capacity,
        max_pending_interests=256,
        sample_period_ms=float(manifest.period_ms),
        sample_classes=tuple(
            SampleClassProfile(
                profile.class_id, profile.seed_source_items,
                profile.hard_max_source_items)
            for profile in manifest.class_profiles),
        fec=fec,
    )


class WorkloadPublisherSession:
    """Publish opaque workload samples through the existing Mapping v2 API."""

    def __init__(self, publisher, manifest: WorkloadManifest,
                 semantic_prefix: str, *, repair_item_count: int | None = None) -> None:
        if manifest.workload_id not in {
                "periodic-sensor", "variable-multisegment"}:
            raise ValueError("unsupported workload publication session")
        self._publisher = publisher
        self.manifest = manifest
        self.semantic_prefix = semantic_prefix.rstrip("/")
        self.repair_item_count = (int(repair_item_count)
            if repair_item_count is not None else
            int(manifest.fec_rule == "xor-one-repair"))
        if self.repair_item_count < 0:
            raise ValueError("repair item count must be nonnegative")
        if not self.semantic_prefix.startswith("/"):
            raise ValueError("semantic prefix must be an absolute NDN name")
        self._stopped = False
        self._paused = False

    def item_name(self, sample_id: int, segment_index: int, kind: str) -> str:
        if kind not in {"source", "repair"}:
            raise ValueError("unsupported workload item kind")
        return (f"{self.semantic_prefix}/sample/{sample_id}/"
                f"{kind}/{segment_index}")

    def announce(self, sample: OpaqueWorkloadSample):
        if self._stopped:
            raise RuntimeError("publication session is stopped")
        return self._publisher.announce_sample(
            sample.sample_id, sample.class_id,
            lambda index, kind: self.item_name(sample.sample_id, index, kind))

    def stop(self) -> None:
        self._stopped = True
        self._paused = False

    def pause(self) -> None:
        if self._stopped:
            raise RuntimeError("cannot pause a stopped publication session")
        self._paused = True

    def resume(self) -> None:
        if self._stopped:
            raise RuntimeError("cannot resume a stopped publication session")
        self._paused = False

    @property
    def stopped(self) -> bool:
        return self._stopped

    @property
    def paused(self) -> bool:
        return self._paused


def run_workload_publication(
        session: WorkloadPublisherSession,
        samples: Iterable[OpaqueWorkloadSample], *,
        monotonic: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        on_publication_intent: Callable[[dict], None] | None = None,
        on_published: Callable[[dict], None] | None = None,
        pause_before_sample: Mapping[int, float] | None = None,
        announcement_ahead_samples: int = 4,
        publication_clock: Callable[[], float] | None = None) -> dict:
    """Run an absolute-deadline cadence; announced names stay payload-opaque."""
    ordered: Tuple[OpaqueWorkloadSample, ...] = tuple(samples)
    if not ordered:
        raise ValueError("periodic sample sequence must be nonempty")
    if session.stopped:
        raise RuntimeError("publication session is stopped")
    if announcement_ahead_samples <= 0:
        raise ValueError("announcement ahead must be positive")
    ahead = min(int(announcement_ahead_samples), len(ordered))
    reservations = {
        offset: session.announce(ordered[offset]) for offset in range(ahead)
    }
    started = monotonic()
    publications = []
    pause_count = 0
    paused_duration_us = 0
    pauses = dict(pause_before_sample or {})
    timestamp = monotonic if publication_clock is None else publication_clock
    for offset, sample in enumerate(ordered):
        if session.stopped:
            break
        reservation = reservations.pop(offset)
        pause_seconds = float(pauses.get(sample.sample_id, 0.0))
        if pause_seconds < 0:
            raise ValueError("pause duration must be nonnegative")
        if pause_seconds:
            session.pause()
            pause_started = monotonic()
            sleep(pause_seconds)
            actual_pause = monotonic() - pause_started
            if session.stopped:
                break
            session.resume()
            started += actual_pause
            pause_count += 1
            paused_duration_us += int(round(actual_pause * 1_000_000))
        deadline = started + offset * session.manifest.period_ms / 1000.0
        remaining = deadline - monotonic()
        if remaining > 0:
            sleep(remaining)
        if session.stopped:
            break
        prepared = session._publisher.prepare_sample_extent(
            reservation, sample.actual_source_items)
        if len(prepared) != sample.actual_source_items:
            raise RuntimeError("prepared extent differs from workload sample")
        payloads = tuple(
            opaque_source_payload(session.manifest, sample, index)
            for index in range(sample.actual_source_items)
        )
        published_us = int(round(timestamp() * 1_000_000))
        publication = {
            "sampleId": sample.sample_id,
            "classId": sample.class_id,
            "phase": sample.phase,
            "actualSourceItems": sample.actual_source_items,
            "repairSelected": session.manifest.fec_rule == "xor-one-repair",
            "repairItems": session.repair_item_count,
            "publishedTimestampUs": published_us,
            "sourceDigests": [hashlib.sha256(value).hexdigest()
                              for value in payloads],
        }
        if on_publication_intent is not None:
            on_publication_intent(dict(publication))
        session._publisher.publish_sample(reservation, payloads)
        publications.append(publication)
        if on_published is not None:
            on_published(dict(publication))
        next_offset = offset + ahead
        if not session.stopped and next_offset < len(ordered):
            reservations[next_offset] = session.announce(ordered[next_offset])
    return {
        "schemaVersion": "spec127-workload-provider-v1",
        "workloadId": session.manifest.workload_id,
        "manifestDigest": session.manifest.digest,
        "periodMs": session.manifest.period_ms,
        "announcementAheadSamples": ahead,
        "expectedSamples": len(ordered),
        "publishedSamples": len(publications),
        "publishedMeasuredSamples": sum(
            value["phase"] == "measured" for value in publications),
        "necessarySourceRepairItems": sum(
            value["actualSourceItems"] + int(value.get("repairItems", 0))
            for value in publications),
        "pauseCount": pause_count,
        "pausedDurationUs": paused_duration_us,
        "stopped": session.stopped,
        "publications": publications,
    }


# Source-compatible names retained for the independently tested periodic slice.
PeriodicPublisherSession = WorkloadPublisherSession
run_periodic_publication = run_workload_publication


def _write_json_atomic(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _native_status(native) -> dict:
    fields = (
        "state", "reason", "retained_items", "pending_interests",
        "mapping_blocks", "delivered", "rejected", "recovered", "timeouts",
        "nacks", "retry_attempts", "mapping_interests",
        "mapping_data_responses", "mapping_new_data_responses",
        "payload_interests", "mapping_bytes", "provider_future_interests",
        "provider_future_hits", "provider_initial_future_interests",
        "provider_initial_future_hits", "provider_retry_future_interests",
        "provider_retry_future_hits", "declared_recovery_capacity",
    )
    result = {
        field: (str(getattr(native, field)) if field in {"state", "reason"}
                else int(getattr(native, field)))
        for field in fields
    }
    result["sampleClassPredictions"] = {
        str(class_id): {
            "prediction": int(value.prediction),
            "observations": int(value.observations),
            "underpredictions": int(value.underpredictions),
            "underpredictedItems": int(value.underpredicted_items),
            "overpredictions": int(value.overpredictions),
            "overpredictedItems": int(value.overpredicted_items),
        }
        for class_id, value in native.sample_class_predictions.items()
    }
    return result


def _counter_delta(final: dict, baseline: dict | None) -> dict:
    if baseline is None:
        return dict(final)
    result = {}
    for key, value in final.items():
        if isinstance(value, int) and isinstance(baseline.get(key), int):
            result[key] = max(0, value - baseline[key])
        else:
            result[key] = value
    return result


def main() -> int:
    from ndnsf import ServiceProvider
    from workload_common import build_workload_manifest, generate_sample_sequence

    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", default="periodic-sensor",
                        choices=("periodic-sensor", "variable-multisegment"))
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--manifest-output", type=Path, required=True)
    parser.add_argument("--publication-log", type=Path, required=True)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--session-epoch", type=int, default=127001)
    parser.add_argument("--mapping-version", type=int, default=127001)
    parser.add_argument("--hold-seconds", type=float, default=3.0)
    parser.add_argument("--recovery-scheme", default=None,
                        choices=("none", "xor-one-repair", "gf256-two-repair"))
    parser.add_argument("--campaign-label", default="spec127",
                        choices=("spec127", "spec128"))
    args = parser.parse_args()

    manifest = build_workload_manifest(args.workload)
    samples = generate_sample_sequence(manifest)
    provider_name = "/example/live/provider"
    semantic_prefix = build_semantic_prefix(
        provider_name, args.workload, args.mapping_version, args.campaign_label)
    provider = ServiceProvider(
        group="/example/live/group", controller="/example/live/controller",
        provider_prefix=provider_name,
        trust_schema="examples/trust-schema.conf", serve_certificates=True)
    provider.add_handler("/LiveStream/Dummy", lambda payload: payload)
    publisher = None
    status = None
    try:
        provider.start_background()
        definition = build_live_stream_definition(
            manifest, provider_name=provider_name,
            semantic_prefix=semantic_prefix,
            session_epoch=args.session_epoch,
            mapping_version=args.mapping_version,
            recovery_scheme=args.recovery_scheme,
            campaign_label=args.campaign_label)
        publisher = provider.create_live_stream(definition)
        _write_json_atomic(args.manifest_output, manifest.to_dict())
        args.publication_log.parent.mkdir(parents=True, exist_ok=True)
        args.publication_log.write_text("", encoding="utf-8")
        descriptor_written = False
        warmup_native_status = None

        def record_intent(publication: dict) -> None:
            with args.publication_log.open("a", encoding="utf-8") as output:
                output.write(json.dumps(publication, sort_keys=True) + "\n")
                output.flush()

        def activate_after_first(publication: dict) -> None:
            nonlocal descriptor_written, warmup_native_status
            if not descriptor_written:
                descriptor = publisher.activate(
                    measured_sample_period_ms=float(manifest.period_ms),
                    safe_join_cursor=0)
                _write_json_atomic(args.descriptor, descriptor.to_dict())
                descriptor_written = True
            if publication["phase"] == "warmup" and \
                    publication["sampleId"] == 49:
                warmup_native_status = _native_status(publisher.status())

        # Route registration is asynchronous; it normally settles while the
        # names are announced, and this bounded setup wait is outside warm-up.
        time.sleep(0.5)
        session = PeriodicPublisherSession(
            publisher, manifest, semantic_prefix,
            repair_item_count=int(definition.fec.repair_symbols))
        status = run_periodic_publication(
            session, samples, publication_clock=time.time,
            on_publication_intent=record_intent,
            on_published=activate_after_first)
        status["nativeStatus"] = _native_status(publisher.status())
        status["warmupNativeStatus"] = warmup_native_status
        status["measuredNativeStatus"] = _counter_delta(
            status["nativeStatus"], warmup_native_status)
        status["measuredNecessarySourceRepairItems"] = sum(
            value["actualSourceItems"] + int(value.get("repairItems", 0))
            for value in status["publications"] if value["phase"] == "measured")
        status["descriptorWritten"] = descriptor_written
        status["passed"] = (
            descriptor_written and status["publishedSamples"] == len(samples)
            and status["publishedMeasuredSamples"] ==
            manifest.expected_measured_samples)
        _write_json_atomic(args.status, status)
        time.sleep(max(0.0, args.hold_seconds))
        return 0 if status["passed"] else 2
    except Exception as error:
        failure = dict(status or {})
        failure.update({
            "schemaVersion": "spec127-workload-provider-v1",
            "workloadId": args.workload,
            "manifestDigest": manifest.digest,
            "passed": False,
            "error": f"{type(error).__name__}: {error}",
        })
        _write_json_atomic(args.status, failure)
        return 2
    finally:
        if publisher is not None:
            publisher.stop()
        provider.stop()


if __name__ == "__main__":
    raise SystemExit(main())
