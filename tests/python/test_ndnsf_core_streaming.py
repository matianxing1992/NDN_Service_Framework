#!/usr/bin/env python3
"""Generic NDNSF streaming substrate tests."""

from __future__ import annotations

import unittest
from concurrent.futures import ThreadPoolExecutor

from ndnsf.streaming import (
    StreamAdaptiveFetcherState,
    StreamChunk,
    StreamConsumerReorderBuffer,
    StreamFecInfo,
    StreamInfo,
    StreamProducerBuffer,
    decode_stream_chunk,
    encode_stream_chunk,
)


class CoreStreamingTest(unittest.TestCase):
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
        self.assertEqual(parsed.chunk_name(9), "/example/drone/video/stream-1/9")

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


if __name__ == "__main__":
    unittest.main()
