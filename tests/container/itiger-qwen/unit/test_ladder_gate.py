from __future__ import annotations
import sys,unittest
from _support import D,TOOLS,load_tool
sys.path.insert(0,str(TOOLS))
import run_spec109_ladder as ladder
matrix_tool=load_tool("spec109_matrix")
class LadderGateTest(unittest.TestCase):
    def test_systemic_blocks_all_but_model_and_placement_stay_scoped(self):
        base=ladder.build_matrix(["1.5B","3B"],D)
        blocked=ladder.block_matrix(base,D,"SYSTEMIC")
        self.assertTrue(all(row["state"]=="BLOCKED" for row in blocked["cells"].values()))
        local=matrix_tool.apply_gate(base,source_cell="1.5B:transfer",scope="model-local",gate_id="MODEL",gate_digest=D)
        self.assertTrue(all(row["state"]=="BLOCKED" for cid,row in local["cells"].items() if cid.startswith("1.5B:")))
        self.assertTrue(all(row["state"]=="NOT_STARTED" for cid,row in local["cells"].items() if cid.startswith("3B:")))
        placed=matrix_tool.apply_gate(base,source_cell="3B:transfer",scope="placement-local",gate_id="PLACE",gate_digest=D)
        self.assertEqual(sum(row["state"]=="BLOCKED" for row in placed["cells"].values()),1)
    def test_blocked_matrix_has_no_runs_and_every_cell_terminal(self):
        value=ladder.block_matrix(ladder.build_matrix(["7B"],D),D,"SYSTEMIC")
        self.assertEqual(value["runs"],{})
        self.assertTrue(value["finalized"])
        self.assertEqual(matrix_tool.validate_matrix(value)["cellCount"],len(ladder.CELL_MODES))
if __name__=="__main__":unittest.main()
