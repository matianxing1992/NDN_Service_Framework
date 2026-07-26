#!/usr/bin/env python3
"""Spec 120 deterministic contract checks."""

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/stream/unified-video-performance-v1.json"


class UnifiedVideoPerformanceFixtureTest(unittest.TestCase):
    def test_current_metric_origin_is_truthful_and_baseline_is_frozen(self) -> None:
        value = json.loads(FIXTURE.read_text())
        metric = value["metric"]
        self.assertEqual(metric["canonicalName"],
                         "encoded-output-to-decoder-output")
        self.assertFalse(metric["includesCameraAcquisition"])
        self.assertFalse(metric["includesH264Encoding"])
        baseline = value["baseline"]
        self.assertEqual(baseline["providerFutureEligible"],
                         baseline["providerFutureHits"])
        self.assertLess(baseline["rttP50Ms"] / 2,
                        baseline["encodedOutputToDecoderOutputP50Ms"])
        self.assertLessEqual(baseline["encodedOutputToDecoderOutputP50Ms"],
                             baseline["encodedOutputToDecoderOutputP95Ms"])
        self.assertLessEqual(baseline["encodedOutputToDecoderOutputP95Ms"],
                             baseline["encodedOutputToDecoderOutputP99Ms"])

    def test_timeline_contract_does_not_invent_cross_clock_one_way_time(self) -> None:
        value = json.loads(FIXTURE.read_text())
        self.assertEqual(value["crossClockRule"],
                         "unavailable-without-offset-uncertainty")
        self.assertEqual(value["requiredProviderOrder"][-1], "data-put")
        self.assertEqual(value["requiredConsumerOrder"][-1], "decoder-output")


class UnifiedVideoImplementationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.core_h = (ROOT / "ndn-service-framework/Stream.hpp").read_text()
        cls.core_cpp = (ROOT / "ndn-service-framework/Stream.cpp").read_text()
        cls.drone = (ROOT / "NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp").read_text()
        cls.gs = (ROOT / "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp").read_text()
        cls.window = (ROOT / "NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp").read_text()
        cls.protocol_h = (ROOT / "NDNSF-UAV-APP/shared/UavProtocol.hpp").read_text()
        cls.protocol_cpp = (ROOT / "NDNSF-UAV-APP/shared/UavProtocol.cpp").read_text()

    def test_auto_video_stop_retries_are_bounded_under_control_loss(self) -> None:
        self.assertIn("for (int attempt = 0; attempt < 4; ++attempt)", self.window)
        self.assertIn("m_runtime.activeVideoDroneId().empty()", self.window)
        self.assertIn("AUTO_VIDEO_STOP_RETRY attempt=", self.window)

    def test_core_feed_is_bounded_immutable_and_app_neutral(self) -> None:
        self.assertIn("class PublishedPacketFeed", self.core_h)
        self.assertIn("ndn::Buffer signedDataWire", self.core_h)
        self.assertIn("StreamContentDigest wireDigest", self.core_h)
        self.assertIn("maxQueuedPackets", self.core_h)
        self.assertIn("maxQueuedBytes", self.core_h)
        self.assertIn("firstDroppedCursor", self.core_h)
        self.assertNotIn("Uav", self.core_h)
        self.assertNotIn("RepoCore", self.core_cpp)

    def test_names_only_mapping_lead_is_core_owned_and_uses_measured_timing(self) -> None:
        self.assertIn("computeLiveStreamMappingLead", self.core_cpp)
        self.assertIn("m_readiness.samplePeriodMs()", self.drone)
        self.assertIn("ensureFutureSampleAnnouncementsLocked", self.drone)
        self.assertNotIn("m_mappingRttMs", self.drone)
        self.assertNotIn("m_mappingJitterMs", self.drone)
        self.assertNotIn("embedded_media", self.drone)

    def test_provider_encoder_pipe_and_packetization_are_time_bounded(self) -> None:
        self.assertIn("::read(fileno(pipe.get())", self.drone)
        self.assertIn("NDNSF_UAV_ENCODER_PIPE_READ_MODE", self.drone)
        self.assertIn('std::string(mode) == "stdio-batched"', self.drone)
        self.assertIn("m_encoderPacketizationTimeoutMs", self.drone)
        self.assertIn("VIDEO_ENCODER_PACKETIZATION_FLUSH", self.drone)

    def test_retention_stores_the_exact_core_packet_once(self) -> None:
        self.assertIn("packet.signedDataWire", self.drone)
        self.assertIn('"application/ndn-data"', self.drone)
        self.assertIn("packet.wireDigest", self.drone)
        self.assertIn("PublishedPacketFeed", self.drone)
        self.assertNotIn("recordRaw" + "Chunk", self.drone)
        self.assertNotIn("recordSingleRaw" + "Chunk", self.drone)

    def test_manifest_and_grant_keep_plaintext_secrets_out_of_repo(self) -> None:
        self.assertIn("CanonicalVideoRecordingManifest", self.protocol_h)
        self.assertIn("UavVideoContentKeyGrant", self.protocol_h)
        self.assertIn("grant_transport", self.drone)
        self.assertIn("protected-ndnsf-response-only", self.drone)
        self.assertIn("canonical-video-epoch-key-archive-v1", self.drone)
        self.assertIn("RSA-OAEP", self.drone)
        self.assertIn("recoverLatestCompletedRecording", self.drone)
        self.assertIn("packet catalog head digest mismatch", self.drone)
        self.assertNotIn("std::vector<uint8_t> contentKey", self.protocol_h)
        runtime = self.drone + self.gs + self.protocol_cpp
        self.assertNotIn("recording_encryption_content_key_hex", runtime)
        self.assertNotIn("hybrid-aes-256-gcm-at-rest", runtime)

    def test_replay_reuses_live_consumer_and_decoder_admission(self) -> None:
        self.assertIn("LiveStreamStart::Beginning", self.gs)
        self.assertIn("admitLiveVideoItem(item)", self.gs)
        self.assertIn("startCanonicalRecordingPlayback", self.gs)
        self.assertNotIn("decodeRecordingFrom" + "FetchedChunksAsync", self.gs)
        self.assertNotIn("startRecordingPlayback", self.gs)

    def test_lifecycle_and_legacy_failure_are_explicit(self) -> None:
        self.assertIn("viewer-detached-retention-active", self.drone)
        self.assertIn("bounded-feed-overflow", self.drone)
        self.assertIn("unsupported-legacy-recording", self.drone)
        self.assertIn("export-with-pre-spec120-or-delete-old-repo", self.drone)
        self.assertIn("publication_state", self.drone)
        self.assertIn("live_consumption_state", self.drone)
        self.assertIn("retention_state", self.drone)
        self.assertIn("finalizeRetention", self.drone)
        self.assertIn("finalize_retention", self.gs)
        self.assertIn("startRetention", self.drone)
        self.assertIn('retentionAction == "restart"', self.drone)
        self.assertIn("retention-finalized-empty", self.drone)
        self.assertIn("m_retentionLifecycleMutex", self.drone)
        self.assertIn("join=next-encoder-idr", self.drone)
        self.assertIn("CAMERA_VIEWER_SAFE_JOIN_REQUESTED", self.drone)
        self.assertIn("CAMERA_VIEWER_SAFE_JOIN_READY", self.drone)
        self.assertIn("viewer-safe-join-timeout", self.drone)
        self.assertIn("requestRecordingRetentionAction", self.gs)
        self.assertIn("action=storage-circuit-open", self.drone)
        self.assertIn("retention_storage_circuit", self.drone)
        self.assertIn("m_retentionWorkerBusy", self.drone)
        self.assertIn("retention-finalize-drain-timeout", self.drone)
        self.assertIn("CAMERA_ARCHIVED_TRUST_CERTIFICATE_ROTATED", self.drone)
        self.assertIn("archived_packets=unchanged", self.drone)

    def test_timeline_uses_sampled_ndn_log_events(self) -> None:
        for event in ("data-received", "signature-validated", "decrypted",
                      "reorder-ready", "decoder-input", "decoder-output"):
            self.assertIn(f'"{event}"', self.gs)
        self.assertIn("logStreamTimelineTrace", self.gs)
        self.assertNotIn("capture_to_decode_ms", self.gs)
        self.assertNotIn("encoded_output_to_decoder_output_ms", self.gs)
        self.assertIn("GS_VIDEO_OUTPUT_CADENCE", self.gs)
        self.assertIn("GS_VIDEO_DECODER_STARTUP", self.gs)
        self.assertNotIn("m_encodedOutputToDecoderOutputMs", self.gs)
        self.assertNotIn("DecoderFrameCorrelation", self.gs)
        self.assertIn('"frame_correlation", "ambiguous-one-to-many"', self.gs)
        self.assertIn("GS_VIDEO_GUI_DELIVERY", self.window)
        launcher = (ROOT / "Experiments/NDNSF_UAV_Unified_Video_Minindn.py").read_text()
        self.assertIn("h264-input-group-to-decoded-frame-cardinality-ambiguous", launcher)

    def test_campaign_can_reduce_trace_sampling_after_overhead_failure(self) -> None:
        launcher = (ROOT / "Experiments/NDNSF_UAV_Unified_Video_Minindn.py").read_text()
        self.assertIn("--trace-sample-denominator", launcher)
        self.assertIn("type=int, default=50", launcher)
        self.assertIn('env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = str(', launcher)
        evidence = json.loads((
            ROOT / "specs/120-uav-unified-video-object/trace-matrix-summary.json"
        ).read_text())
        self.assertEqual(evidence["sample_denominator"], 50)
        self.assertEqual(len(evidence["pairs"]), 5)
        self.assertGreaterEqual(
            sum(pair["within_sc015"] for pair in evidence["pairs"]), 4)
        self.assertTrue(evidence["sc015"]["passed"])
        self.assertTrue(all(pair["sampled_cursors_on"] > 0
                            for pair in evidence["pairs"]))

    def test_canonical_live_ready_status_opens_the_visible_gui_path(self) -> None:
        self.assertIn('status.rfind("Protected LiveStream", 0) == 0', self.window)
        self.assertIn("AUTO_VIDEO_GUI_RENDER_GATE", self.window)
        launcher = (ROOT / "Experiments/NDNSF_UAV_GUI_Minindn.py").read_text()
        self.assertIn(
            'require_log(gs_log, "AUTO_VIDEO_GUI_RENDER_GATE status=PASS")',
            launcher)


if __name__ == "__main__":
    unittest.main()
