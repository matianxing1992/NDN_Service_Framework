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
import stat
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
TRACE_PID = re.compile(r"^\s*(?:\[pid\s+)?(\d+)(?:\]|\s)")
TRACE_RESUMED = re.compile(r"<\.\.\. [^>]+ resumed>")


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


def _read_proc_start_ticks(pid: int) -> int:
    """Read Linux ``/proc/<pid>/stat`` starttime without trusting a PID alone."""
    _require(pid > 0, "node owner PID is invalid")
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError as exc:
        raise PreflightError(f"node owner PID is not readable: {pid}") from exc
    closing = text.rfind(")")
    _require(closing > 0, "node owner stat record is malformed")
    fields = text[closing + 2:].split()
    # The tail starts at field 3 (state); field 22 (starttime) is index 19.
    _require(len(fields) > 19, "node owner stat record has no starttime")
    try:
        start_ticks = int(fields[19])
    except ValueError as exc:
        raise PreflightError("node owner starttime is not numeric") from exc
    _require(start_ticks > 0, "node owner starttime is invalid")
    return start_ticks


def _validate_node_context(case: dict[str, Any], node: dict[str, Any]) -> dict[str, Any]:
    """Validate the MiniNDN node identity that will own a declared process."""
    process = case["isolation"]["processes"][0]
    process_node = process.get("node")
    _require(isinstance(process_node, str) and process_node,
             "declared process node is missing")
    _require(node.get("id") == process_node, "process/node binding mismatch")
    required = ("netnsPath", "netnsInode", "ownerPid", "ownerStartTicks",
                "nfdSocket", "peerNodeIds")
    _require(all(key in node for key in required), "node context is incomplete")
    netns_path = Path(str(node["netnsPath"]))
    _require(netns_path.is_absolute(), "node netns path must be absolute")
    try:
        netns_stat = netns_path.stat()
    except OSError as exc:
        raise PreflightError("node netns path is not readable") from exc
    try:
        netns_inode = int(node["netnsInode"])
        owner_pid = int(node["ownerPid"])
        owner_start_ticks = int(node["ownerStartTicks"])
    except (TypeError, ValueError) as exc:
        raise PreflightError("node numeric metadata is invalid") from exc
    _require(netns_inode == netns_stat.st_ino,
             "node netns inode changed")
    try:
        owner_netns_inode = Path(f"/proc/{owner_pid}/ns/net").stat().st_ino
    except OSError as exc:
        raise PreflightError("node owner network namespace is not readable") from exc
    _require(owner_netns_inode == netns_inode,
             "node netns does not belong to owner PID")
    _require(_read_proc_start_ticks(owner_pid) == owner_start_ticks,
             "node owner starttime changed")
    nfd_socket = Path(str(node["nfdSocket"]))
    _require(nfd_socket.is_absolute(), "node NFD socket must be absolute")
    try:
        nfd_mode = nfd_socket.stat().st_mode
    except OSError as exc:
        raise PreflightError("node NFD socket is not readable") from exc
    _require(stat.S_ISSOCK(nfd_mode), "node NFD socket is not a socket")
    peer_ids = node["peerNodeIds"]
    _require(isinstance(peer_ids, list) and
             all(isinstance(peer, str) and peer for peer in peer_ids) and
             len(set(peer_ids)) == len(peer_ids),
             "node peer metadata is invalid")
    return {
        "id": process_node,
        "netnsPath": str(netns_path),
        "netnsInode": netns_stat.st_ino,
        "ownerPid": owner_pid,
        "ownerStartTicks": owner_start_ticks,
        "nfdSocket": str(nfd_socket),
        "peerNodeIds": list(peer_ids),
    }


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
    if "requiredRoles" in case:
        required_roles = case["requiredRoles"]
        _require(isinstance(required_roles, list) and required_roles,
                 "requiredRoles must be a non-empty list")
        _require(all(isinstance(role, str) and role for role in required_roles),
                 "requiredRoles contains an invalid role")
        _require(len(set(required_roles)) == len(required_roles),
                 "requiredRoles contains a duplicate role")
    if "cold" in case:
        _require(isinstance(case["cold"], bool), "cold marker must be boolean")
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
    if process.get("node") is not None:
        namespace_fd_path = str(node.get("_namespaceFdPath", ""))
        _require(re.fullmatch(r"/proc/self/fd/[0-9]+", namespace_fd_path) is not None,
                 "node namespace FD was not prepared")
    executable = "/probe-root" + process["executable"]
    argv = [str(value) for value in process.get("argv", [])]
    _require(argv and argv[0] == executable, "argv must begin with staged executable")
    bwrap = str(case["isolation"].get("tools", {}).get("bubblewrap", "bwrap"))
    strace = str(case["isolation"].get("tools", {}).get("strace", "strace"))
    launch = [strace, "-f", "-o", str(trace_path), bwrap,
            "--unshare-all", "--cap-drop", "ALL", "--new-session",
            "--die-with-parent", "--ro-bind", staged["root"], "/probe-root",
            "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
            "--chdir", "/tmp", "--", *argv]
    # The executable lives under /probe-root, but ELF PT_INTERP and DT_NEEDED
    # entries are absolute paths.  Bind each declared shared library at its
    # canonical absolute target without exposing the host's whole /lib tree.
    insert_at = launch.index("--proc")
    shared_mounts: list[str] = []
    for artifact in case["isolation"]["artifacts"]:
        if artifact["kind"] != "shared-library":
            continue
        target = str(artifact["target"])
        source = str(Path(staged["root"]) / target.lstrip("/"))
        _require(Path(source).is_file(), f"staged shared library is missing: {target}")
        shared_mounts.extend(["--ro-bind", source, target])
    launch[insert_at:insert_at] = shared_mounts
    namespace_fd_path = node.get("_namespaceFdPath")
    if process.get("node") is not None and namespace_fd_path:
        nsenter = str(case["isolation"].get("tools", {}).get("nsenter", "nsenter"))
        return [nsenter, f"--net={namespace_fd_path}", "--", *launch]
    return launch


