#!/usr/bin/env python3
"""MiniNDN acceptance for Spec 116 exact-name signed catalog Data."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPOLOGY = REPO / "Experiments/Topology/AI_Lab.conf"
ROLE = REPO / "examples/python/NDNSF-DistributedInference/spec116_catalog_transport.py"
IDENTITY = "/example/hello/user"
RECORD = (IDENTITY + "/NDNSF/DI/DEFINITION/spec116/"
          "sha256:catalog-smoke")
PAYLOAD = "spec116-signed-catalog-record"


def log(value: str) -> None:
    info(value + "\n")


def quote(value) -> str:
    return perf.shell_quote(str(value))


def command(mode: str, hold: float = 20.0) -> str:
    return " ".join((
        "cd", quote(REPO), "&&", "exec", "python3", quote(ROLE), mode,
        "--record-name", quote(RECORD), "--payload", quote(PAYLOAD),
        "--hold-seconds", quote(hold), "--timeout-ms", "8000"))


def wait_for(path: Path, needle: str, timeout: float, process=None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process is not None and process.poll() is not None:
            return False
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False


def start(node, name, cmd, env, root, processes):
    path = root / f"{name}.log"
    stream = path.open("wb")
    process = getPopen(
        node, cmd, envDict=env, shell=True,
        stdout=stream, stderr=subprocess.STDOUT)
    processes.append((process, stream, path))
    return process, path


def stop(processes) -> None:
    for process, stream, _ in reversed(processes):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=3)
            except Exception:
                process.kill()
        stream.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(
        REPO / "results/spec116-minindn-catalog-final"))
    parser.add_argument("--publisher-node", default="memphis")
    parser.add_argument("--requester-node", default="ucla")
    args = parser.parse_args()
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    root = Path(args.out).resolve()
    root.mkdir(parents=True, exist_ok=True)
    processes = []
    ndn = None
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    try:
        ndn = Minindn(topoFile=str(TOPOLOGY))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="ERROR")
        perf.wait_for_nfd_sockets(ndn, root)
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin([ndn.net[args.publisher_node]], [
            IDENTITY, "/example/hello/group"])
        routing.addOrigin([ndn.net[args.publisher_node]], [
            "/example/hello/controller"])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            for prefix in ("/example/hello", "/example/hello/group", IDENTITY):
                Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)

        perf_args = SimpleNamespace(
            controller_node=args.publisher_node, providers=1,
            provider_nodes=args.requester_node, performance_mode=False,
            debug_ack=False, timeline_trace=False, svs_piggyback_trace=False,
            dk_bootstrap_check=False, workload_mode="single", ack_threads=2,
            crypto_diagnostics=False)
        perf.initialize_example_keychains(ndn, perf_args, root)
        env = perf.app_env(root, int(time.time()) + os.getpid(), perf_args)

        controller, controller_log = start(
            ndn.net[args.publisher_node], "controller",
            " ".join(("cd", quote(REPO), "&&", "exec",
                      quote(REPO / "build/examples/App_ServiceController"))),
            env, root, processes)
        if not wait_for(controller_log, "ServiceController listening on:", 8, controller):
            raise RuntimeError("controller did not become ready")

        publisher, publisher_log = start(
            ndn.net[args.publisher_node], "publisher",
            command("publish"), env, root, processes)
        if not wait_for(
                publisher_log, "SPEC116_APP_DATA_PUBLISHED", 12, publisher):
            raise RuntimeError("publisher did not publish exact signed Data")

        requester, requester_log = start(
            ndn.net[args.requester_node], "requester",
            command("fetch", 0), env, root, processes)
        requester.wait(timeout=20)
        text = requester_log.read_text(errors="replace")
        success = requester.returncode == 0 and "SPEC116_APP_DATA_FETCHED" in text
        summary = {
            "schema": "spec116-minindn-catalog-v1",
            "status": "PASS" if success else "FAIL",
            "publisherNode": args.publisher_node,
            "requesterNode": args.requester_node,
            "recordName": RECORD,
            "publisherLog": str(publisher_log),
            "requesterLog": str(requester_log),
        }
        (root / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n")
        if not success:
            raise RuntimeError(f"catalog transport failed; logs={root}")
        print(f"SPEC116_MININDN_CATALOG_OK logs={root}")
        return 0
    finally:
        stop(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
