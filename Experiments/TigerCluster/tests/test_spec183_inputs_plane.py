"""Spec183 inputs content plane: real receipt rendering and validation.

Plane assembly is deterministic and fail-closed; a VERIFIED plane is content
integrity only, never a runtime qualification. Real-source tests are skipped
when the ignored CAS inputs are not present on this machine.
"""
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # TigerCluster
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))  # repo root
from tools import spec183_inputs_plane as plane

REPO = Path(__file__).resolve().parents[3]


def _seed(tmp_path, monkeypatch):
    """Point the module at four small fixture sources inside tmp_path."""
    sources = {}
    root = tmp_path / "cas"
    for name in plane.SOURCES:
        source = root / (name + ".bin")
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(("real:" + name).encode())
        sources[name] = source.resolve()  # absolute; joining overrides the repo root
    monkeypatch.setattr(plane, "SOURCES", sources)
    return root


def test_render_assembles_exact_rows_and_is_deterministic(tmp_path, monkeypatch):
    _seed(tmp_path, monkeypatch)
    first = plane.render(tmp_path / "planes/inputs")
    assert set(first["files"]) == {"sourceLock", "sourceSeal",
                                   "buildDefinition", "baseSif"}
    for name, row in first["files"].items():
        file = tmp_path / "planes/inputs" / Path(row["path"]).name
        assert file.read_bytes() == ("real:" + name).encode()
        assert "sha256:" + hashlib.sha256(file.read_bytes()).hexdigest() == row["sha256"]
        assert file.stat().st_nlink > 1  # hard link to the CAS receipt, no copy
    second = plane.render(tmp_path / "planes/inputs")
    assert second == first


def test_check_reports_verified_integrity_without_qualification(
        tmp_path, monkeypatch):
    _seed(tmp_path, monkeypatch)
    plane.render(tmp_path / "planes/inputs")
    report = plane.check(tmp_path / "planes/inputs")
    assert report["integrity"] == "VERIFIED"
    assert report["qualification"] == "NOT_EVALUATED"
    assert report["stage"] == "inputs"


def test_tampered_plane_file_is_rejected(tmp_path, monkeypatch):
    _seed(tmp_path, monkeypatch)
    plane.render(tmp_path / "planes/inputs")
    (tmp_path / "planes/inputs/baseSif.bin").write_bytes(b"tampered")
    report = plane.check(tmp_path / "planes/inputs")
    assert report["integrity"] == "REJECTED"
    assert "baseSif" in report["reason"]


def test_stale_plane_identity_is_never_overwritten(tmp_path, monkeypatch):
    _seed(tmp_path, monkeypatch)
    plane.render(tmp_path / "planes/inputs")
    # Hard links share one inode, so altering the plane file alters the CAS
    # receipt too; the guard that matters is refusing to rewrite plane.json
    # with the new, silently-changed identities.
    before = (tmp_path / "planes/inputs/plane.json").read_bytes()
    (tmp_path / "planes/inputs/baseSif.bin").write_bytes(b"changed input")
    with pytest.raises(RuntimeError, match="different identities"):
        plane.render(tmp_path / "planes/inputs")
    assert (tmp_path / "planes/inputs/plane.json").read_bytes() == before


@pytest.mark.skipif(
    not (REPO / "Experiments/TigerCluster/development-handoff.lock.json").is_file()
    or not (REPO / plane.SOURCES["baseSif"]).is_file(),
    reason="real handoff inputs not received on this machine")
def test_real_received_inputs_plane_is_verified(tmp_path):
    root = REPO / plane.DEFAULT_PLANE_REL
    plane.render(root)
    report = plane.check(root)
    assert report["integrity"] == "VERIFIED", report
    document = json.loads((root / "plane.json").read_text())
    assert document["files"]["baseSif"]["sha256"] == (
        "sha256:b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285")
