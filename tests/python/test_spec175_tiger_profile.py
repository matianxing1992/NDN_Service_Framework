from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "packaging/ndnsf-di-container/lib"
_original_path = sys.path[:]
try:
    sys.path.insert(0, str(LIB))
    import spec175_tiger_profile as profile  # noqa: E402
finally:
    sys.path[:] = _original_path


def _digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _fixture(root: Path) -> tuple[Path, Path]:
    helper = root / "helper.sh"
    helper.write_text("#!/bin/sh\n", encoding="utf-8")
    job_specs = {
        "control": ("ndnsf175-control", "nodes=1;ntasks=1;gres=gpu:0;time=00:20:00"),
        "stage-readiness": ("ndnsf175-stage", "nodes=1;ntasks=1;gres=gpu:3;time=00:45:00"),
        "multi-provider": ("ndnsf175-multi", "nodes=1;ntasks=1;gres=gpu:3;mem=96G;time=01:00:00"),
        "conversation-residency": ("ndnsf175-g6c", "nodes=1;ntasks=1;gres=gpu:3;mem=96G;time=01:00:00"),
        "performance": ("ndnsf175-perf", "nodes=1;ntasks=1;gres=gpu:3;mem=96G;time=02:00:00"),
    }
    for gate, values in profile.GATES.items():
        job_name, envelope = job_specs[gate]
        directives = [f"#SBATCH --job-name={job_name}"]
        directives.extend(f"#SBATCH --{item.split('=', 1)[0]}={item.split('=', 1)[1]}"
                          for item in envelope.split(';'))
        (root / values["job"]).write_text("#!/bin/sh\n" + "\n".join(directives) + "\n", encoding="utf-8")
    tracked = [{"path": "helper.sh", "sha256": _digest(helper)}]
    allow = sorted(profile.COMMON_ALLOWED_DELTAS | profile.GATE_ALLOWED_DELTAS["control"])
    gate_parameters = {
        "control": {"providerCount": 4, "gpuCount": 0},
        "stage-readiness": {"providerCount": 3, "gpuCount": 3},
        "multi-provider": {"providerCount": 3, "gpuCount": 3},
        "conversation-residency": {"providerCount": 3, "gpuCount": 3},
        "performance": {"providerCount": 3, "gpuCount": 3},
    }
    gates = {
        gate: {
            "job": values["job"],
            "checklistGate": values["checklist"],
            "candidateGate": values["candidate"],
            "allowlist": sorted(profile.COMMON_ALLOWED_DELTAS | profile.GATE_ALLOWED_DELTAS[gate]),
            "fixedConfig": {
                **gate_parameters[gate], "seed": 1750001, "resourceEnvelope": job_specs[gate][1],
                **({"stageDeviceIds": [0, 1, 2]} if gate == "stage-readiness" else {}),
            },
        }
        for gate, values in profile.GATES.items()
    }
    profile_path = root / "profile.json"
    profile_path.write_text(json.dumps({
        "schema": profile.PROFILE_SCHEMA,
        "profileId": "test-profile",
        "common": {
            "submitEntry": "submit.sh",
            "jobRoot": ".",
            "runner": "runner.sh",
            "cwd": "/bundle",
            "apptainer": {"path": "/opt/apptainer/1.5.3/bin/apptainer", "version": "1.5.3", "args": ["exec", "--cleanenv"]},
            "exportMode": "NONE",
            "fixedEnvironment": ["SPEC175_GATE"],
            "fixedConfig": {"runtime": "exact-local-sif"},
        },
        "gates": gates,
        "trackedFiles": tracked,
    }), encoding="utf-8")
    sif = root / "runtime.sif"
    sif.write_bytes(b"sif")
    for name in ("closure.json", "workload.json", "checklist.json", "checklist-validation.json"):
        (root / name).write_text("{}\n", encoding="utf-8")
    (root / "bundle").mkdir()
    run = root / "run.json"
    profile_document = json.loads(profile_path.read_text(encoding="utf-8"))
    run.write_text(json.dumps({
        "schema": profile.RUN_SCHEMA,
        "profileSha256": profile.canonical_digest(profile_document),
        "gate": "control",
        "runId": "run-1",
        "candidate": {
            "id": "candidate-1",
            "closureManifest": str(root / "closure.json"),
            "sif": str(sif),
            "sifSha256": _digest(sif),
            "remoteSif": "/project/tma1/runtime.sif",
            "remoteSifSha256": _digest(sif),
        },
        "workload": str(root / "workload.json"),
        "bundle": str(root / "bundle"),
        "output": str(root / "output"),
        "checklist": str(root / "checklist.json"),
        "checklistValidation": str(root / "checklist-validation.json"),
        "profileDelta": str(root / "delta.json"),
        "parameters": {"providerCount": 4, "gpuCount": 0, "seed": 1750001,
                        "resourceEnvelope": job_specs["control"][1]},
        "model": None,
        "prerequisites": {},
    }), encoding="utf-8")
    return profile_path, run


