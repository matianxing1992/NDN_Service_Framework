from __future__ import annotations
import unittest
from _support import load_tool
v=load_tool('validate_spec109')
class ReferenceMetricsTest(unittest.TestCase):
 def test_count_qualified_percentiles(self):
  self.assertEqual(v.validate_percentile(1000,{'status':'AVAILABLE','value':2.0},'p99'),2.0)
  for n,name in ((19,'p50'),(99,'p95'),(999,'p99')):
   self.assertIsNone(v.validate_percentile(n,{'status':'UNAVAILABLE_INSUFFICIENT_N','value':None},name))
if __name__=='__main__':unittest.main()
