#!/usr/bin/env python3
"""Run the two-peer Go SVS comparison with explicit binary and topology inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import time


HERE = Path(__file__).resolve().parent
DEFAULT_BINARY = HERE / "syncps"
DEFAULT_BINARY_SHA256 = "049534b35fae3c08a2e34d6ec9c023e6572764ae1c161e3f0a47369378fb8f54"
DEFAULT_TOPOLOGY = HERE / "Topology/testbed(loss=0%).conf"
PEERS = ("memphis", "ucla")


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def input_identity(binary, expected_digest, topology):
    if platform.system() != "Linux":
        raise ValueError("MiniNDN requires Linux")
    if binary == DEFAULT_BINARY and platform.machine() != "x86_64":
        raise ValueError("the tracked syncps binary requires x86_64; build syncps.go for this machine")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"syncps executable is missing: {binary}")
    actual_digest = sha256(binary)
    if actual_digest != expected_digest:
        raise ValueError(f"syncps SHA-256 mismatch: {binary}")
    nodes = set()
    in_nodes = False
    for line in topology.read_text().splitlines():
        line = line.strip()
        if line.startswith("["):
            in_nodes = line == "[nodes]"
        elif in_nodes and ":" in line and not line.startswith("#"):
            nodes.add(line.split(":", 1)[0])
    missing = {"memphis", "csu", "ucla"} - nodes
    if missing:
        raise ValueError("topology lacks required nodes: " + ", ".join(sorted(missing)))
    return {"binary": str(binary), "binarySha256": actual_digest,
            "topology": str(topology), "topologySha256": sha256(topology)}


def configure_routes(ndn, nfdc):
    for node in ndn.net.hosts:
        nfdc.setStrategy(node, "/muas", nfdc.STRATEGY_MULTICAST)
        for peer in ndn.net.hosts:
            for peer_interface, _ in peer.connectionsTo(node):
                face = nfdc.createFace(node, peer_interface.IP())
                nfdc.registerRoute(node, "/muas", face)
        node.cmd("nlsrc advertise /muas/" + node.name)
    for route in (("memphis", "csu", "ucla"), ("ucla", "csu", "memphis")):
        destination = ndn.net[route[-1]].IP()
        for source, next_hop in zip(route, route[1:]):
            node = ndn.net[source]
            interface, _ = ndn.net[next_hop].connectionsTo(node)[0]
            node.cmd(f"ip route add {destination} via {interface.IP()}")


def run(args, identity):
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.apps.nlsr import Nlsr
    from minindn.helpers.nfdc import Nfdc
    from minindn.minindn import Minindn
    from minindn.util import MiniNDNCLI

    Minindn.verifyDependencies()
    args.output_dir.mkdir(parents=True)
    (args.output_dir / "inputs.json").write_text(json.dumps(identity, indent=2) + "\n")
    with tempfile.TemporaryDirectory(prefix="ndnsf-syncps-", dir="/tmp") as work:
        coordinator_home = Path(work) / "coordinator"
        coordinator_home.mkdir(mode=0o700)
        os.environ["HOME"] = str(coordinator_home)
        for key in ("NDN_CLIENT_CONF", "NDN_CLIENT_PIB", "NDN_CLIENT_TPM",
                    "NDN_CLIENT_TRANSPORT", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            os.environ.pop(key, None)
        original_argv = sys.argv
        try:
            sys.argv = [original_argv[0]]
            ndn = Minindn(topoFile=str(args.topology), workDir=work)
        finally:
            sys.argv = original_argv
        processes = []
        received_counts = {}
        try:
            ndn.start()
            AppManager(ndn, ndn.net.hosts, Nfd)
            AppManager(ndn, ndn.net.hosts, Nlsr)
            configure_routes(ndn, Nfdc)
            for name in PEERS:
                home = Path(ndn.net[name].params["params"]["homeDir"])
                conf = home / ".ndn/client.conf"
                transport = next(line.partition("=")[2].strip() for line in conf.read_text().splitlines()
                                 if line.startswith("transport="))
                env = {**os.environ, "HOME": str(home), "NDN_CLIENT_TRANSPORT": transport}
                with (args.output_dir / (name + ".log")).open("wb") as log:
                    processes.append(ndn.net[name].popen(
                        [str(args.syncps), "/muas/" + name], cwd=home, env=env,
                        stdout=log, stderr=subprocess.STDOUT))
            if args.interactive:
                MiniNDNCLI(ndn.net)
            else:
                deadline = time.monotonic() + args.seconds
                while time.monotonic() < deadline:
                    if any(process.poll() is not None for process in processes):
                        raise RuntimeError("syncps exited early; inspect the peer logs")
                    time.sleep(min(0.25, max(0, deadline - time.monotonic())))
            for name in PEERS:
                other = next(peer for peer in PEERS if peer != name)
                received = (args.output_dir / (name + ".log")).read_text(errors="replace")
                received_counts[name] = len(re.findall(
                    r"^\[recv .*\] /(?:8=)?muas/(?:8=)?" + other + r":",
                    received, re.M))
                if not received_counts[name]:
                    raise RuntimeError(f"{name} did not receive a publication from {other}")
        finally:
            try:
                for process in processes:
                    if process.poll() is None:
                        process.terminate()
                for process in processes:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            finally:
                ndn.stop()
        return received_counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--syncps", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--syncps-sha256", default=DEFAULT_BINARY_SHA256)
    parser.add_argument("--topology", type=Path, default=DEFAULT_TOPOLOGY)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--seconds", type=float, default=35)
    parser.add_argument("--interactive", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    args = parser.parse_args()
    args.syncps = args.syncps.resolve()
    args.topology = args.topology.resolve()
    try:
        identity = input_identity(args.syncps, args.syncps_sha256, args.topology)
        if args.preflight_only:
            print(json.dumps(identity, sort_keys=True))
            return 0
        if os.geteuid() != 0 or args.output_dir is None or not (0 < args.seconds <= 3600):
            parser.error("run requires root, --output-dir, and --seconds in (0, 3600]")
        args.output_dir = args.output_dir.resolve()
        print(json.dumps({"status": "PASS", "received": run(args, identity)}, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"SYNCPS_EXPERIMENT_ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
