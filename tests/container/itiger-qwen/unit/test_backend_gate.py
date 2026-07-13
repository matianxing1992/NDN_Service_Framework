from __future__ import annotations
import copy,unittest
from _support import evidence,load_tool
validator=load_tool("validate_spec109")
class BackendGateTest(unittest.TestCase):
    def test_node_coverage_all_cuda_uuid_and_fallback(self):
        backend=evidence()["backend"]; self.assertEqual(validator.validate_backend(backend,allocated_gpu_uuids={"GPU-test"})["status"],"PASS")
        for mutation,reason in (({"fallbackUsed":True},"BACKEND_FALLBACK_USED"),({"nodeAssignments":[]},"BACKEND_NODE_PROFILE_INCOMPLETE")):
            bad=copy.deepcopy(backend); bad.update(mutation)
            with self.subTest(reason=reason),self.assertRaisesRegex(validator.ValidationError,reason): validator.validate_backend(bad,allocated_gpu_uuids={"GPU-test"})
        bad=copy.deepcopy(backend); bad["nodeAssignments"][0]["provider"]="CPUExecutionProvider"
        with self.assertRaisesRegex(validator.ValidationError,"BACKEND_MODEL_NODE_NOT_CUDA"): validator.validate_backend(bad,allocated_gpu_uuids={"GPU-test"})
if __name__ == "__main__": unittest.main()
