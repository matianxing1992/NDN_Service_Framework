from __future__ import annotations
import copy,unittest
from _support import FIXTURES,load_json,load_tool
candidate=load_tool("spec109_candidate")
class CandidateIdentityTest(unittest.TestCase):
    def setUp(self): self.value=load_json(FIXTURES/"profiles/all-sizes.json")
    def test_fingerprints_all_bindings_and_every_size(self):
        ids={candidate.build_candidate(self.value,size)["candidateId"] for size in self.value["sizes"]}; self.assertEqual(len(ids),7)
    def test_any_binding_change_requires_new_identity(self):
        before=candidate.build_candidate(self.value,"0.5B")["candidateId"]
        for field in ("sourceSnapshotDigest","predecessorGateDigest","deploymentProfileDigest","workloadDigest"):
            changed=copy.deepcopy(self.value); changed[field]="sha256:"+"9"*64
            with self.subTest(field=field): self.assertNotEqual(before,candidate.build_candidate(changed,"0.5B")["candidateId"])
if __name__ == "__main__": unittest.main()
