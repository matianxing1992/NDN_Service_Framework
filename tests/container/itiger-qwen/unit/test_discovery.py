from __future__ import annotations
import unittest
from _support import load_tool
storage=load_tool('spec109_storage')
class DiscoveryTest(unittest.TestCase):
 def test_parses_quota_lustre_gres_apptainer_and_egress(self):
  text='\n'.join(['NDNSF_DISCOVERY|USER|tma1','NDNSF_DISCOVERY|HOST|itiger','NDNSF_DISCOVERY|DF|/project/tma1|1000|100|900','NDNSF_DISCOVERY|QUOTA|lfs|100|1000|true','NDNSF_DISCOVERY|GRES|bigTiger|itiger07|gpu:rtx_5000:8|idle','NDNSF_DISCOVERY|APPTAINER|apptainer version 1.3.4','NDNSF_DISCOVERY|EGRESS|PASS'])
  value=storage.parse_discovery_output(text);self.assertTrue(value['quota']['verified']);self.assertEqual(value['gres'][0]['gres'],'gpu:rtx_5000:8');self.assertEqual(value['egress'],'PASS')
 def test_rejects_shared_capacity_without_required_signals(self):
  with self.assertRaisesRegex(storage.StorageError,'DISCOVERY_INCOMPLETE'):storage.parse_discovery_output('NDNSF_DISCOVERY|USER|tma1\nNDNSF_DISCOVERY|DF|/project/tma1|1000|0|1000')
if __name__=='__main__':unittest.main()
