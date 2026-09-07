"""Focused integrity tests; fixture bytes are not runtime qualification."""
import hashlib
import importlib.util
import json
import shutil
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def closure_module():
    spec = importlib.util.spec_from_file_location(
        "tiger_yolo_closure", ROOT / "runtime/yolo_profile.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def input_plane(root):
    root.mkdir()
    files = {}
    for name in ("sourceLock", "sourceSeal", "buildDefinition", "baseSif"):
        payload = ("fixture-only:" + name).encode()
        (root / name).write_bytes(payload)
        files[name] = {"path": name, "bytes": len(payload),
                       "sha256": "sha256:" + hashlib.sha256(payload).hexdigest()}
    doc = {"schema": "tiger-yolo-plane-v1", "stage": "inputs", "parentId": None,
           "files": files, "parameters": {"maxBuildJobs": 2}}
    path = root / "plane.json"
    path.write_text(json.dumps(doc))
    return path, doc


def test_input_integrity_does_not_require_future_runtime_or_claim_qualification(tmp_path):
    module = closure_module()
    path, _ = input_plane(tmp_path / "inputs")
    checked = module.check_plane(path, expected_stage="inputs")
    assert checked["integrity"] == "VERIFIED"
    assert checked["qualification"] == "NOT_EVALUATED"
    assert checked["id"].startswith("sha256:")
    assert not (path.parent / "runtime.sif").exists()


@pytest.mark.parametrize("mutation", ["missing", "unknown", "boolean-size", "escape",
                                      "absolute", "symlink", "empty", "parameters", "stage"])
def test_invalid_inventory_is_rejected(tmp_path, mutation):
    module = closure_module()
    path, doc = input_plane(tmp_path / "inputs")
    row = doc["files"]["baseSif"]
    if mutation == "missing": del doc["files"]["baseSif"]
    elif mutation == "unknown": doc["uncheckedEnvironment"] = {"LD_PRELOAD": "/evil.so"}
    elif mutation == "boolean-size": row["bytes"] = True
    elif mutation == "escape":
        (tmp_path / "escape").write_bytes((path.parent / "baseSif").read_bytes())
        row["path"] = "../escape"
    elif mutation == "absolute": row["path"] = str(path.parent / "baseSif")
    elif mutation == "symlink":
        (tmp_path / "escape").write_bytes((path.parent / "baseSif").read_bytes())
        (path.parent / "alias").symlink_to(tmp_path / "escape")
        row["path"] = "alias"
    elif mutation == "empty": doc["files"] = {}
    elif mutation == "parameters": doc["parameters"] = []
    elif mutation == "stage": doc["stage"] = "unregistered"
    path.write_text(json.dumps(doc))
    with pytest.raises(module.ClosureError):
        module.check_plane(path, expected_stage="inputs")


def next_plane(root, stage, parent):
    root.mkdir()
    names = {"runtime": ("sif", "nativeManifest", "libraryLock"),
             "dispatch": ("effectiveProfile", "harnessManifest", "modelManifest",
                          "oracle", "fixture", "trustPolicy", "validationContract")}[stage]
    files = {}
    for name in names:
        payload = ("fixture-only:" + name).encode()
        (root / name).write_bytes(payload)
        files[name] = {"path": name, "bytes": len(payload),
                       "sha256": "sha256:" + hashlib.sha256(payload).hexdigest()}
    doc = {"schema": "tiger-yolo-plane-v1", "stage": stage, "parentId": parent,
           "files": files, "parameters": {"timeoutMs": 60000}}
    path = root / "plane.json"
    path.write_text(json.dumps(doc))
    return path, doc


def test_chain_rejects_stale_parent_and_cannot_skip_stage(tmp_path):
    module = closure_module()
    inputs, source = input_plane(tmp_path / "inputs")
    input_id = module.check_plane(inputs, expected_stage="inputs")["id"]
    runtime, _ = next_plane(tmp_path / "runtime", "runtime", input_id)
    runtime_id = module.check_plane(runtime, expected_stage="runtime", parent_id=input_id)["id"]
    experiment, _ = next_plane(tmp_path / "experiment", "dispatch", runtime_id)
    paths = {"inputs": inputs, "runtime": runtime, "dispatch": experiment}
    result = module.check_chain(paths, through="dispatch")
    assert set(result["identities"]) == set(paths)
    assert result["qualification"] == "NOT_EVALUATED"
    with pytest.raises(module.ClosureError, match="CHAIN_STAGES"):
        module.check_chain({"inputs": inputs, "dispatch": experiment}, through="dispatch")
    source["parameters"]["maxBuildJobs"] = 1
    inputs.write_text(json.dumps(source))
    with pytest.raises(module.ClosureError, match="PLANE_PARENT"):
        module.check_chain(paths, through="dispatch")


def test_identity_is_relocatable_but_binds_all_behavior_parameters(tmp_path):
    module = closure_module()
    path, doc = input_plane(tmp_path / "original")
    before = module.check_plane(path, expected_stage="inputs")["id"]
    shutil.copytree(path.parent, tmp_path / "moved")
    moved = tmp_path / "moved/plane.json"
    (moved.parent / "baseSif").rename(moved.parent / "renamed-sif")
    doc["files"]["baseSif"]["path"] = "renamed-sif"
    moved.write_text(json.dumps(doc))
    assert module.check_plane(moved, expected_stage="inputs")["id"] == before
    doc["parameters"]["environment"] = {"NDNSF_HANDLER_THREADS": "2"}
    moved.write_text(json.dumps(doc))
    assert module.check_plane(moved, expected_stage="inputs")["id"] != before


@pytest.mark.parametrize("mutation", ["bytes", "file-removed", "hash", "row-extra",
                                      "duplicate-json", "nan", "fifo", "unknown-stage"])
def test_tampered_or_ambiguous_documents_fail_closed(tmp_path, mutation):
    module = closure_module()
    path, doc = input_plane(tmp_path / "inputs")
    data = path.parent / "baseSif"
    if mutation == "bytes": data.write_bytes(b"x" * data.stat().st_size)
    elif mutation == "file-removed": data.unlink()
    elif mutation == "hash": doc["files"]["baseSif"]["sha256"] = "sha256:" + "0" * 64
    elif mutation == "row-extra": doc["files"]["baseSif"]["ignored"] = True
    elif mutation == "nan": doc["parameters"]["timeout"] = float("nan")
    elif mutation == "fifo":
        import os
        data.unlink()
        os.mkfifo(data)
    path.write_text(json.dumps(doc))
    if mutation == "duplicate-json":
        path.write_text(path.read_text().replace('"maxBuildJobs": 2',
                                                '"maxBuildJobs": 1, "maxBuildJobs": 2'))
    with pytest.raises(module.ClosureError):
        module.check_plane(path, expected_stage="other" if mutation == "unknown-stage" else "inputs")


def test_manifest_special_file_cannot_hang_check(tmp_path):
    import os
    import subprocess
    import sys

    path = tmp_path / "plane.json"
    os.mkfifo(path)
    code = """
import sys
sys.path.insert(0, sys.argv[1])
from yolo_profile import check_plane, ClosureError
try:
    check_plane(sys.argv[2], expected_stage='inputs')
except ClosureError:
    sys.exit(0)
sys.exit(1)
"""
    result = subprocess.run([sys.executable, "-c", code, str(ROOT / "runtime"), str(path)],
                            timeout=2, capture_output=True)
    assert result.returncode == 0
