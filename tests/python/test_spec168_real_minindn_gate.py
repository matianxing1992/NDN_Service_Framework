#!/usr/bin/env python3
"""Contract tests for the Spec 168 real MiniNDN and exact-SIF gates."""

from __future__ import annotations

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from ndnsf_distributed_inference.core.contracts import LifecycleEventV1


REPO = Path(__file__).resolve().parents[2]
JOBS = REPO / "specs/168-itiger-di-deployment-fidelity/jobs"
SPEC = importlib.util.spec_from_file_location(
    "spec168_local_gate", JOBS / "spec168_local_gate.py")
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
EVIDENCE_SPEC = importlib.util.spec_from_file_location(
    "spec168_runtime_evidence", REPO / "tools/ndnsf-di/spec168_runtime_evidence.py")
assert EVIDENCE_SPEC is not None and EVIDENCE_SPEC.loader is not None
runtime_evidence = importlib.util.module_from_spec(EVIDENCE_SPEC)
EVIDENCE_SPEC.loader.exec_module(runtime_evidence)
RETAINED_SPEC = importlib.util.spec_from_file_location(
    "spec168_retained_runtime_gate",
    JOBS / "spec168_retained_runtime_gate.py")
assert RETAINED_SPEC is not None and RETAINED_SPEC.loader is not None
retained_gate = importlib.util.module_from_spec(RETAINED_SPEC)
RETAINED_SPEC.loader.exec_module(retained_gate)

DIGEST = "sha256:" + "a" * 64
PLAN = "sha256:" + "b" * 64


def lifecycle_event(
    index: int, kind: str, role: str | None = None,
) -> LifecycleEventV1:
    role_scoped = kind in {
        "ROLE_ASSIGNED", "LOCAL_READY", "STAGE_EXECUTING", "STAGE_COMPLETED"}
    return LifecycleEventV1(
        experiment_id="spec168-gate-test", request_id="request-1",
        attempt_epoch=1, event_id=f"event-{index}", event_type=kind,
        component="provider" if role_scoped else "user",
        provider=f"/provider/{role[-1]}" if role_scoped and role else None,
        provider_boot_epoch=f"boot-{role[-1]}" if role_scoped and role else None,
        role=role if role_scoped else None,
        plan_digest=PLAN if kind in {
            "PLAN_COMMITTED", "FINAL_SELECTION", "ROLE_ASSIGNED",
            "LOCAL_READY", "STAGE_EXECUTING", "STAGE_COMPLETED",
            "RESPONSE_PUBLISHED"} else None,
        operation_id=None, epoch=0, sequence=index,
        monotonic_ns=index * 1000, wall_time_utc="2026-08-03T00:00:00Z",
        authenticated=True, details_schema="spec168.test.v1", details={})


