#!/usr/bin/env python3
"""MiniNDN smoke test for C++ NDNSF token certificate bootstrap."""

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
    parser = argparse.ArgumentParser(description="MiniNDN C++ token bootstrap smoke")
    parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--provider-node", default="ucla")
    parser.add_argument("--user-node", default="memphis")
    parser.add_argument("--output-dir", default=str(REPO / "results/token_bootstrap_minindn"))
    parser.add_argument("--controller-wait-s", type=float, default=3.0)
    parser.add_argument("--provider-wait-s", type=float, default=5.0)
    parser.add_argument("--user-timeout-s", type=float, default=35.0)
    parser.add_argument("--nfd-log-level", default="ERROR")
    return parser


def make_perf_args(args: argparse.Namespace) -> SimpleNamespace:
    return SimpleNamespace(
        controller_node=args.controller_node,
        providers=1,
        provider_nodes=args.provider_node,
        performance_mode=False,
        debug_ack=False,
        timeline_trace=False,
        svs_piggyback_trace=False,
        dk_bootstrap_check=False,
        workload_mode="single",
        ack_threads=2,
        crypto_diagnostics=False,
    )


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


def app_cmd(binary: str, *argv: str) -> str:
    parts = [
        "cd", shell_quote(REPO), "&&",
        "exec", shell_quote(REPO / "build/examples" / binary),
    ]
    parts.extend(shell_quote(arg) for arg in argv)
    return " ".join(parts)


def main() -> int:
    args = build_parser().parse_args()
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

        controller_proc, controller_log = start(
            ndn.net[args.controller_node],
            "controller",
            app_cmd("App_ServiceController",
                    "--bootstrap-token-file", "examples/hello.bootstrap-tokens"),
            env,
            output_dir,
            processes)
        if not wait_log(controller_log, "ServiceController listening on:", args.controller_wait_s,
                        controller_proc):
            raise RuntimeError(f"controller did not become ready; log={controller_log}")

        provider_proc, provider_log = start(
            ndn.net[args.provider_node],
            "provider",
            app_cmd("App_Provider",
                    "--bootstrap-token", "provider-token-045",
                    "--bootstrap-name", "/example/hello/provider"),
            env,
            output_dir,
            processes)
        if not wait_log(provider_log, "Provider default registered service /HELLO",
                        args.provider_wait_s, provider_proc):
            raise RuntimeError(f"provider did not become ready; log={provider_log}")

        user_proc, user_log = start(
            ndn.net[args.user_node],
            "user",
            app_cmd("App_User",
                    "--bootstrap-token", "user-token-045",
                    "--bootstrap-name", "/example/hello/user"),
            env,
            output_dir,
            processes)
        user_proc.wait(timeout=args.user_timeout_s)

        reuse_proc, reuse_log = start(
            ndn.net[args.user_node],
            "user-reuse",
            app_cmd("App_User",
                    "--bootstrap-token", "user-token-045",
                    "--bootstrap-name", "/example/hello/user"),
            env,
            output_dir,
            processes)
        reuse_proc.wait(timeout=args.user_timeout_s)

        user_text = user_log.read_text(errors="replace")
        reuse_text = reuse_log.read_text(errors="replace")
        provider_text = provider_log.read_text(errors="replace")
        controller_text = controller_log.read_text(errors="replace")
        print(user_text, end="" if user_text.endswith("\n") else "\n")

        user_issued_count = controller_text.count(
            "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user")
        required = [
            ("controller issued provider", controller_text, "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/provider"),
            ("controller issued user", controller_text, "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user"),
            ("provider installed", provider_text, "NDNSF_CERT_BOOTSTRAP_INSTALLED identity=/example/hello/provider"),
            ("user installed", user_text, "NDNSF_CERT_BOOTSTRAP_INSTALLED identity=/example/hello/user"),
            ("user reused", reuse_text, "NDNSF_CERT_BOOTSTRAP_REUSED identity=/example/hello/user"),
            ("provider permission", provider_text, "Installed provider permission provider=/example/hello/provider service=/HELLO"),
            ("user permission", user_text, "Installed user permission provider=/example/hello/provider service=/HELLO"),
            ("reuse user permission", reuse_text, "Installed user permission provider=/example/hello/provider service=/HELLO"),
            ("hello response", user_text, "Received response: HELLO"),
            ("reuse hello response", reuse_text, "Received response: HELLO"),
        ]
        missing = [label for label, text, needle in required if needle not in text]
        if user_proc.returncode != 0 or reuse_proc.returncode != 0 or missing or user_issued_count != 1:
            raise RuntimeError(f"token bootstrap MiniNDN smoke failed "
                               f"user_rc={user_proc.returncode} reuse_rc={reuse_proc.returncode} "
                               f"user_issued_count={user_issued_count} missing={missing} "
                               f"logs={output_dir}")
        print(f"TOKEN_BOOTSTRAP_MININDN_OK logs={output_dir}")
        return 0
    finally:
        stop(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
