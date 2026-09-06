"""Fixed-vector parity through Python and the actual C++ production entry.

The native format operation uses its normal subprocess helper. These tests
prove entry/serialization/fetch/cache parity, not two independent algorithms
or network/grant qualification.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest
import numpy as np
import onnxruntime as ort

from ndnsf_distributed_inference.adapters.onnx.executor import (
    CertifiedOnnxAssemblyRecipe, assemble_certified_onnx_model)
from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "tests/fixtures/spec181/assembly-vectors-v1.json"
CASES = json.loads(VECTORS.read_text())["cases"]


def python_assemble(case):
    return assemble_certified_onnx_model(
        bytes.fromhex(case["canonicalModelHex"]),
        canonical_initializer=bytes.fromhex(case["initializerHex"]) if case["initializerHex"] else None,
        role_spec=RoleAssemblySpec(**case["role"]),
        recipe=CertifiedOnnxAssemblyRecipe(**case["recipe"]))


@pytest.mark.parametrize("case", CASES, ids=lambda case: case["id"])
def test_python_fixed_assembly_vector(case):
    if case["expected"]["accepted"]:
        assembled = python_assemble(case)
        assert assembled.model_bytes == bytes.fromhex(case["expected"]["modelHex"])
        assert assembled.model_digest == case["expected"]["modelDigest"]
    else:
        reason = "initializer digest mismatch" if case["expected"]["boundary"] == "INITIALIZER_IDENTITY" else "RoleAssemblySpec"
        with pytest.raises(ValueError, match=reason):
            python_assemble(case)


@pytest.mark.parametrize("case", CASES, ids=lambda case: case["id"])
def test_native_production_entry_matches_fixed_vector_and_python(case, tmp_path):
    binary = os.environ.get("SPEC181_ASSEMBLY_PARITY_BINARY", "")
    assert binary and Path(binary).is_file(), "build and set SPEC181_ASSEMBLY_PARITY_BINARY; no mock fallback"
    request = tmp_path / "case.json"
    request.write_text(json.dumps(case))
    cache = tmp_path / "cache"
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = os.pathsep.join([
        str(Path(binary).resolve().parent), env.get("LD_LIBRARY_PATH", "")])
    env["PYTHONPATH"] = os.pathsep.join([str(ROOT / "NDNSF-DistributedInference"),
        str(ROOT / "pythonWrapper"), str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
        env.get("PYTHONPATH", "")])
    completed = subprocess.run([binary, str(request), str(cache), sys.executable],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=30)
    assert completed.stdout.strip(), completed.stderr
    result = json.loads(completed.stdout)
    if case["expected"]["accepted"]:
        assert completed.returncode == 0, result
        assert result["status"] == "ASSEMBLED"
        native = Path(result["modelPath"]).read_bytes()
        assert native == bytes.fromhex(case["expected"]["modelHex"])
        assert native == python_assemble(case).model_bytes
        assert result["modelDigest"] == case["expected"]["modelDigest"]
        assert result["modelDigest"] == "sha256:" + hashlib.sha256(native).hexdigest()
        session = ort.InferenceSession(native, providers=["CPUExecutionProvider"])
        observed = session.run(None, {"x": np.asarray([[3.0]], dtype=np.float32)})
        expected = 7.0 if case["recipe"]["role_kind"] == "COMPONENT_SET" else 6.0
        np.testing.assert_array_equal(observed[0], np.asarray([[expected]], dtype=np.float32))
        assert len(list(cache.rglob("manifest.signature"))) == 1
    else:
        assert completed.returncode == 2, result
        assert result["status"] == "REJECTED"
        reason = "initializer digest mismatch" if case["expected"]["boundary"] == "INITIALIZER_IDENTITY" else "RoleAssemblySpec"
        assert reason in result["reason"], result
        assert not list(cache.rglob("model.onnx"))
        assert not list(cache.rglob("manifest.signature"))
    assert not list((cache / ".staging").glob("assembly-*"))
