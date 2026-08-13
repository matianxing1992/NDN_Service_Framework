#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
JOBS = ROOT / "specs/168-itiger-di-deployment-fidelity/jobs"
ANALYZER = JOBS / "spec168-three-node-analyzer.py"
CANARY_ANALYZER = JOBS / "spec168-control-plane-canary-analyzer.py"
CANARY_PREPARE = JOBS / "spec168-prepare-single-campaign.py"
PREPARE = (ROOT / "specs/162-itiger-qwen36-generation/jobs/"
           "prepare-qwen36.py")


def load_module():
    spec = importlib.util.spec_from_file_location("spec168_remote", ANALYZER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


remote = load_module()


def load_canary_module():
    spec = importlib.util.spec_from_file_location(
        "spec168_control_plane_canary", CANARY_ANALYZER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


canary = load_canary_module()


def load_canary_prepare_module():
    spec = importlib.util.spec_from_file_location(
        "spec168_prepare_single_campaign", CANARY_PREPARE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


canary_prepare = load_canary_prepare_module()


def load_prepare_module():
    spec = importlib.util.spec_from_file_location("spec168_prepare", PREPARE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Spec168ThreeNodeRemoteGateTest(unittest.TestCase):
    def fixture(self, root: Path) -> argparse.Namespace:
        request_id = "spec168-request-one"
        model = "sha256:" + "a" * 64
        workload = "sha256:" + "b" * 64
        source = "sha256:" + "c" * 64
        bundle = "sha256:" + "d" * 64
        sif = "sha256:" + "e" * 64
        native_core = "sha256:" + "1" * 64
        native_extension = "sha256:" + "2" * 64
        plan = "sha256:" + "f" * 64
        stages = [{
            "stageIndex": rank,
            "role": f"/LLM/Pipeline/Stage/{rank}",
            "sha256": "sha256:" + str(rank + 1) * 64,
        } for rank in range(3)]
        stage_path = root / "stage-manifest.json"
        stage_path.write_text(json.dumps({
            "modelDigest": model,
            "layerRanges": [[0, 1], [1, 2], [2, 3]],
            "stages": stages,
        }), encoding="utf-8")
        stage_digest = "sha256:" + hashlib.sha256(stage_path.read_bytes()).hexdigest()
        campaign_path = root / "campaign-manifest.json"
        campaign_path.write_text(json.dumps({
            "schema": "ndnsf-di.spec168-campaign.v3",
            "state": "FROZEN",
            "campaignId": "spec168-campaign-v3-fixture",
            "bindingDigests": {
                "sourceDigest": source,
                "sourceBundleDigest": bundle,
                "runtimeSifDigest": sif,
                "remoteSmallStageManifestDigest": stage_digest,
                "promptSetDigest": workload,
            },
        }), encoding="utf-8")
        generation = {
            "phase": "measured",
            "status": "OK",
            "exactReferenceMatch": True,
            "decodedText": "A complete answer.",
            "generatedTokenIds": [10, 11, 12],
            "stopReason": "EOS",
            "modelIdentityDigest": model,
            "workloadDigest": workload,
            "tokenSteps": [{
                "mode": "FULL",
                "metadata": {
                    "requestId": "/" + request_id,
                    "wireRequestCount": 1,
                    "tokenRequestCount": 0,
                },
            }],
        }
        (root / "automatic-planning.json").write_text(
            json.dumps({"candidateDigest": plan}), encoding="utf-8")
        for rank in range(3):
            node = root / f"node-{rank}"
            node.mkdir()
            (node / "hostname.txt").write_text(f"itiger{rank + 7:02d}\n")
            (node / "gpu.csv").write_text(
                f"GPU-{rank}, NVIDIA RTX 5000 Ada Generation, 555.1, 32760 MiB\n")
            (node / "route-list-after-routes.txt").write_text(
                "/NDNSF-DistributeInference/example\n"
                "/NDNSF/DistributedRepo\n/activation/llm\n")
            (node / "face-list-after-routes.txt").write_text(
                f"faceid={rank + 1} remote=tcp4://10.0.0.{rank + 1}:6363\n")
            lines = [
                "LLM_PIPELINE_PROVIDER_READY",
                f"NDNSF_DI_ACK_DECISION requestId=/{request_id} status=true",
                f"LLM_PIPELINE_QWEN_SELECTION_PREPARE requestId=/{request_id} "
                "device=cuda:0 cpuFallback=false",
                f"LLM_PIPELINE_QWEN_FULL_STAGE_START requestId=/{request_id}",
                "NAC_ABE_BOOTSTRAP complete",
                "Installed provider permission",
            ]
            if rank == 0:
                lines.append(
                    f"LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL requestId=/{request_id}")
            else:
                for epoch in range(len(generation["generatedTokenIds"])):
                    lines.append(
                        "LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED "
                        f"requestId=/{request_id} epoch={epoch}")
                    output = (
                        "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED" if rank == 1 else
                        "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED")
                    lines.append(
                        f"{output} requestId=/{request_id} epoch={epoch}")
            lines.append(
                f"NDNSF_DI_SELECTION_RESERVATION_RELEASED requestId=/{request_id}")
            (node / f"provider-{rank}.log").write_text("\n".join(lines) + "\n")
        (root / "node-0/generation-raw.jsonl").write_text(
            json.dumps(generation) + "\n")
        user_markers = (
            "SPEC162_REQUEST_GATE_OPEN",
            "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
            "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
            "NDNSF_DI_AUTOPLANNING_GRAPH_READY",
            "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
        )
        (root / "node-0/user.log").write_text("\n".join([
            *(f"{marker} requestId=/{request_id}" for marker in user_markers),
            "LLM_PIPELINE_GENERATION_FINAL_RESPONSE status=OK",
            "UserToken/ProviderToken runtime mode: enabled",
            "Installed user permission",
        ]) + "\n")
        (root / "rank-step.log").write_text(
            (("SPEC168_SELECTION_FANOUT_ABI_PASS "
              f"coreSha256={native_core} extensionSha256={native_extension}\n")
             + ("SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS "
                f"coreSha256={native_core} extensionSha256={native_extension}\n")) * 3,
            encoding="utf-8",
        )
        return argparse.Namespace(
            root=str(root),
            stage_manifest=str(stage_path),
            campaign_manifest=str(campaign_path),
            expected_source_digest=source,
            expected_source_bundle_digest=bundle,
            expected_sif_digest=sif,
            expected_native_core_digest=native_core,
            expected_native_extension_digest=native_extension,
            expected_request_id=request_id,
            output_json=str(root / "analysis.json"),
        )

    def test_three_node_contract_accepts_one_bound_full_invocation(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = remote.analyze(self.fixture(Path(temporary)))
        self.assertEqual("PASS", result["status"])
        self.assertEqual(1, result["wireRequestCount"])
        self.assertEqual(0, result["tokenRequestCount"])
        self.assertEqual(3, len(result["providers"]))
        self.assertEqual("REQUEST_FIRST_DATA_DRIVEN_V2", result["readiness"])

    def test_request_id_drift_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.fixture(Path(temporary))
            args.expected_request_id = "different-request"
            with self.assertRaisesRegex(RuntimeError, "RESPONSE_REQUEST_ID_MISMATCH"):
                remote.analyze(args)

    def test_synthetic_wait_ready_without_data_flow_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = self.fixture(root)
            provider = root / "node-1/provider-1.log"
            lines = [
                line for line in provider.read_text().splitlines()
                if "LLM_PIPELINE_QWEN_FULL_HIDDEN_" not in line
            ]
            lines.extend([
                f"LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_WAIT requestId=/{args.expected_request_id}",
                f"LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_READY requestId=/{args.expected_request_id}",
                "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED "
                f"requestId=/{args.expected_request_id} epoch=0",
            ])
            provider.write_text("\n".join(lines) + "\n")
            with self.assertRaisesRegex(
                    RuntimeError, "DEPENDENCY_EVIDENCE_MISSING:rank=1"):
                remote.analyze(args)

    def test_launch_surface_uses_full_copy_up_overlay_and_no_fixed_settle(self):
        rank = (JOBS / "spec168-three-node-rank-inner.sh").read_text()
        outer_rank = (JOBS / "spec168-three-node-rank.sh").read_text()
        sbatch = (JOBS / "gate-e-small-single.sbatch").read_text()
        overlay = (JOBS / "spec168-overlay-entrypoint.sh").read_text()
        gate_c = (JOBS / "gate-c-exact-sif-cuda.sbatch").read_text()
        compat = (ROOT / "specs/162-itiger-qwen36-generation/jobs/"
                  "generation-rank-inner.sh").read_text()
        compat_child = (JOBS / "spec168-compat-child-abi-check.py").read_text()
        self.assertIn("/scratch/python-overlay", rank)
        self.assertIn(
            "exec bash /source/jobs/spec168-overlay-entrypoint.sh", rank)
        self.assertIn("SPEC168_REQUIRE_SELECTION_FANOUT_ABI=1", rank)
        self.assertIn(
            'control_plane_canary="${SPEC168_CONTROL_PLANE_CANARY:-0}"',
            outer_rank,
        )
        self.assertIn("model_bind_args=()", outer_rank)
        self.assertIn("model_env_args=()", outer_rank)
        self.assertIn('cp "$SPEC168_POLICY" "$scratch/policy.yaml"', outer_rank)
        self.assertIn(
            '--env "SPEC168_CONTROL_PLANE_CANARY=${control_plane_canary}"',
            outer_rank,
        )
        self.assertIn("model_bind_args=(--bind", outer_rank)
        self.assertNotIn("cp -a /source/ndnsf/ndnsf/.", rank)
        self.assertIn("--nodes=3", sbatch)
        self.assertIn("spec168-three-node-analyzer.py", sbatch)
        self.assertIn(
            '"partition_digest": manifest["candidateDigest"]', compat)
        self.assertIn('"adapter_id": manifest["adapterId"]', compat)
        self.assertIn(
            '"adapter_version": manifest["adapterVersion"]', compat)
        self.assertNotRegex(compat, r"(^|[;&|])\s*sleep\s+300(?:\.0+)?")
        self.assertIn(
            'LD_LIBRARY_PATH="/opt/ndn-base/lib:${LD_LIBRARY_PATH:-/opt/ndnsf-app/lib}"',
            compat,
        )
        self.assertNotIn(
            'LD_LIBRARY_PATH="/opt/ndn-base/lib:/opt/ndnsf-app/lib:${LD_LIBRARY_PATH:-}"',
            compat,
        )
        self.assertIn("spec168-compat-child-abi-check.py", compat)
        self.assertIn("SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS", compat_child)
        self.assertIn("SPEC168_COMPAT_CHILD_NATIVE_CORE_SHADOWED", compat_child)
        self.assertIn("--request-gate-output", compat)
        self.assertIn("mode=REQUEST_FIRST", (
            ROOT / "examples/python/NDNSF-DistributedInference/"
            "llm_pipeline/user.py").read_text())
        self.assertIn('--model-identity-digest "$model_identity_digest"', compat)
        self.assertIn('--workload-digest "$workload_digest"', compat)
        self.assertIn('chmod -R u+w "$SPEC168_OVERLAY_ROOT"', overlay)
        self.assertIn(
            'LD_LIBRARY_PATH="/source/native/lib:${LD_LIBRARY_PATH:-}"',
            overlay,
        )
        self.assertIn('${PYTHONPATH:+:${PYTHONPATH}}', overlay)
        self.assertIn("SPEC168_NATIVE_ABI_CLOSURE_PASS", overlay)
        self.assertIn("SPEC168_NATIVE_PYTHON_EXTENSION", overlay)
        self.assertIn("SPEC168_SELECTION_FANOUT_ABI_PASS", overlay)
        self.assertIn("NDNSF_SELECTION_PROVIDER_PROJECTION", overlay)
        self.assertIn("SELECTION_TARGETED_PREFETCH_ISSUED", overlay)
        self.assertIn('test ! -e "$SPEC168_OUTPUT_DIR/python-overlay"', gate_c)
        self.assertNotIn(
            'find "$SPEC168_OUTPUT_DIR/python-overlay" -mindepth 1 -delete',
            gate_c,
        )
        self.assertIn("removedSecretFileCount", sbatch)
        self.assertIn("SPEC168_CONTROL_PLANE_CANARY", sbatch)
        self.assertIn("spec168-control-plane-canary-analyzer.py", sbatch)
        self.assertIn('control_plane_canary="${SPEC168_CONTROL_PLANE_CANARY:-0}"', sbatch)
        self.assertIn('if test "$control_plane_canary" = 0; then', sbatch)
        self.assertIn(': "${SPEC168_POLICY:?}"', sbatch)

        self.assertIn("spec168-prepare-single-campaign.py", sbatch)
        self.assertIn("--mode canary", sbatch)

        canary_user = compat.split(
            'if test "$control_plane_canary" = 1; then\n'
            '    /opt/venv/bin/python /source/llm_pipeline/user.py',
            1,
        )[1].split("\n  else", 1)[0]
        self.assertIn("--runtime fake", canary_user)
        self.assertIn("--measured-requests 1", canary_user)
        self.assertNotIn("--generation-campaign-manifest", canary_user)
        self.assertNotIn("--automatic-planning-manifest", canary_user)
        self.assertNotIn("--qwen-stage-manifest", canary_user)
        self.assertNotIn("--repo-registration-output", canary_user)
        self.assertNotIn("--model-identity-digest", canary_user)
        self.assertNotIn("--workload-digest", canary_user)
        self.assertIn("provider_runtime=fake", compat)
        self.assertIn("provider_runtime_args=()", compat)
        self.assertNotIn("provider_runtime_args=(--selection-dataflow-v2)", compat)
        model_setup = compat.split(
            'if test "$control_plane_canary" = 0; then\n'
            '  repo_free_bytes=',
            1,
        )[1].split("\nelse\n  provider_runtime=fake", 1)[0]
        self.assertIn("run-repo-node.py", model_setup)
        self.assertIn("build-automatic-planning-manifest.py", model_setup)
        self.assertIn("native_core_digest=", sbatch)
        self.assertIn("native_extension_digest=", sbatch)
        self.assertIn("--expected-native-core-digest", sbatch)
        self.assertIn("--expected-native-extension-digest", sbatch)

        native_builder = (JOBS / "build-native-closure.sh").read_text()
        self.assertIn('CFLAGS="-O1 -g0"', native_builder)
        self.assertIn('--out="$work_dir/core"', native_builder)
        self.assertIn('NDNSF_LIBRARY_DIR="$work_dir/core"', native_builder)
        self.assertIn('pythonWrapper', native_builder)
        self.assertIn('NDNSF-DistributedRepo/pythonWrapper', native_builder)
        self.assertIn('SPEC168_NATIVE_CLOSURE_PASS', native_builder)

    def test_control_plane_canary_requires_three_projections_without_model_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            request_id = "spec168-canary-one"
            (root / "node-0").mkdir()
            (root / "node-0/user.log").write_text(
                "\n".join([
                    *(f"NDNSF_SELECTION_PROVIDER_PROJECTION requestId=/{request_id} "
                      f"providerName=/provider/{rank}" for rank in range(3)),
                    ('LLM_PIPELINE_USER_RESPONSE phase=measured index=0 '
                     '{"schema":"ndnsf-di-llm-pipeline-response-v1",'
                     '"stageCount":3,"finalRole":"/LLM/Pipeline/Stage/2",'
                     '"lineage":["prompt","/LLM/Pipeline/Stage/0",'
                     '"/LLM/Pipeline/Stage/1","/LLM/Pipeline/Stage/2"],'
                     '"text":"fake distributed LLM response"}'),
                ]) + "\n",
                encoding="utf-8",
            )
            for rank in range(3):
                node = root / f"node-{rank}"
                node.mkdir(exist_ok=True)
                (node / f"provider-{rank}.log").write_text(
                    f"requestId=/{request_id} message=selection received\n"
                    f"requestId=/{request_id} message=collaboration handler running\n",
                    encoding="utf-8",
                )
            (root / "rank-step.log").write_text(
                (("SPEC168_SELECTION_FANOUT_ABI_PASS "
                  "coreSha256=sha256:" + "1" * 64 + " "
                  "extensionSha256=sha256:" + "2" * 64 + "\n")
                 + ("SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS "
                    "coreSha256=sha256:" + "1" * 64 + " "
                    "extensionSha256=sha256:" + "2" * 64 + "\n")) * 3,
                encoding="utf-8",
            )
            result = canary.analyze(
                root, request_id,
                expected_core_digest="sha256:" + "1" * 64,
                expected_extension_digest="sha256:" + "2" * 64,
            )
            self.assertEqual("PASS", result["status"])
            self.assertEqual(0, result["modelWorkCount"])
            self.assertFalse(result["globalPreparationBarrier"])
            self.assertEqual(3, result["responseStageCount"])

            original = (root / "node-0/user.log").read_text(encoding="utf-8")
            (root / "node-0/user.log").write_text(
                original.split("LLM_PIPELINE_USER_RESPONSE", 1)[0]
                + f"LLM_PIPELINE_USER_OK requestId=/{request_id}\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                    RuntimeError, "CANARY_COMPLETE_RESPONSE_MISSING"):
                canary.analyze(
                    root, request_id,
                    expected_core_digest="sha256:" + "1" * 64,
                    expected_extension_digest="sha256:" + "2" * 64,
                )
            (root / "node-0/user.log").write_text(original, encoding="utf-8")

            with (root / "node-2/provider-2.log").open("a", encoding="utf-8") as output:
                output.write("LLM_PIPELINE_QWEN_REPO_FETCH requestId=/bad\n")
            with self.assertRaisesRegex(
                    RuntimeError, "CANARY_MODEL_WORK_DETECTED:rank=2"):
                canary.analyze(
                    root, request_id,
                    expected_core_digest="sha256:" + "1" * 64,
                    expected_extension_digest="sha256:" + "2" * 64,
                )

    def test_control_plane_canary_preflight_opens_no_model_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            campaign = root / "campaign.json"
            campaign.write_text(json.dumps({
                "campaignId": "spec168-campaign-test",
                "bindingDigests": {},
            }), encoding="utf-8")
            output = root / "output"
            args = argparse.Namespace(
                mode="canary",
                campaign_manifest=campaign,
                request_id="spec168-canary-no-model",
                output_dir=output,
                stage_manifest=None,
                generation_template=None,
            )
            canary_prepare.prepare(args)
            self.assertEqual(
                "mode=CONTROL_PLANE_CANARY modelWorkAllowed=false\n",
                (output / "canary-mode.txt").read_text(encoding="utf-8"),
            )
            self.assertFalse((output / "generation-campaign.json").exists())
            self.assertFalse((output / "model-identity.digest").exists())
            self.assertFalse((output / "workload.digest").exists())

            args.stage_manifest = root / "does-not-exist-stage.json"
            with self.assertRaisesRegex(
                    RuntimeError, "SPEC168_CANARY_MODEL_INPUT_FORBIDDEN"):
                canary_prepare.prepare(args)

    def test_outer_rank_canary_expands_no_model_mounts_or_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            shared = root / "shared"
            scratch = root / "slurm-tmp"
            fake_bin = root / "bin"
            for directory in (
                source / "di/ndnsf_distributed_inference",
                source / "ndnsf/ndnsf",
                source / "repo/py_repoclient",
                source / "llm_pipeline",
                source / "compat/spec162",
                source / "jobs",
                shared,
                scratch,
                fake_bin,
            ):
                directory.mkdir(parents=True, exist_ok=True)
            (source / "jobs/nfd.conf.in").write_text(
                "face_system { unix { path @@NFD_SOCKET@@ } }\n@@PORT@@\n",
                encoding="utf-8",
            )
            (source / "compat/spec162/generation-rank-inner.sh").write_text(
                "#!/bin/sh\n", encoding="utf-8"
            )
            policy = root / "policy.yaml"
            policy.write_text("services: []\n", encoding="utf-8")
            sif = root / "runtime.sif"
            sif.write_bytes(b"test")
            args_output = root / "apptainer-args.txt"
            (fake_bin / "nvidia-smi").write_text(
                "#!/bin/sh\nprintf 'gpu-test\\n'\n", encoding="utf-8"
            )
            (fake_bin / "apptainer").write_text(
                "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$APPTAINER_ARGS_OUTPUT\"\n",
                encoding="utf-8",
            )
            for executable in (fake_bin / "nvidia-smi", fake_bin / "apptainer"):
                executable.chmod(0o755)

            environment = dict(os.environ)
            for name in (
                "SPEC168_ARTIFACT_DIR",
                "SPEC168_GENERATION_CAMPAIGN",
                "SPEC168_MODEL_IDENTITY_DIGEST",
                "SPEC168_WORKLOAD_DIGEST",
            ):
                environment.pop(name, None)
            environment.update({
                "PATH": str(fake_bin) + os.pathsep + environment["PATH"],
                "APPTAINER_ARGS_OUTPUT": str(args_output),
                "SLURM_PROCID": "0",
                "SLURM_JOB_ID": "999",
                "SLURM_TMPDIR": str(scratch),
                "SPEC168_SHARED": str(shared),
                "SPEC168_SOURCE_DIR": str(source),
                "SPEC168_SIF": str(sif),
                "SPEC168_PORT": "29999",
                "SPEC168_POLICY": str(policy),
                "SPEC168_REQUEST_ID": "spec168-canary-outer-contract",
                "SPEC168_CONTROL_PLANE_CANARY": "1",
                "SPEC168_REQUEST_TIMEOUT_MS": "60000",
                "SPEC168_ACK_TIMEOUT_MS": "30000",
                "SPEC168_SELECTION_OFFER_LEASE_MS": "120000",
            })
            completed = subprocess.run(
                ["bash", str(JOBS / "spec168-three-node-rank.sh")],
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            invocation = args_output.read_text(encoding="utf-8")
            self.assertIn("SPEC168_CONTROL_PLANE_CANARY=1", invocation)
            self.assertIn("SPEC168_POLICY=/scratch/policy.yaml", invocation)
            self.assertNotIn("SPEC168_ARTIFACT_DIR", invocation)
            self.assertNotIn("SPEC168_GENERATION_CAMPAIGN", invocation)
            self.assertNotIn("SPEC168_MODEL_IDENTITY_DIGEST", invocation)
            self.assertNotIn("SPEC168_WORKLOAD_DIGEST", invocation)

    def test_three_node_launch_closes_cross_node_certificate_matrix_before_request(self):
        compat = (ROOT / "specs/162-itiger-qwen36-generation/jobs/"
                  "generation-rank-inner.sh").read_text()
        user = (ROOT / "examples/python/NDNSF-DistributedInference/"
                "llm_pipeline/user.py").read_text()

        self.assertNotIn('provider-certificate-${rank}.cert', compat)
        self.assertNotIn('fetch_certificate_exact', compat)
        self.assertNotIn('ndnpeek', compat)
        self.assertNotIn('ndnsec cert-install', compat)
        self.assertNotIn('--identity-certificate-output', compat)
        self.assertNotIn('--startup-barrier-file', compat)
        self.assertIn('ON_DEMAND_NETWORK_FETCH', user)
        self.assertNotIn('SLURM_JOB_ID', compat)

    def test_collaboration_selection_uses_bounded_provider_projections(self):
        user_source = (
            ROOT / "ndn-service-framework/ServiceUser.cpp"
        ).read_text(encoding="utf-8")
        provider_source = (
            ROOT / "ndn-service-framework/ServiceProvider.cpp"
        ).read_text(encoding="utf-8")

        self.assertTrue(
            "pendingCall.isCollaboration &&\n"
            "            pendingCall.customSelectedAcks.size() > 1"
            in user_source,
            "multi-provider collaboration must use provider projections",
        )
        self.assertTrue(
            "NDNSF_SELECTION_PROVIDER_PROJECTION" in user_source,
            "projection fanout must be observable",
        )
        self.assertTrue(
            "makeServiceSelectionNameV2(identity, providerName," in user_source,
            "individual Selection must use the provider-specific V2 name",
        )
        self.assertTrue(
            "makeServiceSelectionNameWithoutPrefixV2(providerName," in user_source,
            "individual Selection suffix must retain the provider binding",
        )
        self.assertTrue(
            "makeServiceSelectionNameV2(requesterIdentity, identity," in provider_source,
            "collaboration prefetch must target the local provider projection",
        )
        self.assertIn(
            "if (!providerProjected)", provider_source,
            "generic providers must retain bounded compact multi-selection fallback",
        )
        self.assertIn(
            '"provider-projection"', provider_source,
            "all providers must first prefetch the exact provider-bound decision",
        )
        self.assertIn(
            '"compact-fallback"', provider_source,
            "generic multi-selection must remain source compatible",
        )

    def test_overlay_precedes_installed_package_and_rank_failure_aborts_waits(self):
        outer = (JOBS / "spec168-three-node-rank.sh").read_text()
        compat = (ROOT / "specs/162-itiger-qwen36-generation/jobs/"
                  "generation-rank-inner.sh").read_text()
        self.assertIn(
            'export PYTHONPATH="/source/llm_pipeline:${PYTHONPATH:-/opt/ndnsf-app/python}"',
            compat,
        )
        self.assertNotIn(
            'PYTHONPATH="/source/llm_pipeline:/opt/ndnsf-app/python:${PYTHONPATH:-}"',
            compat,
        )
        self.assertIn("RANK_ABORTED_WHILE_WAITING", compat)
        self.assertIn("touch /shared/rank-abort", compat)
        self.assertIn('touch "$SPEC168_SHARED/rank-abort"', outer)

    def test_repo_policy_reconciles_existing_services_to_repo_identities(self):
        import yaml

        prepare = load_prepare_module()
        with tempfile.TemporaryDirectory() as temporary:
            policy = Path(temporary) / "policy.yaml"
            policy.write_text(yaml.safe_dump({
                "services": [{
                    "name": "/NDNSF/DistributedRepo/Artifact/v2/STORE",
                    "model": "legacy",
                    "users": ["/user"],
                    "providers": [{"identity": "/provider", "roles": []}],
                    "roles": [],
                    "dependencies": [{"legacy": True}],
                }],
            }), encoding="utf-8")
            kwargs = {
                "user": "/user",
                "provider_prefix": "/provider",
                "repo_provider_prefix": "/repo",
            }
            prepare._add_distributed_repo_services(policy, **kwargs)
            first = policy.read_bytes()
            prepare._add_distributed_repo_services(policy, **kwargs)
            self.assertEqual(first, policy.read_bytes())
            document = yaml.safe_load(first)
        expected = {"/repo", "/repo/1", "/repo/2"}
        repo_services = [
            item for item in document["services"]
            if item["name"].startswith("/NDNSF/DistributedRepo/")
        ]
        self.assertTrue(repo_services)
        for service in repo_services:
            self.assertEqual(
                expected,
                {item["identity"] for item in service["providers"]},
                service["name"],
            )
            self.assertTrue(expected.issubset(set(service["users"])))


if __name__ == "__main__":
    unittest.main()
