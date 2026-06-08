#!/usr/bin/env python3
"""MiniNDN smoke test for the Python NDNSF service API.

The test runs the Python HELLO example across two MiniNDN nodes:

  memphis: ServiceController and ServiceUser
  ucla:    ServiceProvider

It verifies that Python application logic can use the reusable ndnsf
ServiceUser/ServiceProvider API while the C++ runtime hosts perform the NDNSF
protocol work.
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
    parser = argparse.ArgumentParser(description="MiniNDN smoke test for Python NDNSF HELLO")
    parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--user-node", default="memphis")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--provider-node", default="ucla")
    parser.add_argument("--output-dir", default=str(REPO / "results/python_hello_minindn_smoke"))
    parser.add_argument("--startup-wait-s", type=float, default=5.0)
    parser.add_argument("--controller-wait-s", type=float, default=3.0)
    parser.add_argument("--ack-timeout-ms", type=int, default=3000)
    parser.add_argument("--timeout-ms", type=int, default=8000)
    parser.add_argument("--nfd-log-level", default="ERROR")
    return parser


def make_perf_args(args: argparse.Namespace) -> SimpleNamespace:
    return SimpleNamespace(
        controller_node=args.controller_node,
        user_node=args.user_node,
        providers=1,
        provider_nodes=args.provider_node,
        serve_provider_certs=False,
        debug_ack=False,
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
        performance_mode=False,
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


def python_cmd(script: str, *argv: str) -> str:
    parts = [
        "cd", shell_quote(REPO), "&&",
        "PYTHONPATH=" + shell_quote(REPO / "pythonWrapper"),
        "NDNSF_BINARY_DIR=" + shell_quote(REPO / "build/examples"),
        "NDNSF_LIBRARY_DIR=" + shell_quote(REPO / "build"),
        "exec", shell_quote(sys.executable),
        shell_quote(REPO / "examples/python" / script),
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


def main() -> int:
    args = build_parser().parse_args()
    # MiniNDN inspects sys.argv internally unless a parser is supplied. Keep
    # this script's options from being re-parsed as MiniNDN options.
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    for lock_path in (Path("/tmp/ndnsf-python-provider-keychain.lock"),
                      Path("/tmp/ndnsf-python-user-keychain.lock")):
        try:
            lock_path.unlink()
        except FileNotFoundError:
            pass

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
        env["PYTHONPATH"] = str(REPO / "pythonWrapper")
        env["NDNSF_BINARY_DIR"] = str(REPO / "build/examples")
        env["NDNSF_LIBRARY_DIR"] = str(REPO / "build")

        controller_cmd = python_cmd(
            "hello_controller.py",
            "--policy-file", "examples/hello.policies",
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        )
        start(ndn.net[args.controller_node], "python-controller", controller_cmd, env, output_dir, processes)
        time.sleep(args.controller_wait_s)

        provider_cmd = python_cmd(
            "hello_provider.py",
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        )
        provider_proc, provider_log = start(
            ndn.net[args.provider_node], "python-provider", provider_cmd, env, output_dir, processes)
        time.sleep(args.startup_wait_s)
        if provider_proc.poll() is not None:
            raise RuntimeError(f"provider exited early; see {provider_log}")

        list_cmd = python_cmd(
            "hello_user.py",
            "--list-services",
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        )
        list_proc, list_log = start(ndn.net[args.user_node], "python-user-list-services",
                                    list_cmd, env, output_dir, processes)
        list_proc.wait(timeout=20)
        list_text = list_log.read_text(errors="replace")
        print(list_text, end="" if list_text.endswith("\n") else "\n")
        if list_proc.returncode != 0 or "/HELLO" not in list_text:
            raise RuntimeError("Python permission-list smoke failed "
                               f"rc={list_proc.returncode}; log={list_log}")

        user_cmd = python_cmd(
            "hello_user.py",
            "--ack-timeout-ms", str(args.ack_timeout_ms),
            "--timeout-ms", str(args.timeout_ms),
            "--binary-dir", "build/examples",
            "--library-dir", "build",
        )
        user_proc, user_log = start(ndn.net[args.user_node], "python-user", user_cmd, env,
                                    output_dir, processes)
        user_proc.wait(timeout=(args.timeout_ms / 1000.0) + 20)
        text = user_log.read_text(errors="replace")
        print(text, end="" if text.endswith("\n") else "\n")
        if user_proc.returncode != 0 or "HELLO" not in text:
            raise RuntimeError(f"Python HELLO MiniNDN smoke failed rc={user_proc.returncode}; "
                               f"log={user_log}")
        print(f"PYTHON_HELLO_MININDN_OK log={user_log}")
        return 0
    finally:
        stop(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
