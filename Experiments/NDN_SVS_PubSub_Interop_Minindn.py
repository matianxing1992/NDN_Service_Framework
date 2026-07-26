#!/usr/bin/env python3
"""Conditionally run C++/NDNts SVS-PS payload interoperability in MiniNDN."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import time
from typing import Any, Optional


REPO = Path(__file__).resolve().parents[1]
INTEROP = REPO / "examples/interop/ndn-svs-v3"
sys.path.insert(0, str(INTEROP))

from payload_corpus import classify_receipts, load_manifest  # noqa: E402


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_standalone(path: Path) -> tuple[Path, dict[str, Any]]:
    root = path.expanduser().resolve()
    summary_path = root / "summary.json" if root.is_dir() else root
    if not summary_path.is_file():
        raise ValueError(f"standalone summary is missing: {summary_path}")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if summary.get("schemaVersion") != "spec117-payload-summary-v1":
        raise ValueError("not a Spec 117 standalone summary")
    if summary.get("cellId") != "standalone":
        raise ValueError("MiniNDN admission requires the standalone cell")
    return summary_path.parent, summary


def admitted(summary: dict[str, Any]) -> bool:
    return summary.get("status") == "SUCCESS" and summary.get("passed") is True and \
        summary.get("verifiedReceiptCount") == summary.get("expectedReceiptCount") == 8


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def stop_process(process: Any, grace: float = 3.0) -> Optional[int]:
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


def verify_identity(standalone_root: Path, summary: dict[str, Any],
                    node_bin: Path, ndn_svs: Path) \
        -> tuple[Path, Path, Path]:
    cpp = Path(summary["peers"]["cpp"]["path"]).resolve()
    ndnts = Path(summary["peers"]["ndnts"]["path"]).resolve()
    manifest = standalone_root / "corpus/manifest.json"
    for path, expected in (
        (cpp, summary["peers"]["cpp"]["sha256"]),
        (ndnts, summary["peers"]["ndnts"]["sha256"]),
        (manifest, summary["corpusManifestSha256"]),
        (node_bin, summary["ndntsRuntime"]["nodeSha256"]),
        (Path(summary["ndntsRuntime"]["packageLockPath"]),
         summary["ndntsRuntime"]["packageLockSha256"]),
        (ndn_svs / "build/libndn-svs.so", summary["ndnSvs"]["librarySha256"]),
    ):
        if not path.is_file() or sha256_file(path) != expected:
            raise RuntimeError(f"standalone identity changed: {path}")
    if ndn_svs != Path(summary["ndnSvs"]["path"]).resolve():
        raise RuntimeError("NDN-SVS path differs from standalone subject")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ndn_svs,
                                   text=True).strip()
    tree = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=ndn_svs,
                                   text=True).strip()
    status = subprocess.check_output(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=ndn_svs, text=True).strip()
    if head != summary["ndnSvs"]["head"] or tree != summary["ndnSvs"]["tree"] or status:
        raise RuntimeError("NDN-SVS source identity differs from standalone subject")
    load_manifest(manifest)
    return cpp, ndnts, manifest


def not_admitted(output: Path, standalone_root: Path, summary: dict[str, Any],
                 losses: list[int]) -> dict[str, Any]:
    receipt = {
        "schemaVersion": "spec117-minindn-matrix-v1",
        "status": "NOT_ADMITTED",
        "passed": False,
        "miniNdnLaunched": False,
        "requestedLossPercent": losses,
        "cells": [{"cellId": f"loss{loss:02d}", "lossPercent": loss,
                   "status": "NOT_ADMITTED", "miniNdnLaunched": False}
                  for loss in losses],
        "blockedBy": {
            "path": str(standalone_root / "summary.json"),
            "sha256": sha256_file(standalone_root / "summary.json"),
            "status": summary.get("status"),
            "verifiedReceiptCount": summary.get("verifiedReceiptCount"),
            "expectedReceiptCount": summary.get("expectedReceiptCount"),
            "errors": summary.get("errors", []),
        },
    }
    output.mkdir(parents=True)
    (output / "summary.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def run_cell(output: Path, loss: int, standalone_root: Path,
             standalone: dict[str, Any], node_bin: Path,
             ndn_svs: Path) -> dict[str, Any]:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    cell_id = f"loss{loss:02d}"
    cell = output / cell_id
    cell.mkdir(parents=True)
    cpp_peer, ndnts_peer, source_manifest = verify_identity(
        standalone_root, standalone, node_bin, ndn_svs)
    shutil.copytree(source_manifest.parent, cell / "corpus")
    manifest = cell / "corpus/manifest.json"
    topology = cell / "topology.conf"
    topology.write_text(
        "[nodes]\ncpp:\nndnts:\n\n[links]\n"
        f"cpp:ndnts delay=10ms bw=100 loss={loss}\n", encoding="utf-8")
    sync_prefix = f"/ndn/ndnsf/spec117/minindn/{cell_id}/{os.getpid()}"
    config = {
        "schemaVersion": "spec117-minindn-cell-v1",
        "cellId": cell_id,
        "lossPercent": loss,
        "syncPrefix": sync_prefix,
        "corpusManifestSha256": sha256_file(manifest),
        "standaloneSummarySha256": sha256_file(standalone_root / "summary.json"),
        "miniNdnLaunched": True,
    }
    (cell / "cell-config.json").write_text(
        json.dumps(config, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    ndn = None
    processes: list[Any] = []
    captures: list[Any] = []
    runtime_error = ""
    return_codes: list[Optional[int]] = []
    started = time.monotonic()
    try:
        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        cpp, ndnts = ndn.net["cpp"], ndn.net["ndnts"]
        transports = {
            "cpp": "unix:///run/nfd/cpp.sock",
            "ndnts": "unix:///run/nfd/ndnts.sock",
        }
        for node in (cpp, ndnts):
            socket = Path(f"/run/nfd/{node.name}.sock")
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")

        # Only the inter-host sync-group route is installed. Application
        # publication and Mapping producer routes must be registered by peers.
        for node, neighbor in ((cpp, ndnts), (ndnts, cpp)):
            env = f"NDN_CLIENT_TRANSPORT={transports[node.name]}"
            node.cmd(f"{env} nfdc strategy set {shlex.quote(sync_prefix)} "
                     "/localhost/nfd/strategy/multicast")
            route = node.cmd(f"{env} nfdc route add {shlex.quote(sync_prefix)} "
                             f"udp4://{neighbor.IP()}:6363")
            (cell / f"{node.name}-sync-route.txt").write_text(route, encoding="utf-8")

        for node in (cpp, ndnts):
            capture = cell / f"{node.name}-ndndump.log"
            captures.append(node.popen(
                f"ndndump -i {node.defaultIntf().name} -t -v >{shlex.quote(str(capture))} 2>&1",
                shell=True))
        time.sleep(0.5)

        cpp_events, ndnts_events = cell / "cpp.jsonl", cell / "ndnts.jsonl"
        cpp_cmd = (
            f"NDN_CLIENT_TRANSPORT={transports['cpp']} "
            f"LD_LIBRARY_PATH={shlex.quote(str(ndn_svs / 'build'))} "
            f"{shlex.quote(str(cpp_peer))} --mode payload --version v3 "
            f"--sync-prefix {shlex.quote(sync_prefix)} --node-prefix /cpp "
            f"--manifest {shlex.quote(str(manifest))} --publish-interval-ms 50 "
            f"--start-delay-ms 2500 --settle-ms 8000 --events {shlex.quote(str(cpp_events))} "
            f">{shlex.quote(str(cell / 'cpp.stdout'))} 2>{shlex.quote(str(cell / 'cpp.stderr'))}")
        ndnts_cmd = (
            f"NDNTS_UPLINK={transports['ndnts']} {shlex.quote(str(node_bin))} "
            f"{shlex.quote(str(ndnts_peer))} --mode payload --version v3 "
            f"--sync-prefix {shlex.quote(sync_prefix)} --node-prefix /ndnts "
            f"--manifest {shlex.quote(str(manifest))} --publish-interval-ms 50 "
            f"--start-delay-ms 2500 --settle-ms 8000 --events {shlex.quote(str(ndnts_events))} "
            f">{shlex.quote(str(cell / 'ndnts.stdout'))} "
            f"2>{shlex.quote(str(cell / 'ndnts.stderr'))}")
        (cell / "commands.json").write_text(json.dumps(
            {"cpp": cpp_cmd, "ndnts": ndnts_cmd}, indent=2) + "\n", encoding="utf-8")
        processes.extend((cpp.popen(cpp_cmd, shell=True), ndnts.popen(ndnts_cmd, shell=True)))
        time.sleep(1.0)
        for node in (cpp, ndnts):
            rib = node.cmd(f"NDN_CLIENT_TRANSPORT={transports[node.name]} nfdc route list")
            (cell / f"{node.name}-rib-after-registration.txt").write_text(
                rib, encoding="utf-8")

        deadline = time.monotonic() + 20
        for process in processes:
            process.wait(timeout=max(0.1, deadline - time.monotonic()))
    except Exception as error:
        runtime_error = f"{type(error).__name__}: {error}"
    finally:
        return_codes = [stop_process(process) for process in processes]
        for capture in captures:
            stop_process(capture)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as error:
                runtime_error = runtime_error or f"ndn.stop: {type(error).__name__}: {error}"
        try:
            Minindn.cleanUp()
        except Exception as error:
            runtime_error = runtime_error or f"cleanup: {type(error).__name__}: {error}"
        sys.argv = original_argv

    events = read_jsonl(cell / "cpp.jsonl") + read_jsonl(cell / "ndnts.jsonl")
    if runtime_error or any(code not in (0, 1, None) for code in return_codes):
        events.append({"event": "infra-error", "stage": "orchestration",
                       "reason": runtime_error or f"return codes {return_codes}"})
    result = classify_receipts(load_manifest(manifest), events)
    result.update(config)
    result.update({
        "returnCodes": return_codes,
        "runtimeError": runtime_error,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "packetEvidence": {
            path.name: {"bytes": path.stat().st_size, "sha256": sha256_file(path)}
            for path in (cell / "cpp-ndndump.log", cell / "ndnts-ndndump.log")
            if path.is_file()
        },
    })
    (cell / "summary.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--standalone-result", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--loss", choices=("0", "5", "both"), default="both")
    parser.add_argument("--node-bin", type=Path,
                        default=Path("/home/tianxing/.local/node-v22.23.1/bin/node"))
    parser.add_argument("--ndn-svs", type=Path,
                        default=Path("/home/tianxing/NDN/ndn-svs"))
    parser.add_argument("--ownership-lock", type=Path,
                        default=Path("/run/lock/ndnsf-spec117-minindn.lock"))
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    output = args.output.expanduser().resolve()
    if output.exists():
        raise SystemExit(f"output directory already exists: {output}")
    standalone_root, standalone = load_standalone(args.standalone_result)
    losses = [0, 5] if args.loss == "both" else [int(args.loss)]
    if not admitted(standalone):
        receipt = not_admitted(output, standalone_root, standalone, losses)
        print("SPEC117_MININDN " + json.dumps(receipt, sort_keys=True))
        return 1
    if os.geteuid() != 0:
        raise SystemExit("an admitted MiniNDN run must be launched through sudo")
    node_bin = args.node_bin.expanduser().resolve()
    ndn_svs = args.ndn_svs.expanduser().resolve()
    if not node_bin.is_file() or not os.access(node_bin, os.X_OK):
        raise SystemExit(f"Node.js executable unavailable: {node_bin}")

    output.parent.mkdir(parents=True, exist_ok=True)
    args.ownership_lock.parent.mkdir(parents=True, exist_ok=True)
    with args.ownership_lock.open("a+") as owner:
        try:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise SystemExit("another Spec 117 MiniNDN owner is active") from error
        output.mkdir()
        cells = [run_cell(output, loss, standalone_root, standalone, node_bin, ndn_svs)
                 for loss in losses]
    aggregate = {
        "schemaVersion": "spec117-minindn-matrix-v1",
        "status": "SUCCESS" if all(cell["passed"] for cell in cells) else "FAILURE",
        "passed": all(cell["passed"] for cell in cells),
        "miniNdnLaunched": True,
        "cells": cells,
    }
    (output / "summary.json").write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("SPEC117_MININDN " + json.dumps(aggregate, sort_keys=True))
    return 0 if aggregate["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
