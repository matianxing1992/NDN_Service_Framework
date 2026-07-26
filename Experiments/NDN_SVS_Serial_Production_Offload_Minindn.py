#!/usr/bin/env python3
"""Spec 137 two-peer MiniNDN runner and immutable receipt authority."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[1]
EXPERIMENTS = Path(__file__).resolve().parent
if str(EXPERIMENTS) not in sys.path:
    sys.path.insert(0, str(EXPERIMENTS))
import analyze_svs_serial_production_offload as analysis  # noqa: E402
import build_svs_serial_production_offload as subject_builder  # noqa: E402


DEFAULT_SUBJECT = REPO / "build/spec137-four-core/source-manifest.json"
RATES = (60,)
PEERS = ("peer-a", "peer-b")
MODES = ("face-serial", "worker-serial")
PILOT_TIMING = (5, 15, 5)
FORMAL_TIMING = (10, 60, 10)
SMOKE_TIMING = (1, 3, 2)
TOPOLOGY_TEXT = (
    "[nodes]\npeer-a:\npeer-b:\n\n[links]\n"
    "peer-a:peer-b delay=10ms bw=100 loss=0\n"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot load JSON authority {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON authority is not an object: {path}")
    return value


class CampaignLock:
    """Nonblocking process-level single-writer lock."""

    def __init__(self, campaign: Path):
        self.path = campaign / ".campaign.lock"
        self.handle: Any = None

    def __enter__(self) -> "CampaignLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = self.path.open("a+", encoding="utf-8")
        try:
            fcntl.flock(self.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            self.handle.close()
            self.handle = None
            raise RuntimeError(f"campaign already has a writer: {self.path}") from error
        self.handle.seek(0)
        self.handle.truncate()
        self.handle.write(
            json.dumps(
                {
                    "schema": "spec137.single-writer.v1",
                    "pid": os.getpid(),
                    "host": os.uname().nodename,
                    "startedUnixNs": time.time_ns(),
                },
                sort_keys=True,
            )
            + "\n"
        )
        self.handle.flush()
        os.fsync(self.handle.fileno())
        return self

    def __exit__(self, *_: Any) -> None:
        if self.handle is not None:
            fcntl.flock(self.handle, fcntl.LOCK_UN)
            self.handle.close()
            self.handle = None


class ReceiptLedger:
    """Append-only one-file-per-ordinal formal receipt ledger."""

    def __init__(self, root: Path):
        self.root = root

    def append(self, receipt: dict[str, Any]) -> Path:
        if receipt.get("schema") != "spec137.receipt.v1":
            raise RuntimeError("terminal receipt schema mismatch")
        ordinal = receipt.get("ordinal")
        if not isinstance(ordinal, int) or ordinal not in range(1, 7):
            raise RuntimeError(f"invalid formal receipt ordinal: {ordinal}")
        if receipt.get("retryCount") != 0:
            raise RuntimeError("Spec 137 forbids formal retries")
        self.root.mkdir(parents=True, exist_ok=True)
        path = self.root / f"{ordinal:02d}.json"
        try:
            with path.open("x", encoding="utf-8") as output:
                output.write(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
                output.flush()
                os.fsync(output.fileno())
        except FileExistsError as error:
            raise RuntimeError(
                f"formal ordinal {ordinal} already has a receipt: {path}"
            ) from error
        return path


def formal_cells(rate: int) -> list[dict[str, Any]]:
    if rate not in RATES:
        raise RuntimeError(f"frozen formal rate is not registered: {rate}")
    order = (
        (1, 1, "face-serial"),
        (2, 1, "worker-serial"),
        (3, 2, "worker-serial"),
        (4, 2, "face-serial"),
        (5, 3, "face-serial"),
        (6, 3, "worker-serial"),
    )
    warmup, measure, drain = FORMAL_TIMING
    return [
        {
            "ordinal": ordinal,
            "pair": pair,
            "mode": mode,
            "rate": rate,
            "warmup": warmup,
            "measure": measure,
            "drain": drain,
            "retryCount": 0,
            "cellId": f"{ordinal:02d}-pair-{pair}-{mode}",
        }
        for ordinal, pair, mode in order
    ]


def choose_cpu_map() -> dict[str, Any]:
    allowed = sorted(os.sched_getaffinity(0))
    if len(allowed) < 4:
        raise RuntimeError(
            f"Spec 137 requires the declared four-CPU layout, got {allowed}"
        )
    # The experiment host has four vCPUs. Both NFDs share CPU 0. The sole
    # publisher's pacer/Face share CPU 1, the fixed receiver uses CPU 2, and
    # the publisher's one treatment worker uses CPU 3. The worker is absent in
    # face-serial; there is never a second active production worker.
    selected = allowed[:4]
    peer_a = {"main": selected[1], "face": selected[1], "worker": selected[3]}
    peer_b = {"main": selected[2], "face": selected[2], "worker": selected[3]}
    return {
        "schema": "spec137.cpu-map.v1",
        "allowed": selected,
        "peer-a": peer_a,
        "peer-b": peer_b,
        "fourCoreControlledPlacement": True,
        "sharing": {
            "nfd": "both NFDs share CPU 0",
            "producer": "peer-a pacer and Face share CPU 1",
            "receiver": "peer-b fixed receiver uses CPU 2",
            "worker": "peer-a's sole treatment worker uses CPU 3",
        },
        "nfd": {
            "peer-a": selected[0],
            "peer-b": selected[0],
        },
    }


def self_test_configs(subject: dict[str, Any]) -> dict[str, dict[str, Any]]:
    binary = Path(subject["binary"])
    environment = {
        **os.environ,
        "LD_LIBRARY_PATH": str(Path(subject["library"]).parent),
    }
    configs: dict[str, dict[str, Any]] = {}
    for mode in MODES:
        completed = subprocess.run(
            [str(binary), "--self-test", mode],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"runtime configuration self-test failed for {mode}: "
                f"{completed.stdout}"
            )
        try:
            configs[mode] = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"runtime configuration self-test is not JSON for {mode}"
            ) from error
    delta = analysis.runtime_config_delta(
        configs["face-serial"], configs["worker-serial"]
    )
    if delta != analysis.ALLOWED_TREATMENT_FIELDS:
        raise RuntimeError(f"runtime treatment delta escaped contract: {delta}")
    return configs


def no_op_pacer(rate: int = 1000, seconds: float = 2.0) -> dict[str, Any]:
    if rate <= 0 or seconds <= 0:
        raise RuntimeError("no-op pacer rate and duration must be positive")
    expected = int(rate * seconds)
    period_ns = 1_000_000_000 // rate
    start = time.perf_counter_ns() + 100_000_000
    attempted = 0
    for index in range(expected):
        deadline = start + index * period_ns
        while True:
            remaining = deadline - time.perf_counter_ns()
            if remaining <= 0:
                break
            if remaining > 100_000:
                time.sleep((remaining - 50_000) / 1_000_000_000)
        attempted += 1
    elapsed = max(1, time.perf_counter_ns() - start)
    achieved = attempted / (elapsed / 1_000_000_000)
    error = abs(achieved - rate) / rate
    return {
        "schema": "spec137.noop-pacer.v1",
        "targetPps": rate,
        "durationSeconds": seconds,
        "attempted": attempted,
        "achievedPps": achieved,
        "relativeError": error,
        "withinTwoPercent": error <= 0.02,
    }


def host_command(host: Any, command: str) -> tuple[int, str]:
    marker = "__SPEC137_RC__="
    text = host.cmd(
        f"{command}; spec137_rc=$?; "
        f"printf '\\n{marker}%s\\n' \"$spec137_rc\""
    )
    match = re.search(rf"\n{marker}(\d+)\s*$", text)
    if match is None:
        raise RuntimeError(f"missing command status marker: {command}")
    return int(match.group(1)), text[: match.start()].strip()


def install_routes(
    host: Any,
    transport: str,
    remote_ip: str,
    prefixes: tuple[str, str],
    evidence: Path,
) -> None:
    env = f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)}"
    rc, output = host_command(
        host,
        f"{env} nfdc face create udp4://{remote_ip}:6363 "
        "persistency persistent",
    )
    match = re.search(r"face-created id=(\d+)", output)
    if rc != 0 or match is None:
        raise RuntimeError(f"cannot create peer face: rc={rc} output={output}")
    face_id = int(match.group(1))
    routes: list[dict[str, Any]] = []
    for prefix in prefixes:
        route_rc, route_output = host_command(
            host,
            f"{env} nfdc route add {shlex.quote(prefix)} nexthop {face_id}",
        )
        routes.append(
            {
                "prefix": prefix,
                "faceId": face_id,
                "returnCode": route_rc,
                "output": route_output,
            }
        )
        if route_rc != 0 or "route-add-accepted" not in route_output:
            raise RuntimeError(
                f"route installation failed for {prefix}: {route_output}"
            )
    strategy_rc, strategy_output = host_command(
        host,
        f"{env} nfdc strategy set {shlex.quote(prefixes[0])} "
        "/localhost/nfd/strategy/multicast",
    )
    rib_rc, rib = host_command(host, f"{env} nfdc route list")
    missing = [prefix for prefix in prefixes if prefix not in rib]
    atomic_json(
        evidence,
        {
            "schema": "spec137.routes.v1",
            "faceId": face_id,
            "routes": routes,
            "strategy": {
                "returnCode": strategy_rc,
                "output": strategy_output,
            },
            "ribReturnCode": rib_rc,
            "missingPrefixes": missing,
        },
    )
    if strategy_rc != 0 or rib_rc != 0 or missing:
        raise RuntimeError(
            f"route verification failed: missing={missing} "
            f"strategyRc={strategy_rc} ribRc={rib_rc}"
        )


def wait_ready(path: Path, process: Any, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and "SPEC137_READY" in path.read_text(
            encoding="utf-8", errors="replace"
        ):
            return
        if process.poll() is not None:
            raise RuntimeError(
                f"peer exited before ready: rc={process.returncode} "
                f"log={path}"
            )
        time.sleep(0.1)
    raise RuntimeError(f"peer ready timeout: {path}")


def stop_process(process: Any, grace: float = 3.0) -> int | None:
    if process is None:
        return None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=grace)
    return process.returncode


def clear_stale_named_sockets() -> list[str]:
    active = Path("/proc/net/unix").read_text(
        encoding="utf-8", errors="replace"
    )
    removed = []
    for peer in PEERS:
        socket = Path(f"/run/nfd/{peer}.sock")
        if not socket.exists():
            continue
        if str(socket) in active:
            raise RuntimeError(f"named MiniNDN socket is actively owned: {socket}")
        socket.unlink()
        removed.append(str(socket))
    return removed


def sample_processes(processes: dict[str, Any]) -> dict[str, Any]:
    sample: dict[str, Any] = {
        "schema": "spec137.resource-sample.v1",
        "monotonicNs": time.monotonic_ns(),
        "processes": {},
    }
    for peer, wrapper in processes.items():
        descendants = [wrapper.pid]
        cursor = 0
        while cursor < len(descendants):
            children = Path(
                f"/proc/{descendants[cursor]}/task/"
                f"{descendants[cursor]}/children"
            )
            if children.is_file():
                descendants.extend(
                    int(value)
                    for value in children.read_text().split()
                    if int(value) not in descendants
                )
            cursor += 1
        resolved = wrapper.pid
        for candidate in descendants:
            cmdline = Path(f"/proc/{candidate}/cmdline")
            if cmdline.is_file() and "svs-serial-production-offload" in (
                cmdline.read_bytes().replace(b"\0", b" ").decode(errors="replace")
            ):
                resolved = candidate
        entry: dict[str, Any] = {
            "wrapperPid": wrapper.pid,
            "pid": resolved,
        }
        stat = Path(f"/proc/{resolved}/stat")
        status = Path(f"/proc/{resolved}/status")
        if stat.is_file():
            fields = stat.read_text().split()
            entry["cpuTicks"] = int(fields[13]) + int(fields[14])
        if status.is_file():
            for line in status.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines():
                if line.startswith(("VmRSS:", "Threads:", "Cpus_allowed_list:")):
                    key, value = line.split(":", 1)
                    entry[key] = value.strip()
        sample["processes"][peer] = entry
    return sample


def run_cell(
    campaign: Path,
    subject: dict[str, Any],
    cpu_map: dict[str, Any],
    config: dict[str, Any],
    *,
    namespace: str,
) -> dict[str, Any]:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    cell = campaign / namespace / config["cellId"]
    cell.mkdir(parents=True, exist_ok=False)
    atomic_json(cell / "cell-config.json", config)
    topology = cell / "topology.conf"
    topology.write_text(TOPOLOGY_TEXT, encoding="utf-8")
    sync_prefix = f"/spec137/sync/{config['cellId']}"
    node_prefixes = {
        peer: f"/spec137/{peer}/{config['cellId']}" for peer in PEERS
    }
    ndn = None
    processes: dict[str, Any] = {}
    captures: dict[str, Any] = {}
    samples: list[dict[str, Any]] = []
    error = ""
    infrastructure = True
    started = time.time_ns()
    try:
        setLogLevel("warning")
        Minindn.verifyDependencies()
        stale_sockets_removed = clear_stale_named_sockets()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell / "minindn"))
        ndn.start()
        nfd_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        nfd_affinity: dict[str, list[int]] = {}
        for app in nfd_manager:
            cpu = int(cpu_map["nfd"][app.node.name])
            os.sched_setaffinity(app.process.pid, {cpu})
            actual = sorted(os.sched_getaffinity(app.process.pid))
            if actual != [cpu]:
                raise RuntimeError(
                    f"NFD affinity mismatch for {app.node.name}: {actual}"
                )
            nfd_affinity[app.node.name] = actual
        hosts = {peer: ndn.net[peer] for peer in PEERS}
        transports = {peer: f"unix:///run/nfd/{peer}.sock" for peer in PEERS}
        for peer, host in hosts.items():
            socket = Path(f"/run/nfd/{peer}.sock")
            deadline = time.monotonic() + 15
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")
            other = PEERS[1] if peer == PEERS[0] else PEERS[0]
            install_routes(
                host,
                transports[peer],
                hosts[other].IP(),
                (sync_prefix, node_prefixes[other]),
                cell / f"{peer}-routes.json",
            )
        atomic_json(
            cell / "environment.json",
            {
                "schema": "spec137.environment.v1",
                "cpuMap": cpu_map,
                "transports": transports,
                "syncPrefix": sync_prefix,
                "nodePrefixes": node_prefixes,
                "twoProcesses": True,
                "ndnsfRuntime": False,
                "nfdAffinity": nfd_affinity,
                "staleSocketsRemoved": stale_sockets_removed,
                "topologySha256": sha256_file(topology),
            },
        )
        for peer, host in hosts.items():
            capture_path = cell / f"{peer}-ndndump.log"
            capture_command = (
                f"ndndump -i {shlex.quote(host.defaultIntf().name)} -t -v "
                f">{shlex.quote(str(capture_path))} 2>&1"
            )
            captures[peer] = host.popen(capture_command, shell=True)
        commands: dict[str, str] = {}
        other_peer = {"peer-a": "peer-b", "peer-b": "peer-a"}
        for peer in PEERS:
            cpus = cpu_map[peer]
            is_producer = peer == "peer-a"
            process_mode = config["mode"] if is_producer else "face-serial"
            ndn_log = (
                "ndn_svs.SyncTimeline=TRACE:ndn_svs.SVSPubSub=TRACE"
                if config.get("diagnostics", "enabled") == "enabled"
                else "*=WARN"
            )
            command = (
                f"env NDN_CLIENT_TRANSPORT={shlex.quote(transports[peer])} "
                f"LD_LIBRARY_PATH={shlex.quote(str(Path(subject['library']).parent))} "
                f"NDN_LOG={shlex.quote(ndn_log)} "
                f"{shlex.quote(subject['binary'])} "
                f"--production-mode {process_mode} "
                f"--publish-enabled {'true' if is_producer else 'false'} "
                f"--sync-prefix {shlex.quote(sync_prefix)} "
                f"--node-prefix {shlex.quote(node_prefixes[peer])} "
                f"--campaign-id {shlex.quote(config['campaignId'])} "
                f"--cell-id {shlex.quote(config['cellId'])} "
                f"--peer-id {peer} --remote-peer-id {other_peer[peer]} "
                f"--rate {config['rate']} --warmup {config['warmup']} "
                f"--measure {config['measure']} --drain {config['drain']} "
                f"--events {shlex.quote(str(cell / (peer + '-events.jsonl')))} "
                f"--resources {shlex.quote(str(cell / (peer + '-resources.json')))} "
                f"--main-cpu {cpus['main']} --face-cpu {cpus['face']} "
                f"--worker-cpu {cpus['worker']} "
                f"--diagnostics {config.get('diagnostics', 'enabled')} "
                f">{shlex.quote(str(cell / (peer + '.stdout')))} "
                f"2>{shlex.quote(str(cell / (peer + '.stderr')))}"
            )
            commands[peer] = command
        atomic_json(cell / "commands.json", commands)
        infrastructure = False
        for peer in PEERS:
            processes[peer] = hosts[peer].popen(commands[peer], shell=True)
        for peer in PEERS:
            wait_ready(cell / f"{peer}.stdout", processes[peer])
        deadline = (
            time.monotonic()
            + config["warmup"]
            + config["measure"]
            + config["drain"]
            + 35
        )
        while (
            time.monotonic() < deadline
            and any(process.poll() is None for process in processes.values())
        ):
            samples.append(sample_processes(processes))
            time.sleep(0.1)
        if any(process.poll() is None for process in processes.values()):
            raise RuntimeError("bounded cell watchdog expired")
        if any(process.returncode != 0 for process in processes.values()):
            raise RuntimeError(
                f"peer return codes: "
                f"{[(peer, process.returncode) for peer, process in processes.items()]}"
            )
    except Exception as runtime_error:
        error = f"{type(runtime_error).__name__}: {runtime_error}"
    finally:
        return_codes = {
            peer: stop_process(process) for peer, process in processes.items()
        }
        capture_return_codes = {
            peer: stop_process(process) for peer, process in captures.items()
        }
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as stop_error:
                error = error or f"ndn.stop: {stop_error}"
        sys.argv = original_argv
    with (cell / "resource-samples.jsonl").open("w", encoding="utf-8") as output:
        for sample in samples:
            output.write(json.dumps(sample, sort_keys=True) + "\n")
    terminal = {
        "schema": "spec137.cell-terminal.v1",
        "cellId": config["cellId"],
        "startedUnixNs": started,
        "endedUnixNs": time.time_ns(),
        "returnCodes": return_codes,
        "captureReturnCodes": capture_return_codes,
        "status": (
            "complete"
            if not error
            else "infrastructure-invalid"
            if infrastructure
            else "subject-failure"
        ),
        "error": error,
    }
    atomic_json(cell / "terminal.json", terminal)
    artifacts = cell_artifact_records(cell)
    atomic_json(
        cell / "artifact-hashes.json",
        {
            "schema": "spec137.cell-artifacts.v1",
            "cellId": config["cellId"],
            "records": artifacts,
            "recordsSha256": hashlib.sha256(
                json.dumps(
                    artifacts, sort_keys=True, separators=(",", ":")
                ).encode("utf-8")
            ).hexdigest(),
        },
    )
    return terminal


def cell_evidence(
    campaign: Path, namespace: str, config: dict[str, Any]
) -> dict[str, Any]:
    cell = campaign / namespace / config["cellId"]
    peers = {
        peer: analysis.parse_peer_directory(
            cell,
            config["campaignId"],
            config["cellId"],
            peer,
            config["rate"],
        )
        for peer in PEERS
    }
    validations = {
        peer: peers[peer]["validated"] for peer in PEERS
    }
    producer = peers["peer-a"]
    receiver = peers["peer-b"]
    delivered = int(receiver["summary"]["deliveredMeasured"])
    committed = int(producer["summary"].get(
        "committedMeasured", producer["summary"]["attemptedMeasured"]
    ))
    return {
        "peers": peers,
        "attemptedPps": (
            int(producer["summary"]["attemptedMeasured"])
            / int(producer["validated"]["config"]["measure_s"])
        ),
        "attemptedRateError": producer["validated"]["attemptedRateError"],
        "deliveryRatio": delivered / committed if committed else 0.0,
        "heartbeatP99Ns": int(producer["summary"]["heartbeatP99Ns"]),
        "deliveryP99Ns": int(receiver["summary"].get("deliveryP99Ns", 0)),
        "accountingComplete": all(
            value["productionAccountingRemainder"] == 0
            and value["publicationAccountingRemainder"] == 0
            for value in validations.values()
        ),
        "resourceComplete": all(
            value["resourceComplete"] for value in validations.values()
        ),
        "fallbacks": sum(value["fallbacks"] for value in validations.values()),
        "productionAccountingRemainder": sum(
            value["productionAccountingRemainder"]
            for value in validations.values()
        ),
        "publicationAccountingRemainder": sum(
            value["publicationAccountingRemainder"]
            for value in validations.values()
        ),
        "maxActiveSigners": max(
            value["maxActiveSigners"] for value in validations.values()
        ),
        "shutdownDrained": all(
            value["shutdownDrained"] for value in validations.values()
        ),
        "ownerViolations": sum(
            value["ownerViolations"] for value in validations.values()
        ),
    }


def compact_cell_evidence(value: dict[str, Any]) -> dict[str, Any]:
    return {key: item for key, item in value.items() if key != "peers"}


def artifact_record(path: Path) -> dict[str, str]:
    return {"path": str(path.resolve()), "sha256": sha256_file(path)}


def cell_artifact_records(cell: Path) -> list[dict[str, Any]]:
    records = []
    for path in sorted(item for item in cell.rglob("*") if item.is_file()):
        if path.name == "artifact-hashes.json":
            continue
        records.append(
            {
                "path": str(path.relative_to(cell)),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return records


def run_preflight(campaign: Path, subject_path: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    subject = subject_builder.load_subject(subject_path.resolve())
    preflight = campaign / "preflight"
    if preflight.exists():
        raise RuntimeError(f"preflight already exists: {preflight}")
    campaign.mkdir(parents=True, exist_ok=True)
    preflight.mkdir()
    with CampaignLock(campaign):
        cpu_map = choose_cpu_map()
        atomic_json(preflight / "cpu-map.json", cpu_map)
        configs = self_test_configs(subject)
        atomic_json(preflight / "runtime-configs.json", configs)
        pacer = no_op_pacer()
        atomic_json(preflight / "noop-pacer.json", pacer)
        smoke_results: dict[str, Any] = {}
        smoke_evidence: dict[str, Any] = {}
        campaign_id = campaign.name
        for mode in MODES:
            warmup, measure, drain = SMOKE_TIMING
            config = {
                "campaignId": campaign_id,
                "cellId": f"smoke-{mode}",
                "mode": mode,
                "rate": RATES[0],
                "warmup": warmup,
                "measure": measure,
                "drain": drain,
                "formal": False,
                "diagnostics": "enabled",
            }
            terminal = run_cell(
                campaign,
                subject,
                cpu_map,
                config,
                namespace="preflight/smoke",
            )
            smoke_results[mode] = terminal
            if terminal["status"] == "complete":
                smoke_evidence[mode] = compact_cell_evidence(
                    cell_evidence(campaign, "preflight/smoke", config)
                )
        instrumentation: dict[str, Any] = {}
        for diagnostics in ("disabled", "enabled"):
            warmup, measure, drain = (1, 5, 2)
            config = {
                "campaignId": campaign_id,
                "cellId": f"instrumentation-{diagnostics}",
                "mode": "face-serial",
                "rate": RATES[0],
                "warmup": warmup,
                "measure": measure,
                "drain": drain,
                "formal": False,
                "diagnostics": diagnostics,
            }
            terminal = run_cell(
                campaign,
                subject,
                cpu_map,
                config,
                namespace="preflight/instrumentation",
            )
            instrumentation[diagnostics] = {"terminal": terminal}
            if terminal["status"] == "complete":
                instrumentation[diagnostics]["evidence"] = compact_cell_evidence(
                    cell_evidence(
                        campaign, "preflight/instrumentation", config
                    )
                )
        if all(
            "evidence" in instrumentation[mode]
            for mode in ("disabled", "enabled")
        ):
            disabled = instrumentation["disabled"]["evidence"]
            enabled = instrumentation["enabled"]["evidence"]
            throughput_cost = max(
                0.0,
                (
                    disabled["attemptedPps"] - enabled["attemptedPps"]
                )
                / max(disabled["attemptedPps"], 1e-12),
            )
            heartbeat_cost = max(
                0.0,
                (
                    enabled["heartbeatP99Ns"] - disabled["heartbeatP99Ns"]
                )
                / max(disabled["heartbeatP99Ns"], 1),
            )
        else:
            throughput_cost = float("inf")
            heartbeat_cost = float("inf")
        instrumentation["throughputCost"] = throughput_cost
        instrumentation["heartbeatP99Cost"] = heartbeat_cost
        instrumentation["withinBudget"] = (
            throughput_cost <= 0.05
            and heartbeat_cost <= 0.10
            and all(
                instrumentation[mode].get("evidence", {}).get(
                    "attemptedRateError", float("inf")
                )
                <= 0.02
                and instrumentation[mode].get("evidence", {}).get(
                    "accountingComplete", False
                )
                and instrumentation[mode].get("evidence", {}).get(
                    "resourceComplete", False
                )
                for mode in ("disabled", "enabled")
            )
        )
        treatment_delta = analysis.runtime_config_delta(
            configs["face-serial"], configs["worker-serial"]
        )
        basic_checks = {
            "subject_hashes": True,
            "same_binary": subject["binarySha256"] == sha256_file(
                Path(subject["binary"])
            ),
            "boost_1_71_only": subject["boost"]["versionNumber"] == 107100,
            "same_runtime_controls_except_treatment": treatment_delta
            == analysis.ALLOWED_TREATMENT_FIELDS,
            "noop_attempted_rate_within_2_percent": pacer["withinTwoPercent"],
            "two_nodes_two_processes": all(
                result["status"] == "complete"
                for result in smoke_results.values()
            ),
            "routes_ready": all(
                all(
                    (
                        preflight
                        / "smoke"
                        / f"smoke-{mode}"
                        / f"{peer}-routes.json"
                    ).is_file()
                    for peer in PEERS
                )
                for mode in MODES
            ),
            "cpu_affinity_valid": cpu_map["fourCoreControlledPlacement"],
            "one_face_thread": all(
                config["face_threads"] == 1 for config in configs.values()
            ),
            "receive_workers_zero": all(
                config["receive_workers"] == 0 for config in configs.values()
            ),
            "max_active_sync_signers_one": bool(smoke_evidence)
            and all(
                value["maxActiveSigners"] == 1
                for value in smoke_evidence.values()
            ),
            "production_fallback_zero": bool(smoke_evidence)
            and all(value["fallbacks"] == 0 for value in smoke_evidence.values()),
            "production_accounting_remainder_zero": bool(smoke_evidence)
            and all(
                value["productionAccountingRemainder"] == 0
                for value in smoke_evidence.values()
            ),
            "publication_accounting_remainder_zero": bool(smoke_evidence)
            and all(
                value["publicationAccountingRemainder"] == 0
                for value in smoke_evidence.values()
            ),
            "event_and_resource_files_complete": len(smoke_evidence) == 2
            and all(value["resourceComplete"] for value in smoke_evidence.values()),
            "shutdown_drained": len(smoke_evidence) == 2
            and all(value["shutdownDrained"] for value in smoke_evidence.values()),
            "instrumentation_overhead_within_budget": instrumentation[
                "withinBudget"
            ],
        }
        artifacts = {
            "subjectManifest": artifact_record(subject_path),
            "binary": artifact_record(Path(subject["binary"])),
            "library": artifact_record(Path(subject["library"])),
            "builder": artifact_record(Path(subject_builder.__file__)),
            "runner": artifact_record(Path(__file__)),
            "analyzer": artifact_record(Path(analysis.__file__)),
            "contract": artifact_record(
                REPO
                / "specs/137-svs-serial-production-offload/contracts/"
                "experiment-contract.md"
            ),
        }
        summary = {
            "schema": "spec137.preflight.v1",
            "campaignId": campaign_id,
            "subjectManifest": str(subject_path.resolve()),
            "subjectManifestSha256": sha256_file(subject_path),
            "checks": basic_checks,
            "admitted": all(basic_checks.values()),
            "cpuMap": cpu_map,
            "runtimeConfigs": configs,
            "noopPacer": pacer,
            "smokeResults": smoke_results,
            "smokeEvidence": smoke_evidence,
            "instrumentation": instrumentation,
            "artifacts": artifacts,
            "formalReceiptsCreated": len(
                list((campaign / "receipts").glob("*.json"))
            )
            if (campaign / "receipts").exists()
            else 0,
        }
        atomic_json(preflight / "preflight-summary.json", summary)
        return summary


def run_pilot(campaign: Path) -> dict[str, Any]:
    preflight = analysis.verify_preflight(campaign)
    subject = subject_builder.load_subject(Path(preflight["subjectManifest"]))
    cpu_map = preflight["cpuMap"]
    pilot = campaign / "pilot"
    if pilot.exists():
        raise RuntimeError(f"pilot already exists: {pilot}")
    pilot.mkdir()
    rows: list[dict[str, Any]] = []
    with CampaignLock(campaign):
        for rate in RATES:
            row: dict[str, Any] = {"candidateRate": rate}
            for mode in MODES:
                warmup, measure, drain = PILOT_TIMING
                config = {
                    "campaignId": campaign.name,
                    "cellId": f"pilot-{rate}-{mode}",
                    "mode": mode,
                    "rate": rate,
                    "warmup": warmup,
                    "measure": measure,
                    "drain": drain,
                    "formal": False,
                    "diagnostics": "enabled",
                }
                terminal = run_cell(
                    campaign, subject, cpu_map, config, namespace="pilot/cells"
                )
                if terminal["status"] != "complete":
                    raise RuntimeError(
                        f"pilot cell failed once-only: {config['cellId']}: "
                        f"{terminal['error']}"
                    )
                evidence = compact_cell_evidence(
                    cell_evidence(campaign, "pilot/cells", config)
                )
                row["face" if mode == "face-serial" else "worker"] = evidence
            rows.append(row)
            selection = analysis.select_stress_rate(rows)
            if selection["evaluated"][-1]["faceStressing"]:
                break
        selection.update(
            {
                "subjectManifestSha256": preflight["subjectManifestSha256"],
                "binarySha256": subject["binarySha256"],
                "runnerSha256": sha256_file(Path(__file__)),
                "analyzerSha256": sha256_file(Path(analysis.__file__)),
            }
        )
        atomic_json(pilot / "rate-selection.json", selection)
        return selection


def seal_campaign(campaign: Path) -> dict[str, Any]:
    preflight = analysis.verify_preflight(campaign)
    selection_path = campaign / "pilot/rate-selection.json"
    selection = load_json(selection_path)
    rate = selection.get("selectedRate")
    if rate is None:
        raise RuntimeError("cannot seal without a jointly admissible pilot rate")
    manifest_path = campaign / "campaign-manifest.json"
    seal_path = campaign / ".sealed"
    if manifest_path.exists() or seal_path.exists():
        raise RuntimeError("campaign is already sealed or partially sealed")
    if (campaign / "receipts").exists():
        raise RuntimeError("cannot seal a campaign containing formal receipts")
    with CampaignLock(campaign):
        manifest = {
            "schema": "spec137.campaign.v1",
            "state": "sealed",
            "campaignId": campaign.name,
            "createdUnixNs": time.time_ns(),
            "frozenRate": rate,
            "subjectManifest": preflight["subjectManifest"],
            "subjectManifestSha256": preflight["subjectManifestSha256"],
            "binarySha256": preflight["artifacts"]["binary"]["sha256"],
            "contractSha256": preflight["artifacts"]["contract"]["sha256"],
            "topologySha256": hashlib.sha256(
                b"peer-a:peer-b delay=10ms bw=100 loss=0\n"
            ).hexdigest(),
            "cpuMap": preflight["cpuMap"],
            "cpuMapSha256": hashlib.sha256(
                json.dumps(
                    preflight["cpuMap"], sort_keys=True, separators=(",", ":")
                ).encode("utf-8")
            ).hexdigest(),
            "rateSelection": str(selection_path.resolve()),
            "rateSelectionSha256": sha256_file(selection_path),
            "automaticRetry": False,
            "cells": formal_cells(int(rate)),
        }
        atomic_json(manifest_path, manifest)
        seal_path.write_text(sha256_file(manifest_path) + "\n", encoding="utf-8")
        return manifest


def verify_seal(campaign: Path) -> dict[str, Any]:
    manifest_path = campaign / "campaign-manifest.json"
    seal_path = campaign / ".sealed"
    if not seal_path.is_file() or not manifest_path.is_file():
        raise RuntimeError("campaign is not sealed")
    if seal_path.read_text(encoding="utf-8").strip() != sha256_file(
        manifest_path
    ):
        raise RuntimeError("campaign seal hash mismatch")
    manifest = load_json(manifest_path)
    if manifest.get("schema") != "spec137.campaign.v1":
        raise RuntimeError("campaign manifest schema mismatch")
    if manifest.get("automaticRetry") is not False:
        raise RuntimeError("automatic retry was enabled")
    if manifest.get("cells") != formal_cells(int(manifest["frozenRate"])):
        raise RuntimeError("sealed formal matrix changed")
    subject_builder.load_subject(Path(manifest["subjectManifest"]))
    if sha256_file(Path(manifest["subjectManifest"])) != manifest[
        "subjectManifestSha256"
    ]:
        raise RuntimeError("sealed subject manifest changed")
    if sha256_file(Path(manifest["rateSelection"])) != manifest[
        "rateSelectionSha256"
    ]:
        raise RuntimeError("sealed rate selection changed")
    return manifest


def run_formal(campaign: Path) -> list[dict[str, Any]]:
    outcomes: list[dict[str, Any]] = []
    with CampaignLock(campaign):
        manifest = verify_seal(campaign)
        subject = subject_builder.load_subject(Path(manifest["subjectManifest"]))
        ledger = ReceiptLedger(campaign / "receipts")
        for config in manifest["cells"]:
            manifest = verify_seal(campaign)
            started = time.time_ns()
            terminal = run_cell(
                campaign,
                subject,
                manifest["cpuMap"],
                {**config, "campaignId": campaign.name, "formal": True},
                namespace="formal/cells",
            )
            checks: dict[str, bool] = {}
            reason = terminal["error"]
            if terminal["status"] == "complete":
                evidence = cell_evidence(
                    campaign,
                    "formal/cells",
                    {**config, "campaignId": campaign.name},
                )
                checks = {
                    "attempted_rate_within_2_percent": evidence[
                        "attemptedRateError"
                    ]
                    <= 0.02,
                    "max_active_sync_signers_one": evidence[
                        "maxActiveSigners"
                    ]
                    == 1,
                    "production_fallback_zero": evidence["fallbacks"] == 0,
                    "production_accounting_remainder_zero": evidence[
                        "productionAccountingRemainder"
                    ]
                    == 0,
                    "publication_accounting_remainder_zero": evidence[
                        "publicationAccountingRemainder"
                    ]
                    == 0,
                    "event_and_resource_files_complete": evidence[
                        "resourceComplete"
                    ],
                    "shutdown_drained": evidence["shutdownDrained"],
                    "thread_owner_valid": evidence["ownerViolations"] == 0,
                }
            receipt = {
                "schema": "spec137.receipt.v1",
                "ordinal": config["ordinal"],
                "pair": config["pair"],
                "mode": config["mode"],
                "rate": config["rate"],
                "startedUnixNs": started,
                "endedUnixNs": time.time_ns(),
                "terminalStatus": terminal["status"],
                "admissionChecks": checks,
                "admissible": bool(checks) and all(checks.values()),
                "retryCount": 0,
                "reason": reason,
                "terminal": terminal,
            }
            ledger.append(receipt)
            outcomes.append(receipt)
            if terminal["status"] == "infrastructure-invalid":
                break
        atomic_json(
            campaign / "formal/campaign-terminal.json",
            {
                "schema": "spec137.campaign-terminal.v1",
                "receiptCount": len(outcomes),
                "outcomes": outcomes,
            },
        )
    return outcomes


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--subject-manifest", type=Path, default=DEFAULT_SUBJECT)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--preflight", action="store_true")
    actions.add_argument("--pilot", action="store_true")
    actions.add_argument("--seal", action="store_true")
    actions.add_argument("--run-formal", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    campaign = args.campaign.resolve()
    if args.preflight:
        result = run_preflight(campaign, args.subject_manifest.resolve())
        print(json.dumps(result, sort_keys=True))
        return 0 if result["admitted"] else 1
    if args.pilot:
        result = run_pilot(campaign)
        print(json.dumps(result, sort_keys=True))
        return 0 if result["selectedRate"] is not None else 2
    if args.seal:
        result = seal_campaign(campaign)
        print(json.dumps(result, sort_keys=True))
        return 0
    outcomes = run_formal(campaign)
    print(json.dumps(outcomes, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
