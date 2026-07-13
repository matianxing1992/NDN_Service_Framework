from __future__ import annotations
import copy,unittest
from _support import evidence,load_tool
validator=load_tool("validate_spec109")
class CorrectnessGateTest(unittest.TestCase):
    def test_exact_arrays_checkpoints_and_reference_links(self):
        value=evidence()["correctness"]; self.assertEqual(validator.validate_correctness(value)["status"],"PASS")
        bad=copy.deepcopy(value); bad["outputTokenIds"]=[3]
        with self.assertRaisesRegex(validator.ValidationError,"CORRECTNESS_TOKEN_MISMATCH"): validator.validate_correctness(bad)
        bad=copy.deepcopy(value); bad["checkpoints"][0]["pass"]=False
        with self.assertRaisesRegex(validator.ValidationError,"CORRECTNESS_CHECKPOINT_FAILED"): validator.validate_correctness(bad)
        margin=copy.deepcopy(value); margin["checkpoints"][0]["kind"]="top1-margin"; margin["checkpoints"][0]["maxAbsError"]=0.2; margin["checkpoints"][0]["atol"]=0.1
        with self.assertRaisesRegex(validator.ValidationError,"CORRECTNESS_TOLERANCE_EXCEEDED"): validator.validate_correctness(margin)
if __name__ == "__main__": unittest.main()
