from __future__ import annotations
import copy,unittest
from _support import load_tool,matrix
mx=load_tool("spec109_matrix")
class ScaleMatrixTest(unittest.TestCase):
    def test_keyed_uniqueness_partial_bundle_and_terminal_rules(self):
        value=matrix(); self.assertEqual(mx.validate_matrix(value)["status"],"PASS")
        self.assertFalse(mx.bundle_terminal(value,["c1"])); done=mx.transition_cell(value,"c1","BLOCKED",reason_code="GATE",gate_scope="systemic",gate_id="g",gate_digest="sha256:"+"a"*64); self.assertTrue(mx.bundle_terminal(done,["c1"]))
        with self.assertRaises(mx.MatrixError): mx.transition_cell(done,"c1","PASS",run_id="r",evidence_digest="sha256:"+"a"*64)
    def test_scoped_gate_does_not_block_unrelated_model(self):
        value=matrix(); value["models"].append("1.5B"); value["cells"]["c2"]=copy.deepcopy(value["cells"]["c1"]); value["cells"]["c2"]["modelSize"]="1.5B"
        gated=mx.apply_gate(value,source_cell="c1",scope="model-local",gate_id="g",gate_digest="sha256:"+"a"*64)
        self.assertEqual(gated["cells"]["c1"]["state"],"BLOCKED"); self.assertEqual(gated["cells"]["c2"]["state"],"NOT_STARTED")
if __name__ == "__main__": unittest.main()
