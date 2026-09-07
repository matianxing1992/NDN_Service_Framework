import hashlib
import json
import subprocess
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = ROOT / "Experiments/TigerCluster/bin/ndnsf-di-spec183-preflight"
REQUIRED = {
    "Experiments/TigerCluster/apps/yolo.py",
    "Experiments/TigerCluster/apps/yolo_network.py",
    "Experiments/TigerCluster/runtime/yolo_worker.py",
    "Experiments/TigerCluster/runtime/yolo_result.py",
    "Experiments/TigerCluster/runtime/yolo_bundle.py",
    "Experiments/TigerCluster/runtime/yolo_profile.py",
    "Experiments/TigerCluster/jobs/yolo/submit.py",
    "Experiments/TigerCluster/jobs/yolo/run.sbatch",
    "Experiments/TigerCluster/schemas/tiger-yolo-v1.schema.json",
}


def source_seal(tmp_path, *, omit=()):
    names = sorted(REQUIRED - set(omit))
    archive = tmp_path / "workspace.tar"
    rows = []
    with tarfile.open(archive, "w") as stream:
        for name in names:
            payload = ("fixture:" + name + "\n").encode()
            member = tmp_path / name
            member.parent.mkdir(parents=True, exist_ok=True)
            member.write_bytes(payload)
            stream.add(member, arcname=name)
            rows.append({
                "path": name,
                "bytes": len(payload),
                "sha256": "sha256:" + hashlib.sha256(payload).hexdigest(),
            })
    body = {
        "schemaVersion": "spec170-local-sif-source-v1",
        "sourceRevision": "1" * 40,
        "sourceMode": "sealed-current-worktree-files",
        "workspace": str(tmp_path),
        "archive": {
            "path": str(archive),
            "bytes": archive.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest(),
        },
        "fileCount": len(rows),
        "files": rows,
        "compiledPayloadCount": 0,
    }
    body["sealDigest"] = "sha256:" + hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    path = tmp_path / "source-seal.json"
    path.write_text(json.dumps(body), encoding="utf-8")
    return path, body


def run(*args):
    return subprocess.run([str(PREFLIGHT), *map(str, args)], cwd=ROOT,
                          capture_output=True, text=True)


def test_inputs_preflight_requires_complete_spec183_harness(tmp_path):
    source, body = source_seal(tmp_path)
    result = run("--phase", "inputs", "--source-seal", source)
    assert result.returncode == 0, result.stderr
    value = json.loads(result.stdout)
    assert value["status"] == "PASS"
    assert value["qualification"] == "SPEC183_INPUTS_COMPONENT_ONLY"
    assert value["sourceSeal"]["sealDigest"] == body["sealDigest"]


def test_inputs_preflight_rejects_missing_run_entrypoint(tmp_path):
    source, _ = source_seal(tmp_path, omit=("Experiments/TigerCluster/jobs/yolo/run.sbatch",))
    result = run("--phase", "inputs", "--source-seal", source)
    assert result.returncode == 4
    assert "SPEC183_HARNESS_NOT_SEALED" in result.stderr


def test_sif_preflight_rejects_missing_options_after_input_gate(tmp_path):
    source, _ = source_seal(tmp_path)
    result = run("--phase", "sif", "--source-seal", source)
    assert result.returncode == 4
    assert "SPEC183_SIF_OPTIONS" in result.stderr


def test_sif_preflight_binds_label_and_runs_exact_candidate(tmp_path):
    source, body = source_seal(tmp_path)
    sif = tmp_path / "runtime.sif"
    sif.write_bytes(b"candidate")
    expected = "sha256:" + hashlib.sha256(sif.read_bytes()).hexdigest()
    invocation_log = tmp_path / "apptainer.log"
    apptainer = tmp_path / "apptainer"
    apptainer.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$*\" >> {invocation_log}\n"
        "if [ \"$1\" = inspect ]; then\n"
        "  printf '%s\\n' '{\"data\":{\"attributes\":{\"labels\":{\"org.ndnsf.di.build-boundary\":\"container-runtime-in-sif\",\"org.ndnsf.di.source-seal\":\""
        + body["sealDigest"]
        + "\"}}}}'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = exec ]; then printf '%s\\n' '{\"status\":\"PASS\"}'; exit 0; fi\n"
        "exit 97\n",
        encoding="utf-8",
    )
    apptainer.chmod(0o755)
    result = run("--phase", "sif", "--source-seal", source, "--sif", sif,
                 "--apptainer", apptainer, "--expected-sif-sha256", expected)
    assert result.returncode == 0, result.stderr
    value = json.loads(result.stdout)
    assert value["status"] == "PASS"
    assert value["qualification"] == "SPEC183_SIF_RUNTIME_COMPONENT_ONLY"
    invocation = invocation_log.read_text(encoding="utf-8")
    assert "inspect --json " + str(sif) in invocation
    assert "exec --cleanenv --containall --home " in invocation
    assert "--pwd / --env NDNSF_ALLOW_CPU_FALLBACK=0" in invocation
    assert str(sif) + " python3 -c" in invocation
