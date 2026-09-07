"""Shared launch, identity boundaries, and evidence rules for Tiger applications.

Only the coordinator creates a run directory. Each worker owns its child groups;
containers receive one role HOME, public configuration, and that role's output.
"""
from __future__ import annotations

import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import time


PROFILE_FIELDS = {
    "schema", "profileId", "nodes", "cpusPerNode", "memory", "wallTime",
    "partition", "account", "apptainer", "apptainerVersion", "sif", "sifSha256",
    "tcpPort", "startupSeconds", "requestTimeoutMs", "ackTimeoutMs",
}
ROLE_RANK = {"raw0": 0, "raw1": 1, "controller": 0, "provider": 1,
             "user": 0, "denied": 0, "nfd0": 0, "nfd1": 1}
BIN = "/opt/ndnsf-di/current/bin"
PYTHON = "/opt/venv/bin/python"


def container_env() -> dict:
    """Discard host runtime injection; job configuration comes from argv only."""
    return {key: value for key, value in os.environ.items()
            if not key.startswith(("APPTAINER", "SINGULARITY", "NDN", "PYTHON", "LD_",
                                   "CUDA", "NVIDIA", "ORT_"))}


def digest(path: Path) -> str:
    """Hash file bytes without loading a SIF into RAM."""
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json(path: Path, value: dict) -> None:
    """Atomically publish a complete record on the shared filesystem."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def load_profile(path: Path) -> dict:
    """Reject unconsumed options before creating a job or run directory."""
    value = json.loads(Path(path).read_text())
    if not isinstance(value, dict) or set(value) != PROFILE_FIELDS:
        raise ValueError("PROFILE_FIELDS")
    if value["schema"] != "tiger-two-node-v1" or value["nodes"] != 2:
        raise ValueError("PROFILE_SCHEMA_OR_NODES")
    for key, low, high in [("cpusPerNode", 2, 8), ("tcpPort", 1024, 64000),
                           ("startupSeconds", 10, 120), ("requestTimeoutMs", 1000, 60000),
                           ("ackTimeoutMs", 100, 5000)]:
        if type(value[key]) is not int or not low <= value[key] <= high:
            raise ValueError("PROFILE_RANGE:" + key)
    if value["ackTimeoutMs"] >= value["requestTimeoutMs"]:
        raise ValueError("ACK_DEADLINE_ORDER")
    if not re.fullmatch(r"[0-9a-f]{64}", value["sifSha256"]):
        raise ValueError("SIF_DIGEST")
    for key in ("sif", "apptainer"):
        if not Path(value[key]).is_absolute() or any(c in value[key] for c in ":\n\r,"):
            raise ValueError("PROFILE_PATH:" + key)
    for key in ("partition", "account", "profileId"):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", value[key]):
            raise ValueError("PROFILE_IDENTIFIER:" + key)
    if not re.fullmatch(r"[1-9][0-9]*G", value["memory"]):
        raise ValueError("MEMORY")
    if not re.fullmatch(r"00:[0-5][0-9]:[0-5][0-9]", value["wallTime"]):
        raise ValueError("WALL_TIME")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?", value["apptainerVersion"]):
        raise ValueError("APPTAINER_VERSION")
    return value


def bundle_files(root: Path) -> dict:
    """Bind every executable input, including both application consumers."""
    paths = []
    for folder in ("runtime", "apps", "jobs/baseline"):
        paths.extend(p for p in (root / folder).rglob("*")
                     if p.is_file() and p.suffix in (".py", ".sbatch"))
    if not paths:
        raise ValueError("EMPTY_BUNDLE")
    return {str(p.relative_to(root)): digest(p) for p in sorted(paths)}


def container_command(profile: dict, bundle: Path, home: Path, public: Path,
                      output: Path, argv: list[str], *, node: Path | None = None,
                      prepare: Path | None = None, artifacts: Path | None = None,
                      preparation_inputs: Path | None = None,
                      gpu: bool = False, gpu_device: str | None = None) -> list[str]:
    """Compose a role-isolated command; HOME paths match the PIB's TPM locator.

    ``prepare`` is used only by the offline identity issuer. Normal workloads
    never receive this mount or the root's private key directory.
    ``artifacts`` mounts an immutable model package at /artifacts. GPU workers
    must provide one scheduler-assigned device explicitly; this helper checks
    syntax only, not allocation membership or actual CUDA execution. Those are
    worker/preflight and result-validation responsibilities.
    """
    if (type(gpu) is not bool or (not gpu and gpu_device is not None)
            or (gpu and (not isinstance(gpu_device, str) or not re.fullmatch(
                r"(?:0|[1-9][0-9]*|GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})",
                gpu_device)))):
        raise ValueError("GPU_DEVICE")
    if preparation_inputs is not None and (prepare is None or gpu or node is not None):
        raise ValueError('PREPARATION_INPUT_SCOPE')
    for path in (bundle, home, public, output, node, prepare, artifacts, preparation_inputs):
        if path is None:
            continue
        if (not Path(path).is_absolute() or ".." in Path(path).parts
                or any(c in str(path) for c in ":,\n\r\x00")):
            raise ValueError("BIND_PATH")
    role_home = "/identities/" + home.name
    command = [profile["apptainer"], "exec", "--cleanenv", "--containall",
               "--home", f"{home}:{role_home}", "--pwd", "/bundle",
               "--bind", f"{bundle}:/bundle:ro", "--bind", f"{public}:/config:{'rw' if prepare else 'ro'}",
               "--bind", f"{output}:/output:rw"]
    if gpu:
        command += ["--nv"]
    if artifacts is not None:
        command += ["--bind", f"{artifacts}:/artifacts:ro"]
    if node is not None:
        command += ["--bind", f"{node}:/node:rw"]
    if prepare is not None:
        command += ["--bind", f"{prepare}:/identities:rw"]
    if preparation_inputs is not None:
        command += ["--bind", f"{preparation_inputs}:/inputs:ro"]
    command += [profile["sif"], "/usr/bin/env",
                f"PATH={BIN}:/opt/venv/bin:/usr/bin:/bin",
                "LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib",
                "PYTHONNOUSERSITE=1", "PYTHONPATH=/bundle",
                "NDN_CLIENT_TRANSPORT=unix:///node/nfd.sock",
                "NDNSF_CONFIG=" + role_home + "/session.conf",
                "NDNSF_CONTROLLER_CERT_FILE=/config/controller.cert",
                "NDN_LOG=ndn_service_framework.*=ERROR"]
    if gpu:
        command += ["CUDA_VISIBLE_DEVICES=" + gpu_device]
    command += argv
    return command


class Processes:
    """Own child process groups and their logs until close has reaped them."""
    def __init__(self, log_dir: Path):
        self.log_dir = log_dir
        log_dir.mkdir(parents=True, exist_ok=True)
        self.children = []

    def start(self, name: str, argv: list[str], env: dict | None = None, *,
              cwd: Path | None = None, log_path: Path | None = None):
        log = (Path(log_path) if log_path is not None else self.log_dir / (name + ".log")).open("wb")
        try:
            child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
                                     start_new_session=True, env=env, cwd=cwd)
        except BaseException:
            log.close()
            raise
        self.children.append((name, child, log))
        return child

    def check(self):
        for name, child, _ in self.children:
            if child.poll() is not None:
                raise RuntimeError(f"CHILD_EXIT:{name}:{child.returncode}")

    def close(self, *, seconds: float = 30) -> list[dict]:
        """Stop owned groups within one budget, retaining unreaped owners.

        Stop services in reverse startup order. All groups get a stop/kill
        attempt even if an earlier group uses the grace budget or raises an
        OS error. Reserve part of the *same* deadline for reaping; never grant
        each child a new cleanup budget. A stuck kernel process is reported
        unreaped, not forgotten or represented as clean shutdown.
        """
        if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
                or not math.isfinite(seconds) or seconds <= 0):
            raise ValueError("CLEANUP_BUDGET")
        deadline = time.monotonic() + seconds
        grace_deadline = deadline - min(1.0, seconds / 2)
        owned = list(reversed(self.children))
        result = [{"name": name, "pid": child.pid,
                   "exitedBeforeCleanup": child.poll() is not None, "forced": False}
                  for name, child, _ in owned]
        for (_, child, _), row in zip(owned, result):
            try:
                # Let Apptainer forward TERM to its application before it
                # unmounts FUSE. TERM of the whole group can tear down the
                # mapped executable underneath the still-running application.
                child.terminate()
            except ProcessLookupError:
                pass
            except OSError as exc:
                row["cleanupError"] = type(exc).__name__
                row["forced"] = True
            try:
                child.wait(timeout=min(5, max(0, grace_deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                row["forced"] = True
            # A child may exit while a grandchild survives in its group.
            try:
                os.killpg(child.pid, 0)
                os.killpg(child.pid, signal.SIGKILL)
                row["forced"] = True
            except ProcessLookupError:
                pass
            except OSError as exc:
                row["cleanupError"] = type(exc).__name__
                row["forced"] = True
        remaining = []
        for (name, child, log), row in zip(owned, result):
            try:
                child.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
            exit_code = child.poll()
            row.update(exitCode=exit_code, reaped=exit_code is not None)
            if row["reaped"]:
                log.close()
            else:
                row["cleanupTimedOut"] = True
                remaining.append((name, child, log))
        self.children = list(reversed(remaining))
        return result


def wait_json(path: Path, seconds: int, children: Processes | None = None,
              failure_dir: Path | None = None) -> dict:
    """Wait for evidence, failing on child death or a peer's terminal failure."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if children:
            children.check()
        if failure_dir and list(failure_dir.glob("failed-*.json")):
            raise RuntimeError("PEER_FAILED")
        if path.exists():
            return json.loads(path.read_text())
        time.sleep(0.1)
    raise TimeoutError("BARRIER_TIMEOUT:" + path.name)


