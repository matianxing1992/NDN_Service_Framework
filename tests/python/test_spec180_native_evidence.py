"""Compile actual evidence/identity helpers; synthetic CUDA is not GPU proof."""
import json
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/spec180"


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    output = tmp_path_factory.mktemp("native-evidence")
    binary = output / "probe"
    subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), "-I/usr/local/include",
                    str(FIXTURES / "native-evidence-probe.cpp"),
                    str(ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.cpp"),
                    "-ldl", "-pthread", "-o", str(binary)], check=True, capture_output=True)
    library = output / "libcuda.so.1"
    subprocess.run(["g++", "-shared", "-fPIC", str(FIXTURES / "fake-cuda-identity.cpp"),
                    "-o", str(library)], check=True, capture_output=True)
    (output / "libcudart.so.12").symlink_to(library)
    return binary, output


def invoke(probe, *args, failure=""):
    binary, libraries = probe
    env = dict(os.environ, LD_LIBRARY_PATH=str(libraries), TEST_CUDA_FAILURE=failure,
               NDNSF_DI_GPU_UUID="GPU-forged", CUDA_VISIBLE_DEVICES="0")
    return subprocess.run([str(binary), *args], env=env, text=True, capture_output=True, timeout=5)


def test_selected_runtime_device_maps_to_driver_uuid_not_environment(probe):
    result = invoke(probe, "0")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "GPU-00010203-0405-0607-0809-0a0b0c0d0e0f"


@pytest.mark.parametrize("failure", ["pci", "init", "lookup", "uuid", "zero"])
def test_device_query_fails_closed(probe, failure):
    result = invoke(probe, "0", failure=failure)
    assert result.returncode == 7 and "GPU-" not in result.stdout


@pytest.mark.parametrize("ordinal", ["-1", "1"])
def test_device_query_rejects_invalid_ordinal(probe, ordinal):
    assert invoke(probe, ordinal).returncode == 7


@pytest.mark.parametrize("cached", [False, True])
def test_evidence_roundtrip_preserves_observed_request_and_cache(probe, tmp_path, cached):
    profile = tmp_path / "profile.json"
    profile.write_text(json.dumps([{"cat": "Node", "name": "Conv_kernel_time", "args": {"provider": "CUDAExecutionProvider"}}]))
    result = invoke(probe, str(profile), "cache" if cached else "compute")
    assert result.returncode == 0, result.stderr
    evidence = json.loads(result.stdout)
    assert int(evidence["processId"]) > 0
    assert evidence["cudaVisibleDevices"] == "0"
    assert evidence["requestId"] == evidence["profileRequestId"] == "/request/A"
    assert int(evidence["attemptEpoch"]) == int(evidence["profileAttemptEpoch"]) == 1
    assert evidence["executionCompleted"] == str(not cached).lower()
    assert evidence["exactForwardCacheHit"] == str(cached).lower()
    assert evidence["gpuIdentitySource"] == "cuda-runtime-pci+driver-uuid"


@pytest.mark.parametrize("provider", ["CPUExecutionProvider", "UnknownProvider", ""])
def test_cuda_profile_rejects_missing_or_wrong_provider(probe, tmp_path, provider):
    profile = tmp_path / "profile.json"
    profile.write_text(json.dumps([{"cat": "Node", "name": "Conv_kernel_time", "args": {"provider": provider}}]))
    assert invoke(probe, str(profile), "compute").returncode == 7


@pytest.mark.parametrize("event_name,expected_exit", [("Hidden_kernel_time", 7), ("Conv_fence_before", 0)])
def test_missing_kernel_provider_cannot_hide_among_cuda_events(probe, tmp_path, event_name, expected_exit):
    profile = tmp_path / "mixed-profile.json"
    profile.write_text(json.dumps([
        {"cat": "Node", "name": "Conv_kernel_time", "args": {"provider": "CUDAExecutionProvider"}},
        {"cat": "Node", "name": event_name, "args": {}},
    ]))
    assert invoke(probe, str(profile), "compute").returncode == expected_exit


def test_v3_production_path_has_profile_and_post_run_observation():
    source = (ROOT / "examples/DI_NativeProviderExecutable.cpp").read_text()
    assembly = source.split("spec = prepareNativeCanonicalOnnxRole(", 1)[1].split("return spec;", 1)[0]
    assert "bindNativeRunnerPreparationContext(spec, projection," in assembly
    preparation = (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.cpp").read_text()
    assert 'spec.metadata["providerProfilePrefix"]' in preparation
    assert 'spec.metadata["profileAfterRequest"] = "true"' in preparation
    assert "NDNSF_DI_GPU_UUID" not in source
    assert "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED " in source
    assert "executionEvidenceToJson(observed)" in source
    worker = (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.cpp").read_text()
    tail = worker.split("result.executionEvidence = runner->executionEvidenceSnapshot();", 1)[1]
    assert "bindExecutionObservation(" in tail
    assert "item.role.requestId, item.role.attemptEpoch" in tail
    assert "result.exactForwardCacheHit" in tail
    adapter = (ROOT / "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp").read_text()
    assert 'options.AddConfigEntry("session.disable_cpu_ep_fallback", "1")' in adapter
    assert "m_evidence->profileRequestId = ctx.requestId" in adapter
    assert "m_evidence->profileAttemptEpoch = ctx.attemptEpoch" in adapter
    assert "queryCudaDeviceUuid(" in adapter
    gate = adapter.split("const bool captureRequestProfile =", 1)[1].split("const auto executionDelayMs", 1)[0]
    assert '(!ctx.requestId.empty() && ctx.attemptEpoch != 0)' in gate
    assert 'm_impl->profilingEnabled && captureRequestProfile' in gate
