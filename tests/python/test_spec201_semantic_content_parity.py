"""Compare all C02 content families against the actual Core C++ selector."""
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndn.encoding import Component, Name
from ndnsf_distributed_inference import naming


class SemanticContentParity(unittest.TestCase):
    def test_all_content_families(self):
        binary = os.environ["SPEC201_STATIC_PUBLICATION_CPP"]
        result = subprocess.run([binary, "--run_test=PublishedContentNaming/SemanticWireCorpus"],
                                capture_output=True, text=True, timeout=30, check=True)
        rows = [line.split()[1:] for line in result.stdout.splitlines()
                if line.startswith("SPEC201_SEMANTIC ")]
        self.assertEqual(len(rows), 9)
        producer = Name.from_str("/org/NDNSF-DI/model/publisher") + [Component.from_version(257)]
        kinds = ["blob", "model-file", "weights", "initializer", "tokenizer", "artifact-file", "material", "input", "assignment"]
        for index, (cpp_kind, cpp_wire) in enumerate(rows):
            self.assertEqual(int(cpp_kind), index)
            fields = {"kind": kinds[index], "label": "权重/α.dat"}
            if 1 <= index <= 5 or index == 8:
                fields.update(model_name="qwen2.5-0.5b", model_identity="a" * 64)
            if index == 5:
                fields.update(artifact_kind="stage", coordinate="layers=0-11", profile_identity="c" * 64)
            if index == 7:
                fields["input_kind"] = "image"
            prefix = naming.make_semantic_content_prefix(producer, fields, "b" * 64)
            self.assertEqual(bytes(Name.encode(prefix)).hex(), cpp_wire)
            data = prefix + [Component.from_bytes(b"PROTECTED"),
                             naming.make_naming_digest_component("d" * 64), Component.from_bytes(b"DATA")]
            self.assertEqual(naming.infer_semantic_data_producer(data), [bytes(c) for c in producer])

    def test_reject_invalid_semantics_and_legacy(self):
        for fields in ({"kind": "unknown", "label": "x"},
                       {"kind": "model-file", "label": "x"},
                       {"kind": "blob", "label": "x", "model_identity": "a" * 64},
                       {"kind": "input", "label": "x", "input_kind": "a/b"},
                       {"kind": "blob", "label": "x", "producer": "/other"}):
            with self.assertRaises(ValueError):
                naming.make_semantic_content_prefix("/p", fields, "b" * 64)
        legacy = naming.make_producer_data_prefix("/p", "BLOB") + [
            naming.make_naming_digest_component("b" * 64), Component.from_bytes(b"PROTECTED"),
            naming.make_naming_digest_component("d" * 64), Component.from_bytes(b"DATA")]
        self.assertIsNone(naming.infer_semantic_data_producer(legacy))


if __name__ == "__main__":
    unittest.main()
