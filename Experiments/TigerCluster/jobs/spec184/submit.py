#!/usr/bin/env python3
"""Spec186 check/prepare/local/submit/collect boundary.

The command is deliberately explicit: ``check`` and ``prepare`` are offline,
``submit`` is the only path that may call ``sbatch``, and ``local`` owns one
bounded process group.  The implementation does not contain model or NDN
business logic; those remain in the native application entrypoints.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
from typing import Any, Dict, List, Mapping, Optional, Sequence


HERE = Path(__file__).resolve()
TIGER_ROOT = HERE.parents[2]
REPO_ROOT = HERE.parents[4]
_RUN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
MODULE_PATH = TIGER_ROOT / "runtime" / "spec186_candidate.py"
spec = importlib.util.spec_from_file_location("spec186_candidate", MODULE_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError("SPEC186_CANDIDATE_IMPORT")
candidate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = candidate
spec.loader.exec_module(candidate)


def _read(path: Path) -> Dict[str, Any]:
    return candidate.load_json(path)


def _write(path: Path, value: Mapping[str, Any]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def build_candidate(profile_path: Path) -> Dict[str, Any]:
    profile = candidate.load_profile(profile_path, repo_root=REPO_ROOT)
    return candidate.build_candidate_manifest(profile, repo_root=REPO_ROOT)


class RenderError(ValueError):
    """Raised when a profile cannot be rendered into an executable command."""


def _yolo_case(case: str) -> str:
    """Map a Spec186 profile to the registered Spec180 case.

    The Spec186 profiles describe the four-provider split graph.  That graph
    is Y-B; passing Y-A would ask the runner for a single FullModel provider.
    Negative profiles use the registered Y-N matrix.
    """
    if case.endswith("-negative"):
        return "Y-N"
    return "Y-B"


def render_effective(profile: Mapping[str, Any], manifest: Mapping[str, Any],
                     run_id: str, run_root: Path) -> Dict[str, Any]:
    """Render all argv/env/binds before a scheduler or process mutation."""
    if not isinstance(run_id, str) or _RUN_ID_RE.fullmatch(run_id) is None:
        raise RenderError("RUN_ID_INVALID")
    runtime = profile["runtime"]
    resources = profile["resources"]
    topology = profile["topology"]
    launcher = runtime["harness"]["launcher"]
    if topology["mode"] == "tiger":
        # The current Spec186 profile has no signed Spec180 workload document,
        # canonical package/config/key-map inputs, or per-role Tiger argument
        # files.  The old generic command shape would either import MiniNDN
        # from a SIF that does not contain it or call the supervisor with the
        # wrong candidate schema.  Reject before scheduler mutation until a
        # dedicated Tiger dispatch contract is added to the profile schema.
        raise RenderError(
            "TIGER_HARNESS_INPUTS_UNDECLARED:"
            "SPEC180_WORKLOAD,SPEC180_YOLO_CANONICAL_PACKAGE,"
            "SPEC180_YOLO_CONFIG,provider-args,nfd-config")
    if (profile["case"] == "qwen06b-minindn-cpu" and
            (profile["model"]["format"] != "onnx" or
             profile["model"]["backend"] != "onnxruntime-cpu")):
        raise RenderError("QWEN_HARNESS_MODEL_CONTRACT_MISMATCH:expected-onnxruntime-cpu")
    role_map = [{"name": role["name"], "identity": role["identity"],
                 "node": role["node"], "gpu": role["gpu"],
                 "backend": role["backend"], "service": role["service"],
                 "allowCpuFallback": role["allowCpuFallback"]}
                for role in profile["roles"]]
    gpu_devices = sorted({role["gpu"] for role in profile["roles"] if role["gpu"] >= 0})
    # MiniNDN remains the host orchestration process.  Its child NFD and
    # application commands use the exact SIF through this command-provider
    # contract.
    execution_mode = "host-minindn"
    data_root = str(run_root)
    env = {
        "SPEC186_CASE": profile["case"],
        "SPEC186_RUN_ID": run_id,
        "SPEC186_CANDIDATE_DIGEST": manifest["candidateDigest"],
        "SPEC186_APPTAINER_VERSION": runtime["apptainer"]["version"],
        "NDN_CLIENT_TRANSPORT": "unix:///run/nfd.sock",
        "PYTHONNOUSERSITE": "1",
        "NDNSF_DI_STATE_ROOT": data_root + "/state",
        "NDNSF_DI_ENVELOPE_KEY_FILE": data_root + "/security/request-envelope.key",
        "SPEC180_CASE_OUTPUT_DIR": data_root + "/evidence",
    }
    env.update({
        "SPEC180_RUNTIME_SIF": runtime["baseSif"]["path"],
        "SPEC180_RUNTIME_APPTAINER": runtime["apptainer"]["path"],
    })
    if gpu_devices:
        env["CUDA_VISIBLE_DEVICES"] = ",".join(str(device) for device in gpu_devices)
    binds = [
        {"source": str(run_root), "target": "/run/spec186", "mode": "rw"},
        {"source": runtime["application"]["bundle"]["path"], "target": "/app/bundle", "mode": "ro"},
        {"source": profile["model"]["model"]["path"], "target": "/model/model", "mode": "ro"},
        {"source": profile["workload"]["input"]["path"], "target": "/inputs/input.json", "mode": "ro"},
        {"source": profile["workload"]["oracle"]["path"], "target": "/inputs/oracle.json", "mode": "ro"},
        {"source": profile["security"]["permissions"]["path"], "target": "/inputs/permissions.json", "mode": "ro"},
        {"source": profile["security"]["identityRoot"], "target": "/identity", "mode": "rw"},
    ]
    stage_manifest = profile["model"].get("stageManifest")
    if stage_manifest and stage_manifest.get("path"):
        binds.append({"source": stage_manifest["path"],
                      "target": "/model/stage-manifest.json", "mode": "ro"})
    if profile["case"].startswith("yolo-minindn"):
        workload_argv = [sys.executable, launcher, "--case",
                         _yolo_case(profile["case"])]
    elif profile["case"] == "qwen06b-minindn-cpu":
        if not stage_manifest or not stage_manifest.get("path"):
            raise RenderError("QWEN_STAGE_MANIFEST_UNDECLARED")
        workload_argv = [sys.executable, launcher, "--stage-manifest",
                         stage_manifest["path"]]
    else:
        raise RenderError("CASE_NOT_RENDERABLE:" + str(profile["case"]))
    argv = workload_argv
    return {
        "schemaVersion": "spec186-effective-config-v1",
        "runId": run_id,
        "candidateDigest": manifest["candidateDigest"],
        "case": profile["case"],
        "argv": argv,
        "environment": env,
        "binds": binds,
        "execution": {"mode": execution_mode, "containerLauncher": None},
        "topology": {"mode": topology["mode"], "hosts": topology["hosts"],
                      "nfdEndpoints": topology["nfdEndpoints"]},
        "roles": role_map,
        "timeouts": profile["timeouts"],
        "resources": resources,
        "runRoot": str(run_root),
    }


def offline_prepare(profile_path: Path, *, run_id: str, output: Optional[Path] = None,
                    candidate_output: Optional[Path] = None) -> Dict[str, Any]:
    profile = candidate.load_profile(profile_path, repo_root=REPO_ROOT)
    manifest = candidate.build_candidate_manifest(profile, repo_root=REPO_ROOT)
    if candidate_output:
        _write(candidate_output, manifest)
    try:
        effective = render_effective(profile, manifest, run_id,
                                     Path("${RUN_ROOT}") / run_id)
    except RenderError as exc:
        result = {"schemaVersion": "spec186-prepare-v1", "status": "REJECTED",
                  "runId": run_id, "renderError": str(exc),
                  "sideEffects": {"ssh": 0, "rsync": 0,
                                   "staging": 0, "sbatch": 0}}
        if output:
            _write(output, result)
        return result
    result = {"schemaVersion": "spec186-prepare-v1", "candidate": manifest,
              "effective": effective, "sideEffects": {"ssh": 0, "rsync": 0,
              "staging": 0, "sbatch": 0}}
    if output:
        _write(output, result)
    return result


def offline_check(profile_path: Path, candidate_path: Path) -> Dict[str, Any]:
    return candidate.pre_dispatch(profile_path, candidate_path, repo_root=REPO_ROOT)


def submit(profile_path: Path, candidate_path: Path, *, run_id: str,
           run_root: Path) -> Dict[str, Any]:
    try:
        run_root = Path(run_root).resolve()
    except (OSError, RuntimeError, ValueError) as exc:
        return {"schemaVersion": "spec186-submit-v1", "status": "REJECTED",
                "runId": run_id, "schedulerCalls": 0,
                "runRootError": "SPEC186_RUN_ROOT_INVALID:" + type(exc).__name__}
    try:
        profile = candidate.load_profile(profile_path, repo_root=REPO_ROOT)
        manifest = _read(candidate_path)
    except candidate.CandidateError as exc:
        return {"schemaVersion": "spec186-submit-v1", "status": "REJECTED",
                "runId": run_id, "schedulerCalls": 0,
                "error": "CANDIDATE_INPUT_INVALID:" + str(exc)}
    gate = candidate.pre_dispatch(profile_path, candidate_path, repo_root=REPO_ROOT)
    if not gate["ok"]:
        return {"schemaVersion": "spec186-submit-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "schedulerCalls": 0}
    try:
        effective = render_effective(profile, manifest, run_id, run_root)
    except RenderError as exc:
        return {"schemaVersion": "spec186-submit-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "schedulerCalls": 0,
                "renderError": str(exc)}
    if run_root.exists() or run_root == run_root.parent:
        return {"schemaVersion": "spec186-submit-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "schedulerCalls": 0,
                "runRootError": "SPEC186_RUN_ROOT_EXISTS_OR_INVALID"}
    resources = profile["resources"]
    argv = ["sbatch", "--parsable", "--nodes=" + str(profile["topology"]["nodes"]),
            "--ntasks-per-node=1",
            "--cpus-per-task=" + str(resources["cpusPerNode"]),
            "--mem=" + resources["memory"],
            "--partition=" + resources["partition"], "--account=" + resources["account"],
            "--job-name=spec186-" + run_id,
            str(HERE / "run.sbatch")]
    if resources["gpus"]:
        argv.insert(4, "--gpus-per-node=" + str(resources["gpus"]))
    env = {"SPEC186_RUN_ID": run_id,
           "SPEC186_EFFECTIVE_CONFIG": json.dumps(effective, sort_keys=True),
           "SPEC186_EXEC_ARGV": json.dumps(effective["argv"]),
           "SPEC186_EXEC_ENV": json.dumps(effective["environment"], sort_keys=True),
           "SPEC186_EXECUTE": "1"}
    try:
        result = subprocess.run(argv, env={**os.environ, **env}, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                check=False, timeout=30)
    except (OSError, subprocess.SubprocessError) as exc:
        return {"schemaVersion": "spec186-submit-v1", "status": "FAILED",
                "runId": run_id, "preDispatch": gate, "schedulerCalls": 1,
                "error": type(exc).__name__, "argv": argv}
    return {"schemaVersion": "spec186-submit-v1",
            "status": "SUBMITTED" if result.returncode == 0 else "FAILED",
            "runId": run_id, "preDispatch": gate, "schedulerCalls": 1,
            "argv": argv, "output": result.stdout, "returnCode": result.returncode}


def local_run(profile_path: Path, candidate_path: Path, *, run_id: str,
              run_root: Path, command: Optional[Sequence[str]] = None,
              dry_run: bool = False) -> Dict[str, Any]:
    try:
        run_root = Path(run_root).resolve()
    except (OSError, RuntimeError, ValueError) as exc:
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id,
                "runRootError": "SPEC186_RUN_ROOT_INVALID:" + type(exc).__name__,
                "cleanup": {"reaped": True}}
    gate = candidate.pre_dispatch(profile_path, candidate_path, repo_root=REPO_ROOT)
    if not gate["ok"]:
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "cleanup": {"reaped": True}}
    try:
        profile = candidate.load_profile(profile_path, repo_root=REPO_ROOT)
        manifest = _read(candidate_path)
    except candidate.CandidateError as exc:
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id, "error": "CANDIDATE_INPUT_INVALID:" + str(exc),
                "cleanup": {"reaped": True}}
    try:
        effective = render_effective(profile, manifest, run_id, run_root)
    except RenderError as exc:
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "renderError": str(exc),
                "cleanup": {"reaped": True}}
    argv = list(command) if command is not None else list(effective["argv"])
    if (not argv or any(not isinstance(item, str) or not item or "\x00" in item
                        for item in argv)):
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id, "candidateDigest": manifest["candidateDigest"],
                "error": "SPEC186_LOCAL_ARGV_INVALID",
                "cleanup": {"reaped": True}, "effective": effective}
    if dry_run:
        return {"schemaVersion": "spec186-local-v1", "status": "DRY_RUN",
                "runId": run_id, "candidateDigest": manifest["candidateDigest"],
                "argv": argv, "effective": effective, "cleanup": {"reaped": True}}
    if run_root.exists():
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id, "candidateDigest": manifest["candidateDigest"],
                "runRootError": "SPEC186_RUN_ROOT_EXISTS",
                "cleanup": {"reaped": True}}
    try:
        run_root.mkdir(mode=0o700, parents=True, exist_ok=False)
        for child in ("evidence", "state", "security", "home"):
            (run_root / child).mkdir(mode=0o700)
    except (OSError, ValueError) as exc:
        return {"schemaVersion": "spec186-local-v1", "status": "FAILED",
                "runId": run_id, "candidateDigest": manifest["candidateDigest"],
                "runRootError": "SPEC186_RUN_ROOT_CREATE_FAILED:" + type(exc).__name__,
                "cleanup": {"reaped": True}}
    log_path = run_root / "process.log"
    child = None
    exit_code = 125
    forced = False
    try:
        with log_path.open("wb") as log:
            # Keep the host launcher usable while preventing host ABI and
            # Python search paths from leaking into the candidate process.
            child_env = {
                "PATH": "/usr/local/bin:/usr/bin:/bin",
                "HOME": str(run_root / "home"),
                "LC_ALL": "C",
            }
            for name in ("USER", "LOGNAME", "SUDO_USER", "TMPDIR"):
                if os.environ.get(name):
                    child_env[name] = os.environ[name]
            child_env.update({str(key): str(value)
                              for key, value in effective["environment"].items()})
            child = subprocess.Popen(argv, cwd=str(run_root), env=child_env,
                                     stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            try:
                exit_code = child.wait(timeout=profile["timeouts"]["requestSeconds"])
            except subprocess.TimeoutExpired:
                forced = True
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    exit_code = child.wait(timeout=profile["timeouts"]["cleanupSeconds"])
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    exit_code = child.wait(timeout=5)
            finally:
                if child is not None and child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=5)
    except (OSError, ValueError) as exc:
        return {"schemaVersion": "spec186-local-v1", "status": "FAILED",
                "runId": run_id, "candidateDigest": manifest["candidateDigest"],
                "error": "SPEC186_LOCAL_EXEC_FAILED:" + type(exc).__name__,
                "cleanup": {"reaped": child is not None and child.poll() is not None,
                             "forced": forced}, "effective": effective}
    # A command override is intentionally diagnostic-only.  Treating an
    # arbitrary executable's zero exit as a qualification PASS would let a
    # caller bypass the declared MiniNDN/native launcher and candidate gate.
    status = "UNQUALIFIED" if command is not None and exit_code == 0 else (
        "PASS" if exit_code == 0 else "FAILED")
    return {"schemaVersion": "spec186-local-v1",
            "status": status,
            "runId": run_id, "candidateDigest": manifest["candidateDigest"],
            "argv": argv, "exitCode": exit_code,
            "cleanup": {"reaped": child is not None and child.poll() is not None,
                         "forced": forced}, "effective": effective}


def collect(receipt_paths: Sequence[Path], candidate_digest: str) -> Dict[str, Any]:
    try:
        records: List[Mapping[str, Any]] = [_read(path) for path in receipt_paths]
    except candidate.CandidateError as exc:
        return {"status": "FAILED", "candidateDigest": candidate_digest,
                "failures": ["RECEIPT_INPUT_INVALID:" + str(exc)]}
    return candidate.validate_terminal(records, candidate_digest)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check"); check.add_argument("--profile", type=Path, required=True); check.add_argument("--candidate", type=Path, required=True)
    prep = sub.add_parser("prepare"); prep.add_argument("--profile", type=Path, required=True); prep.add_argument("--run-id", required=True); prep.add_argument("--output", type=Path); prep.add_argument("--candidate-output", type=Path)
    submit_p = sub.add_parser("submit"); submit_p.add_argument("--profile", type=Path, required=True); submit_p.add_argument("--candidate", type=Path, required=True); submit_p.add_argument("--run-id", required=True); submit_p.add_argument("--run-root", type=Path, required=True)
    local = sub.add_parser("local"); local.add_argument("--profile", type=Path, required=True); local.add_argument("--candidate", type=Path, required=True); local.add_argument("--run-id", required=True); local.add_argument("--run-root", type=Path, required=True); local.add_argument("--dry-run", action="store_true"); local.add_argument("command_args", nargs=argparse.REMAINDER)
    coll = sub.add_parser("collect"); coll.add_argument("--candidate-digest", required=True); coll.add_argument("receipts", nargs="+", type=Path)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    if args.command == "check":
        result = offline_check(args.profile, args.candidate)
    elif args.command == "prepare":
        result = offline_prepare(args.profile, run_id=args.run_id,
                                 output=args.output, candidate_output=args.candidate_output)
    elif args.command == "submit":
        result = submit(args.profile, args.candidate, run_id=args.run_id, run_root=args.run_root)
    elif args.command == "local":
        command = list(args.command_args) if args.command_args else None
        if command and command[0] == "--":
            command = command[1:]
        result = local_run(args.profile, args.candidate, run_id=args.run_id,
                           run_root=args.run_root, command=command, dry_run=args.dry_run)
    else:
        result = collect(args.receipts, args.candidate_digest)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result.get("status") in {None, "PASS", "DRY_RUN", "SUBMITTED"} or result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
