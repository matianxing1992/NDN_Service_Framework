from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    sys.modules[name] = obj
    spec.loader.exec_module(obj)
    return obj


handoff = load("test_handoff", SCRIPTS / "prepare-development-handoff.py")
sealer = load("test_handoff_sealer", SCRIPTS / "prepare-local-sif-source.py")


def commit_fixture(path):
    for command in [
        ["git", "init", "-q", str(path)],
        ["git", "-C", str(path), "add", "--all"],
        ["git", "-C", str(path), "-c", "user.name=Fixture", "-c",
         "user.email=fixture@example.invalid", "commit", "-qm", "fixture"],
    ]:
        subprocess.run(command, check=True, capture_output=True)
    return handoff.git(path, "rev-parse", "HEAD")


@pytest.fixture
def inputs(tmp_path):
    workspaces = {}
    revisions = {}
    for name, entries in [("ndnsf", sealer.FILES), ("ndnSvs", sealer.NDN_SVS_FILES),
                          ("nacAbe", sealer.NAC_ABE_FILES), ("ndnSd", sealer.NDNSD_FILES)]:
        folder = tmp_path / name
        folder.mkdir()
        for entry in entries:
            dest = folder / entry
            source_root = ROOT if name == "ndnsf" else ROOT.parent / {"ndnSvs": "ndn-svs", "nacAbe": "NAC-ABE", "ndnSd": "NDNSD"}[name]
            if (source_root / entry).is_dir():
                dest.mkdir(parents=True, exist_ok=True)
                (dest / "fixture.hpp").write_text("// sealed dependency fixture\n")
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text("pass\n" if dest.name == "wscript" else "fixture\n")
        if name == "ndnSvs":
            (folder / "wscript").write_text("VERSION = '0.1.0'\nGIT_TAG_PREFIX = 'ndn-svs-'\n")
            (folder / "VERSION.info").unlink()
        revisions[name] = commit_fixture(folder)
        workspaces[name] = folder
    wheels = tmp_path / "wheel-inputs"
    wheels.mkdir()
    for filename in handoff.REQUIRED_WHEELS:
        (wheels / filename).write_bytes(b"pinned external wheel fixture")
    base = tmp_path / "base.sif"
    base.write_bytes(b"pinned external base fixture")
    lock = tmp_path / "lock.json"
    lock.write_text(json.dumps({
        "schema": handoff.LOCK_SCHEMA, "release": "fixture-r1",
        "repositories": {name: {"revision": value} for name, value in revisions.items()},
        "wheels": [{"filename": wheel.name, "sha256": handoff.digest(wheel)}
                   for wheel in sorted(wheels.iterdir())],
        "baseSif": {"sha256": handoff.digest(base)},
    }))
    return lock, workspaces, wheels, base


@pytest.mark.parametrize("stale_version", [False, True])
def test_source_archives_survive_relocation_and_reject_tampering(inputs, tmp_path, stale_version):
    lock, workspaces, wheels, _ = inputs
    version_file = workspaces['ndnSvs'] / 'VERSION.info'
    if stale_version:
        version_file.write_text('stale-host-version')
    original = tmp_path / "original"
    result = handoff.prepare(lock, workspaces, wheels, original)
    assert result["status"] == "SOURCE_READY"
    assert result["sifBuild"] == "NOT_RUN"
    seal = json.loads((original / "source/source-seal.json").read_text())
    assert set(seal["dependencies"]) == {"nacAbe", "ndnSvs", "ndnSd"}
    with tarfile.open(original / "source/ndn-svs.tar") as source:
        expected = '0.1.0+git.' + handoff.git(workspaces['ndnSvs'], 'rev-parse', '--short=8', 'HEAD')
        assert source.extractfile("VERSION.info").read().decode() == expected
    if stale_version:
        assert version_file.read_text() == 'stale-host-version'
    else:
        assert not version_file.exists()
    with tarfile.open(original / "source/nacAbe.tar") as source:
        assert source.extractfile("src/fixture.hpp").read() == b"// sealed dependency fixture\n"
    relocated = tmp_path / "relocated"
    shutil.move(str(original), relocated)
    assert handoff.verify(relocated)["sourceSealDigest"] == seal["sealDigest"]
    archive = relocated / "source/nacAbe.tar"
    with archive.open("ab") as stream:
        stream.write(b"altered")
    with pytest.raises(ValueError, match="FILE_DIGEST"):
        handoff.verify(relocated)


