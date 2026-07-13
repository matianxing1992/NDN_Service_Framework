from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest


UNIT = Path(__file__).resolve().parents[1] / "unit"
sys.path.insert(0, str(UNIT))
from _support import FIXTURES, load_tool


evidence = load_tool("spec110_evidence")
EVIDENCE = FIXTURES / "evidence"
EXPECTED_SIF = "a" * 64


def load(name: str):
    return json.loads((EVIDENCE / name).read_text(encoding="utf-8"))


def set_path(value, path: str, replacement) -> None:
    current = value
    parts = path.split(".")
    for part in parts[:-1]:
        current = current[int(part)] if isinstance(current, list) else current[part]
    final = parts[-1]
    if isinstance(current, list):
        current[int(final)] = replacement
    else:
        current[final] = replacement


class EvidenceMutationTests(unittest.TestCase):
    def test_positive_single_and_multi_node_fixtures(self):
        single = evidence.validate_evidence(load("single-node-pass.json"), expected_sif_sha256=EXPECTED_SIF)
        multi = evidence.validate_evidence(load("multi-node-pass.json"), expected_sif_sha256=EXPECTED_SIF)
        self.assertEqual((single["nodeCount"], single["stageCount"]), (1, 3))
        self.assertEqual((multi["nodeCount"], multi["stageCount"]), (2, 3))

    def test_all_registered_mutations_fail_with_expected_code(self):
        registry = load("mutations.json")
        self.assertEqual(len(registry["cases"]), 9)
        for case in registry["cases"]:
            with self.subTest(case=case["name"]):
                value = copy.deepcopy(load(case["base"]))
                set_path(value, case["path"], case["value"])
                for extra in case.get("extra", []):
                    set_path(value, extra["path"], extra["value"])
                with self.assertRaisesRegex(
                    evidence.EvidenceValidationError, "^" + case["expectedCode"]
                ):
                    evidence.validate_evidence(value, expected_sif_sha256=EXPECTED_SIF)


if __name__ == "__main__":
    unittest.main()
