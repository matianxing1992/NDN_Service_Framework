#!/usr/bin/env python3
"""Qualify the Spec 261 component or hybrid split across two MiniNDN Providers.

The maintained process driver owns the catalog, credentials, protocol and
oracle. This runner only replaces the transport: each actor executes inside its
MiniNDN node's network namespace and talks to that node's own NFD. The
dependency handoff crosses the emulated links.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
TOPOLOGY = ROOT / "Experiments/Topology/spec261-hybrid.conf"
WIRED_TOPOLOGY = ROOT / "Experiments/Topology/spec270-wired-star.conf"
DRIVER = ROOT / "tests/standalone/run-spec261-yolo-twoprovider-process.py"
ROLE_NODE = {
    "requester": "requester",
    "provider": "provider",
    "provider2": "provider2",
    "controller": "control",
    "authority": "control",
}
WIRED_ROLE_NODE = {
    "requester": "requester",
    "provider": "provider0",
    "provider2": "provider1",
    "controller": "requester",
    "authority": "requester",
}
ACTOR_PREFIX = {
    "requester": "/example/hello/user",
    "provider": "/example/hello/provider",
    "provider2": "/example/hello/provider2",
    "controller": "/example/hello/controller",
    "authority": "/example/hello/authority",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_wired_execution(run_root: Path, mode: str) -> dict:
    expected = ({"provider": {"Prefix"}, "provider2": {"Suffix"}} if mode == "component"
                else {"provider": {"Prefix", "Rank0"}, "provider2": {"Rank1", "Suffix"}})
    placements = {}
    requests = set()
    for actor, roles in expected.items():
        path = run_root / (actor + "-statistics.jsonl")
        actor_requests = set()
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row.get("type") != "request" or not row.get("completionAdmitted"):
                continue
            if row.get("provider") != ACTOR_PREFIX[actor] or row.get("failureReason"):
                raise ValueError(f"invalid execution receipt from {actor}")
            actor_requests.add((row["requestId"], row["attemptEpoch"], row["planDigest"]))
        if len(actor_requests) != 1:
            raise ValueError(f"{actor} lacks a unique admitted request receipt")
        requests.update(actor_requests)
        request_id, attempt, _ = next(iter(actor_requests))
        executed = set()
        with (run_root / (actor + ".log")).open() as log:
            for line in log:
                if "NDNSF_PHASE_TIMING " not in line or " phase=roleExecutionEnd " not in line:
                    continue
                row = dict(field.split("=", 1) for field in line.split() if "=" in field)
                if row.get("requestId") == request_id and row.get("attempt") == str(attempt):
                    executed.add(row["executionRole"].lstrip("/"))
        if executed != roles:
            raise ValueError(f"{actor} executed {sorted(executed)}, expected {sorted(roles)}")
        placements[actor] = sorted(executed)
    if len(requests) != 1:
        raise ValueError("Provider receipts do not identify the same sealed request and attempt")
    timing = json.loads((run_root / "request-timing.json").read_text())
    return {"providerRoles": placements, "request": timing}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--variant", choices=["26n", "11n"], default="26n")
    parser.add_argument("--mode", choices=("component", "hybrid"), default="hybrid")
    parser.add_argument("--cache", type=Path, help="prepared assets for the selected YOLO variant")
    parser.add_argument("--initializer-sha256", help="explicit sha256: digest of prepared weights")
    parser.add_argument("--wired-star", action="store_true", help="use the Spec 270 star topology")
    parser.add_argument("--topology", type=Path, help="override the selected layout's topology file")
    parser.add_argument("--case", action="append", default=[],
                        help="case directory under the model assets (repeatable)")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    if args.wired_star and args.case:
        parser.error("--wired-star selects the tracked fixed input and oracle")
    topology = args.topology or (WIRED_TOPOLOGY if args.wired_star else TOPOLOGY)
    role_node = WIRED_ROLE_NODE if args.wired_star else ROLE_NODE
    if os.geteuid() != 0:
        parser.error("MiniNDN qualification requires root")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    scratch = args.scratch_root.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    receipt = output / "result.json"

    prefix = args.prefix.resolve()
    installed = {
        name: prefix / path for name, path in {
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
        "mode": args.mode,
        "variant": args.variant,
        "expectedInitializerSha256": args.initializer_sha256,
        "topologySha256": digest(topology),
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
        if shutil.which("infoconv") is None:
            raise RuntimeError("MiniNDN NFD configuration requires infoconv on PATH")
        ndn = Minindn(parser=parser, topoFile=str(topology),
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
        def peer_ip(node, other):
            for intf in node.intfList():
                link = intf.link
                if link is None:
                    continue
                a, b = link.intf1, link.intf2
                peer = b if a is intf else a
                if peer.node is other:
                    return str(peer.ip)
            return None

        def nfdc(node, *args):
            env = dict(os.environ, HOME=str(manager[node.name].homeDir),
                       NDN_CLIENT_TRANSPORT=transports[node.name])
            stdout, stderr, code = node.pexec(["timeout", "5", "nfdc", *args], env=env)
            if code:
                raise RuntimeError(f"nfdc on {node.name}: {stdout}{stderr}")
            return stdout

        hosts = list(ndn.net.hosts)
        routing = {}
        for node in hosts:
            nfdc(node, "strategy", "set", "/example/hello", "/localhost/nfd/strategy/multicast")
            faces = {}
            for other in hosts:
                if other is node:
                    continue
                address = peer_ip(node, other)
                if address is None:
                    # Not a direct neighbor: reachability comes from the
                    # neighbors' own faces and the multicast strategy.
                    continue
                face = Nfdc.createFace(node, address)
                if str(face) == "-1":
                    raise RuntimeError("cannot create a face from " + node.name +
                                       " to " + other.name)
                faces[other.name] = str(face)
                nfdc(node, "route", "add", "/example/hello", str(face))
            # Route exact Producer and certificate prefixes by next hop. The
            # shared discovery prefix continues to multicast to neighbors.
            for role, destination in role_node.items():
                if destination == node.name:
                    continue
                face = faces.get(destination)
                if face is None and args.wired_star:
                    face = faces["router"]
                if face is None:
                    continue
                prefix_name = ACTOR_PREFIX[role]
                for namespace in (prefix_name, prefix_name + "/KEY"):
                    nfdc(node, "route", "add", namespace, face)
            if args.wired_star:
                routing[node.name] = {"faces": nfdc(node, "face", "list"),
                                      "fib": nfdc(node, "fib", "list")}
        if args.wired_star:
            (output / "routing.json").write_text(json.dumps(routing, indent=1))
        result["phases"].append({"phase": "routes-ready", "nodes": len(hosts)})
        persist()

        environment = os.environ.copy()
        environment["NDNSF_TEST_RUNTIME_PREFIX"] = str(prefix)
        environment.setdefault("NDN_LOG", "ndnsf.di.NativeRequester=WARN")
        if args.wired_star:
            environment["NDNSF_PHASE_TIMING"] = "1"
            environment["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = "1"
            environment["NDN_LOG"] += ":ndn_service_framework.TimelineTrace=WARN"
        for role, node in role_node.items():
            environment["SPEC261_TRANSPORT_" + role] = transports[node]
            environment["SPEC261_NETNS_PID_" + role] = str(pids[node])
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        run_root = output / "driver"
        log_path = output / "driver.log"
        with log_path.open("wb") as stream:
            streams.append(stream)
            driver_process = subprocess.Popen(
                [sys.executable, str(DRIVER), "--no-local-nfd", "--variant", args.variant,
                 "--mode", args.mode, "--run-root", str(run_root)] +
                (["--frozen-fixtures"] if args.wired_star else []) +
                (["--cache", str(args.cache.resolve())] if args.cache else []) +
                (["--initializer-sha256", args.initializer_sha256] if args.initializer_sha256 else []) +
                [item for case in args.case for item in ("--case", case)],
                cwd=scratch, env=environment, stdout=stream,
                stderr=subprocess.STDOUT)
            driver_process.wait(timeout=args.timeout)
        result["driverExit"] = driver_process.returncode
        requester_log = run_root / "requester.log"
        case_logs = [run_root / f"requester-{case}.log" for case in args.case]
        texts = [requester_log.read_text(errors="replace") if requester_log.exists() else ""]
        texts += [log.read_text(errors="replace") if log.exists() else "" for log in case_logs]
        result["phases"].append({"phase": "driver-finished",
                                 "exit": driver_process.returncode,
                                 "requesterLogs": [str(requester_log)] + [str(log) for log in case_logs]})
        passed = driver_process.returncode == 0 and bool(args.case or texts[0]) and all(
            "NATIVE_NUMERICAL_ORACLE_PASS" in text for text in
            (texts[1:] if args.case else texts[:1]))
        if args.wired_star and (run_root / "actor-cleanup.json").is_file():
            cleanup = json.loads((run_root / "actor-cleanup.json").read_text())
            result["actorCleanup"] = cleanup
            result["forcedCleanup"] |= any(row["forced"] or not row["reaped"] for row in cleanup)
        if passed and args.wired_star:
            if len(result.get("actorCleanup", [])) != len(role_node):
                raise ValueError("incomplete native actor cleanup receipt")
            result.update(check_wired_execution(run_root, args.mode))
        result["status"] = "PASS" if passed else "FAIL"
        if not passed:
            result["firstFailure"] = "native driver did not report the oracle pass"
    except (Exception, KeyboardInterrupt) as error:
        result["status"] = "FAIL"
        result.setdefault("firstFailure", f"{type(error).__name__}: {error}")
        raise
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
        if result["forcedCleanup"]:
            result["status"] = "FAIL"
            result.setdefault("firstFailure", "MiniNDN cleanup did not complete normally")
        result["elapsedSeconds"] = round(time.monotonic() - started, 3)
        persist()
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
