#!/usr/bin/env python3
"""Bounded Spec182 native-closure runner.

The runner owns only staging, observation, and evidence classification.  It
never creates a plan or a business oracle.  Missing observation is reported as
UNQUALIFIED (exit 2), so a collector failure cannot become a protocol result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_SCHEMA = "spec182-native-case-manifest-v1"
ISOLATION_SCHEMA = "spec182-native-isolation-v1"
REQUIRED_EVIDENCE = {
    "identity", "process-tree", "namespace", "exec-map", "endpoints",
    "business-oracle", "cleanup",
}
FORBIDDEN_ENV_PREFIXES = ("PYTHON", "VIRTUAL_ENV", "CONDA_", "LD_PRELOAD", "LD_AUDIT")
TRACE_EXEC = re.compile(r"(?:execve|execveat)\([^)]*\)\s*=\s*(-?\d+)")
TRACE_EXIT = re.compile(r"(?:exit_group|exit)\((-?\d+)\)")


class PreflightError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise PreflightError(message)


def load_case(manifest_path: Path, case_id: str) -> dict[str, Any]:
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PreflightError(f"manifest unreadable: {exc}") from exc
    _require(document.get("schema") == MANIFEST_SCHEMA, "manifest schema mismatch")
    cases = document.get("cases")
    _require(isinstance(cases, list) and cases, "manifest has no cases")
    selected = [case for case in cases if isinstance(case, dict) and case.get("id") == case_id]
    _require(len(selected) == 1, "case id is not unique")
    case = selected[0]
    isolation = case.get("isolation")
    _require(isinstance(isolation, dict), "case isolation is missing")
    _require(isolation.get("schema") == ISOLATION_SCHEMA, "isolation schema mismatch")
    artifacts = isolation.get("artifacts")
    processes = isolation.get("processes")
    _require(isinstance(artifacts, list) and artifacts, "isolation artifacts are missing")
    _require(isinstance(processes, list) and processes, "isolation processes are missing")
    _require(set(isolation.get("requiredEvidence", [])) >= REQUIRED_EVIDENCE,
             "required evidence list is incomplete")
    limits = isolation.get("limits", {})
    _require(isinstance(limits, dict), "isolation limits are invalid")
    _require(0 < int(limits.get("runSeconds", 0)) <= 3600,
             "runSeconds is outside the bounded range")
    _require(0 < int(limits.get("cleanupSeconds", 0)) <= 120,
             "cleanupSeconds is outside the bounded range")
    seen_targets: set[str] = set()
    for artifact in artifacts:
        _require(isinstance(artifact, dict), "artifact entry is invalid")
        target = str(artifact.get("target", ""))
        _require(target.startswith("/") and target not in seen_targets,
                 "artifact target is not unique and absolute")
        seen_targets.add(target)
        _require(artifact.get("kind") in {"executable", "shared-library", "data", "config"},
                 "artifact kind is invalid")
        _require(re.fullmatch(r"sha256:[0-9a-f]{64}", str(artifact.get("sha256", "")))
                 is not None, "artifact digest is not canonical")
    seen_processes: set[str] = set()
    artifact_targets = seen_targets
    allowed_env = {"HOME", "TMPDIR", "LC_ALL", "NDN_CLIENT_CONF", "NDN_DAEMON_CONF"}
    for process in processes:
        _require(isinstance(process, dict), "process entry is invalid")
        pid = str(process.get("id", ""))
        _require(pid and pid not in seen_processes, "process id is not unique")
        seen_processes.add(pid)
        executable = str(process.get("executable", ""))
        _require(executable in artifact_targets, "process executable is undeclared")
        env = process.get("env", {})
        _require(isinstance(env, dict), "process environment is invalid")
        for key in env:
            _require(key in allowed_env and not key.startswith(FORBIDDEN_ENV_PREFIXES),
                     f"forbidden process environment: {key}")
    return case


def stage_root(case: dict[str, Any], output: Path) -> dict[str, Any]:
    _require(not output.exists(), "output run directory must be new")
    output.mkdir(parents=True)
    root = output / "root"
    root.mkdir()
    staged: list[dict[str, Any]] = []
    for artifact in case["isolation"]["artifacts"]:
        source = (ROOT / artifact["source"]).resolve()
        _require(source.is_file(), f"artifact source is missing: {source}")
        _require(_sha256(source) == artifact["sha256"],
                 f"artifact digest mismatch: {artifact['source']}")
        if artifact["kind"] in {"executable", "shared-library"}:
            identity = source.read_bytes()
            _require(b"libpython" not in identity and b"python3" not in identity,
                     "Python runtime identity is present in native artifact")
        destination = root / artifact["target"].lstrip("/")
        _require(root in destination.parents, "artifact target escapes root")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(int(str(artifact.get("mode", "0555")), 8))
        staged.append({"target": artifact["target"], "sha256": _sha256(destination),
                       "mode": oct(destination.stat().st_mode & 0o777)})
    return {"root": str(root), "artifacts": staged}


def make_launch(case: dict[str, Any], staged: dict[str, Any], node: dict[str, Any],
                trace_path: Path) -> list[str]:
    process = case["isolation"]["processes"][0]
    _require(process.get("node") in {None, node.get("id")}, "process/node binding mismatch")
    executable = "/probe-root" + process["executable"]
    argv = [str(value) for value in process.get("argv", [])]
    _require(argv and argv[0] == executable, "argv must begin with staged executable")
    bwrap = str(case["isolation"].get("tools", {}).get("bubblewrap", "bwrap"))
    strace = str(case["isolation"].get("tools", {}).get("strace", "strace"))
    return [strace, "-f", "-o", str(trace_path), bwrap,
            "--unshare-all", "--cap-drop", "ALL", "--new-session",
            "--die-with-parent", "--ro-bind", staged["root"], "/probe-root",
            "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
            "--chdir", "/tmp", "--", *argv]


def run_case(case: dict[str, Any], staged: dict[str, Any], output: Path,
             nodes: dict[str, dict[str, Any]] | None = None) -> dict[str, Any]:
    nodes = nodes or {}
    trace_path = output / "trace.txt"
    node = nodes.get(str(case["isolation"]["processes"][0].get("node", "")), {"id": ""})
    command = make_launch(case, staged, node, trace_path)
    env = {"HOME": "/tmp", "TMPDIR": "/tmp", "LC_ALL": "C"}
    start = time.monotonic()
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=env, start_new_session=True,
                               text=True)
    try:
        stdout, stderr = process.communicate(
            timeout=int(case["isolation"]["limits"]["runSeconds"]))
        timed_out = False
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(process.pid, signal.SIGTERM)
        try:
            stdout, stderr = process.communicate(
                timeout=int(case["isolation"]["limits"]["cleanupSeconds"]))
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
    (output / "stdout.log").write_text(stdout or "", encoding="utf-8")
    (output / "stderr.log").write_text(stderr or "", encoding="utf-8")
    return {"command": command, "returncode": process.returncode, "timedOut": timed_out,
            "durationMs": int((time.monotonic() - start) * 1000),
            "trace": str(trace_path), "stdout": str(output / "stdout.log"),
            "stderr": str(output / "stderr.log")}


def collect_trace(case: dict[str, Any], run: dict[str, Any]) -> dict[str, Any]:
    trace_path = Path(run["trace"])
    if not trace_path.is_file():
        return {"complete": False, "violations": ["TRACE_MISSING"], "events": []}
    text = trace_path.read_text(encoding="utf-8", errors="replace")
    violations: list[str] = []
    if "unfinished ..." in text or "<..." in text:
        violations.append("TRACE_UNPAIRED")
    events = []
    for line in text.splitlines():
        if "execve" in line or "execveat" in line:
            match = TRACE_EXEC.search(line)
            events.append({"kind": "exec", "line": line})
            if match and match.group(1) == "0" and ("python" in line.lower() or "libpython" in line.lower()):
                violations.append("PYTHON_EXEC")
        if "libpython" in line.lower() or "python3" in line.lower():
            violations.append("PYTHON_MAPPING")
        if "connect(" in line and "= 0" in line and "/run/" not in line:
            violations.append("UNDECLARED_ENDPOINT")
        match = TRACE_EXIT.search(line)
        if match:
            events.append({"kind": "exit", "code": int(match.group(1)), "line": line})
    return {"complete": not violations, "violations": sorted(set(violations)), "events": events}


def evaluate_case(case: dict[str, Any], run: dict[str, Any], observation: dict[str, Any]) -> dict[str, Any]:
    required = set(case["isolation"]["requiredEvidence"])
    evidence = {"identity", "process-tree", "namespace", "exec-map", "endpoints",
                "business-oracle", "cleanup"}
    failures = []
    if required - evidence:
        failures.append("EVIDENCE_DECLARATION_INCOMPLETE")
    if run.get("timedOut"):
        failures.append("RUN_TIMEOUT")
    if run.get("returncode") != int(case.get("expectedExit", 0)):
        failures.append("BUSINESS_EXIT_MISMATCH")
    failures.extend(observation.get("violations", []))
    if not observation.get("complete"):
        failures.append("OBSERVATION_UNQUALIFIED")
    return {"status": "PASS" if not failures else "FAIL", "failures": sorted(set(failures))}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    result: dict[str, Any]
    try:
        case = load_case(args.manifest, args.case)
        staged = stage_root(case, args.output)
        run = run_case(case, staged, args.output)
        observation = collect_trace(case, run)
        result = {"case": args.case, "staged": staged, "run": run,
                  "observation": observation,
                  "evaluation": evaluate_case(case, run, observation)}
    except (PreflightError, OSError, subprocess.SubprocessError) as exc:
        result = {"case": args.case, "status": "UNQUALIFIED", "error": str(exc)}
    (args.output / "result.json").parent.mkdir(parents=True, exist_ok=True)
    (args.output / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
    if result.get("status") == "UNQUALIFIED" or result.get("evaluation", {}).get("status") == "UNQUALIFIED":
        return 2
    return 0 if result.get("evaluation", {}).get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
