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
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, List, Mapping, Optional, Sequence


HERE = Path(__file__).resolve()
TIGER_ROOT = HERE.parents[2]
REPO_ROOT = HERE.parents[4]
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


def render_effective(profile: Mapping[str, Any], manifest: Mapping[str, Any],
                     run_id: str, run_root: Path) -> Dict[str, Any]:
    """Render all argv/env/binds before a scheduler or process mutation."""
    runtime = profile["runtime"]
    resources = profile["resources"]
    topology = profile["topology"]
    app = runtime["application"]
    launcher = runtime["harness"]["launcher"]
    role_map = [{"name": role["name"], "identity": role["identity"],
                 "node": role["node"], "gpu": role["gpu"],
                 "backend": role["backend"]} for role in profile["roles"]]
    gpu_devices = sorted({role["gpu"] for role in profile["roles"] if role["gpu"] >= 0})
    env = {
        "SPEC186_CASE": profile["case"],
        "SPEC186_RUN_ID": run_id,
        "SPEC186_CANDIDATE_DIGEST": manifest["candidateDigest"],
        "SPEC186_APPTAINER_VERSION": runtime["apptainer"]["version"],
        "NDN_CLIENT_TRANSPORT": "unix:///run/nfd.sock",
        "PYTHONNOUSERSITE": "1",
    }
    if gpu_devices:
        env["CUDA_VISIBLE_DEVICES"] = ",".join(str(device) for device in gpu_devices)
    binds = [
        {"source": str(run_root), "target": "/run/spec186", "mode": "rw"},
        {"source": runtime["baseSif"]["path"], "target": "/runtime/base.sif", "mode": "ro"},
        {"source": runtime["application"]["bundle"]["path"], "target": "/app/bundle", "mode": "ro"},
        {"source": profile["model"]["model"]["path"], "target": "/model/model", "mode": "ro"},
    ]
    if profile["case"].startswith("yolo-minindn"):
        case_name = "Y-N" if profile["case"].endswith("negative") else "Y-A"
        workload_argv = [sys.executable, launcher, "--case", case_name]
    elif profile["case"] == "qwen06b-minindn-cpu":
        workload_argv = [sys.executable, launcher, "--stage-manifest",
                         profile["model"]["model"]["path"]]
    else:
        workload_argv = [launcher, "--run-id", run_id,
                         "--candidate-digest", manifest["candidateDigest"]]
    argv = [runtime["apptainer"]["path"], "exec", "--cleanenv", "--containall",
            "--bind", str(run_root) + ":/run/spec186:rw",
            "--bind", runtime["application"]["bundle"]["path"] + ":/app/bundle:ro",
            runtime["baseSif"]["path"]] + workload_argv
    return {
        "schemaVersion": "spec186-effective-config-v1",
        "runId": run_id,
        "candidateDigest": manifest["candidateDigest"],
        "case": profile["case"],
        "argv": argv,
        "environment": env,
        "binds": binds,
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
    effective = render_effective(profile, manifest, run_id,
                                 Path("${RUN_ROOT}") / run_id)
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
    profile = candidate.load_profile(profile_path, repo_root=REPO_ROOT)
    manifest = _read(candidate_path)
    gate = candidate.pre_dispatch(profile_path, candidate_path, repo_root=REPO_ROOT)
    if not gate["ok"]:
        return {"schemaVersion": "spec186-submit-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "schedulerCalls": 0}
    effective = render_effective(profile, manifest, run_id, run_root)
    resources = profile["resources"]
    argv = ["sbatch", "--parsable", "--nodes=" + str(profile["topology"]["nodes"]),
            "--cpus-per-task=" + str(resources["cpusPerNode"]),
            "--partition=" + resources["partition"], "--account=" + resources["account"],
            "--job-name=spec186-" + run_id,
            str(HERE / "run.sbatch")]
    env = {"SPEC186_RUN_ID": run_id,
           "SPEC186_CANDIDATE": str(candidate_path),
           "SPEC186_EFFECTIVE_CONFIG": json.dumps(effective, sort_keys=True),
           "SPEC186_EXEC_ARGV": json.dumps(effective["argv"]),
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
    gate = candidate.pre_dispatch(profile_path, candidate_path, repo_root=REPO_ROOT)
    if not gate["ok"]:
        return {"schemaVersion": "spec186-local-v1", "status": "REJECTED",
                "runId": run_id, "preDispatch": gate, "cleanup": {"reaped": True}}
    profile = candidate.load_profile(profile_path, repo_root=REPO_ROOT)
    manifest = _read(candidate_path)
    effective = render_effective(profile, manifest, run_id, run_root)
    argv = list(command) if command else render_effective(
        profile, manifest, run_id, run_root)["argv"]
    if dry_run:
        return {"schemaVersion": "spec186-local-v1", "status": "DRY_RUN",
                "runId": run_id, "candidateDigest": manifest["candidateDigest"],
                "argv": argv, "effective": effective, "cleanup": {"reaped": True}}
    run_root.mkdir(parents=True, exist_ok=False)
    log_path = run_root / "process.log"
    child = None
    exit_code = 125
    forced = False
    try:
        with log_path.open("wb") as log:
            child = subprocess.Popen(argv, cwd=str(run_root), stdout=log,
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
    return {"schemaVersion": "spec186-local-v1",
            "status": "PASS" if exit_code == 0 else "FAILED",
            "runId": run_id, "candidateDigest": manifest["candidateDigest"],
            "argv": argv, "exitCode": exit_code,
            "cleanup": {"reaped": child is not None and child.poll() is not None,
                         "forced": forced}, "effective": effective}


def collect(receipt_paths: Sequence[Path], candidate_digest: str) -> Dict[str, Any]:
    records: List[Mapping[str, Any]] = [_read(path) for path in receipt_paths]
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