def nfd_config(port: int) -> str:
    """Job-scoped TCP forwarder; app trust is enforced separately by probes."""
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError("NFD_PORT")
    return f'''log
{{
  default_level WARN
}}
tables
{{
  cs_max_packets 0
}}
face_system
{{
  unix
  {{
    path /node/nfd.sock
  }}
  tcp
  {{
    listen yes
    port {port}
    enable_v4 yes
    enable_v6 no
  }}
  udp
  {{
    listen no
    mcast no
  }}
  ether
  {{
    listen no
    mcast no
  }}
}}
authorizations
{{
  authorize
  {{
    certfile any
    privileges
    {{
      faces
      fib
      strategy-choice
    }}
  }}
}}
rib
{{
  localhost_security
  {{
    trust-anchor
    {{
      type any
    }}
  }}
}}
'''


def route_commands(namespace: str, remote: str, port: int, *, sync_prefix: str):
    """Shared nfdc arguments with an explicit application Sync name."""
    from runtime.identities import identity_inventory
    identity_inventory(namespace, {'sync': sync_prefix})
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError('NFD_PORT')
    ipaddress.IPv4Address(remote)
    uri = f"tcp4://{remote}:{port}"
    return [["face", "create", "remote", uri, "persistency", "permanent"],
             ["route", "add", "prefix", namespace, "nexthop", uri, "cost", "10"],
             ["strategy", "set", "prefix", sync_prefix,
              "strategy", "/localhost/nfd/strategy/multicast"],
             ["face", "list"], ["route", "list"]]


