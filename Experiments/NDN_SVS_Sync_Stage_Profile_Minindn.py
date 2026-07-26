#!/usr/bin/env python3
"""Spec 133 MiniNDN runner and immutable evidence authority."""

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
DEFAULT_SUBJECT = REPO / "build/spec133/subject-manifest-io.json"
DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-stage-profile.cpp"
RATES = (200, 400, 600, 800, 1000)
PEERS = ("peer-a", "peer-b")
FORMAL = {"warmupSeconds": 10, "measureSeconds": 60, "drainSeconds": 10,
          "payloadBytes": 256, "lossPercent": 0, "oneWayDelayMs": 10,
          "bandwidthMbps": 100}
PREFLIGHT = {"warmupSeconds": 1, "measureSeconds": 5, "drainSeconds": 2,
             "ratePpsPerPeer": 1000, "payloadBytes": 256}
ARMS = (
    ("A-clean-control", "clean", "cleanBinary", "cleanLibrary"),
    ("B-profiled-disabled", "disabled", "profiledBinary", "profiledLibrary"),
    ("C-profiled-enabled", "enabled", "profiledBinary", "profiledLibrary"),
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
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_subject(path: Path) -> dict[str, Any]:
    subject = load_json(path)
    if subject.get("schemaVersion") not in {
            "spec133-subject-manifest-v1", "spec133-subject-manifest-io-v2"}:
        raise RuntimeError("subject manifest schema mismatch")
    for key in ("cleanBinary", "cleanLibrary", "profiledBinary", "profiledLibrary"):
        artifact = Path(str(subject.get(key, "")))
        expected = subject.get(f"{key}Sha256")
        if not artifact.is_file() or sha256_file(artifact) != expected:
            raise RuntimeError(f"subject artifact missing or changed: {key}")
    if subject.get("publishApi") != "publish" or subject.get("parallelWorkers") is not None:
        raise RuntimeError("subject is not synchronous serial NDN-SVS")
    if subject.get("compressionEnabled") is not False:
        raise RuntimeError("subject compression must be disabled")
    if subject.get("schemaVersion") == "spec133-subject-manifest-io-v2" and \
       subject.get("executionModel") != "single-face-io-thread":
        raise RuntimeError("single-I/O subject execution model mismatch")
    return subject


def profile_environment(mode: str, cell_id: str, peer_id: str,
                        subject: dict[str, Any]) -> dict[str, str]:
    environment = {
        "NDN_SVS_PROFILE_ENABLED": "1" if mode == "enabled" else "0",
        "NDN_SVS_PROFILE_CELL_ID": cell_id,
        "NDN_SVS_PROFILE_PEER_ID": peer_id,
        "NDN_SVS_PROFILE_SAMPLE_MODULUS": str(subject["profileConfig"]["sampleModulus"]),
    }
    if mode == "enabled":
        environment["NDN_LOG"] = subject["profileConfig"]["logger"]
    return environment


def make_manifest(campaign_id: str, subject_path: Path,
                  overhead_path: Path) -> dict[str, Any]:
    subject = load_subject(subject_path)
    if subject.get("schemaVersion") != "spec133-subject-manifest-io-v2":
        raise RuntimeError("formal planning requires the single-I/O subject manifest")
    overhead = load_json(overhead_path)
    if overhead.get("schemaVersion") != "spec133-overhead-admission-v1" or \
       overhead.get("verdict") != "ADMITTED":
        raise RuntimeError("formal planning requires an admitted overhead receipt")
    if overhead.get("subjectManifestSha256") != sha256_file(subject_path):
        raise RuntimeError("overhead receipt belongs to another subject manifest")
    cells = []
    for ordinal, rate in enumerate(RATES, 1):
        cell_id = f"{ordinal:02d}-sync-profile-{rate}"
        cells.append({
            "ordinal": ordinal, "cellId": cell_id, "ratePpsPerPeer": rate,
            "aggregateTargetPps": rate * 2, "peers": list(PEERS), "attempt": 1,
            "profileMode": "enabled", "binary": subject["profiledBinary"],
            "binarySha256": subject["profiledBinarySha256"],
            "library": subject["profiledLibrary"],
            "librarySha256": subject["profiledLibrarySha256"], **FORMAL,
        })
    manifest = {
        "schemaVersion": "spec133-campaign-manifest-v1", "campaignId": campaign_id,
        "createdUnixNs": time.time_ns(), "formal": True, "automaticRetry": False,
        "subjectManifest": str(subject_path.resolve()),
        "subjectManifestSha256": sha256_file(subject_path),
        "overheadReceipt": str(overhead_path.resolve()),
        "overheadReceiptSha256": sha256_file(overhead_path),
        "runnerSha256": sha256_file(Path(__file__)), "driverSha256": sha256_file(DRIVER),
        "cpuAffinity": [0, 2], "topology": {"hosts": 2, **FORMAL},
        "executionModel": "single-face-io-thread",
        "routeContract": "one-face-id-two-verified-fib-routes",
        "profileConfig": subject["profileConfig"], "cells": cells,
    }
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: dict[str, Any]) -> None:
    cells = manifest.get("cells", [])
    if manifest.get("automaticRetry") is not False or len(cells) != 5:
        raise RuntimeError("formal manifest must contain exactly five once-only cells")
    if [cell.get("ratePpsPerPeer") for cell in cells] != list(RATES):
        raise RuntimeError("formal rates or order changed")
    if [cell.get("ordinal") for cell in cells] != list(range(1, 6)):
        raise RuntimeError("formal ordinals changed")
    ids = [cell.get("cellId") for cell in cells]
    if len(set(ids)) != 5 or any(cell.get("attempt") != 1 for cell in cells):
        raise RuntimeError("duplicate cell identity or retry attempt")
    for cell in cells:
        if cell.get("peers") != list(PEERS) or cell.get("profileMode") != "enabled":
            raise RuntimeError("formal peer/profile configuration changed")
        for key, expected in FORMAL.items():
            if cell.get(key) != expected:
                raise RuntimeError(f"formal setting changed: {key}")


