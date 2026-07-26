import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


def load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, REPO / relative)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


runner = load("spec132_runner", "Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py")
analyzer = load("spec132_analyzer", "Experiments/analyze_svs_pubsub_commit_latency.py")
builder = load("spec132_builder", "Experiments/build_svs_pubsub_commit_bench.py")


def fake_authority(tmp_path: Path) -> Path:
    subjects = []
    for subject, commit, latest in builder.SUBJECTS:
        binary = tmp_path / f"{subject}.bin"
        library = tmp_path / f"{subject}.so"
        binary.write_bytes(subject.encode())
        library.write_bytes(commit.encode())
        subjects.append({"subject": subject, "baseCommit": commit, "binary": str(binary),
                         "binarySha256": runner.sha256_file(binary), "library": str(library),
                         "librarySha256": runner.sha256_file(library), "worktree": str(tmp_path),
                         "driverSha256": "x", "publishApi": "publishAsync" if latest else "publish"})
    authority = tmp_path / "subjects.json"
    authority.write_text(json.dumps({"schemaVersion": "spec132-subjects-v1",
                         "canonicalPatchSha256": "a" * 64, "subjects": subjects}))
    return authority


def event(cell, peer, kind, logical, seq, phase, timestamp, details=None):
    return {"schemaVersion": "spec132-event-v1", "cellId": cell, "peerId": peer,
            "event": kind, "logicalId": logical, "svsSeqNo": seq, "phase": phase,
            "monotonicRawNs": timestamp, "details": details if details is not None else {}}


