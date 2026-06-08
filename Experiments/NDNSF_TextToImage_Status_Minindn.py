#!/usr/bin/env python3
"""MiniNDN smoke for tracked NDNSF selection execution status.

The experiment runs a simulated long text-to-image service. The provider sleeps
for several seconds in a worker thread, while the user uses RequestServiceTracked
with a shorter timeout. On timeout the user should report the selected
provider's Selection execution state instead of blindly retrying.
"""

from __future__ import annotations

import argparse
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


DEFAULT_TOPOLOGY = REPO / "Experiments/Topology/AI_Lab.conf"


def log(message: str) -> None:
    info(message + "\n")


def shell_quote(value: object) -> str:
    return perf.shell_quote(str(value))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="MiniNDN smoke test for NDNSF tracked Selection status")
    parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--user-node", default="memphis")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--provider-node", default="ucla")
    parser.add_argument("--output-dir",
                        default=str(REPO / "results/text_to_image_status_minindn_smoke"))
    parser.add_argument("--controller-wait-s", type=float, default=3.0)
    parser.add_argument("--provider-wait-s", type=float, default=5.0)
    parser.add_argument("--provider-delay-ms", type=int, default=5000)
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument("--status-interval-ms", type=int, default=500)
    parser.add_argument("--status-timeout-ms", type=int, default=300)
    parser.add_argument("--nfd-log-level", default="ERROR")
    parser.add_argument("--quick-smoke", action="store_true",
                        help="Validate topology, node names, and required binaries without starting MiniNDN.")
    return parser


def make_perf_args(args: argparse.Namespace) -> SimpleNamespace:
    return SimpleNamespace(
        controller_node=args.controller_node,
        user_node=args.user_node,
        providers=1,
        provider_nodes=args.provider_node,
        serve_provider_certs=False,
        debug_ack=False,
        performance_mode=False,
        timeline_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_parallel_sync_processing=True,
        svs_parallel_workers=4,
        svs_parallel_queue=256,
        svs_sync_publish=False,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=4,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=True,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=2,
    )


def start(node, name: str, command: str, env: dict[str, str], output_dir: Path, processes):
    log_path = output_dir / f"{name}.log"
    log_file = log_path.open("wb")
    log(f"start {name} on {node.name}: {command}")
    proc = getPopen(node, command, envDict=env, shell=True,
                    stdout=log_file, stderr=subprocess.STDOUT)
    processes.append((proc, log_file, log_path))
    return proc, log_path


def stop(processes) -> None:
    for proc, log_file, _ in reversed(processes):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        log_file.close()


def wait_log(path: Path, needle: str, timeout_s: float, proc=None) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False


def cpp_cmd(binary: str, *argv: str) -> str:
    parts = [
        "cd", shell_quote(REPO), "&&",
        "LD_LIBRARY_PATH=" + shell_quote(REPO / "build") + ":${LD_LIBRARY_PATH:-}",
        "exec", shell_quote(REPO / "build/examples" / binary),
    ]
    parts.extend(shell_quote(arg) for arg in argv)
    return " ".join(parts)


def configure_routes(ndn, args: argparse.Namespace) -> None:
    routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing.addOrigin([ndn.net[args.controller_node]], ["/example/hello/controller"])
    routing.addOrigin([ndn.net[args.user_node]], ["/example/hello/user", "/example/hello/group"])
    routing.addOrigin([ndn.net[args.provider_node]],
                      ["/example/hello/provider",
                       "/example/hello/provider/KEY",
                       "/example/hello/group"])
    routing.calculateRoutes()
    for node in ndn.net.hosts:
        for prefix in ("/example/hello", "/example/hello/group",
                       "/example/hello/group/sync",
                       "/example/hello/group/s",
                       "/example/hello/group/d"):
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)


def topology_nodes(path: Path) -> set[str]:
    nodes = set()
    in_nodes = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            in_nodes = True
            continue
        if line.startswith("[") and line != "[nodes]":
            in_nodes = False
        if in_nodes and line and not line.startswith("#"):
            nodes.add(line.rstrip(":"))
    return nodes


def quick_smoke(args: argparse.Namespace) -> int:
    topology = Path(args.topology_file)
    required_files = [
        topology,
        REPO / "build/examples/App_ServiceController",
        REPO / "build/examples/App_TextToImageProvider",
        REPO / "build/examples/App_TextToImageUser",
        REPO / "examples/text-to-image-status.policies",
    ]
    missing = [str(path) for path in required_files if not path.exists()]
    nodes = topology_nodes(topology) if topology.exists() else set()
    required_nodes = {args.controller_node, args.user_node, args.provider_node}
    missing_nodes = sorted(required_nodes - nodes)
    if missing or missing_nodes:
        raise RuntimeError(
            "TextToImage status quick smoke failed "
            f"missingFiles={missing} missingNodes={missing_nodes}")
    print(
        "TEXT_TO_IMAGE_STATUS_MININDN_QUICK_SMOKE_OK "
        f"topology={topology} controller={args.controller_node} "
        f"user={args.user_node} provider={args.provider_node}")
    return 0


def main() -> int:
    args = build_parser().parse_args()
    if args.quick_smoke:
        return quick_smoke(args)
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    perf_args = make_perf_args(args)
    processes = []
    ndn = None

    Minindn.cleanUp()
    Minindn.verifyDependencies()
    try:
        ndn = Minindn(topoFile=args.topology_file)
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel=args.nfd_log_level)
        perf.wait_for_nfd_sockets(ndn, output_dir)
        configure_routes(ndn, args)
        perf.initialize_example_keychains(ndn, perf_args, output_dir)

        session = int(time.time()) + os.getpid()
        env = perf.app_env(output_dir, session, perf_args)

        controller_cmd = cpp_cmd(
            "App_ServiceController",
            "--policy-file", "examples/text-to-image-status.policies")
        start(ndn.net[args.controller_node], "controller", controller_cmd, env, output_dir, processes)
        time.sleep(args.controller_wait_s)

        provider_cmd = cpp_cmd(
            "App_TextToImageProvider",
            "--delay-ms", str(args.provider_delay_ms),
            "--handler-threads", "1")
        provider_proc, provider_log = start(
            ndn.net[args.provider_node], "provider", provider_cmd, env, output_dir, processes)
        if not wait_log(provider_log, "TEXT_TO_IMAGE_PROVIDER_READY",
                        args.provider_wait_s, provider_proc):
            raise RuntimeError(f"text-to-image provider did not become ready; see {provider_log}")

        user_cmd = cpp_cmd(
            "App_TextToImageUser",
            "--timeout-ms", str(args.timeout_ms),
            "--status-interval-ms", str(args.status_interval_ms),
            "--status-timeout-ms", str(args.status_timeout_ms))
        user_proc, user_log = start(ndn.net[args.user_node], "user", user_cmd, env,
                                    output_dir, processes)
        user_proc.wait(timeout=(args.timeout_ms / 1000.0) + 20)
        text = user_log.read_text(errors="replace")
        print(text, end="" if text.endswith("\n") else "\n")
        if (user_proc.returncode != 0 or
                "TEXT_TO_IMAGE_TRACKED_TIMEOUT" not in text or
                "TEXT_TO_IMAGE_SELECTION_STATUS" not in text or
                "state=Running" not in text):
            raise RuntimeError(f"text-to-image tracked status smoke failed "
                               f"rc={user_proc.returncode}; log={user_log}")
        print(f"TEXT_TO_IMAGE_STATUS_MININDN_OK log={user_log}")
        return 0
    finally:
        stop(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
