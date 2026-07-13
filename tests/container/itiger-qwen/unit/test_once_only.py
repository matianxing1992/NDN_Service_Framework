from __future__ import annotations
import unittest
from _support import load_tool,matrix
mx=load_tool("spec109_matrix")
class OnceOnlyTest(unittest.TestCase):
    def test_measured_terminal_is_immutable_and_no_retry(self):
        value=matrix(); submitted=mx.transition_cell(value,"c1","SUBMITTED",run_id="r1")
        failed=mx.transition_cell(submitted,"c1","FAIL",run_id="r1",evidence_digest="sha256:"+"a"*64,reason_code="ORIGINAL_EXIT_1")
        for state in ("NOT_STARTED","SUBMITTED","RUNNING","PASS"):
            with self.subTest(state=state),self.assertRaisesRegex(mx.MatrixError,"TERMINAL_IMMUTABLE"): mx.transition_cell(failed,"c1",state,run_id="r2")
if __name__ == "__main__": unittest.main()
