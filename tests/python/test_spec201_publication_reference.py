"""Bind actual Core publication results, using its native integration oracle."""
import os
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.repo_reference import (
    bind_published_large_data_reference, _publication_manifest_digest, publication_manifest_name)
from ndn.encoding import Name
from ndnsf_distributed_inference.naming import (
    canonical_name_uri, infer_semantic_data_producer, make_semantic_content_prefix)


class CorePublicationReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.payload = bytes([0x42]) * 40000
        cls.records = []
        binary = os.environ.get("SPEC201_PUBLICATION_ORACLE_BINARY",
                                "/var/tmp/spec200-asan/spec201-original-packet-publication")
        with tempfile.TemporaryDirectory(prefix="spec201-reference-") as directory:
            oracle = Path(directory) / "publications.txt"
            env = dict(os.environ, SPEC201_PUBLICATION_BINDING_ORACLE=str(oracle),
                       ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1")
            result = subprocess.run([
                binary, "--run_test=Spec201OriginalPacketPublication/"
                "SemanticManifestCategoriesKeepOriginalBlobPackets"],
                cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=30)
            if result.returncode:
                raise AssertionError(result.stdout)
            for block in oracle.read_text().strip().split("\n\n"):
                values = dict(line.split("=", 1) for line in block.splitlines())
                values["plaintext_size"] = int(values["plaintext_size"])
                values.update(success=True, encrypted=True)
                cls.records.append(values)

    def bind(self, values):
        return bind_published_large_data_reference(
            SimpleNamespace(**values), service_name="/spec201/model", payload=self.payload)

    def test_all_native_manifest_categories_and_owner_modes(self):
        self.assertEqual(len(self.records), 12)
        self.assertEqual({record["file_backed"] for record in self.records}, {"0", "1"})
        metadata = [
            {"kind": "blob", "label": "semantic"},
            {"kind": "model-file", "label": "model.onnx", "model_name": "fixture-model",
             "model_identity": "a" * 64},
            {"kind": "assignment", "label": "S0R0", "model_name": "fixture-model",
             "model_identity": "a" * 64},
            {"kind": "input", "label": "image", "input_kind": "application"},
            {"kind": "material", "label": "material"},
            {"kind": "material", "label": "assembly-receipt"},
        ]
        producer = "/spec201/NDNSF/MODEL/owner"
        for index, record in enumerate(self.records):
            with self.subTest(manifest=record["manifest_name"]):
                reference = self.bind(record)
                self.assertEqual(publication_manifest_name(SimpleNamespace(**record)),
                                 record["manifest_name"])
                self.assertEqual(reference.data_name, record["manifest_name"])
                self.assertEqual(reference.manifest_digest, record["manifest_digest"])
                self.assertEqual(reference.plaintext_size, len(self.payload))
                actual = Name.from_str(record["data_prefix"])
                self.assertEqual(infer_semantic_data_producer(actual),
                                 [bytes(c) for c in Name.from_str(producer)])
                expected = make_semantic_content_prefix(
                    producer, metadata[index % len(metadata)], record["content_digest"][7:])
                self.assertEqual(bytes(Name.encode(actual[:-3])), bytes(Name.encode(expected)))

    def test_mismatched_native_metadata_and_implicit_legacy_fail_closed(self):
        original = self.records[0]
        producer = canonical_name_uri("/spec201/NDNSF/MODEL/owner")
        changes = [
            {"manifest_digest": "sha256:" + "0" * 64},
            {"content_digest": "sha256:" + "0" * 64},
            {"manifest_name": original["manifest_name"].replace(producer, "/8=foreign", 1)},
            {"data_prefix": original["data_prefix"] + "/8=extra"},
            {"encrypted_data_name": original["encrypted_data_name"] + "/8=extra"},
            {"manifest_name": ""},
            {"data_prefix": ""},
            {"manifest_name": "", "data_prefix": ""},
        ]
        for changed in changes:
            with self.subTest(changed=changed):
                with self.assertRaises(ValueError):
                    self.bind({**original, **changed})
                with self.assertRaises(ValueError):
                    bind_published_large_data_reference(
                        SimpleNamespace(**{**original, **changed}),
                        service_name="/spec201/model", payload=self.payload,
                        allow_legacy=True)

    def test_receipt_label_must_match_original_material_and_cannot_be_omitted(self):
        original = self.records[5]
        root = Name.from_str(original["manifest_name"])
        from ndn.encoding import Component
        for changed in (root[:-2] + [Component.from_bytes(b"name=another-object")] + root[-1:],
                        root[:-2] + root[-1:]):
            with self.subTest(name=canonical_name_uri(changed)):
                with self.assertRaisesRegex(ValueError, "manifest digest/grammar"):
                    self.bind({**original, "manifest_name": canonical_name_uri(changed)})

    def test_legacy_metadata_requires_explicit_compatibility(self):
        values = {**self.records[0], "manifest_name": "", "data_prefix": "",
                  "encrypted_data_name": "/historical/source"}
        values["manifest_digest"] = _publication_manifest_digest(
            data_name=values["encrypted_data_name"], object_id=values["object_id"],
            plaintext_size=values["plaintext_size"], content_digest=values["content_digest"],
            authorization_scope=values["authorization_scope"],
            protection_epoch=values["protection_epoch"])
        with self.assertRaises(ValueError):
            self.bind(values)
        reference = bind_published_large_data_reference(
            SimpleNamespace(**values), service_name="/spec201/model",
            payload=self.payload, allow_legacy=True)
        self.assertEqual(reference.data_name, values["encrypted_data_name"])

    def test_source_public_wrapper_preserves_native_names_and_forwards_suffix(self):
        module_name = "ndnsf._spec201_publication_service"
        spec = importlib.util.spec_from_file_location(
            module_name, ROOT / "pythonWrapper/ndnsf/service.py")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        self.addCleanup(sys.modules.pop, module_name, None)
        spec.loader.exec_module(module)
        for record in self.records:
            result = module._from_native_large_data_result(SimpleNamespace(**record, error=""))
            self.assertEqual(result.manifest_name, record["manifest_name"])
            self.assertEqual(result.data_prefix, record["data_prefix"])
            self.assertEqual(publication_manifest_name(result), record["manifest_name"])
        calls = []

        def native_publish(*args):
            calls.append(args)
            return SimpleNamespace(**self.records[0], error="")

        receiver = SimpleNamespace(_native=SimpleNamespace(publish_encrypted_large_data=native_publish))
        result = module.ServiceUser.publish_encrypted_large_data(
            receiver, "/spec201/model", self.payload, object_label="source",
            manifest_suffix="/MATERIAL/MANIFEST")
        self.assertEqual(calls, [("/spec201/model", self.payload, "source", 60000,
                                 "/MATERIAL/MANIFEST")])
        self.assertEqual(result.manifest_name, self.records[0]["manifest_name"])


if __name__ == "__main__":
    unittest.main()
