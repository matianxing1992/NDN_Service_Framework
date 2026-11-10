"""Offline trace arithmetic/causality fixtures, not native tracking qualification."""
import copy
import json
import hashlib
from pathlib import Path
import tempfile
import unittest

from Experiments.UAV.person_run_receipt import collect_person_timing, build_person_run_receipt

CAMERAS = ("UAV1", "UAV2", "UAV3")


class PersonTimingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sources = {camera: "source-" + camera for camera in CAMERAS}
        self.scope = {"runId": self.root.name, "missionId": self.root.name, "windowId": "window-0",
                      "sequence": 1, "sourceEpoch": 5}
        self.plan = {"settings": {"windowCount": 1},
                     "observation": {"sourceEpoch": 5, "sourceIds": self.sources},
                     "personTracking": {"requestedDevice": "cpu", "profileDigest": "profile",
                                        "computationManifest": str(self.root / "computation.json")}}
        prepared = {"windowCount": 1, "sourceEpoch": 5, "sourceIds": self.sources,
                    "commonStartUs": 25000000, "sourceManifestDigest": "sources"}
        self.sampling = {"sampling": {camera: {"captureTimeUs": [25000000],
            "targetTimeUs": [25000000], "inputDigests": ["digest-" + camera]} for camera in CAMERAS}}
        self.prepared = prepared
        self.write_sampling()

        (self.root / "computation.json").write_text(json.dumps({"computationDigest": "computation"}))
        (self.root / "lifecycle.json").write_text(json.dumps({"terminal": True, "outcome": "collaboration-result"}))
        (self.root / "person-cleanup.json").write_text(json.dumps({"runStartedNs": 0,
            "cleanupEndedNs": 6000000000, "cleanupSeconds": .5, "remainingOwnedProcesses": 0}))
        self.logs = {name: [] for name in ["uav1-camera.log", "uav2-camera.log", "uav3-camera.log",
                                          "compute.log", "ground-station.log", "ground-station-display.log"]}
        self.native("ground-station.log", "replay-origin", 1000000000,
                    {"runId": self.root.name, "missionId": self.root.name})
        self.native("ground-station.log", "window-request", 1100000000)
        for index, camera in enumerate(CAMERAS):
            frame = {**self.scope, "cameraId": camera, "sourceId": self.sources[camera],
                     "commonPtsUs": 25000000}
            for event, ns in (("capture", 2000000000), ("publication-start", 2100000000),
                              ("published", 2200000000)):
                self.native(f"uav{index+1}-camera.log", event, ns, frame)
            self.native("compute.log", "fetch-start", 1900000000, frame)
            self.native("compute.log", "admitted", 2300000000, frame)
            self.native("ground-station.log", "received", 4000000000, frame)
            self.logs["ground-station-display.log"].append({"event": "DISPLAY_FRAME_SHOWN",
                "runId": self.root.name, "sessionId": self.root.name, "sequence": 1,
                "window": 0, "epoch": 5, "cameraId": camera, "ptsUs": 25000000,
                "clockDomain": "CLOCK_MONOTONIC_NS", "monotonicNs": 4500000000,
                "personDisplay": {"device": "cpu"}})
        for event, ns in (("worker-start", 3000000000), ("worker-end", 3500000000),
                          ("association-start", 3600000000), ("association-end", 3700000000),
                          ("committed", 3800000000)):
            self.native("compute.log", event, ns)
        self.logs["compute.log"].append({**self.scope, "event": "PERSON_BACKEND_COMPLETED",
            "clockDomain": "CLOCK_MONOTONIC_NS", "actualDevice": "cpu",
            "modelInitStartNs": 3100000000, "modelInitEndNs": 3200000000,
            "inferenceStartNs": 3300000000, "inferenceEndNs": 3400000000})

    def write_sampling(self):
        raw = json.dumps(self.sampling).encode()
        (self.root / "source-frame-inputs.json").write_bytes(raw)
        self.prepared["sourceFrameInputsDigest"] = "sha256:" + hashlib.sha256(raw).hexdigest()
        (self.root / "prepared-person-sources.json").write_text(json.dumps(self.prepared))

    def test_actual_per_camera_media_pts_preserve_nominal_schedule(self):
        self.sampling["sampling"]["UAV2"]["captureTimeUs"] = [25025000]
        self.write_sampling()
        for records in self.logs.values():
            for record in records:
                if record.get("cameraId") == "UAV2":
                    record["ptsUs" if record["event"] == "DISPLAY_FRAME_SHOWN" else "commonPtsUs"] = 25025000
        result = self.collect()
        self.assertTrue(result["timingQualified"], result["errors"])
        frame = next(f for f in result["frames"] if f["cameraId"] == "UAV2")
        self.assertEqual((frame["commonPtsUs"], frame["targetTimeUs"], frame["scheduledCaptureNs"]),
                         (25025000, 25000000, 1000000000))

    def native(self, filename, event, ns, scope=None):
        self.logs[filename].append({**(self.scope if scope is None else scope), "event": event,
            "schema": "uav-person-runtime-trace-v1", "clockDomain": "CLOCK_MONOTONIC_NS",
            "monotonicNs": ns})

    def write(self):
        for filename, records in self.logs.items():
            (self.root / filename).write_text("\n".join(
                ("SPEC219_TRACE " if record.get("schema") == "uav-person-runtime-trace-v1" else "") +
                json.dumps(record) for record in records) + "\n")

    def collect(self):
        self.write()
        return collect_person_timing(self.root, self.plan)

    def test_actual_domains_and_literal_durations(self):
        result = self.collect()
        self.assertTrue(result["timingQualified"], result["errors"])
        self.assertEqual((result["completeFrameTimingRecords"], result["completeWindowTimingRecords"]), (3, 1))
        for field, expected in (("e2eMs", 3000), ("displayMs", 500), ("captureToDisplayMs", 2500),
                                ("replayLagMs", 1000), ("modelInitializationMs", 100),
                                ("modelWindowMs", 100), ("workerMs", 500), ("associationMs", 100),
                                ("retrievalAndValidationMs", 400), ("admittedToWorkerMs", 700)):
            self.assertEqual(result["summary"][field]["p50"], expected)
            self.assertEqual(result["summary"][field]["p95"], expected)
        self.assertEqual(result["frames"][0]["commonPtsUs"], 25000000)
        self.assertEqual(result["frames"][0]["scheduledCaptureNs"], 1000000000)

    def test_model_resource_sample_stays_bound_to_its_completion_window(self):
        completed = self.logs['compute.log'][-1]
        sample = {'monotonicNs': 3250000000, 'pid': 20, 'creationTimeUnixSeconds': 2,
                  'rssBytes': 200, 'threads': 2}
        completed.update(pid=20, modelResourceSample=sample)
        result = self.collect()
        self.assertTrue(result['timingQualified'], result['errors'])
        self.assertEqual(result['windows'][0]['modelResourceSample'], sample)
        completed['modelResourceSample'] = {'error': 'AccessDenied'}
        result = self.collect()
        self.assertTrue(result['timingQualified'], result['errors'])
        self.assertEqual(result['windows'][0]['modelResourceSample'], {'error': 'AccessDenied'})
        completed['runId'] = 'different-run'
        result = self.collect()
        self.assertFalse(result['timingQualified'])
        self.assertNotIn('modelResourceSample', result['windows'][0])

    def test_missing_frame_not_removed_or_zero_filled(self):
        self.logs["uav2-camera.log"] = []
        result = self.collect()
        self.assertFalse(result["timingQualified"])
        self.assertEqual(len(result["frames"]), 3)
        self.assertEqual(result["completeFrameTimingRecords"], 2)
        missing = next(f for f in result["frames"] if f["cameraId"] == "UAV2")
        self.assertIsNone(missing["captureNs"])
        self.assertFalse(missing["complete"])

    def test_percentile_interpolates_observed_values(self):
        self.logs["ground-station.log"][3]["monotonicNs"] = 4100000000
        self.logs["ground-station.log"][4]["monotonicNs"] = 4200000000
        summary = self.collect()["summary"]["e2eMs"]
        self.assertEqual((summary["count"], summary["p50"], summary["p95"], summary["max"]),
                         (3, 3100, 3190, 3200))

    def test_no_samples_have_no_percentile(self):
        self.logs = {filename: [] for filename in self.logs}
        result = self.collect()
        self.assertFalse(result["timingQualified"])
        self.assertEqual(result["completeFrameTimingRecords"], 0)
        self.assertTrue(all(value is None for value in result["summary"].values()))

    def test_wrong_scope_source_clock_and_boolean_are_rejected(self):
        original = copy.deepcopy(self.logs)
        for field, value in (("sourceId", "wrong"), ("sourceEpoch", 6), ("commonPtsUs", 0),
                             ("clockDomain", "MEDIA_US"), ("runId", "wrong"),
                             ("monotonicNs", True)):
            with self.subTest(field=field):
                self.logs = copy.deepcopy(original)
                self.logs["uav1-camera.log"][0][field] = value
                result = self.collect()
                self.assertFalse(result["timingQualified"])
                self.assertTrue(result["errors"])

    def test_duplicate_and_wrong_actor_are_rejected(self):
        self.logs["compute.log"].append(copy.deepcopy(self.logs["compute.log"][0]))
        self.assertFalse(self.collect()["timingQualified"])
        self.logs["compute.log"].pop()
        self.logs["uav1-camera.log"].append(self.logs["compute.log"].pop(0))
        self.assertFalse(self.collect()["timingQualified"])

    def test_causal_order_and_model_clock(self):
        self.logs["compute.log"][-1]["modelInitEndNs"] = 3600000000
        result = self.collect()
        self.assertFalse(result["timingQualified"])
        self.assertEqual(result["completeWindowTimingRecords"], 0)
        self.assertIn("noncausal window", " ".join(result["errors"]))

    def test_request_drives_source_and_fetch_after_replay_origin(self):
        original = copy.deepcopy(self.logs)
        for defect in ("source-before-request", "fetch-before-request", "request-before-origin"):
            with self.subTest(defect=defect):
                self.logs = copy.deepcopy(original)
                if defect == "source-before-request":
                    for record, ns in zip(self.logs["uav1-camera.log"], (500000000, 600000000, 700000000)):
                        record["monotonicNs"] = ns
                elif defect == "fetch-before-request":
                    self.logs["compute.log"][0]["monotonicNs"] = 500000000
                else:
                    self.logs["ground-station.log"][1]["monotonicNs"] = 500000000
                result = self.collect()
                self.assertFalse(result["timingQualified"])
                self.assertIn("noncausal", " ".join(result["errors"]))

    def test_publication_completion_can_overlap_admission(self):
        self.logs["uav1-camera.log"][-1]["monotonicNs"] = 2400000000
        self.assertTrue(self.collect()["timingQualified"])
        self.logs["uav1-camera.log"][-1]["monotonicNs"] = 2000000000
        self.assertFalse(self.collect()["timingQualified"])

    def test_sampling_digest_and_byte_bound_reject_before_join(self):
        path = self.root / "source-frame-inputs.json"
        path.write_bytes(path.read_bytes() + b" ")
        self.write()
        with self.assertRaisesRegex(ValueError, "sampling digest"):
            collect_person_timing(self.root, self.plan)
        path.write_bytes(b" " * (1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "oversize"):
            collect_person_timing(self.root, self.plan)

    def test_dropped_native_trace_is_explicit_even_when_other_points_complete(self):
        self.write()
        with (self.root / "compute.log").open("a") as stream:
            stream.write("SPEC219_TRACE_DROPPED reason=oversize\n")
        result = collect_person_timing(self.root, self.plan)
        self.assertEqual(result["completeFrameTimingRecords"], 3)
        self.assertFalse(result["timingQualified"])
        self.assertIn("native timing trace dropped", " ".join(result["errors"]))

    def test_receipt_retains_unqualified_data_resources(self):
        self.write()
        receipt = build_person_run_receipt(self.root, self.plan, closure_verified=False)
        self.assertEqual(receipt["elapsedSeconds"], 6)
        self.assertEqual(receipt["effectiveProcessedFramesPerSecond"], .5)
        self.assertEqual(receipt["sourceSampleHz"], 2)
        self.assertEqual(receipt["counts"], {"plannedWindows": 1, "plannedFrames": 3,
            "admittedFrames": 3, "committedWindows": 1, "receivedFrames": 3,
            "displayedFrames": 3, "uncommittedWindows": 0, "unreceivedScheduledFrames": 0})
        self.assertEqual(receipt["completeFrameTimingRecords"], 3)
        self.assertFalse(receipt["installedClosureVerified"])
        self.assertFalse(receipt["resourceMeasurementsComplete"])
        self.assertFalse(receipt["dataQualified"])
        self.assertIsNone(receipt["synthetic"])
        self.assertEqual(receipt['datasetOrigin'], 'unspecified')
        for origin in ('real-captured', 'synthetic-rendered'):
            self.prepared['datasetOrigin'] = origin
            self.write_sampling()
            self.assertEqual(json.loads((self.root / 'prepared-person-sources.json').read_text())[
                'datasetOrigin'], origin)
            receipt = build_person_run_receipt(self.root, self.plan, closure_verified=False)
            self.assertEqual(receipt['datasetOrigin'], origin)
            self.assertIsNone(receipt['synthetic'])
            self.assertFalse(receipt['dataQualified'])


if __name__ == "__main__":
    unittest.main()
