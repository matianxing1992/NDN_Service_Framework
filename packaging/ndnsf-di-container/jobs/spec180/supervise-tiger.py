#!/usr/bin/env python3
"""The node-local Tiger supervisor; no model, ACK, or result-oracle ownership."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")


class SupervisionError(RuntimeError):
    pass


@dataclass
class Child:
    name: str
    process: subprocess.Popen
    log: Path
    stream: object
    timed_out: bool = False


def group_alive(pgid: int) -> bool:
    """Include descendants after leader exit; zombies cannot execute or write."""
    for path in Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = path.read_text().rsplit(")", 1)[1].split()
            if int(fields[2]) == pgid and fields[0] != "Z":
                return True
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return False


class Supervisor:
    def __init__(self, logs: Path):
        self.logs = logs
        self.logs.mkdir(parents=True, exist_ok=True)
        self.children: list[Child] = []
        self.closed = False

    def start(self, name: str, argv: list[str], env: dict) -> Child:
        if self.closed or any(child.name == name for child in self.children):
            raise SupervisionError("DUPLICATE_CHILD_OR_CLOSED:" + name)
        if not re.fullmatch(r"[A-Za-z0-9-]+", name) or not argv:
            raise SupervisionError("INVALID_CHILD:" + name)
        log = self.logs / (name + ".log")
        stream = log.open("xb")
        try:
            process = subprocess.Popen(argv, env=env, stdout=stream,
                                       stderr=subprocess.STDOUT, start_new_session=True)
        except BaseException:
            stream.close()
            raise
        child = Child(name, process, log, stream)
        self.children.append(child)
        return child

    def check_alive(self, exclude: Child | None = None) -> None:
        for child in self.children:
            if child is not exclude and child.process.poll() is not None:
                raise SupervisionError("CHILD_EXITED:" + child.name)

    def wait_until(self, predicate, timeout_s: float, label: str) -> None:
        deadline = time.monotonic() + timeout_s
        while True:
            self.check_alive()
            if predicate():
                self.check_alive()
                return
            if time.monotonic() >= deadline:
                raise SupervisionError(label)
            time.sleep(min(0.05, max(0, deadline - time.monotonic())))

    def wait_marker(self, child: Child, marker: str, timeout_s: float) -> None:
        # Poll before examining bytes: a dead child with a stale marker is not ready.
        self.wait_until(lambda: marker in child.log.read_text(errors="replace"),
                        timeout_s, "READY_TIMEOUT:" + child.name)

    def wait_user(self, user: Child, timeout_s: float) -> None:
        deadline = time.monotonic() + timeout_s
        while True:
            self.check_alive(exclude=user)
            status = user.process.poll()
            if status is not None:
                if status != 0:
                    raise SupervisionError("USER_FAILED:" + str(status))
                return
            if time.monotonic() >= deadline:
                user.timed_out = True
                raise SupervisionError("USER_TIMEOUT")
            time.sleep(0.05)

    def cleanup(self, grace_s: float = 15, kill_s: float = 3) -> dict:
        self.closed = True
        def send(child, sig):
            try:
                os.killpg(child.process.pid, sig)
            except ProcessLookupError:
                pass

        pending = [child for child in self.children if group_alive(child.process.pid)]
        for child in pending:
            send(child, signal.SIGINT)
        deadline = time.monotonic() + grace_s
        while pending and time.monotonic() < deadline:
            for child in self.children:
                child.process.poll()
            pending = [child for child in pending if group_alive(child.process.pid)]
            if pending:
                time.sleep(0.02)
        for child in pending:
            child.timed_out = True
            send(child, signal.SIGKILL)
        deadline = time.monotonic() + kill_s
        while pending and time.monotonic() < deadline:
            for child in self.children:
                child.process.poll()
            pending = [child for child in pending if group_alive(child.process.pid)]
            if pending:
                time.sleep(0.02)
        rows = []
        for child in self.children:
            try:
                child.process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                child.timed_out = True
            child.stream.close()
            rows.append({"id": child.name, "exitStatus": child.process.returncode,
                         "timedOut": child.timed_out})
        return {"children": rows, "childExitCount": len(rows),
                "allExited": not pending and all(row["exitStatus"] is not None for row in rows),
                "outputClosed": all(child.stream.closed for child in self.children),
                "survivors": [child.name for child in pending],
                "pids": {child.name: child.process.pid for child in self.children}}


def validate_terminal(evidence: Path, digest: str, candidate_id=None, supervision=None) -> None:
    path = evidence / "spec180-result.json"
    if not path.is_file():
        raise SupervisionError("TERMINAL_RESULT_MISSING")
    spec = importlib.util.spec_from_file_location("spec180_results", ROOT / "scripts/validate_spec180_results.py")
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    result = json.loads(path.read_text())
    validator.validate_result(result, digest, evidence)
    if result["candidateId"] != candidate_id:
        raise SupervisionError("TERMINAL_CANDIDATE_MISMATCH")
    if supervision is None or result["children"] != supervision["children"]:
        raise SupervisionError("TERMINAL_CHILD_RECORD_MISMATCH")
    for provider in result["runtimeOracle"]["fields"]["providers"]:
        if provider["pid"] != supervision["pids"].get("provider-" + provider["role"]):
            raise SupervisionError("TERMINAL_PROVIDER_PID_MISMATCH")
    expected_cleanup = {key: supervision[key] for key in
                        ("childExitCount", "allExited", "outputClosed")}
    if result["cleanup"]["fields"] != expected_cleanup:
        raise SupervisionError("TERMINAL_CLEANUP_MISMATCH")


def execute(args) -> int:
    evidence, scratch = args.evidence.resolve(), args.scratch.resolve()
    digest = os.environ.get("SPEC180_CANDIDATE_DIGEST", "")
    candidate_id = os.environ.get("SPEC180_CANDIDATE_ID", "")
    if not os.environ.get("SLURM_JOB_ID") or not candidate_id or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise SupervisionError("TIGER_CANDIDATE_IDENTITY_REQUIRED")
    if scratch != evidence / "runtime" or scratch.exists():
        raise SupervisionError("SCRATCH_NOT_FRESH_OR_NOT_RUN_OWNED")
    if any((evidence / name).exists() for name in (
            "spec180-result.json", "orchestration-terminal.json", "supervision.json",
            "runtime-publication-receipt.json")):
        raise SupervisionError("EVIDENCE_NOT_FRESH")
    files = {"controller": args.controller_args, "repo": args.repo_args, "user": args.user_args}
    provider_files = {path.stem: path for path in args.provider_args_dir.glob("*.args")}
    if set(provider_files) != set(ROLES):
        raise SupervisionError("PROVIDER_ARGUMENT_INVENTORY_MISMATCH")
    files.update({"provider-" + role: provider_files[role] for role in ROLES})
    commands = {name: path.read_text().splitlines() for name, path in files.items()}
    if not args.nfd_config.is_file() or any(not argv or any(not arg for arg in argv) for argv in commands.values()):
        raise SupervisionError("ARGUMENT_FILE_INVALID")
    scratch.mkdir(parents=True, mode=0o700)
    (scratch / "run").mkdir()
    supervisor = Supervisor(scratch / "log")
    environment = dict(os.environ, NDN_CLIENT_TRANSPORT="unix://" + str(scratch / "run/nfd.sock"),
                       NDNSF_DI_STATE_ROOT=str(scratch / "state"),
                       NDNSF_HANDLER_THREADS="1", NDNSF_ACK_THREADS="1")
    homes = {name: scratch / "home" / name for name in ("root", "nfd", *commands)}
    failure = ""
    cleanup = None
    try:
        for home in homes.values():
            home.mkdir(parents=True, mode=0o700)
        bootstrap = HERE / "bootstrap-tiger-identities.sh"
        def bootstrap_run(*argv):
            subprocess.run(["bash", str(bootstrap), *map(str, argv)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        bootstrap_run("init", homes["root"], "/example")
        def add(name, identity):
            bootstrap_run("add", homes[name], homes["root"] / ".ndn/root.cert",
                          homes["root"] / ".ndn/root.key", identity)
        add("nfd", "/example")
        identities = {"controller": "/example/controller", "repo": "/example/provider/Repo",
                      "user": "/example/user", **{"provider-" + role: "/example/provider/" + role for role in ROLES}}
        for name, identity in identities.items():
            add("nfd", identity)
            add(name, identity)
        supervisor.start("nfd", ["nfd", "--config", str(args.nfd_config)],
                         dict(environment, HOME=str(homes["nfd"])))
        def nfd_ready():
            if not (scratch / "run/nfd.sock").is_socket():
                return False
            try:
                return subprocess.run(["nfdc", "status"], env=dict(environment, HOME=str(homes["nfd"])),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2).returncode == 0
            except subprocess.TimeoutExpired:
                return False
        supervisor.wait_until(nfd_ready, 20, "NFD_READINESS_TIMEOUT")
        def start(name):
            return supervisor.start(name, commands[name], dict(environment, HOME=str(homes[name])))
        controller = start("controller")
        supervisor.wait_marker(controller, "SPEC180_RUNTIME_CATALOGUE_PUBLISHED", 60)
        repo = start("repo")
        supervisor.wait_marker(repo, "Installed provider permission", 60)
        providers = [start("provider-" + role) for role in ROLES]
        for provider in providers:
            supervisor.wait_marker(provider, "NDNSF_DI_NATIVE_PROVIDER_READY provider=", 60)
        supervisor.wait_until(lambda: (evidence / "runtime-publication-receipt.json").is_file(),
                              60, "REPO_PUBLICATION_RECEIPT_TIMEOUT")
        settle_until = time.monotonic() + 5
        supervisor.wait_until(lambda: time.monotonic() >= settle_until, 6, "SVS_SETTLE_TIMEOUT")
        supervisor.wait_user(start("user"), 120)
    except Exception as exc:
        # Never serialize arbitrary child stderr, command arguments, or secrets.
        failure = str(exc) if isinstance(exc, SupervisionError) else type(exc).__name__
    finally:
        # A second scheduler/terminal signal must not interrupt owned teardown.
        previous_handlers = {sig: signal.signal(sig, signal.SIG_IGN)
                             for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            cleanup = supervisor.cleanup()
        finally:
            for sig, handler in previous_handlers.items():
                signal.signal(sig, handler)
        (evidence / "supervision.json").write_text(json.dumps({
            "schema": "spec180-supervision-v1", "candidateId": candidate_id,
            "candidateDigest": digest, **cleanup}, sort_keys=True) + "\n")
    if not failure:
        try:
            collector_path = HERE / "collect_spec180_result.py"
            collector_spec = importlib.util.spec_from_file_location(
                "spec180_terminal_collector", collector_path)
            collector = importlib.util.module_from_spec(collector_spec)
            collector_spec.loader.exec_module(collector)
            collector.collect_terminal_result(evidence, candidate_id, digest,
                                               json.loads((evidence / "supervision.json").read_text()))
        except Exception as exc:
            failure = str(exc) if isinstance(exc, SupervisionError) else type(exc).__name__
    if not failure:
        try:
            validate_terminal(evidence, digest, candidate_id, cleanup)
        except Exception as exc:
            failure = str(exc) if isinstance(exc, SupervisionError) else type(exc).__name__
    status = "FAILED" if failure else "PASS"
    (evidence / "orchestration-terminal.json").write_text(json.dumps({
        "schema": "spec180-orchestration-terminal-v1", "candidateId": candidate_id,
        "candidateDigest": digest, "status": status, "reason": failure,
        "exitCode": 8 if failure else 0}, sort_keys=True) + "\n")
    return 8 if failure else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("scratch", "evidence", "nfd-config", "controller-args", "repo-args", "provider-args-dir", "user-args"):
        parser.add_argument("--" + name, type=Path, required=True)
    def interrupted(signum, _frame):
        raise SupervisionError("SUPERVISOR_INTERRUPTED:" + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        return execute(parser.parse_args())
    except Exception as exc:
        print(str(exc) if isinstance(exc, SupervisionError) else type(exc).__name__)
        return 8


if __name__ == "__main__":
    raise SystemExit(main())
