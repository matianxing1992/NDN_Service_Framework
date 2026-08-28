from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight"
HOST_PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight"
WORKLOAD = ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json"
EXAMPLES_WSCRIPT = ROOT / "examples/wscript"
SPEC175_JOBS = ROOT / "packaging/ndnsf-di-container/jobs/spec175"
REPLAY_DRIVER = SPEC175_JOBS / "replay-exact-sif.py"
CHECKLIST_VALIDATOR = ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist"
MODEL_PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-spec175-model-preflight"
HOST_GATE = ROOT / "results/spec175/g3/spec175-g3-current-20260827.json"
HOST_GATE_MODULE = ROOT / "packaging/ndnsf-di-container/lib/spec175_host_gate.py"


def load_module():
    loader = importlib.machinery.SourceFileLoader("spec175_preflight", str(PREFLIGHT))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def load_host_gate_module():
    loader = importlib.machinery.SourceFileLoader(
        "spec175_host_gate", str(HOST_GATE_MODULE))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class Spec175SifPreflightTests(unittest.TestCase):
    def test_host_gate_requires_current_42_process_matrix(self):
        if not HOST_GATE.is_file():
            self.skipTest("current G3 host manifest is not present")
        module = load_host_gate_module()
        result = module.validate_host_gate(HOST_GATE, ROOT)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["total"], 42)
        self.assertEqual(result["passed"], 42)

    def test_host_gate_rejects_incomplete_matrix(self):
        if not HOST_GATE.is_file():
            self.skipTest("current G3 host manifest is not present")
        module = load_host_gate_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "host-gate.json"
            value = json.loads(HOST_GATE.read_text(encoding="utf-8"))
            value["matrix"]["entries"] = value["matrix"]["entries"][:-1]
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(module.HostGateError, "ENTRY_COUNT"):
                module.validate_host_gate(path, ROOT)

    def test_tiger_submission_is_frozen_and_cwd_safe(self):
        submit = (SPEC175_JOBS / "submit.sh").read_text(encoding="utf-8")
        runner = (SPEC175_JOBS / "run-streamed-generation.sh").read_text(
            encoding="utf-8")
        self.assertIn("SPEC175_GATE_CLOSURE_NOT_PASS", submit)
        self.assertIn("CHECKLIST_VALIDATOR", submit)
        self.assertIn("PRE_TIGER_CHECKLIST", submit)
        self.assertIn("conversation-residency", submit)
        self.assertIn("control", submit)
        self.assertIn("sha256", submit)
        self.assertIn("sbatch --export=ALL", submit)
        self.assertNotIn("docker", submit.lower())
        self.assertIn("cd /bundle", runner)
        self.assertIn("--cleanenv", runner)
        self.assertIn('--env "SLURM_JOB_ID=${SLURM_JOB_ID:-spec175-local}"', runner)
        self.assertIn('--env "SPEC175_GATE=${SPEC175_GATE}"', runner)
        self.assertIn("run-ndnsf-qwen.sh", runner)
        self.assertIn("SPEC175_SIF_CONTROL_ENTRYPOINT_MISSING", runner)
        self.assertIn("spec175-sif-control-v1", runner)
        self.assertIn("SPEC175_SIF_DIGEST_MISMATCH", runner)
        self.assertIn('SPEC175_JOB_ROOT', submit)
        for name in ("qualify-control.sbatch",
                     "qualify-stage-readiness.sbatch",
                     "qualify-multiprovider.sbatch",
                     "qualify-conversation-residency.sbatch",
                     "qualify-performance.sbatch"):
            text = (SPEC175_JOBS / name).read_text(encoding="utf-8")
            self.assertIn("SPEC175_SIF_SHA256", text)
            self.assertIn("SPEC175_JOB_ROOT", text)
            self.assertNotIn('dirname "$0"', text)
            if name == "qualify-stage-readiness.sbatch":
                self.assertIn("run-qwen-stage-readiness.py", text)
                self.assertIn("--gres=gpu:rtx_5000:3", text)
                self.assertIn("--stage-device-ids", text)
                self.assertIn("SPEC175_MODEL_MANIFEST", text)
                self.assertIn("SPEC175_REMOTE_MODEL_ROOT", text)
                self.assertNotIn("run-streamed-generation.sh", text)
            else:
                self.assertIn("run-streamed-generation.sh", text)

    def test_repository_checklist_validator_is_home_independent(self):
        self.assertTrue(os.access(CHECKLIST_VALIDATOR, os.X_OK))
        text = CHECKLIST_VALIDATOR.read_text(encoding="utf-8")
        self.assertIn('ndnsf-itiger-pre-submit-checklist-v1', text)
        self.assertNotIn('/skills/', text)
        self.assertNotIn('Path.home()', text)
        self.assertIn('--candidate-manifest', text)
        self.assertIn('--expected-sif-sha256', text)

    def test_submit_control_has_no_model_requirement(self):
        submit = (SPEC175_JOBS / "submit.sh").read_text(encoding="utf-8")
        model_block = submit.split('if [[ "$CHECKLIST_GATE" != control ]]; then', 1)[1]
        self.assertIn('MODEL_MANIFEST', model_block)
        self.assertIn('fi', model_block)
        self.assertIn('if [[ "$CHECKLIST_GATE" != control ]]; then', submit)

    def test_model_preflight_rejects_legacy_manifest_before_allocation(self):
        self.assertTrue(os.access(MODEL_PREFLIGHT, os.X_OK))
        result = subprocess.run(
            [str(MODEL_PREFLIGHT), "--manifest", str(ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json")],
            cwd=ROOT, text=True, capture_output=True, check=False,
        )
        assert result.returncode != 0
        assert "SPEC175_MODEL_SCHEMA_REQUIRED" in result.stderr

    def test_model_preflight_accepts_complete_stateful_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stateful-stage-manifest.json"
            state_in = ["attention_kv_in", "recurrent_state_in", "convolution_state_in"]
            state_out = ["attention_kv_out", "recurrent_state_out", "convolution_state_out"]
            stages = []
            ranges = ((0, 21), (21, 42), (42, 64))
            for index, (start, end) in enumerate(ranges):
                stages.append({
                    "stageIndex": index,
                    "role": f"/LLM/Pipeline/Stage/{index}",
                    "layerRange": {"start": start, "endExclusive": end},
                    "sha256": "sha256:" + "a" * 64,
                    "metadata": {
                        "inputNames": ["input_ids", "attention_mask", "position_ids", *state_in],
                        "outputNames": ["hidden_states_out", *state_out],
                        "stateInputNames": state_in,
                        "stateOutputNames": state_out,
                    },
                })
            path.write_text(json.dumps({
                "schemaVersion": "ndnsf-di-qwen36-onnx-stage-manifest-v1",
                "repository": "Qwen/Qwen3.6-27B",
                "revision": "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
                "modelType": "qwen3_5",
                "dtype": "float16",
                "sequencePolicy": "stateful-prefill-decode-v1",
                "decodeMode": "single-token-autoregressive",
                "modality": "text-only",
                "mtpEnabled": False,
                "thinkingMode": "disabled",
                "graphComponents": ["text_embedding", "hybrid_decoder", "lm_head"],
                "layerCount": 64,
                "layerRanges": [[0, 21], [21, 42], [42, 64]],
                "promptLength": 2,
                "promptIds": [1, 2],
                "modelDigest": "sha256:" + "b" * 64,
                "tokenizer": {"digest": "sha256:" + "c" * 64},
                "stages": stages,
            }), encoding="utf-8")
            result = subprocess.run(
                [str(MODEL_PREFLIGHT), "--manifest", str(path)],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout)["status"] == "PASS"

    def test_direct_native_targets_keep_stream_and_epoch_sources(self):
        text = EXAMPLES_WSCRIPT.read_text(encoding="utf-8")
        required = {
            "di-native-provider": (
                "ndn-service-framework/InvocationStream.cpp",
            ),
            "di-native-fault-provider": (
                "ndn-service-framework/InvocationStream.cpp",
            ),
        }
        self.assertIn(
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp",
            text,
        )
        for target, sources in required.items():
            start = text.index(f"bld.program(name='{target}'")
            next_target = text.find("\n    bld.program(", start + 1)
            body = text[start:] if next_target < 0 else text[start:next_target]
            for source in sources:
                self.assertIn(source, body, f"{target} omitted {source}")

    def test_entrypoint_is_executable_and_workload_is_strict(self):
        mode = stat.S_IMODE(PREFLIGHT.stat().st_mode)
        self.assertTrue(mode & stat.S_IXUSR)
        module = load_module()
        value = module.validate_workload(WORKLOAD)
        self.assertEqual(value["model"]["repository"], "Qwen/Qwen3.6-27B")
        self.assertFalse(value["model"]["mtpEnabled"])

    def test_exact_sif_probe_does_not_pull_host_substrate_into_sif(self):
        text = PREFLIGHT.read_text(encoding="utf-8")
        probe = text.split("_RUNTIME_PROBE = r'''", 1)[1].split("'''", 1)[0]
        runtime_section = probe.split("host_substrate_commands =", 1)[0]
        for command in ("mn", "mnexec", "ovs-vswitchd", "ovs-vsctl", "ip", "nlsr"):
            self.assertNotIn(f'"{command}"', runtime_section)
        for command in ("nfd", "nfdc", "ndnsec", "App_ServiceController",
                        "di-native-provider"):
            self.assertIn(f'"{command}"', text)
        for module in ("ndnsf", "py_repoclient", "ndnsf_distributed_inference"):
            self.assertIn(f'"{module}"', text)
        for module in ("minindn", "mininet"):
            self.assertIn(f'"{module}"', text)
        host_section = probe.split("host_substrate_commands =", 1)[1]
        for command in ("mn", "mnexec", "ovs-vswitchd", "ovs-vsctl", "nlsr"):
            self.assertIn(f'"{command}"', host_section)

    def test_host_substrate_preflight_is_a_distinct_executable_layer(self):
        self.assertTrue(os.access(HOST_PREFLIGHT, os.X_OK))
        text = HOST_PREFLIGHT.read_text(encoding="utf-8")
        self.assertIn("host-substrate", text)
        self.assertIn("HOST_NAMESPACE_PRIVILEGE_MISSING", text)
        self.assertIn("SPEC175_RUNTIME_SIF", text)
        self.assertIn("NDNSF_DI_LlmPipeline_Minindn.py", text)
        self.assertIn("SPEC175_SIF_HOST_PROCESS_FALLBACK", text)

    def test_host_replay_driver_matches_minindn_python38_boundary(self):
        text = REPLAY_DRIVER.read_text(encoding="utf-8")
        self.assertIn("WORKLOAD_SEED = 1750001", text)
        self.assertIn("REPETITIONS = 3", text)
        self.assertIn("range(1, REPETITIONS + 1)", text)
        self.assertIn('len(entries) == EXPECTED_ENTRIES', text)
        self.assertIn('"repetition": repetition', text)
        self.assertIn('parser.add_argument("--source-seal", required=True', text)
        self.assertIn("complete candidate source seal", text)
        self.assertIn("PRODUCTION_RUNNER", text)
        self.assertIn("SPEC175_SIF_HOST_PROCESS_FALLBACK", text)
        self.assertIn("without_sha256_prefix", text)
        self.assertNotIn("removeprefix(", text)

    def test_sif_launcher_uses_apptainer_home_mapping(self):
        text = (ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py").read_text(
            encoding="utf-8")
        self.assertIn('"--home"', text)
        self.assertIn('--home "${HOME:-/tmp/minindn}:${HOME:-/tmp/minindn}"', text)
        self.assertNotIn('"--env", perf.shell_quote(f"HOME=', text)
        self.assertNotIn("--env 'HOME=${HOME:-/tmp/minindn}'", text)

    def test_sif_launcher_forwards_node_scoped_controller_certificate(self):
        text = (ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py").read_text(
            encoding="utf-8")
        self.assertIn(
            'NDNSF_CONTROLLER_CERT_FILE="${NDNSF_CONTROLLER_CERT_FILE:-}"',
            text,
        )
        self.assertIn("node_env[REPOSITORY_NODE]", text)

    def test_workload_mutation_is_rejected(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "workload.json"
            value = json.loads(WORKLOAD.read_text())
            value["generation"]["maxGeneratedTokens"] = 32
            path.write_text(json.dumps(value))
            with self.assertRaisesRegex(module.PreflightError, "GENERATION_MISMATCH"):
                module.validate_workload(path)

    def test_source_seal_must_contain_exact_workload(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / "workload.json"
            copied.write_bytes(WORKLOAD.read_bytes())
            with self.assertRaisesRegex(module.PreflightError, "SOURCE_SEAL_INVALID"):
                module.validate_source(ROOT / "missing-source-seal.json", copied)

    def test_cli_applies_source_dependency_boundary_to_prepared_seal(self):
        # Locate a current prepared seal only when one exists; source-only unit
        # tests must remain runnable on clean checkouts without a build output.
        seals = []
        for candidate in sorted(ROOT.glob("**/source-seal.json")):
            try:
                value = json.loads(candidate.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            rows = {row.get("path") for row in value.get("files", []) if isinstance(row, dict)}
            if (value.get("schemaVersion") == "spec170-local-sif-source-v1" and
                    "packaging/ndnsf-di-container/jobs/spec175/workload.json" in rows):
                # Copied records under local scratch roots may retain an operational
                # path to a temporary archive that is no longer beside the
                # seal.  Such a record is not an independently valid source
                # subject and must not poison this source-only regression.
                archive = candidate.parent / "workspace.tar"
                recorded_archive = value.get("archive", {}).get("path")
                if (archive.is_file() and isinstance(recorded_archive, str)
                        and Path(recorded_archive).resolve() == archive.resolve()):
                    seals.append(candidate)
        # The glob also finds unrelated historical seals.  A prepared current
        # Spec170 seal is recognized by the validator's schema constant.
        if not seals:
            self.skipTest("no prepared local SIF source seal")
        result = subprocess.run(
            [str(PREFLIGHT), "--source-seal", str(seals[-1]),
             "--workload", str(WORKLOAD)],
            text=True, capture_output=True, check=False)
        selected = json.loads(seals[-1].read_text(encoding="utf-8"))
        leaked = {"miniNdn", "nlsr", "ndnTools", "infoedit"}.intersection(
            selected.get("dependencies", {})
        )
        if leaked:
            self.assertEqual(result.returncode, 4, result.stderr)
            self.assertIn("SPEC175_SOURCE_HOST_SUBSTRATE_DEPENDENCY", result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('"status": "PASS"', result.stdout)

    def test_sif_runtime_probe_rejects_forbidden_deployment_module(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sif = root / "candidate.sif"
            sif.write_bytes(b"candidate")
            runtime = {
                "status": "FAIL",
                "python": "3.10.18",
                "extension": "/opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so",
                "forbiddenModules": {"functorch": "/opt/venv/lib/python3.10/site-packages/functorch/__init__.py"},
                "onnxruntime": {"providers": ["CUDAExecutionProvider"]},
                "ldd": {"extension": {"state": "PASS"}, "provider": {"state": "PASS"}},
            }
            apptainer = root / "apptainer"
            apptainer.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = inspect ]; then "
                "echo '{\"data\":{\"attributes\":{\"labels\":{\"org.ndnsf.di.build-boundary\":\"container-runtime-in-sif\"}}}}'; exit 0; fi\n"
                "if [ \"$1\" = exec ]; then "
                "printf '%s\\n' '" + json.dumps(runtime, separators=(",", ":")) + "'; exit 0; fi\n"
                "exit 97\n",
                encoding="utf-8",
            )
            apptainer.chmod(0o755)
            with self.assertRaisesRegex(module.PreflightError, "RUNTIME_INVALID"):
                module.validate_sif(sif, str(apptainer), None)

    def test_sif_source_seal_label_must_match_source_subject(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sif = root / "candidate.sif"
            sif.write_bytes(b"candidate")
            apptainer = root / "apptainer"
            apptainer.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = inspect ]; then "
                "echo '{\"data\":{\"attributes\":{\"labels\":{\"org.ndnsf.di.build-boundary\":\"container-runtime-in-sif\",\"org.ndnsf.di.source-seal\":\"sha256:old\"}}}}'; exit 0; fi\n"
                "exit 97\n",
                encoding="utf-8",
            )
            apptainer.chmod(0o755)
            with self.assertRaisesRegex(module.PreflightError, "SOURCE_SEAL_LABEL_MISMATCH"):
                module.validate_sif(sif, str(apptainer), None, "sha256:new")


if __name__ == "__main__":
    unittest.main()
