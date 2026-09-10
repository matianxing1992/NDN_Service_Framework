"""Content freezing tests. Small text fixtures are not qualified runtimes."""
import hashlib
import json
import os
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def fixture_manifest(root):
    from runtime.yolo_bundle import REQUIRED_HARNESS_FILES
    rows = {}
    for name in sorted(REQUIRED_HARNESS_FILES):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = b"{}\n" if path.suffix == ".json" else b"# fixture only, not an executable release\n"
        path.write_bytes(payload)
        rows[name] = {"bytes": len(payload), "sha256": "sha256:" + hashlib.sha256(payload).hexdigest()}
    manifest = root / "harness-manifest.json"
    manifest.write_text(json.dumps({"schema": "tiger-yolo-harness-v1", "files": rows}))
    return manifest, "sha256:" + hashlib.sha256(manifest.read_bytes()).hexdigest()


def test_freeze_is_independent_of_later_source_edits(tmp_path):
    from runtime.yolo_bundle import freeze_harness, verify_harness
    source = tmp_path / "source"
    manifest, expected = fixture_manifest(source)
    output = tmp_path / "frozen"
    result = freeze_harness(manifest, output, expected_manifest_sha256=expected)
    assert result["integrity"] == "VERIFIED"
    assert result["qualification"] == "NOT_EVALUATED"
    original = (output / "runtime/baseline.py").read_bytes()
    (source / "runtime/baseline.py").write_text("# changed source\n")
    assert (output / "runtime/baseline.py").read_bytes() == original
    assert verify_harness(output, expected_manifest_sha256=expected) == result
    assert (output.stat().st_mode & 0o222) == 0


def test_frozen_slurm_wrapper_remains_executable_read_only(tmp_path):
    from runtime.yolo_bundle import freeze_harness
    source = tmp_path / "source"
    manifest, expected = fixture_manifest(source)
    output = tmp_path / "frozen"
    freeze_harness(manifest, output, expected_manifest_sha256=expected)
    wrapper = output / "jobs/yolo/run.sbatch"
    assert wrapper.stat().st_mode & 0o777 == 0o555
    assert os.access(wrapper, os.X_OK)
    assert (output / "runtime/baseline.py").stat().st_mode & 0o777 == 0o444


