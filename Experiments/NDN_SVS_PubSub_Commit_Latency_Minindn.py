#!/usr/bin/env python3
"""Spec 131 pure NDN-SVS PubSub MiniNDN campaign authority."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import signal
import subprocess
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[1]
BUILD_AUTHORITY = REPO / "build/spec131/subjects.json"
ANALYZER = REPO / "Experiments/analyze_svs_pubsub_commit_latency.py"
DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-pubsub-bench.cpp"
RATES = (200, 400, 600, 800, 1000)
SUBJECTS = ("baseline-sync-serial", "latest-async-parallel")
SEED = 13120260721
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


def make_manifest(campaign_id: str, authority_path: Path = BUILD_AUTHORITY) -> dict[str, Any]:
    authority = json.loads(authority_path.read_text(encoding="utf-8"))
    subjects = subject_map(authority)
    cells = []
    ordinal = 0
    schedule = matched_schedule()
    for subject in SUBJECTS:
        for rate, repetition in schedule:
            ordinal += 1
            cell_id = f"{ordinal:02d}-{subject}-r{rate}-rep{repetition}"
            cells.append({"ordinal": ordinal, "cellId": cell_id, "subject": subject,
                          "ratePps": rate, "repetition": repetition, "attempt": 1,
                          **FORMAL, "binary": subjects[subject]["binary"],
                          "binarySha256": subjects[subject]["binarySha256"],
                          "librarySha256": subjects[subject]["librarySha256"],
                          "baseCommit": subjects[subject]["baseCommit"]})
    return {
        "schemaVersion": "spec131-campaign-v1", "campaignId": campaign_id,
        "createdUnixNs": time.time_ns(), "formal": True, "automaticRetry": False,
        "seed": SEED, "cpuAffinity": [0, 1, 2, 3],
        "topology": {"hosts": 2, "bandwidthMbps": 100, "oneWayDelayMs": 10,
                     "lossPercent": 0},
        "buildAuthority": str(authority_path.resolve()),
        "buildAuthoritySha256": sha256_file(authority_path),
        "canonicalPatchSha256": authority["canonicalPatchSha256"],
        "runnerSha256": sha256_file(Path(__file__)), "analyzerSha256": sha256_file(ANALYZER),
        "driverSha256": sha256_file(DRIVER), "subjects": authority["subjects"], "cells": cells,
    }


def verify_frozen(campaign: Path, manifest: dict[str, Any]) -> None:
    if sha256_file(Path(manifest["buildAuthority"])) != manifest["buildAuthoritySha256"]:
        raise RuntimeError("build authority drift after campaign sealing")
    for path, expected in ((Path(__file__), manifest["runnerSha256"]),
                           (ANALYZER, manifest["analyzerSha256"]),
                           (DRIVER, manifest["driverSha256"])):
        if sha256_file(path) != expected:
            raise RuntimeError(f"formal source drift: {path}")
    for subject in manifest["subjects"]:
        binary, library = Path(subject["binary"]), Path(subject["library"])
        if sha256_file(binary) != subject["binarySha256"] or sha256_file(library) != subject["librarySha256"]:
            raise RuntimeError(f"frozen binary/library drift: {subject['subject']}")
        if "ndnsf" in subprocess.check_output(["ldd", str(binary)], text=True).lower():
            raise RuntimeError("NDNSF dependency in pure NDN-SVS subject")


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
        if path.is_file() and "SPEC131_READY" in path.read_text(encoding="utf-8", errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError(f"peer exited before READY: rc={process.returncode}")
        time.sleep(0.1)
    raise RuntimeError("peer READY timeout")


def process_sample(processes: dict[str, Any]) -> dict[str, Any]:
    sample = {"monotonicNs": time.monotonic_ns(), "processes": {}}
    for role, process in processes.items():
        candidates = [process.pid]
        cursor = 0
        while cursor < len(candidates):
            children = Path(f"/proc/{candidates[cursor]}/task/{candidates[cursor]}/children")
            if children.is_file():
                candidates.extend(int(value) for value in children.read_text().split()
                                  if int(value) not in candidates)
            cursor += 1
        peer_pids = []
        for candidate in candidates:
            cmdline = Path(f"/proc/{candidate}/cmdline")
            if cmdline.is_file() and "svs-pubsub-bench" in cmdline.read_bytes().replace(b"\0", b" ").decode(errors="replace"):
                peer_pids.append(candidate)
        pid = peer_pids[-1] if peer_pids else process.pid
        status = Path(f"/proc/{pid}/status")
        stat = Path(f"/proc/{pid}/stat")
        entry: dict[str, Any] = {"pid": pid, "wrapperPid": process.pid,
                                 "peerResolved": bool(peer_pids)}
        if status.is_file():
            for line in status.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith(("VmRSS:", "Threads:")):
                    key, value = line.split(":", 1); entry[key] = value.strip()
        if stat.is_file():
            fields = stat.read_text().split()
            entry["cpuTicks"] = int(fields[13]) + int(fields[14])
        sample["processes"][role] = entry
    return sample


def run_cell(campaign: Path, config: dict[str, Any], *, formal: bool = True) -> dict[str, Any]:
    original_argv = list(sys.argv); sys.argv = [sys.argv[0]]
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
    topology.write_text("[nodes]\npublisher:\nsubscriber:\n\n[links]\n"
                        "publisher:subscriber delay=10ms bw=100 loss=0\n", encoding="utf-8")
    sync_prefix = f"/spec131/sync/{config['cellId']}"
    pub_prefix = f"/spec131/publisher/{config['cellId']}"
    sub_prefix = f"/spec131/subscriber/{config['cellId']}"
    ndn = None; peers: dict[str, Any] = {}; captures = []; runtime_error = ""
    started = time.monotonic_ns(); samples = []
    try:
        setLogLevel("warning"); Minindn.cleanUp(); Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell / "minindn")); ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        pub, sub = ndn.net["publisher"], ndn.net["subscriber"]
        transports = {"publisher": "unix:///run/nfd/publisher.sock",
                      "subscriber": "unix:///run/nfd/subscriber.sock"}
        for node in (pub, sub):
            socket = Path(f"/run/nfd/{node.name}.sock"); deadline = time.monotonic() + 15
            while not socket.exists() and time.monotonic() < deadline: time.sleep(0.1)
            if not socket.exists(): raise RuntimeError(f"NFD socket not ready: {socket}")
        for node, neighbor, remote_prefix in ((pub, sub, sub_prefix), (sub, pub, pub_prefix)):
            env = f"NDN_CLIENT_TRANSPORT={transports[node.name]}"
            route_outputs = []
            for prefix in (sync_prefix, remote_prefix):
                route_outputs.append(node.cmd(f"{env} nfdc route add {shlex.quote(prefix)} udp4://{neighbor.IP()}:6363"))
            node.cmd(f"{env} nfdc strategy set {shlex.quote(sync_prefix)} /localhost/nfd/strategy/multicast")
            (cell / f"{node.name}-routes.txt").write_text("\n".join(route_outputs), encoding="utf-8")
            rib = node.cmd(f"{env} nfdc route list")
            if "/spec131/publication" in rib:
                raise RuntimeError("forbidden concrete publication route detected")
            interface = node.defaultIntf().name
            (cell / f"{node.name}-qdisc-before.txt").write_text(
                node.cmd(f"tc -s qdisc show dev {shlex.quote(interface)}"), encoding="utf-8")
            counters = node.cmd(
                f"for f in rx_bytes rx_packets rx_dropped tx_bytes tx_packets tx_dropped; do "
                f"printf '%s=' \"$f\"; cat /sys/class/net/{shlex.quote(interface)}/statistics/$f; done")
            (cell / f"{node.name}-network-before.txt").write_text(counters, encoding="utf-8")
        atomic_json(cell / "environment.json", {"cpuAffinity": [0, 1, 2, 3],
                    "transports": transports, "syncPrefix": sync_prefix,
                    "publisherPrefix": pub_prefix, "subscriberPrefix": sub_prefix,
                    "ndnsfRuntime": False})
        for node in (pub, sub):
            captures.append(node.popen(f"ndndump -i {node.defaultIntf().name} -t -v >{shlex.quote(str(cell / (node.name + '-ndndump.log')))} 2>&1", shell=True))
        binary = Path(config["binary"]); worktree = next(Path(s["worktree"]) for s in
            json.loads(BUILD_AUTHORITY.read_text(encoding="utf-8"))["subjects"] if s["subject"] == config["subject"])
        common = (f"taskset -c 0-3 env LD_LIBRARY_PATH={shlex.quote(str(worktree / 'build'))} "
                  f"NDN_CLIENT_TRANSPORT={{transport}} {shlex.quote(str(binary))} "
                  f"--subject {config['subject']} --sync-prefix {shlex.quote(sync_prefix)} "
                  f"--cell-id {shlex.quote(config['cellId'])} --rate-pps {config['ratePps']} "
                  f"--warmup-s {config['warmupSeconds']} --measure-s {config['measureSeconds']} "
                  f"--drain-s {config['drainSeconds']} --start-delay-ms 1000")
        sub_cmd = (common.format(transport=transports["subscriber"]) +
                   f" --role subscriber --node-prefix {shlex.quote(sub_prefix)} --peer-prefix {shlex.quote(pub_prefix)}"
                   f" --events {shlex.quote(str(cell / 'subscriber.jsonl'))} >{shlex.quote(str(cell / 'subscriber.stdout'))} 2>{shlex.quote(str(cell / 'subscriber.stderr'))}")
        pub_cmd = (common.format(transport=transports["publisher"]) +
                   f" --role publisher --node-prefix {shlex.quote(pub_prefix)} --peer-prefix {shlex.quote(sub_prefix)}"
                   f" --events {shlex.quote(str(cell / 'publisher.jsonl'))} >{shlex.quote(str(cell / 'publisher.stdout'))} 2>{shlex.quote(str(cell / 'publisher.stderr'))}")
        atomic_json(cell / "commands.json", {"subscriber": sub_cmd, "publisher": pub_cmd})
        peers["subscriber"] = sub.popen(sub_cmd, shell=True); wait_ready(cell / "subscriber.stdout", peers["subscriber"])
        time.sleep(config.get("convergeSeconds", 5))
        peers["publisher"] = pub.popen(pub_cmd, shell=True); wait_ready(cell / "publisher.stdout", peers["publisher"])
        deadline = time.monotonic() + config["warmupSeconds"] + config["measureSeconds"] + config["drainSeconds"] + 25
        while time.monotonic() < deadline and any(p.poll() is None for p in peers.values()):
            samples.append(process_sample(peers)); time.sleep(1)
        if any(p.poll() is None for p in peers.values()): raise RuntimeError("bounded cell deadline exceeded")
        if any(p.returncode != 0 for p in peers.values()): raise RuntimeError(f"peer return codes: {[(k,p.returncode) for k,p in peers.items()]}")
        for node in (pub, sub):
            (cell / f"{node.name}-rib-final.txt").write_text(
                node.cmd(f"NDN_CLIENT_TRANSPORT={transports[node.name]} nfdc route list"), encoding="utf-8")
            interface = node.defaultIntf().name
            (cell / f"{node.name}-qdisc-final.txt").write_text(
                node.cmd(f"tc -s qdisc show dev {shlex.quote(interface)}"), encoding="utf-8")
            counters = node.cmd(
                f"for f in rx_bytes rx_packets rx_dropped tx_bytes tx_packets tx_dropped; do "
                f"printf '%s=' \"$f\"; cat /sys/class/net/{shlex.quote(interface)}/statistics/$f; done")
            (cell / f"{node.name}-network-final.txt").write_text(counters, encoding="utf-8")
    except Exception as error:
        runtime_error = f"{type(error).__name__}: {error}"
    finally:
        return_codes = {key: stop(process) for key, process in peers.items()}
        for process in captures: stop(process)
        if ndn is not None:
            try: ndn.stop()
            except Exception as error: runtime_error = runtime_error or f"ndn.stop: {error}"
        try: Minindn.cleanUp()
        except Exception as error: runtime_error = runtime_error or f"cleanup: {error}"
        sys.argv = original_argv
    with (cell / "resource-samples.jsonl").open("w", encoding="utf-8") as out:
        for sample in samples: out.write(json.dumps(sample, sort_keys=True) + "\n")
    receipt = {"schemaVersion": "spec131-attempt-v1", "cellId": config["cellId"],
               "attempt": 1, "subject": config["subject"], "startedMonotonicNs": started,
               "endedMonotonicNs": time.monotonic_ns(), "returnCodes": return_codes,
               "status": "COMPLETE" if not runtime_error else "FAILED", "error": runtime_error}
    atomic_json(cell / "attempt-receipt.json", receipt)
    (campaign / "receipts").mkdir(exist_ok=True)
    atomic_json(campaign / "receipts" / f"{config['cellId']}.json", receipt)
    return receipt


def preflight(manifest: dict[str, Any]) -> dict[str, Any]:
    if sorted(os.sched_getaffinity(0)) != [0, 1, 2, 3]:
        raise RuntimeError(f"unexpected CPU affinity: {sorted(os.sched_getaffinity(0))}")
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    namespace = Path("/proc/self/timens_offsets").read_text() if Path("/proc/self/timens_offsets").is_file() else ""
    clocks = []
    for subject in manifest["subjects"]:
        values = [int(subprocess.check_output([subject["binary"], "--clock-probe"], text=True)) for _ in range(3)]
        if values != sorted(values): raise RuntimeError("CLOCK_MONOTONIC_RAW not monotonic")
        clocks.append({"subject": subject["subject"], "samples": values})
    return {"schemaVersion": "spec131-preflight-v1", "bootId": boot,
            "timeNamespaceOffsets": namespace, "cpuAffinity": sorted(os.sched_getaffinity(0)),
            "clockSamples": clocks, "freeBytes": os.statvfs(str(REPO)).f_bavail * os.statvfs(str(REPO)).f_frsize}


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    plan = sub.add_parser("plan"); plan.add_argument("--campaign-id", required=True); plan.add_argument("--output", type=Path)
    seal = sub.add_parser("seal"); seal.add_argument("campaign", type=Path)
    smoke = sub.add_parser("smoke")
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
        manifest = make_manifest(args.campaign_id)
        output = args.output or REPO / "results/spec131-svs-pubsub-commit-latency" / args.campaign_id
        output.mkdir(parents=True, exist_ok=False); atomic_json(output / "campaign-manifest.json", manifest)
        print(output); return 0
    if args.command == "seal":
        manifest = json.loads((args.campaign / "campaign-manifest.json").read_text())
        verify_frozen(args.campaign, manifest); atomic_json(args.campaign / "preflight.json", preflight(manifest))
        (args.campaign / ".sealed").write_text(sha256_file(args.campaign / "campaign-manifest.json") + "\n")
        print(args.campaign / ".sealed"); return 0
    if args.command == "smoke":
        authority = json.loads(BUILD_AUTHORITY.read_text()); subject = subject_map(authority)[args.subject]
        args.output.mkdir(parents=True, exist_ok=False)
        config = {"cellId": f"smoke-{args.subject}-r{args.rate_pps}-{args.measure_seconds}s",
                  "subject": args.subject, "ratePps": args.rate_pps,
                  "repetition": 0, "attempt": 1, "warmupSeconds": args.warmup_seconds,
                  "measureSeconds": args.measure_seconds,
                  "drainSeconds": args.drain_seconds,
                  "convergeSeconds": args.converge_seconds, "payloadBytes": 256,
                  "binary": subject["binary"], "binarySha256": subject["binarySha256"]}
        receipt = run_cell(args.output, config, formal=False)
        cell = args.output / "cells" / config["cellId"]
        subprocess.run([sys.executable, str(ANALYZER), str(args.output), "--cell", str(cell),
                        "--config", str(cell / "cell-config.json")], check=True)
        summary = json.loads((cell / "cell-summary.json").read_text(encoding="utf-8"))
        adapter = summary.get("adapterMetrics") or {}
        admitted = (receipt["status"] == "COMPLETE" and not summary["senderLimited"] and
                    adapter.get("accountingReconciled") is True and
                    adapter.get("rejected") == 0 and adapter.get("apiFailures") == 0 and
                    adapter.get("remainingAtShutdown") == 0)
        print(json.dumps({"receipt": receipt, "summary": summary, "admitted": admitted}))
        return 0 if admitted else 1
    campaign = args.campaign.resolve(); lock_path = campaign / ".campaign.lock"
    with lock_path.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest = json.loads((campaign / "campaign-manifest.json").read_text())
        if not (campaign / ".sealed").is_file(): raise RuntimeError("campaign not sealed")
        verify_frozen(campaign, manifest)
        cells = [c for c in manifest["cells"] if c["subject"] == args.subject]
        if args.subject == SUBJECTS[1]:
            baseline = [c for c in manifest["cells"] if c["subject"] == SUBJECTS[0]]
            if any(not (campaign / "receipts" / f"{c['cellId']}.json").is_file() for c in baseline):
                raise RuntimeError("treatment blocked until all 5 baseline receipts exist")
        outcomes = []
        for cell in cells:
            verify_frozen(campaign, manifest)
            outcomes.append(run_cell(campaign, cell, formal=True))
            print(json.dumps(outcomes[-1]), flush=True)
        status = "COMPLETE" if all(x["status"] == "COMPLETE" for x in outcomes) else "INCOMPLETE"
        atomic_json(campaign / f"{args.subject}-block.json", {"subject": args.subject,
                    "receiptCount": len(outcomes), "status": status, "outcomes": outcomes})
        print(f"{args.subject.upper().replace('-', '_')}_{status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
