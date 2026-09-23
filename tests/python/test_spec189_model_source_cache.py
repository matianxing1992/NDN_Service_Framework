from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "spec189_qwen06b_native_minindn", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_model_source_cache_is_digest_namespaced_and_reusable(tmp_path: Path):
    module = load_module()
    identity = module.model_source_cache_identity(
        "Qwen/Qwen3-0.6B", "sha256:manifest", "sha256:source",
        "sha256:graph", "sha256:initializer", "sha256:mapping",
        "sha256:tokenizer")
    repository, namespace = module.resolve_model_source_repository(
        tmp_path / "cache", identity)
    assert namespace.startswith("sha256:")
    assert repository.parent.name == "model-source"
    assert json.loads((repository / "cache-identity.json").read_text()) == identity

    payload = b"canonical source"
    digest = module.digest_bytes(payload)
    target = repository / "payloads" / "sha256" / digest[7:9] / digest[7:]
    target.parent.mkdir(parents=True)
    target.write_bytes(payload)
    assert module.validate_model_source_repository(repository, [(digest, len(payload))])

    target.write_bytes(b"tampered source!")
    with pytest.raises(RuntimeError, match="MODEL_SOURCE_CACHE_DIGEST_MISMATCH"):
        module.validate_model_source_repository(repository, [(digest, len(payload))])


def test_model_source_cache_rejects_identity_reuse(tmp_path: Path):
    module = load_module()
    first = module.model_source_cache_identity(
        "Qwen/Qwen3-0.6B", "sha256:manifest", "sha256:source",
        "sha256:graph", "sha256:initializer", "sha256:mapping",
        "sha256:tokenizer")
    repository, _ = module.resolve_model_source_repository(tmp_path / "cache", first)
    marker = repository / "cache-identity.json"
    marker.write_text(json.dumps({"schema": "wrong"}), encoding="utf-8")
    with pytest.raises(RuntimeError, match="MODEL_SOURCE_CACHE_IDENTITY_MISMATCH"):
        module.resolve_model_source_repository(tmp_path / "cache", first)


def test_external_encrypted_repository_is_removed_without_run_evidence(tmp_path: Path):
    module = load_module()
    run_root = tmp_path / "run"
    run_root.mkdir()
    external = tmp_path / "external-r-test" / "encrypted-repo"
    resolved = module.resolve_encrypted_repository_path(run_root, external)
    (resolved / "payloads").mkdir()
    (resolved / "payloads" / "ciphertext").write_bytes(b"large staging")
    (run_root / "requester").mkdir()
    evidence = run_root / "requester" / "requester.log"
    evidence.write_text("retain raw evidence", encoding="utf-8")

    assert module.cleanup_encrypted_repository(resolved, run_root)
    assert not resolved.exists()
    assert evidence.exists()


def test_assembled_model_budget_covers_native_material_working_set(tmp_path: Path):
    module = load_module()
    stages = [
        {"bytes": 752_094_335},
        {"bytes": 752_097_486},
    ]
    source_bytes = 951_819
    initializer_bytes = 1_503_264_768

    budget = module.assembled_model_budget(
        stages, source_bytes, initializer_bytes)

    assert budget == (4 * 752_097_486 +
                      module.LOCAL_ASSEMBLY_DISK_MARGIN_BYTES)
    assert budget > initializer_bytes
    assert budget < 4 * 1024 * 1024 * 1024


def test_external_encrypted_repository_does_not_clean_live_owner(tmp_path: Path):
    module = load_module()
    run_root = tmp_path / "run"
    run_root.mkdir()
    external = tmp_path / "external-r-live" / "encrypted-repo"
    resolved = module.resolve_encrypted_repository_path(run_root, external)
    (resolved / "payload").write_bytes(b"do not remove")
    with pytest.raises(RuntimeError, match="ENCRYPTED_REPOSITORY_PATH_BUSY"):
        module.resolve_encrypted_repository_path(run_root, external)
    assert (resolved / "payload").exists()


def test_fixed_workspace_cleans_only_disposable_dirs(tmp_path: Path):
    module = load_module()
    work_root = tmp_path / "fixed-workspace"
    (work_root / "ndnsf").mkdir(parents=True)
    (work_root / "ndnsf" / "old-run.log").write_text("discard", encoding="utf-8")
    (work_root / "repo").mkdir()
    (work_root / "repo" / "old-ciphertext").write_bytes(b"discard")
    (work_root / "cache").mkdir()
    reusable = work_root / "cache" / "canonical.onnx"
    reusable.write_bytes(b"reuse")

    ndnsf_root, repo_root, lock = module.prepare_fixed_workspace(work_root)
    try:
        assert ndnsf_root == work_root / "ndnsf"
        assert repo_root == work_root / "repo"
        assert list(ndnsf_root.iterdir()) == []
        assert list(repo_root.iterdir()) == []
        assert reusable.read_bytes() == b"reuse"
    finally:
        module.release_fixed_workspace(lock)


def test_fixed_workspace_refuses_concurrent_cleanup(tmp_path: Path):
    module = load_module()
    first_root, _, first_lock = module.prepare_fixed_workspace(tmp_path / "fixed")
    assert first_root.is_dir()
    try:
        with pytest.raises(RuntimeError, match="FIXED_WORKSPACE_BUSY"):
            module.prepare_fixed_workspace(tmp_path / "fixed")
    finally:
        module.release_fixed_workspace(first_lock)
