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
DRIVER = ROOT / "tests/standalone/run-spec261-yolo-twoprovider-process.py"
ROLE_NODE = {
    "requester": "requester",
    "provider": "provider",
    "provider2": "provider2",
    "controller": "control",
    "authority": "control",
}
ACTOR_PREFIXES = {
    "requester": ("/example/hello/user",),
    "provider": ("/example/hello/provider",),
    "provider2": ("/example/hello/provider2",),
    "control": ("/example/hello/controller", "/example/hello/authority"),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_ffn_handoffs(run_root: Path, requester_log: str, warm: bool) -> dict:
    selection = "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId="
    sessions = [line.split(selection, 1)[1].split()[0]
                for line in requester_log.splitlines() if selection in line]
    if len(sessions) != 1:
        raise RuntimeError("hybrid run did not seal exactly one request")
    session = sessions[0]
    owners = {"Prefix": "provider", "Rank0": "provider",
              "Rank1": "provider2", "Suffix": "provider2"}
    edges = [
        ("Prefix", "Rank0", "Prefix/RUN", "hidden", 66),
        ("Prefix", "Rank1", "Prefix/RUN", "hidden", 66),
        ("Rank1", "Rank0", "Rank1/RUN/0", "mid", 67),
        ("Rank0", "Rank1", "Rank0/RUN/0", "mid", 67),
        ("Rank1", "Rank0", "Rank1/RUN/1", "last", 64),
        ("Rank0", "Suffix", "Rank0/SUM/1", "last", 64),
        ("Prefix", "Suffix", "Prefix/RUN", "hidden", 66),
    ]
    rows = []
    fetch_events = {"Rank0": [], "Rank1": []}
    middle_cache = {}
    terminals = []
    marker = "NDNSF_DI_DEPENDENCY_OBJECT "
    for file in ("provider.log", "provider2.log", "provider2-fault.log"):
        path = run_root / file
        if not path.exists():
            continue
        node = path.stem.split("-", 1)[0]
        with path.open(errors="replace") as log:
            for line in log:
                if marker in line and f" session={session} " in line:
                    fields = dict(field.split("=", 1)
                                  for field in line.split(marker, 1)[1].split())
                    if fields.get("status") == "ok":
                        rows.append((node, fields))
                        role = fields.get("consumer")
                        if (role in fetch_events and node == owners[role] and
                                fields.get("direction") == "fetch-exact-ndn"):
                            fetch_events[role].append(("validated", fields["planned_name"]))
                elif ("stage=DEPENDENCY_FETCH status=begin " in line and
                      f" sessionId={session} " in line):
                    fields = dict(field.split("=", 1) for field in
                                  line.split("status=begin ", 1)[1].split())
                    role = fields["role"]
                    if role in fetch_events and node == owners[role]:
                        fetch_events[role].append(("begin", fields["plannedName"]))
                elif f"stage=TERMINAL status=observed requestId={session} " in line:
                    fields = dict(field.split("=", 1) for field in
                                  line.split("status=observed ", 1)[1].split())
                    terminals.append((node, fields["provider"], fields["role"]))
                elif ("NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_ACQUIRE_DONE " in line and
                      f" requestId={session} " in line):
                    role = "Rank0" if node == "provider" else "Rank1"
                    if f" role={role} " in line:
                        middle_cache[role] = line.split(" detail=", 1)[1].split()[0]
    verified = []
    for producer, consumer, step, port, size in edges:
        pair = [row for node, row in rows if row.get("producer") == producer and
                row.get("consumer") == consumer and
                f"/8=step/8={step.replace('/', '%2F')}/" in row.get("planned_name", "") and
                f"/8=port/8={port}/" in row["planned_name"] and
                row.get("encoded_payload_bytes") == str(size) and
                ((node == owners[producer] and row["direction"] == "publish-exact-ndn") or
                 (node == owners[consumer] and row["direction"] == "fetch-exact-ndn"))]
        if ({row["direction"] for row in pair} != {"publish-exact-ndn", "fetch-exact-ndn"} or
                len({row["planned_name"] for row in pair}) != 1 or
                not pair[0]["planned_name"].startswith(
                    f"/8=example/8=hello/8={owners[producer]}/")):
            raise RuntimeError(f"missing signed handoff from {producer} to {consumer} for {step}")
        verified.append({"producer": producer, "consumer": consumer, "step": step, "port": port,
                         "bytes": size, "name": pair[0]["planned_name"]})
    expected_cache = {"template-hit", "assembled-disk-hit"} if warm else {"assembled-built"}
    if set(middle_cache) != {"Rank0", "Rank1"} or not set(middle_cache.values()) <= expected_cache:
        raise RuntimeError(f"unexpected middle-rank artifact cache path: {middle_cache}")
    for role, actual in fetch_events.items():
        expected = [(event, edge["name"]) for edge in verified if edge["consumer"] == role
                    for event in ("begin", "validated")]
        if actual != expected:
            raise RuntimeError(f"{role} did not fetch only the active step's remote operand")
    if (terminals != [("provider2", "/example/hello/provider2", "Suffix")] or
            "NATIVE_NUMERICAL_ORACLE_PASS tensor=y values=3" not in requester_log.splitlines()):
        raise RuntimeError("hybrid run did not return the verified y output from Suffix")
    return {"request": session, "handoffs": verified, "middleCache": middle_cache,
            "middleFetchOrder": fetch_events, "terminalRole": "Suffix", "outputTensor": "y"}


def check_ffn_cancellation(run_root: Path) -> dict:
    session = (run_root / "cancelled-request.txt").read_text().strip()
    requester = (run_root / "requester-hybrid-cancel.log").read_text(errors="replace")
    marker = f"NDNSF_DI_NATIVE_CANCELLATION_RECEIPT requestId={session} "
    receipt = [line.split(marker, 1)[1] for line in requester.splitlines() if marker in line]
    if len(receipt) != 1:
        raise RuntimeError("cancelled request has no unique native cancellation receipt")
    counts = {key: int(value) for key, value in
              (field.split("=", 1) for field in receipt[0].split())}
    if (counts.get("expected") != 4 or counts.get("drained") != 4 or
            counts.get("fenced", 0) < 2 or
            f"NATIVE_REQUEST_CANCEL_DRAINED request={session}" not in requester or
            "NATIVE_REQUEST_INTERRUPTED" not in requester or
            (run_root / "requester/output-hybrid-cancel.bin").exists()):
        raise RuntimeError(f"incomplete cancellation or drain receipt: {counts}")
    left = (run_root / "provider.log").read_text(errors="replace").splitlines()
    right = (run_root / "provider2-fault.log").read_text(errors="replace").splitlines()
    if not any(session in line and "status=begin" in line and " role=Rank0 " in line and
               "/8=step/8=Rank1%2FRUN%2F0/" in line for line in left):
        raise RuntimeError("Rank0 did not wait on the withheld Rank1 partial")
    for line in left + right:
        if session not in line:
            continue
        if ("stage=DEPENDENCY_FETCH status=begin " in line and " role=Rank0 " in line and
                "/8=step/8=Rank1%2FRUN%2F1/" in line):
            raise RuntimeError("Rank0 started the next window with its first operand missing")
        if ("direction=publish-exact-ndn" in line and
                any(step in line for step in ("Rank1%2FRUN%2F0", "Rank0%2FSUM%2F1"))):
            raise RuntimeError("cancelled rank published the withheld or dependent tensor")
        if "stage=EXECUTION_COMPLETED" in line and " role=Suffix " in line:
            raise RuntimeError("suffix completed with a missing rank operand")
    return {"request": session, **counts}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--variant", choices=["26n", "11n", "ffn"], default="26n")
    parser.add_argument("--ffn-fixture", type=Path, help="output of prepare-ffn.py")
    parser.add_argument("--ffn-cancel", action="store_true")
    parser.add_argument("--topology", type=Path, default=TOPOLOGY,
                        help="MiniNDN topology file (default: the maintained hybrid topology)")
    parser.add_argument("--case", action="append", default=[],
                        help="case directory under the model assets (repeatable)")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--nfd-log-level", default="NONE", choices=("NONE", "WARN", "DEBUG"))
    args = parser.parse_args()
    if (args.variant == "ffn") != (args.ffn_fixture is not None):
        parser.error("--variant ffn and --ffn-fixture must be supplied together")
    if args.ffn_fixture and args.case:
        parser.error("--case selects YOLO samples")
    if args.ffn_cancel and not args.ffn_fixture:
        parser.error("--ffn-cancel requires the FFN fixture")
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
    if args.ffn_cancel:
        installed["fault-provider"] = prefix / "bin/di-native-fault-provider"
    missing = [str(path) for path in installed.values() if not path.is_file()]
    if missing:
        raise RuntimeError("installed native entry is missing: " + ",".join(missing))

    result = {
        "schema": "spec263-minindn-hybrid-v1" if args.ffn_fixture else "spec261-minindn-hybrid-v1",
        "status": "IN_PROGRESS",
        "variant": args.variant,
        "topologySha256": digest(args.topology),
        "harnessSha256": digest(Path(__file__)),
        "installed": {name: {"path": str(path), "sha256": digest(path)}
                      for name, path in installed.items()},
        "phases": [],
        "forcedCleanup": False,
    }
    if args.ffn_fixture:
        result["fixture"] = {name: digest(args.ffn_fixture / name)
                             for name in ("model.onnx", "input.f32", "oracle.json")}

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
        ndn = Minindn(parser=parser, topoFile=str(args.topology),
                      workDir=str(scratch / "nodes"), controller=None)
        if Minindn.ndnSecurityDisabled:
            raise RuntimeError("dummy keychain cannot qualify signature behavior")
        ndn.start()
        manager = AppManager(ndn, [], Nfd)
        transports: dict[str, str] = {}
        pids: dict[str, int] = {}
        for node in ndn.net.hosts:
            manager.startOnNode(node, logLevel=args.nfd_log_level, csSize=0)
            app = manager[node.name]
            if app.process.poll() is not None:
                raise RuntimeError(f"NFD exited on {node.name}; see {app.logFile}")
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

        hosts = list(ndn.net.hosts)
        for node in hosts:
            node.cmd("nfdc strategy set /example/hello /localhost/nfd/strategy/multicast")
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
                node.cmd("nfdc route add /example/hello " + str(face))
                # Keep discovery multicast and send Producer-owned objects along direct links.
                for actor_prefix in ACTOR_PREFIXES.get(other.name, ()):
                    node.cmd("nfdc route add " + actor_prefix + " " + str(face))
        result["phases"].append({"phase": "routes-ready", "nodes": len(hosts)})
        persist()

        environment = os.environ.copy()
        environment["NDNSF_TEST_RUNTIME_PREFIX"] = str(prefix)
        environment.setdefault("NDN_LOG", "ndnsf.di.NativeRequester=WARN")
        for role, node in ROLE_NODE.items():
            environment["SPEC261_TRANSPORT_" + role] = transports[node]
            environment["SPEC261_NETNS_PID_" + role] = str(pids[node])
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        run_root = output / "driver"
        log_path = output / "driver.log"
        with log_path.open("wb") as stream:
            streams.append(stream)
            driver_process = subprocess.Popen(
                [sys.executable, str(DRIVER), "--no-local-nfd", "--variant", args.variant,
                 "--run-root", str(run_root)] +
                (["--cache", str(args.ffn_fixture.resolve())] if args.ffn_fixture else []) +
                (["--ffn-cancel"] if args.ffn_cancel else []) +
                [item for case in args.case for item in ("--case", case)],
                cwd=scratch, env=environment, stdout=stream,
                stderr=subprocess.STDOUT)
            driver_process.wait(timeout=args.timeout)
        result["driverExit"] = driver_process.returncode
        requester_log = run_root / "requester.log"
        phases = ("full", "hybrid-cold", "hybrid-warm") if args.ffn_fixture else args.case
        if args.ffn_cancel:
            phases += ("hybrid-recovery",)
        case_logs = [run_root / f"requester-{case}.log" for case in phases]
        texts = [requester_log.read_text(errors="replace") if requester_log.exists() else ""]
        texts += [log.read_text(errors="replace") if log.exists() else "" for log in case_logs]
        result["phases"].append({"phase": "driver-finished",
                                 "exit": driver_process.returncode,
                                 "requesterLogs": [str(requester_log)] + [str(log) for log in case_logs]})
        passed = driver_process.returncode == 0 and bool(phases or texts[0]) and all(
            "NATIVE_NUMERICAL_ORACLE_PASS" in text for text in
            (texts[1:] if phases else texts[:1]))
        result["status"] = "PASS" if passed else "FAIL"
        if not passed:
            failure = next((line for source in texts for line in source.splitlines()
                            if "NATIVE_REQUEST_STAGE_FAILED" in line), None)
            result["firstFailure"] = failure or "native driver did not report the oracle pass"
        if args.ffn_fixture:
            trials = run_root / "requester-trials.jsonl"
            if trials.exists():
                result["requesterTrials"] = [json.loads(line) for line in trials.read_text().splitlines()]
            cleanup = run_root / "process-cleanup.json"
            if cleanup.exists():
                result["actorCleanup"] = json.loads(cleanup.read_text())
                result["forcedCleanup"] |= any(actor["exit"] in (None, -signal.SIGKILL)
                                               for actor in result["actorCleanup"])
            if args.ffn_cancel and any(trial["phase"] == "hybrid-cancel"
                                       for trial in result.get("requesterTrials", [])):
                result["cancellation"] = check_ffn_cancellation(run_root)
            if passed:
                actor_exits = {actor["pid"]: actor["exit"] for actor in result.get("actorCleanup", [])}
                actors = ["controller", "authority", "provider", "provider2"]
                actors += [f"requester-{phase}" for phase in phases]
                expected_exits = {int((run_root / f"{actor}.pid").read_text()): 0
                                  for actor in actors}
                if args.ffn_cancel:
                    expected_exits[int((run_root / "provider2-fault.pid").read_text())] = 0
                    expected_exits[int((run_root / "requester-hybrid-cancel.pid").read_text())] = 1
                if actor_exits != expected_exits:
                    raise RuntimeError(f"unexpected FFN actor exit status: {actor_exits}")
                result["hybridHandoffs"] = [check_ffn_handoffs(run_root, log, warm)
                                            for log, warm in zip(texts[2:], (False, True))]
                if len({trial["request"] for trial in result["hybridHandoffs"]}) != 2:
                    raise RuntimeError("cold and warm trials reused a request identity")
                if args.ffn_cancel:
                    cancelled = result["cancellation"]
                    recovery = check_ffn_handoffs(run_root, texts[-1], warm=True)
                    earlier = {trial["request"] for trial in result["hybridHandoffs"]}
                    if recovery["request"] in earlier | {cancelled["request"]}:
                        raise RuntimeError("recovery reused a request identity")
                    result["recovery"] = recovery
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
