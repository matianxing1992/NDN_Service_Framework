#!/usr/bin/env python3
"""Qualify the Spec 261 hybrid split across two MiniNDN Provider nodes.

The maintained process driver owns the catalog, credentials, protocol and
oracle. This runner only replaces the transport: each actor executes inside its
MiniNDN node's network namespace and talks to that node's own NFD, so the rank
hand-off crosses real links instead of one host's Unix socket.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
TOPOLOGY = ROOT / "Experiments/Topology/spec261-hybrid.conf"
DRIVER = ROOT / "tests/standalone/run-spec261-twoprovider-process.py"
ROLE_NODE = {
    "requester": "requester",
    "provider": "provider",
    "provider2": "provider2",
    "controller": "control",
    "authority": "control",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--variant", choices=["26n", "11n"], default="26n")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("MiniNDN qualification requires root")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    scratch = args.scratch_root.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    receipt = output / "result.json"

    installed = {
        name: args.prefix / path for name, path in {
            "requester": "bin/DI_NativeRequester",
            "provider": "bin/di-native-provider",
            "controller": "bin/App_ServiceController",
            "authority": "bin/DI_NativeArtifactAuthority",
            "worker": "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker",
        }.items()
    }
    missing = [str(path) for path in installed.values() if not path.is_file()]
    if missing:
        raise RuntimeError("installed native entry is missing: " + ",".join(missing))

    result = {
        "schema": "spec261-minindn-hybrid-v1",
        "status": "IN_PROGRESS",
        "variant": args.variant,
        "topologySha256": digest(TOPOLOGY),
        "harnessSha256": digest(Path(__file__)),
        "installed": {name: {"path": str(path), "sha256": digest(path)}
                      for name, path in installed.items()},
        "phases": [],
        "forcedCleanup": False,
    }

    def persist() -> None:
        receipt.write_text(json.dumps(result, indent=1, sort_keys=True), encoding="utf-8")

    persist()

    # Mini-NDN installs its Python package outside the default site-packages on
    # this host (a versioned prefix). Resolve it explicitly before importing.
    candidates = [os.environ.get("NDNSF_MININDN_PYTHONPATH", "")]
    for pattern in ("/usr/local/lib/ndnsf/mini-ndn/*/python*/site-packages",
                    "/home/*/NDN/mini-ndn"):
        candidates.extend(str(Path(item).parent if Path(item).name == "minindn"
                               else item)
                          for item in glob.glob(pattern))
    for candidate in candidates:
        if candidate and (Path(candidate) / "minindn" / "__init__.py").is_file():
            sys.path.insert(0, candidate)
            result["minindnPath"] = candidate
            break
    try:
        from minindn.minindn import Minindn
        from minindn.apps.app_manager import AppManager
        from minindn.apps.nfd import Nfd
        from minindn.helpers.nfdc import Nfdc
    except ModuleNotFoundError as error:
        # The MiniNDN Python package is a host prerequisite, like Mininet
        # itself. Report the unmet prerequisite instead of a traceback.
        result["status"] = "UNQUALIFIED"
        result["unmetPrerequisite"] = "python-module:" + error.name
        persist()
        return 2

    ndn = None
    manager = None
    driver_process = None
    streams = []
    started = time.monotonic()

    def interrupted(signum, frame):
        raise KeyboardInterrupt("signal " + str(signum))

    previous = {number: signal.signal(number, interrupted)
                for number in (signal.SIGINT, signal.SIGTERM)}

    def stop(process) -> None:
        if process is None or process.poll() is not None:
            return
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            result["forcedCleanup"] = True
            process.kill()
            process.wait(timeout=10)

    try:
        ndn = Minindn(parser=parser, topoFile=str(TOPOLOGY),
                      workDir=str(scratch / "nodes"), controller=None)
        if Minindn.ndnSecurityDisabled:
            raise RuntimeError("dummy keychain cannot qualify signature behavior")
        ndn.start()
        manager = AppManager(ndn, [], Nfd)
        transports: dict[str, str] = {}
        pids: dict[str, int] = {}
        for node in ndn.net.hosts:
            manager.startOnNode(node, logLevel="NONE", csSize=0)
            app = manager[node.name]
            transports[node.name] = "unix://" + app.sockFile
            pids[node.name] = app.process.pid
            result["phases"].append({"phase": "nfd-ready", "node": node.name,
                                     "pid": app.process.pid})
        persist()

        # Static reachability for the shared /example/hello namespace: every
        # node gets a route through each of its links. The small topology has no
        # NLSR, and a single missing route would look like a protocol failure.
        hosts = list(ndn.net.hosts)
        for node in hosts:
            for other in hosts:
                if other is node:
                    continue
                face = Nfdc.createFace(node, other.IP())
                if str(face) == "-1":
                    raise RuntimeError("cannot create a face from " + node.name +
                                       " to " + other.name)
                node.cmd("nfdc route add /example/hello " + str(face))
        result["phases"].append({"phase": "routes-ready", "nodes": len(hosts)})
        persist()

        environment = os.environ.copy()
        for role, node in ROLE_NODE.items():
            environment["SPEC261_TRANSPORT_" + role] = transports[node]
            environment["SPEC261_NETNS_PID_" + role] = str(pids[node])
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        run_root = output / "driver"
        log_path = output / "driver.log"
        with log_path.open("wb") as stream:
            streams.append(stream)
            driver_process = subprocess.Popen(
                [sys.executable, str(DRIVER), "--build", str(ROOT / "build"),
                 "--no-local-nfd", "--variant", args.variant,
                 "--run-root", str(run_root)],
                cwd=ROOT, env=environment, stdout=stream,
                stderr=subprocess.STDOUT)
            driver_process.wait(timeout=args.timeout)
        result["driverExit"] = driver_process.returncode
        requester_log = run_root / "requester.log"
        text = requester_log.read_text(errors="replace") if requester_log.exists() else ""
        result["phases"].append({"phase": "driver-finished",
                                 "exit": driver_process.returncode,
                                 "requesterLog": str(requester_log)})
        passed = "NATIVE_NUMERICAL_ORACLE_PASS" in text and driver_process.returncode == 0
        result["status"] = "PASS" if passed else "FAIL"
        if not passed:
            result["firstFailure"] = "results/spec261-minindn: driver did not report the oracle pass"
    finally:
        stop(driver_process)
        if manager is not None:
            try:
                manager.cleanup()
            except Exception:
                result["forcedCleanup"] = True
        if ndn is not None:
            try:
                ndn.stop()
            except Exception:
                result["forcedCleanup"] = True
        for stream in streams:
            try:
                stream.close()
            except Exception:
                pass
        for number, handler in previous.items():
            signal.signal(number, handler)
        result["elapsedSeconds"] = round(time.monotonic() - started, 3)
        persist()
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