class Spec132ContractTests(unittest.TestCase):
    def test_subjects_are_capability_named_and_exactly_pinned(self):
        self.assertEqual(builder.SUBJECTS, (
            ("sync-publish-no-internal-parallelism",
             "a9944019f76791773604999f00128057b9534ace", 0),
            ("async-publish-parallel-sync",
             "6bb34545b4f89f1f6c265a68c18f1a40ade413eb", 1),
        ))
        patch = builder.canonical_patch_bytes().decode()
        self.assertIn("107400", patch)
        self.assertIn("107100", patch)
        self.assertIn("1.74.0", patch)
        self.assertIn("1.71.0", patch)
        self.assertNotIn("NDNSF", patch)

    def test_schedule_is_exactly_ten_baseline_first_matched_cells(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = runner.make_manifest("c", fake_authority(Path(directory)))
        self.assertEqual(manifest["schemaVersion"], "spec132-campaign-v1")
        self.assertEqual(len(manifest["cells"]), 10)
        self.assertEqual([c["ordinal"] for c in manifest["cells"]], list(range(1, 11)))
        self.assertEqual([c["ratePpsPerPeer"] for c in manifest["cells"][:5]],
                         [200, 400, 600, 800, 1000])
        self.assertEqual([c["ratePpsPerPeer"] for c in manifest["cells"][5:]],
                         [200, 400, 600, 800, 1000])
        self.assertEqual([c["subject"] for c in manifest["cells"][:5]],
                         [runner.SUBJECTS[0]] * 5)
        self.assertEqual([c["subject"] for c in manifest["cells"][5:]],
                         [runner.SUBJECTS[1]] * 5)
        self.assertTrue(all(c["attempt"] == 1 and c["aggregateTargetPps"] ==
                            2 * c["ratePpsPerPeer"] for c in manifest["cells"]))

    def test_manifest_validator_rejects_hidden_rerun_or_grid_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = runner.make_manifest("c", fake_authority(Path(directory)))
        analyzer.validate_manifest(manifest)
        bad = json.loads(json.dumps(manifest))
        bad["cells"][0]["attempt"] = 2
        with self.assertRaises(ValueError):
            analyzer.validate_manifest(bad)
        bad = json.loads(json.dumps(manifest))
        bad["cells"][5]["ratePpsPerPeer"] = 201
        with self.assertRaises(ValueError):
            analyzer.validate_manifest(bad)

    def test_driver_uses_bidirectional_direct_main_thread_calls(self):
        source = (REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-pubsub-bench.cpp").read_text()
        self.assertIn("SPEC132_LATEST", source)
        self.assertIn("m_pubsub->subscribe", source)
        self.assertIn("std::thread faceThread", source)
        self.assertIn("publishLoop(start, rawStart);", source)
        self.assertIn("m_pubsub->publish(name", source)
        self.assertIn("m_pubsub->publishAsync(name", source)
        self.assertIn("setParallelSyncProcessing(true, 4, 4096)", source)
        self.assertIn("--peer-id", source)
        self.assertIn("--remote-peer-id", source)
        self.assertNotIn("std::thread pacer", source)
        self.assertNotIn("eventfd", source)
        self.assertNotIn("PublishJob", source)
        self.assertNotIn("enqueuePublication", source)
        self.assertNotIn("boost::asio::post", source)
        self.assertNotIn("m_scheduler.schedule", source)
        self.assertNotIn("ndn-service-framework", source.lower())
        self.assertNotIn("ndnsf", source.lower())

    def test_direction_analysis_joins_sender_returns_to_remote_delivery(self):
        with tempfile.TemporaryDirectory() as directory:
            cell = Path(directory) / "cell"
            cell.mkdir()
            a = []
            for logical in (1, 2, 3):
                a += [event("c", "peer-a", "deadline", logical, 0, "measured",
                            logical * 1_000_000, {"actualWakeNs": logical * 1_000_000 + 5}),
                      event("c", "peer-a", "api-enter", logical, 0, "measured",
                            logical * 1_000_000 + 10),
                      event("c", "peer-a", "api-return", logical, logical, "measured",
                            logical * 1_000_000 + 20)]
            b = [event("c", "peer-b", "delivery", 2, 2, "measured", 5_000_000,
                       {"senderPeer": "peer-a", "scheduledNs": 2_000_000,
                        "payloadValid": True}),
                 event("c", "peer-b", "delivery", 1, 1, "measured", 6_000_000,
                       {"senderPeer": "peer-a", "scheduledNs": 1_000_000,
                        "payloadValid": True}),
                 event("c", "peer-b", "duplicate", 1, 1, "measured", 7_000_000,
                       {"senderPeer": "peer-a", "scheduledNs": 1_000_000,
                        "payloadValid": True})]
            (cell / "peer-a.jsonl").write_text("\n".join(map(json.dumps, a)) + "\n")
            (cell / "peer-b.jsonl").write_text("\n".join(map(json.dumps, b)) + "\n")
            config = {"cellId": "c", "subject": runner.SUBJECTS[0],
                      "ratePpsPerPeer": 1, "aggregateTargetPps": 2,
                      "repetition": 1, "measureSeconds": 3, "drainSeconds": 2}
            result = analyzer.summarize_cell(cell, config)
        direction = result["directions"]["peer-a-to-peer-b"]
        self.assertEqual(direction["apiReturned"], 3)
        self.assertEqual(direction["delivered"], 2)
        self.assertEqual(direction["missing"], 1)
        self.assertEqual(direction["deliveryReturnRatio"], 2 / 3)
        self.assertEqual(direction["duplicates"], 1)
        self.assertEqual(direction["reorderTransitions"], 1)
        self.assertEqual(direction["deadlineCappedDelayMs"]["p99"], 2000)
        self.assertFalse(result["sustainable"])

    def test_terminal_classifier_distinguishes_subject_and_infrastructure_failure(self):
        self.assertEqual(runner.classify_terminal("", {"peer-a": 0, "peer-b": 0}),
                         "COMPLETE")
        self.assertEqual(runner.classify_terminal("", {"peer-a": 134, "peer-b": 0}),
                         "SUBJECT_FAILURE")
        self.assertEqual(runner.classify_terminal("NFD socket not ready", {}),
                         "INFRA_FAILURE")

    def test_nearest_rank_contract(self):
        self.assertEqual(analyzer.nearest_rank([1, 2, 3, 4], 0.5), 2)
        self.assertEqual(analyzer.nearest_rank([1, 2, 3, 4], 0.99), 4)
        self.assertIsNone(analyzer.nearest_rank([], 0.5))


if __name__ == "__main__":
    unittest.main()

