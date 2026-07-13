#!/usr/bin/env python3
"""Local packaged-command supervisor for Spec 107 operational evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import subprocess
import time
from typing import Sequence


CANDIDATE_RE = re.compile(r"^spec107-c1(?:-[0-9a-f]{12}){6}$")
DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


class LocalSupervisorError(RuntimeError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise LocalSupervisorError(code + (f":{detail}" if detail else ""))


class LocalSupervisor:
    def __init__(self, *, staging_root: Path | str, release_root: Path | str,
                 candidate_id: str, plan_digest: str) -> None:
        if CANDIDATE_RE.fullmatch(candidate_id) is None:
            _fail("LOCAL_SUPERVISOR_CANDIDATE_INVALID")
        if DIGEST_RE.fullmatch(plan_digest) is None:
            _fail("LOCAL_SUPERVISOR_PLAN_DIGEST_INVALID")
        self.staging_root = Path(staging_root).resolve()
        self.release_root = Path(release_root).resolve()
        if not self.release_root.is_dir():
            _fail("LOCAL_SUPERVISOR_RELEASE_MISSING")
        self.candidate_id = candidate_id
        self.plan_digest = plan_digest
        self._processes: dict[str, dict[str, object]] = {}

    def _command(self, command: Sequence[str]) -> list[str]:
        if not command:
            _fail("LOCAL_SUPERVISOR_COMMAND_INVALID")
        relative = PurePosixPath(str(command[0]))
        if relative.is_absolute() or any(part in ("", ".", "..") for part in relative.parts):
            _fail("LOCAL_SUPERVISOR_COMMAND_ESCAPE")
        executable = (self.release_root / Path(*relative.parts)).resolve()
        try:
            executable.relative_to(self.release_root)
        except ValueError:
            _fail("LOCAL_SUPERVISOR_COMMAND_ESCAPE")
        if not executable.is_file() or not os.access(executable, os.X_OK):
            _fail("LOCAL_SUPERVISOR_COMMAND_NOT_EXECUTABLE")
        return [str(executable), *(str(value) for value in command[1:])]

    def start(self, name: str, command: Sequence[str], *, ready_marker: str,
              timeout_seconds: float) -> dict[str, object]:
        if not name or name in self._processes or not ready_marker or timeout_seconds <= 0:
            _fail("LOCAL_SUPERVISOR_START_INVALID", name)
        argv = self._command(command)
        log_root = self.staging_root / "logs"
        log_root.mkdir(parents=True, exist_ok=True)
        log_path = log_root / f"{name}.log"
        stream = log_path.open("xb")
        process = subprocess.Popen(
            argv, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        entry: dict[str, object] = {
            "name": name, "command": list(command), "argv": argv,
            "readyMarker": ready_marker, "logPath": str(log_path),
            "process": process, "stream": stream,
            "bootIdentity": f"{name}@{process.pid}@{time.monotonic_ns()}",
        }
        self._processes[name] = entry
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            stream.flush()
            if ready_marker in log_path.read_text(encoding="utf-8", errors="replace"):
                return self._public(entry, "READY")
            if process.poll() is not None:
                break
            time.sleep(0.01)
        self._stop(name)
        _fail("LOCAL_SUPERVISOR_READINESS_FAILED", name)

    @staticmethod
    def _public(entry: dict[str, object], state: str) -> dict[str, object]:
        process = entry["process"]
        assert isinstance(process, subprocess.Popen)
        argv = entry["argv"]
        assert isinstance(argv, list)
        command_digest = "sha256:" + hashlib.sha256(json.dumps(
            argv, separators=(",", ":")).encode()).hexdigest()
        return {
            "name": entry["name"], "pid": process.pid,
            "processGroupId": os.getpgid(process.pid), "state": state,
            "bootIdentity": entry["bootIdentity"],
            "commandDigest": command_digest, "logPath": entry["logPath"],
        }

    def status(self) -> dict[str, object]:
        rows = []
        for entry in self._processes.values():
            process = entry["process"]
            assert isinstance(process, subprocess.Popen)
            rows.append(self._public(
                entry, "READY" if process.poll() is None else "EXITED"))
        return {
            "schema": "ndnsf-di-spec107-local-supervisor-status-v1",
            "supervisionClass": "local-process-supervision",
            "physicalProductionDeferred": True,
            "candidateId": self.candidate_id,
            "planDigest": self.plan_digest,
            "releaseRoot": str(self.release_root),
            "processes": rows,
        }

    def restart(self, name: str, *, timeout_seconds: float) -> dict[str, object]:
        entry = self._processes.get(name)
        if entry is None:
            _fail("LOCAL_SUPERVISOR_PROCESS_UNKNOWN", name)
        command = entry["command"]
        marker = entry["readyMarker"]
        assert isinstance(command, list) and isinstance(marker, str)
        self._stop(name)
        log_path = self.staging_root / "logs" / f"{name}.log"
        if log_path.exists():
            log_path.unlink()
        return self.start(name, command, ready_marker=marker,
                          timeout_seconds=timeout_seconds)

    def _stop(self, name: str) -> None:
        entry = self._processes.pop(name, None)
        if entry is None:
            return
        process = entry["process"]
        stream = entry["stream"]
        assert isinstance(process, subprocess.Popen)
        if process.poll() is None:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
                process.wait(timeout=2)
        stream.close()  # type: ignore[union-attr]

    def stop_all(self) -> dict[str, object]:
        failures = []
        for name in list(self._processes):
            try:
                self._stop(name)
            except Exception:
                failures.append(name)
        return {"cleanupProven": not failures, "failedProcesses": failures}


__all__ = ["LocalSupervisor", "LocalSupervisorError"]


def _run_canary(args: argparse.Namespace) -> int:
    try:
        config = json.loads(Path(args.config).read_text(encoding="utf-8"))
        if config.get("schema") != "ndnsf-di-spec107-local-supervisor-config-v1":
            _fail("LOCAL_SUPERVISOR_CONFIG_INVALID")
        processes = config.get("processes")
        if not isinstance(processes, list) or not processes:
            _fail("LOCAL_SUPERVISOR_CONFIG_INVALID")
        supervisor = LocalSupervisor(
            staging_root=args.staging_root,
            release_root=config.get("releaseRoot", ""),
            candidate_id=config.get("candidateId", ""),
            plan_digest=config.get("planDigest", ""))
        started = []
        restarted = None
        try:
            for row in processes:
                if not isinstance(row, dict):
                    _fail("LOCAL_SUPERVISOR_CONFIG_INVALID")
                command = row.get("command")
                if not isinstance(command, list):
                    _fail("LOCAL_SUPERVISOR_CONFIG_INVALID")
                started.append(supervisor.start(
                    str(row.get("name", "")), command,
                    ready_marker=str(row.get("readyMarker", "")),
                    timeout_seconds=args.timeout_seconds))
            status = supervisor.status()
            if args.restart:
                restarted = supervisor.restart(
                    str(processes[0].get("name", "")),
                    timeout_seconds=args.timeout_seconds)
        finally:
            cleanup = supervisor.stop_all()
        record = {
            "schema": "ndnsf-di-spec107-local-canary-v1",
            "verdict": "PASS" if cleanup["cleanupProven"] else "BLOCK",
            "status": status,
            "started": started,
            "restart": restarted,
            "cleanup": cleanup,
            "physicalProductionDeferred": True,
        }
    except Exception as exc:
        record = {
            "schema": "ndnsf-di-spec107-local-canary-v1",
            "verdict": "BLOCK", "error": str(exc),
            "physicalProductionDeferred": True,
        }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("x", encoding="utf-8") as stream:
            json.dump(record, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except FileExistsError:
        print(f"LOCAL_SUPERVISOR_OUTPUT_EXISTS:{output}", file=sys.stderr)
        return 2
    print(json.dumps(record, sort_keys=True))
    return 0 if record["verdict"] == "PASS" else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    canary = commands.add_parser("canary")
    canary.add_argument("--config", required=True)
    canary.add_argument("--staging-root", required=True)
    canary.add_argument("--output", required=True)
    canary.add_argument("--timeout-seconds", type=float, default=20.0)
    canary.add_argument("--restart", action="store_true")
    args = parser.parse_args(argv)
    if args.command == "canary":
        return _run_canary(args)
    return 2


if __name__ == "__main__":
    import sys
    raise SystemExit(main())