def run_case(case: dict[str, Any], staged: dict[str, Any], output: Path,
             nodes: dict[str, dict[str, Any]] | None = None) -> dict[str, Any]:
    nodes = nodes or {}
    trace_path = output / "trace.txt"
    process_node = case["isolation"]["processes"][0].get("node")
    node = nodes.get(str(process_node), {"id": ""})
    namespace_fd: int | None = None
    node_record: dict[str, Any] | None = None
    if process_node is not None:
        _require(str(process_node) in nodes, "node context is missing")
        node_record = _validate_node_context(case, node)
        try:
            namespace_fd = os.open(node_record["netnsPath"], os.O_RDONLY | os.O_CLOEXEC)
            _require(os.fstat(namespace_fd).st_ino == node_record["netnsInode"],
                     "node netns inode changed before launch")
            node = dict(node_record)
            node["_namespaceFdPath"] = f"/proc/self/fd/{namespace_fd}"
        except OSError as exc:
            if namespace_fd is not None:
                os.close(namespace_fd)
                namespace_fd = None
            raise PreflightError("node netns FD cannot be opened") from exc
        except PreflightError:
            if namespace_fd is not None:
                os.close(namespace_fd)
                namespace_fd = None
            raise
    try:
        command = make_launch(case, staged, node, trace_path)
    except Exception:
        if namespace_fd is not None:
            os.close(namespace_fd)
            namespace_fd = None
        raise
    env = {"HOME": "/tmp", "TMPDIR": "/tmp", "LC_ALL": "C"}
    start = time.monotonic()
    try:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=env, start_new_session=True,
                                   pass_fds=(() if namespace_fd is None else (namespace_fd,)),
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
    finally:
        if namespace_fd is not None:
            os.close(namespace_fd)
    (output / "stdout.log").write_text(stdout or "", encoding="utf-8")
    (output / "stderr.log").write_text(stderr or "", encoding="utf-8")
    result = {"command": command, "returncode": process.returncode, "timedOut": timed_out,
              "durationMs": int((time.monotonic() - start) * 1000),
              "trace": str(trace_path), "stdout": str(output / "stdout.log"),
              "stderr": str(output / "stderr.log")}
    if node_record is not None:
        result["node"] = node_record
    return result


