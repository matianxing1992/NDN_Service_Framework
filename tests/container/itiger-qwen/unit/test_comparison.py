from __future__ import annotations
import copy,unittest
from _support import load_tool
candidate=load_tool("spec109_candidate"); validator=load_tool("validate_spec109")
class ComparisonTest(unittest.TestCase):
    def setUp(self):
        self.base={"artifactDigest":"sha256:"+"a"*64,"runtimeDigest":"sha256:"+"b"*64,"sessionDigest":"sha256:"+"c"*64,"workloadDigest":"sha256:"+"d"*64,"cacheState":"controlled-reset","loggingProfile":"warn","stageTopology":"0-1-2","gpuMapping":"0,1,2","warmup":2,"timeoutMs":30000,"windowSeconds":60,"plane":"baseline"}
    def test_matched_fingerprint_allows_only_plane(self):
        other=copy.deepcopy(self.base); other["plane"]="candidate"; self.assertEqual(candidate.comparison_fingerprint(self.base),candidate.comparison_fingerprint(other))
        other["cacheState"]="warm"; self.assertNotEqual(candidate.comparison_fingerprint(self.base),candidate.comparison_fingerprint(other))
        with self.assertRaisesRegex(validator.ValidationError,"COMPARISON_UNMATCHED"): validator.validate_comparison(self.base,other)
    def test_three_plane_authority_and_sample_thresholds(self):
        self.assertEqual(validator.validate_percentile(20,{"status":"AVAILABLE","value":1.0},"p50"),1.0)
        with self.assertRaisesRegex(validator.ValidationError,"PERCENTILE_SAMPLE_COUNT"): validator.validate_percentile(19,{"status":"AVAILABLE","value":1.0},"p50")
        with self.assertRaisesRegex(validator.ValidationError,"COMPARISON_ORACLE_TIMING_FORBIDDEN"): validator.validate_overhead_roles("correctness-oracle","candidate")
if __name__ == "__main__": unittest.main()
