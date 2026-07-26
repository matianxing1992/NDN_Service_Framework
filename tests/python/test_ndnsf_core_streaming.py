#!/usr/bin/env python3
"""Generic NDNSF streaming substrate tests."""

from __future__ import annotations

import json
import unittest
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from pathlib import Path

from ndnsf.streaming import (
    LiveStreamDefinition,
    LiveStreamDescriptor,
    LiveStreamFecOptions,
    LiveStreamItemAdmission,
    LiveStreamSamplePredictor,
    SampleClassProfile,
    STREAM_NAME_MAP_CONTRACT_VERSION_V2,
    StreamAdaptiveFetcherState,
    StreamChunk,
    StreamConsumerReorderBuffer,
    StreamCursorFrontiers,
    StreamFecInfo,
    StreamInfo,
    StreamNameMapBlock,
    StreamNameMapCheckpoint,
    StreamNameMapEntry,
    StreamNameMapResolverConfig,
    StreamNameResolver,
    StreamProducerBuffer,
    decode_stream_chunk,
    encode_stream_chunk,
    make_stream_name_map_block_name,
    make_stream_name_map_root,
)


FIXTURE_ROOT = Path(__file__).resolve().parents[1] / "fixtures" / "stream-prefetch"


def _fixture(name: str) -> dict:
    return json.loads((FIXTURE_ROOT / name).read_text(encoding="utf-8"))


def _map_block(fixture_key: str) -> StreamNameMapBlock:
    value = _fixture("map-wire-v1.json")[fixture_key]
    return StreamNameMapBlock.decode(bytes.fromhex(value["blockWireHex"]))


def _frontiers(*, oldest_retained: int = 0, latest_join: int = 1,
               latest_produced: int = 2,
               mapping_committed_through: int = 7,
               next_reserved: int = 8) -> StreamCursorFrontiers:
    return StreamCursorFrontiers(
        oldest_retained=oldest_retained,
        latest_join=latest_join,
        latest_produced=latest_produced,
        mapping_committed_through=mapping_committed_through,
        next_reserved=next_reserved,
    )


def _config(*, session_epoch: int = 17, mapping_version: int = 23,
            payload_prefix: str = "/uav/7/video/v=23") -> StreamNameMapResolverConfig:
    return StreamNameMapResolverConfig(
        stream_id="front-camera",
        session_epoch=session_epoch,
        mapping_version=mapping_version,
        block_capacity=4,
        expected_provider="/uav/7",
        mapping_root=make_stream_name_map_root("/uav/7", "front-camera"),
        payload_prefix=payload_prefix,
        max_verified_blocks=8,
        max_quarantine_blocks=4,
        max_original_name_wire_bytes=1024,
    )


def _checkpoint(block: StreamNameMapBlock,
                frontiers: StreamCursorFrontiers | None = None) -> StreamNameMapCheckpoint:
    return StreamNameMapCheckpoint(
        frontiers=_frontiers() if frontiers is None else frontiers,
        block_number=block.block_number,
        content_digest=block.content_digest(),
    )


def _admit(resolver: StreamNameResolver, block: StreamNameMapBlock, *,
           provider: str = "/uav/7", received_ms: int = 100,
           required_before_ms: int = 200, data_name: str | None = None,
           content: bytes | None = None, signed_wire_size: int | None = None,
           content_type: int = 4, has_final_block: bool = False):
    content = block.canonical_content() if content is None else bytes(content)
    exact_name = make_stream_name_map_block_name(
        make_stream_name_map_root("/uav/7", block.stream_id),
        block.mapping_version,
        block.block_number,
    )
    return resolver.admit_verified_block(
        data_name=exact_name if data_name is None else data_name,
        verified_provider=provider,
        content=content,
        signed_wire_size=(len(content) + 200 if signed_wire_size is None
                          else signed_wire_size),
        content_type=content_type,
        has_final_block=has_final_block,
        received_monotonic_ms=received_ms,
        required_before_monotonic_ms=required_before_ms,
    )


