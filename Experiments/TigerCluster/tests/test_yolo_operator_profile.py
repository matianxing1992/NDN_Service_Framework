"""Operator profile fixtures, explicitly not executable qualified releases."""
import hashlib
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from runtime import yolo_profile


def profile_fixture(tmp_path):
    schema = json.loads((ROOT / "schemas/tiger-yolo-v1.schema.json").read_text())
    file = tmp_path / "fixture-only.txt"
    file.write_text("test fixture, not a model or release")
    ref = {"path": file.name, "bytes": file.stat().st_size,
           "sha256": "sha256:" + hashlib.sha256(file.read_bytes()).hexdigest()}
    value = {
        "schema": "tiger-yolo-v1", "profileId": "fixture-only",
        "release": {"inputs": ref, "gates": {}},
        "workload": {"descriptor": ref, "packageManifest": ref},
        "runtime": {"apptainer": "/usr/local/bin/apptainer", "apptainerVersion": "1.3.4",
                    "python": "/opt/venv/bin/python", "nativeProvider": "/opt/ndnsf-di/current/bin/di-native-provider",
                    "cwd": "/bundle", "environment": schema["properties"]["runtime"]["properties"]["environment"]["const"]},
        "cluster": {"partition": "bigTiger", "account": "devs", "constraint": "", "nodes": 2,
                    "gpusPerNode": 1, "gpuClass": "rtx_6000", "cpusPerNode": 4,
                    "memoryGiB": 16, "wallTimeSeconds": 900, "tcpPort": 20383},
        "roles": schema["properties"]["roles"]["const"],
        "security": {"identityNamespace": "/test/spec183", "trustPolicy": ref,
                     "authorityPrivateKey": "private/authority.key", "protectionEpoch": "epoch-1"},
        "timing": {"startupSeconds": 120, "ackTimeoutMs": 1500, "requestDeadlineMs": 60000,
                   "progressTimeoutSeconds": 30, "cleanupSeconds": 30, "stagingSeconds": 120},
        "cases": schema["properties"]["cases"]["const"],
        "schedule": schema["properties"]["schedule"]["const"],
        "oracle": {"contract": ref, "reference": ref, "input": ref},
        "storage": {"localArtifactRoot": str(tmp_path / "cache"), "remoteArtifactRoot": "/project/test/cache",
                    "sharedRunRoot": "/project/test/runs", "sharedLockRoot": "/project/test/locks",
                    "scratchRoot": "/tmp", "peakBytes": 100, "marginBytes": 20},
        "evidence": {"operatorLock": ref, "harnessManifest": ref, "validationContract": ref}}
    path = tmp_path / "profile.json"
    path.write_text(json.dumps(value))
    return path, value


def test_profile_structure_does_not_claim_runtime_qualification(tmp_path):
    path, _ = profile_fixture(tmp_path)
    result = yolo_profile.load_operator_profile(path, stage="inputs")
    assert result["structure"] == "VALIDATED"
    assert result["qualification"] == "NOT_EVALUATED"
    assert result["minimumWallTimeSeconds"] == 510


@pytest.mark.parametrize("section,key,bad,code", [
    ("cluster", "unused", 1, "PROFILE_SCHEMA"),
    ("cluster", "nodes", 3, "PROFILE_SCHEMA"),
    ("cluster", "cpusPerNode", True, "PROFILE_SCHEMA"),
    ("cluster", "cpusPerNode", 4.0, "PROFILE_INTEGER_REQUIRED"),
    ("cluster", "wallTimeSeconds", 509, "PROFILE_WALLTIME_BUDGET"),
    ("timing", "ackTimeoutMs", 1000, "PROFILE_SCHEMA"),
    ("timing", "progressTimeoutSeconds", 61, "PROFILE_PROGRESS_BUDGET"),
    ("runtime", "environment", {"LD_PRELOAD": "/evil.so"}, "PROFILE_SCHEMA"),
    ("runtime", "apptainer", "/opt/a,b", "PROFILE_PATH"),
    ("runtime", "apptainer", "/opt/a\nb", "PROFILE_CONTROL_CHARACTER"),
    ("storage", "sharedRunRoot", "relative/runs", "PROFILE_REMOTE_PATH"),
    ("storage", "sharedRunRoot", "/project/../runs", "PROFILE_REMOTE_PATH"),
    ("security", "identityNamespace", "/test/../bad", "PROFILE_IDENTITY_NAMESPACE"),
    ("release", "qualified", True, "PROFILE_SCHEMA"),
])
def test_reject_bad_operator_fields(tmp_path, section, key, bad, code):
    path, value = profile_fixture(tmp_path)
    value[section][key] = bad
    path.write_text(json.dumps(value))
    with pytest.raises(yolo_profile.ClosureError, match=code):
        yolo_profile.load_operator_profile(path, stage="inputs")


