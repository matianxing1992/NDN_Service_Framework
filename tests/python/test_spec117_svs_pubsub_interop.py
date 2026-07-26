from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO / "examples/interop/ndn-svs-v3/payload_corpus.py"
MININDN_PATH = REPO / "Experiments/NDN_SVS_PubSub_Interop_Minindn.py"


def load_module():
    spec = importlib.util.spec_from_file_location("spec117_payload_corpus", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {MODULE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_minindn_module():
    spec = importlib.util.spec_from_file_location("spec117_minindn", MININDN_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {MININDN_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PayloadCorpusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module()

    def test_corpus_is_deterministic_binary_safe_and_forces_segmentation(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            one = self.module.create_corpus(Path(first))
            two = self.module.create_corpus(Path(second))

            self.assertEqual(one, two)
            self.assertEqual([case["caseId"] for case in one["cases"]],
                             ["text", "binary", "large", "segmented"])
            by_id = {case["caseId"]: case for case in one["cases"]}
            self.assertEqual(by_id["binary"]["length"], 1024)
            binary = Path(first, by_id["binary"]["path"]).read_bytes()
            self.assertIn(b"\x00", binary)
            self.assertIn(b"\x80", binary)
            self.assertIn(b"\xff", binary)
            self.assertEqual(by_id["large"]["length"], 4096)
            self.assertEqual(by_id["segmented"]["length"], 32768)
            self.assertTrue(by_id["segmented"]["requiresSegmentation"])
            self.assertGreater(by_id["segmented"]["length"], 8800)

            for case in one["cases"]:
                payload = Path(first, case["path"]).read_bytes()
                self.assertEqual(len(payload), case["length"])
                self.assertEqual(hashlib.sha256(payload).hexdigest(), case["sha256"])
                self.assertEqual(set(case["names"]), {"cpp", "ndnts"})

    def test_manifest_round_trip_rejects_tampered_payload(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = self.module.create_corpus(Path(root))
            path = Path(root, "manifest.json")
            loaded = self.module.load_manifest(path)
            self.assertEqual(loaded, manifest)
            payload = Path(root, manifest["cases"][0]["path"])
            payload.write_bytes(payload.read_bytes() + b"tamper")
            with self.assertRaisesRegex(ValueError, "payload (length|digest) mismatch"):
                self.module.load_manifest(path)


class PayloadReceiptTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module()

    def _manifest(self, root: Path):
        return self.module.create_corpus(root)

    @staticmethod
    def _valid_events(manifest):
        events = []
        for sender, receiver in (("cpp", "ndnts"), ("ndnts", "cpp")):
            for sequence, case in enumerate(manifest["cases"], 1):
                events.append({
                    "event": "receive",
                    "implementation": "cpp" if receiver == "cpp" else "ndnts-typescript",
                    "direction": f"{sender}-to-{receiver}",
                    "caseId": case["caseId"],
                    "name": case["names"][sender],
                    "sequence": sequence,
                    "length": case["length"],
                    "sha256": case["sha256"],
                    "segments": 2 if case["requiresSegmentation"] else 1,
                    "stage": "payload-check",
                    "reason": "",
                })
        return events

    def test_exact_bilateral_receipts_pass(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = self._manifest(Path(root))
            summary = self.module.classify_receipts(manifest, self._valid_events(manifest))
            self.assertEqual(summary["status"], "SUCCESS")
            self.assertTrue(summary["passed"])
            self.assertEqual(summary["expectedReceiptCount"], 8)
            self.assertEqual(summary["verifiedReceiptCount"], 8)
            self.assertEqual(summary["duplicates"], [])
            self.assertEqual(summary["missing"], [])
            self.assertEqual(summary["mismatches"], [])

    def test_missing_duplicate_and_digest_mismatch_fail_closed(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = self._manifest(Path(root))
            events = self._valid_events(manifest)
            events.pop(0)
            events.append(dict(events[0]))
            events[-1]["sha256"] = "0" * 64
            events.append(dict(events[-1]))
            summary = self.module.classify_receipts(manifest, events)
            self.assertEqual(summary["status"], "INTEROP_INCOMPATIBLE")
            self.assertFalse(summary["passed"])
            self.assertTrue(summary["missing"])
            self.assertTrue(summary["duplicates"])
            self.assertTrue(summary["mismatches"])

    def test_protocol_error_is_classified_without_sync_only_false_pass(self):
        with tempfile.TemporaryDirectory() as root:
            manifest = self._manifest(Path(root))
            events = [{
                "event": "sync-update",
                "implementation": "cpp",
                "direction": "ndnts-to-cpp",
                "stage": "sync",
            }, {
                "event": "error",
                "implementation": "cpp",
                "direction": "ndnts-to-cpp",
                "caseId": "text",
                "stage": "mapping",
                "reason": "mapping timeout",
            }]
            summary = self.module.classify_receipts(manifest, events)
            self.assertEqual(summary["status"], "INTEROP_INCOMPATIBLE")
            self.assertEqual(summary["errors"][0]["stage"], "mapping")
            self.assertEqual(summary["verifiedReceiptCount"], 0)


class MiniNdnAdmissionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_minindn_module()

    def test_negative_standalone_produces_stop_receipt_without_minindn(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            standalone = root_path / "standalone"
            standalone.mkdir()
            summary = {
                "schemaVersion": "spec117-payload-summary-v1",
                "cellId": "standalone",
                "status": "INTEROP_INCOMPATIBLE",
                "passed": False,
                "expectedReceiptCount": 8,
                "verifiedReceiptCount": 0,
                "errors": [{"stage": "mapping", "reason": "timeout"}],
            }
            (standalone / "summary.json").write_text(json.dumps(summary))
            output = root_path / "matrix"
            rc = self.module.main([
                "--standalone-result", str(standalone),
                "--output", str(output),
                "--loss", "both",
            ])
            receipt = json.loads((output / "summary.json").read_text())
            self.assertEqual(rc, 1)
            self.assertEqual(receipt["status"], "NOT_ADMITTED")
            self.assertFalse(receipt["miniNdnLaunched"])
            self.assertEqual([cell["lossPercent"] for cell in receipt["cells"]], [0, 5])
            self.assertEqual(receipt["blockedBy"]["sha256"],
                             hashlib.sha256((standalone / "summary.json").read_bytes()).hexdigest())

    def test_launcher_only_injects_inter_host_sync_prefix_route(self):
        source = MININDN_PATH.read_text(encoding="utf-8")
        route_lines = [line for line in source.splitlines() if "nfdc route add" in line]
        self.assertEqual(len(route_lines), 1)
        self.assertIn("sync_prefix", route_lines[0])
        self.assertNotIn("payload", route_lines[0])
        self.assertIn("rib-after-registration", source)


if __name__ == "__main__":
    unittest.main()
