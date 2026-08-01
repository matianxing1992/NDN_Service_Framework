#!/usr/bin/env python3

import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


RUNNER = load_module(
    "spec167_runner",
    ROOT / "Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py",
)
ANALYZER = load_module(
    "spec167_analyzer",
    ROOT / "Experiments/analyze_spec167_itiger_repo.py",
)


class Spec167ItigerArtifactRunnerTest(unittest.TestCase):
    def test_schedule_is_complete_unique_and_deterministic(self):
        first = RUNNER.build_schedule()
        second = RUNNER.build_schedule()
        self.assertEqual(first, second)
        self.assertEqual(60, len(first))
        self.assertEqual(60, len({row["runId"] for row in first}))
        self.assertEqual(10, sum(row["warmup"] for row in first))
        self.assertEqual(50, sum(not row["warmup"] for row in first))

    def test_path_isolation_rejects_shared_and_equal_paths(self):
        with self.assertRaisesRegex(ValueError, "rank-local"):
            RUNNER.validate_path_isolation(
                Path("/project/tma1/evidence"), Path("/project/tma1/payload")
            )
        with self.assertRaisesRegex(ValueError, "rank-local"):
            RUNNER.validate_path_isolation(
                Path("/tmp/evidence"), Path("/home/tma1/payload")
            )
        with tempfile.TemporaryDirectory() as temporary:
            value = Path(temporary)
            with self.assertRaisesRegex(ValueError, "different"):
                RUNNER.validate_path_isolation(value, value)

    def test_prepare_places_payload_only_in_local_data_dir(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            coord = root / "coord"
            data = root / "rank-local"
            coord.mkdir()
            digest = RUNNER.prepare_payload(coord, data, 64 << 20)
            self.assertEqual(64 << 20, (data / "payload.bin").stat().st_size)
            self.assertFalse((coord / "payload.bin").exists())
            fixture = json.loads((coord / "raw-fixture.json").read_text())
            self.assertEqual(digest, fixture["contentDigest"])
            self.assertFalse((coord / "manifest-private.pem").exists())

    def test_prepare_rejects_stale_coordination_state(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            coord = root / "coord"
            coord.mkdir()
            (coord / "raw-fixture.json").write_text("{}")
            with self.assertRaisesRegex(FileExistsError, "coordination state"):
                RUNNER.prepare_payload(coord, root / "rank-local", 64 << 20)

    def test_warm_reuse_writes_no_duplicate_payload(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            coord = root / "coord"
            data = root / "rank-local"
            coord.mkdir()
            payload = data / "stores/result/payload.bin"
            payload.parent.mkdir(parents=True)
            payload.write_bytes(b"immutable-payload")
            expected = __import__("hashlib").sha256(payload.read_bytes()).hexdigest()
            before = payload.stat().st_size
            self.assertEqual(0, RUNNER.warm_reuse(coord, data, "result", expected))
            result = json.loads((coord / "result.warm.json").read_text())
            self.assertEqual(0, result["duplicatePayloadBytesWritten"])
            self.assertEqual(before, payload.stat().st_size)

    def test_analyzer_accepts_complete_synthetic_ledger(self):
        manifest = RUNNER.create_manifest("spec167-test", 16720260731)
        rows = []
        subject_rate = {
            "physical-network": 900.0,
            "raw-segmented-ndn": 700.0,
            "legacy-exact-packet": 250.0,
            "digest-only": 680.0,
            "signed-manifest": 670.0,
        }
        for scheduled in manifest["schedule"]:
            row = dict(scheduled)
            row.update({
                "status": "PASS",
                "logicalGoodputMbps": subject_rate[row["subject"]],
                "dataPath": "/tmp/spec167/data",
                "storePath": "/tmp/spec167/store",
                "destinationPath": "/tmp/spec167/destination",
            })
            rows.append(row)
        result = ANALYZER.analyze(manifest, rows)
        self.assertEqual("PASS", result["verdict"])
        self.assertEqual(50, result["expectedMeasured"])
        ratio = result["cells"][str(64 << 20)]["ratios"]["signedOverLegacy"]
        self.assertAlmostEqual(670.0 / 250.0, ratio["median"])

    def test_analyzer_fails_missing_duplicate_and_shared_path(self):
        manifest = RUNNER.create_manifest("spec167-test", 16720260731)
        rows = []
        for scheduled in manifest["schedule"][:-1]:
            row = dict(scheduled)
            row.update({
                "status": "PASS",
                "logicalGoodputMbps": 1.0,
                "dataPath": "/project/tma1/not-local",
            })
            rows.append(row)
        rows.append(dict(rows[0]))
        result = ANALYZER.analyze(manifest, rows)
        self.assertEqual("FAIL", result["verdict"])
        self.assertTrue(result["duplicates"])
        self.assertTrue(result["missing"])
        self.assertTrue(result["pathViolations"])

    def test_tcp_ceiling_transfers_over_real_loopback_socket(self):
        import socket
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            coord = Path(temporary)
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
            result = {}
            thread = threading.Thread(
                target=lambda: result.setdefault(
                    "rc", RUNNER.tcp_ceiling_server(
                        coord, "127.0.0.1", port, 10.0
                    )
                )
            )
            thread.start()
            for _ in range(100):
                if (coord / "tcp-server.ready").is_file():
                    break
                __import__("time").sleep(0.01)
            self.assertEqual(
                0, RUNNER.tcp_ceiling_client(
                    coord, "127.0.0.1", port, 0.1, 10.0
                )
            )
            thread.join(10)
            self.assertEqual(0, result.get("rc"))
            measured = json.loads((coord / "tcp-result.json").read_text())
            self.assertEqual("SUCCESS", measured["status"])
            self.assertGreater(measured["logicalBytes"], 0)

    def test_control_bundle_crosses_socket_without_shared_file_polling(self):
        import socket
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            source = root / "source"
            destination = root / "destination"
            source.mkdir()
            (source / "metadata.json").write_text('{"ready":true}\n')
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
            result = {}
            thread = threading.Thread(
                target=lambda: result.setdefault(
                    "rc", RUNNER.control_receive(
                        destination, "127.0.0.1", port, 10.0
                    )
                )
            )
            thread.start()
            self.assertEqual(
                0,
                RUNNER.control_send(
                    source, "127.0.0.1", port, 10.0, ("metadata.json",)
                ),
            )
            thread.join(10)
            self.assertEqual(0, result.get("rc"))
            self.assertEqual(
                (source / "metadata.json").read_bytes(),
                (destination / "metadata.json").read_bytes(),
            )
            with self.assertRaisesRegex(ValueError, "unsafe"):
                RUNNER.control_send(
                    source, "127.0.0.1", port, 0.1, ("../metadata.json",)
                )


if __name__ == "__main__":
    unittest.main()
