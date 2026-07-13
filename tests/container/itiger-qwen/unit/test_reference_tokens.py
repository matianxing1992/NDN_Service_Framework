from __future__ import annotations
import importlib.util,unittest
from pathlib import Path
from _support import REPO
P=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-qwen-reference.py';s=importlib.util.spec_from_file_location('ref',P);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class ReferenceTokenTest(unittest.TestCase):
 def test_token_digest_is_order_and_value_sensitive(self):self.assertEqual(m.digest([1,2]),m.digest([1,2]));self.assertNotEqual(m.digest([1,2]),m.digest([2,1]));self.assertNotEqual(m.digest([1,2]),m.digest([1,3]))
if __name__=='__main__':unittest.main()
