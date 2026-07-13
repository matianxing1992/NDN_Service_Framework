from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest


UNIT = Path(__file__).resolve().parents[1] / "unit"
sys.path.insert(0, str(UNIT))
from _support import load_tool


proof = load_tool("spec110_execution_proof")


def valid_proof():
    stages = []
    processes = []
    gpus = []
    for index in range(3):
        identity = f"/ndnsf/provider/{index}"
        role = f"/LLM/Stage/{index}"
        uuid = f"GPU-{index}"
        pid = 100 + index
        gpus.append({"node": "itiger07", "uuid": uuid})
        processes.append({"identity": identity, "pid": pid, "role": role, "node": "itiger07", "gpuUuid": uuid})
        stages.append({"providerIdentity": identity, "providerPid": pid, "role": role, "node": "itiger07", "gpuUuid": uuid, "backend": "onnxruntime-cuda", "startedAtNs": 30 + index})
    return {
        "candidateId": "spec110-c1" + "-aaaaaaaaaaaa" * 6,
        "runId": "spec110-run-" + "a" * 20,
        "cellId": "spec110-cell-" + "a" * 20,
        "jobId": "123",
        "placementClass": "single-node-multi-gpu",
        "plane": "distributed-candidate",
        "readinessAtNs": 10,
        "request": {"requestId": "r", "sessionId": "s", "accepted": True, "acceptedAtNs": 20},
        "security": {"permissionEncrypted": True, "nacAbeVerified": True, "userTokenVerified": True, "providerTokenVerified": True, "providerPermissionVerified": True},
        "allocatedGpus": gpus,
        "nfds": [{"node": "itiger07", "pid": 10}],
        "providerProcesses": processes,
        "stageStarts": stages,
        "dependencies": [
            {"fromProvider": "/ndnsf/provider/0", "toProvider": "/ndnsf/provider/1", "fromNode": "itiger07", "toNode": "itiger07", "crossNode": False},
            {"fromProvider": "/ndnsf/provider/1", "toProvider": "/ndnsf/provider/2", "fromNode": "itiger07", "toNode": "itiger07", "crossNode": False}
        ],
        "promotion": {"complete": True},
        "generation": {"terminalResponseCount": 1, "oracleTokenIds": [1, 2], "outputTokenIds": [1, 2]},
    }


class ExecutionBoundaryTests(unittest.TestCase):
    def assert_not_started(self, value, code):
        decision = proof.closure_decision(value, "EXECUTED_PASS")
        self.assertFalse(decision["canCloseLiveTask"])
        self.assertEqual(decision["reasonCode"], code)

    def test_ack_only_cannot_reach_boundary(self):
        value = valid_proof()
        value["stageStarts"] = []
        self.assert_not_started(value, "EXECUTION_BOUNDARY_NOT_REACHED")

    def test_standalone_only_cannot_reach_boundary(self):
        value = valid_proof()
        value["plane"] = "standalone-oracle"
        self.assert_not_started(value, "EXECUTION_BOUNDARY_STANDALONE_ONLY")

    def test_gpu_visible_only_cannot_reach_boundary(self):
        value = valid_proof()
        value["request"]["accepted"] = False
        value["stageStarts"] = []
        self.assert_not_started(value, "EXECUTION_BOUNDARY_REQUEST_NOT_ACCEPTED")

    def test_pre_stage_crash_cannot_reach_boundary(self):
        value = valid_proof()
        value["stageStarts"] = []
        self.assert_not_started(value, "EXECUTION_BOUNDARY_NOT_REACHED")

    def test_incomplete_promotion_cannot_close_after_stage_start(self):
        value = valid_proof()
        value["promotion"]["complete"] = False
        decision = proof.closure_decision(value, "EXECUTED_FAIL", "stage-execution")
        self.assertFalse(decision["canCloseLiveTask"])
        self.assertEqual(decision["state"], "EVIDENCE_INCOMPLETE")

    def test_complete_single_node_dataflow_closes_pass(self):
        decision = proof.closure_decision(valid_proof(), "EXECUTED_PASS")
        self.assertTrue(decision["canCloseLiveTask"])
        self.assertTrue(decision["completeDataflow"])

    def test_backend_and_gpu_correlation_fail_closed(self):
        for mutate, code in (
            (lambda value: value["stageStarts"][0].update(backend="onnxruntime-cpu"), "EXECUTION_PROOF_CPU_FALLBACK"),
            (lambda value: value["stageStarts"][0].update(gpuUuid="GPU-stale"), "EXECUTION_PROOF_PROCESS_MISMATCH"),
        ):
            value = valid_proof()
            mutate(value)
            with self.assertRaisesRegex(proof.ExecutionProofError, code):
                proof.validate_execution_start(value)


if __name__ == "__main__":
    unittest.main()