def verify_frozen(manifest: dict[str, Any]) -> dict[str, Any]:
    validate_manifest(manifest)
    for key in ("subjectManifest", "overheadReceipt"):
        path = Path(manifest[key])
        if sha256_file(path) != manifest[f"{key}Sha256"]:
            raise RuntimeError(f"sealed authority drift: {key}")
    if sha256_file(Path(__file__)) != manifest["runnerSha256"] or \
       sha256_file(DRIVER) != manifest["driverSha256"]:
        raise RuntimeError("sealed runner or driver drift")
    subject = load_subject(Path(manifest["subjectManifest"]))
    for cell in manifest["cells"]:
        if sha256_file(Path(cell["binary"])) != cell["binarySha256"] or \
           sha256_file(Path(cell["library"])) != cell["librarySha256"]:
            raise RuntimeError(f"cell artifact drift: {cell['cellId']}")
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


def wait_ready(path: Path, process: Any, timeout: float = 15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and "SPEC133_READY" in path.read_text(encoding="utf-8", errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError(f"peer exited before READY: rc={process.returncode}")
        time.sleep(0.1)
    raise RuntimeError("peer READY timeout")


def host_command(host: Any, command: str) -> tuple[int, str]:
    marker = "__SPEC133_RC__="
    text = host.cmd(f"{command}; spec133_rc=$?; printf '\\n{marker}%s\\n' \"$spec133_rc\"")
    match = re.search(rf"\n{marker}(\d+)\s*$", text)
    if match is None:
        raise RuntimeError(f"missing command status marker: {command}")
    return int(match.group(1)), text[:match.start()].strip()


def install_verified_routes(host: Any, transport: str, remote_uri: str,
                            prefixes: tuple[str, str], evidence_path: Path) -> int:
    env = f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)}"
    face_command = (
        f"{env} nfdc face create {shlex.quote(remote_uri)} "
        "persistency persistent"
    )
    face_rc, face_output = host_command(host, face_command)
    face_match = re.search(r"face-created id=(\d+)", face_output)
    if face_rc != 0 or face_match is None:
        raise RuntimeError(
            f"cannot create deterministic inter-peer face rc={face_rc}: {face_output}"
        )
    face_id = int(face_match.group(1))
    route_records = []
    for prefix in prefixes:
        command = (
            f"{env} nfdc route add {shlex.quote(prefix)} "
            f"nexthop {face_id}"
        )
        rc, output = host_command(host, command)
        route_records.append(
            {"prefix": prefix, "faceId": face_id, "returnCode": rc, "output": output}
        )
        if rc != 0 or "route-add-accepted" not in output:
            raise RuntimeError(
                f"route installation failed prefix={prefix} face={face_id} "
                f"rc={rc}: {output}"
            )
    strategy_rc, strategy_output = host_command(
        host,
        f"{env} nfdc strategy set {shlex.quote(prefixes[0])} "
        "/localhost/nfd/strategy/multicast",
    )
    rib_rc, rib = host_command(host, f"{env} nfdc route list")
    missing = [prefix for prefix in prefixes if prefix not in rib]
    evidence = {
        "schemaVersion": "spec133-route-evidence-v1",
        "remoteUri": remote_uri,
        "faceId": face_id,
        "faceCreate": {"returnCode": face_rc, "output": face_output},
        "routes": route_records,
        "strategy": {"returnCode": strategy_rc, "output": strategy_output},
        "rib": {"returnCode": rib_rc, "output": rib},
        "missingPrefixes": missing,
    }
    atomic_json(evidence_path, evidence)
    if strategy_rc != 0 or rib_rc != 0 or missing:
        raise RuntimeError(
            f"route verification failed face={face_id} missing={missing} "
            f"strategyRc={strategy_rc} ribRc={rib_rc}"
        )
    return face_id


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
        resolved = []
        for candidate in candidates:
            cmdline = Path(f"/proc/{candidate}/cmdline")
            if cmdline.is_file() and "svs-sync-" in cmdline.read_bytes().replace(
                    b"\0", b" ").decode(errors="replace"):
                resolved.append(candidate)
        pid = resolved[-1] if resolved else process.pid
        stat = Path(f"/proc/{pid}/stat")
        status = Path(f"/proc/{pid}/status")
        entry: dict[str, Any] = {"pid": pid, "wrapperPid": process.pid,
                                 "peerResolved": bool(resolved)}
        if stat.is_file():
            fields = stat.read_text().split()
            entry["cpuTicks"] = int(fields[13]) + int(fields[14])
        if status.is_file():
            for line in status.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith(("VmRSS:", "Threads:")):
                    key, value = line.split(":", 1)
                    entry[key] = value.strip()
        sample["processes"][role] = entry
    return sample


