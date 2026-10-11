"""Execute the real Controller file-store caller with original-packet transport."""
import ast
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from ndn.encoding import Component, Name
from ndnsf import make_content_data_prefix
from ndnsf_distributed_inference.app_sdk.canonical_artifacts import canonical_model_object_name
from ndnsf_distributed_inference.naming import (
    NamingDigestField, canonical_name_uri, compute_naming_digest,
    make_naming_digest_component,
)


class OriginalRepoCaller(unittest.TestCase):
    def test_actual_caller_submits_semantic_originals_and_complete_index(self):
        path = Path(__file__).resolve().parents[2] / "examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py"
        tree = ast.parse(path.read_text())
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                        and n.name == "_store_original_repo_file")
        calls = []
        lifecycle = []
        fail_index_start = [False]

        class Producer:
            segment_count = 2
            file_size = 9

            def __init__(self, prefix, source, **kwargs):
                self.versioned_name = prefix + "/v=7"
                self.prefix = prefix
                calls.append((prefix, source, kwargs))

            def start(self):
                lifecycle.append(("start", self.prefix))
                if fail_index_start[0] and b"name=original-packet-index" in [
                        bytes(Component.get_value(c)) for c in Name.from_str(self.prefix)]:
                    raise RuntimeError("index registration failed")
                return self

            def stop(self):
                lifecycle.append(("stop", self.prefix))

            def packet(self, i):
                return SimpleNamespace(name=self.versioned_name + "/seg=" + str(i),
                                       segment=i,
                                       wire=b"original-wire-" + bytes([i]))

        class Repo:
            def store_signed_packets(self, **kwargs):
                self.saved = kwargs
                return SimpleNamespace(packet_names=tuple(p.name for p in kwargs["packets"]))

        namespace = dict(globals(), FileSegmentedObjectProducer=Producer,
                         SegmentedObjectProducer=Producer)
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), "exec"), namespace)
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "payload"; source.write_bytes(b"immutable")
            for model in ["/Model/YOLO/Backbone", ""]:
                repo = Repo()
                result = namespace[function.name](repo, path=source, publisher="/actual/controller",
                    object_name="/actual/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/catalog-key",
                    object_type="onnx-model" if model else "runtime-script",
                    model_name=model, model_digest="sha256:" + "a" * 64 if model else "",
                    label="model.onnx" if model else "yolo-runner.py",
                    replication_factor=2, policy_epoch="/Policy/yolo")
                prefix = repo.saved["data_name"]
                components = Name.from_str(prefix)
                values = [bytes(Component.get_value(c)) for c in components]
                self.assertEqual(values[:2], [b"actual", b"controller"])
                self.assertEqual(values[2:4], [b"NDNSF-DI", b"model"] if model else [b"NDNSF", b"blob"])
                self.assertIn(b"name=model.onnx" if model else b"name=yolo-runner.py", values)
                self.assertEqual(values[-3], b"REPRESENTATION")
                self.assertEqual(values[-4], hashlib.sha256(b"immutable").digest())
                self.assertEqual(values[-1], b"DATA")
                self.assertNotIn(b"UPLOAD", values)
                self.assertEqual(repo.saved["object_sha256"], hashlib.sha256(b"immutable").hexdigest())
                self.assertEqual(result.packet_names, tuple(p.name for p in repo.saved["packets"]))
                self.assertEqual([p.wire for p in repo.saved["packets"]], [b"original-wire-\0", b"original-wire-\1"])
                self.assertEqual(calls[-1][2]["signing_identity"], "/actual/controller")
                index = json.loads(calls[-1][1])
                self.assertEqual(repo.saved["packet_manifest_sha256"],
                                 hashlib.sha256(calls[-1][1]).hexdigest())
                self.assertEqual(repo.saved["packet_manifest_name"], calls[-1][0] + "/v=7")
                self.assertEqual([p["name"] for p in index["packets"]], list(result.packet_names))
                self.assertEqual([p["wireSha256"] for p in index["packets"]],
                                 [hashlib.sha256(p.wire).hexdigest() for p in repo.saved["packets"]])
                self.assertEqual([action for action, _ in lifecycle[-4:]],
                                 ["start", "start", "stop", "stop"])
            fail_index_start[0] = True
            with self.assertRaisesRegex(RuntimeError, "index registration failed"):
                namespace[function.name](Repo(), path=source, publisher="/actual/controller",
                    object_name="/actual/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/catalog-key",
                    object_type="runtime-script", label="yolo-runner.py",
                    replication_factor=1, policy_epoch="/Policy/yolo")
            self.assertEqual([action for action, _ in lifecycle[-4:]],
                             ["start", "start", "stop", "stop"])


if __name__ == "__main__":
    unittest.main()
