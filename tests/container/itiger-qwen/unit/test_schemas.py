from __future__ import annotations

import copy
import unittest
from jsonschema import Draft202012Validator, RefResolver

from _support import CONTRACTS, evidence, load_json, matrix, predecessor_gate, profile, source_snapshot


class SchemaTest(unittest.TestCase):
    def _validate(self, name: str, value: dict) -> None:
        path = CONTRACTS / name
        schema = load_json(path)
        store = {}
        for item in CONTRACTS.glob("*.json"):
            loaded = load_json(item)
            store[item.as_uri()] = loaded
            if "$id" in loaded:
                store[loaded["$id"]] = loaded
        Draft202012Validator(
            schema, resolver=RefResolver(path.as_uri(), schema, store=store)
        ).validate(value)

    def test_positive_contracts(self) -> None:
        for name, value in (
            ("source-snapshot.schema.json", source_snapshot()),
            ("predecessor-gate.schema.json", predecessor_gate()),
            ("qwen-experiment-profile.schema.json", profile()),
            ("scale-matrix.schema.json", matrix()),
            ("qwen-experiment-evidence.schema.json", evidence()),
        ):
            with self.subTest(name=name):
                self._validate(name, value)

    def test_adversarial_unknown_and_conditional_fields(self) -> None:
        bad_source = source_snapshot(); bad_source["secret"] = "x"
        bad_matrix = matrix(); bad_matrix["finalized"] = True; bad_matrix["cells"]["c1"]["state"] = "RUNNING"
        bad_evidence = evidence(); bad_evidence["authority"]["physicalProduction"] = "PASS"
        for name, value in (
            ("source-snapshot.schema.json", bad_source),
            ("scale-matrix.schema.json", bad_matrix),
            ("qwen-experiment-evidence.schema.json", bad_evidence),
        ):
            with self.subTest(name=name), self.assertRaises(Exception):
                self._validate(name, value)


if __name__ == "__main__": unittest.main()
