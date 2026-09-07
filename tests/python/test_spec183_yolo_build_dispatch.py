import hashlib
import importlib.util
import json
import subprocess
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILD_LOCAL_SIF = (
    ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    / "build-local-sif.sh"
)
HOST_GATE_TESTS = ROOT / "tests/python/test_spec183_yolo_host_gate.py"
_host_spec = importlib.util.spec_from_file_location("spec183_host_gate_tests", HOST_GATE_TESTS)
_host_tests = importlib.util.module_from_spec(_host_spec)
assert _host_spec.loader is not None
_host_spec.loader.exec_module(_host_tests)


def _source_seal(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    member = root / "sealed-source.txt"
    member.write_text("sealed source\n", encoding="utf-8")
    archive = root / "workspace.tar"
    with tarfile.open(archive, "w") as stream:
        stream.add(member, arcname=member.name)
    row = {
        "path": member.name,
        "bytes": member.stat().st_size,
        "sha256": "sha256:" + hashlib.sha256(member.read_bytes()).hexdigest(),
    }
    body = {
        "schemaVersion": "spec170-local-sif-source-v1",
        "sourceRevision": "0" * 40,
        "sourceMode": "sealed-current-worktree-files",
        "workspace": str(root),
        "archive": {
            "path": str(archive),
            "bytes": archive.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest(),
        },
        "fileCount": 1,
        "files": [row],
        "compiledPayloadCount": 0,
    }
    body["sealDigest"] = "sha256:" + hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    result = root / "source-seal.json"
    result.write_text(json.dumps(body), encoding="utf-8")
    return result


def test_invalid_spec183_receipt_rejects_before_any_apptainer_call(tmp_path):
    source_seal = _source_seal(tmp_path)
    receipt = tmp_path / "host-gate.json"
    receipt.write_text(json.dumps({
        "schema": "wrong-schema",
        "status": "PASS",
    }), encoding="utf-8")
    definition = tmp_path / "candidate.def"
    definition.write_text("Bootstrap: localimage\nFrom: /missing/base.sif\n",
                          encoding="utf-8")
    invocation_log = tmp_path / "apptainer.log"
    apptainer = tmp_path / "apptainer"
    apptainer.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$*\" >> {invocation_log}\n"
        "exit 97\n",
        encoding="utf-8",
    )
    apptainer.chmod(0o755)

    result = subprocess.run([
        str(BUILD_LOCAL_SIF),
        "--definition", str(definition),
        "--sif", str(tmp_path / "runtime.sif"),
        "--record", str(tmp_path / "record.json"),
        "--source-seal", str(source_seal),
        "--spec183-host-gate", str(receipt),
        "--workload-kind", "spec183-yolo",
        "--apptainer", str(apptainer),
        "--expected-apptainer", "1.3.4-1.el9",
    ], cwd=ROOT, capture_output=True, text=True)

    assert result.returncode == 4
    assert "YOLO_HOST_GATE_FIELDS" in result.stderr
    assert not invocation_log.exists()
    assert not (tmp_path / "runtime.sif").exists()
    assert not (tmp_path / "record.json").exists()


def test_spec183_dispatch_rejects_legacy_host_gate_argument(tmp_path):
    apptainer = tmp_path / "apptainer"
    apptainer.write_text("#!/bin/sh\nexit 97\n", encoding="utf-8")
    apptainer.chmod(0o755)
    result = subprocess.run([
        str(BUILD_LOCAL_SIF),
        "--definition", str(tmp_path / "candidate.def"),
        "--sif", str(tmp_path / "runtime.sif"),
        "--record", str(tmp_path / "record.json"),
        "--source-seal", str(tmp_path / "source-seal.json"),
        "--host-gate-manifest", str(tmp_path / "legacy.json"),
        "--workload-kind", "spec183-yolo",
        "--apptainer", str(apptainer),
        "--expected-apptainer", "1.3.4",
    ], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 2
    assert "usage:" in result.stderr


def test_valid_component_receipt_cannot_bypass_unsealed_yolo_harness(tmp_path):
    _host_source, receipt, value = _host_tests.write_receipt(tmp_path)
    source_seal = _source_seal(tmp_path / "build-source")
    source_body = json.loads(source_seal.read_text(encoding="utf-8"))
    value["sourceSeal"] = {
        "path": str(source_seal),
        "sha256": "sha256:" + hashlib.sha256(source_seal.read_bytes()).hexdigest(),
        "sealDigest": source_body["sealDigest"],
        "sourceRevision": source_body["sourceRevision"],
    }
    receipt.write_text(json.dumps(value), encoding="utf-8")
    definition = tmp_path / "candidate.def"
    definition.write_text("Bootstrap: localimage\nFrom: /missing/base.sif\n",
                          encoding="utf-8")
    invocation_log = tmp_path / "apptainer.log"
    apptainer = tmp_path / "apptainer"
    apptainer.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$*\" >> {invocation_log}\n"
        "exit 97\n",
        encoding="utf-8",
    )
    apptainer.chmod(0o755)
    result = subprocess.run([
        str(BUILD_LOCAL_SIF),
        "--definition", str(definition),
        "--sif", str(tmp_path / "runtime.sif"),
        "--record", str(tmp_path / "record.json"),
        "--source-seal", str(source_seal),
        "--spec183-host-gate", str(receipt),
        "--workload-kind", "spec183-yolo",
        "--apptainer", str(apptainer),
        "--expected-apptainer", "1.3.4",
    ], cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 4
    # The real two-phase preflight exists now.  It must reject an old/minimal
    # source seal before any Apptainer call instead of falling through to a
    # build or accepting the component receipt as sufficient evidence.
    assert "SPEC183_HARNESS_NOT_SEALED" in result.stderr
    assert not invocation_log.exists()