def run_cell(root: Path, config: dict[str, Any], subject: dict[str, Any],
             *, formal: bool) -> dict[str, Any]:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    cell = root / "cells" / config["cellId"]
    receipt_path = root / "receipts" / f"{config['cellId']}.json"
    if formal and (cell.exists() or receipt_path.exists()):
        raise RuntimeError(f"formal cell already attempted: {config['cellId']}")
    cell.mkdir(parents=True, exist_ok=False)
    atomic_json(cell / "cell-config.json", config)
    topology = cell / "topology.conf"
    topology.write_text("[nodes]\npeer-a:\npeer-b:\n\n[links]\n"
                        "peer-a:peer-b delay=10ms bw=100 loss=0\n", encoding="utf-8")
    sync_prefix = f"/spec133/sync/{config['cellId']}"
    prefixes = {peer: f"/spec133/{peer}/{config['cellId']}" for peer in PEERS}
    ndn = None
    peers: dict[str, Any] = {}
    captures: list[Any] = []
    samples: list[dict[str, Any]] = []
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
        for peer, host in hosts.items():
            socket = Path(f"/run/nfd/{peer}.sock")
            deadline = time.monotonic() + 15
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")
            neighbor = hosts[PEERS[1] if peer == PEERS[0] else PEERS[0]]
            install_verified_routes(
                host,
                transports[peer],
                f"udp4://{neighbor.IP()}:6363",
                (sync_prefix, prefixes[neighbor.name]),
                cell / f"{peer}-route-evidence.json",
            )
            captures.append(host.popen(
                f"ndndump -i {host.defaultIntf().name} -t -v >"
                f"{shlex.quote(str(cell / (peer + '-ndndump.log')))} 2>&1", shell=True))
        atomic_json(cell / "environment.json", {"cpuAffinity": [0, 2],
                    "transports": transports, "syncPrefix": sync_prefix,
                    "nodePrefixes": prefixes, "ndnsfRuntime": False,
                    "executionModel": "single-face-io-thread"})

        commands = {}
        io_cpus = {"peer-a": 0, "peer-b": 2}
        other = {"peer-a": "peer-b", "peer-b": "peer-a"}
        for peer in PEERS:
            mode = config["profileMode"]
            binary_key = "cleanBinary" if mode == "clean" else "profiledBinary"
            library_key = "cleanLibrary" if mode == "clean" else "profiledLibrary"
            env_values = profile_environment(mode, config["cellId"], peer, subject)
            env_values["NDN_CLIENT_TRANSPORT"] = transports[peer]
            env_text = " ".join(f"{key}={shlex.quote(value)}" for key, value in env_values.items())
            io_cpu = io_cpus[peer]
            command = (
                f"taskset -c {io_cpu} env LD_LIBRARY_PATH="
                f"{shlex.quote(str(Path(subject[library_key]).parent))} {env_text} "
                f"{shlex.quote(str(subject[binary_key]))} --subject sync-publish-no-internal-parallelism "
                f"--profile-mode {mode} --sync-prefix {shlex.quote(sync_prefix)} "
                f"--node-prefix {shlex.quote(prefixes[peer])} --cell-id {shlex.quote(config['cellId'])} "
                f"--peer-id {peer} --remote-peer-id {other[peer]} --rate-pps {config['ratePpsPerPeer']} "
                f"--warmup-s {config['warmupSeconds']} --measure-s {config['measureSeconds']} "
                f"--drain-s {config['drainSeconds']} --start-delay-ms 2000 "
                f"--io-cpu {io_cpu} "
                f"--events {shlex.quote(str(cell / (peer + '-events.jsonl')))} "
                f">{shlex.quote(str(cell / (peer + '.stdout')))} "
                f"2>{shlex.quote(str(cell / (peer + '.stderr')))}"
            )
            commands[peer] = command
        atomic_json(cell / "commands.json", commands)
        infrastructure_stage = False
        for peer in PEERS:
            peers[peer] = hosts[peer].popen(commands[peer], shell=True)
        for peer in PEERS:
            wait_ready(cell / f"{peer}.stdout", peers[peer])
        deadline = time.monotonic() + config["warmupSeconds"] + config["measureSeconds"] + \
            config["drainSeconds"] + 30
        while time.monotonic() < deadline and any(process.poll() is None for process in peers.values()):
            samples.append(process_sample(peers))
            time.sleep(1)
        if any(process.poll() is None for process in peers.values()):
            raise RuntimeError("bounded cell deadline exceeded")
        if any(process.returncode != 0 for process in peers.values()):
            raise RuntimeError(f"peer return codes: {[(key, p.returncode) for key, p in peers.items()]}")
    except Exception as error:
        runtime_error = f"{type(error).__name__}: {error}"
    finally:
        return_codes = {key: stop(process) for key, process in peers.items()}
        for process in captures:
            stop(process)
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
    with (cell / "resource-samples.jsonl").open("w", encoding="utf-8") as output:
        for sample in samples:
            output.write(json.dumps(sample, sort_keys=True) + "\n")
    status = "COMPLETE" if not runtime_error else \
             ("INFRA_INVALID" if infrastructure_stage else "SUBJECT_FAILURE")
    receipt = {"schemaVersion": "spec133-terminal-receipt-v1", "cellId": config["cellId"],
               "attempt": 1, "startedMonotonicNs": started,
               "endedMonotonicNs": time.monotonic_ns(), "returnCodes": return_codes,
               "status": status, "error": runtime_error}
    atomic_json(cell / "terminal-receipt.json", receipt)
    atomic_json(receipt_path, receipt)
    return receipt


