from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist"
SUBMIT = ROOT / "packaging/ndnsf-di-container/jobs/spec175/submit.sh"

COMMON = (
    "release-identity",
    "local-sif-route",
    "cluster-substrate",
    "target-apptainer-parity",
    "container-abi-provenance",
    "complete-target-link-closure",
    "exact-sif-library-entrypoint",
    "bundle-cwd-artifact-mount",
    "isolated-home-pib-bootstrap",
    "lower-gates-native-exits",
    "wrapper-config-child-status",
    "resource-envelope",
    "promotion-hash-config-delta",
    "credential-secret-scan",
)
MODEL = (
    "current-sif-tiger-control",
    "onnx-model-runtime-compatibility",
    "cuda-no-fallback",
    "routes-stage-dataflow",
)


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def make_subject(tmp_path: Path, gate: str = "control") -> dict[str, Path]:
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate-sif")
    evidence = tmp_path / "evidence.txt"
    evidence.write_text("candidate-bound evidence\n", encoding="utf-8")
    closure = tmp_path / "closure.json"
    closure.write_text(json.dumps({
        "status": "PROMOTABLE",
        "candidate": {
            "id": "candidate-175",
            "sifPath": str(sif),
            "sifSha256": digest(sif),
        },
    }), encoding="utf-8")
    checks = {
        name: {"status": "PASS", "evidence": [{
            "path": str(evidence), "sha256": digest(evidence),
        }]}
        for name in COMMON + (() if gate == "control" else MODEL)
    }
    checklist = tmp_path / "pre-tiger-checklist.json"
    checklist.write_text(json.dumps({
        "schema": "ndnsf-itiger-pre-submit-checklist-v1",
        "gate": gate,
        "candidateId": "candidate-175",
        "sif": {"path": str(sif), "sha256": digest(sif)},
        "checks": checks,
    }), encoding="utf-8")
    return {"sif": sif, "closure": closure, "checklist": checklist,
            "evidence": evidence}


def run_validator(subject: dict[str, Path], tmp_path: Path, gate: str = "control",
                  *extra: str) -> subprocess.CompletedProcess[str]:
    output = tmp_path / "validation.json"
    command = [
        sys.executable, str(VALIDATOR),
        "--manifest", str(subject["checklist"]),
        "--gate", gate,
        "--output", str(output),
        "--candidate-manifest", str(subject["closure"]),
        "--expected-sif", str(subject["sif"]),
        "--expected-sif-sha256", digest(subject["sif"]),
        *extra,
    ]
    return subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                          check=False)


def test_checklist_validator_accepts_candidate_bound_control(tmp_path: Path) -> None:
    subject = make_subject(tmp_path)
    result = run_validator(subject, tmp_path)
    assert result.returncode == 0, result.stderr
    assert json.loads((tmp_path / "validation.json").read_text())["status"] == "PASS"


def test_submit_digest_parser_is_python39_compatible() -> None:
    text = SUBMIT.read_text(encoding="utf-8")
    assert "expected = sys.argv[3]" in text
    assert 'expected.startswith("sha256:")' in text
    assert "map(pathlib.Path, sys.argv[1:5])" not in text


def test_checklist_validator_requires_model_rows_for_functional_gate(tmp_path: Path) -> None:
    subject = make_subject(tmp_path, "functional")
    payload = json.loads(subject["checklist"].read_text())
    payload["checks"].pop("cuda-no-fallback")
    subject["checklist"].write_text(json.dumps(payload))
    result = run_validator(subject, tmp_path, "functional")
    assert result.returncode == 2
    assert "cuda-no-fallback" in json.loads((tmp_path / "validation.json").read_text())["errors"][0]


def test_checklist_validator_rejects_unknown_or_stale_rows(tmp_path: Path) -> None:
    subject = make_subject(tmp_path)
    payload = json.loads(subject["checklist"].read_text())
    payload["checks"]["unexpected-row"] = payload["checks"][COMMON[0]]
    payload["checks"][COMMON[1]]["evidence"][0]["sha256"] = "0" * 64
    subject["checklist"].write_text(json.dumps(payload))
    result = run_validator(subject, tmp_path)
    assert result.returncode == 2
    errors = json.loads((tmp_path / "validation.json").read_text())["errors"]
    assert any("unknown checklist row" in item for item in errors)
    assert any("sha256 mismatch" in item for item in errors)


def test_checklist_validator_rejects_candidate_gate_and_sif_binding(tmp_path: Path) -> None:
    subject = make_subject(tmp_path)
    payload = json.loads(subject["checklist"].read_text())
    payload["candidateId"] = "other-candidate"
    payload["gate"] = "functional"
    subject["checklist"].write_text(json.dumps(payload))
    result = run_validator(subject, tmp_path, "control", "--expected-sif-sha256", "1" * 64)
    assert result.returncode == 2
    errors = json.loads((tmp_path / "validation.json").read_text())["errors"]
    assert any("gate" in item for item in errors)
    assert any("candidateId" in item for item in errors)
    assert any("digest" in item for item in errors)


def test_submit_rejects_before_sbatch_when_checklist_is_invalid(tmp_path: Path) -> None:
    subject = make_subject(tmp_path)
    subject["checklist"].write_text("{}", encoding="utf-8")
    spy_dir = tmp_path / "bin"
    spy_dir.mkdir()
    marker = tmp_path / "sbatch-called"
    spy = spy_dir / "sbatch"
    spy.write_text(f"#!/bin/sh\ntouch {marker}\nexit 0\n", encoding="utf-8")
    spy.chmod(0o755)
    env = dict(os.environ)
    env.update({
        "CLOSURE_MANIFEST": str(subject["closure"]),
        "SIF": str(subject["sif"]),
        "SIF_SHA256": digest(subject["sif"]),
        "WORKLOAD": str(ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json"),
        "REMOTE_SIF": "/project/tma1/ndnsf-di/releases/candidate/runtime.sif",
        "REMOTE_SIF_SHA256": digest(subject["sif"]),
        "PRE_TIGER_CHECKLIST": str(subject["checklist"]),
        "PRE_TIGER_CHECKLIST_VALIDATION": str(tmp_path / "validation.json"),
        "PATH": str(spy_dir) + os.pathsep + env["PATH"],
    })
    result = subprocess.run([str(SUBMIT), "control"], cwd=ROOT, env=env,
                            text=True, capture_output=True, check=False)
    assert result.returncode != 0
    assert not marker.exists(), result.stdout + result.stderr