def test_real_frozen_reference_owner_has_no_native_import_dependency(tmp_path):
    import subprocess
    from tools.spec183_dispatch_plane import _sealed_harness
    from runtime.yolo_bundle import reference_owner, tensor_bundle_owner, verify_harness, harness_source
    _sealed_harness(tmp_path)
    frozen = tmp_path / 'harness'
    owner = reference_owner(frozen)
    assert frozen in Path(owner.__file__).parents
    assert owner.FIXTURE_PATH == 'tests/fixtures/spec180/yolo26n/fixed-fixture.ppm'
    assert (frozen / 'owners/yolo_reference.py').read_bytes() == harness_source(ROOT, 'owners/yolo_reference.py').read_bytes()
    assert 'ndnsf' not in owner.__dict__
    decoder = tensor_bundle_owner(frozen)
    assert frozen in Path(decoder.__file__).parents
    assert decoder.decode_tensor_bundle(b'NDITB001\0\0\0\0') == {}
    assert (frozen / 'owners/yolo_tensor_bundle.py').read_bytes() == harness_source(ROOT, 'owners/yolo_tensor_bundle.py').read_bytes()
    manifest = frozen / 'harness-manifest.json'
    verify_harness(frozen, expected_manifest_sha256='sha256:' + hashlib.sha256(manifest.read_bytes()).hexdigest())
    # A fresh interpreter has no test-suite sys.modules/package-path help.
    program = '''
import builtins, sys
sys.path.insert(0, sys.argv[1])
original = builtins.__import__
def checked(name, *args, **kwargs):
    if name.startswith(('ndnsf', 'py_repoclient')):
        raise AssertionError('UNDECLARED_NATIVE_DI_IMPORT:' + name)
    return original(name, *args, **kwargs)
builtins.__import__ = checked
from runtime.yolo_bundle import reference_owner, tensor_bundle_owner
assert reference_owner().ATOL == 1e-3
assert tensor_bundle_owner().decode_tensor_bundle(b'NDITB001' + bytes(4)) == {}
print('FROZEN_NUMPY_OWNERS_OK')
'''
    result = subprocess.run([sys.executable, '-I', '-B', '-c', program, str(frozen)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == 'FROZEN_NUMPY_OWNERS_OK'


@pytest.mark.parametrize("mutation", ["missing", "extra", "escape", "boolean", "hash",
                                      "symlink", "binary", "private-pem", "oversize"])
def test_invalid_sources_create_no_destination(tmp_path, mutation):
    from runtime.yolo_bundle import freeze_harness, MAX_FILE_BYTES
    from runtime.yolo_profile import ClosureError
    manifest, _ = fixture_manifest(tmp_path / "source")
    value = json.loads(manifest.read_text())
    file = manifest.parent / "runtime/baseline.py"
    row = value["files"]["runtime/baseline.py"]
    if mutation == "missing": del value["files"]["runtime/baseline.py"]
    elif mutation == "extra": value["files"]["oracle/result.npy"] = row
    elif mutation == "escape": value["files"]["../escaped.py"] = row
    elif mutation == "boolean": row["bytes"] = True
    elif mutation == "hash": file.write_bytes(b"x" * file.stat().st_size)
    elif mutation == "symlink":
        saved = file.with_suffix(".saved")
        file.rename(saved)
        file.symlink_to(saved)
    elif mutation in ("binary", "private-pem"):
        payload = b"\x7fELF\x00" if mutation == "binary" else b"-----BEGIN PRIVATE KEY-----\nfixture-not-a-key\n"
        file.write_bytes(payload)
        row.update(bytes=len(payload), sha256="sha256:" + hashlib.sha256(payload).hexdigest())
    elif mutation == "oversize": row["bytes"] = MAX_FILE_BYTES + 1
    manifest.write_text(json.dumps(value))
    expected = "sha256:" + hashlib.sha256(manifest.read_bytes()).hexdigest()
    with pytest.raises(ClosureError):
        freeze_harness(manifest, tmp_path / "frozen", expected_manifest_sha256=expected)
    assert not (tmp_path / "frozen").exists()


@pytest.mark.parametrize("mutation", ["extra-file", "extra-tree", "writable", "changed"])
def test_frozen_bundle_rejects_later_mutations(tmp_path, mutation):
    from runtime.yolo_bundle import freeze_harness, verify_harness
    from runtime.yolo_profile import ClosureError
    manifest, expected = fixture_manifest(tmp_path / "source")
    root = tmp_path / "frozen"
    freeze_harness(manifest, root, expected_manifest_sha256=expected)
    auditing = [False]
    if mutation.startswith("extra"):
        root.chmod(0o755)
        if mutation == "extra-file":
            (root / "untracked.so").write_bytes(b"not a real library")
            (root / "untracked.so").chmod(0o444)
        else:
            (root / "unregistered-tree").mkdir(mode=0o555)
            def bounded_scan(event, args):
                if auditing[0] and event == "os.scandir" and str(args[0]).endswith("unregistered-tree"):
                    pytest.fail("must reject unknown directory without walking it")
            sys.addaudithook(bounded_scan)
            auditing[0] = True
        root.chmod(0o555)
    else:
        file = root / "runtime/baseline.py"
        file.chmod(0o644)
        if mutation == "changed":
            file.write_bytes(b"x" * file.stat().st_size)
            file.chmod(0o444)
    try:
        with pytest.raises(ClosureError):
            verify_harness(root, expected_manifest_sha256=expected)
    finally:
        auditing[0] = False


def test_existing_destination_never_overwritten(tmp_path):
    from runtime.yolo_bundle import freeze_harness
    manifest, expected = fixture_manifest(tmp_path / "source")
    root = tmp_path / "frozen"
    root.mkdir()
    (root / "evidence.txt").write_text("preserve")
    with pytest.raises(FileExistsError):
        freeze_harness(manifest, root, expected_manifest_sha256=expected)
    assert (root / "evidence.txt").read_text() == "preserve"


def test_partial_write_is_retained_but_cannot_be_verified_or_reused(tmp_path, monkeypatch):
    from runtime.yolo_bundle import freeze_harness, verify_harness
    from runtime.yolo_profile import ClosureError
    manifest, expected = fixture_manifest(tmp_path / "source")
    root = tmp_path / "frozen"
    def fail(fd):
        raise OSError("fixture write failure")
    monkeypatch.setattr(os, "fsync", fail)
    with pytest.raises(OSError, match="fixture write failure"):
        freeze_harness(manifest, root, expected_manifest_sha256=expected)
    assert root.exists() and not (root / "harness-manifest.json").exists()
    with pytest.raises(ClosureError):
        verify_harness(root, expected_manifest_sha256=expected)
    with pytest.raises(FileExistsError):
        freeze_harness(manifest, root, expected_manifest_sha256=expected)


def test_manifest_can_live_outside_source_tree(tmp_path):
    from runtime.yolo_bundle import freeze_harness, verify_harness
    source = tmp_path / "source"
    manifest, expected = fixture_manifest(source)
    external = tmp_path / "declared-harness.json"
    manifest.rename(external)
    result = freeze_harness(external, tmp_path / "frozen", source_root=source,
                            expected_manifest_sha256=expected)
    assert verify_harness(tmp_path / "frozen", expected_manifest_sha256=expected) == result
    assert not manifest.exists()