def parse_cell_metrics(cell: Path, config: dict[str, Any]) -> dict[str, Any]:
    attempted = delivered = delivered_all = invalid = 0
    for peer in PEERS:
        events = cell / f"{peer}-events.jsonl"
        for line in events.read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            if event["event"] == "delivery":
                delivered_all += 1
                if event["phase"] == "measured":
                    delivered += 1
                continue
            if event["phase"] != "measured":
                continue
            if event["event"] == "api-return":
                attempted += 1
            elif event["event"] == "invalid":
                invalid += 1
    samples = [json.loads(line) for line in (cell / "resource-samples.jsonl").read_text().splitlines()]
    cpu_percent = 0.0
    if len(samples) >= 2:
        ticks = os.sysconf("SC_CLK_TCK")
        deltas = []
        for peer in PEERS:
            points = [
                (sample["monotonicNs"],
                 sample["processes"].get(peer, {}).get("cpuTicks"))
                for sample in samples
            ]
            points = [(stamp, value) for stamp, value in points if value is not None]
            if len(points) >= 2:
                elapsed = (points[-1][0] - points[0][0]) / 1e9
                if elapsed > 0:
                    deltas.append(
                        (points[-1][1] - points[0][1]) / ticks / elapsed * 100.0
                    )
        cpu_percent = sum(deltas)
    profile_complete = True
    if config["profileMode"] == "enabled":
        expected_summaries = int(config["profileStageCount"])
        sample_modulus = str(config["profileSampleModulus"])
        for peer in PEERS:
            text = (cell / f"{peer}.stderr").read_text(encoding="utf-8", errors="replace")
            profile_complete &= text.count("event=profile-start") == 1
            profile_complete &= text.count("event=profile-stop") == 1
            summaries = [line for line in text.splitlines() if "event=stage-summary" in line]
            spans = [line for line in text.splitlines() if "event=stage-span" in line]
            profile_complete &= len(summaries) == expected_summaries
            for line in summaries + spans:
                profile_complete &= f"cell={config['cellId']}" in line
                profile_complete &= f"peer={peer}" in line
                profile_complete &= f"sampleModulus={sample_modulus}" in line
                profile_complete &= "schema=spec133-stage-" in line
            profile_complete &= all("droppedRecords=0" in line for line in summaries)
    duration = config["measureSeconds"]
    return {"attempted": attempted, "delivered": delivered,
            "deliveredAllPhases": delivered_all, "invalid": invalid,
            "attemptedPpsPerPeer": attempted / duration / 2,
            "deliveryRatio": delivered / attempted if attempted else 0.0,
            "aggregateCpuPercent": cpu_percent, "profileComplete": bool(profile_complete)}


