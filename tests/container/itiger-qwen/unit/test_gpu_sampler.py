from __future__ import annotations
import importlib.util,unittest
from _support import REPO
P=REPO/'packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/sample-qwen-resources.py';s=importlib.util.spec_from_file_location('sampler',P);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class GpuSamplerTest(unittest.TestCase):
 def test_uuid_peak_memory_and_utilization_parse(self):
  rows=m.parse('GPU-a, NVIDIA RTX 5000, 87, 1234, 32760\n');self.assertEqual(rows[0]['uuid'],'GPU-a');self.assertEqual(rows[0]['memoryUsedMiB'],1234);self.assertEqual(rows[0]['utilizationPercent'],87)
if __name__=='__main__':unittest.main()
