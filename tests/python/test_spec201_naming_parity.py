"""Compare python-ndn wire bytes with the production C++ naming selector."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndn.encoding import Component, Name
from ndnsf_distributed_inference import naming


class NamingParity(unittest.TestCase):
    def test_cpp_wire_and_crypto_transcript(self):
        binary = os.environ["SPEC201_NAMING_CPP"]
        result = subprocess.run([binary, "--run_test=ProducerDataNaming/PythonWireCorpus"],
                                capture_output=True, text=True, timeout=30, check=True)
        rows = [line.split()[1:] for line in result.stdout.splitlines() if line.startswith("SPEC201_WIRE ")]
        self.assertEqual(len(rows), 2)
        producers = [Name.from_str("/org/provider/p0"),
                     Name.from_str("/org/NDNSF/TENSOR/provider") + [Component.from_version(257)]]
        for producer, (cpp_name, cpp_digest, cpp_routing) in zip(producers, rows):
            fields = [naming.NamingDigestField(1, 1, bytes(Name.encode(producer))),
                      naming.NamingDigestField(2, 2, "模型/α".encode()),
                      naming.NamingDigestField(3, 3, b"\xff" * 8),
                      naming.NamingDigestField(4, 4, b"\0\xff"),
                      naming.NamingDigestField(5, 5, b'{"a":1,"b":"x"}')]
            digest = naming.compute_naming_digest("ndnsf-naming/fixture", fields)
            name = naming.make_producer_data_prefix(producer, "TENSOR") + [
                naming.make_naming_digest_component(digest), Component.from_segment(7)]
            self.assertEqual(digest, cpp_digest)
            self.assertEqual(bytes(Name.encode(name)).hex(), cpp_name)
            routing = Name.from_str("/full/service") + [Component.from_version(257),
                                                      Component.from_bytes(b"."), Component.from_bytes(b"")]
            self.assertEqual(naming.make_reversible_name_component(routing).hex(), cpp_routing)
            self.assertEqual(naming.parse_reversible_name_component(bytes.fromhex(cpp_routing)),
                             [bytes(component) for component in routing])
            self.assertIsNotNone(naming.parse_producer_data_name(name, producer))
            self.assertIsNone(naming.parse_producer_data_name(name, "/org/NDNSF"))

    def test_readable_semantic_wire(self):
        binary = os.environ["SPEC201_NAMING_CPP"]
        result = subprocess.run([binary, "--run_test=ProducerDataNaming/ReadableSemanticNames"],
                                check=True, text=True, capture_output=True)
        cpp_name = next(line.split()[1] for line in result.stdout.splitlines()
                        if line.startswith("SPEC201_LABEL "))
        prefix = naming.make_producer_data_prefix("/org/NDNSF-DI/model/producer", "model",
                                                  name_space="NDNSF-DI")
        name = prefix + [naming.make_naming_label_component("模型/qwen=α"),
                         naming.make_naming_digest_component("a" * 64)]
        self.assertEqual(bytes(Name.encode(name)).hex(), cpp_name)
        self.assertEqual(naming.parse_producer_data_name(name, "/org/NDNSF-DI/model/producer").name_space,
                         "NDNSF-DI")
        self.assertEqual(naming.make_naming_label_component("a" * 63 + "模suffix"),
                         bytes(Component.from_bytes(b"name=" + b"a" * 63)))
        for invalid in ("", "\x00", "\u0085", "\ud800"):
            with self.assertRaises(ValueError):
                naming.make_naming_label_component(invalid)

    def test_invalid_fields_and_uint64(self):
        for fields in [[(1, 3, b"x")], [(1, 2, b"\xc0\x80")], [(1, 4, b""), (1, 4, b"")],
                       [(1, 6, b"")], [(1, 1, b"\x07\x01")]]:
            with self.assertRaises(ValueError):
                naming.compute_naming_digest("d", fields)
        for value in (0, 1, 255, 256, (1 << 64) - 1):
            self.assertEqual(naming.make_naming_decimal_component(value), bytes(Component.from_str(str(value))))
        for value in (-1, 1 << 64, True):
            with self.assertRaises(ValueError):
                naming.make_naming_decimal_component(value)


if __name__ == "__main__":
    unittest.main()
