from __future__ import annotations
import copy,unittest
from _support import evidence,load_tool
validator=load_tool("validate_spec109")
class EvidenceTest(unittest.TestCase):
    def test_promotion_original_exit_redaction_and_checksums(self):
        value=evidence(); self.assertEqual(validator.validate_evidence(value)["status"],"PASS")
        for mutate,reason in (
            (lambda x:x["promotion"].update(complete=False),"EVIDENCE_PROMOTION_INCOMPLETE"),
            (lambda x:x["terminal"].update(originalExitCode=1,status="PASS"),"EVIDENCE_EXIT_CONTRADICTION"),
            (lambda x:x["checksums"].update(stdout="bad"),"EVIDENCE_CHECKSUM_INVALID"),
        ):
            bad=copy.deepcopy(value); mutate(bad)
            with self.subTest(reason=reason),self.assertRaisesRegex(validator.ValidationError,reason): validator.validate_evidence(bad)
        bad=copy.deepcopy(value); bad["rawProviderToken"]="secret"
        with self.assertRaisesRegex(validator.ValidationError,"EVIDENCE_SECRET_FIELD"): validator.validate_evidence(bad)
if __name__ == "__main__": unittest.main()