def relative_delta(left: float, right: float) -> float:
    return 0.0 if left == right == 0 else abs(right - left) / max(abs(left), 1e-12)


def compare_overhead(metrics: dict[str, dict[str, Any]]) -> dict[str, Any]:
    comparisons = {}
    admitted = True
    for label, left, right in (("A-vs-B", "A-clean-control", "B-profiled-disabled"),
                               ("B-vs-C", "B-profiled-disabled", "C-profiled-enabled"),
                               ("A-vs-C", "A-clean-control", "C-profiled-enabled")):
        a, b = metrics[left], metrics[right]
        result = {
            "attemptedRateRelativeDelta": relative_delta(a["attemptedPpsPerPeer"], b["attemptedPpsPerPeer"]),
            "deliveryRatioRelativeDelta": relative_delta(a["deliveryRatio"], b["deliveryRatio"]),
            "cpuPercentagePointDelta": abs(b["aggregateCpuPercent"] - a["aggregateCpuPercent"]),
        }
        result["admitted"] = result["attemptedRateRelativeDelta"] <= 0.05 and \
            result["deliveryRatioRelativeDelta"] <= 0.05 and result["cpuPercentagePointDelta"] <= 5.0
        admitted &= result["admitted"]
        comparisons[label] = result
    admitted &= metrics["C-profiled-enabled"]["profileComplete"]
    admitted &= all(item["invalid"] == 0 for item in metrics.values())
    admitted &= all(item.get("deliveredAllPhases", 0) > 0 for item in metrics.values())
    return {"comparisons": comparisons, "admitted": bool(admitted)}


