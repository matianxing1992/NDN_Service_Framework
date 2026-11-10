"""Literal offline metric oracles; these fixtures do not qualify native runs."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from Experiments.UAV.person_tracking_metrics import (
    _assess_admitted_run, evaluate_person_tracking, validate_person_run)
from Experiments.UAV.tracking_report import evaluate_tracking_run, ResultError

CAMERAS = ("UAV1", "UAV2", "UAV3")
BOX_A = [0, 0, 10, 10]
BOX_B = [20, 0, 30, 10]


def travel_event(source="UAV1", target="UAV2", a=0, b=500000, *,
                 identity="A", event_id="travel"):
    return {"eventId": event_id, "globalGtId": identity, "sourceCamera": source,
            "targetCamera": target, "sourceLastVisiblePts": a,
            "targetFirstVisiblePts": b, "nonOverlap": True}


def literal_origin(camera, local, pts):
    return {"localKey": {"cameraId": camera, "sourceId": "source-" + camera,
                         "sourceEpoch": 1, "trackerGeneration": 1, "byteTrackId": local},
            "ptsUs": pts, "active": False}


class Fixture:
    def __init__(self, root, windows=2):
        self.root = root
        self.context = {"schema": "uav-person-display-context-v1", "runId": "r", "missionId": "m",
                        "profileDigest": "p", "computationDigest": "c", "requestedDevice": "cpu",
                        "maxPersonsPerFrame": 64, "windowCount": windows, "windowPrefix": "window",
                        "generations": [{"firstWindowIndex": 0, "sourceEpoch": 1,
                                         "sourceIds": ["source-" + c for c in CAMERAS]}]}
        self.manifest = {"schema": "uav-person-evaluation-v1", "dataset": "literal-fixture",
                         "sourceManifestDigest": "s", "sampleHz": 2, "durationUs": windows * 500000,
                         "split": {"name": "development", "frameDisjoint": True,
                                   "sharedActorsDisclosed": True},
                         "windows": [], "transitions": [], "excludedEvents": []}
        self.results = []
        for index in range(windows):
            pts = index * 500000
            wid = "window-" + str(index)
            gt_frames, frames = [], []
            for camera in CAMERAS:
                gt_frames.append({"cameraId": camera, "commonPtsUs": pts, "width": 100, "height": 100,
                                  "persons": [], "ignoreRegions": []})
                frames.append({"cameraId": camera, "sourceId": "source-" + camera, "sourceEpoch": 1,
                               "sequence": index + 1, "trackerGeneration": 1, "commonPtsUs": pts,
                               "width": 100, "height": 100, "tracks": []})
            self.manifest["windows"].append({"windowId": wid, "sequence": index + 1, "targetTimeUs": pts, "frames": gt_frames})
            self.results.append({"schema": "uav-person-tracking-result-v1", "trackingMode": "indoor-person-reid",
                                 "runId": "r", "missionId": "m", "profileDigest": "p", "computationDigest": "c",
                                 "windowId": wid, "sequence": index + 1, "terminal": True, "frames": frames,
                                 "associationEvents": [], "requestedDevice": "cpu", "actualDetectorDevice": "cpu",
                                 "actualReidDevice": "cpu", "reidInferenceBatches": 1})

    def person(self, window, camera, gt, gid, local, box=BOX_A, origin=None):
        index = CAMERAS.index(camera)
        if gt is not None:
            self.manifest["windows"][window]["frames"][index]["persons"].append({"gtId": gt, "box": box})
        if gid is False:  # absent prediction; None instead means an unassigned predicted box
            return
        self.results[window]["frames"][index]["tracks"].append({
            "cameraId": camera, "sourceId": "source-" + camera, "sourceEpoch": 1, "trackerGeneration": 1,
            "byteTrackId": local, "globalId": gid, "box": box, "ptsUs": window * 500000,
            "associationStatus": "MATCHED" if origin else "CONTINUED", "appearanceDistance": None,
            "associationOrigin": origin})

    def write(self, missing=()):
        (self.root / "context.json").write_text(json.dumps(self.context))
        for index, result in enumerate(self.results):
            if index in missing:
                continue
            result["associationEvents"] = []
            for frame in result["frames"]:
                for track in frame["tracks"]:
                    result["associationEvents"].append({
                        "cameraId": track["cameraId"], "byteTrackId": track["byteTrackId"],
                        "globalId": track["globalId"], "status": track["associationStatus"],
                        "distance": track["appearanceDistance"], "ptsUs": track["ptsUs"],
                        "targetLocalKey": {k: track[k] for k in ("cameraId", "sourceId", "sourceEpoch",
                                                               "trackerGeneration", "byteTrackId")},
                        "origin": track["associationOrigin"]})
            raw = json.dumps(result)
            projection = {k: self.context[k] for k in ("runId", "missionId", "profileDigest", "computationDigest")}
            projection.update(schema="uav-person-display-v1", state="ready", resultJson=raw,
                              responseProvider="/example/uav/compute",
                              resultDigest="sha256:" + hashlib.sha256(raw.encode()).hexdigest())
            directory = self.root / result["windowId"]
            directory.mkdir(exist_ok=True)
            (directory / "presentation.json").write_text(json.dumps(projection))
        path = self.root / "evaluation.json"
        path.write_text(json.dumps(self.manifest))
        return path

    def score(self, missing=()):
        return evaluate_person_tracking(self.root, self.write(missing))


class PersonMetricTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture = Fixture(Path(self.temp.name))

    def test_perfect_global_match(self):
        f = self.fixture
        for i in range(2):
            f.person(i, "UAV1", "A", 1, 1)
            f.person(i, "UAV2", "B", 2, 2)
        m = f.score()
        self.assertEqual((m["Ngt"], m["Npred"], m["IDTP"], m["IDF1"]), (4, 4, 4, 1))
        self.assertEqual((m["detectionTP"], m["idSwitches"]), (4, 0))
        self.assertFalse(validate_person_run({}, m)["qualified"])

    def test_dataset_origin_is_reported_and_invalid_origin_rejected_before_results(self):
        f = self.fixture
        self.assertEqual(f.score()['datasetOrigin'], 'unspecified')
        for origin in ('real-captured', 'synthetic-rendered'):
            f.manifest['datasetOrigin'] = origin
            self.assertEqual(f.score()['datasetOrigin'], origin)
        for invalid in ('unspecified', 'synthetic', None, True, {}):
            with self.subTest(invalid=invalid):
                f.manifest['datasetOrigin'] = invalid
                path = f.write()
                with self.assertRaisesRegex(ValueError, 'dataset origin'):
                    evaluate_person_tracking(f.root / 'absent', path)

    def test_actual_camera_pts_can_follow_nominal_sampling_target(self):
        f = self.fixture
        f.person(0, "UAV1", "A", 1, 1)
        f.person(0, "UAV2", "B", 2, 2)
        f.manifest["windows"][0]["frames"][1]["commonPtsUs"] = 25000
        f.results[0]["frames"][1]["commonPtsUs"] = 25000
        f.results[0]["frames"][1]["tracks"][0]["ptsUs"] = 25000
        self.assertEqual(f.score()["IDF1"], 1)

    def test_all_one_id_is_penalized_across_cameras(self):
        f = self.fixture
        for i in range(2):
            f.person(i, "UAV1", "A", 1, 1)
            f.person(i, "UAV2", "B", 1, 2)
        m = f.score()
        self.assertEqual((m["IDTP"], m["IDFP"], m["IDFN"], m["IDF1"]), (2, 2, 2, .5))
        self.assertEqual(m["detectionRecall"], 1)

    def test_all_new_and_switched_ids(self):
        f = self.fixture
        for i in range(2):
            f.person(i, "UAV1", "A", 2*i + 1, 1)
            f.person(i, "UAV2", "B", 2*i + 2, 2)
        m = f.score()
        self.assertEqual((m["IDTP"], m["IDF1"], m["idSwitches"]), (2, .5, 2))
        f.results[1]["frames"][0]["tracks"][0]["globalId"] = 2
        f.results[1]["frames"][1]["tracks"][0]["globalId"] = 1
        m = f.score()
        self.assertEqual((m["IDTP"], m["IDF1"], m["idSwitches"]), (2, .5, 2))

    def test_missing_window_remains_false_negative(self):
        f = self.fixture
        f.person(0, "UAV1", "A", 1, 1)
        f.person(1, "UAV1", "A", 1, 1)
        m = f.score(missing=(1,))
        self.assertEqual((m["Ngt"], m["Npred"], m["IDTP"], m["IDFN"]), (2, 1, 1, 1))
        self.assertAlmostEqual(m["IDF1"], 2/3)
        self.assertEqual(m["missingWindows"], ["window-1"])

    def test_ignore_cannot_remove_gt_candidate_or_unassigned_fp(self):
        f = self.fixture
        frame = f.manifest["windows"][0]["frames"][0]
        frame["ignoreRegions"] = [BOX_A, BOX_B]
        f.person(0, "UAV1", "A", 1, 1)
        f.person(0, "UAV1", None, 2, 2, BOX_B)
        f.person(0, "UAV2", None, None, 3)
        m = f.score()
        self.assertEqual((m["Ngt"], m["Npred"], m["ignoredPredictions"], m["IDFP"]), (1, 2, 1, 1))
        self.assertAlmostEqual(m["IDF1"], 2/3)

    def transition(self, active=False):
        f = self.fixture
        origin = {"localKey": {"cameraId": "UAV1", "sourceId": "source-UAV1", "sourceEpoch": 1,
                               "trackerGeneration": 1, "byteTrackId": 7}, "ptsUs": 0, "active": active}
        f.person(0, "UAV1", "A", 1, 7)
        f.person(1, "UAV2", "A", 1, 9, origin=origin)
        f.manifest["transitions"] = [{"eventId": "A-1-2", "globalGtId": "A", "sourceCamera": "UAV1",
                                      "targetCamera": "UAV2", "sourceLastVisiblePts": 0,
                                      "targetFirstVisiblePts": 500000, "nonOverlap": True}]
        return f

    def test_transition_rejects_source_episode_interior(self):
        f = Fixture(self.fixture.root, windows=3)
        f.person(0, "UAV1", "A", False, 7)
        f.person(1, "UAV1", "A", False, 7)
        f.person(2, "UAV2", "A", False, 9)
        f.manifest["transitions"] = [travel_event(b=1000000)]
        with self.assertRaisesRegex(ValueError, "source endpoint.*episode end"):
            f.score()

    def test_transition_rejects_target_episode_interior(self):
        f = Fixture(self.fixture.root, windows=3)
        f.person(0, "UAV1", "A", False, 7)
        f.person(1, "UAV2", "A", False, 9)
        f.person(2, "UAV2", "A", False, 9)
        f.manifest["transitions"] = [travel_event(b=1000000)]
        with self.assertRaisesRegex(ValueError, "target endpoint.*episode start"):
            f.score()

    def test_transition_rejects_stale_source_then_accepts_repaired_event(self):
        f = Fixture(self.fixture.root, windows=4)
        f.person(0, "UAV1", "A", 1, 7)
        f.person(2, "UAV1", "A", 1, 8)
        f.person(3, "UAV2", "A", 1, 9, origin=literal_origin("UAV1", 8, 1000000))
        event = travel_event(b=1500000)
        f.manifest["transitions"] = [event]
        with self.assertRaisesRegex(ValueError, "source reappears"):
            f.score()
        event["sourceLastVisiblePts"] = 1000000
        m = f.score()
        self.assertEqual((m["crossCameraTP"], m["crossCameraFN"], m["IDF1"]), (1, 0, 1))

    def test_transition_rejects_target_occlusion_reappearance(self):
        f = Fixture(self.fixture.root, windows=4)
        f.person(0, "UAV1", "A", False, 7)
        f.person(1, "UAV2", "A", False, 9)
        f.person(3, "UAV2", "A", False, 10)
        f.manifest["transitions"] = [travel_event(b=1500000)]
        with self.assertRaisesRegex(ValueError, "target is already visible"):
            f.score()

    def test_transition_rejects_simultaneous_and_reversed_sampled_endpoints(self):
        for source_window, a, b in ((0, 0, 0), (1, 200000, 300000)):
            with self.subTest(source_window=source_window):
                f = Fixture(self.fixture.root)
                f.person(source_window, "UAV1", "A", False, 7)
                f.person(0, "UAV2", "A", False, 9)
                f.manifest["transitions"] = [travel_event(a=a, b=b)]
                with self.assertRaisesRegex(ValueError, "simultaneous or reversed"):
                    f.score()

    def test_transition_rejects_ambiguous_source_or_target_endpoints(self):
        for ambiguous in ("source", "target"):
            with self.subTest(ambiguous=ambiguous):
                f = Fixture(self.fixture.root, windows=3)
                f.person(0, "UAV1", "A", False, 7)
                f.person(2, "UAV2", "A", False, 9)
                if ambiguous == "source":
                    f.person(1, "UAV1", "A", False, 7)
                    event = travel_event(a=250000, b=1000000)
                else:
                    f.person(1, "UAV2", "A", False, 9)
                    event = travel_event(b=750000)
                f.manifest["transitions"] = [event]
                with self.assertRaisesRegex(ValueError, "ambiguous sampled GT"):
                    f.score()

    def test_transition_rejects_missing_or_outside_tolerance_endpoints(self):
        for missing in (False, True):
            with self.subTest(missing=missing):
                f = Fixture(self.fixture.root, windows=4)
                f.person(0, "UAV1", "A", False, 7)
                f.person(3, "UAV2", "A", False, 9)
                f.manifest["transitions"] = [travel_event(
                    a=0 if missing else 500001, b=1500000,
                    identity="missing" if missing else "A")]
                with self.assertRaisesRegex(ValueError, "no sampled GT endpoint"):
                    f.score()

    def test_transition_accepts_inclusive_endpoint_tolerance_and_actual_pts(self):
        for shifted in (False, True):
            with self.subTest(shifted=shifted):
                f = Fixture(self.fixture.root, windows=4)
                source_pts, target_pts = (25000, 1520000) if shifted else (0, 1500000)
                f.person(0, "UAV1", "A", 1, 7)
                f.person(3, "UAV2", "A", 1, 9,
                         origin=literal_origin("UAV1", 7, source_pts))
                for window, camera, pts in ((0, 0, source_pts), (3, 1, target_pts)):
                    f.manifest["windows"][window]["frames"][camera]["commonPtsUs"] = pts
                    frame = f.results[window]["frames"][camera]
                    frame["commonPtsUs"] = frame["tracks"][0]["ptsUs"] = pts
                f.manifest["transitions"] = [travel_event(
                    a=0 if shifted else 500000, b=1500000)]
                m = f.score()
                self.assertEqual((m["crossCameraTP"], m["crossCameraFP"], m["crossCameraFN"]),
                                 (1, 0, 0))

    def test_transition_rejects_renamed_and_jittered_event_reuse(self):
        for jittered in (False, True):
            with self.subTest(jittered=jittered):
                f = Fixture(self.fixture.root)
                f.person(0, "UAV1", "A", False, 7)
                f.person(1, "UAV2", "A", False, 9)
                f.manifest["transitions"] = [travel_event(), travel_event(
                    a=100000 if jittered else 0, b=500001 if jittered else 500000,
                    event_id="another-event")]
                with self.assertRaisesRegex(ValueError, "reuses a source exit or target entry"):
                    f.score()

    def test_transition_rejects_source_exit_reuse_across_targets(self):
        f = self.fixture
        f.person(0, "UAV1", "A", False, 7)
        f.person(1, "UAV2", "A", False, 9)
        f.person(1, "UAV3", "A", False, 10)
        f.manifest["transitions"] = [travel_event(), travel_event(
            target="UAV3", event_id="another-target")]
        with self.assertRaisesRegex(ValueError, "reuses a source exit or target entry"):
            f.score()

    def test_transition_rejects_target_entry_reuse_across_sources(self):
        f = self.fixture
        f.person(0, "UAV1", "A", False, 7)
        f.person(0, "UAV3", "A", False, 8)
        f.person(1, "UAV2", "A", False, 9)
        f.manifest["transitions"] = [travel_event(), travel_event(
            source="UAV3", event_id="another-source")]
        with self.assertRaisesRegex(ValueError, "reuses a source exit or target entry"):
            f.score()

    def test_transition_allows_other_people_at_same_endpoints(self):
        f = self.fixture
        for gt, gid, source_local, target_local, box in (("A", 1, 7, 9, BOX_A),
                                                        ("B", 2, 8, 10, BOX_B)):
            f.person(0, "UAV1", gt, gid, source_local, box)
            f.person(1, "UAV2", gt, gid, target_local, box,
                     origin=literal_origin("UAV1", source_local, 0))
        f.manifest["transitions"] = [travel_event(), travel_event(identity="B", event_id="B")]
        m = f.score()
        self.assertEqual((m["crossCameraTP"], m["crossCameraFN"], m["IDF1"]), (2, 0, 1))

    def test_transition_allows_later_genuine_repeat_visits(self):
        f = Fixture(self.fixture.root, windows=6)
        f.person(0, "UAV1", "A", 1, 7)
        f.person(1, "UAV2", "A", 1, 9, origin=literal_origin("UAV1", 7, 0))
        f.person(3, "UAV1", "A", 1, 11, origin=literal_origin("UAV2", 9, 500000))
        f.person(5, "UAV2", "A", 1, 13, origin=literal_origin("UAV1", 11, 1500000))
        f.manifest["transitions"] = [travel_event(), travel_event(
            "UAV2", "UAV1", 500000, 1500000, event_id="return"), travel_event(
            a=1500000, b=2500000, event_id="later-visit")]
        m = f.score()
        self.assertEqual((m["crossCameraTP"], m["crossCameraFP"], m["crossCameraFN"]), (3, 0, 0))

    def test_long_gt_travel_is_retained_as_false_negative(self):
        f = Fixture(self.fixture.root, windows=64)
        f.person(0, "UAV1", "A", 1, 7)
        f.person(63, "UAV2", "A", False, 9)
        f.manifest["transitions"] = [travel_event(b=31500000)]
        m = f.score()
        self.assertEqual((m["eligibleTransitions"], m["crossCameraFN"], m["crossCameraRecall"]),
                         (1, 1, 0))
        self.assertEqual((m["Ngt"], m["Npred"]), (2, 1))

    def test_invalid_transition_cli_preserves_existing_output(self):
        f = self.transition()
        f.manifest["transitions"].append(travel_event(event_id="another-event"))
        path = f.write()
        output = f.root / "score.json"
        original = b"preserve existing score\n"
        output.write_bytes(original)
        completed = subprocess.run(
            [sys.executable, "-m", "Experiments.UAV.person_tracking_metrics", "--received",
             str(f.root), "--evaluation-manifest", str(path), "--output", str(output)],
            cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("reuses a source exit or target entry", completed.stderr)
        self.assertEqual(output.read_bytes(), original)

    def test_dispatcher_rejects_invalid_transition_before_received_reads(self):
        f = self.transition()
        f.manifest["transitions"].append(travel_event(event_id="another-event"))
        path = f.write()
        with self.assertRaisesRegex(ValueError, "reuses a source exit or target entry"):
            evaluate_tracking_run(f.root / "absent", path, schema="uav-person-tracking-result-v1")

    def test_explicit_handoff_and_missing_source(self):
        f = self.transition()
        m = f.score()
        self.assertEqual((m["crossCameraTP"], m["crossCameraFP"], m["crossCameraFN"]), (1, 0, 0))
        (f.root / "window-0" / "presentation.json").unlink()
        m = f.score(missing=(0,))
        self.assertEqual((m["crossCameraTP"], m["crossCameraFP"], m["crossCameraFN"]), (0, 1, 1))

    def test_wrong_endpoint_global_id_and_no_link(self):
        f = self.transition()
        f.results[1]["frames"][1]["tracks"][0]["globalId"] = 2
        m = f.score()
        self.assertEqual((m["crossCameraTP"], m["crossCameraFP"], m["crossCameraFN"]), (0, 1, 1))
        track = f.results[1]["frames"][1]["tracks"][0]
        track["associationOrigin"] = None
        track["associationStatus"] = "NO_CANDIDATE"
        m = f.score()
        self.assertEqual((m["crossCameraPrecision"], m["crossCameraRecall"], m["crossCameraFN"]), (0, 0, 1))

    def test_coview_is_separate(self):
        f = self.transition(active=True)
        m = f.score()
        self.assertEqual((len(m["coViewLinks"]), m["crossCameraTP"], m["crossCameraFN"]), (1, 0, 1))

    def test_duplicate_global_identity_and_wrong_scope_rejected(self):
        f = self.fixture
        f.person(0, "UAV1", "A", 1, 1)
        f.person(0, "UAV1", "B", 1, 2, BOX_B)
        with self.assertRaisesRegex(ValueError, "duplicate global"):
            f.score()
        f.results[0]["frames"][0]["tracks"][1]["globalId"] = 2
        f.results[0]["missionId"] = "wrong"
        with self.assertRaisesRegex(ValueError, "scope"):
            f.score()

    def test_projection_digest_mirror_and_explicit_dispatch(self):
        f = self.transition()
        path = f.write()
        self.assertEqual(evaluate_tracking_run(f.root, path, schema="uav-person-tracking-result-v1")["IDF1"], 1)
        with self.assertRaises(ResultError):
            evaluate_tracking_run(f.root, path, schema="legacy")
        projection_path = f.root / "window-1" / "presentation.json"
        projection = json.loads(projection_path.read_text())
        projection["resultJson"] += " "
        projection_path.write_text(json.dumps(projection))
        with self.assertRaisesRegex(ValueError, "digest"):
            evaluate_person_tracking(f.root, path)
        path = f.write()
        projection = json.loads(projection_path.read_text())
        result = json.loads(projection["resultJson"])
        result["associationEvents"][0]["origin"]["ptsUs"] = 1
        projection["resultJson"] = json.dumps(result)
        projection["resultDigest"] = "sha256:" + hashlib.sha256(projection["resultJson"].encode()).hexdigest()
        projection_path.write_text(json.dumps(projection))
        with self.assertRaisesRegex(ValueError, "mirror"):
            evaluate_person_tracking(f.root, path)

    def test_detection_matching_prioritizes_cardinality(self):
        f = self.fixture
        for identity, x in enumerate((3, 6, 9), 1):
            f.person(0, "UAV1", str(identity), identity, identity, [x, 0, x + 10, 10])
        # Two exact matches have IoU sum2; three eligible matches have sum21/13.
        f.results[0]["frames"][0]["tracks"][2]["box"] = [0, 0, 10, 10]
        self.assertEqual(f.score()["detectionTP"], 3)

    def test_repeated_link_uses_first_association_only(self):
        f = Fixture(self.fixture.root, windows=3)
        self.fixture = f
        origin = {"localKey": {"cameraId": "UAV1", "sourceId": "source-UAV1", "sourceEpoch": 1,
                               "trackerGeneration": 1, "byteTrackId": 7}, "ptsUs": 0, "active": False}
        f.person(0, "UAV1", "A", 1, 7)
        f.person(1, "UAV2", "A", 1, 9, origin=origin)
        f.person(2, "UAV2", "A", 1, 9, origin=origin)
        f.manifest["transitions"] = [{"eventId": "event", "globalGtId": "A", "sourceCamera": "UAV1",
                                      "targetCamera": "UAV2", "sourceLastVisiblePts": 0,
                                      "targetFirstVisiblePts": 500000, "nonOverlap": True}]
        m = f.score()
        self.assertEqual((m["crossCameraTP"], m["crossCameraFP"], len(m["links"])), (1, 0, 1))
        self.assertEqual(m["links"][0]["targetPts"], 500000)

    def test_qualification_gates_accept_literal_baseline_and_reject_each_defect(self):
        # Fabricated gate inputs are an offline oracle, never actual qualification evidence.
        scope = {"runId": "r", "missionId": "m", "profileDigest": "p", "computationDigest": "c",
                 "datasetOrigin": "real-captured",
                 "sourceManifestDigest": "s"}
        metrics = {**scope, "schema": "uav-person-metrics-v1", "durationUs": 60000000,
                   "gtIdentityCount": 3, "eligibleTransitions": 20, "nonOverlapTransitions": 5,
                   "Ngt": 60, "split": {"name": "evaluation", "frameDisjoint": True,
                                        "sharedActorsDisclosed": True},
                   "plannedWindows": 120, "receivedWindows": 120, "plannedFrames": 360,
                   "receivedFrames": 360, "missingWindows": [], "IDF1": .8,
                   "crossCameraPrecision": .95, "crossCameraRecall": .8,
                   "devices": [{"requestedDevice": "cpu", "actualDetectorDevice": "cpu",
                                "actualReidDevice": "cpu", "reidInferenceBatches": 1}] * 120}
        receipt = {**scope, "schema": "uav-person-run-receipt-v1", "dataQualified": True,
                   "installedClosureVerified": True, "synthetic": False,
                   "completeFrameTimingRecords": 360, "completeWindowTimingRecords": 120,
                   "timingQualified": True, "resourceMeasurementsComplete": True,
                   "elapsedSeconds": 900, "remainingOwnedProcesses": 0, "cleanupSeconds": 10,
                   "cleanupErrors": [],
                   "terminal": True, "failedWindows": 0}
        self.assertTrue(validate_person_run(receipt, metrics)["qualified"])
        rendered_receipt = {**receipt, "datasetOrigin": "synthetic-rendered"}
        rendered_metrics = {**metrics, "datasetOrigin": "synthetic-rendered"}
        self.assertTrue(validate_person_run(rendered_receipt, rendered_metrics)["qualified"])
        self.assertIn('closure', validate_person_run(
            {**rendered_receipt, 'synthetic': True}, rendered_metrics)['failedGates'])
        for bad_origin in (None, 'unspecified', 'synthetic-rendered'):
            with self.subTest(mismatchedOrigin=bad_origin):
                self.assertIn('datasetOrigin', validate_person_run(
                    {**receipt, 'datasetOrigin': bad_origin}, metrics)['failedGates'])
        self.assertIn('datasetOrigin', validate_person_run(
            {k: v for k, v in receipt.items() if k != 'datasetOrigin'},
            {k: v for k, v in metrics.items() if k != 'datasetOrigin'})['failedGates'])
        receipt_mutations = [("schema", "wrong", "schema"), ("missionId", "wrong", "scope"),
                             ("dataQualified", False, "data"), ("synthetic", True, "closure"),
                             ("installedClosureVerified", False, "closure"),
                             ("completeFrameTimingRecords", 359, "timing"),
                             ("resourceMeasurementsComplete", False, "resources"),
                             ("elapsedSeconds", 900.01, "deadline"),
                             ("remainingOwnedProcesses", 1, "cleanup"), ("cleanupSeconds", 10.01, "cleanup"),
                             ("cleanupErrors", ["stop failed"], "cleanup"),
                             ("terminal", False, "terminal"), ("failedWindows", 1, "terminal")]
        for field, value, gate in receipt_mutations:
            with self.subTest(receipt=field):
                changed = {**receipt, field: value}
                result = validate_person_run(changed, metrics)
                self.assertFalse(result["qualified"])
                self.assertIn(gate, result["failedGates"])
        metric_mutations = [("gtIdentityCount", 2, "data"), ("eligibleTransitions", 19, "data"),
                            ("nonOverlapTransitions", 4, "data"), ("Ngt", 0, "data"),
                            ("split", {**metrics["split"], "frameDisjoint": False}, "split"),
                            ("receivedWindows", 119, "windows"), ("receivedFrames", 359, "windows"),
                            ("IDF1", .799, "accuracy"), ("crossCameraPrecision", .949, "accuracy"),
                            ("crossCameraRecall", .799, "accuracy"),
                            ("devices", [{"requestedDevice": "cpu", "actualDetectorDevice": "cpu",
                                          "actualReidDevice": "cuda", "reidInferenceBatches": 1}] * 120, "cpu")]
        for field, value, gate in metric_mutations:
            with self.subTest(metric=field):
                result = validate_person_run(receipt, {**metrics, field: value})
                self.assertFalse(result["qualified"])
                self.assertIn(gate, result["failedGates"])

    def test_optional_receipt_binds_scope_and_keeps_unqualified_runtime_explicit(self):
        f = self.fixture
        f.person(0, "UAV1", "A", 1, 1)
        manifest = f.write()
        receipt = {"schema": "uav-person-run-receipt-v1", "runId": "r", "missionId": "m",
                   "profileDigest": "p", "computationDigest": "c", "sourceManifestDigest": "s",
                   "timing": {"summary": {"e2eMs": {"count": 3, "p50": 10}}}}
        path = f.root / "receipt.json"
        path.write_text(json.dumps(receipt))
        metrics = evaluate_person_tracking(f.root, manifest, run_receipt=path)
        self.assertFalse(metrics["qualification"]["qualified"])
        self.assertIn("resources", metrics["qualification"]["failedGates"])
        self.assertEqual(metrics["runtimeTimingSummary"], receipt["timing"]["summary"])
        receipt["missionId"] = "wrong"
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "receipt does not bind"):
            evaluate_person_tracking(f.root, manifest, run_receipt=path)

    def test_empty_gt_does_not_qualify(self):
        m = self.fixture.score()
        self.assertEqual((m["IDF1"], m["crossCameraPrecision"], m["crossCameraRecall"]), (0, 0, 0))
        self.assertIn("data", validate_person_run({}, m)["failedGates"])


class PersonDataAdmissionTests(unittest.TestCase):
    """Offline evidence fixtures never constitute real model/run qualification."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scope = {"runId": "r", "missionId": "m", "sourceManifestDigest": "sha256:" + "1" * 64,
                      "profileDigest": "sha256:" + "2" * 64, "computationDigest": "c",
                      "datasetOrigin": "synthetic-rendered"}
        self.evaluation_digest = "sha256:" + "3" * 64
        self.metrics = {**self.scope, "schema": "uav-person-metrics-v1",
                        "devices": [{"requestedDevice": "cpu", "actualDetectorDevice": "cpu",
                                     "actualReidDevice": "cpu", "reidInferenceBatches": 1}] * 120}
        self.receipt = {**self.scope, "schema": "uav-person-run-receipt-v1",
                        "dataQualified": False, "synthetic": None, "installedClosureVerified": True}
        self.evidence = {"status": "VERIFIED_ACTUAL_360_SOURCE_INPUTS_ONLY",
                         "sourceDigest": self.scope["sourceManifestDigest"],
                         "evaluatorDigest": self.evaluation_digest,
                         "frames": [{"camera": camera, "window": window,
                                     "path": str(self.root / camera / f"window-{window}.jsonl"),
                                     "digest": "sha256:" + "4" * 64, "jpegBytes": 100}
                                    for camera in CAMERAS for window in range(120)]}
        self.evidence_path = self.root / "audit.json"
        self.admission = {"schema": "uav-person-data-admission-v1", "dataQualified": True,
                          "sourceManifestDigest": self.scope["sourceManifestDigest"],
                          "profileDigest": self.scope["profileDigest"],
                          "datasetOrigin": self.scope["datasetOrigin"],
                          "evaluationManifestDigest": self.evaluation_digest,
                          "evidencePath": "audit.json"}
        self.path = self.root / "admission.json"
        self.write()

    def write(self):
        self.evidence_path.write_text(json.dumps(self.evidence))
        self.admission["evidenceDigest"] = "sha256:" + hashlib.sha256(
            self.evidence_path.read_bytes()).hexdigest()
        self.path.write_text(json.dumps(self.admission))

    def assess(self, receipt=None, metrics=None):
        return _assess_admitted_run(self.receipt if receipt is None else receipt,
                                    self.metrics if metrics is None else metrics,
                                    self.evaluation_digest, self.path)

    def test_bound_admission_preserves_original_and_all_other_gates(self):
        original = copy.deepcopy(self.receipt)
        assessed, evidence = self.assess()
        self.assertEqual(self.receipt, original)
        self.assertEqual(assessed, dict(original, dataQualified=True, synthetic=False))
        self.assertTrue(evidence["dataQualified"])
        self.assertFalse(evidence["synthetic"])
        self.assertIn("accuracy", validate_person_run(assessed, self.metrics)["failedGates"])
        self.assertIn("windows", validate_person_run(assessed, self.metrics)["failedGates"])
        self.assertEqual(evidence["dataAdmissionDigest"], "sha256:" + hashlib.sha256(
            self.path.read_bytes()).hexdigest())

    def test_each_admission_binding_and_approval_is_required(self):
        original = copy.deepcopy(self.admission)
        for key, value in (("schema", "wrong"), ("dataQualified", False), ("dataQualified", 1),
                           ("sourceManifestDigest", "sha256:" + "9" * 64),
                           ("profileDigest", "sha256:" + "9" * 64),
                           ("evaluationManifestDigest", "sha256:" + "9" * 64),
                           ("datasetOrigin", "real-captured"), ("datasetOrigin", "unspecified"),
                           ("evidenceDigest", "sha256:" + "9" * 64), ("evidencePath", "")):
            with self.subTest(field=key, value=value):
                admission = dict(original, **{key: value})
                self.path.write_text(json.dumps(admission))
                with self.assertRaises(ValueError):
                    self.assess()
        self.path.write_text(json.dumps(original))
        for key in self.scope:
            if key not in ("sourceManifestDigest", "profileDigest", "datasetOrigin"):
                continue
            with self.subTest(receiptField=key):
                with self.assertRaises(ValueError):
                    self.assess(receipt=dict(self.receipt, **{key: "wrong"}))

    def test_changed_or_incomplete_audit_cannot_admit_data(self):
        self.evidence_path.write_text(self.evidence_path.read_text() + " ")
        with self.assertRaisesRegex(ValueError, "audit mismatch"):
            self.assess()
        original = copy.deepcopy(self.evidence)
        for key, value in (("status", "NOT_VERIFIED"), ("sourceDigest", "wrong"),
                           ("evaluatorDigest", "wrong"), ("frames", []), ("frames", [None] * 359)):
            with self.subTest(field=key):
                self.evidence = dict(original, **{key: value})
                self.write()  # Even a new digest cannot repair an unqualified audit.
                with self.assertRaisesRegex(ValueError, "audit mismatch"):
                    self.assess()

    def test_360_invalid_duplicate_or_missing_frame_records_cannot_admit(self):
        valid = copy.deepcopy(self.evidence["frames"])
        invalid = [([None] * 360, "invalid"), ([valid[0]] * 360, "duplicate")]
        for updates in ({"camera": "UAV4"}, {"window": 120}, {"window": True},
                        {"path": "relative"}, {"digest": "wrong"}, {"jpegBytes": 0},
                        {"jpegBytes": True}, {"jpegBytes": 8388609}):
            frames = copy.deepcopy(valid)
            frames[-1].update(updates)
            invalid.append((frames, "invalid"))
        missing = copy.deepcopy(valid)
        del missing[-1]["path"]
        invalid.append((missing, "invalid"))
        duplicate = copy.deepcopy(valid)
        duplicate[-1] = duplicate[0]
        invalid.append((duplicate, "duplicate"))
        for frames, reason in invalid:
            with self.subTest(reason=reason, last=frames[-1]):
                self.evidence["frames"] = frames
                self.write()
                with self.assertRaisesRegex(ValueError, reason):
                    self.assess()

    def test_mock_or_unverified_closure_cannot_be_reclassified(self):
        for updates in ({"synthetic": True}, {"installedClosureVerified": False},
                        {"installedClosureVerified": None}):
            with self.subTest(updates=updates):
                receipt = dict(self.receipt, **updates)
                assessed, _ = self.assess(receipt=receipt)
                self.assertIs(assessed["synthetic"], receipt["synthetic"])
                self.assertIn("closure", validate_person_run(assessed, self.metrics)["failedGates"])

    def test_incomplete_or_wrong_device_evidence_stays_unclassified(self):
        for devices in ([], self.metrics["devices"][:-1],
                        [dict(self.metrics["devices"][0], actualDetectorDevice="cuda:0")] * 120,
                        [dict(self.metrics["devices"][0], reidInferenceBatches=0,
                              actualReidDevice="not-run")] * 120):
            with self.subTest(count=len(devices), device=devices[:1]):
                assessed, _ = self.assess(metrics=dict(self.metrics, devices=devices))
                self.assertIsNone(assessed["synthetic"])

    def test_admission_requires_runtime_receipt_before_reading_other_inputs(self):
        with self.assertRaisesRegex(ValueError, "requires an execution receipt"):
            evaluate_person_tracking(self.root / "absent", self.root / "absent-evaluator",
                                     data_admission=self.path)

    def test_public_api_cli_and_raw_receipt_are_consistent(self):
        fixture = Fixture(self.root)
        fixture.context["profileDigest"] = self.scope["profileDigest"]
        fixture.manifest["sourceManifestDigest"] = self.scope["sourceManifestDigest"]
        fixture.manifest["datasetOrigin"] = "synthetic-rendered"
        for result in fixture.results:
            result["profileDigest"] = self.scope["profileDigest"]
        evaluation = fixture.write()
        self.evaluation_digest = "sha256:" + hashlib.sha256(evaluation.read_bytes()).hexdigest()
        self.admission["evaluationManifestDigest"] = self.evaluation_digest
        self.evidence["evaluatorDigest"] = self.evaluation_digest
        self.write()
        receipt_path = self.root / "receipt.json"
        receipt_path.write_text(json.dumps(self.receipt))
        original = receipt_path.read_bytes()
        plain = evaluate_person_tracking(self.root, evaluation, run_receipt=receipt_path)
        self.assertNotIn("qualificationEvidence", plain)
        result = evaluate_person_tracking(self.root, evaluation, run_receipt=receipt_path,
                                          data_admission=self.path)
        self.assertTrue(result["qualificationEvidence"]["dataQualified"])
        self.assertIsNone(result["qualificationEvidence"]["synthetic"])
        self.assertFalse(result["qualification"]["qualified"])
        self.assertIn("windows", result["qualification"]["failedGates"])
        self.assertEqual(receipt_path.read_bytes(), original)
        output = self.root / "metrics.json"
        child = subprocess.run([sys.executable, "-m", "Experiments.UAV.person_tracking_metrics",
                                "--received", str(self.root), "--evaluation-manifest", str(evaluation),
                                "--run-receipt", str(receipt_path), "--data-admission", str(self.path),
                                "--output", str(output)], capture_output=True, text=True, timeout=30)
        self.assertEqual(child.returncode, 0, child.stderr)
        self.assertEqual(json.loads(output.read_text()), result)
        self.assertEqual(receipt_path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