def configure_routes(command, namespace: str, remote: str, port: int,
                     output: Path, *, sync_prefix: str | None = None) -> None:
    """Use in-image nfdc; keep the old baseline's group unless explicitly supplied."""
    calls = route_commands(namespace, remote, port,
                           sync_prefix=sync_prefix if sync_prefix is not None else namespace + '/group')
    records = []
    for args in calls:
        completed = subprocess.run(command([BIN + "/nfdc", *args]), capture_output=True, env=container_env(),
                                   text=True, timeout=20)
        records.append({"arguments": args, "exitCode": completed.returncode,
                        "stdout": completed.stdout, "stderr": completed.stderr})
        write_json(output, {"commands": records})
        if completed.returncode:
            raise RuntimeError("ROUTE_COMMAND_FAILED")


def native_trust_rejection(exit_code: int, log: str, observation: dict) -> dict:
    """Classify only a proved native trust rejection, not an arbitrary crash.

    r119's NAC-ABE constructor aborts on failed PUBPARAMS authentication. This
    exact negative is isolated from the live services and recorded as such.
    """
    if exit_code in (-6, 134) and "Fetched public parameters cannot be authenticated:" in log:
        return {"status": "PASS", "nativeValidationFailure": True, "boundary": "PUBPARAMS",
                "exitCode": exit_code, "termination": "EXPECTED_AUTHENTICATION_ABORT"}
    if (exit_code == 0 and observation.get("allowed") == [] and
            "PermissionResponse Data validation failed:" in log):
        return {"status": "PASS", "nativeValidationFailure": True, "boundary": "PERMISSION",
                "exitCode": 0, "termination": "NORMAL"}
    raise RuntimeError("NO_NATIVE_TRUST_REJECTION")