class Spec168RealMiniNdnGateTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.output = Path(self.temporary.name)
        prefix = (
            "REQUEST_CREATED", "REQUEST_PUBLISHED", "ACK_CLOSED",
            "GRAPH_INSPECTED", "PLAN_VALIDATED", "PLAN_COMMITTED",
            "FINAL_SELECTION",
        )
        events = []
        index = 1
        for kind in prefix:
            events.append(lifecycle_event(index, kind))
            index += 1
        for role in ("stage-0", "stage-1", "stage-2"):
            for kind in ("ROLE_ASSIGNED", "LOCAL_READY", "STAGE_EXECUTING",
                         "STAGE_COMPLETED"):
                events.append(lifecycle_event(index, kind, role))
                index += 1
        events.append(lifecycle_event(index, "RESPONSE_PUBLISHED"))
        lifecycle = self.output / "lifecycle.jsonl"
        lifecycle.write_text("".join(
            json.dumps(item.to_dict(), sort_keys=True) + "\n"
            for item in events), encoding="utf-8")
        roles = (
            "controller", "repository", "user", "provider-stage-0",
            "provider-stage-1", "provider-stage-2",
        )
        self.payload = {
            "schema": gate.RUNTIME_SCHEMA,
            "fidelity": "REAL_MININDN",
            "sourceDigest": DIGEST,
            "modelIdentityDigest": DIGEST,
            "workloadDigest": DIGEST,
            "simulatedComponents": [],
            "processes": [
                {"role": role, "pid": 2000 + index}
                for index, role in enumerate(roles)
            ],
            "network": {
                "realMiniNdn": True, "realNfdPerNode": True,
                "hostNfdUsed": False, "routeSnapshotDigest": DIGEST,
            },
            "security": {
                "normalPermissions": True, "nacAbe": True,
                "userToken": True, "providerToken": True,
                "replayProtection": True, "testOnlyIdentities": False,
                "bypassEnabled": False,
            },
            "artifactDelivery": {
                "transport": "NDNSF-DistributedRepo", "throughNdn": True,
                "sharedFilesystemPayloadInjection": False,
                "uniqueBytes": 1024, "wireBytes": 1100,
                "artifactDigests": {
                    f"stage-{index}": DIGEST for index in range(3)
                },
            },
            "readiness": {"mode": "event-driven", "fixedSettleWaitMs": 0},
            "adapter": {
                "name": "qwen-transformers", "mocked": False,
                "backend": "transformers-cuda", "deviceClass": "CUDA",
                "assignments": [
                    {
                        "role": f"stage-{index}", "device": "cuda:0",
                        "artifactDigest": DIGEST, "loadCompleted": True,
                        "warmupCompleted": True, "cpuFallbackCount": 0,
                    }
                    for index in range(3)
                ],
            },
            "invocation": {
                "requestId": "request-1", "attemptEpoch": 1,
                "wireRequestCount": 1, "tokenRequestCount": 0,
                "completeResponse": True, "tokenCount": 8,
                "cpuFallbackCount": 0,
            },
            "evidence": {"lifecycleJsonl": "lifecycle.jsonl"},
        }

    def tearDown(self):
        self.temporary.cleanup()

    def test_valid_real_minindn_contract_is_admitted(self):
        result = gate.validate_runtime_manifest(
            self.output, self.payload, mode="real-minindn")
        self.assertEqual(result["requestId"], "request-1")
        self.assertEqual(result["tokenCount"], 8)

    def test_runtime_evidence_writer_admits_observed_cpu_logic_run(self):
        source = "sha256:" + "c" * 64
        model = "sha256:" + "d" * 64
        workload = "sha256:" + "e" * 64
        plan = "sha256:" + "f" * 64
        request = "/spec168%2Ftest%2Frequest"
        user = "\n".join((
            "UserToken/ProviderToken runtime mode: enabled",
            "Installed user permission provider=/provider/AI/LLM service=/AI/LLM",
            f"NDNSF_DI_AUTOPLANNING_ACK_CLOSED requestId={request} ackCount=3",
            f"NDNSF_DI_AUTOPLANNING_GRAPH_READY requestId={request} graphDigest={DIGEST}",
            f"NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED requestId={request} candidateDigest={plan}",
            f"NDNSF_DI_AUTOPLANNING_ACK_CLOSED requestId={request}-measured ackCount=3",
            f"NDNSF_DI_AUTOPLANNING_GRAPH_READY requestId={request}-measured graphDigest={DIGEST}",
            f"NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED requestId={request}-measured candidateDigest={plan}",
            "LLM_PIPELINE_GENERATION_CAMPAIGN_PASS campaignId=spec168-test",
        ))
        (self.output / "llm-pipeline-user.log").write_text(user)
        (self.output / "automatic-planning.json").write_text(json.dumps({
            "candidateDigest": plan,
        }))
        warmup = {
            "campaignId": "spec168-test",
            "phase": "warmup",
            "status": "OK",
            "modelIdentityDigest": model,
            "workloadDigest": workload,
            "generatedTokenIds": [5, 6, 7],
            "totalMs": 5000.0,
            "tokenSteps": [{"metadata": {
                "requestId": request,
                "wireRequestCount": 1,
                "tokenRequestCount": 0,
            }}],
        }
        measured = {
            "campaignId": "spec168-test",
            "phase": "measured",
            "status": "OK",
            "modelIdentityDigest": model,
            "workloadDigest": workload,
            "generatedTokenIds": [7, 8, 9, 10],
            "tokenSteps": [{"metadata": {
                "requestId": request + "-measured",
                "wireRequestCount": 1,
                "tokenRequestCount": 0,
            }}],
            "totalMs": 100.0,
        }
        (self.output / "generation.jsonl").write_text(
            json.dumps(warmup) + "\n" + json.dumps(measured) + "\n")
        artifacts = []
        completions = (
            "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL",
            "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED",
            "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED",
        )
        for index, completion in enumerate(completions):
            digest = "sha256:" + str(index + 1) * 64
            role = f"/LLM/Pipeline/Stage/{index}"
            snapshot = {
                "providerBootEpoch": f"boot-{index}",
                "records": [{
                    "partitionDigest": plan,
                    "backend": "transformers-cpu",
                }],
            }
            (self.output / f"stage{index}-provider.log").write_text("\n".join((
                "NAC_ABE_BOOTSTRAP provider=/provider authority=/controller",
                "Installed provider permission provider=/provider service=/AI/LLM",
                f"LLM_PIPELINE_QWEN_RUNTIME_READY requestId={request} role={role} device=cpu artifactDigest={digest} loadCompleted=true warmupCompleted=true cpuFallbackCount=0 deviceClass=CPU_LOGIC",
                f"LLM_PIPELINE_QWEN_RESIDENCY_SNAPSHOT requestId={request} role={role} snapshot={json.dumps(snapshot, separators=(',', ':'))}",
                f"LLM_PIPELINE_QWEN_RUNTIME_READY requestId={request}-measured role={role} device=cpu artifactDigest={digest} loadCompleted=true warmupCompleted=true cpuFallbackCount=0 deviceClass=CPU_LOGIC",
                f"LLM_PIPELINE_QWEN_RESIDENCY_SNAPSHOT requestId={request}-measured role={role} snapshot={json.dumps(snapshot, separators=(',', ':'))}",
                f"NDNSF_DI_PROVIDER_HANDLER_TIMING event=start session={request} role={role}",
                completion,
            )))
            artifacts.append({
                "stageIndex": index,
                "fileSha256": digest,
                "fileBytes": 1024 + index,
            })
        (self.output / "repo-registration.json").write_text(json.dumps({
            "artifacts": artifacts,
        }))
        roles = (
            "controller", "repository", "user", "provider-stage-0",
            "provider-stage-1", "provider-stage-2",
        )
        manifest = runtime_evidence.write_spec168_runtime_evidence(
            self.output,
            source_digest=source,
            model_identity_digest=model,
            workload_digest=workload,
            process_rows=[
                {"role": role, "pid": 3000 + index}
                for index, role in enumerate(roles)
            ],
            route_snapshot="/example nexthop=1\n",
            admission_path=self.output / "runtime-admission.json",
        )
        self.assertTrue((self.output / "runtime-admission.json").is_file())
        result = gate.validate_runtime_manifest(
            self.output, manifest, mode="real-minindn")
        self.assertEqual(result["tokenCount"], 4)
        self.assertEqual(result["deviceClass"], "CPU_LOGIC")
        self.assertEqual(len(manifest["observedInvocations"]), 2)
        self.assertEqual(
            [item["phase"] for item in manifest["observedInvocations"]],
            ["warmup", "measured"],
        )

    def test_forbidden_shortcuts_fail_closed(self):
        cases = {
            "mock": ("adapter", "mocked", True),
            "shared payload": (
                "artifactDelivery", "sharedFilesystemPayloadInjection", True),
            "fixed wait": ("readiness", "fixedSettleWaitMs", 300_000),
            "test identity": ("security", "testOnlyIdentities", True),
            "host NFD": ("network", "hostNfdUsed", True),
            "per-token request": ("invocation", "tokenRequestCount", 8),
        }
        for name, (section, key, value) in cases.items():
            with self.subTest(name=name):
                payload = deepcopy(self.payload)
                payload[section][key] = value
                with self.assertRaises(gate.GateError):
                    gate.validate_runtime_manifest(
                        self.output, payload, mode="real-minindn")

    def test_retained_runtime_gate_reuses_evidence_without_reexecution(self):
        source = "sha256:" + "c" * 64
        (self.output / "runtime-admission.json").write_text(json.dumps({
            **self.payload, "sourceDigest": source,
        }))
        (self.output / "gate-manifest.json").write_text(json.dumps({
            "status": "BLOCK", "failureCode": "EXEC_LOCAL_GATE_PROCESS_FAILED",
        }))
        (self.output / "launcher.log").write_text("completed runtime\n")
        admitted = retained_gate.admit(
            self.output, self.output / "retained",
            expected_source_digest=source,
        )
        self.assertEqual(admitted["status"], "PASS")
        self.assertTrue(admitted["retainedEvidence"])
        self.assertFalse(admitted["runtimeReexecuted"])
        with self.assertRaises(retained_gate.gate.GateError):
            retained_gate.admit(
                self.output, self.output / "wrong-source",
                expected_source_digest="sha256:" + "d" * 64,
            )

    def test_independent_process_and_evidence_requirements_fail_closed(self):
        payload = deepcopy(self.payload)
        payload["processes"][1]["pid"] = payload["processes"][0]["pid"]
        with self.assertRaisesRegex(gate.GateError, "independent"):
            gate.validate_runtime_manifest(
                self.output, payload, mode="real-minindn")
        (self.output / "lifecycle.jsonl").unlink()
        with self.assertRaises(gate.GateError):
            gate.validate_runtime_manifest(
                self.output, self.payload, mode="real-minindn")

    def test_adapter_readiness_requires_exact_cuda_load_and_warmup(self):
        cases = {
            "missing role": lambda value: value["adapter"]["assignments"].pop(),
            "cpu device": lambda value: value["adapter"]["assignments"][0].update(
                device="cpu"),
            "not loaded": lambda value: value["adapter"]["assignments"][0].update(
                loadCompleted=False),
            "not warmed": lambda value: value["adapter"]["assignments"][0].update(
                warmupCompleted=False),
            "fallback": lambda value: value["adapter"]["assignments"][0].update(
                cpuFallbackCount=1),
            "bad digest": lambda value: value["adapter"]["assignments"][0].update(
                artifactDigest="sha256:" + "f" * 64),
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                payload = deepcopy(self.payload)
                mutate(payload)
                with self.assertRaises(gate.GateError):
                    gate.validate_runtime_manifest(
                        self.output, payload, mode="real-minindn")

    def test_explicit_cpu_logic_gate_is_not_mislabeled_as_gpu_or_fallback(self):
        payload = deepcopy(self.payload)
        payload["adapter"]["backend"] = "transformers-cpu"
        payload["adapter"]["deviceClass"] = "CPU_LOGIC"
        for assignment in payload["adapter"]["assignments"]:
            assignment["device"] = "cpu"
        result = gate.validate_runtime_manifest(
            self.output, payload, mode="real-minindn")
        self.assertEqual(result["deviceClass"], "CPU_LOGIC")
        with self.assertRaisesRegex(gate.GateError, "CUDA"):
            gate.validate_runtime_manifest(
                self.output, payload, mode="real-minindn", require_cuda=True)

        payload["adapter"]["assignments"][0]["cpuFallbackCount"] = 1
        with self.assertRaisesRegex(gate.GateError, "fallback"):
            gate.validate_runtime_manifest(
                self.output, payload, mode="real-minindn")

    def test_exact_sif_requires_matching_runtime_binding(self):
        payload = deepcopy(self.payload)
        payload["fidelity"] = "EXACT_SIF"
        payload["container"] = {
            "runtime": "apptainer", "sifDigest": DIGEST}
        gate.validate_runtime_manifest(
            self.output, payload, mode="exact-sif",
            expected_container_digest=DIGEST, require_cuda=True)
        with self.assertRaises(gate.GateError):
            gate.validate_runtime_manifest(
                self.output, payload, mode="exact-sif",
                expected_container_digest="sha256:" + "f" * 64)

    def test_runner_retains_block_when_runtime_omits_evidence(self):
        output = self.output / "missing-evidence"
        result = subprocess.run([
            "python3", str(JOBS / "spec168_local_gate.py"),
            "--mode", "real-minindn", "--output-dir", str(output),
            "--hard-timeout-s", "5", "--", "/bin/true",
        ], cwd=REPO, text=True, stdout=subprocess.PIPE,
           stderr=subprocess.STDOUT, check=False)
        self.assertEqual(result.returncode, 1, result.stdout)
        manifest = json.loads((output / "gate-manifest.json").read_text())
        self.assertEqual(manifest["status"], "BLOCK")
        self.assertEqual(manifest["failureCode"],
                         "ANALYZER_RUNTIME_MANIFEST_MISSING")
        self.assertFalse(manifest["automaticRetry"])
        checkpoint = json.loads((output / "gate-checkpoint.json").read_text())
        self.assertEqual(checkpoint["status"], "FINALIZED")
        self.assertEqual(checkpoint["gateStatus"], "BLOCK")
        self.assertEqual(checkpoint["command"], ["/bin/true"])
        self.assertEqual(
            checkpoint["commandDigest"], manifest["commandDigest"])

    def test_shell_launchers_have_no_settle_sleep_or_mock_path(self):
        mini = (JOBS / "run-real-minindn-gate.sh").read_text()
        exact = (JOBS / "run-exact-container-gate.sh").read_text()
        experiment = (REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py").read_text()
        self.assertNotIn("sleep ", mini + exact)
        self.assertNotIn("mock", mini.lower() + exact.lower())
        self.assertIn("apptainer exec --containall", exact)
        self.assertIn("spec168_local_gate.py", mini)
        self.assertIn('"--provider-wait-s", type=float, default=0.0', experiment)
        self.assertNotIn("time.sleep(args.provider_wait_s)", experiment)
        self.assertIn('"--controller-wait-s", type=float, default=0.0', experiment)
        self.assertNotIn("time.sleep(args.controller_wait_s)", experiment)
        self.assertIn('wait_log(controller_log, "controller ready"', experiment)
        self.assertIn("NDNSF_SPEC168_REQUIRE_CUDA", mini)
        self.assertIn("NDNSF_SPEC168_REQUIRE_CUDA", exact)
        self.assertIn("--require-cuda", mini + exact)

    def test_minindn_node_commands_pin_candidate_runtime_closure(self):
        experiment = (
            REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        ).read_text()
        self.assertIn(
            "entries = inherited_entries + source_entries", experiment)
        self.assertIn(
            'prefer_runtime_closure = bool(os.environ.get("SPEC168_OVERLAY_ROOT"))',
            experiment,
        )
        self.assertIn(
            'or os.environ.get("NDNSF_PREFER_INSTALLED_NATIVE") == "1"',
            experiment,
        )
        self.assertIn(
            '"PYTHONNOUSERSITE": "1"', experiment)
        self.assertIn(
            "base = python_process_prefix(base_env)", experiment)
        self.assertNotIn(
            'base = f"cd {perf.shell_quote(REPO)} && exec python3 "',
            experiment,
        )

        overlay_entrypoint = (
            JOBS / "spec168-overlay-entrypoint.sh"
        ).read_text()
        self.assertIn(
            "from py_repoclient import AdaptiveArtifactTransfer",
            overlay_entrypoint,
        )
        self.assertIn("SPEC168_REPO_ABI_CLOSURE_PASS", overlay_entrypoint)

    def test_selection_dataflow_enables_authenticated_targeted_prefetch(self):
        experiment = (
            REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
        ).read_text()
        self.assertRegex(
            experiment,
            r'if args\.selection_dataflow_v2:\n'
            r'(?:        #.*\n)+'
            r'        base_env\["NDNSF_SELECTION_TARGETED_PREFETCH"\] = "1"',
        )
        self.assertIn(
            'closure["NDNSF_SELECTION_TARGETED_PREFETCH"] = '
            'base_env["NDNSF_SELECTION_TARGETED_PREFETCH"]',
            experiment,
        )
        self.assertIn(
            'if os.environ.get("NDNSF_SPEC168_SELECTION_TRACE") == "1":',
            experiment,
        )
        self.assertIn(
            'base_env["NDN_LOG"] = "ndn_service_framework.*=TRACE"',
            experiment,
        )
        self.assertIn('"NDN_LOG": base_env["NDN_LOG"]', experiment)


if __name__ == "__main__":
    unittest.main()
