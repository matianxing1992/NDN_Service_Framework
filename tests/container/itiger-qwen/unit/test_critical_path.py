from __future__ import annotations
import sys,unittest
from _support import TOOLS
sys.path.insert(0,str(TOOLS))
import run_spec109_analysis as analysis
class CriticalPathTest(unittest.TestCase):
    def test_reconciliation_requires_99_percent_coverage(self):
        self.assertEqual(analysis.reconcile_critical_path(100,{"compute":60,"queue":20,"dependency":19})["status"],"PASS")
        self.assertEqual(analysis.reconcile_critical_path(100,{"compute":50,"queue":20,"dependency":10})["status"],"FAIL")
if __name__=="__main__":unittest.main()