def collect_trace(case: dict[str, Any], run: dict[str, Any]) -> dict[str, Any]:
    trace_path = Path(run["trace"])
    if not trace_path.is_file():
        return {"complete": False, "violations": ["TRACE_MISSING"],
                "integrityViolations": ["TRACE_MISSING"], "policyViolations": [],
                "events": [], "evidence": []}
    text = trace_path.read_text(encoding="utf-8", errors="replace")
    integrity_violations: list[str] = []
    policy_violations: list[str] = []
    unfinished_by_pid: dict[str, int] = {}
    events = []
    for line in text.splitlines():
        pid_match = TRACE_PID.match(line)
        pid = pid_match.group(1) if pid_match else None
        if "<unfinished ...>" in line:
            if pid is None:
                integrity_violations.append("TRACE_UNPAIRED")
            else:
                unfinished_by_pid[pid] = unfinished_by_pid.get(pid, 0) + 1
        elif TRACE_RESUMED.search(line):
            if pid is None or unfinished_by_pid.get(pid, 0) <= 0:
                integrity_violations.append("TRACE_UNPAIRED")
            else:
                unfinished_by_pid[pid] -= 1
        if "execve" in line or "execveat" in line:
            match = TRACE_EXEC.search(line)
            events.append({"kind": "exec", "line": line})
            if match and match.group(1) == "0" and ("python" in line.lower() or "libpython" in line.lower()):
                policy_violations.append("PYTHON_EXEC")
        if "libpython" in line.lower() or "python3" in line.lower():
            policy_violations.append("PYTHON_MAPPING")
        if "connect(" in line and "= 0" in line and "/run/" not in line:
            policy_violations.append("UNDECLARED_ENDPOINT")
        match = TRACE_EXIT.search(line)
        if match:
            events.append({"kind": "exit", "code": int(match.group(1)), "line": line})
    if any(count > 0 for count in unfinished_by_pid.values()):
        integrity_violations.append("TRACE_UNPAIRED")
    return {
        # Completeness describes whether the observer delivered a trustworthy
        # trace.  Policy violations are still a complete observation and must
        # therefore become a business/isolation FAIL rather than UNQUALIFIED.
        "complete": not integrity_violations,
        "violations": sorted(set(integrity_violations + policy_violations)),
        "integrityViolations": sorted(set(integrity_violations)),
        "policyViolations": sorted(set(policy_violations)),
        "events": events,
        "evidence": [],
    }


def evaluate_case(case: dict[str, Any], run: dict[str, Any], observation: dict[str, Any]) -> dict[str, Any]:
    required = set(case["isolation"]["requiredEvidence"])
    evidence = set(run.get("evidence", [])) | set(observation.get("evidence", []))
    failures = []
    unqualified_observation = False
    failures.extend("MISSING_EVIDENCE:" + item for item in sorted(required - evidence))
    required_roles = set(case.get("requiredRoles", []))
    if required_roles:
        observed_roles = observation.get("roles")
        if observed_roles is None:
            failures.append("ROLE_OBSERVATION_MISSING")
            unqualified_observation = True
        elif not isinstance(observed_roles, (list, tuple, set, frozenset)) or \
                any(not isinstance(role, str) or not role for role in observed_roles) or \
                (not isinstance(observed_roles, (set, frozenset)) and
                 len(set(observed_roles)) != len(observed_roles)):
            failures.append("ROLE_OBSERVATION_INVALID")
            unqualified_observation = True
        elif set(observed_roles) != required_roles:
            failures.append("ROLE_COVERAGE_MISMATCH")
    if "cold" in case:
        if "coldVerified" not in observation:
            failures.append("COLD_PATH_OBSERVATION_MISSING")
            unqualified_observation = True
        elif not isinstance(observation["coldVerified"], bool):
            failures.append("COLD_PATH_OBSERVATION_INVALID")
            unqualified_observation = True
        elif observation["coldVerified"] != case["cold"]:
            failures.append("COLD_PATH_MISMATCH")
    if run.get("timedOut"):
        failures.append("RUN_TIMEOUT")
    if run.get("returncode") != int(case.get("expectedExit", 0)):
        failures.append("BUSINESS_EXIT_MISMATCH")
    failures.extend(observation.get("violations", []))
    if not observation.get("complete"):
        failures.append("OBSERVATION_UNQUALIFIED")
    failures = sorted(set(failures))
    # A missing/invalid observation cannot be interpreted as a protocol or
    # isolation result.  Keep this separate from an observed policy/business
    # violation so the CLI's exit-2 boundary remains meaningful.
    unqualified = (
        bool(run.get("timedOut"))
        or not observation.get("complete")
        or unqualified_observation
        or any(item.startswith("MISSING_EVIDENCE:") for item in failures)
    )
    status = "UNQUALIFIED" if unqualified else ("PASS" if not failures else "FAIL")
    return {"status": status, "failures": failures}


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
