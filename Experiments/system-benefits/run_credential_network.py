#!/usr/bin/env python3
"""Three native permission-update events over real MiniNDN, with separate probe barriers."""
import argparse
import copy
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "targeted-invocation"))
from run_minindn import digest, save, stop, wait_marker
from credential_network_inputs import verify_qualification

ARMS = ("AGGREGATED_ABE", "PERMISSION_ABE", "BATCHED_RSA")
ACTOR = "/usr/local/bin/ndnsf-credential-network"
ORACLE = "/usr/local/bin/ndnsf-credential-archive-oracle"


def live(path):
    if not path.exists():
        return []
    text = path.read_text()
    lines = text.splitlines()
    if text and not text.endswith("\n"):
        lines = lines[:-1]
    return [json.loads(line) for line in lines]


def candidate():
    paths = [HERE / name for name in (
        "credential-network.cpp", "credential-receiver.hpp", "credential-archive.hpp",
        "credential-archive-oracle.cpp", "credential-cost.cpp", "wscript",
        "run_credential_network.py", "analyze_credential_network.py")]
    paths.append(HERE / "credential_network_inputs.py")
    paths += [HERE.parent / "targeted-invocation" / name for name in ("common.hpp", "run_minindn.py")]
    paths += [HERE / "analyze_dynamic.py"]
    result = {"sources": {str(p): digest(p) for p in paths},
              "binaries": {p: digest(p) for p in (ACTOR, ORACLE, "/usr/local/bin/ndnsf-credential-cost", "/usr/local/bin/nfd")},
              "libraries": {}}
    for binary in result["binaries"]:
        closure = subprocess.check_output(["ldd", binary], text=True,
                                         env=dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib"))
        if "not found" in closure or str(REPO) in closure:
            raise RuntimeError("invalid installed library closure")
        for word in closure.split():
            if word.startswith("/") and Path(word).is_file():
                result["libraries"][str(Path(word).resolve())] = digest(word)
    return result


def write_epoch(path, epoch):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(str(epoch) + "\n")
    temporary.replace(path)


def run(args, parser):
    from mininet.topo import Topo
    from minindn.minindn import Minindn
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.util import getPopen
    from ndn.encoding import Name, parse_data

    root = Path(args.workspace).resolve()
    if str(root).startswith("/dev/shm"):
        raise ValueError("disk workspace required")
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".active.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        output = Path(args.output_dir).resolve()
        output.mkdir(parents=True, exist_ok=False)
        archive = Path(args.archive).resolve()
        case = f"{args.arm}-p{args.permissions}-r{args.repetition}"
        completion = json.loads((archive / "complete.json").read_text())
        if completion.get("repetitions") != args.archive_repetitions or case not in completion.get("cases", []):
            raise ValueError("archive/caller matrix mismatch")
        qualification = None
        if args.qualification:
            qualification = Path(args.qualification).resolve()
            verify_qualification(archive, args.archive_repetitions, qualification)
            shutil.copy2(qualification, output / "qualification.json")
            shutil.copy2(qualification.parent / "archive-oracle.jsonl", output / "archive-oracle.jsonl")
        else:
            with (output / "archive-oracle.jsonl").open("wb") as out, (output / "archive-oracle.log").open("wb") as err:
                subprocess.run([ORACLE, str(archive), str(args.archive_repetitions)], stdout=out, stderr=err, check=True)
        private_manifest = json.loads((archive / case / "manifest.json").read_text())
        if (private_manifest["arm"], private_manifest["permissions"], private_manifest["repetition"]) != (args.arm, args.permissions, args.repetition):
            raise ValueError("case identity mismatch")
        public = output / "case.json"
        save(public, private_manifest)
        cert_bytes = (archive / "authority.cert").read_bytes()
        certificate_name = Name.to_str(parse_data(cert_bytes)[0])
        pin = digest(archive / "authority.cert")
        stage, probes = root / "epoch", root / "probe-epoch"
        write_epoch(stage, 0)
        write_epoch(probes, 0)

        topology = Topo()
        for node in ("gs", "relay", "drone1", "drone2"):
            topology.addHost(node)
        for peer in ("gs", "drone1", "drone2"):
            topology.addLink("relay", peer, delay="10ms", loss=0, bw=1000)
        ndn = Minindn(parser=parser, topo=topology, workDir=str(root / "nodes"))
        manifest = {"schema": "ndnsf-credential-network-cell-v1", "status": "PARTIAL",
                    "arm": args.arm, "permissions": args.permissions, "repetition": args.repetition,
                    "cache": args.cache, "negative": args.negative, "smoke": args.smoke,
                    "archive_repetitions": args.archive_repetitions,
                    "certificate_name": certificate_name, "certificate_sha256": pin,
                    "certificate_wire_bytes": len(cert_bytes),
                    "qualification": str(qualification) if qualification else None,
                    "qualification_sha256": digest(qualification) if qualification else None,
                    "candidate": candidate(), "archive": str(archive), "case": case,
                    "archive_hashes": {str(p.relative_to(archive)): digest(p)
                        for p in [archive / "complete.json", archive / "authority.cert", archive / case / "manifest.json"]
                        + list((archive / case).glob("*.wire"))
                        + [archive / f"recipient-{i}.{ext}" for i in range(8) for ext in ("cert", "safebag")]},
                    "topology": {"links": 3, "delay_ms": 10, "loss": 0, "bandwidth_mbps": 1000},
                    "anchor": {"wall_ns": time.time_ns(), "mono_ns": time.monotonic_ns()},
                    "epochs": [], "children": [], "capture": {}, "bootstrap": {}}
        children = []
        nfd_manager = None

        def launch(node, label, command, home=None):
            handle = (output / (label + ".log")).open("wb")
            env = {"LD_LIBRARY_PATH": "/usr/local/lib", "NDN_LOG": "*=NONE"}
            if home:
                env["HOME"] = str(home)
            process = getPopen(node, command, envDict=env, stdout=handle, stderr=subprocess.STDOUT)
            children.append((label, process, handle))
            return process

        def home_for(label, node):
            home = root / "actor-homes" / label
            (home / ".ndn").mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(node.params["params"]["homeDir"]) / ".ndn/client.conf", home / ".ndn/client.conf")
            return home

        def barrier(event, epoch=None, users=range(8), seconds=12):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                result = {}
                for label, process, _ in children:
                    if process.poll() is not None:
                        raise RuntimeError("child exited at barrier: " + label)
                for i in users:
                    matches = [r for r in live(output / f"user{i}.jsonl") if r["event"] == event
                               and (epoch is None or r.get("epoch") == epoch)]
                    if len(matches) > 1:
                        raise RuntimeError("duplicate barrier event")
                    if matches:
                        result[str(i)] = matches[0]
                if len(result) == len(users):
                    return result
                time.sleep(.01)
            raise RuntimeError(f"barrier timeout: {event}/{epoch}")

        try:
            ndn.start()
            nfd_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
            routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
            routing.addOrigin([ndn.net["gs"]], ["/system-benefits/controller"])
            routing.calculateRoutes()
            for node in ndn.net.hosts:
                Nfdc.setStrategy(node, "/system-benefits/controller", Nfdc.STRATEGY_BEST_ROUTE)
                listing = node.cmd("nfdc strategy list")
                match = [line for line in listing.splitlines() if "prefix=/system-benefits/controller " in line]
                if len(match) != 1 or "/strategy/best-route/" not in match[0]:
                    raise RuntimeError("best-route configuration mismatch")
                (output / (node.name + "-routing.txt")).write_text(listing + node.cmd("nfdc fib list") + node.cmd("tc qdisc show"))
            time.sleep(1 if args.smoke else 10)
            server = launch(ndn.net["gs"], "server", [ACTOR, "--role", "server", "--archive", str(archive),
                "--case", case, "--records", str(output / "server.jsonl")], home_for("server", ndn.net["gs"]))
            until = time.monotonic() + 10
            while not any(r["event"] == "ready" for r in live(output / "server.jsonl")):
                if server.poll() is not None or time.monotonic() >= until:
                    raise RuntimeError("server readiness failed")
                time.sleep(.02)
            for i in range(8):
                node = ndn.net["drone1" if i < 4 else "drone2"]
                home = home_for(f"u{i}", node)
                for ext in ("cert", "safebag"):
                    target = home / ("own." + ext)
                    shutil.copy2(archive / f"recipient-{i}.{ext}", target)
                    target.chmod(0o600)
                manifest["bootstrap"][str(i)] = {"node": node.name, "home": str(home),
                    "own_cert_sha256": digest(home / "own.cert"), "own_bag_sha256": digest(home / "own.safebag")}
                reference = public
                if i == 0 and args.negative == "wrong-recipient":
                    changed = copy.deepcopy(private_manifest)
                    donor = [o for o in changed["objects"] if o["epoch"] == 1 and o["kind"] == "management"
                             and o["recipient"] == "/system-benefits/user/1"]
                    targets = [o for o in changed["objects"] if o["epoch"] == 1 and o["kind"] == "management"
                               and o["recipient"] == "/system-benefits/user/0"]
                    if len(donor) != len(targets) or not donor:
                        raise RuntimeError("negative recipient fixture cardinality")
                    for target, source in zip(targets, donor):
                        target["packets"] = source["packets"]
                    reference = output / "wrong-recipient.json"
                    save(reference, changed)
                fingerprint = "0" * 64 if i == 0 and args.negative == "certificate-pin" else pin
                launch(node, f"user{i}", [ACTOR, "--role", "client", "--manifest", str(reference),
                    "--stage", str(stage), "--probe-stage", str(probes), "--bag", str(home / "own.safebag"),
                    "--own-cert", str(home / "own.cert"), "--certificate-name", certificate_name,
                    "--certificate-sha256", fingerprint, "--user", str(i), "--cache", args.cache,
                    "--records", str(output / f"user{i}.jsonl")], home)
            barrier("ready")
            for peer in ("gs", "drone1", "drone2"):
                interface = ndn.net["relay"].connectionsTo(ndn.net[peer])[0][0].name
                capture = launch(ndn.net["relay"], "capture-" + peer, ["tcpdump", "-n", "-U", "-s", "0", "-i", interface,
                    "-w", str(output / (peer + ".pcap")), "ip"])
                wait_marker(capture, output / ("capture-" + peer + ".log"), "listening on", 5)
                manifest["capture"][peer] = {"interface": interface}
            time.sleep(.25)
            manifest["observation_begin_ns"] = time.monotonic_ns()
            for epoch in (1, 2, 3):
                phase = {"epoch": epoch, "management_begin_ns": time.monotonic_ns()}
                manifest["epochs"].append(phase)
                write_epoch(stage, epoch)
                if args.negative != "none":
                    until = time.monotonic() + 5
                    while not any(r["event"] == "failure" for r in live(output / "user0.jsonl")):
                        if time.monotonic() >= until:
                            raise RuntimeError("negative did not reject")
                        time.sleep(.01)
                    phase["negative_failure"] = live(output / "user0.jsonl")[-1]
                    manifest["status"] = "NEGATIVE_COMPLETE"
                    break
                phase["installed"] = barrier("management_installed", epoch)
                time.sleep(.1)
                phase["management_end_ns"] = time.monotonic_ns()
                phase["probe_begin_ns"] = time.monotonic_ns()
                write_epoch(probes, epoch)
                phase["complete"] = barrier("phase_complete", epoch)
                time.sleep(.1)
                phase["probe_end_ns"] = time.monotonic_ns()
                save(output / "manifest.json", manifest)
            if args.negative == "none":
                manifest["status"] = "RUN_COMPLETE"
            remaining = 0 if args.negative != "none" else (1 if args.smoke else 60) - (time.monotonic_ns() - manifest["observation_begin_ns"]) / 1e9
            if remaining > 0:
                time.sleep(remaining)
            manifest["observation_end_ns"] = time.monotonic_ns()
            time.sleep(.25)
        except BaseException as error:
            manifest["status"] = "PARTIAL"
            manifest["first_failure"] = str(error)
        finally:
            # Cancel consumers while their server and NFD remain available.
            ordered = sorted(children, key=lambda child: 0 if child[0].startswith("user") else 1 if child[0] == "server" else 2)
            drained = False
            for label, process, handle in ordered:
                if label.startswith("capture-") and not drained:
                    manifest["capture_drain_begin_ns"] = time.monotonic_ns()
                    time.sleep(2)
                    manifest["capture_drain_end_ns"] = time.monotonic_ns()
                    drained = True
                try:
                    code = stop(process)
                    manifest["children"].append({"label": label, "exit": code})
                    expected = 2 if label.startswith("user") and args.negative != "none" else 0
                    if code != expected:
                        raise RuntimeError(label + " unexpected exit " + str(code))
                except Exception as error:
                    manifest["status"] = "PARTIAL"
                    manifest.setdefault("cleanup_errors", []).append(str(error))
                handle.close()
            for peer in ("gs", "drone1", "drone2"):
                log = output / ("capture-" + peer + ".log")
                matches = re.findall(r"(\d+) packets dropped by kernel", log.read_text() if log.exists() else "")
                captured = re.findall(r"(\d+) packets captured", log.read_text() if log.exists() else "")
                received = re.findall(r"(\d+) packets received by filter", log.read_text() if log.exists() else "")
                if matches != ["0"] or len(captured) != 1 or captured != received:
                    manifest["status"] = "PARTIAL"
                    manifest.setdefault("cleanup_errors", []).append("capture unclosed/lost: " + peer)
                else:
                    manifest["capture"][peer]["dropped_packets"] = 0
                    manifest["capture"][peer]["captured_packets"] = int(captured[0])
            if nfd_manager:
                for app in nfd_manager:
                    try:
                        code = stop(app.process)
                        manifest["children"].append({"label": app.node.name + "-nfd", "exit": code})
                        if code != 0:
                            raise RuntimeError("NFD nonzero exit")
                    except Exception as error:
                        manifest["status"] = "PARTIAL"
                        manifest.setdefault("cleanup_errors", []).append(str(error))
            try:
                ndn.stop()
            except Exception as error:
                manifest["status"] = "PARTIAL"
                manifest.setdefault("cleanup_errors", []).append(str(error))
            manifest["inputs_unchanged"] = candidate() == manifest["candidate"] and all(
                digest(archive / name) == expected for name, expected in manifest["archive_hashes"].items())
            if qualification:
                try:
                    verify_qualification(archive, args.archive_repetitions, qualification)
                    if digest(qualification) != manifest["qualification_sha256"]:
                        raise ValueError("qualification receipt changed")
                except Exception as error:
                    manifest["inputs_unchanged"] = False
                    manifest.setdefault("cleanup_errors", []).append(str(error))
            if not manifest["inputs_unchanged"]:
                manifest["status"] = "PARTIAL"
            save(output / "manifest.json", manifest)
        if manifest["status"] not in ("RUN_COMPLETE", "NEGATIVE_COMPLETE"):
            raise RuntimeError("credential network cell incomplete: " + str(output))
        print("CREDENTIAL_NETWORK_CELL_COMPLETE", output, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", default="/var/tmp/ndnsf-spec204-credential-network")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--archive-repetitions", type=int, choices=range(1, 6), required=True)
    parser.add_argument("--qualification", help="Immutable common native archive qualification receipt")
    parser.add_argument("--arm", choices=ARMS, required=True)
    parser.add_argument("--permissions", type=int, choices=(1, 4, 16), required=True)
    parser.add_argument("--repetition", type=int, choices=range(1, 6), required=True)
    parser.add_argument("--cache", choices=("reuse", "cold"), required=True)
    parser.add_argument("--negative", choices=("none", "certificate-pin", "wrong-recipient"), default="none")
    parser.add_argument("--smoke", action="store_true")
    args, _ = parser.parse_known_args()
    if os.geteuid() != 0:
        raise SystemExit("MiniNDN requires root")
    def interrupted(signum, frame):
        raise KeyboardInterrupt("supervisor signal " + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    run(args, parser)
