#!/usr/bin/env python3
"""Once-only MiniNDN diagnostics and repair qualification for Spec 134."""

from __future__ import annotations

import argparse
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
DEFAULT_SUBJECT = REPO / "build/spec134/subject-manifest.json"
DEFAULT_IO_SUBJECT = REPO / "build/spec134/io-qualification-manifest.json"
IO_SCHEMA = "spec134-io-qualification-manifest-v1"
PEERS = ("peer-a", "peer-b")
DIAGNOSTIC = {"ratePpsPerPeer": 1000, "payloadBytes": 256,
              "warmupSeconds": 1, "measureSeconds": 5, "drainSeconds": 2}
QUALIFICATION = {"ratePpsPerPeer": 1000, "payloadBytes": 256,
                 "warmupSeconds": 10, "measureSeconds": 60, "drainSeconds": 10}
CORRUPTION_PATTERNS = (
    r"ERROR: AddressSanitizer", r"runtime error:", r"WARNING: ThreadSanitizer",
    r"SUMMARY: (?:AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer)",
    r"malloc_consolidate", r"corrupted (?:size|double-linked|unsorted)",
    r"double free", r"invalid pointer", r"std::bad_alloc", r"Assertion .* failed",
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
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def load_subject(path: Path) -> dict[str, Any]:
    subject = json.loads(path.read_text(encoding="utf-8"))
    if subject.get("schemaVersion") not in {
            "spec134-subject-manifest-v1", "spec134-repair-manifest-v1", IO_SCHEMA}:
        raise RuntimeError("Spec 134 subject manifest schema mismatch")
    if subject.get("publishApi") != "publish" or subject.get("parallelWorkers") is not None:
        raise RuntimeError("subject is not direct synchronous publish")
    for mode, item in subject.get("subjects", {}).items():
        for key in ("binary", "library"):
            artifact = Path(item[key])
            if not artifact.is_file() or sha256_file(artifact) != item[f"{key}Sha256"]:
                raise RuntimeError(f"subject artifact drift: {mode}/{key}")
        linkage = Path(item["binaryLdd"]).read_text(encoding="utf-8")
        if "libndn-svs.so" not in linkage or "1.74" in linkage:
            raise RuntimeError(f"invalid linkage: {mode}")
        boost_rows = [line for line in linkage.splitlines() if "libboost_" in line]
        if not boost_rows or any(".1.71.0" not in line for line in boost_rows):
            raise RuntimeError(f"non-Boost-1.71 subject: {mode}")
    return subject


def stop(process: Any, grace: float = 3.0) -> int | None:
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


def wait_ready(path: Path, process: Any, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and "SPEC134_READY" in path.read_text(
                encoding="utf-8", errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError(f"peer exited before READY: rc={process.returncode}")
        time.sleep(0.1)
    raise RuntimeError("peer READY timeout")


def sanitizer_environment(mode: str, cell: Path, peer: str) -> dict[str, str]:
    if mode == "asan-ubsan":
        return {
            "ASAN_OPTIONS": f"abort_on_error=1:halt_on_error=1:detect_leaks=0:log_path={cell / (peer + '-asan')}",
            "UBSAN_OPTIONS": f"halt_on_error=1:print_stacktrace=1:log_path={cell / (peer + '-ubsan')}",
        }
    if mode == "tsan":
        return {
            "TSAN_OPTIONS": f"halt_on_error=1:history_size=7:second_deadlock_stack=1:log_path={cell / (peer + '-tsan')}",
        }
    return {}


def scan_corruption(cell: Path) -> list[dict[str, str]]:
    findings = []
    candidates = sorted(cell.glob("peer-*.stderr"))
    candidates += sorted(cell.glob("peer-*-asan*"))
    candidates += sorted(cell.glob("peer-*-ubsan*"))
    candidates += sorted(cell.glob("peer-*-tsan*"))
    for path in candidates:
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in CORRUPTION_PATTERNS:
            match = re.search(pattern, text, re.I)
            if match:
                findings.append({"path": str(path), "pattern": pattern,
                                 "match": match.group(0)})
    return findings


def final_counters(path: Path) -> tuple[bool, dict[str, int]]:
    if not path.is_file():
        return False, {}
    last: dict[str, Any] | None = None
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "process-stop":
            last = event
    if last is None:
        return False, {}
    details = last.get("details", {})
    fields = (
        "scheduledMeasured",
        "attemptedMeasured",
        "missedReleaseMeasured",
        "deliveredMeasured",
        "localDeliveryIgnored",
        "invalidRemoteMeasured",
        "invalidMeasured",
        "publishErrors",
        "latenessP50Ns",
        "latenessP95Ns",
        "latenessMaxNs",
    )
    return True, {key: int(details.get(key, 0)) for key in fields}


def run_once(subject_path: Path, output: Path, mode: str,
             config: dict[str, int], purpose: str) -> dict[str, Any]:
    if output.exists():
        raise RuntimeError(f"attempt path already exists: {output}")
    subject = load_subject(subject_path)
    io_mode = subject.get("schemaVersion") == IO_SCHEMA
    if io_mode and subject.get("executionModel") != "single-face-io-thread":
        raise RuntimeError("I/O qualification subject execution model mismatch")
    if mode not in subject["subjects"]:
        raise RuntimeError(f"subject mode not frozen: {mode}")
    item = subject["subjects"][mode]
    output.mkdir(parents=True, exist_ok=False)
    cell = output / "cell"
    cell.mkdir()
    topology = cell / "topology.conf"
    topology.write_text("[nodes]\npeer-a:\npeer-b:\n\n[links]\n"
                        "peer-a:peer-b delay=10ms bw=100 loss=0\n", encoding="utf-8")
    exact = {"schemaVersion": "spec134-attempt-config-v1", "purpose": purpose,
             "attempt": 1, "automaticRetry": False, "mode": mode,
             "subjectManifest": str(subject_path.resolve()),
             "subjectManifestSha256": sha256_file(subject_path), **config,
             "topology": {"hosts": 2, "oneWayDelayMs": 10,
                          "bandwidthMbps": 100, "lossPercent": 0}}
    atomic_json(cell / "config.json", exact)

    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    ndn = None
    processes: dict[str, Any] = {}
    commands: dict[str, str] = {}
    runtime_error = ""
    infrastructure_stage = True
    started = time.monotonic_ns()
    try:
        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        hosts = {peer: ndn.net[peer] for peer in PEERS}
        transports = {peer: f"unix:///run/nfd/{peer}.sock" for peer in PEERS}
        sync_prefix = f"/spec134/sync/{output.name}"
        prefixes = {peer: f"/spec134/{peer}/{output.name}" for peer in PEERS}
        for peer, host in hosts.items():
            socket = Path(f"/run/nfd/{peer}.sock")
            deadline = time.monotonic() + 15
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")
            other = hosts["peer-b" if peer == "peer-a" else "peer-a"]
            env = f"NDN_CLIENT_TRANSPORT={transports[peer]}"
            for prefix in (sync_prefix, prefixes[other.name]):
                host.cmd(f"{env} nfdc route add {shlex.quote(prefix)} udp4://{other.IP()}:6363")
            host.cmd(f"{env} nfdc strategy set {shlex.quote(sync_prefix)} /localhost/nfd/strategy/multicast")

        cpu_pairs = {"peer-a": (0, 1), "peer-b": (2, 3)}
        other_peer = {"peer-a": "peer-b", "peer-b": "peer-a"}
        for peer in PEERS:
            main_cpu, face_cpu = cpu_pairs[peer]
            environment = sanitizer_environment(mode, cell, peer)
            environment["NDN_CLIENT_TRANSPORT"] = transports[peer]
            env_text = " ".join(f"{key}={shlex.quote(value)}"
                                for key, value in environment.items())
            common = (
                f"env LD_LIBRARY_PATH={shlex.quote(str(Path(item['library']).parent))} "
                f"{env_text} {shlex.quote(item['binary'])} "
                f"--sync-prefix {shlex.quote(sync_prefix)} "
                f"--node-prefix {shlex.quote(prefixes[peer])} "
                f"--cell-id {shlex.quote(output.name)} "
                f"--peer-id {peer} --remote-peer-id {other_peer[peer]} "
                f"--rate-pps {config['ratePpsPerPeer']} "
                f"--warmup-s {config['warmupSeconds']} "
                f"--measure-s {config['measureSeconds']} "
                f"--drain-s {config['drainSeconds']} --start-delay-ms 2000 "
            )
            if io_mode:
                command = f"taskset -c {main_cpu} {common}--io-cpu {main_cpu} "
            else:
                command = (
                    f"taskset -c {main_cpu},{face_cpu} {common}"
                    f"--main-cpu {main_cpu} --face-cpu {face_cpu} "
                )
            command += (
                f"--events {shlex.quote(str(cell / (peer + '-events.jsonl')))} "
                f">{shlex.quote(str(cell / (peer + '.stdout')))} "
                f"2>{shlex.quote(str(cell / (peer + '.stderr')))}"
            )
            commands[peer] = command
        atomic_json(cell / "commands.json", commands)
        infrastructure_stage = False
        for peer in PEERS:
            processes[peer] = hosts[peer].popen(commands[peer], shell=True)
        for peer in PEERS:
            wait_ready(cell / f"{peer}.stdout", processes[peer])
        deadline = time.monotonic() + config["warmupSeconds"] + \
            config["measureSeconds"] + config["drainSeconds"] + 40
        while time.monotonic() < deadline and any(
                process.poll() is None for process in processes.values()):
            time.sleep(0.5)
        if any(process.poll() is None for process in processes.values()):
            raise RuntimeError("bounded attempt deadline exceeded")
        if any(process.returncode != 0 for process in processes.values()):
            raise RuntimeError(f"peer return codes: {[(k, p.returncode) for k, p in processes.items()]}")
    except Exception as error:
        runtime_error = f"{type(error).__name__}: {error}"
    finally:
        return_codes = {peer: stop(processes.get(peer)) for peer in PEERS}
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as error:
                runtime_error = runtime_error or f"ndn.stop: {error}"
        try:
            Minindn.cleanUp()
        except Exception as error:
            runtime_error = runtime_error or f"cleanup: {error}"
        sys.argv = original_argv

    counters: dict[str, dict[str, int]] = {}
    rates: dict[str, dict[str, float]] = {}
    flush_complete: dict[str, bool] = {}
    for peer in PEERS:
        complete, values = final_counters(cell / f"{peer}-events.jsonl")
        flush_complete[peer] = complete
        counters[peer] = values
        attempted = values.get("attemptedMeasured", 0)
        delivered = values.get("deliveredMeasured", 0)
        rates[peer] = {
            "attemptedPps": attempted / config["measureSeconds"],
            "deliveredPps": delivered / config["measureSeconds"],
            "deliveryAttemptedRatio": delivered / attempted if attempted else 0.0,
        }
    corruption = scan_corruption(cell)
    status = "COMPLETE"
    if runtime_error:
        status = "INFRA_FAILURE" if infrastructure_stage else "SUBJECT_FAILURE"
    receipt = {
        "schemaVersion": "spec134-terminal-receipt-v1", "purpose": purpose,
        "mode": mode, "attempt": 1, "automaticRetry": False,
        "executionModel": subject.get("executionModel", "historical-cross-thread"),
        "startedMonotonicNs": started, "endedMonotonicNs": time.monotonic_ns(),
        "returnCodes": return_codes, "eventFlushComplete": flush_complete,
        "counters": counters, "rates": rates, "corruptionFindings": corruption,
        "status": status, "error": runtime_error,
        "subjectManifestSha256": sha256_file(subject_path),
        "driverSha256": subject.get("driverSha256"),
        "binarySha256": item["binarySha256"], "librarySha256": item["librarySha256"],
        "binaryLdd": item["binaryLdd"], "libraryLdd": item["libraryLdd"],
    }
    if purpose in {"qualification", "io-qualification"}:
        qualified = status == "COMPLETE" and all(code == 0 for code in return_codes.values())
        qualified &= all(flush_complete.values()) and not corruption
        for peer in PEERS:
            values = counters.get(peer, {})
            qualified &= values.get("attemptedMeasured", 0) > 0
            qualified &= values.get("deliveredMeasured", 0) > 0
            invalid_key = "invalidRemoteMeasured" if io_mode else "invalidMeasured"
            qualified &= values.get(invalid_key, 1) == 0
            qualified &= values.get("publishErrors", 1) == 0
        receipt["verdict"] = "QUALIFIED" if qualified else "NOT_QUALIFIED"
    atomic_json(output / "terminal-receipt.json", receipt)
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    diagnose = sub.add_parser("diagnose")
    diagnose.add_argument("--subject-manifest", type=Path, default=DEFAULT_SUBJECT)
    diagnose.add_argument("--mode", choices=("asan-ubsan", "tsan"), required=True)
    diagnose.add_argument("--output", type=Path, required=True)
    qualify = sub.add_parser("qualify")
    qualify.add_argument("--subject-manifest", type=Path, default=DEFAULT_SUBJECT)
    qualify.add_argument("--mode", default="repaired-normal")
    qualify.add_argument("--output", type=Path, required=True)
    qualify.add_argument("--target-pps", type=int, default=1000)
    qualify.add_argument("--payload-bytes", type=int, default=256)
    qualify.add_argument("--warmup", type=int, default=10)
    qualify.add_argument("--measure", type=int, default=60)
    qualify.add_argument("--drain", type=int, default=10)
    qualify_io = sub.add_parser("qualify-io")
    qualify_io.add_argument("--subject-manifest", type=Path, default=DEFAULT_IO_SUBJECT)
    qualify_io.add_argument("--mode", default="io-qualification-normal")
    qualify_io.add_argument("--output", type=Path, required=True)
    qualify_io.add_argument("--target-pps", type=int, default=1000)
    qualify_io.add_argument("--payload-bytes", type=int, default=256)
    qualify_io.add_argument("--warmup", type=int, default=10)
    qualify_io.add_argument("--measure", type=int, default=60)
    qualify_io.add_argument("--drain", type=int, default=10)
    args = parser.parse_args()
    if args.command == "diagnose":
        receipt = run_once(args.subject_manifest.resolve(), args.output.resolve(),
                           args.mode, dict(DIAGNOSTIC), "diagnostic")
        print(json.dumps(receipt, sort_keys=True))
        return 0 if receipt["status"] == "COMPLETE" else 1
    requested = {"ratePpsPerPeer": args.target_pps, "payloadBytes": args.payload_bytes,
                 "warmupSeconds": args.warmup, "measureSeconds": args.measure,
                 "drainSeconds": args.drain}
    if requested != QUALIFICATION:
        raise RuntimeError(f"qualification contract changed: {requested}")
    schema = json.loads(args.subject_manifest.read_text(encoding="utf-8")).get(
        "schemaVersion"
    )
    if args.command == "qualify-io":
        if schema != IO_SCHEMA or args.mode != "io-qualification-normal":
            raise RuntimeError("qualify-io requires the clean I/O qualification subject")
        purpose = "io-qualification"
    else:
        if schema == IO_SCHEMA:
            raise RuntimeError("use qualify-io for the single-I/O-thread subject")
        purpose = "qualification"
    receipt = run_once(args.subject_manifest.resolve(), args.output.resolve(),
                       args.mode, requested, purpose)
    print(json.dumps(receipt, sort_keys=True))
    return 0 if receipt.get("verdict") == "QUALIFIED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
