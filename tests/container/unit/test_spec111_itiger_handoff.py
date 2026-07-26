from __future__ import annotations

import hashlib
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests/fixtures/ndnsf-di-core-app-separation/itiger-handoff-process-map.json"
CANDIDATE = ROOT / "specs/111-ndnsf-di-core-app-separation/evidence/post-separation-candidate.json"
CONTRACT = ROOT / "specs/111-ndnsf-di-core-app-separation/contracts/itiger-slurm-apptainer-handoff.md"


class Spec111ItigerStaticHandoffTest(unittest.TestCase):
    def setUp(self) -> None:
        self.value = json.loads(FIXTURE.read_text(encoding="utf-8"))

    def test_fixture_has_exact_identity_digests_but_no_execution_claim(self) -> None:
        self.assertTrue(self.value["fixtureOnly"])
        for field in ("deploymentRevisionDigest", "ociDigest", "sifDigest"):
            self.assertRegex(self.value[field], r"^sha256:[0-9a-f]{64}$")
        self.assertRegex(hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), r"^[0-9a-f]{64}$")
        candidate = json.loads(CANDIDATE.read_text(encoding="utf-8"))
        for field in ("ociDigest", "sifDigest", "containerBuildDigest", "itigerExecutionDigest"):
            self.assertEqual(candidate[field], "DEFERRED_TO_SPEC110")

    def test_revision_derived_roles_and_every_project_process_share_one_sif(self) -> None:
        processes = self.value["processes"]
        self.assertEqual(
            [item["role"] for item in processes if item["kind"] == "provider"],
            ["/LLM/Prefill", "/LLM/Decode"],
        )
        self.assertEqual({item["sifDigest"] for item in processes}, {self.value["sifDigest"]})
        self.assertTrue(all(item["command"][0] == "/opt/ndnsf/bin/run-container.sh" for item in processes))

    def test_binds_are_least_privilege_and_node_run_socket_is_shared(self) -> None:
        binds = self.value["binds"]
        modes = {item["target"]: item["mode"] for item in binds}
        self.assertEqual(modes["/models/qwen"], "ro")
        self.assertEqual(modes["/artifacts"], "ro")
        self.assertEqual(modes["/state"], "rw")
        self.assertEqual(modes["/run/ndnsf"], "rw")
        self.assertNotIn("/project", {item["target"] for item in binds})
        sockets = {item["nfdSocket"] for item in self.value["processes"]}
        self.assertEqual(sockets, {"/run/ndnsf/nfd.sock"})
        identities = [item["identityBind"] for item in self.value["processes"] if item["identityBind"]]
        self.assertTrue(all(item["mode"] == "ro" for item in identities))
        self.assertEqual(len({item["source"] for item in identities}), len(identities))

    def test_gpu_mapping_is_unique_and_state_authorities_are_disjoint(self) -> None:
        gpu_uuids = [item["gpuUuid"] for item in self.value["processes"] if item["gpuUuid"]]
        self.assertEqual(len(gpu_uuids), len(set(gpu_uuids)))
        states = self.value["stateMachines"]
        self.assertEqual(set(states), {"scheduler", "deployment", "request"})
        self.assertNotIn("ACTIVE", states["scheduler"])
        self.assertNotIn("SUCCEEDED", states["deployment"])
        self.assertNotIn("RUNNING", set(states["deployment"]) & set(states["request"]))

    def test_contract_prohibits_all_runtime_and_remote_actions_in_spec111(self) -> None:
        text = CONTRACT.read_text(encoding="utf-8")
        self.assertIn("MUST NOT build", text)
        self.assertIn("invoke Docker/Podman/Buildah/Apptainer", text)
        self.assertIn("contact/submit to iTiger", text)


if __name__ == "__main__":
    unittest.main()