def collect(run: dict, nodes: list[dict], worker_exit: int) -> dict:
    """Accept exact case/identity evidence only after all workers are reaped."""
    required = {"raw-roundtrip", "bad-signature", "wrong-root"}
    problems = []
    if worker_exit != 0 or len(nodes) != 2:
        problems.append("WORKER_EXIT_OR_COUNT")
    if {n.get("rank") for n in nodes} != {0, 1}:
        problems.append("NODE_RANKS")
    if run["mode"] == "slurm" and len({n.get("hostname") for n in nodes}) != 2:
        problems.append("NOT_TWO_PHYSICAL_NODES")
    if not run.get("privateRemoved"):
        problems.append("PRIVATE_STATE_REMAINS")
    for node in nodes:
        if node.get("status") != "PASS":
            problems.append("NODE_FAILED")
        if not node.get("runtime", {}).get("native"):
            problems.append("RUNTIME_IDENTITY_MISSING")
        for field in ("runId", "sifSha256", "bundleFiles", "profileSha256"):
            if node.get(field) != run.get(field):
                problems.append("IDENTITY:" + field)
        cases = node.get("cases", {})
        expected = required | ({"service-echo", "permission-rejection", "native-trust-rejection"}
                               if node.get("rank") == 0 else set())
        if set(cases) != expected or any(v.get("status") != "PASS" for v in cases.values()):
            problems.append("CASE_SET_OR_RESULT")
        raw = cases.get("raw-roundtrip", {})
        peer = "raw" + str(1 - node.get("rank", -1))
        if raw.get("payload") != "DATA:" + peer + ":" + run["runId"]:
            problems.append("RAW_PAYLOAD")
        if any(cases.get(key, {}).get("reason") != "ValidationFailure"
               for key in ("bad-signature", "wrong-root")):
            problems.append("SIGNATURE_NEGATIVE_ORACLE")
        if node.get("rank") == 0:
            service = cases.get("service-echo", {})
            denial = cases.get("permission-rejection", {})
            request_id = service.get("requestId")
            provider = run.get("namespace", "") + "/provider"
            acks = service.get("acks")
            valid_acks = isinstance(acks, list) and bool(acks) and all(
                isinstance(ack, dict) and ack.get("status") is True and
                ack.get("provider") == provider and ack.get("service") == "/TIGER_ECHO" and
                ack.get("requestId") == request_id for ack in acks)
            if (service.get("payload") != "ECHO:" + run["payload"] or
                    not isinstance(request_id, str) or request_id in ("", "/") or
                    not valid_acks or service.get("selectedProviders") != [provider]):
                problems.append("SERVICE_PAYLOAD_OR_ACK")
            if not denial.get("nativeRejection") or denial.get("providerCalls") != [run["payload"]]:
                problems.append("PERMISSION_NEGATIVE_ORACLE")
            rejection = cases.get("native-trust-rejection", {})
            boundary = rejection.get("boundary")
            exit_code = rejection.get("exitCode")
            termination = rejection.get("termination")
            valid_rejection = type(exit_code) is int and (
                (boundary == "PUBPARAMS" and exit_code in (-6, 134) and
                 termination == "EXPECTED_AUTHENTICATION_ABORT") or
                (boundary == "PERMISSION" and exit_code == 0 and termination == "NORMAL"))
            if rejection.get("nativeValidationFailure") is not True or not valid_rejection:
                problems.append("NATIVE_TRUST_NEGATIVE_ORACLE")
        if not node.get("cleanup") or any(not row.get("reaped") or row.get("forced") or
                row.get("exitedBeforeCleanup") for row in node["cleanup"]):
            problems.append("CLEANUP")
    return {**run, "status": "FAIL" if problems else
            ("PASS" if run["mode"] == "slurm" else "LOCAL_PASS"),
            "nodes": nodes, "workerExit": worker_exit, "problems": problems}
