#!/usr/bin/env python3
"""Run the paired bidirectional NDN-SVS PubSub benchmark in MiniNDN."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import signal
import subprocess
import sys
import time
from typing import Any

REPO = Path(__file__).resolve().parents[1]
if __name__ == "__main__" and not __package__:
    sys.path.insert(0, str(REPO))

from Experiments.analyze_svs_pubsub_commit_latency import (  # noqa: E402
    PEERS, RATES, summarize_cell, validate_manifest,
)
from Experiments.build_svs_pubsub_commit_bench import SUBJECTS as PINNED_SUBJECTS  # noqa: E402


ANALYZER = REPO / "Experiments/analyze_svs_pubsub_commit_latency.py"
DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-pubsub-bench.cpp"
SUBJECTS = tuple(subject for subject, _, _ in PINNED_SUBJECTS)
FORMAL = {"warmupSeconds": 10, "measureSeconds": 60, "drainSeconds": 10,
          "convergeSeconds": 5, "payloadBytes": 256}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def subject_map(authority: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["subject"]: item for item in authority["subjects"]}


def matched_schedule() -> list[tuple[int, int]]:
    return [(rate, 1) for rate in RATES]


def make_manifest(campaign_id: str, authority_path: Path) -> dict[str, Any]:
    authority = json.loads(authority_path.read_text(encoding="utf-8"))
    subjects = subject_map(authority)
    if authority.get("schemaVersion") != "spec132-subjects-v1" or len(authority["subjects"]) != 2:
        raise ValueError("expected a bidirectional benchmark build authority")
    for name, commit, latest in PINNED_SUBJECTS:
        record = subjects.get(name, {})
        if record.get("baseCommit") != commit or record.get("publishApi") != (
                "publishAsync" if latest else "publish"):
            raise ValueError(f"wrong pinned build subject: {name}")
        if any(not Path(record[key]).is_absolute() for key in ("binary", "library")):
            raise ValueError(f"binary and library paths must be absolute: {name}")
    cells = []
    ordinal = 0
    schedule = matched_schedule()
    for subject in SUBJECTS:
        for rate, repetition in schedule:
            ordinal += 1
            cell_id = f"{ordinal:02d}-{subject}-r{rate}-rep{repetition}"
            cells.append({"ordinal": ordinal, "cellId": cell_id, "subject": subject,
                          "ratePpsPerPeer": rate, "aggregateTargetPps": 2 * rate,
                          "repetition": repetition, "attempt": 1,
                          **FORMAL, "binary": subjects[subject]["binary"],
                          "library": subjects[subject]["library"],
                          "binarySha256": subjects[subject]["binarySha256"],
                          "librarySha256": subjects[subject]["librarySha256"],
                          "baseCommit": subjects[subject]["baseCommit"]})
    return {
        "schemaVersion": "spec132-campaign-v1", "campaignId": campaign_id,
        "createdUnixNs": time.time_ns(), "formal": True, "automaticRetry": False,
        "cpuAffinity": sorted(os.sched_getaffinity(0))[:4],
        "topology": {"hosts": 2, "bandwidthMbps": 100, "oneWayDelayMs": 10,
                     "lossPercent": 0},
        "buildAuthority": str(authority_path.resolve()),
        "buildAuthoritySha256": sha256_file(authority_path),
        "canonicalPatchSha256": authority["canonicalPatchSha256"],
        "runnerSha256": sha256_file(Path(__file__)), "analyzerSha256": sha256_file(ANALYZER),
        "driverSha256": sha256_file(DRIVER), "subjects": authority["subjects"], "cells": cells,
    }


def verify_frozen(manifest: dict[str, Any]) -> None:
    validate_manifest(manifest)
    if manifest.get("schemaVersion") != "spec132-campaign-v1" or manifest.get("topology") != {
            "hosts": 2, "bandwidthMbps": 100, "oneWayDelayMs": 10, "lossPercent": 0}:
        raise RuntimeError("wrong bidirectional campaign schema or topology")
    authority_path = Path(manifest["buildAuthority"])
    if sha256_file(authority_path) != manifest["buildAuthoritySha256"]:
        raise RuntimeError("build authority drift after campaign sealing")
    authority = json.loads(authority_path.read_text())
    if manifest["subjects"] != authority["subjects"]:
        raise RuntimeError("manifest subjects do not match pinned build authority")
    for path, expected in ((Path(__file__), manifest["runnerSha256"]),
                           (ANALYZER, manifest["analyzerSha256"]),
                           (DRIVER, manifest["driverSha256"])):
        if sha256_file(path) != expected:
            raise RuntimeError(f"formal source drift: {path}")
    subjects = subject_map(authority)
    for cell in manifest["cells"]:
        subject = subjects[cell["subject"]]
        if Path(cell["cellId"]).name != cell["cellId"] or cell["cellId"] == "..":
            raise RuntimeError("cell identifiers must be a single path component")
        for key in ("binary", "library", "binarySha256", "librarySha256", "baseCommit"):
            if cell[key] != subject[key]:
                raise RuntimeError(f"cell {key} does not match pinned build authority")
        if any(cell.get(key) != value for key, value in FORMAL.items()):
            raise RuntimeError("formal workload drift")
    for subject in manifest["subjects"]:
        binary, library = Path(subject["binary"]), Path(subject["library"])
        if sha256_file(binary) != subject["binarySha256"] or sha256_file(library) != subject["librarySha256"]:
            raise RuntimeError(f"frozen binary/library drift: {subject['subject']}")
        if subject["driverSha256"] != manifest["driverSha256"]:
            raise RuntimeError(f"build driver mismatch: {subject['subject']}")
        linkage = subprocess.check_output(["ldd", str(binary)], text=True,
                                          env=library_env(library))
        if any(line.split()[0].startswith("libndnsf") for line in linkage.splitlines()
               if line.split()):
            raise RuntimeError("NDNSF dependency in pure NDN-SVS subject")
        links = [line.split()[2] for line in linkage.splitlines()
                 if line.strip().startswith("libndn-svs.so") and " => " in line]
        if len(links) != 1 or Path(links[0]).resolve() != library.resolve():
            raise RuntimeError(f"benchmark is linked to a different NDN-SVS build: {binary}")


def library_env(library: Path) -> dict[str, str]:
    env = {**os.environ, "LD_LIBRARY_PATH": str(library.parent)}
    env.pop("LD_PRELOAD", None)
    return env


def stop(process: Any, grace: float = 3.0) -> int | None:
    if process is None:
        return None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=grace)
    return process.returncode


def wait_ready(path: Path, process: Any, timeout: float = 15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and "SPEC132_READY" in path.read_text(encoding="utf-8", errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError(f"peer exited before READY: rc={process.returncode}")
        time.sleep(0.1)
    raise RuntimeError("peer READY timeout")


def classify_terminal(infrastructure_error: str, codes: dict[str, int | None]) -> str:
    if infrastructure_error:
        return "INFRA_FAILURE"
    if set(codes) == set(PEERS) and all(code == 0 for code in codes.values()):
        return "COMPLETE"
    return "SUBJECT_FAILURE"


def process_sample(processes: dict[str, Any]) -> dict[str, Any]:
    sample = {"monotonicNs": time.monotonic_ns(), "processes": {}}
    for role, process in processes.items():
        if process.poll() is not None:
            continue
        proc = Path(f"/proc/{process.pid}")
        try:
            status = (proc / "status").read_text(encoding="utf-8")
            fields = (proc / "stat").read_text().rpartition(")")[2].split()
        except FileNotFoundError:
            continue
        entry = {key: value.strip() for line in status.splitlines()
                 for key, _, value in [line.partition(":")] if key in ("VmRSS", "Threads")}
        entry.update(pid=process.pid, peerResolved=True,
                     cpuTicks=int(fields[11]) + int(fields[12]))
        sample["processes"][role] = entry
    return sample


def nfd_command(node, arguments: list[str], env: dict[str, str], log) -> str:
    process = node.popen(["nfdc", *arguments], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    try:
        output, _ = process.communicate(timeout=10)
    finally:
        stop(process)
    log.write(output)
    if process.returncode:
        raise RuntimeError(f"failed NFD command on {node.name}: {shlex.join(arguments)}")
    return output


def run_cell(campaign: Path, config: dict[str, Any], *, formal: bool = True) -> dict[str, Any]:
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    cell = campaign / "cells" / config["cellId"]
    if formal and (cell.exists() or (campaign / "receipts" / f"{config['cellId']}.json").exists()):
        raise RuntimeError(f"formal cell already attempted: {config['cellId']}")
    cell.mkdir(parents=True, exist_ok=False)
    atomic_json(cell / "cell-config.json", config)
    topology = cell / "topology.conf"
    nonce = secrets.token_hex(4)
    left, right = f"sa{nonce}", f"sb{nonce}"
    topology.write_text(f"[nodes]\n{left}:\n{right}:\n\n[links]\n"
                        f"{left}:{right} delay=10ms bw=100 loss=0\n", encoding="utf-8")
    sync_prefix = f"/spec132/sync/{config['cellId']}"
    prefixes = {peer: f"/spec132/peer/{config['cellId']}/{peer}" for peer in PEERS}
    cpus = config["cpuAffinity"]
    ndn = None; peers: dict[str, Any] = {}; captures = []; runtime_error = ""
    sockets = []
    failure_phase = "INFRA_FAILURE"
    started = time.monotonic_ns(); samples = []
    original_argv = sys.argv
    original_env = os.environ.copy()
    try:
        home = cell / "coordinator"
        home.mkdir(mode=0o700)
        os.environ["HOME"] = str(home)
        for key in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "NDN_CLIENT_CONF", "NDN_CLIENT_PIB",
                    "NDN_CLIENT_TPM", "NDN_CLIENT_TRANSPORT"):
            os.environ.pop(key, None)
        sys.argv = [original_argv[0]]
        setLogLevel("warning"); Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell / "minindn")); ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        nodes = dict(zip(PEERS, (ndn.net[left], ndn.net[right])))
        envs = {}
        for peer, node in nodes.items():
            node_home = Path(node.params["params"]["homeDir"])
            conf = (node_home / ".ndn/client.conf").read_text(encoding="utf-8")
            transport = next(line.partition("=")[2].strip() for line in conf.splitlines()
                             if line.startswith("transport="))
            if not transport.startswith("unix://"):
                raise RuntimeError(f"unexpected NFD transport: {transport}")
            socket = Path(transport[len("unix://"):])
            deadline = time.monotonic() + 15
            while not socket.is_socket() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.is_socket():
                raise RuntimeError(f"NFD socket not ready: {socket}")
            sockets.append((socket, socket.stat().st_ino))
            envs[peer] = {**library_env(Path(config["library"])), "HOME": str(node_home),
                          "NDN_CLIENT_TRANSPORT": transport}
        for peer, other in (PEERS, PEERS[::-1]):
            node, neighbor = nodes[peer], nodes[other]
            with (cell / f"{peer}-routes.txt").open("w", encoding="utf-8") as log:
                output = nfd_command(node, ["face", "create", f"udp4://{neighbor.IP()}:6363"],
                                     envs[peer], log)
                face = re.search(r"\bid=(\d+)\b", output)
                if face is None:
                    raise RuntimeError(f"cannot resolve UDP face on {peer}: {output}")
                for prefix in (sync_prefix, prefixes[other]):
                    nfd_command(node, ["route", "add", prefix, face[1]], envs[peer], log)
                nfd_command(node, ["strategy", "set", sync_prefix,
                                   "/localhost/nfd/strategy/multicast"], envs[peer], log)
            rib = node.cmd(f"NDN_CLIENT_TRANSPORT={shlex.quote(envs[peer]['NDN_CLIENT_TRANSPORT'])} nfdc route list")
            if "/spec132/publication" in rib:
                raise RuntimeError("forbidden concrete publication route detected")
            interface = node.defaultIntf().name
            (cell / f"{peer}-qdisc-before.txt").write_text(
                node.cmd(f"tc -s qdisc show dev {shlex.quote(interface)}"), encoding="utf-8")
            counters = node.cmd(
                f"for f in rx_bytes rx_packets rx_dropped tx_bytes tx_packets tx_dropped; do "
                f"printf '%s=' \"$f\"; cat /sys/class/net/{shlex.quote(interface)}/statistics/$f; done")
            (cell / f"{peer}-network-before.txt").write_text(counters, encoding="utf-8")
        atomic_json(cell / "environment.json", {"cpuAffinity": cpus, "syncPrefix": sync_prefix,
                    "peerPrefixes": prefixes, "ndnsfRuntime": False})
        commands = {}
        for index, (peer, other) in enumerate((PEERS, PEERS[::-1])):
            node = nodes[peer]
            with (cell / f"{peer}-ndndump.log").open("w", encoding="utf-8") as log:
                captures.append(node.popen(["ndndump", "-i", node.defaultIntf().name, "-t", "-v"],
                                          stdout=log, stderr=subprocess.STDOUT))
            commands[peer] = [config["binary"], "--subject", config["subject"],
                              "--sync-prefix", sync_prefix, "--cell-id", config["cellId"],
                              "--peer-id", peer, "--remote-peer-id", other,
                              "--node-prefix", prefixes[peer], "--events", str(cell / f"{peer}.jsonl"),
                              "--rate-pps", str(config["ratePpsPerPeer"]),
                              "--warmup-s", str(config["warmupSeconds"]),
                              "--measure-s", str(config["measureSeconds"]),
                              "--drain-s", str(config["drainSeconds"]),
                              "--start-delay-ms", str(1000 * config["convergeSeconds"]),
                              "--main-cpu", str(cpus[index * 2]), "--face-cpu", str(cpus[index * 2 + 1])]
        atomic_json(cell / "commands.json", commands)
        failure_phase = "SUBJECT_FAILURE"
        for peer, node in nodes.items():
            with (cell / f"{peer}.stdout").open("w", encoding="utf-8") as out, \
                 (cell / f"{peer}.stderr").open("w", encoding="utf-8") as err:
                peers[peer] = node.popen(commands[peer], env=envs[peer], stdout=out, stderr=err)
        for peer, process in peers.items():
            wait_ready(cell / f"{peer}.stdout", process)
        if any(process.poll() is not None for process in captures):
            failure_phase = "INFRA_FAILURE"
            raise RuntimeError("ndndump exited before measurement")
        deadline = time.monotonic() + config["warmupSeconds"] + config["measureSeconds"] + config["drainSeconds"] + 25
        while time.monotonic() < deadline and any(p.poll() is None for p in peers.values()):
            samples.append(process_sample(peers)); time.sleep(1)
        if any(p.poll() is None for p in peers.values()): raise RuntimeError("bounded cell deadline exceeded")
        if any(p.returncode != 0 for p in peers.values()): raise RuntimeError(f"peer return codes: {[(k,p.returncode) for k,p in peers.items()]}")
        summarize_cell(cell, config)
        failure_phase = "INFRA_FAILURE"
        for peer, node in nodes.items():
            (cell / f"{peer}-rib-final.txt").write_text(
                node.cmd(f"NDN_CLIENT_TRANSPORT={shlex.quote(envs[peer]['NDN_CLIENT_TRANSPORT'])} nfdc route list"),
                encoding="utf-8")
            interface = node.defaultIntf().name
            (cell / f"{peer}-qdisc-final.txt").write_text(
                node.cmd(f"tc -s qdisc show dev {shlex.quote(interface)}"), encoding="utf-8")
            counters = node.cmd(
                f"for f in rx_bytes rx_packets rx_dropped tx_bytes tx_packets tx_dropped; do "
                f"printf '%s=' \"$f\"; cat /sys/class/net/{shlex.quote(interface)}/statistics/$f; done")
            (cell / f"{peer}-network-final.txt").write_text(counters, encoding="utf-8")
    except Exception as error:
        runtime_error = f"{type(error).__name__}: {error}"
    finally:
        return_codes = {key: stop(process) for key, process in peers.items()}
        for process in captures: stop(process)
        if ndn is not None:
            try:
                ndn.stop()
                for socket, inode in sockets:
                    if socket.is_socket() and socket.stat().st_ino == inode:
                        socket.unlink()
            except Exception as error:
                if not runtime_error:
                    runtime_error = f"ndn.stop: {error}"
                    failure_phase = "INFRA_FAILURE"
        sys.argv = original_argv
        os.environ.clear()
        os.environ.update(original_env)
    with (cell / "resource-samples.jsonl").open("w", encoding="utf-8") as out:
        for sample in samples: out.write(json.dumps(sample, sort_keys=True) + "\n")
    receipt = {"schemaVersion": "spec132-attempt-v1", "cellId": config["cellId"],
               "attempt": 1, "subject": config["subject"], "startedMonotonicNs": started,
               "endedMonotonicNs": time.monotonic_ns(), "returnCodes": return_codes,
               "status": failure_phase if runtime_error else classify_terminal("", return_codes),
               "error": runtime_error}
    atomic_json(cell / "attempt-receipt.json", receipt)
    (campaign / "receipts").mkdir(exist_ok=True)
    atomic_json(campaign / "receipts" / f"{config['cellId']}.json", receipt)
    return receipt


def preflight(manifest: dict[str, Any]) -> dict[str, Any]:
    cpus = manifest["cpuAffinity"]
    if len(set(cpus)) != 4 or not set(cpus) <= os.sched_getaffinity(0):
        raise RuntimeError(f"four recorded CPUs must be available: {cpus}")
    for executable in ("nfd", "nfdc", "ndndump", "infoconv", "tc"):
        if shutil.which(executable) is None:
            raise RuntimeError(f"missing runtime dependency: {executable}")
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    namespace = Path("/proc/self/timens_offsets").read_text() if Path("/proc/self/timens_offsets").is_file() else ""
    clocks = []
    for subject in manifest["subjects"]:
        values = [int(subprocess.check_output([subject["binary"], "--clock-probe"], text=True,
                                             env=library_env(Path(subject["library"]))))
                  for _ in range(3)]
        if values != sorted(values): raise RuntimeError("CLOCK_MONOTONIC_RAW not monotonic")
        clocks.append({"subject": subject["subject"], "samples": values})
    return {"schemaVersion": "spec132-preflight-v1", "bootId": boot,
            "timeNamespaceOffsets": namespace, "cpuAffinity": cpus,
            "clockSamples": clocks, "freeBytes": os.statvfs(str(REPO)).f_bavail * os.statvfs(str(REPO)).f_frsize}


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    plan = sub.add_parser("plan")
    plan.add_argument("--campaign-id", required=True)
    plan.add_argument("--build-authority", type=Path, required=True)
    plan.add_argument("--output", type=Path, required=True)
    seal = sub.add_parser("seal"); seal.add_argument("campaign", type=Path)
    smoke = sub.add_parser("smoke")
    smoke.add_argument("--build-authority", type=Path, required=True)
    smoke.add_argument("--subject", choices=SUBJECTS, required=True)
    smoke.add_argument("--output", type=Path, required=True)
    smoke.add_argument("--rate-pps", type=int, default=1000)
    smoke.add_argument("--warmup-seconds", type=int, default=1)
    smoke.add_argument("--measure-seconds", type=int, default=5)
    smoke.add_argument("--drain-seconds", type=int, default=2)
    smoke.add_argument("--converge-seconds", type=int, default=2)
    block = sub.add_parser("run-block"); block.add_argument("campaign", type=Path); block.add_argument("--subject", choices=SUBJECTS, required=True)
    args = parser.parse_args()
    if args.command == "plan":
        manifest = make_manifest(args.campaign_id, args.build_authority)
        verify_frozen(manifest)
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False); atomic_json(output / "campaign-manifest.json", manifest)
        print(output); return 0
    if args.command == "seal":
        manifest = json.loads((args.campaign / "campaign-manifest.json").read_text())
        verify_frozen(manifest)
        with (args.campaign / ".campaign.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if (args.campaign / ".sealed").exists():
                raise RuntimeError("campaign is already sealed")
            atomic_json(args.campaign / "preflight.json", preflight(manifest))
            (args.campaign / ".sealed").write_text(sha256_file(args.campaign / "campaign-manifest.json") + "\n")
        print(args.campaign / ".sealed"); return 0
    if args.command == "smoke":
        if min(args.rate_pps, args.measure_seconds) <= 0 or min(
                args.warmup_seconds, args.drain_seconds, args.converge_seconds) < 0:
            parser.error("invalid smoke rate or duration")
        manifest = make_manifest("smoke", args.build_authority)
        verify_frozen(manifest)
        identity = preflight(manifest)
        subject = subject_map(manifest)[args.subject]
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        atomic_json(output / "preflight.json", identity)
        config = {"cellId": f"smoke-{args.subject}-r{args.rate_pps}-{args.measure_seconds}s",
                  "subject": args.subject, "ratePpsPerPeer": args.rate_pps,
                  "aggregateTargetPps": 2 * args.rate_pps,
                  "repetition": 0, "attempt": 1, "warmupSeconds": args.warmup_seconds,
                  "measureSeconds": args.measure_seconds,
                  "drainSeconds": args.drain_seconds,
                  "convergeSeconds": args.converge_seconds, "payloadBytes": 256,
                  "binary": subject["binary"], "binarySha256": subject["binarySha256"],
                  "library": subject["library"], "librarySha256": subject["librarySha256"],
                  "cpuAffinity": manifest["cpuAffinity"]}
        receipt = run_cell(output, config, formal=False)
        cell = output / "cells" / config["cellId"]
        summary = summarize_cell(cell, config) if receipt["status"] == "COMPLETE" else None
        if summary is not None:
            atomic_json(cell / "cell-summary.json", summary)
        admitted = summary is not None and summary["sustainable"]
        print(json.dumps({"receipt": receipt, "summary": summary, "admitted": admitted}))
        return 0 if admitted else 1
    campaign = args.campaign.resolve(); lock_path = campaign / ".campaign.lock"
    with lock_path.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest_path = campaign / "campaign-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        seal_path = campaign / ".sealed"
        if not seal_path.is_file() or sha256_file(manifest_path) != seal_path.read_text().strip():
            raise RuntimeError("campaign is unsealed or its manifest changed")
        verify_frozen(manifest)
        preflight(manifest)
        cells = [c for c in manifest["cells"] if c["subject"] == args.subject]
        if args.subject == SUBJECTS[1]:
            baseline = [c for c in manifest["cells"] if c["subject"] == SUBJECTS[0]]
            if any(not (campaign / "receipts" / f"{c['cellId']}.json").is_file() for c in baseline):
                raise RuntimeError("treatment blocked until all 5 baseline receipts exist")
        outcomes = []
        for cell in cells:
            verify_frozen(manifest)
            receipt_path = campaign / "receipts" / f"{cell['cellId']}.json"
            if receipt_path.is_file():
                receipt = json.loads(receipt_path.read_text())
                if (receipt.get("cellId"), receipt.get("subject"), receipt.get("attempt")) != (
                        cell["cellId"], args.subject, 1):
                    raise RuntimeError(f"receipt identity mismatch: {receipt_path}")
            else:
                receipt = run_cell(campaign, {**cell, "cpuAffinity": manifest["cpuAffinity"]})
            outcomes.append(receipt)
            print(json.dumps(outcomes[-1]), flush=True)
        status = "COMPLETE" if all(x["status"] == "COMPLETE" for x in outcomes) else "INCOMPLETE"
        atomic_json(campaign / f"{args.subject}-block.json", {"subject": args.subject,
                    "receiptCount": len(outcomes), "status": status, "outcomes": outcomes})
        print(f"{args.subject.upper().replace('-', '_')}_{status}")
    return 0 if status == "COMPLETE" else 1


if __name__ == "__main__":
    raise SystemExit(main())
