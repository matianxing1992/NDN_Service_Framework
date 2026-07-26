#!/usr/bin/env python3

from dataclasses import replace
from pathlib import Path
import inspect
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
LIVE_STREAM_EXAMPLES = REPO / "examples/python/live_stream"
EXPERIMENTS = REPO / "Experiments"
sys.path.insert(0, str(LIVE_STREAM_EXAMPLES))
sys.path.insert(0, str(EXPERIMENTS))

from workload_common import (
    CompleteSampleTracker,
    TrafficUtilityReport,
    build_semantic_prefix,
    build_workload_manifest,
    generate_sample_sequence,
    opaque_source_payload,
)
from workload_provider import (
    PeriodicPublisherSession,
    WorkloadPublisherSession,
    build_live_stream_definition,
    run_periodic_publication,
    run_workload_publication,
)
from workload_consumer import (
    PeriodicConsumerSession,
    PublicationLogReader,
    WorkloadConsumerSession,
    open_periodic_consumer,
    open_workload_consumer,
)
from NDNSF_LiveStream_Generality_Minindn import build_commands, planned_cell


class FakeClock:
    def __init__(self):
        self.now = 10.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class FakeReservation:
    def __init__(self, sample_id, sample_class, names):
        self.sample_id = sample_id
        self.sample_class = sample_class
        self.predicted_source_items = 1
        self.names = names


class FakePublisher:
    def __init__(self):
        self.calls = []
        self.published = []

    def announce_sample(self, sample_id, sample_class, name_factory):
        names = (name_factory(0, "source"),)
        self.calls.append(("announce", sample_id, sample_class, names))
        return FakeReservation(sample_id, sample_class, names)

    def prepare_sample_extent(self, reservation, actual_source_items):
        self.calls.append(("prepare", reservation.sample_id, actual_source_items))
        return tuple(range(actual_source_items))

    def publish_sample(self, reservation, opaque_sources):
        payloads = tuple(opaque_sources)
        self.calls.append(("publish", reservation.sample_id, len(payloads)))
        self.published.append((reservation, payloads))


class FakeVerifiedItem:
    def __init__(self, name, content, *, received_ms=1000,
                 provider="/example/spec127/provider",
                 provenance="signed-data"):
        self.original_name = name
        self.content = content
        self.received_ms = received_ms
        self.verified_provider = provider
        self.provenance = provenance


class FakeConsumerHandle:
    def __init__(self):
        self.observations = []

    def observe_accepted_sample(self, sample_id, arrival_ms,
                                retrieval_delay_ms, item_count=1):
        self.observations.append((sample_id, arrival_ms,
                                  retrieval_delay_ms, item_count))
        return True


class FakeUser:
    def __init__(self):
        self.descriptor = None
        self.options = None
        self.handle = FakeConsumerHandle()

    def open_live_stream(self, descriptor, **options):
        self.descriptor = descriptor
        self.options = options
        return self.handle


