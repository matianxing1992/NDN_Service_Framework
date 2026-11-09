#!/usr/bin/env python3
"""Run the installed Spec226 Q1 actors over the maintained MiniNDN topology."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence

ROOT = Path(__file__).resolve().parents[2]
TOPOLOGY = ROOT / "Experiments/UAV/topology.conf"
NODE_NAMES = {"uav1", "uav2", "uav3", "compute", "gs"}
NEXT_HOPS = {
    "uav1": ("compute",),
    "uav2": ("compute",),
    "uav3": ("compute",),
    "compute": ("uav1", "uav2", "uav3", "gs"),
    "gs": ("compute",),
}
ACTOR_IDENTITIES = (
    "/example/uav/controller",
    "/example/uav/source-adapter/UAV1",
    "/example/uav/source-adapter/UAV2",
    "/example/uav/compute",
    "/example/uav/gs",
)
ACTOR_NAME_IDENTITIES = {
    "controller": "/example/uav/controller",
    "source-a": "/example/uav/source-adapter/UAV1",
    "source-b": "/example/uav/source-adapter/UAV2",
    "consumer": "/example/uav/compute",
}
REQUEST_ROWS = ("q1-cold", "q1-warm", "q1-missing")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def identity_keychain(root: Path, identity: str) -> Path:
    prefix = "/example/uav/"
    if not identity.startswith(prefix):
        raise RuntimeError(f"Spec226 actor identity is outside the campaign namespace: {identity}")
    slug = identity[len(prefix):].replace("/", "-")
    return root / "private" / "identities" / slug / "keychain"


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def socket_path(node: Any) -> Path:
    name = str(node.name)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", name):
        raise RuntimeError(f"invalid MiniNDN node name: {name!r}")
    path = Path("/run/nfd") / f"{name}.sock"
    if len(os.fsencode(str(path))) >= 108:
        raise RuntimeError(f"NFD Unix socket path exceeds AF_UNIX limit: {path}")
    return path


def nfdc(node: Any, command: str) -> str:
    transport = f"unix://{socket_path(node)}"
    return node.cmd(f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} {command}")


def clear_cs(nodes: Sequence[Any]) -> list[dict[str, Any]]:
    records = []
    for node in nodes:
        transport = f"unix://{socket_path(node)}"
        command = (
            f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} "
            "timeout --signal=TERM 15s nfdc cs erase / >/dev/null 2>&1; "
            "rc=$?; printf 'SPEC226_NFD_CS_ERASE_RC=%s\\n' \"$rc\"")
        output = node.cmd(command)
        return_codes = re.findall(
            r"(?m)^SPEC226_NFD_CS_ERASE_RC=(\d+)\r?$", output)
        if return_codes != ["0"]:
            raise RuntimeError(
                f"NFD CS erase failed for {node.name}: {output.strip()!r}")
        records.append({"node": node.name, "prefix": "/", "returnCode": 0})
    return records


def capture_cs(nodes: Sequence[Any], label: str,
               output_root: Path) -> dict[str, Any]:
    records = []
    for node in nodes:
        output = nfdc(node, "timeout --signal=TERM 15s nfdc cs info")
        if not output.strip() or "could not connect" in output.lower():
            raise RuntimeError(f"NFD CS snapshot failed for {node.name} at {label}")
        nfd_log = output_root / "minindn" / node.name / "log" / "nfd.log"
        try:
            with nfd_log.open("rb") as log:
                log_bytes = os.fstat(log.fileno()).st_size
                digest = hashlib.sha256()
                remaining = log_bytes
                while remaining:
                    block = log.read(min(1 << 20, remaining))
                    if not block:
                        raise RuntimeError(
                            f"NFD forwarder log truncated during snapshot for {node.name}")
                    digest.update(block)
                    remaining -= len(block)
                log_sha256 = digest.hexdigest()
        except OSError as error:
            raise RuntimeError(
                f"NFD forwarder log is unavailable for {node.name}: {error}") from error
        records.append({"node": node.name,
                        "output": output,
                        "sha256": hashlib.sha256(output.encode()).hexdigest(),
                        "forwarderLogPath": str(nfd_log.relative_to(output_root)),
                        "forwarderLogBytes": log_bytes,
                        "forwarderLogSha256": log_sha256})
    return {"label": label, "nodes": records}


def run_node_command(node: Any, command: str) -> tuple[int, str]:
    output = node.cmd(
        f'{command}; rc=$?; printf "\\nSPEC226_Q1_NODE_RC=%s\\n" "$rc"')
    matches = list(re.finditer(
        r"(?m)^SPEC226_Q1_NODE_RC=(\d+)\r?$", output))
    if len(matches) != 1:
        raise RuntimeError(f"MiniNDN node command returned no unique status: {command}")
    match = matches[0]
    return int(match.group(1)), output[:match.start()].strip()


def drop_first_input_interest(compute: Any, peer_ip: str,
                              gate_path: Path) -> dict[str, Any]:
    """Drop one cold-input Interest on the compute-to-source-A MiniNDN link."""
    record: dict[str, Any] = {
        "status": "FAIL", "node": "compute", "peer": "uav1",
        "peerIp": peer_ip, "protocol": "udp", "destinationPort": 6363,
        "filterPreference": 49152, "filterAdded": False,
        "filterRemoved": True, "clsactAdded": False,
        "clsactRemoved": True, "gateReleased": False, "dropPackets": 0,
    }
    interface = ""
    filter_added = False
    clsact_added = False
    errors: list[str] = []
    try:
        status, route = run_node_command(
            compute, f"ip -o route get {shlex.quote(peer_ip)}")
        record["route"] = route
        if status != 0:
            raise RuntimeError("cannot resolve compute-to-source-A interface")
        match = re.search(r"\bdev\s+([A-Za-z0-9_.-]{1,15})\b", route)
        if match is None:
            raise RuntimeError("route lookup did not identify a MiniNDN interface")
        interface = match.group(1)
        record["interface"] = interface

        status, qdisc_text = run_node_command(
            compute, f"tc -j qdisc show dev {shlex.quote(interface)}")
        qdiscs = json.loads(qdisc_text) if status == 0 else []
        if status != 0 or not isinstance(qdiscs, list):
            raise RuntimeError("cannot inspect the original MiniNDN qdiscs")
        if any(item.get("kind") == "clsact" for item in qdiscs
               if isinstance(item, dict)):
            raise RuntimeError("MiniNDN interface already has a clsact qdisc")

        status, output = run_node_command(
            compute, f"tc qdisc add dev {shlex.quote(interface)} clsact")
        if status != 0:
            raise RuntimeError(f"cannot add temporary clsact qdisc: {output}")
        clsact_added = True
        record["clsactAdded"] = True

        preference = str(record["filterPreference"])
        status, filters_text = run_node_command(
            compute, "tc -j filter show dev " + shlex.quote(interface) +
            " egress pref " + preference)
        filters = json.loads(filters_text) if status == 0 else []
        if status != 0 or filters:
            raise RuntimeError("temporary tc filter preference is already occupied")
        command = (
            "tc filter add dev " + shlex.quote(interface) +
            " egress protocol ip pref " + preference +
            " flower ip_proto udp dst_port 6363 action drop")
        status, output = run_node_command(compute, command)
        if status != 0:
            raise RuntimeError(f"cannot add one-shot NDN Interest filter: {output}")
        filter_added = True
        record["filterAdded"] = True

        gate_path.parent.mkdir(parents=True, exist_ok=True)
        gate_path.write_text("release\n", encoding="utf-8")
        record["gateReleased"] = True
        deadline = time.monotonic() + 3.0
        stats_pattern = re.compile(
            r"\bSent\s+\d+\s+bytes\s+\d+\s+pkt\s+"
            r"\(dropped\s+(\d+)")
        stats_output = ""
        while time.monotonic() < deadline:
            status, stats_output = run_node_command(
                compute, "tc -s filter show dev " + shlex.quote(interface) +
                " egress pref " + preference)
            if status != 0:
                raise RuntimeError("cannot read one-shot NDN drop counters")
            stat_match = stats_pattern.search(stats_output)
            if stat_match is not None:
                record["dropPackets"] = int(stat_match.group(1))
                if record["dropPackets"] > 0:
                    break
            time.sleep(0.005)
        record["filterStats"] = stats_output
        if record["dropPackets"] < 1:
            raise RuntimeError("no NDN packet was dropped before the bounded deadline")
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        record["error"] = f"{type(error).__name__}: {error}"
    finally:
        if filter_added:
            try:
                status, output = run_node_command(
                    compute, "tc filter del dev " + shlex.quote(interface) +
                    " egress pref " + str(record["filterPreference"]))
                record["filterRemoved"] = status == 0
                if status != 0:
                    errors.append(f"filter removal failed: {output}")
            except (OSError, RuntimeError) as error:
                record["filterRemoved"] = False
                errors.append(f"filter removal failed: {error}")
        if clsact_added:
            try:
                status, output = run_node_command(
                    compute, f"tc qdisc del dev {shlex.quote(interface)} clsact")
                record["clsactRemoved"] = status == 0
                if status != 0:
                    errors.append(f"clsact removal failed: {output}")
            except (OSError, RuntimeError) as error:
                record["clsactRemoved"] = False
                errors.append(f"clsact removal failed: {error}")
    if errors:
        record["cleanupErrors"] = errors
    record["status"] = (
        "PASS" if record["filterAdded"] and record["gateReleased"] and
        record["dropPackets"] > 0 and record["filterRemoved"] and
        record["clsactRemoved"] and not errors and "error" not in record
        else "FAIL")
    return record


def wait_for(name: str, process: Any, log_path: Path,
             marker: str, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        if marker in text:
            return
        if process.poll() is not None:
            raise RuntimeError(
                f"{name} exited before {marker}: returncode={process.returncode}")
        time.sleep(0.1)
    raise TimeoutError(f"{name} did not emit {marker}")


def verify_python_consumer_result(log_path: Path, row: str) -> None:
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    results = [line for line in lines
               if line.startswith("SPEC226_Q1_PYTHON_RESULT ") and
               f"row={row} " in line]
    if len(results) != 1 or "status=PASS" not in results[0]:
        raise RuntimeError(f"Python direct-admission check failed for {row}")
    expected = "READY" if row == "q1-warm" else "REJECTED"
    if f"expected={expected} " not in results[0]:
        raise RuntimeError(f"Python direct-admission outcome mismatch for {row}")
    admissions = [line for line in lines
                  if line.startswith("SPEC226_Q1_ADMISSION ") and
                  f"row={row} " in line]
    computes = [line for line in lines
                if line.startswith("SPEC226_Q1_COMPUTE ") and
                f"row={row} " in line]
    wanted_status = "READY" if row == "q1-warm" else "REJECTED"
    wanted_compute = ("count=1 result=10" if row == "q1-warm" else
                      "count=0 result=none")
    if (len(admissions) != 1 or f"status={wanted_status} " not in admissions[0] or
            len(computes) != 1 or wanted_compute not in computes[0]):
        raise RuntimeError(f"Python admission gate or compute boundary failed for {row}")
    if row == "q1-missing" and not any(
            f"reason={reason} " in admissions[0]
            for reason in ("INPUT_DEADLINE", "INPUT_TRANSFER_FAILED")):
        raise RuntimeError("Python missing input did not fail at dependency retrieval")
    objects = [line for line in lines
               if line.startswith("SPEC226_Q1_PYTHON_INPUT ") and
               f"row={row} " in line]
    if row == "q1-warm":
        input_lines = [line for line in lines
                       if line.startswith("SPEC226_Q1_PYTHON_INPUT ")]
        if len(input_lines) != 2 or "readonly=true" not in results[0] or \
                "lifetime=true" not in results[0]:
            raise RuntimeError("Python admitted slot or read-only view check failed")
    elif objects:
        raise RuntimeError("missing Python input reached application validation")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--actor", required=True, type=Path)
    parser.add_argument("--controller", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--trust-schema", required=True, type=Path)
    parser.add_argument("--python-consumer", type=Path,
                        help="run the installed Python direct-admission consumer")
    parser.add_argument("--python-executable", default=sys.executable)
    parser.add_argument("--cache-size", type=int, default=65536)
    parser.add_argument("--request-timeout", type=float, default=75.0)
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> dict[str, Any]:
    run_root = args.run_root.expanduser()
    if run_root.is_symlink():
        raise RuntimeError("Q1 run root must be an existing non-symlink directory")
    output = run_root.resolve(strict=True)
    python_consumer = getattr(args, "python_consumer", None)
    if (python_consumer is not None and
            getattr(args, "core_loss_row", None) is not None):
        raise RuntimeError("Python consumer mode is limited to the direct Q1 gate")
    if not output.is_dir():
        raise RuntimeError("Q1 run root must be an existing non-symlink directory")
    if (args.cache_size < 1 or args.request_timeout <= 0 or
            not args.actor.is_file() or not args.controller.is_file() or
            not args.policy.is_file() or not args.trust_schema.is_file()):
        raise RuntimeError("Q1 runtime input is missing or invalid")
    if python_consumer is not None and not python_consumer.is_file():
        raise RuntimeError("Python direct-admission consumer is missing")
    missing_keychains = [identity for identity in ACTOR_IDENTITIES
                         if not (identity_keychain(output, identity) / "pib").is_dir() or
                         not (identity_keychain(output, identity) / "tpm").is_dir()]
    if missing_keychains:
        raise RuntimeError(
            "Spec226 actor credential homes are unavailable: " + ",".join(missing_keychains))

    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn
    from minindn.util import getPopen

    run_dir = output / "minindn"
    if run_dir.exists():
        raise RuntimeError("MiniNDN work directory already exists")
    saved_argv = sys.argv
    sys.argv = [saved_argv[0]]
    try:
        ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(run_dir), controller=None)
    finally:
        sys.argv = saved_argv

    processes: list[dict[str, Any]] = []
    nfd_apps: list[Any] = []
    cleanup: dict[str, Any] = {
        "processes": [], "processesAllExited": False,
        "ndnStopped": False, "miniNdnCleaned": False, "ok": False,
    }
    snapshots: list[dict[str, Any]] = []
    first_failure: str | None = None
    first_interest_drop: dict[str, Any] | None = None
    request_runs: list[dict[str, Any]] = []
    nodes: list[Any] = []
    core_loss_row = getattr(args, "core_loss_row", None)
    loss_profile = getattr(args, "loss_profile", None)
    core_capture_launcher = getattr(args, "core_capture_launcher", None)
    if core_loss_row is not None:
        if (not isinstance(core_loss_row, str) or not core_loss_row.startswith("L-Core-") or
                not isinstance(loss_profile, dict) or loss_profile.get("module") != "Core"):
            raise RuntimeError("Core-loss run requires its frozen row and loss profile")
        core_mode = "Xor" if loss_profile.get("mode") == "xor" else "None"
        request_rows = [
            f"core-loss-{core_mode}-loss{loss_profile['lossPercent']}-"
            f"seed{loss_profile['rowSeed']}-object-{index:02d}"
            for index in range(32)
        ]
    else:
        request_rows = (list(REQUEST_ROWS) if python_consumer is None else
                        ["q1-warm", "q1-missing"])
    core_loss_shutdown = output / "private" / "control" / "core-loss.shutdown"
    consumer_wire_dir = output / "consumer-admitted-wires"
    seeded_loss_record: dict[str, Any] | None = None
    packet_capture_session: dict[str, Any] | None = None
    seeded_loss_result: dict[str, Any] | None = None
    packet_capture_result: dict[str, Any] | None = None
    core_process_run: dict[str, Any] | None = None

    def launch(name: str, node_name: str, identity: str,
               command: Sequence[str]) -> dict[str, Any]:
        node = ndn.net[node_name]
        private = output / "private" / name
        for child in ("home", "config", "data", "cache"):
            (private / child).mkdir(parents=True, exist_ok=True)
        mpl = private / "mpl"
        mpl.mkdir(parents=True, exist_ok=True)
        actor_keychain = identity_keychain(output, identity)
        generation_state = output / "private" / "controller" / "generation.state"
        generation_state.parent.mkdir(parents=True, exist_ok=True)
        socket = socket_path(node)
        actor_environment = {
            "PATH": os.environ.get("PATH", ""),
            "HOME": str(private / "home"),
            "XDG_CONFIG_HOME": str(private / "config"),
            "XDG_DATA_HOME": str(private / "data"),
            "XDG_CACHE_HOME": str(private / "cache"),
            "MPLCONFIGDIR": str(mpl),
            "NDN_CLIENT_PIB": "pib-sqlite3:" + str(actor_keychain / "pib"),
            "NDN_CLIENT_TPM": "tpm-file:" + str(actor_keychain / "tpm"),
            "NDN_CLIENT_TRANSPORT": "unix://" + str(socket),
            "NDNSF_CONTROLLER_GENERATION_STATE": str(generation_state),
            "NDNSF_SPEC191_RUN_ROOT": str(output),
            "NDN_LOG": "*=WARN",
        }
        if (core_loss_row is not None and name == "consumer" and
                loss_profile.get("mode") == "xor"):
            actor_environment["NDN_LOG"] = (
                "*=WARN:ndn_service_framework.ServiceProvider=TRACE:"
                "ndn_service_framework.DependencyTransfer=TRACE")
        env = ["unset LD_LIBRARY_PATH NDNSF_UAV_LIBRARY_PATH"]
        env.extend(f"export {key}={shlex.quote(value)}"
                   for key, value in actor_environment.items())
        env.extend((f"cd {shlex.quote(str(ROOT))}",
                    f"exec {shlex.join([str(item) for item in command])}"))
        log_path = output / f"{name}.log"
        direct_capture = (name == "consumer" and core_capture_launcher is not None)
        if direct_capture:
            capture = core_capture_launcher(
                name, list(command), log_path, actor_environment)
            process = capture["process"]
            log = None
        else:
            capture = None
            log = log_path.open("wb")
            process = getPopen(node, "; ".join(env), shell=True,
                               stdout=log, stderr=subprocess.STDOUT)
        record = {"name": name, "node": node_name, "identity": identity,
                  "keychain": str(actor_keychain), "pid": process.pid,
                  "command": list(command), "log": str(log_path),
                  "logHandle": log, "process": process,
                  "directCapture": capture}
        processes.append(record)
        return record

    try:
        ndn.start()
        nodes = list(ndn.net.hosts)
        if {node.name for node in nodes} != NODE_NAMES:
            raise RuntimeError("MiniNDN host set does not match Spec226 Q1 topology")

        for node in nodes:
            nfd = Nfd(node, csSize=args.cache_size)
            socket = socket_path(node)
            conf_path = Path(nfd.confFile)
            if node.name in {"compute", "uav1"}:
                conf_text, replacements = re.subn(
                    r"(?m)^([ \t]*default_level[^\n]*)$",
                    r"\1\n  Forwarder TRACE",
                    conf_path.read_text(encoding="utf-8"),
                    count=1,
                )
                if replacements != 1:
                    raise RuntimeError(f"cannot enable {node.name} NFD forwarder trace")
                conf_path.write_text(conf_text, encoding="utf-8")
            Path(nfd.clientConf).write_text(f"transport=unix://{socket}\n",
                                            encoding="utf-8")
            nfd.start()
            nfd_apps.append(nfd)
        for node in nodes:
            socket = socket_path(node)
            deadline = time.monotonic() + 15
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not socket.exists():
                raise RuntimeError(f"NFD socket did not appear: {socket}")
        time.sleep(1)

        created_faces = ndn.setupFaces()
        face_target_ips = {
            (node.name, peer_name): peer_ip
            for node, links in created_faces.items()
            for peer_name, peer_ip, _cost in links
        }
        route_records = []
        for node in nodes:
            face_list = nfdc(node, "nfdc face list")
            for peer_name in NEXT_HOPS[node.name]:
                peer_ip = face_target_ips[(node.name, peer_name)]
                match = re.search(
                    rf"faceid=(\d+)\s+remote=(?:udp|udp4)://"
                    rf"{re.escape(peer_ip)}:6363", face_list)
                if match is None:
                    raise RuntimeError(
                        f"NFD face missing node={node.name} peer={peer_name}")
                face_id = match.group(1)
                command = f"nfdc route add /example/uav nexthop {face_id}"
                route_records.append({"node": node.name, "peer": peer_name,
                                      "faceId": int(face_id),
                                      "result": nfdc(node, command)})
            route_records.append({
                "node": node.name, "peer": None,
                "result": nfdc(node, "nfdc strategy set /example/uav "
                                      "/localhost/nfd/strategy/multicast"),
            })
        write_json(output / "routes.json", {"routes": route_records})
        clear_records = clear_cs(nodes)
        snapshots.append(capture_cs(nodes, "startup-after-clear", output))

        if core_loss_row is not None:
            from spec226_tc_loss import install_seeded_loss, start_packet_captures
            seeded_loss_record = install_seeded_loss(
                ndn.net, row_seed=loss_profile["rowSeed"],
                loss_percent=loss_profile["lossPercent"],
                output_dir=output / "spec226-tc-loss")
            seeded_loss_record["row"] = core_loss_row
            seeded_loss_record["profile"] = {
                "row": core_loss_row,
                "rowSeed": loss_profile["rowSeed"],
                "lossPercent": loss_profile["lossPercent"],
                "evidenceDir": str((output / "spec226-tc-loss").resolve()),
            }
            write_json(output / "spec226-tc-loss" / "tc-loss.json",
                       seeded_loss_record)
            packet_capture_session = start_packet_captures(
                ndn.net, output / "packet-captures")

        controller = launch("controller", "gs", ACTOR_NAME_IDENTITIES["controller"], (
            str(args.controller), "--controller-prefix", "/example/uav/controller",
            "--policy-file", str(args.policy), "--trust-schema", str(args.trust_schema),
            "--run-for-ms", "180000",
        ))
        wait_for("controller", controller["process"],
                 Path(controller["log"]), "NDNSF_CONTROLLER_READY", 45)

        actors = (
            ("source-a", "uav1", "source-a"),
            ("source-b", "uav2", "source-b"),
            ("consumer", "compute", "consumer"),
        )
        input_gate = output / "private" / "control" / "q1-cold-input.gate"
        input_gate.unlink(missing_ok=True)
        for name, node_name, mode in actors:
            if mode == "consumer" and python_consumer is not None:
                command = [
                    args.python_executable,
                    str(python_consumer.resolve()),
                    "--trust-schema", str(args.trust_schema.resolve()),
                ]
            else:
                command = [
                    str(args.actor), "--mode", mode,
                    "--trust-schema", str(args.trust_schema),
                ]
            if mode == "consumer" and python_consumer is None:
                command.extend(("--input-admission-gate", str(input_gate)))
            if core_loss_row is not None:
                command.extend(("--shutdown-file", str(core_loss_shutdown)))
                if mode == "consumer":
                    command.extend(("--wire-output-dir", str(consumer_wire_dir)))
            record = launch(name, node_name, ACTOR_NAME_IDENTITIES[mode], command)
            wait_for(name, record["process"], Path(record["log"]),
                     f"SPEC226_Q1_ACTOR_READY role={mode}", 45)

        if core_loss_row is not None:
            from spec226_tc_loss import mark_seeded_loss_workload_start
            mark_seeded_loss_workload_start(ndn.net, seeded_loss_record)

        for request_index, request_row in enumerate(request_rows):
            started = time.monotonic()
            if core_loss_row is not None and request_index > 0:
                not_before = request_runs[-1]["startedMonotonic"] + 1.0
                delay = not_before - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                started = time.monotonic()
            user = launch(f"user-{request_row}", "gs", "/example/uav/gs", (
                str(args.actor), "--mode", "user", "--request-row", request_row,
                "--trust-schema", str(args.trust_schema),
            ))
            if request_row == "q1-cold":
                consumer = next(item for item in processes
                                if item["name"] == "consumer")
                wait_for("consumer", consumer["process"], Path(consumer["log"]),
                         "SPEC226_Q1_INPUT_READY row=q1-cold after=source-b", 15)
                first_interest_drop = drop_first_input_interest(
                    ndn.net["compute"], face_target_ips[("compute", "uav1")],
                    input_gate)
                if first_interest_drop["status"] != "PASS":
                    raise RuntimeError(
                        "Q1 first Interest drop failed: " +
                        str(first_interest_drop.get("error", "cleanup or drop counter failed")))
            wait_for(user["name"], user["process"], Path(user["log"]),
                     f"SPEC226_Q1_RESULT row={request_row}",
                     args.request_timeout)
            try:
                return_code = user["process"].wait(timeout=10)
            except subprocess.TimeoutExpired:
                user["process"].terminate()
                return_code = user["process"].wait(timeout=5)
            request_runs.append({
                "row": request_row,
                "returnCode": return_code,
                "durationSeconds": round(time.monotonic() - started, 6),
                "startedMonotonic": started,
                "log": str(Path(user["log"]).relative_to(output)),
                "logSha256": sha256_file(Path(user["log"])),
            })
            if return_code != 0:
                raise RuntimeError(
                    f"user actor failed for {request_row}: {return_code}")
            if core_loss_row is None:
                expected_markers = (
                    ("source-a", f"SPEC226_Q1_SOURCE_DONE row={request_row} role=source-a"),
                    ("source-b", f"SPEC226_Q1_SOURCE_DONE row={request_row} role=source-b"),
                    ("consumer", f"SPEC226_Q1_CONSUMER_DONE row={request_row}"))
            else:
                object_index = request_index
                expected_markers = (
                    ("source-a", f"SPEC226_CORE_LOSS_SOURCE_DONE row={request_row} index={object_index}"),
                    ("consumer", f"SPEC226_CORE_LOSS_CONSUMER_DONE row={request_row} index={object_index}"))
            for name, marker in expected_markers:
                actor = next(item for item in processes if item["name"] == name)
                wait_for(name, actor["process"], Path(actor["log"]), marker, 15)
            if python_consumer is not None:
                consumer = next(item for item in processes
                                if item["name"] == "consumer")
                verify_python_consumer_result(Path(consumer["log"]), request_row)
            if core_loss_row is None:
                snapshots.append(capture_cs(nodes, f"after-{request_row}", output))

        if core_loss_row is not None:
            from spec226_tc_loss import finish_packet_captures, finish_seeded_loss_session
            seeded_loss_result = finish_seeded_loss_session(ndn.net, seeded_loss_record)
            seeded_loss_record = None
            packet_capture_result = finish_packet_captures(packet_capture_session)
            packet_capture_session = None
            if seeded_loss_result.get("qualificationStatus") != "PASS":
                raise RuntimeError("Core-loss seeded netem exposure or cleanup failed")
            if packet_capture_result.get("status") != "PASS":
                raise RuntimeError("Core-loss packet capture did not close cleanly")
            core_loss_shutdown.parent.mkdir(parents=True, exist_ok=True)
            core_loss_shutdown.touch()
            for actor in processes:
                if actor["name"] not in {"source-a", "source-b", "consumer"}:
                    continue
                try:
                    actor["process"].wait(timeout=15)
                except subprocess.TimeoutExpired:
                    raise RuntimeError(f"Core-loss Provider did not shut down: {actor['name']}")
                if actor.get("directCapture") is not None:
                    core_process_run = actor["directCapture"]["wait"]()
                    if core_process_run.get("returnCode") != 0:
                        raise RuntimeError("captured Core process exited unsuccessfully")

    except Exception as error:
        first_failure = f"{type(error).__name__}: {error}"
    finally:
        if core_loss_row is not None:
            core_loss_shutdown.parent.mkdir(parents=True, exist_ok=True)
            core_loss_shutdown.touch()
            if seeded_loss_record is not None and nodes:
                try:
                    from spec226_tc_loss import finish_seeded_loss_session
                    seeded_loss_result = finish_seeded_loss_session(
                        ndn.net, seeded_loss_record)
                    seeded_loss_record = None
                except Exception as error:
                    cleanup["seededLossCleanupError"] = (
                        f"{type(error).__name__}: {error}")
            if packet_capture_session is not None:
                try:
                    from spec226_tc_loss import finish_packet_captures
                    packet_capture_result = finish_packet_captures(
                        packet_capture_session)
                    packet_capture_session = None
                except Exception as error:
                    cleanup["packetCaptureCleanupError"] = (
                        f"{type(error).__name__}: {error}")
            for actor in processes:
                if actor["name"] not in {"source-a", "source-b", "consumer"}:
                    continue
                try:
                    actor["process"].wait(timeout=10)
                except subprocess.TimeoutExpired:
                    cleanup.setdefault("gracefulProviderShutdownTimeouts", []).append(
                        actor["name"])
            for actor in processes:
                capture = actor.get("directCapture")
                if capture is None or core_process_run is not None:
                    continue
                try:
                    core_process_run = capture["wait"](timeout=30)
                except Exception as error:
                    cleanup["coreCaptureError"] = (
                        f"{type(error).__name__}: {error}")
        for record in reversed(processes):
            process = record["process"]
            signal_error = None
            wait_error = None
            if process.poll() is None:
                try:
                    process.send_signal(signal.SIGTERM)
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=5)
                    except Exception as error:
                        wait_error = f"{type(error).__name__}: {error}"
                except Exception as error:
                    signal_error = f"{type(error).__name__}: {error}"
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=5)
                    except Exception as wait_failure:
                        wait_error = f"{type(wait_failure).__name__}: {wait_failure}"
            cleanup_item = {"name": record["name"], "pid": record["pid"],
                            "exitStatus": process.poll()}
            if signal_error:
                cleanup_item["signalError"] = signal_error
            if wait_error:
                cleanup_item["waitError"] = wait_error
            cleanup["processes"].append(cleanup_item)
        for record in processes:
            if record["logHandle"] is not None:
                record["logHandle"].close()
        for nfd in reversed(nfd_apps):
            try:
                nfd.stop()
            except Exception as error:
                cleanup.setdefault("nfdStopErrors", []).append(
                    f"{type(error).__name__}: {error}")
        try:
            ndn.stop()
            cleanup["ndnStopped"] = True
        except Exception as error:
            cleanup["ndnStopError"] = f"{type(error).__name__}: {error}"
        try:
            Minindn.cleanUp()
            cleanup["miniNdnCleaned"] = True
        except Exception as error:
            cleanup["miniNdnCleanupError"] = f"{type(error).__name__}: {error}"
        cleanup["processesAllExited"] = all(
            item["exitStatus"] is not None for item in cleanup["processes"])
        cleanup["seededLossClosed"] = (
            core_loss_row is None or
            (isinstance(seeded_loss_result, dict) and
             seeded_loss_result.get("qualificationStatus") == "PASS"))
        cleanup["packetCapturesClosed"] = (
            core_loss_row is None or
            (isinstance(packet_capture_result, dict) and
             packet_capture_result.get("status") == "PASS"))
        cleanup["networkFilterRemoved"] = (
            first_interest_drop is None or
            (first_interest_drop.get("filterRemoved") is True and
             first_interest_drop.get("clsactRemoved") is True))
        cleanup["ok"] = (cleanup["processesAllExited"] and
                         cleanup["ndnStopped"] and cleanup["miniNdnCleaned"] and
                         cleanup["seededLossClosed"] and
                         cleanup["packetCapturesClosed"] and
                         not cleanup.get("nfdStopErrors") and
                         cleanup["networkFilterRemoved"])

    report = {
        "schema": "spec226-q1-minindn-run-v1",
        "status": "PASS" if first_failure is None and cleanup["ok"] else "FAIL",
        "firstFailure": first_failure,
        "topology": {"path": str(TOPOLOGY), "sha256": sha256_file(TOPOLOGY)},
        "cacheSizePackets": args.cache_size,
            "consumerImplementation": (
                "python" if python_consumer is not None else "cpp"),
        "cacheClears": clear_records if "clear_records" in locals() else [],
        "cacheSnapshots": snapshots,
        "requests": request_runs,
        "firstInterestDrop": first_interest_drop,
        "seededLoss": seeded_loss_result,
        "packetCapture": packet_capture_result,
        "coreProcessRun": core_process_run,
        "processes": [
            {"name": item["name"], "node": item["node"],
             "command": item["command"],
             "log": str(Path(item["log"]).relative_to(output)),
             "logSha256": sha256_file(Path(item["log"]))}
            for item in processes
        ],
        "cleanup": cleanup,
    }
    write_json(output / "q1-run.json", report)
    return report


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = run(args)
    except Exception as error:
        print(f"SPEC226_Q1_RUNNER_FAIL error={type(error).__name__}:{error}",
              file=sys.stderr)
        return 1
    print(json.dumps({"status": report["status"],
                      "firstFailure": report["firstFailure"],
                      "requestRows": [item["row"] for item in report["requests"]],
                      "cleanup": report["cleanup"]}, sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
