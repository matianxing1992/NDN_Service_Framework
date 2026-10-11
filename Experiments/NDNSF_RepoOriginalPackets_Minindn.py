#!/usr/bin/env python3
"""Qualify transparent original-packet replicas through actual MiniNDN NFDs."""
import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

from ndn.encoding import Name
from native_install_candidate import validate_spec201_candidate
from native_prefix_roles import configure_prefix_roles, prefix_role_plan

NS = {"n": "ndn:/localhost/nfd/status/1"}
ROOT = Path(__file__).resolve().parents[1]


def covers(prefix, name):
    left, right = Name.from_str(prefix), Name.from_str(name)
    return len(left) <= len(right) and all(bytes(a) == bytes(b) for a, b in zip(left, right))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec201-candidate", type=Path, required=True)
    parser.add_argument("--spec201-candidate-sha256", required=True)
    parser.add_argument("--seed-root", type=Path, required=True)
    parser.add_argument("--prefix-role-plan", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--topology-file", type=Path,
                        default=ROOT / "Experiments/Topology/Spec201_RepoOriginalPackets.conf")
    parser.add_argument("--oracle-binary", type=Path, required=True)
    parser.add_argument("--server-binary", type=Path, default=Path("/usr/local/bin/NDNSF_RepoPacketServer"))
    parser.add_argument("--timeout-s", type=float, default=30.0)
    parser.add_argument("--smoke-test", action="store_true",
                        help="check one replica only; makes no qualification or performance claim")
    args = parser.parse_args()
    if args.timeout_s <= 0:
        parser.error("timeout must be positive")
    candidate = validate_spec201_candidate(args.spec201_candidate,
        args.spec201_candidate_sha256, [args.oracle_binary, args.server_binary],
        selected_profile=("replica-smoke" if args.smoke_test else "replica", vars(args)))
    frozen = {row["path"] for group in ("sources", "oracles", "topologies", "configs")
              for row in candidate[group]}
    seed = args.seed_root.resolve(strict=True)
    index = seed / "packets.tsv"
    rows = [line.split("\t") for line in index.read_text().splitlines()]
    if len(rows) != 3 or any(len(row) != 3 or row[2] != str(i) + ".wire"
                             for i, row in enumerate(rows)):
        raise RuntimeError("invalid complete seed index")
    immutable = [index, seed / "producer.cert", seed / "producer-prefix.txt",
                 args.prefix_role_plan.resolve(strict=True), args.topology_file.resolve(strict=True),
                 Path(__file__).resolve()] + [seed / "originals" / row[2] for row in rows]
    if any(str(path.resolve(strict=True)) not in frozen for path in immutable):
        raise RuntimeError("replica oracle input is not frozen in candidate")
    # Public certificate identity is extracted by the C++ oracle, not inferred
    # from rank or a repository prefix. Its own verifier checks all originals.
    subprocess.run([str(args.oracle_binary), "--verify-original-packets", str(seed),
                    str(seed / "originals")], check=True, timeout=args.timeout_s)
    names = [row[0] for row in rows]
    # The whole common parent includes the semantic object and DATA coordinates.
    prefix = Name.to_str(Name.from_str(names[0])[:-1])
    producer = (seed / "producer-prefix.txt").read_text().strip()
    actors = [producer, "/spec201/r1", "/spec201/r2"]
    expected_roles = json.loads(args.prefix_role_plan.read_text())
    if (not producer or any(not covers(producer, name) for name in names) or
            expected_roles != list(prefix_role_plan(actors=actors, groups=[], data_roots=[prefix]))):
        raise RuntimeError("frozen prefix roles disagree with actual Producer and Repo actors")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)

    children, owned, streams, phases = [], [], [], []
    ndn = None
    result = {"status": "IN_PROGRESS", "candidateId": candidate["candidateId"],
              "phases": phases, "children": children}
    previous_signals = {number: signal.getsignal(number) for number in (signal.SIGTERM, signal.SIGINT)}
    def interrupted(signum, frame):
        raise RuntimeError("replica qualification interrupted by signal " + str(signum))
    for number in previous_signals:
        signal.signal(number, interrupted)

    def launch(node, label, command):
        path = output / (label + ".log")
        stream = path.open("wb")
        streams.append(stream)
        node_env = dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib",
                        NDN_CLIENT_TRANSPORT="unix:///run/nfd/" + node.name + ".sock")
        # getPopen supplies the node HOME; the launcher HOME must not replace it.
        node_env.pop("HOME", None)
        process = getPopen(node, "exec " + shlex.join(list(map(str, command))),
            envDict=node_env, shell=True,
            stdout=stream, stderr=stream)
        record = {"label": label, "pid": process.pid, "exit": None}
        children.append(record)
        owned.append((process, record))
        return process, path

    def run(node, label, command):
        process, path = launch(node, label, command)
        try:
            code = process.wait(timeout=args.timeout_s)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
            raise RuntimeError("command deadline exceeded: " + label)
        if code != 0:
            raise RuntimeError("command failed: " + label + " exit=" + str(code))
        return path.read_bytes()

    def status(node, label):
        return ET.fromstring(run(node, label, ["nfdc", "status", "report", "xml"]))

    def fresh_status(node, label):
        # MustBeFresh still permits an unexpired management dataset. Fence
        # each counter snapshot with a subsequent changed Face dataset.
        previous = ET.tostring(status(node, label + "-cached").find("n:faces", NS))
        deadline = time.monotonic() + args.timeout_s
        attempt = 0
        while time.monotonic() < deadline:
            current = status(node, label + "-fresh-" + str(attempt))
            if ET.tostring(current.find("n:faces", NS)) != previous:
                return current
            attempt += 1
            time.sleep(0.05)
        raise RuntimeError("fresh Face dataset deadline exceeded: " + node.name)

    def require_zero_cs(tree):
        for field in ("capacity", "nEntries"):
            value = tree.find("n:cs/n:" + field, NS)
            if value is None or int(value.text) != 0:
                raise RuntimeError("CS disabled readback failed: " + field)

    def face(tree, node, peer):
        links = node.connectionsTo(peer)
        if len(links) != 1:
            raise RuntimeError("missing or ambiguous physical link to " + peer.name)
        uri = "://" + links[0][1].IP() + ":6363"
        matches = [item for item in tree.findall("n:faces/n:face", NS)
                   if (item.findtext("n:remoteUri", namespaces=NS) or "").endswith(uri)]
        if len(matches) != 1:
            raise RuntimeError("missing or ambiguous physical face to " + peer.name)
        return matches[0]

    def counter(item, direction):
        value = item.find("n:packetCounters/n:" + direction + "Packets/n:nData", NS)
        if value is None:
            raise RuntimeError("missing actual face Data counter")
        return int(value.text)

    def stop(server, label):
        process, _ = server
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            code = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
            raise RuntimeError("server did not stop: " + label)
        next(record for child, record in owned if child is process)["exit"] = code
        if code not in (0, -signal.SIGTERM):
            raise RuntimeError("server failed: " + label + " exit=" + str(code))

    servers = {}
    def start_server(label):
        server = launch(ndn.net[label], label + "-server-" + str(len(children)),
            [args.server_binary, seed / label, "/spec201/" + label, str(1 << 20)])
        servers[label] = server
        deadline = time.monotonic() + args.timeout_s
        while time.monotonic() < deadline:
            if server[0].poll() is not None:
                raise RuntimeError("server exited before readiness: " + label)
            if "REPO_ORIGINAL_PACKETS_READY " in server[1].read_text(errors="replace"):
                return
            time.sleep(0.05)
        raise RuntimeError("server registration deadline exceeded: " + label)

    try:
        from minindn.minindn import Minindn
        from minindn.apps.app_manager import AppManager
        from minindn.apps.nfd import Nfd
        from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
        from minindn.helpers.nfdc import Nfdc
        from minindn.util import getPopen
        # Validate again before any network cleanup/startup side effects.
        validate_spec201_candidate(args.spec201_candidate, args.spec201_candidate_sha256,
                                  [args.oracle_binary, args.server_binary])
        saved_argv = sys.argv
        try:
            sys.argv = [saved_argv[0]]
            Minindn.cleanUp()
            ndn = Minindn(topoFile=str(args.topology_file.resolve()))
        finally:
            sys.argv = saved_argv
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, csSize=0, logLevel="INFO")
        routing = NdnRoutingHelper(ndn.net, Nfdc.PROTOCOL_UDP, "link-state")
        routing.addOrigin([ndn.net["r1"]], [prefix])
        routing.calculateRoutes()
        configure_prefix_roles(ndn, actors=actors, groups=[], data_roots=[prefix],
            expected_roles=expected_roles, receipt=output / "prefix-role-strategies.json")
        start_server("r1")
        start_server("r2")
        user = ndn.net["user"]
        stages = (("first-r1", "r1"),) if args.smoke_test else (
            ("first-r1", "r1"), ("failover-r2", "r2"), ("restart-r1", "r1"))
        window_start = time.monotonic()
        for stage, target in stages:
            if stage == "failover-r2":
                stop(servers.pop("r1"), "r1-first")
            elif stage == "restart-r1":
                start_server("r1")
            tree = status(user, stage + "-user-route-before")
            wanted = face(tree, user, ndn.net[target]).findtext("n:faceId", namespaces=NS)
            if not wanted:
                raise RuntimeError("missing target face identity")
            for entry in tree.findall("n:rib/n:ribEntry", NS):
                existing = entry.findtext("n:prefix", namespaces=NS)
                if existing and any(covers(existing, name) for name in names):
                    for route in entry.findall("n:routes/n:route", NS):
                        run(user, stage + "-remove-" + str(len(list(output.glob(stage + '-remove-*')))),
                            ["nfdc", "route", "remove", "prefix", existing, "nexthop",
                             route.findtext("n:faceId", namespaces=NS), "origin",
                             route.findtext("n:origin", namespaces=NS)])
            run(user, stage + "-add-route", ["nfdc", "route", "add", "prefix", prefix,
                "nexthop", wanted, "origin", "255", "cost", "0"])
            tree = status(user, stage + "-user-route-after")
            for name in names:
                entries = [entry for entry in tree.findall("n:fib/n:fibEntry", NS)
                           if covers(entry.findtext("n:prefix", namespaces=NS), name)]
                longest = max(entries, key=lambda entry: len(Name.from_str(
                    entry.findtext("n:prefix", namespaces=NS))))
                ids = [hop.findtext("n:faceId", namespaces=NS)
                       for hop in longest.findall("n:nextHops/n:nextHop", NS)]
                if ids != [wanted]:
                    raise RuntimeError("packet FIB does not select exactly one intended Repo")
            before = {node: fresh_status(ndn.net[node], stage + "-" + node + "-before")
                      for node in ("user", "r1", "r2")}
            destination = output / stage
            destination.mkdir()
            for name, _, filename in rows:
                wire = run(user, stage + "-fetch-" + filename,
                           ["ndnpeek", "-f", "-w", "5000", "-l", "4000", name])
                (destination / filename).write_bytes(wire)
            oracle = run(user, stage + "-oracle", [args.oracle_binary,
                "--verify-original-packets", seed, destination])
            if b"ORIGINAL_PACKET_WIRE_ORACLE_PASS packets=3" not in oracle:
                raise RuntimeError("missing C++ full-wire oracle proof")
            after = {node: fresh_status(ndn.net[node], stage + "-" + node + "-after")
                     for node in ("user", "r1", "r2")}
            deltas = {}
            for node in before:
                require_zero_cs(before[node])
                require_zero_cs(after[node])
            for replica in ("r1", "r2"):
                old, new = (face(before[replica], ndn.net[replica], user),
                            face(after[replica], ndn.net[replica], user))
                if old.findtext("n:faceId", namespaces=NS) != new.findtext("n:faceId", namespaces=NS):
                    raise RuntimeError("network face lifetime changed within retrieval")
                deltas[replica] = counter(new, "outgoing") - counter(old, "outgoing")
            incoming = counter(face(after["user"], user, ndn.net[target]), "incoming") - counter(
                face(before["user"], user, ndn.net[target]), "incoming")
            if deltas[target] < 3 or deltas["r2" if target == "r1" else "r1"] != 0 or incoming < 3:
                raise RuntimeError("actual network egress does not prove intended replica")
            phases.append({"stage": stage, "target": target, "faceId": wanted,
                           "repoOutgoingData": deltas, "userIncomingData": incoming,
                           "cppWireOracle": "PASS", "csDisabled": True})
        if args.smoke_test and time.monotonic() - window_start >= 10:
            raise RuntimeError("smoke window exceeded 10 seconds")
        result["status"] = "SMOKE_PASS" if args.smoke_test else "PASS"
    except BaseException as error:
        result.update(status="FAIL", boundary=type(error).__name__ + ": " + str(error))
        raise
    finally:
        # Finish cleanup and the receipt even if a second interrupt arrives.
        for number in previous_signals:
            signal.signal(number, signal.SIG_IGN)
        errors = []
        for label, server in list(servers.items()):
            try:
                stop(server, label + "-final")
            except BaseException as error:
                errors.append(str(error))
        for process, record in owned:
            try:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    record["exit"] = process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    record["exit"] = process.wait(timeout=10)
                    errors.append("child cleanup deadline exceeded: " + record["label"])
            except BaseException as error:
                errors.append(str(error))
        try:
            if ndn is not None:
                ndn.stop()
        except BaseException as error:
            errors.append(str(error))
        finally:
            for stream in streams:
                stream.close()
            if errors:
                result.update(status="FAIL", cleanupBoundaries=errors)
            try:
                (output / "qualification.json").write_text(json.dumps(result, indent=2) + "\n")
            finally:
                for number, previous in previous_signals.items():
                    signal.signal(number, previous)
        if errors:
            raise RuntimeError("replica cleanup failed: " + "; ".join(errors))
    marker = "SPEC201_REPLICA_SMOKE_OK" if args.smoke_test else "SPEC201_TRANSPARENT_NETWORK_REPLICA_PASS"
    print(marker + " candidate=" + candidate["candidateId"])


if __name__ == "__main__":
    main()