class WorkloadManifestTest(unittest.TestCase):
    def test_periodic_manifest_and_measurement_identity_are_deterministic(self):
        first = build_workload_manifest("periodic-sensor")
        second = build_workload_manifest("periodic-sensor")

        self.assertEqual(first.digest, second.digest)
        self.assertEqual(first.period_ms, 100)
        self.assertEqual(first.warmup_seconds, 5)
        self.assertEqual(first.measurement_seconds, 60)
        self.assertEqual(first.expected_measured_samples, 600)

        samples = generate_sample_sequence(first)
        self.assertEqual(len(samples), 650)
        self.assertEqual(sum(sample.phase == "warmup" for sample in samples), 50)
        measured = [sample for sample in samples if sample.phase == "measured"]
        self.assertEqual(len(measured), 600)
        self.assertEqual([sample.sample_id for sample in measured], list(range(50, 650)))
        self.assertEqual(samples, generate_sample_sequence(second))

    def test_variable_sequence_is_balanced_seeded_and_within_signed_bounds(self):
        manifest = build_workload_manifest("variable-multisegment")
        samples = generate_sample_sequence(manifest)
        profiles = {profile.class_id: profile for profile in manifest.class_profiles}

        self.assertEqual([profile.hard_max_source_items
                          for profile in manifest.class_profiles], [1, 2, 4, 8])
        self.assertEqual(manifest.segment_payload_bytes, 4096)
        self.assertEqual(manifest.fec_rule, "xor-one-repair")
        self.assertEqual(len(samples), 650)
        for sample in samples:
            self.assertGreaterEqual(sample.actual_source_items, 1)
            self.assertLessEqual(
                sample.actual_source_items,
                profiles[sample.class_id].hard_max_source_items)
        for offset in range(0, 648, 12):
            block = samples[offset:offset + 12]
            self.assertEqual(
                {class_id: sum(sample.class_id == class_id for sample in block)
                 for class_id in profiles},
                {class_id: 3 for class_id in profiles})
        self.assertEqual(samples, generate_sample_sequence(manifest))

    def test_opaque_payload_is_exact_length_stable_and_not_a_scheduler_input(self):
        manifest = build_workload_manifest("variable-multisegment")
        samples = generate_sample_sequence(manifest)
        sample = next(value for value in samples[50:]
                      if value.actual_source_items >= 2)

        first = opaque_source_payload(manifest, sample, 0)
        self.assertEqual(len(first), 4096)
        self.assertEqual(first, opaque_source_payload(manifest, sample, 0))
        self.assertNotEqual(first, opaque_source_payload(manifest, sample, 1))
        self.assertNotEqual(
            first,
            opaque_source_payload(
                manifest,
                next(value for value in samples if value.sample_id != sample.sample_id),
                0))
        self.assertEqual(
            tuple(inspect.signature(generate_sample_sequence).parameters),
            ("manifest",))

    def test_manifest_round_trip_is_canonical_and_invalid_contracts_fail_closed(self):
        manifest = build_workload_manifest("variable-multisegment")
        restored = type(manifest).from_dict(manifest.to_dict())
        self.assertEqual(restored, manifest)
        self.assertEqual(restored.digest, manifest.digest)

        with self.assertRaisesRegex(ValueError, "measured sample count"):
            replace(manifest, expected_measured_samples=599)
        with self.assertRaisesRegex(ValueError, "unique class IDs"):
            replace(manifest, class_profiles=(
                manifest.class_profiles[0], manifest.class_profiles[0]))
        with self.assertRaisesRegex(ValueError, "actual extent"):
            invalid_profile = replace(
                manifest.class_profiles[-1], actual_extents=(9,))
            replace(manifest, class_profiles=(invalid_profile,))
        with self.assertRaisesRegex(ValueError, "schemaVersion"):
            encoded = manifest.to_dict()
            encoded["schemaVersion"] = "unknown"
            type(manifest).from_dict(encoded)


