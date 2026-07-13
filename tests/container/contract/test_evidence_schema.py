from __future__ import annotations

import copy
import unittest

from contract._support import load_impl, valid_slurm_evidence

evidence_impl = load_impl("evidence")
EvidenceError = evidence_impl.EvidenceError
validate_evidence = evidence_impl.validate_evidence


class EvidenceSchemaTest(unittest.TestCase):
    def test_valid_slurm_evidence(self) -> None:
        validate_evidence(valid_slurm_evidence())

    def test_physical_pass_is_rejected(self) -> None:
        value = valid_slurm_evidence()
        value["authority"]["physicalProduction"] = "PASS"
        with self.assertRaises(EvidenceError):
            validate_evidence(value)

    def test_fallback_cannot_pass(self) -> None:
        value = copy.deepcopy(valid_slurm_evidence())
        value["backend"]["fallbackOccurred"] = True
        value["backend"]["status"] = "DEGRADED"
        with self.assertRaises(EvidenceError):
            validate_evidence(value)


if __name__ == "__main__":
    unittest.main()
