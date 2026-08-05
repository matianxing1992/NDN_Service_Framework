from __future__ import annotations

from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.app_sdk.canonical_artifacts import (  # noqa: E402
    AssembledOnnxArtifactV1,
)
from ndnsf_distributed_inference.artifact_deployment import (  # noqa: E402
    assemble_onnx_role,
)


class Spec170ProviderAssemblyTest(unittest.TestCase):
    def bundle(self, external: bool = False):
        entries = {"model.onnx": b"onnx-bytes"}
        if external:
            entries["model.onnx.data"] = b"external-data"
        entry_digests = {
            name: "sha256:" + hashlib.sha256(payload).hexdigest()
            for name, payload in entries.items()
        }
        return AssembledOnnxArtifactV1(
            manifest={
                "schema": "ndnsf-di-assembled-onnx-v1",
                "model": "Qwen/Qwen3-0.6B",
                "signer": "/provider/p0",
                "layout": "ONNX_EXTERNAL_DATA" if external else "INLINE_ONNX",
                "entryDigests": entry_digests,
            }, entries=entries, signer="/provider/p0", signature="signed")

    def test_deterministic_single_file_round_trip(self):
        original = self.bundle(external=True)
        wire = original.to_bytes()
        restored = AssembledOnnxArtifactV1.from_bytes(wire)
        self.assertEqual(restored.to_bytes(), wire)
        self.assertEqual(restored.object_digest, original.object_digest)
        restored.verify_provider("/provider/p0")
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "stage.ndnsf-onnx-artifact"
            self.assertEqual(original.write_atomic(target), original.object_digest)
            self.assertEqual(target.read_bytes(), wire)

    def test_unsafe_or_duplicate_entries_fail(self):
        with self.assertRaises(ValueError):
            AssembledOnnxArtifactV1(
                manifest={"signer": "/provider/p0"},
                entries={"../model.onnx": b"x"}, signer="/provider/p0",
                signature="signed")
        wire = self.bundle().to_bytes()
        with self.assertRaises(ValueError):
            AssembledOnnxArtifactV1.from_bytes(wire + b"trailing")

    def test_cross_provider_import_fails_closed(self):
        with self.assertRaises(ValueError):
            self.bundle().verify_provider("/provider/p1")

    def test_provider_assembly_is_atomic_and_content_addressed(self):
        payloads = {"/layer/0": b"layer-zero", "/layer/1": b"layer-one"}
        digests = {
            name: "sha256:" + hashlib.sha256(payload).hexdigest()
            for name, payload in payloads.items()
        }
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "stage.ndnsf-onnx-artifact"
            result = assemble_onnx_role(
                role="stage-0", model_name="Qwen/Qwen3-0.6B",
                model_digest="sha256:" + "a" * 64,
                profile="qwen-onnx-cpu", graph_digest="sha256:" + "b" * 64,
                layer_payloads=payloads, layer_digests=digests,
                recipe_digest="sha256:" + "c" * 64,
                provider="/provider/p0", signature="signed", output_path=target,
            )
            self.assertTrue(target.is_file())
            self.assertEqual(result.path, target)
            self.assertFalse(target.with_name(target.name + ".tmp").exists())
            restored = AssembledOnnxArtifactV1.from_bytes(target.read_bytes())
            restored.verify_provider("/provider/p0")


if __name__ == "__main__":
    unittest.main()
