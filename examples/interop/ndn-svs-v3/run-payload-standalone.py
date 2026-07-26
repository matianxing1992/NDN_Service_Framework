#!/usr/bin/env python3
"""Run one bounded native C++/NDNts SVS-PS payload interoperability gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Any, Optional


ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(ROOT))

from payload_corpus import classify_receipts, create_corpus, load_manifest  # noqa: E402


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_identity(path: Path) -> dict[str, Any]:
    def git(*arguments: str) -> str:
        return subprocess.check_output(["git", *arguments], cwd=path, text=True).strip()

    status = git("status", "--porcelain=v1", "--untracked-files=all")
    return {
        "path": str(path),
        "head": git("rev-parse", "HEAD"),
        "tree": git("rev-parse", "HEAD^{tree}"),
        "clean": not status,
        "status": status.splitlines(),
    }


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    events = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError as error:
            events.append({"event": "infra-error", "stage": "orchestration",
                           "reason": f"invalid JSONL {path.name}:{number}: {error}"})
    return events


def stop_process(process: Optional[subprocess.Popen], grace: float = 2.0,
                 privileged: bool = False) -> Optional[int]:
    if process is None:
        return None
    if process.poll() is None:
        if privileged:
            subprocess.run(["sudo", "-n", "kill", "-INT", "--", f"-{process.pid}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           check=False)
        else:
            process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            if privileged:
                subprocess.run(["sudo", "-n", "kill", "-KILL", "--", f"-{process.pid}"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               check=False)
            else:
                process.kill()
            process.wait(timeout=grace)
    return process.returncode


def wait_for_nfd(deadline_seconds: float = 5.0) -> None:
    deadline = time.monotonic() + deadline_seconds
    while time.monotonic() < deadline:
        if subprocess.run(["nfdc", "status"], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, check=False).returncode == 0:
            return
        time.sleep(0.1)
    raise RuntimeError("host NFD did not become ready")


def run(args: argparse.Namespace) -> dict[str, Any]:
    output = args.output.expanduser().resolve()
    if output.exists():
        raise RuntimeError(f"output directory already exists: {output}")
    output.mkdir(parents=True)
    corpus_dir = output / "corpus"
    create_corpus(corpus_dir)
    manifest_path = corpus_dir / "manifest.json"
    manifest = load_manifest(manifest_path)

    cpp = args.cpp_peer.expanduser().resolve()
    ndnts = args.ndnts_peer.expanduser().resolve()
    node = args.node_bin.expanduser().resolve()
    ndn_svs = args.ndn_svs.expanduser().resolve()
    for path, executable in ((cpp, True), (ndnts, False), (node, True)):
        if not path.is_file() or (executable and not os.access(path, os.X_OK)):
            raise RuntimeError(f"required peer input is unavailable: {path}")
    ndn_svs_identity = git_identity(ndn_svs)
    ndn_svs_library = ndn_svs / "build/libndn-svs.so"
    ndnts_lock = ROOT / "ndnts/package-lock.json"
    if not ndn_svs_identity["clean"] or not ndn_svs_library.is_file() or \
       not ndnts_lock.is_file():
        raise RuntimeError("dependency identity is unavailable or NDN-SVS is dirty")

    started_nfd = subprocess.run(["nfdc", "status"], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, check=False).returncode != 0
    capture: Optional[subprocess.Popen] = None
    cpp_process: Optional[subprocess.Popen] = None
    ndnts_process: Optional[subprocess.Popen] = None
    open_logs: list[Any] = []
    timed_out = False
    started = time.monotonic()
    sync_prefix = f"/ndn/ndnsf/spec117/standalone/{os.getpid()}"
    cpp_events = output / "cpp.jsonl"
    ndnts_events = output / "ndnts.jsonl"
    try:
        if started_nfd:
            with (output / "nfd-start.log").open("wb") as log:
                completed = subprocess.run(["nfd-start"], stdout=log,
                                           stderr=subprocess.STDOUT, check=False)
            if completed.returncode != 0:
                raise RuntimeError(f"nfd-start failed with {completed.returncode}")
        wait_for_nfd()
        with (output / "strategy.txt").open("w", encoding="utf-8") as log:
            subprocess.run(["nfdc", "strategy", "set", sync_prefix,
                            "/localhost/nfd/strategy/multicast"], check=True,
                           stdout=log, stderr=subprocess.STDOUT)
        (output / "rib-before.txt").write_text(
            subprocess.check_output(["nfdc", "route", "list"], text=True), encoding="utf-8")

        if subprocess.run(["sudo", "-n", "true"], check=False,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            capture_log = (output / "ndndump.log").open("wb")
            open_logs.append(capture_log)
            capture = subprocess.Popen(["sudo", "-n", "ndndump", "-i", "any", "-t", "-v"],
                                       stdout=capture_log, stderr=subprocess.STDOUT,
                                       start_new_session=True)

        cpp_command = [
            str(cpp), "--mode", "payload", "--version", "v3",
            "--sync-prefix", sync_prefix, "--node-prefix", "/cpp",
            "--manifest", str(manifest_path), "--publish-interval-ms", "50",
            "--start-delay-ms", "1500", "--settle-ms", str(args.settle_ms),
            "--events", str(cpp_events),
        ]
        ndnts_command = [
            str(node), str(ndnts), "--mode", "payload", "--version", "v3",
            "--sync-prefix", sync_prefix, "--node-prefix", "/ndnts",
            "--manifest", str(manifest_path), "--publish-interval-ms", "50",
            "--start-delay-ms", "1500", "--settle-ms", str(args.settle_ms),
            "--events", str(ndnts_events),
        ]
        (output / "commands.json").write_text(json.dumps({
            "cpp": cpp_command, "ndnts": ndnts_command,
        }, indent=2) + "\n", encoding="utf-8")
        cpp_env = dict(os.environ)
        cpp_env["LD_LIBRARY_PATH"] = str(ndn_svs / "build") + (
            ":" + cpp_env["LD_LIBRARY_PATH"] if cpp_env.get("LD_LIBRARY_PATH") else "")
        ndnts_env = dict(os.environ)
        ndnts_env.setdefault("NDNTS_UPLINK", "unix:///run/nfd/nfd.sock")
        cpp_stdout = (output / "cpp.stdout").open("wb")
        cpp_stderr = (output / "cpp.stderr").open("wb")
        ndnts_stdout = (output / "ndnts.stdout").open("wb")
        ndnts_stderr = (output / "ndnts.stderr").open("wb")
        open_logs.extend((cpp_stdout, cpp_stderr, ndnts_stdout, ndnts_stderr))
        cpp_process = subprocess.Popen(cpp_command, env=cpp_env,
                                       stdout=cpp_stdout, stderr=cpp_stderr)
        ndnts_process = subprocess.Popen(ndnts_command, env=ndnts_env,
                                         stdout=ndnts_stdout, stderr=ndnts_stderr)
        deadline = time.monotonic() + args.timeout_seconds
        for process in (cpp_process, ndnts_process):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            try:
                process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                timed_out = True
                break
        (output / "rib-after.txt").write_text(
            subprocess.check_output(["nfdc", "route", "list"], text=True), encoding="utf-8")
    finally:
        cpp_rc = stop_process(cpp_process)
        ndnts_rc = stop_process(ndnts_process)
        try:
            stop_process(capture, privileged=True)
        finally:
            for log in open_logs:
                log.close()
            if started_nfd:
                with (output / "nfd-stop.log").open("wb") as log:
                    subprocess.run(["nfd-stop"], stdout=log, stderr=subprocess.STDOUT,
                                   check=False)

    events = read_jsonl(cpp_events) + read_jsonl(ndnts_events)
    if timed_out:
        events.append({"event": "infra-error", "stage": "orchestration",
                       "reason": "standalone peer deadline exceeded"})
    if cpp_rc not in (0, 1, None) or ndnts_rc not in (0, 1, None):
        events.append({"event": "infra-error", "stage": "orchestration",
                       "reason": f"peer return codes cpp={cpp_rc} ndnts={ndnts_rc}"})
    summary = classify_receipts(manifest, events)
    summary.update({
        "cellId": "standalone",
        "syncPrefix": sync_prefix,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "returnCodes": {"cpp": cpp_rc, "ndnts": ndnts_rc},
        "timedOut": timed_out,
        "corpusManifestSha256": sha256_file(manifest_path),
        "peers": {
            "cpp": {"path": str(cpp), "sha256": sha256_file(cpp)},
            "ndnts": {"path": str(ndnts), "sha256": sha256_file(ndnts),
                       "language": "TypeScript"},
        },
        "ndnSvs": {
            **ndn_svs_identity,
            "libraryPath": str(ndn_svs_library),
            "librarySha256": sha256_file(ndn_svs_library),
        },
        "ndntsRuntime": {
            "nodePath": str(node),
            "nodeVersion": subprocess.check_output([str(node), "--version"],
                                                   text=True).strip(),
            "nodeSha256": sha256_file(node),
            "packageLockPath": str(ndnts_lock),
            "packageLockSha256": sha256_file(ndnts_lock),
        },
        "packetEvidence": ({"path": str(output / "ndndump.log"),
                            "sha256": sha256_file(output / "ndndump.log")}
                           if (output / "ndndump.log").is_file() else None),
    })
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpp-peer", type=Path,
                        default=ROOT / "build/svs3-peer")
    parser.add_argument("--ndnts-peer", type=Path,
                        default=ROOT / "ndnts/svs3-peer.ts")
    parser.add_argument("--node-bin", type=Path,
                        default=Path("/home/tianxing/.local/node-v22.23.1/bin/node"))
    parser.add_argument("--ndn-svs", type=Path,
                        default=Path("/home/tianxing/NDN/ndn-svs"))
    parser.add_argument("--settle-ms", type=int, default=8000)
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    args = parser.parse_args()
    try:
        summary = run(args)
    except Exception as error:
        print(f"spec117 standalone infrastructure failure: {error}", file=sys.stderr)
        return 2
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
