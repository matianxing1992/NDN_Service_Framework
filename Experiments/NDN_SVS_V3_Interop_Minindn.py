#!/usr/bin/env python3
"""Run immutable C++/NDNts SVS V3 interoperability cells in MiniNDN."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterable, Optional


REPO = Path(__file__).resolve().parents[1]
DEFAULT_NODE_BIN = Path(os.environ.get(
    "SPEC114_NODE_BIN", "/home/tianxing/.local/node-v22.23.1/bin/node"))
sys.path.insert(0, str(REPO))

from Experiments import spec114_candidate_manifest as candidate  # noqa: E402


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()]


def normalize_node(value: str) -> str:
    """Normalize NDNts typed generic components (`/8=x`) to URI `/x`."""
    return "/" + "/".join(re.sub(r"^8=", "", part)
                              for part in value.split("/") if part)


def covered_remote_sequences(events: Iterable[dict[str, Any]], remote: str) -> dict[str, Any]:
    values: list[int] = []
    for event in events:
        if event.get("event") != "update" or normalize_node(str(event.get("nodeName", ""))) != remote:
            continue
        low, high = int(event["low"]), int(event["high"])
        if low < 1 or high < low:
            raise ValueError(f"invalid callback range {low}..{high}")
        values.extend(range(low, high + 1))
    seen: set[int] = set()
    duplicates: list[int] = []
    for value in values:
        if value in seen:
            duplicates.append(value)
        seen.add(value)
    return {"covered": sorted(seen), "duplicates": duplicates,
            "callbackRangeCount": sum(1 for e in events if e.get("event") == "update" and
                                       normalize_node(str(e.get("nodeName", ""))) == remote)}


def final_state(events: Iterable[dict[str, Any]]) -> list[tuple[str, int, int]]:
    return sorted((normalize_node(str(event["nodeName"])),
                   int(event.get("bootstrapTime", 0)), int(event["high"]))
                  for event in events if event.get("event") == "state")


def count_sync_ack_lines(text: str, sync_prefix: str) -> int:
    count = 0
    for line in text.splitlines():
        if sync_prefix not in line:
            continue
        if re.search(r"(^|\s)(?:DATA|Data)(?::|\s)", line):
            count += 1
    return count


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_node_bin(path: Path) -> dict[str, str]:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError(f"Node.js executable is unavailable: {resolved}")
    completed = subprocess.run([str(resolved), "--version"], text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               check=False)
    version = completed.stdout.strip()
    match = re.fullmatch(r"v(\d+)(?:\.\d+){2}", version)
    if completed.returncode != 0 or match is None or int(match.group(1)) < 22:
        raise ValueError(f"Spec 114 requires Node.js >=22, got {version!r} from {resolved}")
    return {"path": str(resolved), "version": version, "sha256": sha256_file(resolved)}


def require_formal_privilege(formal: bool, aggregate_only: bool,
                             euid: Optional[int] = None) -> None:
    """Reject an unprivileged formal launch before any run-once mutation."""
    actual_euid = os.geteuid() if euid is None else euid
    if formal and not aggregate_only and actual_euid != 0:
        raise RuntimeError("formal MiniNDN matrix must be launched through sudo")


def analyze_cell(cpp_events: list[dict[str, Any]], ndnts_events: list[dict[str, Any]],
                 publish_count: int) -> dict[str, Any]:
    cpp = covered_remote_sequences(cpp_events, "/ndnts")
    ndnts = covered_remote_sequences(ndnts_events, "/cpp")
    expected = list(range(1, publish_count + 1))
    cpp_state, ndnts_state = final_state(cpp_events), final_state(ndnts_events)
    rejects = [e for e in cpp_events + ndnts_events if e.get("event") == "reject"]
    return {
        "expectedRemoteSequences": expected,
        "cppObservedNdnts": cpp,
        "ndntsObservedCpp": ndnts,
        "cppFinalState": cpp_state,
        "ndntsFinalState": ndnts_state,
        "finalVectorsEqual": cpp_state == ndnts_state and bool(cpp_state),
        "rejects": rejects,
        "coveragePassed": cpp["covered"] == expected and ndnts["covered"] == expected,
        "duplicateCoverageCount": len(cpp["duplicates"]) + len(ndnts["duplicates"]),
    }


def aggregate_formal_summaries(manifest_path: Path,
                               manifest: dict[str, Any]) -> dict[str, Any]:
    summaries = []
    for cell_id in candidate.CELLS:
        path = manifest_path.parent / cell_id / "summary.json"
        if not path.is_file():
            raise ValueError(f"formal cell summary is missing: {cell_id}")
        summary = json.loads(path.read_text(encoding="utf-8"))
        if summary.get("candidateId") != manifest["candidateId"] or \
           summary.get("cellId") != cell_id or not summary.get("formal"):
            raise ValueError(f"formal cell identity mismatch: {cell_id}")
        summaries.append(summary)
    aggregate = {
        "schemaVersion": "spec114-minindn-matrix-v1",
        "candidateId": manifest["candidateId"], "formal": True,
        "cells": summaries,
        "passed": all(item.get("status") == "SUCCESS" for item in summaries),
    }
    output = manifest_path.parent / "formal-summary.json"
    output.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    return aggregate


def candidate_ndn_svs_path(manifest: dict[str, Any]) -> Path:
    """Return the source checkout frozen by the candidate manifest."""
    return Path(manifest["identity"]["ndnSvs"]["path"]).expanduser().resolve()


def candidate_interop_paths(manifest: dict[str, Any]) -> tuple[Path, Path]:
    """Return the NDNSF-owned C++ binary and TypeScript peer frozen by the manifest."""
    interop = manifest["identity"]["interop"]
    if interop.get("owner") != "NDNSF" or \
       interop.get("ndntsSourceLanguage") != "TypeScript":
        raise RuntimeError("formal interop peers must be NDNSF-owned C++ and TypeScript")
    cpp_peer = Path(interop["cppPeer"]).expanduser().resolve()
    ndnts_peer = Path(interop["ndntsSource"]).expanduser().resolve()
    if not cpp_peer.is_file() or not os.access(cpp_peer, os.X_OK):
        raise RuntimeError(f"NDNSF C++ interop peer is unavailable: {cpp_peer}")
    if ndnts_peer.suffix != ".ts" or not ndnts_peer.is_file():
        raise RuntimeError(f"NDNSF TypeScript interop peer is unavailable: {ndnts_peer}")
    return cpp_peer, ndnts_peer


def assert_candidate_unchanged(manifest: dict[str, Any],
                               ndn_svs: Optional[Path] = None) -> None:
    if ndn_svs is None:
        ndn_svs = candidate_ndn_svs_path(manifest)
    identity = manifest["identity"]
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ndn_svs,
                                   text=True).strip()
    tree = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=ndn_svs,
                                   text=True).strip()
    status = subprocess.check_output(["git", "status", "--porcelain=v1",
                                      "--untracked-files=all"], cwd=ndn_svs,
                                     text=True).strip()
    if head != identity["ndnSvs"]["head"] or tree != identity["ndnSvs"]["tree"] or status:
        raise RuntimeError("NDN-SVS source identity changed after candidate freeze")
    lock = Path(identity["ndntsLock"]["path"])
    if sha256_file(lock) != identity["ndntsLock"]["sha256"]:
        raise RuntimeError("NDNts lock identity changed after candidate freeze")
    for campaign_input in identity.get("campaignInputs", []):
        path = Path(campaign_input["path"])
        if not path.is_file() or sha256_file(path) != campaign_input["sha256"]:
            raise RuntimeError(f"campaign input changed after candidate freeze: {path}")
    candidate_interop_paths(manifest)


def update_run_once(manifest_path: Path, cell_id: str, expected: str, replacement: str) -> None:
    lock_path = manifest_path.with_name(".runonce.lock")
    with lock_path.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        value = candidate.inspect(manifest_path)
        current = value["runOnce"].get(cell_id)
        if current != expected:
            raise RuntimeError(f"cell {cell_id} is {current!r}, expected {expected!r}; rerun refused")
        value["runOnce"][cell_id] = replacement
        temporary = manifest_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
        os.replace(temporary, manifest_path)


def stop_process(process, grace: float = 3.0) -> Optional[int]:
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


def run_cell(*, manifest: dict[str, Any], cell_id: str, cell_dir: Path,
             publish_count: int, settle_ms: int, formal: bool,
             node_identity: dict[str, str], ndn_svs: Path) -> dict[str, Any]:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    if cell_dir.exists():
        raise RuntimeError(f"cell directory exists; rerun refused: {cell_dir}")
    cell_dir.mkdir(parents=True)
    loss = 0 if cell_id.startswith("loss00-") else 5
    topology = cell_dir / "topology.conf"
    topology.write_text(
        "[nodes]\ncpp:\nndnts:\n\n[links]\n"
        f"cpp:ndnts delay=10ms bw=100 loss={loss}\n", encoding="utf-8")
    config = {
        "schemaVersion": "spec114-minindn-cell-v1",
        "candidateId": manifest["candidateId"], "cellId": cell_id,
        "lossPercent": loss, "publishCountPerPeer": publish_count,
        "convergenceBoundSeconds": 60, "settleMs": settle_ms,
        "formal": formal, "command": sys.argv, "nodeRuntime": node_identity,
    }
    (cell_dir / "cell-config.json").write_text(json.dumps(config, indent=2) + "\n",
                                                encoding="utf-8")

    setLogLevel("warning")
    ndn = None
    processes: list[Any] = []
    captures: list[Any] = []
    runtime_error = ""
    started = time.monotonic()
    sync_prefix = f"/ndn/spec114/minindn/{manifest['candidateId']}/{cell_id}"
    cpp_peer, ndnts_peer = candidate_interop_paths(manifest)
    try:
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell_dir / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        cpp, ndnts = ndn.net["cpp"], ndn.net["ndnts"]
        for node in (cpp, ndnts):
            socket = Path(f"/run/nfd/{node.name}.sock")
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")

        cpp_transport = "unix:///run/nfd/cpp.sock"
        ndnts_transport = "unix:///run/nfd/ndnts.sock"
        for node, peer, transport in ((cpp, ndnts, cpp_transport),
                                      (ndnts, cpp, ndnts_transport)):
            node.cmd(f"NDN_CLIENT_TRANSPORT={transport} nfdc strategy set {sync_prefix} "
                     "/localhost/nfd/strategy/multicast")
            result = node.cmd(f"NDN_CLIENT_TRANSPORT={transport} nfdc route add {sync_prefix} "
                              f"udp4://{peer.IP()}:6363")
            (cell_dir / f"{node.name}-route.txt").write_text(result, encoding="utf-8")

        for node in (cpp, ndnts):
            capture_path = cell_dir / f"{node.name}-ndndump.log"
            captures.append(node.popen(
                f"ndndump -i {node.defaultIntf().name} -t -v >{capture_path} 2>&1",
                shell=True))
        time.sleep(0.5)

        cpp_events, ndnts_events = cell_dir / "cpp.jsonl", cell_dir / "ndnts.jsonl"
        cpp_cmd = (
            f"NDN_CLIENT_TRANSPORT={cpp_transport} LD_LIBRARY_PATH={ndn_svs}/build "
            f"{cpp_peer} --version v3 "
            f"--sync-prefix {sync_prefix} --node-prefix /cpp --publish-count {publish_count} "
            f"--publish-interval-ms 20 --start-delay-ms 2500 --settle-ms {settle_ms} "
            f"--events {cpp_events} >{cell_dir / 'cpp.stdout'} 2>{cell_dir / 'cpp.stderr'}")
        js_cmd = (
            f"NDNTS_UPLINK={ndnts_transport} {node_identity['path']} "
            f"{ndnts_peer} "
            f"--version v3 --sync-prefix {sync_prefix} --node-prefix /ndnts "
            f"--publish-count {publish_count} --publish-interval-ms 20 "
            f"--start-delay-ms 2500 --settle-ms {settle_ms} --events {ndnts_events} "
            f">{cell_dir / 'ndnts.stdout'} 2>{cell_dir / 'ndnts.stderr'}")
        cpp_proc = cpp.popen(cpp_cmd, shell=True)
        js_proc = ndnts.popen(js_cmd, shell=True)
        processes.extend([cpp_proc, js_proc])

        time.sleep(0.8)
        if cpp_proc.poll() is not None or js_proc.poll() is not None:
            raise RuntimeError("interop peer exited during startup")
        # NDNts registers its own local producer route.  Adding another route by
        # guessing among transient Unix management faces is both redundant and
        # racy, so preserve the RIB snapshot as evidence instead.
        rib = ndnts.cmd(f"NDN_CLIENT_TRANSPORT={ndnts_transport} nfdc route list")
        (cell_dir / "ndnts-rib.txt").write_text(rib, encoding="utf-8")

        deadline = time.monotonic() + settle_ms / 1000.0 + 20
        for process in processes:
            remaining = max(0.1, deadline - time.monotonic())
            process.wait(timeout=remaining)
    except Exception as exc:
        runtime_error = f"{type(exc).__name__}: {exc}"
    finally:
        return_codes = [stop_process(process) for process in processes]
        for capture in captures:
            stop_process(capture)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exc:
                runtime_error = runtime_error or f"ndn.stop: {type(exc).__name__}: {exc}"
        try:
            Minindn.cleanUp()
        except Exception as exc:
            runtime_error = runtime_error or f"cleanup: {type(exc).__name__}: {exc}"
        sys.argv = original_argv

    cpp_events_data = read_jsonl(cell_dir / "cpp.jsonl")
    ndnts_events_data = read_jsonl(cell_dir / "ndnts.jsonl")
    analysis = analyze_cell(cpp_events_data, ndnts_events_data, publish_count)
    capture_paths = [cell_dir / "cpp-ndndump.log", cell_dir / "ndnts-ndndump.log"]
    capture_text = "\n".join(path.read_text(encoding="utf-8", errors="replace")
                               for path in capture_paths if path.is_file())
    sync_acks = count_sync_ack_lines(capture_text, sync_prefix)
    packet_samples = {path.name: {"sha256": sha256_file(path), "bytes": path.stat().st_size}
                      for path in capture_paths if path.is_file()}
    passed = (not runtime_error and return_codes == [0, 0] and analysis["coveragePassed"] and
              analysis["duplicateCoverageCount"] == 0 and analysis["finalVectorsEqual"] and
              not analysis["rejects"] and sync_acks == 0)
    summary = {
        **config, **analysis, "status": "SUCCESS" if passed else "FAILURE",
        "runtimeError": runtime_error, "returnCodes": return_codes,
        "restartCount": 0, "syncAckCount": sync_acks,
        "interopPeers": {
            "owner": "NDNSF",
            "cpp": {"path": str(cpp_peer), "sha256": sha256_file(cpp_peer)},
            "ndnts": {"path": str(ndnts_peer), "language": "TypeScript",
                      "sha256": sha256_file(ndnts_peer)},
        },
        "packetSamples": packet_samples,
        "elapsedSeconds": round(time.monotonic() - started, 3),
    }
    (cell_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                            encoding="utf-8")
    return summary


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-manifest", type=Path, required=True)
    parser.add_argument("--matrix", choices=("formal", "smoke"), default="formal")
    parser.add_argument("--cells", default="")
    parser.add_argument("--publish-count", type=int, default=20)
    parser.add_argument("--convergence-timeout-s", type=int, default=60)
    parser.add_argument("--settle-ms", type=int, default=60000)
    parser.add_argument("--ownership-lock", type=Path,
                        default=Path("/run/lock/ndnsf-spec114-minindn.lock"))
    parser.add_argument("--node-bin", type=Path, default=DEFAULT_NODE_BIN)
    parser.add_argument("--aggregate-only", action="store_true")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    manifest_path = args.candidate_manifest.resolve()
    manifest = candidate.inspect(manifest_path)
    ndn_svs = candidate_ndn_svs_path(manifest)
    try:
        node_identity = validate_node_bin(args.node_bin)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    formal = args.matrix == "formal"
    # MiniNDN performs privileged namespace and link setup.  Reject before
    # acquiring the ownership lock or mutating any run-once cell state so a
    # non-root launch remains setup evidence rather than consuming a cell.
    try:
        require_formal_privilege(formal, args.aggregate_only)
    except RuntimeError as exc:
        raise SystemExit(str(exc)) from exc
    if args.aggregate_only:
        if not formal:
            raise SystemExit("--aggregate-only is valid only for the formal matrix")
        try:
            aggregate = aggregate_formal_summaries(manifest_path, manifest)
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc
        print("SPEC114_MATRIX " + json.dumps(aggregate, sort_keys=True))
        return 0 if aggregate["passed"] else 1
    if formal and (args.publish_count != 20 or args.convergence_timeout_s != 60 or
                   args.settle_ms != 60000):
        raise SystemExit("formal matrix requires count=20, timeout=60s, settle=60000ms")
    cells = list(candidate.CELLS) if not args.cells else args.cells.split(",")
    if any(cell not in candidate.CELLS for cell in cells):
        raise SystemExit("unknown cell ID")

    args.ownership_lock.parent.mkdir(parents=True, exist_ok=True)
    summaries: list[dict[str, Any]] = []
    with args.ownership_lock.open("a+") as owner:
        try:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise SystemExit("another Spec 114 MiniNDN owner is active") from exc
        for cell_id in cells:
            manifest = candidate.inspect(manifest_path)
            if formal:
                assert_candidate_unchanged(manifest, ndn_svs)
                update_run_once(manifest_path, cell_id, "pending", "running")
                manifest = candidate.inspect(manifest_path)
                cell_dir = manifest_path.parent / cell_id
            else:
                cell_dir = Path(tempfile.mkdtemp(prefix=f"spec114-{cell_id}-")) / cell_id
            try:
                summary = run_cell(manifest=manifest, cell_id=cell_id, cell_dir=cell_dir,
                                   publish_count=args.publish_count,
                                   settle_ms=args.settle_ms, formal=formal,
                                   node_identity=node_identity, ndn_svs=ndn_svs)
            except Exception as exc:
                summary = {"candidateId": manifest["candidateId"], "cellId": cell_id,
                           "status": "FAILURE", "runtimeError": f"{type(exc).__name__}: {exc}"}
                if not cell_dir.exists():
                    cell_dir.mkdir(parents=True)
                (cell_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            summaries.append(summary)
            if formal:
                update_run_once(manifest_path, cell_id, "running",
                                "complete" if summary["status"] == "SUCCESS" else "failed")
            print("SPEC114_CELL " + json.dumps(summary, sort_keys=True), flush=True)

    if formal:
        aggregate = aggregate_formal_summaries(manifest_path, manifest)
        output = manifest_path.parent / "formal-summary.json"
    else:
        aggregate = {
            "schemaVersion": "spec114-minindn-matrix-v1",
            "candidateId": manifest["candidateId"], "formal": False,
            "cells": summaries, "passed": len(summaries) == len(cells) and
                                        all(item["status"] == "SUCCESS" for item in summaries),
        }
        output = cell_dir.parent / "smoke-summary.json"
        output.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    print(f"SPEC114_MATRIX {output}")
    return 0 if aggregate["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
