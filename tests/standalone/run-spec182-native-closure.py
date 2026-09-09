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
PROCESS_ROLES = frozenset({"requester", "provider", "authority", "nfd", "repo", "controller"})
TRACE_EXEC = re.compile(r"(?:execve|execveat)\([^)]*\)\s*=\s*(-?\d+)")
TRACE_EXIT = re.compile(r"(?:exit_group|exit)\((-?\d+)\)")
TRACE_PID = re.compile(r"^\s*(?:\[pid\s+)?(\d+)(?:\]|\s)")
TRACE_RESUMED = re.compile(r"<\.\.\. [^>]+ resumed>")
TRACE_SYSCALL = re.compile(r"^\s*(?:\[pid\s+)?\d+(?:\])?\s+([A-Za-z_][A-Za-z0-9_]*)\(")
TRACE_CALL_RESULT = re.compile(r"\)\s*=\s*(-?\d+)")
OBSERVED_SYSCALLS = frozenset({
    "clone", "clone3", "fork", "vfork", "open", "openat", "close", "dup",
    "dup2", "dup3", "mmap", "mprotect", "munmap", "socket", "connect",
})


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


def _validate_node_context(case: dict[str, Any], node: dict[str, Any],
                           process: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate the MiniNDN node identity that will own a declared process."""
    process = process or case["isolation"]["processes"][0]
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
    if "businessOracle" in case:
        oracle = case["businessOracle"]
        _require(isinstance(oracle, dict), "businessOracle must be an object")
        marker = oracle.get("stdoutMarker")
        _require(isinstance(marker, str) and bool(marker) and "\n" not in marker
                 and "\r" not in marker and len(marker) <= 4096,
                 "businessOracle.stdoutMarker is invalid")
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
    process_roles: dict[str, str] = {}
    artifact_targets = seen_targets
    allowed_env = {"HOME", "TMPDIR", "LC_ALL", "NDN_CLIENT_CONF", "NDN_DAEMON_CONF"}
    for process in processes:
        _require(isinstance(process, dict), "process entry is invalid")
        pid = str(process.get("id", ""))
        _require(re.fullmatch(r"[A-Za-z0-9_.-]+", pid) is not None and
                 pid not in seen_processes, "process id is not unique and safe")
        seen_processes.add(pid)
        _require(process.get("role") in PROCESS_ROLES, "process role is invalid")
        process_roles[pid] = str(process["role"])
        executable = str(process.get("executable", ""))
        _require(executable in artifact_targets, "process executable is undeclared")
        argv = process.get("argv", [])
        _require(isinstance(argv, list) and all(isinstance(value, str) for value in argv),
                 "process argv is invalid")
        env = process.get("env", {})
        _require(isinstance(env, dict), "process environment is invalid")
        for key in env:
            _require(key in allowed_env and not key.startswith(FORBIDDEN_ENV_PREFIXES),
                     f"forbidden process environment: {key}")
            _require(isinstance(env[key], str) and "\x00" not in env[key],
                     "process environment value is invalid")
            if key in {"NDN_CLIENT_CONF", "NDN_DAEMON_CONF"}:
                value = env[key]
                _require(value == "/tmp" or value == "/probe-root" or
                         value.startswith("/probe-root/"),
                         "NDN configuration must be staged or in /tmp")
        working_directory = process.get("workingDirectory")
        if working_directory is not None:
            _require(isinstance(working_directory, str) and working_directory.startswith("/"),
                     "workingDirectory must be absolute")
            _require(working_directory == "/tmp" or
                     working_directory == "/probe-root" or
                     working_directory.startswith("/probe-root/"),
                     "workingDirectory must stay inside staged root or /tmp")
    child_processes = isolation.get("childProcesses", [])
    _require(isinstance(child_processes, list), "childProcesses is invalid")
    for child in child_processes:
        _require(isinstance(child, dict), "child process entry is invalid")
        _require(child.get("role") == "assembly-worker",
                 "child process role is invalid")
        _require(str(child.get("executable", "")) in artifact_targets,
                 "child process executable is undeclared")
        parents = child.get("parentProcessIds")
        _require(isinstance(parents, list) and parents and
                 all(isinstance(parent, str) and parent in seen_processes
                     for parent in parents),
                 "child process parents are invalid")
        _require(all(process_roles[parent] == "provider" for parent in parents),
                 "child process parent role is invalid")
        try:
            max_concurrent = int(child.get("maxConcurrentPerParent", 0))
        except (TypeError, ValueError) as exc:
            raise PreflightError("child process concurrency is invalid") from exc
        _require(0 < max_concurrent <= 1024, "child process concurrency is invalid")
    endpoints = isolation.get("endpoints", [])
    _require(isinstance(endpoints, list), "endpoints is invalid")
    for endpoint in endpoints:
        _require(isinstance(endpoint, dict), "endpoint entry is invalid")
        _require(endpoint.get("ownerProcess") in seen_processes,
                 "endpoint owner is undeclared")
        transport = endpoint.get("transport")
        address = endpoint.get("address")
        _require(isinstance(transport, str) and bool(transport),
                 "endpoint transport is invalid")
        _require(isinstance(address, str) and bool(address) and
                 "\x00" not in address and "\n" not in address and
                 "\r" not in address, "endpoint address is invalid")
        if transport == "unix":
            _require(address.startswith("/") and not address.startswith("@"),
                     "filesystem UNIX endpoint must be absolute")
        peers = endpoint.get("peerProcessIds")
        _require(isinstance(peers, list) and peers and
                 all(isinstance(peer, str) and peer in seen_processes for peer in peers),
                 "endpoint peers are invalid")
        _require(isinstance(endpoint.get("purpose"), str) and
                 bool(endpoint["purpose"]), "endpoint purpose is invalid")
    return case


def _process_for_id(case: dict[str, Any], process_id: str | None) -> dict[str, Any]:
    processes = case["isolation"]["processes"]
    if process_id is None:
        return processes[0]
    selected = [process for process in processes if process.get("id") == process_id]
    _require(len(selected) == 1, "process id is not declared")
    return selected[0]


def _process_environment(process: dict[str, Any]) -> dict[str, str]:
    """Build the explicit environment for one isolated business process."""
    environment = {"HOME": "/tmp", "TMPDIR": "/tmp", "LC_ALL": "C"}
    for key, value in process.get("env", {}).items():
        _require(isinstance(key, str) and isinstance(value, str),
                 "process environment entry is invalid")
        _require("\x00" not in value, "process environment value contains NUL")
        environment[key] = value
    return environment


def _start_supervisor() -> tuple[int, int]:
    """Create a stable process-group leader owned by this harness."""
    hold_read, hold_write = os.pipe()
    ready_read, ready_write = os.pipe()
    try:
        supervisor_pid = os.fork()
    except OSError as exc:
        for fd in (hold_read, hold_write, ready_read, ready_write):
            os.close(fd)
        raise PreflightError("runner supervisor fork failed") from exc
    if supervisor_pid == 0:
        os.close(hold_write)
        os.close(ready_read)
        try:
            # Keep the group in the parent's session so declared children can
            # join it with setpgid before their own bubblewrap session starts.
            os.setpgid(0, os.getpid())
            os.write(ready_write, b"1")
            os.close(ready_write)
            while os.read(hold_read, 4096):
                pass
        except OSError:
            os._exit(127)
        finally:
            os.close(hold_read)
        os._exit(0)
    os.close(hold_read)
    os.close(ready_write)
    ready = os.read(ready_read, 1)
    os.close(ready_read)
    if ready != b"1":
        try:
            os.kill(supervisor_pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(supervisor_pid, 0)
        os.close(hold_write)
        raise PreflightError("runner supervisor failed to start")
    return supervisor_pid, hold_write


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
                trace_path: Path, process_id: str | None = None) -> list[str]:
    process = _process_for_id(case, process_id)
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
    working_directory = process.get("workingDirectory", "/tmp")
    _require(isinstance(working_directory, str) and working_directory.startswith("/"),
             "workingDirectory must be absolute")
    _require(working_directory == "/tmp" or working_directory == "/probe-root" or
             working_directory.startswith("/probe-root/"),
             "workingDirectory must stay inside staged root or /tmp")
    launch = [strace, "-f", "-o", str(trace_path), bwrap,
            "--unshare-all", "--cap-drop", "ALL", "--new-session",
            "--die-with-parent", "--ro-bind", staged["root"], "/probe-root",
            "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
            "--chdir", working_directory, "--", *argv]
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
    processes = case["isolation"]["processes"]
    _require(output.is_dir(), "run output directory is missing")
    opened_fds: list[int] = []
    prepared: list[tuple[dict[str, Any], dict[str, Any], int | None]] = []
    children: list[dict[str, Any]] = []
    files: list[tuple[Any, Any]] = []
    trace_paths: list[Path] = []
    start = time.monotonic()
    timed_out = False
    leader_pid: int | None = None
    supervisor_hold_fd: int | None = None

    def terminate_group(sig: int) -> None:
        if leader_pid is None:
            return
        try:
            os.killpg(leader_pid, sig)
        except ProcessLookupError:
            pass

    try:
        leader_pid, supervisor_hold_fd = _start_supervisor()
        for process_spec in processes:
            process_node = process_spec.get("node")
            node = nodes.get(str(process_node), {"id": ""})
            node_record: dict[str, Any] | None = None
            namespace_fd: int | None = None
            if process_node is not None:
                _require(str(process_node) in nodes, "node context is missing")
                node_record = _validate_node_context(case, node, process_spec)
                try:
                    namespace_fd = os.open(node_record["netnsPath"], os.O_RDONLY | os.O_CLOEXEC)
                    _require(os.fstat(namespace_fd).st_ino == node_record["netnsInode"],
                             "node netns inode changed before launch")
                except (OSError, PreflightError) as exc:
                    if namespace_fd is not None:
                        os.close(namespace_fd)
                    if isinstance(exc, PreflightError):
                        raise
                    raise PreflightError("node netns FD cannot be opened") from exc
                node = dict(node_record)
                node["_namespaceFdPath"] = f"/proc/self/fd/{namespace_fd}"
                opened_fds.append(namespace_fd)
            prepared.append((process_spec, node, namespace_fd))

        for process_spec, node, namespace_fd in prepared:
            process_id = str(process_spec["id"])
            trace_path = output / ("trace.txt" if len(prepared) == 1
                                   else f"trace.{process_id}.txt")
            trace_paths.append(trace_path)
            stdout_path = output / ("stdout.log" if len(prepared) == 1
                                    else f"stdout.{process_id}.log")
            stderr_path = output / ("stderr.log" if len(prepared) == 1
                                    else f"stderr.{process_id}.log")
            stdout_file = stdout_path.open("w", encoding="utf-8")
            stderr_file = stderr_path.open("w", encoding="utf-8")
            files.append((stdout_file, stderr_file))
            command = make_launch(case, staged, node, trace_path, process_id)
            popen_kwargs: dict[str, Any] = {
                "stdin": subprocess.DEVNULL,
                "stdout": stdout_file,
                "stderr": stderr_file,
                "env": _process_environment(process_spec),
                "start_new_session": False,
                "pass_fds": (() if namespace_fd is None else (namespace_fd,)),
                "text": True,
            }
            # Join the stable harness supervisor's process group before exec
            # so a timeout terminates every declared requester/provider peer.
            popen_kwargs["preexec_fn"] = lambda pgid=leader_pid: os.setpgid(0, pgid)
            child = subprocess.Popen(command, **popen_kwargs)
            children.append({"id": process_id, "role": process_spec["role"],
                             "pid": child.pid, "process": child,
                             "command": command, "executable": process_spec["executable"],
                             "argv": list(process_spec.get("argv", [])),
                             "stdout": str(stdout_path), "stderr": str(stderr_path),
                             "trace": str(trace_path),
                             **({"node": node_record} if node_record is not None else {})})

        deadline = start + int(case["isolation"]["limits"]["runSeconds"])
        for child in children:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            try:
                child["process"].wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                timed_out = True
                break
        if timed_out:
            terminate_group(signal.SIGTERM)
            cleanup_deadline = time.monotonic() + int(
                case["isolation"]["limits"]["cleanupSeconds"])
            for child in children:
                remaining = cleanup_deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    child["process"].wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    continue
            if any(child["process"].poll() is None for child in children):
                terminate_group(signal.SIGKILL)
                for child in children:
                    child["process"].wait()
    except Exception:
        terminate_group(signal.SIGTERM)
        if leader_pid is not None:
            try:
                terminate_group(signal.SIGKILL)
            except OSError:
                pass
        for child in children:
            try:
                child["process"].wait(timeout=1)
            except (OSError, subprocess.TimeoutExpired):
                pass
        raise
    finally:
        if supervisor_hold_fd is not None:
            os.close(supervisor_hold_fd)
            supervisor_hold_fd = None
        if leader_pid is not None:
            try:
                os.waitpid(leader_pid, 0)
            except ChildProcessError:
                pass
        for stdout_file, stderr_file in files:
            stdout_file.close()
            stderr_file.close()
        for namespace_fd in opened_fds:
            os.close(namespace_fd)

    process_records: list[dict[str, Any]] = []
    for child in children:
        process = child["process"]
        process_records.append({key: value for key, value in child.items()
                                if key != "process"})
        process_records[-1]["returncode"] = process.returncode
    if len(process_records) > 1:
        merged_trace = output / "trace.txt"
        trace_chunks = []
        for path in trace_paths:
            try:
                trace_chunks.append(path.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                # Keep the canonical path absent/empty for collect_trace to
                # classify as an observation boundary; do not discard exit
                # status or process records because an observer failed.
                continue
        merged_trace.write_text("\n".join(trace_chunks), encoding="utf-8")
        merged_stdout = "\n".join(Path(record["stdout"]).read_text(
            encoding="utf-8", errors="replace") for record in process_records)
        merged_stderr = "\n".join(Path(record["stderr"]).read_text(
            encoding="utf-8", errors="replace") for record in process_records)
        (output / "stdout.log").write_text(merged_stdout, encoding="utf-8")
        (output / "stderr.log").write_text(merged_stderr, encoding="utf-8")
    else:
        merged_trace = trace_paths[0]
    returncodes = [record["returncode"] for record in process_records]
    returncode = next((code for code in returncodes if code != 0), 0)
    result: dict[str, Any] = {
        "command": process_records[0]["command"],
        "commands": [record["command"] for record in process_records],
        "supervisorPid": leader_pid,
        "returncode": returncode,
        "returncodes": returncodes,
        "timedOut": timed_out,
        "durationMs": int((time.monotonic() - start) * 1000),
        "trace": str(merged_trace), "traceFiles": [str(path) for path in trace_paths],
        "stdout": str(output / "stdout.log"), "stderr": str(output / "stderr.log"),
        "processes": process_records,
    }
    if process_records and process_records[0].get("node") is not None:
        result["node"] = process_records[0]["node"]
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
    pids: set[str] = set()
    successful_execs: list[dict[str, Any]] = []
    child_links: list[dict[str, str]] = []
    exit_events = 0
    events = []
    for line in text.splitlines():
        pid_match = TRACE_PID.match(line)
        pid = pid_match.group(1) if pid_match else None
        if pid is not None:
            pids.add(pid)
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
            successful = bool(match and match.group(1) == "0")
            events.append({"kind": "exec", "pid": pid, "success": successful, "line": line})
            if successful:
                successful_execs.append({"pid": pid, "line": line})
            if successful and ("python" in line.lower() or "libpython" in line.lower()):
                policy_violations.append("PYTHON_EXEC")
        if "libpython" in line.lower() or "python3" in line.lower():
            policy_violations.append("PYTHON_MAPPING")
        if "connect(" in line and "= 0" in line:
            declared_endpoints = case.get("isolation", {}).get("endpoints", [])
            if not any(isinstance(endpoint, dict) and
                       str(endpoint.get("address", "")) in line
                       for endpoint in declared_endpoints):
                policy_violations.append("UNDECLARED_ENDPOINT")
        syscall_match = TRACE_SYSCALL.match(line)
        if syscall_match and syscall_match.group(1) in OBSERVED_SYSCALLS:
            syscall_name = syscall_match.group(1)
            event = {"kind": "syscall", "name": syscall_name,
                     "pid": pid, "line": line}
            result_match = TRACE_CALL_RESULT.search(line)
            if syscall_name in {"clone", "clone3", "fork", "vfork"} and \
                    pid is not None and result_match and int(result_match.group(1)) > 0:
                child_links.append({"parentPid": pid, "childPid": result_match.group(1),
                                    "syscall": syscall_name})
                event["childPid"] = result_match.group(1)
            events.append(event)
        match = TRACE_EXIT.search(line)
        if match:
            exit_events += 1
            events.append({"kind": "exit", "code": int(match.group(1)), "line": line})
    if any(count > 0 for count in unfinished_by_pid.values()):
        integrity_violations.append("TRACE_UNPAIRED")
    command = [str(value) for value in run.get("command", [])]
    evidence: list[str] = []
    if pids and run.get("supervisorPid") is not None:
        evidence.append("identity")
    if successful_execs and exit_events:
        evidence.append("process-tree")
    if "nsenter" in command or "--unshare-all" in command:
        evidence.append("namespace")
    if successful_execs and not any(item in policy_violations
                                    for item in ("PYTHON_EXEC", "PYTHON_MAPPING")):
        evidence.append("exec-map")
    if "UNDECLARED_ENDPOINT" not in policy_violations:
        evidence.append("endpoints")
    if not run.get("timedOut") and run.get("returncode") is not None:
        evidence.append("cleanup")
    oracle = case.get("businessOracle")
    if isinstance(oracle, dict):
        marker = oracle.get("stdoutMarker")
        output_text = ""
        output_path = run.get("stdout")
        if output_path:
            try:
                output_text = Path(str(output_path)).read_text(
                    encoding="utf-8", errors="replace")
            except OSError:
                pass
        if isinstance(marker, str) and marker in output_text:
            evidence.append("business-oracle")
    observed_roles: list[str] = []
    role_candidates: dict[str, list[dict[str, Any]]] = {}
    for process in run.get("processes", []):
        if not isinstance(process, dict):
            continue
        expected = "/probe-root" + str(process.get("executable", ""))
        role_candidates.setdefault(expected, []).append(process)
    for expected, candidates in role_candidates.items():
        if not expected:
            continue
        expected_marker = f'"{expected}"'
        matches = [item for item in successful_execs
                   if expected_marker in item["line"]]
        # A shared executable cannot identify which role ran from a single
        # trace line.  Require one successful exec per declared process before
        # reporting the complete role set; this keeps missing peer startup
        # observable instead of allowing a requester exec to cover a provider.
        if len(matches) < len(candidates):
            continue
        for process in candidates:
            role = process.get("role")
            if isinstance(role, str) and role not in observed_roles:
                observed_roles.append(role)
    child_coverage: list[dict[str, Any]] = []
    for child in case.get("isolation", {}).get("childProcesses", []):
        if not isinstance(child, dict):
            continue
        expected = "/probe-root" + str(child.get("executable", ""))
        matching_execs = [item for item in successful_execs
                          if f'"{expected}"' in item["line"]]
        linked_pids = {link["childPid"] for link in child_links}
        observed = any(item.get("pid") in linked_pids for item in matching_execs)
        child_coverage.append({
            "role": child.get("role"), "executable": child.get("executable"),
            "parentProcessIds": list(child.get("parentProcessIds", [])),
            "observed": observed,
            "execPids": [item.get("pid") for item in matching_execs],
            "childLinks": child_links,
        })
    return {
        # Completeness describes whether the observer delivered a trustworthy
        # trace.  Policy violations are still a complete observation and must
        # therefore become a business/isolation FAIL rather than UNQUALIFIED.
        "complete": not integrity_violations,
        "violations": sorted(set(integrity_violations + policy_violations)),
        "integrityViolations": sorted(set(integrity_violations)),
        "policyViolations": sorted(set(policy_violations)),
        "events": events,
        "evidence": sorted(set(evidence)),
        "pids": sorted(pids),
        "successfulExecs": len(successful_execs),
        "exitEvents": exit_events,
        "roles": observed_roles,
        "childLinks": child_links,
        "childProcessCoverage": child_coverage,
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
    for child in observation.get("childProcessCoverage", []):
        if isinstance(child, dict) and not child.get("observed", False):
            failures.append("CHILD_PROCESS_MISSING")
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