def test_profile_renders_explicit_none_export_without_ambient_values() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        loaded = profile.load_profile(profile_path)
        run = profile.load_run(run_path, "control")
        argv, exports, report = profile.render_sbatch_argv(loaded, run, root, profile_path)
        assert argv[2].startswith("--export=NONE,")
        assert "--export=ALL" not in " ".join(argv)
        assert exports["SPEC175_GATE"] == "control"
        assert report["argvSha256"].startswith("sha256:")


def test_profile_rejects_unknown_run_fields_before_rendering() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        payload = json.loads(run_path.read_text(encoding="utf-8"))
        payload["ambientValue"] = "must-fail"
        run_path.write_text(json.dumps(payload), encoding="utf-8")
        with pytest.raises(profile.ProfileError, match="unknown keys"):
            profile.load_run(run_path, "control")


def test_profile_transports_stage_device_override_without_export_commas() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        payload = json.loads(run_path.read_text(encoding="utf-8"))
        payload["gate"] = "stage-readiness"
        payload["model"] = {"manifest": str(root / "workload.json"), "remoteRoot": "/project/tma1/model"}
        payload["parameters"]["providerCount"] = 3
        payload["parameters"]["gpuCount"] = 3
        payload["parameters"]["resourceEnvelope"] = "nodes=1;ntasks=1;gres=gpu:3;time=00:45:00"
        payload["parameters"]["stageDeviceIds"] = [2, 1, 0]
        run_path.write_text(json.dumps(payload), encoding="utf-8")
        loaded = profile.load_profile(profile_path)
        run = profile.load_run(run_path, "stage-readiness")
        _, exports, effective = profile.render_sbatch_argv(loaded, run, root, profile_path)
        assert exports["SPEC175_STAGE_DEVICE_IDS"] == "2:1:0"
        assert "," not in exports["SPEC175_STAGE_DEVICE_IDS"]
        assert effective["allowlistedDeltas"] == [{
            "field": "parameters.stageDeviceIds",
            "baseline": [0, 1, 2],
            "value": [2, 1, 0],
            "reason": "ALLOWLISTED_DELTA",
        }]


def test_profile_rejects_tracked_helper_mutation() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        loaded = profile.load_profile(profile_path)
        (root / "helper.sh").write_text("#!/bin/sh\necho changed\n", encoding="utf-8")
        run = profile.load_run(run_path, "control")
        with pytest.raises(profile.ProfileError, match="TRACKED_FILE_CHANGED"):
            profile.render_sbatch_argv(loaded, run, root, profile_path)


def test_profile_rejects_job_directive_drift_before_submission() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        profile_document = json.loads(profile_path.read_text(encoding="utf-8"))
        profile_document["gates"]["control"]["fixedConfig"] = {"jobName": "expected-job"}
        profile_path.write_text(json.dumps(profile_document), encoding="utf-8")
        run_document = json.loads(run_path.read_text(encoding="utf-8"))
        run_document["profileSha256"] = profile.canonical_digest(profile_document)
        run_path.write_text(json.dumps(run_document), encoding="utf-8")
        loaded = profile.load_profile(profile_path)
        (root / "qualify-control.sbatch").write_text(
            "#!/bin/sh\n#SBATCH --job-name=wrong-job\n", encoding="utf-8")
        run = profile.load_run(run_path, "control")
        with pytest.raises(profile.ProfileError, match="JOB_DIRECTIVE_MISMATCH"):
            profile.render_sbatch_argv(loaded, run, root, profile_path)


def test_profile_rejects_run_bound_to_another_profile() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        payload = json.loads(run_path.read_text(encoding="utf-8"))
        payload["profileSha256"] = "sha256:" + "0" * 64
        run_path.write_text(json.dumps(payload), encoding="utf-8")
        with pytest.raises(profile.ProfileError, match="PROFILE_DIGEST_MISMATCH"):
            profile_document = profile.load_profile(profile_path)
            run = profile.load_run(run_path, "control")
            profile.render_sbatch_argv(profile_document, run, root, profile_path)


def test_profile_rejects_unconsumed_parameter_delta() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        profile_path, run_path = _fixture(root)
        profile_document = json.loads(profile_path.read_text(encoding="utf-8"))
        profile_document["gates"]["control"]["fixedConfig"] = {"seed": 1750001}
        profile_path.write_text(json.dumps(profile_document), encoding="utf-8")
        run_document = json.loads(run_path.read_text(encoding="utf-8"))
        run_document["profileSha256"] = profile.canonical_digest(profile_document)
        run_document["parameters"]["seed"] = 1750002
        run_path.write_text(json.dumps(run_document), encoding="utf-8")
        loaded = profile.load_profile(profile_path)
        run = profile.load_run(run_path, "control")
        with pytest.raises(profile.ProfileError, match="UNREGISTERED_DELTA"):
            profile.render_sbatch_argv(loaded, run, root, profile_path)
