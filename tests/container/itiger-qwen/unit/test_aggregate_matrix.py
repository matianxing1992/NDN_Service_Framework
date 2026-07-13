from __future__ import annotations
import sys,unittest
from _support import D,TOOLS
sys.path.insert(0,str(TOOLS))
import run_spec109_analysis as analysis
import run_spec109_ladder as ladder
class AggregateMatrixTest(unittest.TestCase):
    def test_complete_denominator_includes_blocked_cells(self):
        value=ladder.block_matrix(ladder.build_matrix(["0.5B","1.5B"],D),D,"BLOCK")
        report=analysis.aggregate_matrix(value)
        self.assertEqual(report["plannedCellCount"],2*len(ladder.CELL_MODES))
        self.assertEqual(report["representedCellCount"],report["plannedCellCount"])
        self.assertEqual(report["stateCounts"],[*report["stateCounts"]] and {"BLOCKED":2*len(ladder.CELL_MODES)})
        self.assertFalse(report["successfulOnlyFiltering"])
    def test_nonterminal_matrix_rejected(self):
        with self.assertRaisesRegex(ValueError,"AGGREGATE_CELL_NONTERMINAL"):
            analysis.aggregate_matrix(ladder.build_matrix(["0.5B"],D))
if __name__=="__main__":unittest.main()
