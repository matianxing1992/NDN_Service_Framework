from __future__ import annotations

import importlib.util
import hashlib
import csv
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import MappingProxyType, SimpleNamespace
import unittest

import numpy as np
import onnxruntime as ort

from ndnsf import ProviderCapabilityHint, encode_provider_capability_ack
from ndnsf_distributed_inference.sdk.placement import (
    ProviderSelectionProjectionV3,
)


ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/native_di_tracer"
    / "generate_spec170_hybrid_native_bundle.py"
)
RUNNER_PATH = ROOT / "Experiments/NDNSF_DI_NativeTracer_Minindn.py"
USER_DRIVER_PATH = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/native_di_tracer"
    / "user_driver.py"
)


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "generate_spec170_hybrid_native_bundle", GENERATOR_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {GENERATOR_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runner():
    spec = importlib.util.spec_from_file_location("spec170_native_runner", RUNNER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_user_driver():
    spec = importlib.util.spec_from_file_location(
        "spec170_native_user_driver", USER_DRIVER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {USER_DRIVER_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def fake_ack_closed(summary: dict, plan_digest: str):
    candidates = []
    for provider in sorted(set(summary["providersByRole"].values())):
        provider_roles = sorted(
            role for role, owner in summary["providersByRole"].items()
            if owner == provider)
        artifact_digests = {
            role: summary["artifactDigestsByRole"][role].upper()
            for role in provider_roles
        }
        evidence = {
            "schema": "ndnsf-di-execution-evidence-v1",
            "providerName": provider,
            "providerBootId": provider + "@boot",
            "runnerKind": "onnxruntime-cpu",
            "realCompute": "true",
            "device": {"kind": "cpu", "id": "cpu0", "ids": ["cpu0"]},
            "planDigest": plan_digest,
            "artifactDigests": artifact_digests,
            "roles": provider_roles,
            "cpuFallbackUsed": "false",
            "loadCompleted": "true",
            "warmupCompleted": "true",
        }
        payload = encode_provider_capability_ack(ProviderCapabilityHint(
            provider_name=provider,
            service_name="/Inference/NativeTracer",
            service_payload_schema="ndnsf-di-capability-v1",
            service_payload={
                "roles": ",".join(provider_roles),
                "runtimeStatus": "ready",
                "executionEvidence": evidence,
            },
        ))
        public_key = (provider + ":selection-key").encode()
        candidates.append(SimpleNamespace(
            provider_name=provider,
            service_name="/Inference/NativeTracer",
            request_id="/request-121",
            status=True,
            message="ready",
            payload=payload,
            telemetry=None,
            selection_input_key_offer={
                "schemaVersion": "1",
                "recipient": provider,
                "recipientCertName": provider + "/KEY/1/ID-CERT/0",
                "recipientPublicKey": public_key.hex(),
                "recipientCertDigest": (
                    "sha256:" + hashlib.sha256(public_key).hexdigest().upper()),
                "providerBootEpoch": provider + ":boot-epoch",
                "ndnsfDataV1EndpointPrefix": provider + "/data-v1",
            },
        ))
    return SimpleNamespace(
        request_id="/request-121",
        digest="sha256:" + "c" * 64,
        candidates=tuple(candidates),
    )


class Spec170HybridNativeBundleTest(unittest.TestCase):
    def test_user_result_json_normalization_unfreezes_nested_mappings(self) -> None:
        user_driver = load_user_driver()
        frozen = MappingProxyType({
            "provider": "/provider/0",
            "metadata": MappingProxyType({"roles": ("S0R0", "S1R0")}),
        })

        normalized = user_driver.json_compatible(frozen)

        self.assertEqual(normalized, {
            "provider": "/provider/0",
            "metadata": {"roles": ["S0R0", "S1R0"]},
        })
        self.assertEqual(json.loads(json.dumps(normalized)), normalized)

    def test_tensor_bundle_summary_exposes_small_float32_oracle_values(self) -> None:
        user_driver = load_user_driver()
        payload = bytearray(b"NDITB001")
        payload += struct.pack("<I", 1)
        name = b"output"
        payload += struct.pack("<I", len(name)) + name
        payload += struct.pack("<I", 1)
        payload += struct.pack("<I", 1) + struct.pack("<q", 1)
        payload += struct.pack("<Q", 4) + struct.pack("<f", 0.6)

        summary = user_driver.summarize_tensor_bundle(bytes(payload))

        self.assertTrue(summary["encoded"])
        self.assertEqual(summary["tensorCount"], 1)
        self.assertEqual(summary["tensors"][0]["name"], "output")
        self.assertEqual(summary["tensors"][0]["shape"], [1])
        self.assertAlmostEqual(summary["tensors"][0]["values"][0], 0.6, places=6)
        self.assertFalse(summary["tensors"][0]["valuesTruncated"])

    def test_runner_requires_final_tensor_to_match_numeric_oracle(self) -> None:
        runner = load_runner()
        user_result = {
            "requests": [{
                "status": "executed",
                "tensorBundle": {
                    "encoded": True,
                    "tensors": [{
                        "name": "final",
                        "shape": [1],
                        "values": [0.6000000238418579],
                        "valuesTruncated": False,
                    }],
                },
            }],
        }

        passed = runner.evaluate_user_numerical_oracle(
            user_result, "final", [0.6000000238418579])
        failed = runner.evaluate_user_numerical_oracle(
            user_result, "final", [0.7])

        self.assertEqual(passed["status"], "PASS")
        self.assertEqual(passed["checkedRequestCount"], 1)
        self.assertEqual(failed["status"], "FAIL")
        self.assertGreater(failed["maxAbsoluteError"], 0.09)

    def _generate(self, profile: str):
        module = load_generator()
        temporary = tempfile.TemporaryDirectory(
            prefix=f"spec170-hybrid-{profile}-")
        root = Path(temporary.name)
        summary = module.generate_hybrid_native_bundle(root, profile)
        return temporary, root, summary

    def test_121_bundle_has_exact_roles_edges_and_one_provider_per_role(self):
        temporary, root, summary = self._generate("121")
        self.addCleanup(temporary.cleanup)
        plan = json.loads((root / "native-execution-plan.json").read_text())
        service = plan["services"][0]

        self.assertEqual(service["executionPolicy"], "DATA_DRIVEN_V2")
        self.assertEqual(service["roles"], ["S0R0", "S1R0", "S1R1", "S2R0"])
        self.assertEqual(
            summary["providersByRole"],
            {
                "S0R0": "/NDNSF-DI/Tracer/provider/hybrid0",
                "S1R0": "/NDNSF-DI/Tracer/provider/hybrid1",
                "S1R1": "/NDNSF-DI/Tracer/provider/hybrid2",
                "S2R0": "/NDNSF-DI/Tracer/provider/hybrid3",
            },
        )
        self.assertEqual(
            [edge["redistributions"][0]["operation"]
             for edge in service["dependencies"]],
            ["SCATTER", "GATHER"],
        )
        self.assertTrue(all(
            edge["transportProfile"] == "NDNSF_DATA_V1"
            for edge in service["dependencies"]))
        self.assertEqual(summary["expectedOutput"], [0.6000000238418579])
        policy = (root / "controller.policies").read_text(encoding="utf-8")
        self.assertIn("for /NDNSF-DI/Tracer/provider/hybrid0", policy)
        self.assertIn("/Inference/NativeTracer/ROLE/S0R0", policy)
        self.assertIn("for /NDNSF-DI/Tracer/provider/hybrid1", policy)
        self.assertIn("/Inference/NativeTracer/ROLE/S2R0", policy)
        self._assert_onnx_oracle(root, summary)

    def test_212_bundle_has_final_cross_provider_merge(self):
        temporary, root, summary = self._generate("212")
        self.addCleanup(temporary.cleanup)
        plan = json.loads((root / "native-execution-plan.json").read_text())
        service = plan["services"][0]

        self.assertEqual(
            service["roles"], ["S0R0", "S0R1", "S1R0", "S2R0", "S2R1"])
        self.assertEqual(
            summary["providersByRole"],
            {
                "S0R0": "/NDNSF-DI/Tracer/provider/hybrid0",
                "S0R1": "/NDNSF-DI/Tracer/provider/hybrid1",
                "S1R0": "/NDNSF-DI/Tracer/provider/hybrid2",
                "S2R0": "/NDNSF-DI/Tracer/provider/hybrid3",
                "S2R1": "/NDNSF-DI/Tracer/provider/hybrid4",
            },
        )
        self.assertEqual(
            [edge.get("redistributions", [{}])[0].get("operation", "PIPELINE_TRANSFER")
             for edge in service["dependencies"]],
            ["GATHER", "SCATTER", "PIPELINE_TRANSFER"],
        )
        self.assertEqual(service["dependencies"][2]["tensors"], ["partial-sum"])
        self.assertEqual(summary["expectedOutput"], [0.6000000238418579])
        self._assert_onnx_oracle(root, summary)

    def test_unknown_profile_is_rejected_before_writing(self):
        module = load_generator()
        with tempfile.TemporaryDirectory(prefix="spec170-hybrid-invalid-") as temp:
            root = Path(temp)
            with self.assertRaisesRegex(ValueError, "profile"):
                module.generate_hybrid_native_bundle(root, "111")
            self.assertEqual(list(root.iterdir()), [])

    def test_native_tracer_cli_accepts_both_hybrid_profiles(self):
        for profile in ("121", "212"):
            with self.subTest(profile=profile):
                result = subprocess.run(
                    [
                        sys.executable, str(RUNNER_PATH), "--dry-run",
                        "--assignment", f"hybrid-{profile}",
                    ],
                    cwd=ROOT,
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                payload = json.loads(result.stdout)
                self.assertEqual(payload["assignment"], f"hybrid-{profile}")

    def test_provider_process_runs_from_generated_bundle_root(self):
        runner = load_runner()
        with tempfile.TemporaryDirectory(prefix="spec170-provider-cwd-") as temp:
            bundle = Path(temp).resolve()
            row = {
                "provider": "/NDNSF-DI/Tracer/provider/hybrid0",
                "role": "S0R0",
                "roles": "S0R0,S1R0",
            }
            command = runner.provider_serve_command(row, bundle)
            self.assertTrue(command.startswith(f"cd {bundle} && exec "), command)
            self.assertIn(str(bundle / "trust-schema.conf"), command)

    def test_native_tracer_request_ids_are_single_name_components(self):
        user_driver = load_user_driver()

        self.assertEqual(
            user_driver.wire_request_id(
                "", default="native-tracer:1"),
            "/native-tracer:1")
        self.assertEqual(
            user_driver.wire_request_id(
                "/spec170-d2b-replay-1", default="unused"),
            "/spec170-d2b-replay-1")
        with self.assertRaises(ValueError):
            user_driver.wire_request_id(
                "/NDNSF-DI/Tracer/user:native-tracer:1",
                default="unused")

    def test_lease_fields_are_projected_per_local_role(self):
        user_driver = load_user_driver()
        source = (
            "executionProviderBootId=boot-s0;"
            "executionLeaseId=lease-s0;"
            "executionLeaseEpoch=epoch-s0;"
            "executionLeasePlanDigest=sha256:" + "a" * 64 + ";"
            "executionLeaseBindingProof=proof-s0;"
            "executionLeaseProviderRoleCount=2;"
            "executionActivationDigest=sha256:" + "b" * 64 + ";"
            "executionActivationMembers=member-s0,member-s1;"
            "executionActivationLocalMember=member-s0;"
        ).encode()
        bindings = user_driver.execution_bindings_from_roles([
            {"role": "S0R0", "app_requirement": source},
        ])

        self.assertEqual(bindings["S0R0"]["lease_id"], "lease-s0")
        self.assertEqual(
            bindings["S0R0"]["activation_local_member"], "member-s0")

        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            user_driver.execution_bindings_from_roles([
                {"role": "S0R1", "app_requirement": b"executionLeaseId=only;"},
            ])

    def test_121_static_plan_builds_ack_bound_v3_group_selection(self):
        temporary, root, summary = self._generate("121")
        self.addCleanup(temporary.cleanup)
        user_driver = load_user_driver()
        service_plan = json.loads(
            (root / "native-execution-plan.json").read_text())['services'][0]
        plan_digest = (
            "sha256:" + hashlib.sha256(
                (root / "native-execution-plan.json").read_bytes()
            ).hexdigest().upper()
        )

        closed = fake_ack_closed(summary, plan_digest)

        commit = user_driver.build_static_v3_selection_commit(
            closed=closed,
            service_plan=service_plan,
            role_provider_assignments=summary["providersByRole"],
            plan_path=root / "native-execution-plan.json",
            deadline_ms=10**15,
            group_epoch_key_wrapper=(
                lambda public_key, epoch_key:
                hashlib.sha256(public_key + epoch_key).digest()
            ),
        )

        self.assertEqual(commit.plan_digest, plan_digest)
        self.assertEqual(dict(commit.provider_by_role), summary["providersByRole"])
        self.assertEqual(set(commit.assignment_payloads_by_role), set(service_plan["roles"]))
        projections = {
            role: ProviderSelectionProjectionV3.from_bytes(payload)
            for role, payload in commit.assignment_payloads_by_role.items()
        }
        artifact_digests = summary["artifactDigestsByRole"]
        group_wires = {item.group_capability_v1 for item in projections.values()}
        self.assertEqual(
            len(group_wires), len(set(summary["providersByRole"].values())))
        self.assertNotEqual(group_wires, {""})
        for role, projection in projections.items():
            provider = summary["providersByRole"][role]
            self.assertEqual(projection.provider, provider)
            self.assertEqual(projection.request_id, closed.request_id)
            self.assertEqual(projection.plan_digest, plan_digest)
            role_spec = next(item for item in projection.roles if item.role == role)
            self.assertEqual(
                role_spec.artifact_digest, artifact_digests[role])
            self.assertTrue(all(
                dependency["transportProfile"] == "NDNSF_DATA_V1"
                for dependency in projection.dependencies))

    def test_121_static_plan_passes_explicit_no_progress_bound_to_sealer(self):
        temporary, root, summary = self._generate("121")
        self.addCleanup(temporary.cleanup)
        user_driver = load_user_driver()
        service_plan = json.loads(
            (root / "native-execution-plan.json").read_text())["services"][0]
        plan_digest = (
            "sha256:" + hashlib.sha256(
                (root / "native-execution-plan.json").read_bytes()
            ).hexdigest().upper()
        )
        closed = fake_ack_closed(summary, plan_digest)
        original_sealer = user_driver.seal_group_capability_v1
        observed: list[int] = []

        def capture_sealer(**kwargs):
            observed.append(int(kwargs["no_progress_ms"]))
            return original_sealer(**kwargs)

        user_driver.seal_group_capability_v1 = capture_sealer
        self.addCleanup(setattr, user_driver,
                        "seal_group_capability_v1", original_sealer)
        user_driver.build_static_v3_selection_commit(
            closed=closed,
            service_plan=service_plan,
            role_provider_assignments=summary["providersByRole"],
            plan_path=root / "native-execution-plan.json",
            deadline_ms=10**15,
            no_progress_ms=12000,
            group_epoch_key_wrapper=(
                lambda public_key, epoch_key:
                hashlib.sha256(public_key + epoch_key).digest()),
        )
        self.assertEqual(observed, [12000])
        with self.assertRaises(ValueError):
            user_driver.build_static_v3_selection_commit(
                closed=closed,
                service_plan=service_plan,
                role_provider_assignments=summary["providersByRole"],
                plan_path=root / "native-execution-plan.json",
                deadline_ms=10**15,
                no_progress_ms=0,
                group_epoch_key_wrapper=(
                    lambda public_key, epoch_key:
                    hashlib.sha256(public_key + epoch_key).digest()),
            )

    def test_121_request_waits_for_ack_closed_before_v3_commit(self):
        temporary, root, summary = self._generate("121")
        self.addCleanup(temporary.cleanup)
        user_driver = load_user_driver()
        service_plan = json.loads(
            (root / "native-execution-plan.json").read_text())["services"][0]
        plan_digest = (
            "sha256:" + hashlib.sha256(
                (root / "native-execution-plan.json").read_bytes()
            ).hexdigest().upper()
        )
        closed = fake_ack_closed(summary, plan_digest)

        class Invocation:
            def __init__(self):
                self.request_id = closed.request_id
                self.commit = None

            def acks_closed(self):
                return closed

            def commit_plan(self, **kwargs):
                self.commit = kwargs
                return True

            def result(self):
                return SimpleNamespace(
                    status=True, payload=b"response", error="")

        class User:
            def __init__(self):
                self.begin_kwargs = None
                self.invocation = Invocation()

            def begin_collaboration(self, _service, _payload, **kwargs):
                self.begin_kwargs = kwargs
                return self.invocation

            def collaboration_status(self, _request_id, *, timeout_ms):
                del timeout_ms
                return ()

        user = User()
        assignment_csv = root / "assignment.csv"
        with assignment_csv.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=[
                "assignment", "role", "provider", "node", "service",
            ])
            writer.writeheader()
            writer.writerows(summary["assignmentRows"])
        args = SimpleNamespace(
            user="/NDNSF-DI/Tracer/user",
            role_provider_preference="",
            assignment_csv=str(assignment_csv),
            execution_leases=False,
            service="/Inference/NativeTracer",
            ack_timeout_ms=1200,
            timeout_ms=15000,
            plan=str(root / "native-execution-plan.json"),
            requests=1,
            concurrency=1,
            overload_fast_fail_timeout_ms=0,
        )
        roles = user_driver.collaboration_roles(service_plan, args.service)
        dependencies = user_driver.collaboration_dependencies(service_plan)
        key_scopes, role_scopes = user_driver.key_scopes_and_role_scopes(
            service_plan)
        original_wrapper = user_driver._native_group_epoch_key_wrapper
        user_driver._native_group_epoch_key_wrapper = (
            lambda public_key, epoch_key:
            hashlib.sha256(public_key + epoch_key).digest()
        )
        try:
            result = user_driver.run_one_request(
                user, args, service_plan, roles, key_scopes, dependencies,
                {}, role_scopes, 1)
        finally:
            user_driver._native_group_epoch_key_wrapper = original_wrapper

        self.assertEqual(result["status"], "executed")
        self.assertEqual(user.begin_kwargs["mode"], "DEFERRED")
        self.assertEqual(
            user.begin_kwargs["request_capabilities"],
            {"NDNSF_DATA_V1": "required"},
        )
        self.assertEqual(
            user.invocation.commit["ack_closed_digest"], closed.digest)
        self.assertEqual(
            user.invocation.commit["role_provider_assignments"],
            summary["providersByRole"],
        )
        self.assertEqual(
            set(user.invocation.commit["assignment_payloads_by_role"]),
            set(service_plan["roles"]),
        )

    def _assert_onnx_oracle(self, root: Path, summary: dict) -> None:
        values: dict[str, np.ndarray] = {
            "images": np.arange(12, dtype=np.float32).reshape(1, 3, 2, 2) / 10.0,
        }
        for step in summary["oracleSteps"]:
            if step["kind"] == "onnx":
                session = ort.InferenceSession(
                    str(root / step["artifactPath"]),
                    providers=["CPUExecutionProvider"])
                feeds = {
                    model_name: values[value_name]
                    for model_name, value_name in step["inputs"].items()
                }
                model_outputs = list(step["outputs"])
                outputs = session.run(model_outputs, feeds)
                for model_name, output in zip(model_outputs, outputs):
                    values[step["outputs"][model_name]] = output
            elif step["kind"] == "scatter":
                chunks = np.array_split(
                    values[step["source"]], len(step["targets"]),
                    axis=step["axis"])
                values.update(zip(step["targets"], chunks))
            elif step["kind"] == "gather":
                values[step["target"]] = np.concatenate(
                    [values[name] for name in step["sources"]],
                    axis=step["axis"])
            else:
                self.fail(f"unknown oracle step: {step}")
        actual = values[summary["finalTensor"]].reshape(-1).tolist()
        self.assertEqual(len(actual), 1)
        self.assertAlmostEqual(actual[0], summary["expectedOutput"][0], places=6)


if __name__ == "__main__":
    unittest.main()
