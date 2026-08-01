import tempfile
import unittest
from pathlib import Path

from Experiments.NDNSF_DI_Run_Local_Deployment_Gates import (
    MANDATORY_TIERS,
    base_record,
    container_command,
)
from Experiments.NDNSF_Run_Minindn_Quick_Checks import (
    checks as quick_checks,
    fidelity_declaration,
)
from Experiments.ndnsf_validation.fidelity import GatePolicy, aggregate_records
from Experiments.ndnsf_validation.workload import canonical_workload


class GateRunnerTests(unittest.TestCase):
    def setUp(self):
        self.workload = canonical_workload(include_snapshot_manifest=False)

    def test_default_policy_names_all_four_gates(self):
        self.assertEqual(
            set(MANDATORY_TIERS),
            {
                "gate-a-fidelity",
                "gate-b-minindn",
                "gate-c-container",
                "gate-d-deadline",
            },
        )

    def test_legacy_quick_checks_are_explicitly_non_authorizing(self):
        declarations = [
            fidelity_declaration(item) for item in quick_checks().values()
        ]
        self.assertTrue(declarations)
        self.assertTrue(
            all(not item["deploymentAuthorizing"] for item in declarations)
        )
        self.assertNotIn(
            "REAL_MININDN_MODEL",
            {item["fidelityTier"] for item in declarations},
        )

    def test_missing_real_gates_keep_external_validation_unauthorized(self):
        record = base_record(
            case_id="gate-a-fidelity",
            gate_id="A",
            run_id="run",
            source_revision="source",
            tier=MANDATORY_TIERS["gate-a-fidelity"],
            command=["true"],
            started_at="start",
            status="PASS",
            failure_reason="",
            real_components=["python"],
            simulated_components=[],
            network_mode="none",
            container_mode="host",
            model_identity=self.workload["modelIdentity"],
            workload_digest=self.workload["workloadDigest"],
            backend="cpu",
            evidence_paths=["a"],
        )
        verdict = aggregate_records(
            [record],
            GatePolicy(
                schema_version=1,
                run_id="run",
                source_revision="source",
                mandatory_cases=MANDATORY_TIERS,
                model_identity_digest=self.workload["modelIdentity"]["contentDigest"],
                workload_digest=self.workload["workloadDigest"],
            ),
        )
        self.assertFalse(verdict["passed"])
        self.assertFalse(verdict["externalValidationAuthorized"])

    def test_passing_subset_is_diagnostic_not_external_authorization(self):
        record = base_record(
            case_id="gate-c-container",
            gate_id="C",
            run_id="run",
            source_revision="source",
            tier=MANDATORY_TIERS["gate-c-container"],
            command=["true"],
            started_at="start",
            status="PASS",
            failure_reason="",
            real_components=["docker", "minindn"],
            simulated_components=[],
            network_mode="real-minindn",
            container_mode="candidate",
            model_identity=self.workload["modelIdentity"],
            workload_digest=self.workload["workloadDigest"],
            backend="cpu",
            evidence_paths=["c"],
        )
        verdict = aggregate_records(
            [record],
            GatePolicy(
                schema_version=1,
                run_id="run",
                source_revision="source",
                mandatory_cases={
                    "gate-c-container": MANDATORY_TIERS["gate-c-container"]
                },
                model_identity_digest=self.workload["modelIdentity"]["contentDigest"],
                workload_digest=self.workload["workloadDigest"],
                authorization_case_ids=frozenset(MANDATORY_TIERS),
            ),
        )
        self.assertTrue(verdict["passed"])
        self.assertFalse(verdict["externalValidationAuthorized"])

    def test_container_command_binds_same_workload_and_limits(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            run_dir = Path(temporary).resolve()
            snapshot = run_dir / "model"
            snapshot.mkdir()
            command = container_command(
                image="candidate@sha256:abc",
                run_dir=run_dir,
                snapshot=snapshot,
                workload=self.workload,
                memory="4g",
                memory_swap="5g",
            )
        rendered = " ".join(command)
        self.assertIn("--privileged", command)
        self.assertIn("--memory=4g", command)
        self.assertIn("--memory-swap=5g", command)
        self.assertIn("NDNSF_PREFER_INSTALLED_NATIVE=1", command)
        self.assertIn(self.workload["workloadDigest"], rendered)
        self.assertIn("candidate@sha256:abc", command)
        self.assertIn("service openvswitch-switch start", rendered)
        self.assertIn("/opt/mini-ndn:ro", rendered)
        self.assertNotIn("tigercluster", rendered.lower())


if __name__ == "__main__":
    unittest.main()