@pytest.mark.parametrize("change", ["revision", "dirty", "untracked", "wheel", "missing-wheel"])
def test_prepare_rejects_unpinned_inputs_before_output(inputs, tmp_path, change):
    lock, workspaces, wheels, _ = inputs
    if change == "revision":
        data = json.loads(lock.read_text())
        data["repositories"]["ndnSvs"]["revision"] = "0" * 40
        lock.write_text(json.dumps(data))
    elif change == "missing-wheel":
        data = json.loads(lock.read_text())
        data["wheels"] = [row for row in data["wheels"] if not row["filename"].startswith("python_ndn-")]
        lock.write_text(json.dumps(data))
    elif change == "dirty":
        (workspaces["ndnSvs"] / "wscript").write_text("changed\n")
    elif change == "untracked":
        (workspaces["ndnSvs"] / "ndn-svs/uncommitted.hpp").write_text("uncommitted\n")
    else:
        next(wheels.iterdir()).write_bytes(b"wrong wheel")
    output = tmp_path / "rejected"
    with pytest.raises(ValueError):
        handoff.prepare(lock, workspaces, wheels, output)
    assert not output.exists()


def test_verifier_rejects_unlisted_payload_and_false_revision(inputs, tmp_path):
    lock, workspaces, wheels, _ = inputs
    bundle = tmp_path / "bundle"
    handoff.prepare(lock, workspaces, wheels, bundle)
    added = bundle / "wheels/host.so"
    added.write_bytes(b"host native payload")
    with pytest.raises(ValueError, match="UNEXPECTED_FILE"):
        handoff.verify(bundle)
    added.unlink()
    path = bundle / "handoff.json"
    manifest = json.loads(path.read_text())
    manifest["repositories"]["nacAbe"]["revision"] = "f" * 40
    path.write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="LOCK_MISMATCH"):
        handoff.verify(bundle)


def test_render_checks_base_identity_and_preserves_package(inputs, tmp_path):
    lock, workspaces, wheels, base = inputs
    bundle = tmp_path / "bundle"
    handoff.prepare(lock, workspaces, wheels, bundle)
    wrong = tmp_path / "wrong.sif"
    wrong.write_bytes(b"wrong base")
    destination = tmp_path / "runtime.def"
    with pytest.raises(ValueError, match="BASE_DIGEST"):
        handoff.render(bundle, wrong, destination)
    assert not destination.exists()
    result = handoff.render(bundle, base, destination)
    assert result["status"] == "SOURCE_READY"
    assert result["hostGateManifest"] == "REQUIRED_FOR_BUILD"
    text = destination.read_text()
    assert "@BUNDLE@" not in text and "@BASE_SIF@" not in text
    assert str(bundle / "source/nacAbe.tar") in text
    assert handoff.verify(bundle)["sifBuild"] == "NOT_RUN"
    with pytest.raises(FileExistsError):
        handoff.render(bundle, base, destination)


def test_sealer_keeps_legacy_and_canonical_workload_paths(inputs):
    _, workspaces, _, _ = inputs
    repo = workspaces['ndnsf']
    old = repo / 'packaging/ndnsf-di-container/jobs'
    canonical = repo / 'Experiments/TigerCluster/jobs'
    canonical.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(old), canonical)
    old.symlink_to('../../Experiments/TigerCluster/jobs', target_is_directory=True)
    selected = {path.as_posix() for path in sealer.selected_files(repo)}
    assert 'packaging/ndnsf-di-container/jobs/spec175/workload.json' in selected
    assert 'Experiments/TigerCluster/jobs/spec175/workload.json' in selected
