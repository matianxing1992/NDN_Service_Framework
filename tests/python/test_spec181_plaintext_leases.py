"""Focused T001 lifecycle tests; real files and buffers, no network claims."""

from pathlib import Path
from unittest.mock import patch

import pytest

from ndnsf_distributed_inference.core.protected_artifacts import PlaintextLeaseRegistry


def test_content_key_lease_stays_in_memory_and_is_overwritten(tmp_path):
    registry = PlaintextLeaseRegistry()
    key = registry.register_secret("content-key", b"k" * 32)
    assert bytes(key) == b"k" * 32
    assert list(tmp_path.iterdir()) == []
    registry.zeroize_all()
    assert bytes(key) == bytes(32)


def test_duplicate_lease_cannot_orphan_the_first_plaintext(tmp_path):
    registry = PlaintextLeaseRegistry()
    first, second = tmp_path / "first", tmp_path / "second"
    registry.register("model", first, b"original")
    with pytest.raises(ValueError, match="duplicate"):
        registry.register("model", second, b"replacement")
    assert first.read_bytes() == b"original"
    assert not second.exists()
    registry.zeroize_all()
    assert not first.exists()


def test_one_cleanup_failure_does_not_leave_other_leases_live(tmp_path):
    registry = PlaintextLeaseRegistry()
    first, second = tmp_path / "first", tmp_path / "second"
    registry.register("first", first, b"first secret")
    registry.register("second", second, b"second secret")
    unlink = Path.unlink

    def fail_first(path, *args, **kwargs):
        if path == first:
            raise PermissionError("injected unlink failure")
        return unlink(path, *args, **kwargs)

    with patch.object(Path, "unlink", fail_first):
        with pytest.raises(RuntimeError, match="zeroization"):
            registry.zeroize_all()
    assert not second.exists()
    assert first.read_bytes() == bytes(len(b"first secret"))
    # A failed removal remains registered; the next close can finish it.
    registry.zeroize_all()
    assert not first.exists()


def test_file_plaintext_is_private_even_with_a_permissive_umask(tmp_path):
    import os
    registry = PlaintextLeaseRegistry()
    path = tmp_path / "plaintext"
    previous = os.umask(0)
    try:
        registry.register("model", path, b"private")
    finally:
        os.umask(previous)
    assert path.stat().st_mode & 0o777 == 0o600
    registry.zeroize_all()


def test_plaintext_registration_rejects_a_symlink_without_touching_target(tmp_path):
    target = tmp_path / "canonical"
    target.write_bytes(b"canonical source")
    link = tmp_path / "lease"
    link.symlink_to(target)
    registry = PlaintextLeaseRegistry()
    with pytest.raises((OSError, ValueError)):
        registry.register("model", link, b"replacement")
    assert target.read_bytes() == b"canonical source"
    registry.zeroize_all()
    assert target.exists()


def test_replacing_a_lease_path_never_zeroizes_the_replacement(tmp_path):
    registry = PlaintextLeaseRegistry()
    path, moved, foreign = (tmp_path / name for name in ("lease", "moved", "foreign"))
    registry.register("model", path, b"owned plaintext")
    path.rename(moved)
    foreign.write_bytes(b"unrelated canonical")
    path.symlink_to(foreign)
    with pytest.raises(RuntimeError, match="zeroization"):
        registry.zeroize_all()
    assert foreign.read_bytes() == b"unrelated canonical"
    assert moved.read_bytes() == bytes(len(b"owned plaintext"))
    path.unlink()
    registry.zeroize_all()


def test_runtime_exception_erases_both_memory_and_file_leases(tmp_path):
    path = tmp_path / "model"
    with pytest.raises(ValueError, match="prepare failed"):
        with PlaintextLeaseRegistry() as registry:
            key = registry.register_secret("key", b"k" * 32)
            registry.register("model", path, b"model bytes")
            raise ValueError("prepare failed")
    assert bytes(key) == bytes(32)
    assert not path.exists()