class PeriodicPublisherTest(unittest.TestCase):
    def test_live_definition_is_mapping_v2_and_uses_one_opaque_class(self):
        manifest = build_workload_manifest("periodic-sensor")
        definition = build_live_stream_definition(
            manifest, provider_name="/example/spec127/provider",
            semantic_prefix="/example/spec127/provider/periodic",
            session_epoch=127, mapping_version=127)
        native = definition._to_native()

        self.assertEqual(native.contract_version, 2)
        self.assertEqual(native.mapping_version, 127)
        self.assertEqual(native.mapping_ahead_blocks, 4)
        self.assertEqual(native.sample_period_ms, 100.0)
        self.assertEqual(len(native.sample_classes), 1)
        self.assertEqual(native.sample_classes[0].class_id, "opaque-small")
        self.assertEqual(native.sample_classes[0].hard_max_source_items, 1)
        self.assertFalse(native.fec.enabled)
        self.assertGreaterEqual(
            native.max_name_reservations,
            len(generate_sample_sequence(manifest)) *
            native.mapping_block_capacity)

    def test_live_semantic_prefix_is_bound_to_mapping_version(self):
        self.assertEqual(build_semantic_prefix(
            "/example/spec127/provider", "periodic-sensor", 127001),
            "/example/spec127/provider/spec127/periodic-sensor/v=127001")

    def test_all_samples_use_mapping_v2_sample_api_at_exact_cadence(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)
        publisher = FakePublisher()
        clock = FakeClock()
        session = PeriodicPublisherSession(
            publisher, manifest, "/example/spec127/provider/periodic")

        status = run_periodic_publication(
            session, samples, monotonic=clock.monotonic, sleep=clock.sleep)

        self.assertEqual(status["publishedSamples"], 650)
        self.assertEqual(status["publishedMeasuredSamples"], 600)
        self.assertEqual(status["announcementAheadSamples"], 4)
        self.assertTrue(all(not value["repairSelected"]
                            for value in status["publications"]))
        self.assertEqual(status["manifestDigest"], manifest.digest)
        self.assertEqual(
            [entry["publishedTimestampUs"] for entry in status["publications"]],
            [10_000_000 + sample.sample_id * 100_000 for sample in samples])
        for sample in samples:
            operations = [call[0] for call in publisher.calls
                          if call[1] == sample.sample_id]
            self.assertEqual(operations, ["announce", "prepare", "publish"])
        first_publish = next(index for index, call in enumerate(publisher.calls)
                             if call[0] == "publish")
        self.assertEqual(sum(call[0] == "announce"
                             for call in publisher.calls[:first_publish]), 4)
        for sample, (reservation, payloads) in zip(samples, publisher.published):
            self.assertEqual(reservation.sample_id, sample.sample_id)
            self.assertEqual(payloads, (opaque_source_payload(
                manifest, sample, 0),))
            self.assertEqual(
                reservation.names,
                (f"/example/spec127/provider/periodic/sample/"
                 f"{sample.sample_id}/source/0",))

    def test_pause_resume_shifts_deadlines_without_a_catch_up_burst(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)[:5]
        publisher = FakePublisher()
        clock = FakeClock()
        session = PeriodicPublisherSession(
            publisher, manifest, "/example/spec127/provider/periodic")

        status = run_periodic_publication(
            session, samples, monotonic=clock.monotonic, sleep=clock.sleep,
            pause_before_sample={2: 0.75})

        self.assertEqual(status["pauseCount"], 1)
        self.assertEqual(status["pausedDurationUs"], 750_000)
        self.assertEqual(
            [value["publishedTimestampUs"] for value in status["publications"]],
            [10_000_000, 10_100_000, 10_950_000, 11_050_000, 11_150_000])

    def test_publication_attribution_uses_explicit_receipt_clock_domain(self):
        manifest = build_workload_manifest("periodic-sensor")
        clock = FakeClock()
        status = run_periodic_publication(
            PeriodicPublisherSession(
                FakePublisher(), manifest, "/example/spec127/provider/periodic"),
            generate_sample_sequence(manifest)[:1],
            monotonic=clock.monotonic, sleep=clock.sleep,
            publication_clock=lambda: 1234.5)
        self.assertEqual(status["publications"][0]["publishedTimestampUs"],
                         1_234_500_000)

    def test_stop_fences_every_later_publication_and_api_mutation(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)[:6]
        publisher = FakePublisher()
        clock = FakeClock()
        session = PeriodicPublisherSession(
            publisher, manifest, "/example/spec127/provider/periodic")

        def stop_after_third(publication):
            if publication["sampleId"] == 2:
                session.stop()

        status = run_periodic_publication(
            session, samples, monotonic=clock.monotonic, sleep=clock.sleep,
            on_published=stop_after_third)
        self.assertTrue(status["stopped"])
        self.assertEqual(status["publishedSamples"], 3)
        self.assertEqual([reservation.sample_id
                          for reservation, _payloads in publisher.published],
                         [0, 1, 2])

        before = tuple(publisher.calls)
        with self.assertRaisesRegex(RuntimeError, "stopped"):
            session.announce(samples[4])
        with self.assertRaisesRegex(RuntimeError, "stopped"):
            run_periodic_publication(
                session, samples, monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(tuple(publisher.calls), before)


class VariablePublisherTest(unittest.TestCase):
    def test_variable_definition_and_publication_use_all_signed_class_bounds(self):
        manifest = build_workload_manifest("variable-multisegment")
        definition = build_live_stream_definition(
            manifest, provider_name="/example/spec127/provider",
            semantic_prefix="/example/spec127/provider/variable",
            session_epoch=128, mapping_version=128)
        native = definition._to_native()
        self.assertEqual(native.contract_version, 2)
        self.assertEqual(native.mapping_ahead_blocks, 4)
        self.assertEqual(
            [(profile.class_id, profile.hard_max_source_items)
             for profile in native.sample_classes],
            [("cap-1", 1), ("cap-2", 2), ("cap-4", 4), ("cap-8", 8)])
        self.assertTrue(native.fec.enabled)
        self.assertEqual(native.fec.max_source_items, 8)
        self.assertEqual(native.fec.max_source_bytes, 4096)
        spec128 = build_live_stream_definition(
            manifest, provider_name="/example/spec128/provider",
            semantic_prefix="/example/spec128/provider/variable",
            session_epoch=128001, mapping_version=128001,
            recovery_scheme="gf256-two-repair", campaign_label="spec128")
        self.assertEqual(spec128.stream_id,
                         "spec128-variable-multisegment")
        self.assertEqual(spec128._to_native().fec.recovery_capacity, 2)
        self.assertEqual(spec128._to_native().fec.repair_symbols, 2)
        self.assertGreaterEqual(
            native.max_name_reservations,
            len(generate_sample_sequence(manifest)) *
            native.mapping_block_capacity)

        samples = generate_sample_sequence(manifest)
        publisher = FakePublisher()
        clock = FakeClock()
        status = run_workload_publication(
            WorkloadPublisherSession(
                publisher, manifest, "/example/spec127/provider/variable"),
            samples, monotonic=clock.monotonic, sleep=clock.sleep)
        self.assertEqual(status["publishedSamples"], 650)
        self.assertEqual(status["publishedMeasuredSamples"], 600)
        self.assertTrue(all(value["repairSelected"]
                            for value in status["publications"]))
        self.assertEqual(
            status["necessarySourceRepairItems"],
            sum(sample.actual_source_items + 1 for sample in samples))
        self.assertEqual({sample.class_id for sample in samples},
                         {"cap-1", "cap-2", "cap-4", "cap-8"})
        for sample, (reservation, payloads) in zip(samples, publisher.published):
            self.assertEqual(len(payloads), sample.actual_source_items)
            self.assertTrue(all(len(payload) == 4096 for payload in payloads))
            self.assertEqual(
                payloads,
                tuple(opaque_source_payload(manifest, sample, index)
                      for index in range(sample.actual_source_items)))


class PeriodicConsumerTest(unittest.TestCase):
    def test_publication_ledger_incrementally_exposes_exact_sample_times(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "publication.jsonl"
            path.write_text(
                '{"sampleId": 0, "publishedTimestampUs": 1000000}\n',
                encoding="utf-8")
            reader = PublicationLogReader(path)
            self.assertEqual(reader.lookup(0), 1_000_000)
            with path.open("a", encoding="utf-8") as output:
                output.write(
                    '{"sampleId": 1, "publishedTimestampUs": 1100000}\n')
            self.assertEqual(reader.lookup(1), 1_100_000)
            self.assertEqual(reader.lookup(0), 1_000_000)

    def test_open_uses_default_adaptive_policy_and_reports_completed_observation(self):
        manifest = build_workload_manifest("periodic-sensor")
        sample = generate_sample_sequence(manifest)[0]
        consumer = PeriodicConsumerSession(
            manifest, (sample,), "/example/spec127/provider/periodic",
            expected_provider="/example/spec127/provider",
            publication_times_us={0: 900_000})
        user = FakeUser()

        handle = open_periodic_consumer(user, object(), consumer)

        self.assertIs(handle, user.handle)
        self.assertNotIn("prefetch_policy", user.options)
        decision = user.options["on_item"](FakeVerifiedItem(
            "/example/spec127/provider/periodic/sample/0/source/0",
            opaque_source_payload(manifest, sample, 0), received_ms=1000))
        self.assertTrue(decision._native.accepted)
        self.assertEqual(user.handle.observations, [(0, 1000, 100.0, 1)])

    def test_full_periodic_sequence_yields_exactly_600_measured_receipts(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)
        consumer = PeriodicConsumerSession(
            manifest, samples, "/example/spec127/provider/periodic",
            expected_provider="/example/spec127/provider")
        for sample in samples:
            decision = consumer.accept_item(FakeVerifiedItem(
                f"/example/spec127/provider/periodic/sample/"
                f"{sample.sample_id}/source/0",
                opaque_source_payload(manifest, sample, 0),
                received_ms=1000 + sample.sample_id * 100))
            self.assertTrue(decision.accepted)
        consumer.stop()
        status = consumer.status()

        self.assertEqual(status["completeSamples"], 650)
        self.assertEqual(status["completeMeasuredSamples"], 600)
        self.assertEqual(status["completeSampleIds"], list(range(650)))
        self.assertEqual(status["duplicates"], 0)
        self.assertEqual(status["partialSamples"], 0)
        self.assertEqual(status["outOfOrderSamples"], 0)
        self.assertEqual(status["postStopIgnored"], 0)

    def test_measured_latency_excludes_warmup_receipts(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)
        received_ms = {sample.sample_id: 10_000 + sample.sample_id * 100
                       for sample in samples}
        consumer = PeriodicConsumerSession(
            manifest, samples, "/example/spec127/provider/periodic",
            expected_provider="/example/spec127/provider",
            publication_times_us={
                sample.sample_id: received_ms[sample.sample_id] * 1000 -
                (1_000_000 if sample.phase == "warmup" else 10_000)
                for sample in samples})
        for sample in samples:
            consumer.accept_item(FakeVerifiedItem(
                f"/example/spec127/provider/periodic/sample/"
                f"{sample.sample_id}/source/0",
                opaque_source_payload(manifest, sample, 0),
                received_ms=received_ms[sample.sample_id]))
        consumer.stop()
        status = consumer.status()
        self.assertEqual(len(status["publicationToDeliveryMs"]), 650)
        self.assertEqual(len(status["measuredPublicationToDeliveryMs"]), 600)
        self.assertEqual(set(status["measuredPublicationToDeliveryMs"]), {10.0})

    def test_byte_exact_items_emit_complete_samples_once_in_order(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)[:4]
        observations = []
        consumer = PeriodicConsumerSession(
            manifest, samples, "/example/spec127/provider/periodic",
            expected_provider="/example/spec127/provider",
            publication_times_us={sample.sample_id: 900_000 +
                                  sample.sample_id * 100_000
                                  for sample in samples},
            on_complete=lambda receipt, latency_ms: observations.append(
                (receipt.sample_id, latency_ms)))

        def item(sample, *, content=None, received_ms=None):
            return FakeVerifiedItem(
                f"/example/spec127/provider/periodic/sample/"
                f"{sample.sample_id}/source/0",
                (opaque_source_payload(manifest, sample, 0)
                 if content is None else content),
                received_ms=(1000 + sample.sample_id * 100
                             if received_ms is None else received_ms))

        later = consumer.accept_item(item(samples[1]))
        self.assertTrue(later.accepted)
        self.assertEqual(later.completed, ())
        first = consumer.accept_item(item(samples[0]))
        self.assertTrue(first.accepted)
        self.assertEqual([receipt.sample_id for receipt in first.completed], [0, 1])
        self.assertEqual(observations, [(0, 100.0), (1, 100.0)])

        duplicate = consumer.accept_item(item(samples[0]))
        self.assertFalse(duplicate.accepted)
        self.assertEqual(duplicate.reason, "duplicate-item")
        invalid = consumer.accept_item(item(samples[2], content=b"wrong"))
        self.assertFalse(invalid.accepted)
        self.assertEqual(invalid.reason, "content-digest-mismatch")
        wrong_name = consumer.accept_item(FakeVerifiedItem(
            "/example/spec127/provider/periodic/not-a-sample", b"opaque"))
        self.assertFalse(wrong_name.accepted)
        self.assertEqual(wrong_name.reason, "semantic-name-mismatch")

        consumer.stop()
        before = consumer.status()
        stopped = consumer.accept_item(item(samples[3]))
        self.assertFalse(stopped.accepted)
        self.assertEqual(stopped.reason, "consumer-stopped")
        after = consumer.status()
        self.assertEqual(after["completeSampleIds"], before["completeSampleIds"])
        self.assertEqual(after["partialSamples"], before["partialSamples"])
        self.assertEqual(after["postStopIgnored"], 1)


class VariableConsumerTest(unittest.TestCase):
    def test_terminal_multiloss_skip_releases_later_complete_sample(self):
        manifest = build_workload_manifest("variable-multisegment")
        sequence = generate_sample_sequence(manifest)
        failed = next(sample for sample in sequence
                      if sample.actual_source_items >= 4)
        later = sequence[failed.sample_id + 1]
        prefix = "/example/spec127/provider/variable"
        consumer = WorkloadConsumerSession(
            manifest, (failed, later), prefix,
            expected_provider="/example/spec127/provider")

        for index in range(later.actual_source_items):
            decision = consumer.accept_item(FakeVerifiedItem(
                f"{prefix}/sample/{later.sample_id}/source/{index}",
                opaque_source_payload(manifest, later, index)))
        self.assertEqual(decision.completed, ())
        for index in range(failed.actual_source_items):
            if index not in {1, 2}:
                consumer.accept_item(FakeVerifiedItem(
                    f"{prefix}/sample/{failed.sample_id}/source/{index}",
                    opaque_source_payload(manifest, failed, index)))
        consumer.accept_item(FakeVerifiedItem(
            f"{prefix}/sample/{failed.sample_id}/source/1",
            opaque_source_payload(manifest, failed, 1),
            provenance="fec-recovered"))

        released = consumer.fail_sample(failed.sample_id, "recovery-exhausted")
        self.assertEqual([receipt.sample_id for receipt in released],
                         [later.sample_id])
        consumer.stop()
        status = consumer.status()
        self.assertEqual(status["completeSampleIds"], [later.sample_id])
        self.assertEqual(status["partialSamples"], 0)
        self.assertEqual(status["skipsByReason"], {"recovery-exhausted": 1})

    def test_full_variable_sequence_completes_600_measured_samples_exactly(self):
        manifest = build_workload_manifest("variable-multisegment")
        samples = generate_sample_sequence(manifest)
        prefix = "/example/spec127/provider/variable"
        consumer = WorkloadConsumerSession(
            manifest, samples, prefix,
            expected_provider="/example/spec127/provider")
        for sample in samples:
            for index in range(sample.actual_source_items):
                decision = consumer.accept_item(FakeVerifiedItem(
                    f"{prefix}/sample/{sample.sample_id}/source/{index}",
                    opaque_source_payload(manifest, sample, index)))
                self.assertTrue(decision.accepted)
        consumer.stop()
        status = consumer.status()
        self.assertEqual(status["completeSamples"], 650)
        self.assertEqual(status["completeMeasuredSamples"], 600)
        self.assertEqual(status["completeSampleIds"], list(range(650)))
        self.assertEqual(
            {receipt["class_id"] for receipt in status["receipts"]},
            {"cap-1", "cap-2", "cap-4", "cap-8"})
        self.assertEqual(status["duplicates"], 0)
        self.assertEqual(status["partialSamples"], 0)

    def test_class_transition_waits_for_delayed_earlier_sample_boundary(self):
        manifest = build_workload_manifest("variable-multisegment")
        sequence = generate_sample_sequence(manifest)
        small = next(sample for sample in sequence if sample.class_id == "cap-1")
        large = next(sample for sample in sequence
                     if sample.class_id == "cap-8" and
                     sample.sample_id > small.sample_id)
        prefix = "/example/spec127/provider/variable"
        consumer = WorkloadConsumerSession(
            manifest, (small, large), prefix,
            expected_provider="/example/spec127/provider")

        for index in reversed(range(large.actual_source_items)):
            decision = consumer.accept_item(FakeVerifiedItem(
                f"{prefix}/sample/{large.sample_id}/source/{index}",
                opaque_source_payload(manifest, large, index)))
        self.assertEqual(decision.completed, ())
        released = consumer.accept_item(FakeVerifiedItem(
            f"{prefix}/sample/{small.sample_id}/source/0",
            opaque_source_payload(manifest, small, 0)))
        self.assertEqual([receipt.sample_id for receipt in released.completed],
                         [small.sample_id, large.sample_id])
        self.assertEqual([receipt.class_id for receipt in released.completed],
                         ["cap-1", "cap-8"])

    def test_variable_consumer_enables_generic_fec_with_default_policy(self):
        manifest = build_workload_manifest("variable-multisegment")
        sample = generate_sample_sequence(manifest)[0]
        consumer = WorkloadConsumerSession(
            manifest, (sample,), "/example/spec127/provider/variable",
            expected_provider="/example/spec127/provider")
        user = FakeUser()
        open_workload_consumer(user, object(), consumer)
        self.assertTrue(user.options["enable_fec_recovery"])
        self.assertNotIn("prefetch_policy", user.options)

    def test_one_recovered_source_completes_once_but_two_missing_fail_closed(self):
        manifest = build_workload_manifest("variable-multisegment")
        sample = next(sample for sample in generate_sample_sequence(manifest)
                      if sample.actual_source_items >= 4)
        prefix = "/example/spec127/provider/variable"

        def item(index, *, recovered=False):
            return FakeVerifiedItem(
                f"{prefix}/sample/{sample.sample_id}/source/{index}",
                opaque_source_payload(manifest, sample, index),
                provenance="fec-recovered" if recovered else "signed-data")

        recovered = WorkloadConsumerSession(
            manifest, (sample,), prefix,
            expected_provider="/example/spec127/provider")
        decisions = []
        for index in range(sample.actual_source_items):
            if index != 1:
                decisions.append(recovered.accept_item(item(index)))
        decisions.append(recovered.accept_item(item(1, recovered=True)))
        recovered.stop()
        status = recovered.status()
        self.assertEqual(status["completeSamples"], 1)
        self.assertEqual(status["recoveredItems"], 1)
        self.assertEqual(status["recoveredSamples"], 1)
        self.assertEqual(status["partialSamples"], 0)
        self.assertEqual(sum(len(value.completed) for value in decisions), 1)

        incomplete = WorkloadConsumerSession(
            manifest, (sample,), prefix,
            expected_provider="/example/spec127/provider")
        for index in range(sample.actual_source_items):
            if index not in {1, 2}:
                incomplete.accept_item(item(index))
        incomplete.accept_item(item(1, recovered=True))
        incomplete.stop()
        failed = incomplete.status()
        self.assertEqual(failed["completeSamples"], 0)
        self.assertEqual(failed["partialSamples"], 1)
        self.assertEqual(failed["recoveredItems"], 0)


class PeriodicMiniNdnCellTest(unittest.TestCase):
    def test_check_plan_is_fixed_complete_and_uses_only_generic_apps(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            cell = planned_cell(output)
            commands = build_commands(
                output,
                {"provider": "unix:///run/nfd/provider.sock",
                 "consumer": "unix:///run/nfd/consumer.sock"})

        self.assertEqual(cell["workloadId"], "periodic-sensor")
        self.assertEqual(cell["periodMs"], 100)
        self.assertEqual(cell["warmupSeconds"], 5)
        self.assertEqual(cell["measurementSeconds"], 60)
        self.assertEqual(cell["expectedMeasuredSamples"], 600)
        self.assertFalse(cell["automaticRetry"])
        self.assertFalse(cell["rerunAllowed"])
        self.assertIn("workload_provider.py", commands["provider"])
        self.assertIn("workload_consumer.py", commands["consumer"])
        self.assertIn("--workload periodic-sensor", commands["provider"])
        self.assertIn("--workload periodic-sensor", commands["consumer"])
        self.assertIn("--minimum-measured-completion-ratio 0.999",
                      commands["consumer"])
        self.assertNotIn("UAV", " ".join(commands.values()))
        self.assertNotIn("codec", " ".join(commands.values()).lower())

    def test_variable_plan_uses_same_apps_with_a_distinct_frozen_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            periodic = planned_cell(output, "periodic-sensor")
            variable = planned_cell(output, "variable-multisegment")
            commands = build_commands(
                output,
                {"provider": "unix:///run/nfd/provider.sock",
                 "consumer": "unix:///run/nfd/consumer.sock"},
                "variable-multisegment")

        self.assertEqual(variable["workloadId"], "variable-multisegment")
        self.assertNotEqual(variable["manifestDigest"], periodic["manifestDigest"])
        self.assertEqual(variable["expectedMeasuredSamples"], 600)
        self.assertIn("--workload variable-multisegment", commands["provider"])
        self.assertIn("--workload variable-multisegment", commands["consumer"])
        self.assertIn("--minimum-measured-completion-ratio 0.99",
                      commands["consumer"])
        self.assertFalse(variable["automaticRetry"])
        self.assertFalse(variable["rerunAllowed"])


class CompleteSampleTrackerTest(unittest.TestCase):
    def test_only_complete_digest_valid_samples_emit_once_in_sample_order(self):
        manifest = build_workload_manifest("variable-multisegment")
        measured = [sample for sample in generate_sample_sequence(manifest)
                    if sample.phase == "measured"]
        samples = [measured[0], measured[1],
                   next(sample for sample in measured[2:]
                        if sample.actual_source_items > 1)]
        tracker = CompleteSampleTracker(manifest, samples)

        later = samples[1]
        for index in reversed(range(later.actual_source_items)):
            emitted = tracker.accept(
                later.sample_id, index,
                opaque_source_payload(manifest, later, index),
                completed_timestamp_us=2000 + index)
        self.assertEqual(emitted, ())

        first = samples[0]
        for index in reversed(range(first.actual_source_items)):
            emitted = tracker.accept(
                first.sample_id, index,
                opaque_source_payload(manifest, first, index),
                completed_timestamp_us=3000 + index)
        self.assertEqual([receipt.sample_id for receipt in emitted],
                         [first.sample_id, later.sample_id])

        self.assertEqual(tracker.accept(
            first.sample_id, 0, opaque_source_payload(manifest, first, 0),
            completed_timestamp_us=4000), ())
        self.assertEqual(tracker.duplicates, 1)

        partial = samples[2]
        self.assertEqual(tracker.accept(
            partial.sample_id, 0, b"wrong-digest",
            completed_timestamp_us=5000), ())
        self.assertEqual(tracker.invalid_segments, 1)
        if partial.actual_source_items > 1:
            tracker.accept(
                partial.sample_id, 0,
                opaque_source_payload(manifest, partial, 0),
                completed_timestamp_us=5001)
        tracker.stop()
        self.assertEqual(tracker.partial_samples, 1)
        before = tracker.snapshot()
        self.assertEqual(tracker.accept(
            partial.sample_id, 0,
            opaque_source_payload(manifest, partial, 0),
            completed_timestamp_us=6000), ())
        self.assertEqual(tracker.snapshot(), before)
        self.assertEqual(tracker.post_stop_ignored, 1)

    def test_terminalizing_incomplete_samples_releases_later_complete_samples(self):
        manifest = build_workload_manifest("periodic-sensor")
        samples = generate_sample_sequence(manifest)[:3]
        tracker = CompleteSampleTracker(manifest, samples)

        later = samples[1]
        self.assertEqual(tracker.accept(
            later.sample_id, 0,
            opaque_source_payload(manifest, later, 0),
            completed_timestamp_us=2000), ())

        emitted = tracker.terminalize_incomplete("native-terminal-skip")

        self.assertEqual([receipt.sample_id for receipt in emitted],
                         [later.sample_id])
        self.assertEqual(tracker.skips_by_reason,
                         {"native-terminal-skip": 2})
        tracker.stop()
        self.assertEqual(tracker.partial_samples, 0)


class TrafficUtilityReportTest(unittest.TestCase):
    def test_ratios_are_exact_and_zero_denominators_are_explicitly_unavailable(self):
        unavailable = TrafficUtilityReport(
            payload_interests=0, necessary_source_repair_items=0,
            mapping_interests=3, mapping_data_responses=0,
            mapping_new_data_responses=0, mapping_bytes=0,
            retry_attempts=2, timeouts=1, nacks=4,
            provider_future_interests=0, provider_future_hits=0)
        encoded = unavailable.to_dict()
        self.assertIsNone(encoded["payloadInterestOverheadRatio"])
        self.assertEqual(encoded["payloadInterestOverheadRatioUnavailableReason"],
                         "no-necessary-source-repair-items")
        self.assertIsNone(encoded["mappingNewDataRatio"])
        self.assertEqual(encoded["mappingNewDataRatioUnavailableReason"],
                         "no-mapping-data-responses")
        self.assertIsNone(encoded["providerFutureHitRatio"])
        self.assertEqual(encoded["providerFutureHitRatioUnavailableReason"],
                         "no-provider-future-interests")
        self.assertEqual(encoded["retryAttempts"], 2)
        self.assertEqual(encoded["timeouts"], 1)
        self.assertEqual(encoded["nacks"], 4)

        measured = TrafficUtilityReport(
            payload_interests=110, necessary_source_repair_items=100,
            mapping_interests=12, mapping_data_responses=10,
            mapping_new_data_responses=8, mapping_bytes=4096,
            retry_attempts=3, timeouts=5, nacks=1,
            provider_future_interests=100, provider_future_hits=95).to_dict()
        self.assertAlmostEqual(measured["payloadInterestOverheadRatio"], 0.10)
        self.assertAlmostEqual(measured["mappingNewDataRatio"], 0.80)
        self.assertAlmostEqual(measured["providerFutureHitRatio"], 0.95)
        self.assertNotIn("mappingNewDataRatioUnavailableReason", measured)

    def test_impossible_counter_relationships_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "new Mapping"):
            TrafficUtilityReport(
                payload_interests=1, necessary_source_repair_items=1,
                mapping_interests=1, mapping_data_responses=1,
                mapping_new_data_responses=2, mapping_bytes=1,
                retry_attempts=0, timeouts=0, nacks=0,
                provider_future_interests=1, provider_future_hits=1)
        with self.assertRaisesRegex(ValueError, "future hits"):
            TrafficUtilityReport(
                payload_interests=1, necessary_source_repair_items=1,
                mapping_interests=1, mapping_data_responses=1,
                mapping_new_data_responses=1, mapping_bytes=1,
                retry_attempts=0, timeouts=0, nacks=0,
                provider_future_interests=1, provider_future_hits=2)


if __name__ == "__main__":
    unittest.main()
