"""Exercise backend dispatch through the maintained native Provider executable."""
import json
import os
from pathlib import Path
import runpy
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def invoke_provider(tmp_path, backend, execution_provider="cpu"):
    binary = Path(os.environ.get(
        "SPEC181_NATIVE_PROVIDER_BINARY",
        str(ROOT / "build-system-j2/examples/di-native-provider")))
    assert binary.is_file(), "Build the maintained di-native-provider target first"
    fixture = runpy.run_path(str(
        ROOT / "tests/fixtures/spec170/generate_cpu_onnx_fixture.py"))
    model = tmp_path / "model.onnx"
    fixture["write_model"](model, ["y"], 3)
    service = "/spec181/backend-registration"
    plan = tmp_path / "plan.json"
    plan.write_text(json.dumps({"version": 2, "services": [{
        "service": service, "model": "tiny-add", "roles": ["Compute"],
        "executionPolicy": "DATA_DRIVEN_V2", "dependencies": []}]}))
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"services": [{"name": service,
        "artifacts": [{"role": "Compute", "kind": "onnx-model",
            "backend": backend, "path": str(model),
            "metadata": {"executionProvider": execution_provider}}]}]}))
    home = tmp_path / "home"
    home.mkdir()
    env = dict(os.environ, HOME=str(home),
               NDN_CLIENT_PIB="pib-sqlite3:" + str(tmp_path / "pib"),
               NDN_CLIENT_TPM="tpm-file:" + str(tmp_path / "tpm"),
               NDN_CLIENT_TRANSPORT="unix://" + str(tmp_path / "unused.sock"))
    result = subprocess.run([
        str(binary), "--check-only", "--plan", str(plan),
        "--manifest", str(manifest), "--service", service,
        "--provider", "/spec181/provider", "--roles", "Compute"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
    (tmp_path / "stdout.log").write_text(result.stdout)
    (tmp_path / "stderr.log").write_text(result.stderr)
    return result


@pytest.mark.parametrize("backend", ["onnxruntime", "onnxruntime-cpu"])
def test_public_cpu_backend_loads_and_warms_real_model(tmp_path, backend):
    result = invoke_provider(tmp_path, backend)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "NDNSF_DI_NATIVE_PROVIDER_CHECK_OK" in result.stdout
    prefix = "NDNSF_DI_EXECUTION_EVIDENCE "
    records = [json.loads(line[len(prefix):]) for line in result.stdout.splitlines()
               if line.startswith(prefix)]
    assert len(records) == 1
    assert records[0]["loadCompleted"] == "true"
    assert records[0]["warmupCompleted"] == "true"
    assert records[0]["realCompute"] == "true"
    assert records[0]["runnerKind"] == "onnxruntime-cpu"
    assert records[0]["device"]["kind"] == "cpu"


def test_unknown_backend_still_rejects(tmp_path):
    result = invoke_provider(tmp_path, "unknown-backend")
    assert result.returncode != 0
    assert "no NativeModelRunner backend registered: unknown-backend" in result.stderr
    assert "NDNSF_DI_NATIVE_PROVIDER_CHECK_OK" not in result.stdout


@pytest.mark.parametrize("backend", ["onnxruntime-cpu", "onnxruntime-cuda"])
def test_public_backend_preserves_execution_provider_validation(tmp_path, backend):
    result = invoke_provider(tmp_path, backend, "invalid-provider")
    assert result.returncode != 0
    assert "unsupported ONNX Runtime execution provider: invalid-provider" in result.stderr
    assert "NDNSF_DI_NATIVE_PROVIDER_CHECK_OK" not in result.stdout