class CoreStreamingTest(unittest.TestCase):
    def test_mapping_v2_and_predictor_match_shared_golden_contract(self) -> None:
        expected = _fixture("map-wire-v2.json")["canonicalExample"]
        entries = tuple(
            StreamNameMapEntry.from_grouped_name(
                f"/uav/7/video/v=23/frame/42/{'source' if index < 3 else 'repair'}/"
                f"{index if index < 3 else 0}",
                "42", "key", index, 3, 1)
            for index in range(4)
        )
        block = StreamNameMapBlock(
            contract_version=STREAM_NAME_MAP_CONTRACT_VERSION_V2,
            stream_id="front-camera", session_epoch=17, mapping_version=23,
            block_number=0, block_capacity=4, first_cursor=0,
            previous_content_digest=None, entries=entries)
        self.assertIsNone(block.validate())
        self.assertEqual(block.wire_encode().hex(), expected["blockWireHex"])
        self.assertEqual(block.canonical_content().hex(), expected["contentWireHex"])
        self.assertEqual(block.content_digest().hex(),
                         expected["contentDigestSha256Hex"])
        decoded = StreamNameMapBlock.decode(block.wire_encode())
        self.assertEqual(decoded.entries[3].group_item_index, 3)
        self.assertEqual(decoded.entries[0].sample_class, "key")

        predictor = LiveStreamSamplePredictor((
            SampleClassProfile("key", 12, 32, 3, 1),
            SampleClassProfile("delta", 3, 8, 3, 1),
        ))
        self.assertEqual(predictor.predict("key"), 12)
        self.assertEqual(predictor.predict("delta"), 3)
        self.assertTrue(predictor.observe("key", 20))
        self.assertEqual(predictor.predict("key"), 21)
        self.assertEqual(predictor.predict("delta"), 3)
        for count in (8, 7, 6):
            self.assertTrue(predictor.observe("key", count))
        # The seed applies only before authenticated observations; after the
        # bounded history is warm, max(8, 7, 6) + margin(1) is 9.
        self.assertEqual(predictor.predict("key"), 9)
        self.assertFalse(predictor.observe("delta", 9))
        self.assertEqual(predictor.status("key").observations, 3)

    def test_live_stream_public_values_are_simple_opaque_and_default_fec_off(self) -> None:
        definition = LiveStreamDefinition(
            stream_id="front-camera",
            provider="/memphis/uav/7",
            semantic_data_prefix="/memphis/uav/7/video/front/session-9",
            session_epoch=9,
            mapping_version=23,
        )
        native = definition._to_native()
        self.assertEqual(native.stream_id, "front-camera")
        self.assertEqual(native.provider, "/memphis/uav/7")
        self.assertFalse(native.fec.enabled)
        self.assertIsNone(native.validate())

        enabled = LiveStreamFecOptions.xor_one_repair(3, 4096, 400)
        self.assertEqual(enabled.scheme, "xor-one-repair")
        self.assertTrue(enabled._to_native().enabled)
        stronger = LiveStreamFecOptions.gf256_two_repair(8, 4096, 400)
        self.assertEqual(stronger.scheme, "gf256-two-repair")
        self.assertEqual(stronger.repair_symbols, 2)
        self.assertEqual(stronger._to_native().recovery_capacity, 2)
        self.assertTrue(LiveStreamItemAdmission.accept_item()._native.accepted)
        self.assertFalse(
            LiveStreamItemAdmission.reject_item("aead-invalid")._native.accepted)

        public_names = set(definition.__dataclass_fields__)
        self.assertFalse(public_names & {"key", "cipher", "nonce", "plaintext"})

        native_module = __import__("ndnsf._ndnsf", fromlist=["NativeLiveStreamStatus"])
        status_type = native_module.NativeLiveStreamStatus
        self.assertTrue(hasattr(status_type, "mapping_data_responses"))
        self.assertTrue(hasattr(status_type, "mapping_new_data_responses"))
        for field in (
                "initial_payload_interests", "retry_payload_interests",
                "initial_future_payload_interests", "retry_future_payload_interests",
                "retry_successes", "retry_suppressions",
                "retry_suppression_reasons", "declared_recovery_capacity",
                "recovery_eligible_sources",
                "terminal_missing_sources", "recoverable_groups",
                "recovered_groups",
                "recovery_attempts", "recovery_exhaustions",
                "provider_initial_future_interests", "provider_initial_future_hits",
                "provider_retry_future_interests", "provider_retry_future_hits"):
            self.assertTrue(hasattr(status_type, field), field)

        # Descriptor JSON is application control-plane material, not nested Data.
        descriptor_native = native_module
        raw = descriptor_native.NativeLiveStreamDescriptor()
        native.fec = stronger._to_native()
        raw.definition = native
        raw.checkpoint = StreamNameMapCheckpoint(
            frontiers=StreamCursorFrontiers(0, 0, 0, 15, 16),
            block_number=0,
            content_digest=bytes(range(32)),
        )._to_native()
        raw.measured_sample_period_ms = 33.0
        raw.safe_join_cursor = 0
        descriptor = LiveStreamDescriptor(raw)
        restored = LiveStreamDescriptor.from_dict(descriptor.to_dict())
        self.assertEqual(restored.stream_id, descriptor.stream_id)
        self.assertEqual(restored.safe_join_cursor, 0)
        self.assertEqual(restored._native.definition.fec.recovery_capacity, 2)
        self.assertEqual(descriptor.to_dict()["definition"]["fec"]["scheme"],
                         "gf256-two-repair")

    def test_stream_info_round_trip_and_chunk_names(self) -> None:
        info = StreamInfo(
            stream_id="stream-1",
            session_epoch=7,
            stream_prefix="/example/drone/video/stream-1",
            next_seq=3,
            content_type="video/h264",
            window=48,
            metadata={"fps": 15, "app": "uav"},
        )

        parsed = StreamInfo.from_dict(info.to_dict())

        self.assertEqual(parsed.stream_id, "stream-1")
        self.assertEqual(parsed.session_epoch, 7)
        self.assertEqual(parsed.content_type, "video/h264")
        self.assertEqual(parsed.metadata["fps"], 15)
        self.assertFalse(hasattr(parsed, "chunk_name"))

    def test_stream_chunk_wire_round_trip_keeps_payload_and_fec(self) -> None:
        chunk = StreamChunk(
            stream_id="stream-1",
            session_epoch=1,
            seq=42,
            payload=b"\x00\x01h264-bytes",
            content_type="video/h264",
            capture_ms=1000,
            frame_id=10,
            frame_first_seq=40,
            frame_last_seq=43,
            segment_index=2,
            segment_count=4,
            key_chunk=True,
            fec=StreamFecInfo(
                scheme="xor-parity",
                data_shards=3,
                parity_shards=1,
                symbol_index=2,
                symbol_count=4,
                data_lengths=(10, 11, 12),
                source_block_id="frame-10",
            ),
            metadata={"roi": "foreground"},
        )

        parsed = decode_stream_chunk(encode_stream_chunk(chunk))

        self.assertEqual(parsed.stream_id, chunk.stream_id)
        self.assertEqual(parsed.session_epoch, chunk.session_epoch)
        self.assertEqual(parsed.seq, chunk.seq)
        self.assertEqual(parsed.payload, chunk.payload)
        self.assertEqual(parsed.frame_id, 10)
        self.assertTrue(parsed.key_chunk)
        self.assertIsNotNone(parsed.fec)
        self.assertEqual(parsed.fec.data_lengths, (10, 11, 12))
        self.assertEqual(parsed.metadata["roi"], "foreground")

    def test_producer_buffer_evicts_old_chunks_and_encodes_lookup(self) -> None:
        buffer = StreamProducerBuffer(max_chunks=2)
        for seq in range(3):
            buffer.put(StreamChunk("s", 1, seq, f"payload-{seq}".encode()))

        self.assertEqual(buffer.seqs(), [1, 2])
        self.assertIsNone(buffer.get(0))
        self.assertEqual(buffer.get(1).payload, b"payload-1")
        self.assertEqual(decode_stream_chunk(buffer.encoded(2)).payload, b"payload-2")
        self.assertEqual(buffer.metrics.produced, 3)
        self.assertEqual(buffer.metrics.evicted, 1)

    def test_consumer_reorder_buffer_emits_in_order_and_rejects_bad_chunks(self) -> None:
        buffer = StreamConsumerReorderBuffer("s", 3, next_seq=0)

        self.assertEqual(buffer.push(StreamChunk("s", 3, 1, b"one")), [])
        self.assertEqual(buffer.missing_sequences(), [0])
        emitted = buffer.push(StreamChunk("s", 3, 0, b"zero"))
        self.assertEqual([chunk.payload for chunk in emitted], [b"zero", b"one"])

        self.assertEqual(buffer.push(StreamChunk("s", 3, 1, b"dup")), [])
        self.assertEqual(buffer.push(StreamChunk("old", 2, 2, b"old")), [])
        self.assertEqual(buffer.metrics.duplicates, 1)
        self.assertEqual(buffer.metrics.stale, 1)
        self.assertEqual(buffer.metrics.emitted, 2)

    def test_consumer_skip_to_unblocks_later_chunks(self) -> None:
        buffer = StreamConsumerReorderBuffer("s", 1, next_seq=0)
        self.assertEqual(buffer.push(StreamChunk("s", 1, 2, b"two")), [])
        buffer.skip_to(2)
        self.assertEqual([chunk.payload for chunk in buffer.push(StreamChunk("s", 1, 3, b"three"))],
                         [b"two", b"three"])

    def test_consumer_reports_pending_bytes_and_overflow(self) -> None:
        buffer = StreamConsumerReorderBuffer("s", 1, next_seq=0, max_pending=2)
        buffer.push(StreamChunk("s", 1, 2, b"22"))
        buffer.push(StreamChunk("s", 1, 3, b"333"))
        self.assertEqual((buffer.pending_count, buffer.pending_bytes), (2, 5))

        buffer.push(StreamChunk("s", 1, 4, b"4"))
        self.assertEqual((buffer.pending_count, buffer.pending_bytes), (2, 4))
        self.assertEqual(buffer.metrics.overflows, 1)
        self.assertEqual(buffer.metrics.max_pending, 2)

        buffer.reset("new-session", 2, next_seq=10)
        self.assertEqual(buffer.pending_count, 0)
        self.assertEqual(buffer.metrics.received, 0)
        self.assertEqual(buffer.metrics.overflows, 0)
        self.assertEqual(buffer.metrics.max_pending, 0)

    def test_native_producer_buffer_is_thread_safe(self) -> None:
        buffer = StreamProducerBuffer(max_chunks=128)
        with ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(
                lambda seq: buffer.put(StreamChunk("s", 1, seq, b"x")),
                range(100),
            ))
        self.assertEqual(len(buffer), 100)
        self.assertEqual(buffer.metrics.produced, 100)

    def test_adaptive_fetcher_decision_reacts_to_pressure(self) -> None:
        state = StreamAdaptiveFetcherState(rtt_ms=100, base_window=32, base_lookahead=8)
        stable = state.decide()

        state.record_timeout()
        state.record_timeout()
        state.record_nack()
        state.set_backlog_pressure(0.8)
        congested = state.decide()

        self.assertEqual(stable.reason, "stable")
        self.assertEqual(stable.window, 32)
        self.assertEqual(stable.lookahead, 8)
        self.assertEqual(congested.reason, "congested")
        self.assertLess(congested.window, stable.window)
        self.assertLess(congested.lookahead, stable.lookahead)
        self.assertGreater(congested.interest_lifetime_ms, stable.interest_lifetime_ms)

    def test_live_prefetch_matches_frame_period_and_rejects_stale_session(self) -> None:
        state = StreamAdaptiveFetcherState(
            rtt_ms=100,
            base_window=8,
            live_edge_window=2,
            live_edge_stable_required=2,
            detection_period_ms=0,
        )
        state.reset_live(7, 40, 33.0)
        self.assertFalse(state.observe_accepted_sample(6, 1, 90, 10))
        self.assertTrue(state.observe_accepted_sample(
            7, 1, 100, 100, segment_count=3, known_produced=False))
        for sample_id, arrival_ms in ((3, 166), (4, 199), (5, 232),
                                      (6, 265), (7, 298)):
            self.assertTrue(state.observe_accepted_sample(
                7, sample_id, arrival_ms, 100,
                segment_count=3, known_produced=False))

        decision = state.decide()
        self.assertEqual(decision.policy_mode, "live-v1")
        self.assertIn(decision.phase, {"ADJUSTING", "FETCHING"})
        self.assertEqual(decision.sample_demand, 4)
        self.assertGreaterEqual(decision.packet_demand, 10)
        self.assertEqual(state.invalid_observations, 1)
        state.stop_live()
        self.assertEqual(state.decide().phase, "STOPPED")

    def test_mapped_live_controller_matches_shared_trace(self) -> None:
        config = _fixture("controller-traces-v1.json")["mappedLive"]
        state = StreamAdaptiveFetcherState(rtt_ms=config["retrievalDelayMs"])
        state.configure_mapped_live(
            aggregate_limit=config["aggregateInFlightLimit"],
            mapping_reserve=config["mappingReserve"],
            retransmission_reserve=config["retransmissionReserve"],
            block_capacity=config["mappingBlockCapacity"],
            detector_profile=config["detectorProfile"],
        )
        state.reset_mapped_live(
            config["sessionEpoch"], config["nextCursor"],
            config["samplePeriodMs"], config["latestProducedCursor"],
            config["mappingCommittedThroughCursor"],
            config["nextReservedCursor"],
        )
        state.live_edge_window = 2
        state.live_edge_stable_required = 2
        state.detection_period_ms = 0
        for sample_id, arrival_ms in zip(config["sampleIds"],
                                         config["arrivalMs"]):
            self.assertTrue(state.observe_accepted_sample(
                config["sessionEpoch"], sample_id, arrival_ms,
                config["retrievalDelayMs"],
                segment_count=config["segmentsPerSample"],
                known_produced=True,
            ))

        decision = state.decide(now_ms=340)
        self.assertEqual(decision.policy_mode,
                         "mapped-live-v1-future-on")
        self.assertEqual(decision.detector_profile,
                         config["detectorProfile"])
        self.assertIn(decision.phase, {"ADJUSTING", "FETCHING"})
        self.assertTrue(decision.mapping_ready)
        self.assertEqual(decision.mapping_begin_block, 25)
        self.assertLessEqual(
            decision.mapping_budget + decision.payload_budget +
            decision.retransmission_budget,
            config["aggregateInFlightLimit"],
        )

        pressure = state.timeout_pressure
        state.record_timeout_evidence(110, known_produced=False,
                                      was_future=True)
        self.assertEqual(state.timeout_pressure, pressure)
        self.assertTrue(state.decide(now_ms=350).future_wait)
        state.record_timeout_evidence(101, known_produced=True,
                                      was_future=False)
        self.assertGreater(state.timeout_pressure, pressure)
        state.record_nack_reason(101, "congestion")
        state.record_congestion_mark(101, 1)
        self.assertTrue(state.decide(now_ms=351).congestion_hold)

        state.observe_sample_extent(5, 3)
        state.observe_sample_extent(5, 7)
        decision = state.decide(now_ms=352)
        self.assertEqual(decision.terminal_unproduced_advice, 2)
        self.assertEqual(decision.later_cursor_advice, 2)

        state.begin_recovery(400, 460)
        decision = state.decide(now_ms=420, playout_deadline_ms=460)
        self.assertEqual(decision.phase, "RECOVERING")
        self.assertTrue(decision.retransmission_eligible)
        self.assertEqual(decision.remaining_recovery_budget_ms, 40)
        self.assertFalse(state.decide(
            now_ms=470, playout_deadline_ms=460).retransmission_eligible)
        state.record_recovery(True)
        self.assertEqual(state.phase, "FETCHING")
        state.set_mapped_live_policy_enabled(False)
        self.assertEqual(state.decide(now_ms=480).policy_mode,
                         "mapped-pressure")
        state.set_mapped_live_policy_enabled(True)
        self.assertEqual(state.decide(now_ms=481).policy_mode,
                         "mapped-live-v1-future-on")

    def test_paper_literal_profile_is_not_ndnsf_stability(self) -> None:
        fixture = _fixture("controller-traces-v1.json")["paperLiteral"]
        state = StreamAdaptiveFetcherState()
        state.configure_mapped_live(
            aggregate_limit=12, mapping_reserve=2,
            retransmission_reserve=1, block_capacity=4,
            detector_profile=fixture["detectorProfile"],
        )
        state.reset_mapped_live(42, 0, fixture["samplePeriodMs"], 3, 7, 8)
        state.live_edge_window = 2
        state.live_edge_stable_required = 2
        state.detection_period_ms = 0
        for sample_id, arrival_ms in zip(fixture["sampleIds"],
                                         fixture["arrivalMs"]):
            self.assertTrue(state.observe_accepted_sample(
                42, sample_id, arrival_ms, 40.0,
                segment_count=1, known_produced=True,
            ))
        self.assertEqual(state.phase, fixture["expectedPhase"])
        self.assertEqual(state.decide(now_ms=340).detector_profile,
                         fixture["detectorProfile"])

    def test_stream_name_map_python_uses_shared_canonical_wire(self) -> None:
        fixture = _fixture("map-wire-v1.json")
        expected = fixture["canonicalExample"]
        block = StreamNameMapBlock.decode(bytes.fromhex(expected["blockWireHex"]))

        root = make_stream_name_map_root("/uav/7", "front-camera")
        exact_name = make_stream_name_map_block_name(root, 23, 0)
        self.assertEqual(root, fixture["typedName"]["mappingRootUri"])
        self.assertEqual(exact_name, fixture["typedName"]["dataNameUri"])
        self.assertEqual(block.wire_encode().hex(), expected["blockWireHex"])
        self.assertEqual(block.canonical_content().hex(), expected["contentWireHex"])
        self.assertEqual(block.content_digest().hex(),
                         expected["contentDigestSha256Hex"])
        self.assertEqual(block.last_cursor, 3)
        self.assertEqual(len(block.entries), 4)
        self.assertTrue(block.entries[2].tombstone)
        self.assertEqual(block.entries[1].original_name,
                         "/uav/7/video/v=23/seq=1")
        self.assertTrue(block.fits_signed_wire_budget(
            200, len(block.canonical_content()) + 200))
        self.assertFalse(block.fits_signed_wire_budget(
            200, len(block.canonical_content()) + 199))

        frontiers = _fixture("frontier-retention-v1.json")["valid"]
        value = StreamCursorFrontiers(
            oldest_retained=frontiers["oldestRetained"],
            latest_join=frontiers["latestJoin"],
            latest_produced=frontiers["latestProduced"],
            mapping_committed_through=frontiers["mappingCommittedThrough"],
            next_reserved=frontiers["nextReserved"],
        )
        self.assertIsNone(value.validate(frontiers["blockCapacity"],
                                         frontiers["checkpointBlock"]))

    def test_stream_name_resolver_matches_shared_golden_trace(self) -> None:
        fixture = _fixture("resolver-traces-v1.json")
        operations = fixture["operations"]
        block0 = _map_block("resolverGenesis")
        block1 = _map_block("resolverSuccessor")
        resolver = StreamNameResolver(_config(), _checkpoint(block0))

        def check_admission(index: int, result) -> None:
            expected = operations[index]
            self.assertEqual(result.disposition,
                             expected["expectedDisposition"])
            self.assertEqual(result.reason, expected["expectedReason"])
            self.assertEqual(result.state_changed, expected["stateChanged"])
            if "faulted" in expected:
                self.assertEqual(resolver.faulted, expected["faulted"])
            if "verifiedBlocks" in expected:
                self.assertEqual(resolver.verified_block_count,
                                 expected["verifiedBlocks"])
            if "quarantinedBlocks" in expected:
                self.assertEqual(resolver.quarantined_block_count,
                                 expected["quarantinedBlocks"])
            if "bindingCount" in expected:
                self.assertEqual(resolver.binding_count,
                                 expected["bindingCount"])
            if "resolvable" in expected:
                actual = [cursor for cursor in range(8)
                          if resolver.resolve(cursor) is not None]
                self.assertEqual(actual, expected["resolvable"])

        check_admission(0, _admit(resolver, block1))
        check_admission(1, _admit(resolver, block0))
        check_admission(2, _admit(resolver, block0))

        terminal = operations[3]
        self.assertEqual(resolver.mark_terminal_unproduced(terminal["cursor"]),
                         terminal["changed"])
        terminal_value = resolver.lookup(terminal["cursor"])
        self.assertIsNotNone(terminal_value)
        self.assertEqual(terminal_value.schedulable, terminal["schedulable"])
        self.assertEqual(
            resolver.reverse_resolve(
                f"/uav/7/video/v=23/packet/seq={terminal['cursor']}") is not None,
            terminal["reverseBindingPreserved"],
        )

        eviction = operations[4]
        self.assertEqual(resolver.evict_local_block(eviction["block"]),
                         eviction["changed"])
        self.assertEqual(resolver.frontiers.oldest_retained,
                         eviction["providerOldestRetained"])
        self.assertEqual(resolver.verified_block_count,
                         eviction["verifiedBlocks"])
        self.assertEqual(resolver.quarantined_block_count,
                         eviction["quarantinedBlocks"])
        self.assertEqual(resolver.binding_count, eviction["bindingCount"])

        check_admission(5, _admit(resolver, block0))
        refetch = operations[5]
        terminal_value = resolver.lookup(refetch["terminalCursor"])
        self.assertIsNotNone(terminal_value)
        self.assertEqual(terminal_value.schedulable,
                         refetch["terminalSchedulable"])

        refresh = operations[6]
        check_admission(6, resolver.refresh_checkpoint(StreamNameMapCheckpoint(
            frontiers=_frontiers(oldest_retained=refresh["oldestRetained"],
                                 latest_join=4, latest_produced=5),
            block_number=refresh["checkpointBlock"],
            content_digest=block1.content_digest(),
        )))

        reset = operations[7]
        version = reset["mappingVersion"]
        entries = tuple(
            StreamNameMapEntry.make_tombstone() if entry.tombstone else
            StreamNameMapEntry.from_name(
                entry.original_name.replace("v=23", f"v={version}"))
            for entry in block0.entries
        )
        new_block = replace(block0, session_epoch=reset["sessionEpoch"],
                            mapping_version=version, entries=entries)
        resolver.reset(_config(session_epoch=reset["sessionEpoch"],
                               mapping_version=version,
                               payload_prefix=f"/uav/7/video/v={version}"),
                       _checkpoint(new_block))
        stale = _admit(resolver, block0)
        self.assertEqual(stale.disposition,
                         reset["oldSessionBlockExpectedDisposition"])
        self.assertEqual(stale.reason,
                         reset["oldSessionBlockExpectedReason"])

        for timing in fixture["timingCases"]:
            timing_resolver = StreamNameResolver(_config(), _checkpoint(block0))
            result = _admit(
                timing_resolver,
                block0,
                received_ms=timing["receivedMonotonicMs"],
                required_before_ms=timing["requiredBeforeMonotonicMs"],
            )
            self.assertEqual(result.timing, timing["expected"])

    def test_stream_name_resolver_connects_quarantine_and_preserves_bindings(self) -> None:
        block0 = _map_block("resolverGenesis")
        block1 = _map_block("resolverSuccessor")
        resolver = StreamNameResolver(_config(), _checkpoint(block0))

        future = _admit(resolver, block1)
        self.assertEqual(future.disposition, "QUARANTINED")
        self.assertEqual(future.timing, "AHEAD")
        self.assertIsNone(resolver.resolve(4))
        self.assertEqual(resolver.quarantined_block_count, 1)

        anchor = _admit(resolver, block0)
        self.assertEqual(anchor.disposition, "ADMITTED")
        self.assertEqual(resolver.resolve(4), "/uav/7/video/v=23/packet/seq=4")
        self.assertEqual(resolver.reverse_resolve(
            "/uav/7/video/v=23/packet/seq=4"), 4)
        self.assertEqual((resolver.verified_block_count,
                          resolver.quarantined_block_count), (2, 0))

        duplicate = _admit(resolver, block0)
        self.assertEqual(duplicate.disposition, "DUPLICATE")
        self.assertFalse(duplicate.state_changed)
        tombstone = resolver.lookup(2)
        self.assertIsNotNone(tombstone)
        self.assertTrue(tombstone.tombstone)
        self.assertFalse(tombstone.schedulable)
        self.assertIsNone(resolver.resolve(2))

        self.assertTrue(resolver.mark_terminal_unproduced(1))
        terminal = resolver.lookup(1)
        self.assertIsNotNone(terminal)
        self.assertTrue(terminal.terminal_unproduced)
        self.assertFalse(terminal.schedulable)
        self.assertIsNone(resolver.resolve(1))
        self.assertEqual(resolver.reverse_resolve(
            "/uav/7/video/v=23/packet/seq=1"), 1)

        provider_frontier = resolver.frontiers.oldest_retained
        self.assertTrue(resolver.evict_local_block(0))
        self.assertIsNone(resolver.resolve(0))
        self.assertEqual(resolver.frontiers.oldest_retained, provider_frontier)
        self.assertTrue(_admit(resolver, block0).accepted)
        self.assertIsNotNone(resolver.resolve(0))

        refreshed = StreamNameMapCheckpoint(
            frontiers=_frontiers(oldest_retained=4, latest_join=4,
                                 latest_produced=5),
            block_number=1,
            content_digest=block1.content_digest(),
        )
        refresh_result = resolver.refresh_checkpoint(refreshed)
        self.assertEqual(refresh_result.disposition, "ADMITTED")
        self.assertEqual(resolver.frontiers.oldest_retained, 4)
        self.assertIsNone(resolver.lookup(0))
        self.assertIsNotNone(resolver.resolve(4))

    def test_stream_name_resolver_closes_on_provider_fork_and_stale_reset(self) -> None:
        block0 = _map_block("resolverGenesis")
        block1 = _map_block("resolverSuccessor")

        wrong_provider = StreamNameResolver(_config(), _checkpoint(block0))
        rejected = _admit(wrong_provider, block0, provider="/uav/8")
        self.assertEqual((rejected.disposition, rejected.reason),
                         ("FATAL_SESSION", "wrong-provider"))
        self.assertTrue(wrong_provider.faulted)

        forked = StreamNameResolver(_config(), _checkpoint(block0))
        self.assertTrue(_admit(forked, block0).accepted)
        bad_successor = replace(block1, previous_content_digest=bytes(32))
        result = _admit(forked, bad_successor)
        self.assertEqual((result.disposition, result.reason),
                         ("FATAL_SESSION", "continuity-fork"))
        self.assertTrue(forked.faulted)
        self.assertIsNone(forked.resolve(0))

        reset = StreamNameResolver(_config(), _checkpoint(block0))
        new_entries = tuple(
            StreamNameMapEntry.make_tombstone() if entry.tombstone else
            StreamNameMapEntry.from_name(
                entry.original_name.replace("v=23", "v=24"))
            for entry in block0.entries
        )
        new_block = replace(
            block0,
            session_epoch=18,
            mapping_version=24,
            entries=new_entries,
        )
        reset.reset(_config(session_epoch=18, mapping_version=24,
                            payload_prefix="/uav/7/video/v=24"),
                    _checkpoint(new_block))
        stale = _admit(reset, block0)
        self.assertEqual((stale.disposition, stale.reason),
                         ("FATAL_SESSION", "stale-session"))
        self.assertEqual(reset.binding_count, 0)

    def test_stream_name_resolver_matches_shared_rejection_vectors(self) -> None:
        fixture = _fixture("map-rejections-v1.json")
        block0 = _map_block("resolverGenesis")
        block1 = _map_block("resolverSuccessor")
        observed: dict[str, tuple[str, bool]] = {}

        def fresh() -> StreamNameResolver:
            return StreamNameResolver(_config(), _checkpoint(block0))

        resolver = fresh()
        exact = make_stream_name_map_block_name(
            make_stream_name_map_root("/uav/7", block0.stream_id), 23, 0)
        result = _admit(resolver, block0, data_name=f"{exact}/wrong")
        observed["wrong-control-name"] = (result.reason, result.fatal)

        result = _admit(fresh(), block0, provider="/uav/8")
        observed["wrong-provider"] = (result.reason, result.fatal)

        result = _admit(fresh(), replace(block0, session_epoch=16))
        observed["stale-session"] = (result.reason, result.fatal)

        result = _admit(fresh(), replace(block0, mapping_version=22))
        observed["stale-mapping-version"] = (result.reason, result.fatal)

        resolver = fresh()
        self.assertTrue(_admit(resolver, block0).accepted)
        changed_entries = list(block0.entries)
        changed_entries[3] = StreamNameMapEntry.from_name(
            "/uav/7/video/v=23/packet/seq=99")
        result = _admit(resolver, replace(block0, entries=tuple(changed_entries)))
        observed["same-name-different-content"] = (result.reason, result.fatal)

        resolver = fresh()
        self.assertTrue(_admit(resolver, block0).accepted)
        result = _admit(resolver, replace(block1,
                                          previous_content_digest=bytes(32)))
        observed["continuity-fork"] = (result.reason, result.fatal)

        resolver = fresh()
        self.assertTrue(_admit(resolver, block0).accepted)
        reused_entries = list(block1.entries)
        reused_entries[0] = block0.entries[0]
        result = _admit(resolver, replace(block1, entries=tuple(reused_entries)))
        observed["original-name-reuse"] = (result.reason, result.fatal)

        result = _admit(fresh(), block0, content_type=0)
        observed["wrong-content-type"] = (result.reason, result.fatal)

        result = _admit(fresh(), block0, has_final_block=True)
        observed["final-block"] = (result.reason, result.fatal)

        result = _admit(fresh(), block0, signed_wire_size=8801)
        observed["wire-cap"] = (result.reason, result.fatal)

        result = _admit(fresh(), block0, content=b"\x15")
        observed["noncanonical"] = (result.reason, result.fatal)

        outside_entries = list(block0.entries)
        outside_entries[0] = StreamNameMapEntry.from_name(
            "/attacker/not-authorized/seq=0")
        result = _admit(fresh(), replace(block0, entries=tuple(outside_entries)))
        observed["outside-prefix"] = (result.reason, result.fatal)

        short_names = replace(_config(), max_original_name_wire_bytes=16)
        result = _admit(StreamNameResolver(short_names, _checkpoint(block0)),
                        block0)
        observed["name-too-large"] = (result.reason, result.fatal)

        stale_resolver = StreamNameResolver(
            _config(),
            StreamNameMapCheckpoint(
                frontiers=_frontiers(oldest_retained=4, latest_join=4,
                                     latest_produced=5),
                block_number=1,
                content_digest=block1.content_digest(),
            ),
        )
        result = _admit(stale_resolver, block0)
        observed["stale-block"] = (result.reason, result.fatal)

        block2 = StreamNameMapBlock(
            stream_id="front-camera", session_epoch=17, mapping_version=23,
            block_number=2, block_capacity=4, first_cursor=8,
            previous_content_digest=block1.content_digest(),
            entries=tuple(StreamNameMapEntry.from_name(
                f"/uav/7/video/v=23/packet/seq={8 + index}")
                for index in range(4)),
        )
        result = _admit(fresh(), block2)
        self.assertEqual(result.disposition, "QUARANTINED")
        self.assertEqual(result.reason, "awaiting-continuity")

        regressed = StreamNameMapCheckpoint(
            frontiers=_frontiers(latest_produced=1),
            block_number=0,
            content_digest=block0.content_digest(),
        )
        result = fresh().refresh_checkpoint(regressed)
        observed["frontier-regression"] = (result.reason, result.fatal)

        advanced = StreamNameMapCheckpoint(
            frontiers=_frontiers(latest_join=4, latest_produced=5),
            block_number=1,
            content_digest=block1.content_digest(),
        )
        result = fresh().refresh_checkpoint(advanced)
        observed["checkpoint-anchor-not-verified"] = (result.reason,
                                                        result.fatal)

        conflicting = StreamNameMapCheckpoint(
            frontiers=_frontiers(), block_number=0, content_digest=bytes(32))
        result = fresh().refresh_checkpoint(conflicting)
        observed["checkpoint-equivocation"] = (result.reason, result.fatal)

        for vector in fixture["rejections"]:
            self.assertIn(vector["case"], observed)
            self.assertEqual(observed[vector["case"]],
                             (vector["reason"], vector["fatal"]))

    def test_stream_name_resolver_rejects_namespace_reuse_and_impossible_bounds(self) -> None:
        block0 = _map_block("resolverGenesis")
        block1 = _map_block("resolverSuccessor")
        resolver = StreamNameResolver(_config(), _checkpoint(block0))

        with self.assertRaisesRegex(ValueError, "reused-session-epoch"):
            resolver.reset(_config(), _checkpoint(block0))

        with self.assertRaisesRegex(ValueError, "reused-session-namespace"):
            resolver.reset(_config(session_epoch=18), _checkpoint(block0))

        bad_version = _config(payload_prefix="/uav/7/video/v=22")
        with self.assertRaisesRegex(ValueError, "invalid-versioned-payload-prefix"):
            StreamNameResolver(bad_version, _checkpoint(block0))

        too_small = replace(_config(), max_verified_blocks=1)
        with self.assertRaisesRegex(ValueError,
                                    "verified-cache-too-small-for-frontier"):
            StreamNameResolver(too_small, _checkpoint(block0))

        too_few_names = replace(_config(), max_reverse_entries=7)
        with self.assertRaisesRegex(ValueError,
                                    "reverse-cache-too-small-for-frontier"):
            StreamNameResolver(too_few_names, _checkpoint(block0))

        conflict = resolver.refresh_checkpoint(StreamNameMapCheckpoint(
            frontiers=_frontiers(),
            block_number=0,
            content_digest=bytes(32),
        ))
        self.assertEqual((conflict.reason, conflict.fatal),
                         ("checkpoint-equivocation", True))

        resolver = StreamNameResolver(_config(), _checkpoint(block0))
        advanced = StreamNameMapCheckpoint(
            frontiers=_frontiers(latest_join=4, latest_produced=5),
            block_number=1,
            content_digest=block1.content_digest(),
        )
        unverified = resolver.refresh_checkpoint(advanced)
        self.assertEqual((unverified.reason, unverified.fatal),
                         ("checkpoint-anchor-not-verified", False))
        self.assertTrue(_admit(resolver, block0).accepted)
        self.assertTrue(_admit(resolver, block1).accepted)
        self.assertTrue(resolver.refresh_checkpoint(advanced).accepted)

        extended = StreamNameMapCheckpoint(
            frontiers=_frontiers(oldest_retained=4, latest_join=4,
                                 latest_produced=5,
                                 mapping_committed_through=11,
                                 next_reserved=12),
            block_number=1,
            content_digest=block1.content_digest(),
        )
        self.assertTrue(resolver.refresh_checkpoint(extended).accepted)
        block2_entries = tuple(
            block0.entries[0] if index == 0 else
            StreamNameMapEntry.from_name(
                f"/uav/7/video/v=23/packet/seq={8 + index}")
            for index in range(4)
        )
        block2 = StreamNameMapBlock(
            stream_id="front-camera", session_epoch=17, mapping_version=23,
            block_number=2, block_capacity=4, first_cursor=8,
            previous_content_digest=block1.content_digest(),
            entries=block2_entries,
        )
        reuse = _admit(resolver, block2)
        self.assertEqual((reuse.reason, reuse.fatal),
                         ("original-name-reuse", True))

        bounded = replace(_config(), max_quarantine_blocks=1)
        resolver = StreamNameResolver(
            bounded,
            StreamNameMapCheckpoint(
                frontiers=_frontiers(mapping_committed_through=11,
                                     next_reserved=12),
                block_number=0,
                content_digest=block0.content_digest(),
            ),
        )
        self.assertTrue(_admit(resolver, block0).accepted)
        self.assertTrue(_admit(resolver, block1).accepted)
        normal_block2 = replace(block2, entries=tuple(
            StreamNameMapEntry.from_name(
                f"/uav/7/video/v=23/packet/seq={8 + index}")
            for index in range(4)
        ))
        self.assertTrue(_admit(resolver, normal_block2).accepted)
        self.assertFalse(resolver.evict_local_block(0))
        self.assertEqual(resolver.verified_block_count, 3)
        self.assertIsNotNone(resolver.resolve(0))

        self.assertIsNone(block0.validate())
        malformed = replace(block0, first_cursor=1)
        self.assertEqual(malformed.validate(), "invalid-first-cursor")

        import ndnsf
        self.assertIs(ndnsf.StreamNameMapBlock, StreamNameMapBlock)
        self.assertIs(ndnsf.StreamNameResolver, StreamNameResolver)


if __name__ == "__main__":
    unittest.main()
