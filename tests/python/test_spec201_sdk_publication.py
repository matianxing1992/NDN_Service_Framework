"""SDK admission checks; mocked Repo is not native packet qualification."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.adapters.qwen import repo_registration
from ndnsf_distributed_inference.naming import canonical_name_uri, parse_producer_data_name
from ndn.encoding import Component


class QwenProducerAdmissionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        stages = []
        for index in range(3):
            payload = bytes([index + 1]) * (index + 1)
            path = self.root / f"stage-{index}.bin"
            path.write_bytes(payload)
            stages.append({
                "role": f"stage-{index}", "stageIndex": index,
                "layerRange": {"start": index, "endExclusive": index + 1},
                "path": str(path), "bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            })
        self.manifest = self.root / "manifest.json"
        self.manifest.write_text(json.dumps({
            "modelDigest": "sha256:" + "d" * 64,
            "revision": "fixed-revision", "stages": stages,
        }), encoding="utf-8")
        self.output = self.root / "registration.json"
        self.backend = SimpleNamespace(last_receipts=())

    def publish(self, publisher, alias=""):
        return repo_registration.publish_qwen_stage_manifest(
            backend=self.backend, stage_manifest_path=self.manifest,
            output_path=self.output, publisher_identity=publisher,
            object_prefix="/unused", deadline_ms=2**62,
            canonical_model_name="Qwen/model",
            canonical_graph_digest="sha256:" + "e" * 64,
            canonical_profile="qwen-profile", canonical_publisher=alias)

    def test_foreign_or_root_producer_rejected_before_repo_construction(self):
        for publisher, alias in (("/example/user", "/ndnsf-di"),
                                 ("/example/user", "/example/user-other"),
                                 ("/", "")):
            with self.subTest(publisher=publisher, alias=alias):
                with mock.patch.object(repo_registration, "ArtifactRepositoryApi") as repo:
                    with self.assertRaises(ValueError):
                        self.publish(publisher, alias)
                    repo.assert_not_called()
                self.assertFalse(self.output.exists())

    def test_default_and_equivalent_alias_keep_complete_actual_producer(self):
        for publisher, alias in (("/example/user", ""),
                                 ("/example/user", "/8=example/8=user"),
                                 ("/example/NDNSF/50=%00%01/%FF", "")):
            with self.subTest(publisher=publisher, alias=alias):
                self.output.unlink(missing_ok=True)
                calls = []
                backend = self.backend

                class Repo:
                    def __init__(self, injected, **kwargs):
                        self.publisher = kwargs["publisher_identity"]

                    def publish_file(self, path, **kwargs):
                        calls.append((self.publisher, kwargs["name"]))
                        backend.last_receipts = ({"dataName": kwargs["name"]},)
                        return SimpleNamespace(
                            achieved_replicas=1, requested_replicas=1,
                            operation_id="upload",
                            reference=SimpleNamespace(to_dict=lambda: {}))

                with mock.patch.object(repo_registration, "ArtifactRepositoryApi", Repo):
                    with contextlib.redirect_stdout(io.StringIO()):
                        result = self.publish(publisher, alias)
                self.assertEqual(len(calls), 3)
                for stage_index, (actual, name) in enumerate(calls):
                    self.assertEqual(actual, publisher)
                    content = parse_producer_data_name(name, publisher)
                    self.assertIsNotNone(content)
                    self.assertEqual(content.category, "model")
                    self.assertEqual(content.name_space, "NDNSF-DI")
                    self.assertEqual(len(content.suffix), 10)
                    values = [bytes(Component.get_value(c)) for c in content.suffix]
                    self.assertEqual(values[0], b"name=Qwen/model")
                    self.assertEqual(values[2:5], [b"artifact", b"stage",
                                                  f"layers={stage_index}-{stage_index}".encode()])
                    self.assertEqual(values[6:8], [b"file", b"name=model.onnx"])
                    for offset in (1, 5, 8, 9):
                        self.assertEqual(Component.get_type(content.suffix[offset]), 8)
                        self.assertEqual(len(values[offset]), 32)
                    self.assertEqual(values[9], hashlib.sha256(
                        bytes([stage_index + 1]) * (stage_index + 1)).digest())
                self.assertEqual(result["publisher"], publisher)
                self.assertEqual(json.loads(self.output.read_text())["publisher"], publisher)
                self.output.unlink()


if __name__ == "__main__":
    unittest.main()
