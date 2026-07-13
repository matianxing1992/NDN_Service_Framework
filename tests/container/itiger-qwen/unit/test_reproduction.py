from __future__ import annotations
import sys,unittest
from _support import TOOLS,load_tool
sys.path.insert(0,str(TOOLS))
import run_spec109_analysis as analysis
validator=load_tool("validate_spec109")
class ReproductionTest(unittest.TestCase):
    def test_exact_tokens_numerical_and_engineering_equivalence(self):
        value={"inputTokenIds":[1],"outputTokenIds":[2],"referenceOutputTokenIds":[2],"exactMatch":True,"checkpoints":[{"name":"logits","kind":"logits","rtol":.01,"atol":.001,"maxAbsError":0,"maxRelError":0,"pass":True}]}
        self.assertEqual(validator.validate_correctness(value)["status"],"PASS")
        self.assertEqual(analysis.engineering_equivalence(100,95,105),"PASS")
        self.assertEqual(analysis.engineering_equivalence(100,108,112),"INCONCLUSIVE")
        self.assertEqual(analysis.engineering_equivalence(100,112,118),"FAIL")
if __name__=="__main__":unittest.main()