def overhead_preflight(subject_path: Path, output: Path) -> dict[str, Any]:
    subject = load_subject(subject_path)
    if subject.get("schemaVersion") != "spec133-subject-manifest-io-v2":
        raise RuntimeError("overhead preflight requires the single-I/O subject manifest")
    output.mkdir(parents=True, exist_ok=False)
    receipts: dict[str, Any] = {}
    metrics: dict[str, Any] = {}
    for arm, mode, binary_key, library_key in ARMS:
        config = {"cellId": arm, "profileMode": mode, "binary": subject[binary_key],
                  "library": subject[library_key], "attempt": 1,
                  "profileStageCount": subject["profileConfig"]["stageCount"],
                  "profileSampleModulus": subject["profileConfig"]["sampleModulus"],
                  **PREFLIGHT}
        receipts[arm] = run_cell(output, config, subject, formal=False)
        if receipts[arm]["status"] != "COMPLETE":
            metrics[arm] = {"attemptedPpsPerPeer": 0.0, "deliveryRatio": 0.0,
                            "aggregateCpuPercent": 0.0, "profileComplete": False,
                            "invalid": 1}
        else:
            metrics[arm] = parse_cell_metrics(output / "cells" / arm, config)
    comparison = compare_overhead(metrics)
    receipt = {"schemaVersion": "spec133-overhead-admission-v1",
               "subjectManifest": str(subject_path.resolve()),
               "subjectManifestSha256": sha256_file(subject_path),
               "fixedRatePpsPerPeer": 1000, "arms": receipts, "metrics": metrics,
               **comparison, "verdict": "ADMITTED" if comparison["admitted"] else "REJECTED"}
    atomic_json(output / "overhead-receipt.json", receipt)
    return receipt


