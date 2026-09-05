"""Actual Python/C++ storage wire interoperability; no network qualification."""
import hashlib
import json
from pathlib import Path
import subprocess

import pytest

from ndnsf_distributed_inference.core.protected_artifacts import (
    AssembledCiphertextV1, decrypt_assembled_entry, encrypt_assembled_entry,
)

ROOT = Path(__file__).resolve().parents[2]
CONTEXT = dict(model_manifest_digest="sha256:" + "a" * 64,
               role_assembly_spec_digest="sha256:" + "b" * 64,
               storage_profile_digest="sha256:" + "c" * 64)
KEY = bytes(range(32))


@pytest.fixture(scope="module")
def native_driver(tmp_path_factory):
    target = tmp_path_factory.mktemp("native-storage") / "driver"
    sources = ["tests/fixtures/spec181/native-storage-driver.cpp"] + [
        "NDNSF-DistributedInference/cpp/ndnsf-di/" + name + ".cpp"
        for name in ("NativeProtectedArtifactStore", "ProtectedRuntime", "NativeGrantVerifier")]
    subprocess.run(["/usr/bin/g++", "-B/usr/bin", "-std=c++17", "-I.", *sources,
                    "-lcrypto", "-pthread", "-o", str(target)], cwd=ROOT,
                   check=True, capture_output=True, text=True, timeout=120)
    return target


def invoke(driver, mode, kind, source, target, limit, **changes):
    context = dict(CONTEXT, **changes)
    return subprocess.run([str(driver), mode, kind, *context.values(),
                           str(source), str(target), str(limit)],
                          capture_output=True, text=True, timeout=10)


@pytest.mark.parametrize("kind", ["MODEL_PROTO", "EXTERNAL_DATA"])
def test_python_and_native_disk_ciphertext_interoperate(native_driver, tmp_path, kind):
    plain = bytes(range(256)) * 31 + b"\x00model\xff"
    source, encrypted, loaded = [tmp_path / name for name in ("input", "cipher", "output")]
    source.write_bytes(plain)
    result = invoke(native_driver, "seal", kind, source, encrypted, len(plain))
    assert result.returncode == 0, result.stderr
    assert decrypt_assembled_entry(KEY, AssembledCiphertextV1.from_bytes(
        encrypted.read_bytes()), entry_kind=kind) == plain
    encrypted.write_bytes(encrypt_assembled_entry(KEY, plain, entry_kind=kind,
                                                **CONTEXT).to_bytes())
    result = invoke(native_driver, "open", kind, encrypted, loaded, len(plain))
    assert result.returncode == 0, result.stderr
    assert loaded.read_bytes() == plain
    for changes, limit in [({}, len(plain) - 1),
                           ({"model_manifest_digest": "sha256:" + "d" * 64}, len(plain)),
                           ({"role_assembly_spec_digest": "sha256:" + "d" * 64}, len(plain)),
                           ({"storage_profile_digest": "sha256:" + "d" * 64}, len(plain))]:
        loaded.unlink(missing_ok=True)
        result = invoke(native_driver, "open", kind, encrypted, loaded, limit, **changes)
        assert result.returncode == 1
        assert "DI_PROTECTED_GRANT_REJECTED" in result.stderr
        assert not loaded.exists()


@pytest.mark.parametrize("assembly", [
    {"role": "stage0", "inputs": [1, {"name": "a}b\\\""}]},
    {"name": "中文", "limits": {"maxAssembledBytes": 42}, "optional": None},
])
def test_native_hashes_exact_python_canonical_assembly(native_driver, assembly):
    canonical = lambda obj: json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    projection = canonical({"a": {"assembly": {}}, "assembly": assembly, "z": 1})
    result = subprocess.run([str(native_driver), "assembly-digest", projection],
                            capture_output=True, text=True, check=True)
    assert result.stdout == "sha256:" + hashlib.sha256(canonical(assembly).encode()).hexdigest()


@pytest.mark.parametrize("field,value", [
    ("aead", "AES-128-GCM"), ("kdf", "unknown"), ("unexpected", "ignored"),
    ("ciphertextLength", "21"),
])
def test_both_readers_reject_manifest_contract_substitution(native_driver, tmp_path, field, value):
    sealed = encrypt_assembled_entry(KEY, b"model", entry_kind="MODEL_PROTO", **CONTEXT)
    manifest = dict(sealed.manifest(), **{field: value})
    header = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    wire = len(header).to_bytes(8, "big") + header + sealed.ciphertext
    with pytest.raises(ValueError):
        AssembledCiphertextV1.from_bytes(wire)
    source, target = tmp_path / "cipher", tmp_path / "plain"
    source.write_bytes(wire)
    result = invoke(native_driver, "open", "MODEL_PROTO", source, target, 5)
    assert result.returncode == 1
    assert "DI_PROTECTED_GRANT_REJECTED" in result.stderr
    assert not target.exists()
