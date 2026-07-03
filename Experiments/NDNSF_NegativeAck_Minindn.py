#!/usr/bin/env python3
"""MiniNDN evidence for negative ACK known-provider early stop."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
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
NEGATIVE_ACK_RE = re.compile(r"event=NEGATIVE_ACK_RECORDED\b.*?\breason=([^\s,]+)")
EARLY_STOP_RE = re.compile(r"event=NEGATIVE_ACK_EARLY_STOP_ALL_KNOWN_PROVIDERS\b")


def log(message: str) -> None:
    info(message + "\n")


def shell_quote(value: object) -> str:
    return perf.shell_quote(str(value))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--user-node", default="memphis")
    parser.add_argument("--provider-nodes", default="ucla,wustl")
    parser.add_argument("--output-dir", default=str(
        REPO / "results/negative_ack_early_stop_minindn"))
    parser.add_argument("--ack-timeout-ms", type=int, default=9000)
    parser.add_argument("--controller-wait-s", type=float, default=5.0)
    parser.add_argument("--provider-wait-s", type=float, default=8.0)
    parser.add_argument("--user-timeout-s", type=float, default=35.0)
    parser.add_argument("--nfd-log-level", default="ERROR")
    return parser


def make_perf_args(args: argparse.Namespace) -> SimpleNamespace:
    provider_nodes = perf.parse_csv_list(args.provider_nodes)
    return SimpleNamespace(
        controller_node=args.controller_node,
        user_node=args.user_node,
        providers=len(provider_nodes),
        provider_nodes=args.provider_nodes,
        serve_provider_certs=False,
        debug_ack=True,
        timeline_trace=False,
        svs_piggyback_trace=False,
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
        workload_mode="single",
    )


def app_cmd(binary: str, *argv: str) -> str:
    parts = [
        "cd", shell_quote(REPO), "&&",
        "exec", shell_quote(REPO / "build/examples" / binary),
    ]
    parts.extend(shell_quote(arg) for arg in argv)
    return " ".join(parts)


def configure_routes(ndn, args: argparse.Namespace) -> None:
    provider_nodes = perf.parse_csv_list(args.provider_nodes)
    provider_ids = perf.provider_ids(len(provider_nodes))
    routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing.addOrigin([ndn.net[args.controller_node]], ["/example/hello/controller"])
    routing.addOrigin([ndn.net[args.user_node]], ["/example/hello/user", "/example/hello/group"])
    for node_name, provider_id in zip(provider_nodes, provider_ids):
        routing.addOrigin(
            [ndn.net[node_name]],
            [
                "/example/hello/provider",
                f"/example/hello/provider/{provider_id}",
                f"/example/hello/provider/{provider_id}/KEY",
                "/example/hello/group",
            ],
        )
    routing.calculateRoutes()
    for node in ndn.net.hosts:
        for prefix in (
            "/example/hello",
            "/example/hello/group",
            "/example/hello/group/sync",
            "/example/hello/group/s",
            "/example/hello/group/d",
        ):
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)


def start(node, name: str, command: str, env: dict[str, str], output_dir: Path, processes):
    log_path = output_dir / f"{name}.log"
    log_file = log_path.open("wb")
    log(f"start {name} on {node.name}: {command}")
    proc = getPopen(
        node,
        command,
        envDict=env,
        shell=True,
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
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


def count_negative_ack_reasons(text: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for match in NEGATIVE_ACK_RE.finditer(text):
        reason = match.group(1)
        counts[reason] = counts.get(reason, 0) + 1
    return counts


def run_user_case(ndn,
                  args: argparse.Namespace,
                  env: dict[str, str],
                  output_dir: Path,
                  processes,
                  name: str,
                  known_provider_ids: str) -> dict[str, object]:
    argv = [
        "--custom-selection",
        "--ack-timeout-ms", str(args.ack_timeout_ms),
    ]
    if known_provider_ids:
        argv.extend(["--known-provider-ids", known_provider_ids])
    start_time = time.monotonic()
    proc, log_path = start(
        ndn.net[args.user_node],
        name,
        app_cmd("App_User", *argv),
        env,
        output_dir,
        processes,
    )
    try:
        proc.wait(timeout=args.user_timeout_s)
    except Exception:
        proc.kill()
        proc.wait(timeout=3)
    elapsed_ms = (time.monotonic() - start_time) * 1000.0
    text = log_path.read_text(errors="replace")
    return {
        "name": name,
        "knownProviderIds": known_provider_ids,
        "returncode": proc.returncode,
        "elapsedMs": elapsed_ms,
        "log": str(log_path),
        "negativeAckReasons": count_negative_ack_reasons(text),
        "earlyStop": bool(EARLY_STOP_RE.search(text)),
        "timedOutMessage": "HELLO request timed out" in text,
        "responseReceived": "Received response:" in text,
    }


def main() -> int:
    args = build_parser().parse_args()
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    output_dir = Path(args.output_dir).resolve()
    if output_dir.exists():
        subprocess.run(["rm", "-rf", str(output_dir)], check=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    perf_args = make_perf_args(args)
    provider_nodes = perf.parse_csv_list(args.provider_nodes)
    provider_ids = perf.provider_ids(len(provider_nodes))
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
            app_cmd("App_ServiceController"),
            env,
            output_dir,
            processes,
        )
        if not wait_log(controller_log, "ServiceController listening on:",
                        args.controller_wait_s, controller_proc):
            raise RuntimeError(f"controller did not become ready; log={controller_log}")

        provider_logs = []
        for provider_node, provider_id, reason in zip(
                provider_nodes,
                provider_ids,
                ["QUEUE_FULL", "MODEL_UNAVAILABLE"]):
            proc, log_path = start(
                ndn.net[provider_node],
                f"provider-{provider_id}",
                app_cmd(
                    "App_Provider",
                    "--provider-id", provider_id,
                    "--ack-status", "reject",
                    "--ack-message", reason,
                    "--ack-payload", "queue=100;gpu=busy;rank=99",
                    "--response-payload", f"SHOULD_NOT_RUN_{provider_id}",
                ),
                env,
                output_dir,
                processes,
            )
            if not wait_log(log_path, f"Provider {provider_id} registered service /HELLO",
                            args.provider_wait_s, proc):
                raise RuntimeError(f"provider {provider_id} did not become ready; log={log_path}")
            provider_logs.append(str(log_path))

        known_case = run_user_case(
            ndn,
            args,
            env,
            output_dir,
            processes,
            "user-known-providers",
            ",".join(provider_ids),
        )
        discovery_case = run_user_case(
            ndn,
            args,
            env,
            output_dir,
            processes,
            "user-discovery-no-early-stop",
            "",
        )
        summary = {
            "status": "SUCCESS",
            "ackTimeoutMs": args.ack_timeout_ms,
            "requestTimeoutMs": 20000,
            "topology": str(args.topology_file),
            "controllerNode": args.controller_node,
            "userNode": args.user_node,
            "providerNodes": provider_nodes,
            "providerIds": provider_ids,
            "providerLogs": provider_logs,
            "controllerLog": str(controller_log),
            "cases": [known_case, discovery_case],
        }
        summary["knownProviderSpeedupVsDiscovery"] = (
            discovery_case["elapsedMs"] / known_case["elapsedMs"]
            if known_case["elapsedMs"] else 0.0
        )
        summary_path = output_dir / "negative-ack-early-stop-minindn-summary.json"
        summary_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(summary_path)
        print(json.dumps(summary, indent=2, sort_keys=True))
        if not known_case["earlyStop"] or not discovery_case["timedOutMessage"]:
            return 1
        return 0
    except Exception as exc:
        summary = {
            "status": "FAILURE",
            "error": str(exc),
            "outputDir": str(output_dir),
        }
        (output_dir / "negative-ack-early-stop-minindn-summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 1
    finally:
        stop(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