def reanalyze_preflight(output: Path) -> dict[str, Any]:
    original_path = output / "overhead-receipt.json"
    corrected_path = output / "overhead-receipt-corrected.json"
    if corrected_path.exists():
        raise RuntimeError(f"corrected overhead receipt already exists: {corrected_path}")
    original = load_json(original_path)
    subject_path = Path(original["subjectManifest"])
    subject = load_subject(subject_path)
    metrics: dict[str, Any] = {}
    for arm, mode, binary_key, library_key in ARMS:
        config = {
            "cellId": arm,
            "profileMode": mode,
            "binary": subject[binary_key],
            "library": subject[library_key],
            "attempt": 1,
            "profileStageCount": subject["profileConfig"]["stageCount"],
            "profileSampleModulus": subject["profileConfig"]["sampleModulus"],
            **PREFLIGHT,
        }
        receipt = load_json(output / "receipts" / f"{arm}.json")
        if receipt.get("status") != "COMPLETE":
            raise RuntimeError(f"cannot reanalyze incomplete preflight arm: {arm}")
        metrics[arm] = parse_cell_metrics(output / "cells" / arm, config)
    comparison = compare_overhead(metrics)
    receipt = {
        "schemaVersion": "spec133-overhead-admission-v1",
        "correction": "per-peer-first-last-valid-cpu-sample",
        "correctionOf": str(original_path.resolve()),
        "correctionOfSha256": sha256_file(original_path),
        "networkRerun": False,
        "subjectManifest": str(subject_path.resolve()),
        "subjectManifestSha256": sha256_file(subject_path),
        "fixedRatePpsPerPeer": 1000,
        "arms": original["arms"],
        "metrics": metrics,
        **comparison,
        "verdict": "ADMITTED" if comparison["admitted"] else "REJECTED",
    }
    atomic_json(corrected_path, receipt)
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    overhead = sub.add_parser("overhead-preflight")
    overhead.add_argument("--subject-manifest", type=Path, default=DEFAULT_SUBJECT)
    overhead.add_argument("--output", type=Path, required=True)
    reanalyze = sub.add_parser("reanalyze-preflight")
    reanalyze.add_argument("--output", type=Path, required=True)
    plan = sub.add_parser("plan")
    plan.add_argument("--campaign-id", required=True)
    plan.add_argument("--subject-manifest", type=Path, default=DEFAULT_SUBJECT)
    plan.add_argument("--overhead-receipt", type=Path, required=True)
    plan.add_argument("--output", type=Path, required=True)
    seal = sub.add_parser("seal")
    seal.add_argument("campaign", type=Path)
    run = sub.add_parser("run")
    run.add_argument("campaign", type=Path)
    args = parser.parse_args()

    if args.command == "overhead-preflight":
        receipt = overhead_preflight(args.subject_manifest.resolve(), args.output.resolve())
        print(json.dumps(receipt, sort_keys=True))
        return 0 if receipt["verdict"] == "ADMITTED" else 1
    if args.command == "reanalyze-preflight":
        receipt = reanalyze_preflight(args.output.resolve())
        print(json.dumps(receipt, sort_keys=True))
        return 0 if receipt["verdict"] == "ADMITTED" else 1
    if args.command == "plan":
        manifest = make_manifest(args.campaign_id, args.subject_manifest.resolve(),
                                 args.overhead_receipt.resolve())
        args.output.mkdir(parents=True, exist_ok=False)
        atomic_json(args.output / "campaign-manifest.json", manifest)
        print(args.output)
        return 0
    campaign = args.campaign.resolve()
    manifest = load_json(campaign / "campaign-manifest.json")
    if args.command == "seal":
        verify_frozen(manifest)
        if (campaign / "receipts").exists() or (campaign / "cells").exists():
            raise RuntimeError("cannot seal a campaign with existing attempts")
        (campaign / ".sealed").write_text(
            sha256_file(campaign / "campaign-manifest.json") + "\n", encoding="utf-8")
        print(campaign / ".sealed")
        return 0

    lock_path = campaign / ".campaign.lock"
    with lock_path.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if not (campaign / ".sealed").is_file():
            raise RuntimeError("campaign is not sealed")
        if (campaign / ".sealed").read_text().strip() != sha256_file(campaign / "campaign-manifest.json"):
            raise RuntimeError("campaign seal mismatch")
        subject = verify_frozen(manifest)
        outcomes = []
        for cell in manifest["cells"]:
            verify_frozen(manifest)
            outcome = run_cell(campaign, cell, subject, formal=True)
            outcomes.append(outcome)
            print(json.dumps(outcome, sort_keys=True), flush=True)
        atomic_json(campaign / "campaign-terminal.json",
                    {"schemaVersion": "spec133-campaign-terminal-v1",
                     "receiptCount": len(outcomes), "outcomes": outcomes})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
