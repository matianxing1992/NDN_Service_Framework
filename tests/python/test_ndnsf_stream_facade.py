#!/usr/bin/env python3

import unittest
from types import SimpleNamespace

from ndnsf.streaming import (
    LiveStreamDescriptor,
    LiveStreamDefinition,
    LiveStreamFecOptions,
    LiveStreamConsumerHandle,
    PredictiveStreamDescriptor,
    PredictiveStreamSubscriber,
    SampleClassProfile,
    StreamPublisher,
    StreamAdvancedOptions,
    StreamConfig,
    StreamSubscriptionOptions,
    STREAM_NAME_MAP_CONTRACT_VERSION_V2,
)
from ndnsf.service import ServiceProvider, ServiceUser
from ndnsf._ndnsf import (
    NativeLiveStreamStatus,
    NativePredictiveStreamCheckpoint,
    NativePredictiveStreamDescriptor,
)


class StreamFacadePythonContractTest(unittest.TestCase):
    def test_predictive_descriptor_round_trip_preserves_authenticated_frontier(self):
        definition = LiveStreamDefinition(
            stream_id="round-trip",
            provider="/provider",
            semantic_data_prefix="/provider/stream/round-trip",
            session_epoch=3,
            mapping_version=3,
            contract_version=STREAM_NAME_MAP_CONTRACT_VERSION_V2,
            sample_period_ms=20.0,
            sample_classes=(SampleClassProfile("default", 1, 4),),
            fec=LiveStreamFecOptions.none(),
        )
        checkpoint = NativePredictiveStreamCheckpoint()
        checkpoint.latest_produced_sample_id = 2
        checkpoint.next_expected_sample_id = 3
        native = NativePredictiveStreamDescriptor(
            definition._to_native(),
            checkpoint,
            "/provider/NDNSF/STREAM-MAP/round-trip/frontier",
            20.0,
        )
        descriptor = PredictiveStreamDescriptor(native)
        encoded = descriptor.to_dict()
        restored = PredictiveStreamDescriptor.from_dict(encoded)
        self.assertEqual(restored.to_dict(), encoded)

    def test_recovery_control_status_surface_is_explicit(self):
        for field in (
            "recovery_control_interests",
            "recovery_frontier_interests",
            "recovery_group_interests",
            "recovery_coalesced_waiters",
            "recovery_metadata_cache_hits",
            "next_deliver_cursor",
            "ready_queue_depth",
            "oldest_ready_cursor",
            "terminal_gap_queue_depth",
            "drain_wake_count",
            "stale_ready_drops",
            "terminal_gap_superseded",
            "future_cursor_horizon",
        ):
            self.assertTrue(hasattr(NativeLiveStreamStatus, field), field)

    def test_predictive_publisher_is_the_only_high_level_surface(self):
        self.assertFalse(hasattr(StreamPublisher, "announce"))
        self.assertFalse(hasattr(StreamPublisher, "publish"))
        self.assertFalse(hasattr(StreamPublisher, "start_predictive"))
        self.assertTrue(hasattr(StreamPublisher, "start"))
        self.assertTrue(hasattr(StreamPublisher, "push"))
        self.assertTrue(hasattr(StreamPublisher, "flush"))

        native_definition = SimpleNamespace(
            stream_id="predictive",
            provider="/provider",
            semantic_data_prefix="/provider/data",
            session_epoch=17,
            mapping_version=2,
            contract_version=2,
            mapping_block_capacity=16,
            mapping_ahead_blocks=4,
            retained_items=600,
            max_name_reservations=65536,
            max_pending_interests=256,
            signed_wire_cap=8800,
            sample_period_ms=20.0,
            sample_classes=(),
            fec=LiveStreamFecOptions.none()._to_native(),
            mapping_root="/provider/NDNSF/STREAM-MAP/predictive",
        )

        class NativeDescriptor:
            definition = native_definition
            checkpoint = object()
            frontier_name = "/provider/stream/frontier"

        class FakeNative:
            def __init__(self):
                self.start_calls = 0

            def start(self):
                self.start_calls += 1
                return NativeDescriptor()

        native = FakeNative()
        publisher = StreamPublisher(native)
        descriptor = publisher.start()
        self.assertIsInstance(descriptor, PredictiveStreamDescriptor)
        self.assertEqual(descriptor.definition.stream_id, "predictive")
        self.assertEqual(
            descriptor.definition.mapping_root,
            "/provider/NDNSF/STREAM-MAP/predictive",
        )
        self.assertEqual(native.start_calls, 1)

    def test_config_defaults_map_to_native(self):
        config = StreamConfig(
            stream_id="telemetry",
            data_prefix="/provider/streams/telemetry",
            sample_period_ms=50.0,
            sample_classes=(SampleClassProfile("single", 1, 1),),
        )
        native = config._to_native()
        self.assertEqual(native.stream_id, "telemetry")
        self.assertEqual(native.data_prefix, "/provider/streams/telemetry")
        self.assertEqual(native.advanced.mapping_block_capacity, 16)
        self.assertEqual(native.advanced.startup_timeout_ms, 1000)

    def test_subscription_uses_frozen_predictive_defaults(self):
        options = StreamSubscriptionOptions(on_item=lambda item: True)
        self.assertIsNone(options.prefetch_policy)
        self.assertTrue(options.enable_fec_recovery)
        self.assertFalse(options.require_full_delivery)
        self.assertEqual(options.aggregate_interest_limit, 64)

    def test_advanced_and_fec_are_independent(self):
        config = StreamConfig(
            stream_id="audio",
            data_prefix="/provider/streams/audio",
            sample_period_ms=20.0,
            sample_classes=(SampleClassProfile("block", 2, 4),),
            fec=LiveStreamFecOptions.gf256_two_repair(4, 4096),
            advanced=StreamAdvancedOptions(retained_items=1200),
        )
        native = config._to_native()
        self.assertEqual(native.advanced.retained_items, 1200)
        self.assertEqual(native.fec.repair_symbols, 2)

    def test_service_provider_delegates_once(self):
        class FakeNative:
            def __init__(self):
                self.calls = []

            def create_stream(self, config):
                self.calls.append(config)
                return object()

        provider = ServiceProvider.__new__(ServiceProvider)
        provider._native = FakeNative()
        config = StreamConfig(
            stream_id="telemetry",
            data_prefix="/provider/telemetry",
            sample_period_ms=50.0,
            sample_classes=(SampleClassProfile("single", 1, 1),),
        )
        result = provider.create_stream(config)
        self.assertIsInstance(result, StreamPublisher)
        self.assertEqual(len(provider._native.calls), 1)

    def test_service_user_delegates_and_returns_predictive_subscriber(self):
        class FakeNative:
            def __init__(self):
                self.calls = []
                self.handle = object()

            def subscribe_stream(self, *args, **kwargs):
                self.calls.append((args, kwargs))
                return self.handle

        descriptor = PredictiveStreamDescriptor.__new__(PredictiveStreamDescriptor)
        descriptor._native = object()
        user = ServiceUser.__new__(ServiceUser)
        user._native = FakeNative()
        options = StreamSubscriptionOptions(on_item=lambda item: True)
        result = user.subscribe_stream(descriptor, options)
        self.assertIsInstance(result, PredictiveStreamSubscriber)
        self.assertIs(result._native, user._native.handle)
        self.assertEqual(len(user._native.calls), 1)
        self.assertIsNone(user._native.calls[0][1]["prefetch_policy"])
        self.assertTrue(user._native.calls[0][1]["enable_fec_recovery"])
        self.assertFalse(user._native.calls[0][1]["require_full_delivery"])


if __name__ == "__main__":
    unittest.main()