@pytest.mark.parametrize("stage", ["runtime", "dispatch", "unexpected"])
def test_missing_stage_is_not_bypassed(tmp_path, stage):
    path, _ = profile_fixture(tmp_path)
    with pytest.raises(yolo_profile.ClosureError, match="PROFILE_STAGE"):
        yolo_profile.load_operator_profile(path, stage=stage)


def test_relative_references_are_independent_of_caller_cwd(tmp_path, monkeypatch):
    path, value = profile_fixture(tmp_path)
    value["storage"]["localArtifactRoot"] = "cache"
    value["runtime"]["apptainer"] = "bin/apptainer"
    path.write_text(json.dumps(value))
    first = yolo_profile.load_operator_profile(path, stage="inputs")
    monkeypatch.chdir(tmp_path.parent)
    second = yolo_profile.load_operator_profile(path, stage="inputs")
    assert first == second
    assert second["profile"]["workload"]["descriptor"]["path"] == str(tmp_path / "fixture-only.txt")
    assert second["profile"]["runtime"]["apptainer"] == str(tmp_path / "bin/apptainer")
    assert not (tmp_path / "cache").exists()


def test_read_only_structure_does_not_hash_or_launch(tmp_path, monkeypatch):
    path, value = profile_fixture(tmp_path)
    value["release"]["inputs"]["path"] = "not-yet-received.json"
    path.write_text(json.dumps(value))

    def forbidden(*args, **kwargs):
        pytest.fail("structural validation must not launch, create, or verify artifacts")

    import subprocess
    monkeypatch.setattr(subprocess, "Popen", forbidden)
    monkeypatch.setattr(Path, "mkdir", forbidden)
    monkeypatch.setattr(yolo_profile, "check_chain", forbidden)
    result = yolo_profile.load_operator_profile(path, stage="inputs")
    assert result["integrity"] == "NOT_EVALUATED"
    assert result["qualification"] == "NOT_EVALUATED"


@pytest.mark.parametrize("profile_link", [True, False])
def test_symlink_profile_or_reference_is_rejected(tmp_path, profile_link):
    path, value = profile_fixture(tmp_path)
    if profile_link:
        link = tmp_path / "alias.json"
        link.symlink_to(path)
        path = link
    else:
        link = tmp_path / "alias.txt"
        link.symlink_to(tmp_path / "fixture-only.txt")
        value["oracle"]["input"]["path"] = link.name
        path.write_text(json.dumps(value))
    with pytest.raises(yolo_profile.ClosureError, match="PROFILE_SYMLINK"):
        yolo_profile.load_operator_profile(path, stage="inputs")


def test_duplicate_keys_rejected(tmp_path):
    path, _ = profile_fixture(tmp_path)
    path.write_text('{"schema":"tiger-yolo-v1","schema":"tiger-yolo-v1"}')
    with pytest.raises(yolo_profile.ClosureError, match="DUPLICATE_JSON_KEY"):
        yolo_profile.load_operator_profile(path, stage="inputs")


def test_profile_filename_control_character_rejected_before_open(tmp_path):
    with pytest.raises(yolo_profile.ClosureError, match="PROFILE_PATH"):
        yolo_profile.load_operator_profile(tmp_path / "bad\nprofile.json", stage="inputs")


def test_budget_boundary_and_fingerprint(tmp_path):
    path, value = profile_fixture(tmp_path)
    value["cluster"]["wallTimeSeconds"] = 510
    path.write_text(json.dumps(value))
    first = yolo_profile.load_operator_profile(path, stage="inputs")
    value["timing"]["stagingSeconds"] = 119
    path.write_text(json.dumps(value, indent=4))
    second = yolo_profile.load_operator_profile(path, stage="inputs")
    assert second["minimumWallTimeSeconds"] == 509
    assert first["documentDigest"] != second["documentDigest"]
