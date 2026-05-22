#!/usr/bin/env python3

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

from mininet.log import info, setLogLevel
from mininet.node import Controller

from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.apps.nlsr import Nlsr
from minindn.minindn import Minindn
from minindn.util import getPopen

import NDNSF_NewAPI_Minindn_Perf as perf


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_CONTROLLER = REPO_ROOT / "build/examples/App_ServiceController"
APP_PROVIDER = REPO_ROOT / "build/examples/App_IntermittentProvider"
APP_USER = REPO_ROOT / "build/examples/App_IntermittentUser"


def log(message):
    info(f"{message}\n")


def parse_probabilities(value):
    return [float(item) for item in value.split(",") if item.strip()]


def make_perf_args(args):
    return SimpleNamespace(
        providers=3,
        provider_nodes=args.provider_nodes,
        user_node=args.user_node,
        controller_node=args.controller_node,
        serve_provider_certs=False,
        debug_ack=False,
        timeline_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_parallel_sync_processing=False,
        svs_parallel_workers=4,
        svs_parallel_queue=256,
        svs_sync_publish=False,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
    )


def wait_for_log(path, pattern, timeout_s, process=None):
    regex = re.compile(pattern)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if process is not None and process.poll() is not None:
            return False
        if path.exists() and regex.search(path.read_text(errors="replace")):
            return True
        time.sleep(0.2)
    return False


def parse_summary(log_path):
    summary_line = ""
    for line in log_path.read_text(errors="replace").splitlines():
        if line.startswith("INTERMITTENT_USER_SUMMARY"):
            summary_line = line
    if not summary_line:
        raise RuntimeError(f"user summary missing in {log_path}")
    result = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", summary_line):
        try:
            result[key] = float(value)
        except ValueError:
            result[key] = value
    return result


def start_process(node, name, binary, argv, output_dir, session_base, perf_args):
    log_path = output_dir / f"{name}.log"
    log(f"Starting {name} on {node.name} -> {log_path}")
    log_file = log_path.open("wb")
    process = getPopen(
        node,
        perf.managed_cmd(binary, argv),
        envDict=perf.app_env(output_dir, session_base, perf_args),
        shell=True,
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    return process, log_file, log_path


def stop_processes(processes):
    for process, log_file, _ in reversed(processes):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=3)
            except Exception:
                process.kill()
        log_file.close()


def run_one(args, probability, run_dir):
    perf_args = make_perf_args(args)
    processes = []
    session_base = int(time.time()) + os.getpid()
    Minindn.cleanUp()
    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=args.topology_file, controller=Controller)
    finally:
        sys.argv = original_argv

    try:
        ndn.start()
        log("Starting NFD")
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel=args.nfd_log_level)
        log("Starting NLSR")
        AppManager(ndn, ndn.net.hosts, Nlsr)
        time.sleep(2)
        perf.configure_static_routes(ndn, perf_args)
        perf.advertise_nlsr_prefixes(ndn, perf_args)
        time.sleep(args.nlsr_settle_seconds)
        perf.initialize_example_keychains(ndn, perf_args, run_dir)

        controller_proc, controller_file, controller_log = start_process(
            ndn.net[args.controller_node],
            "controller",
            APP_CONTROLLER,
            [],
            run_dir,
            session_base,
            perf_args,
        )
        processes.append((controller_proc, controller_file, controller_log))
        if not wait_for_log(controller_log, r"ServiceController listening on:", 20,
                            controller_proc):
            raise RuntimeError(f"controller not ready; see {controller_log}")
        time.sleep(args.controller_settle_seconds)

        provider_nodes = [item.strip() for item in args.provider_nodes.split(",")]
        for index, node_name in enumerate(provider_nodes):
            provider_id = chr(ord("A") + index)
            proc, log_file, log_path = start_process(
                ndn.net[node_name],
                f"provider-{provider_id}",
                APP_PROVIDER,
                [
                    "--provider-id", provider_id,
                    "--failure-probability", str(probability),
                    "--epoch-ms", str(args.epoch_ms),
                    "--reject-ms", str(args.reject_ms),
                    "--processing-delay-ms", str(args.processing_delay_ms),
                    "--handler-threads", str(args.provider_handler_threads),
                    "--seed", str(args.seed_base + index),
                ],
                run_dir,
                session_base,
                perf_args,
            )
            processes.append((proc, log_file, log_path))
            if not wait_for_log(log_path, r"INTERMITTENT_PROVIDER_READY", 30, proc):
                raise RuntimeError(f"provider {provider_id} not ready; see {log_path}")
            time.sleep(args.provider_start_gap_seconds)

        time.sleep(args.provider_settle_seconds)
        user_proc, user_file, user_log = start_process(
            ndn.net[args.user_node],
            "user",
            APP_USER,
            [
                "--rate-rps", str(args.rate_rps),
                "--duration-ms", str(args.duration_s * 1000),
                "--ack-timeout-ms", str(args.ack_timeout_ms),
                "--timeout-ms", str(args.timeout_ms),
                "--startup-delay-ms", str(args.user_startup_delay_ms),
            ],
            run_dir,
            session_base,
            perf_args,
        )
        processes.append((user_proc, user_file, user_log))
        user_proc.wait(timeout=args.duration_s + args.timeout_ms / 1000.0 + 40)
        summary = parse_summary(user_log)
        summary.update({
            "failure_probability": probability,
            "duration_s": args.duration_s,
            "rate_rps": args.rate_rps,
            "user_log": str(user_log),
            "run_dir": str(run_dir),
        })
        (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        return summary
    finally:
        stop_processes(processes)
        try:
            ndn.stop()
        finally:
            Minindn.cleanUp()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology-file", default="Experiments/Topology/testbed(loss=0%).conf")
    parser.add_argument("--user-node", default="memphis")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--provider-nodes", default="ucla,wustl,uiuc")
    parser.add_argument("--probabilities", default="0.1,0.2,0.3")
    parser.add_argument("--rate-rps", type=float, default=10.0)
    parser.add_argument("--duration-s", type=int, default=120)
    parser.add_argument("--epoch-ms", type=int, default=10000)
    parser.add_argument("--reject-ms", type=int, default=10000)
    parser.add_argument("--processing-delay-ms", type=int, default=5)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--ack-timeout-ms", type=int, default=200)
    parser.add_argument("--user-startup-delay-ms", type=int, default=2500)
    parser.add_argument("--nlsr-settle-seconds", type=int, default=20)
    parser.add_argument("--controller-settle-seconds", type=int, default=5)
    parser.add_argument("--provider-start-gap-seconds", type=int, default=2)
    parser.add_argument("--provider-settle-seconds", type=int, default=5)
    parser.add_argument("--provider-handler-threads", type=int, default=4)
    parser.add_argument("--seed-base", type=int, default=4242)
    parser.add_argument("--nfd-log-level", default="ERROR")
    parser.add_argument("--output-dir", default="")
    args = parser.parse_args()

    output_dir = Path(args.output_dir or
                      f"results/intermittent_provider_failure_{int(time.time())}").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    summaries = []
    for probability in parse_probabilities(args.probabilities):
        label = str(probability).replace(".", "p")
        run_dir = output_dir / f"prob_{label}"
        run_dir.mkdir(parents=True, exist_ok=True)
        log(f"Running NDNSF intermittent provider failure probability={probability}")
        summaries.append(run_one(args, probability, run_dir))

    report = {
        "system": "NDNSF",
        "topology_file": args.topology_file,
        "user_node": args.user_node,
        "provider_nodes": args.provider_nodes,
        "controller_node": args.controller_node,
        "admission_control": "disabled",
        "summaries": summaries,
    }
    (output_dir / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    with (output_dir / "summary.csv").open("w") as out:
        out.write("failure_probability,actual_rps,sent,success,timeout,bad_response,success_rate,timeout_rate\n")
        for item in summaries:
            out.write("{failure_probability},{actual_rps},{sent},{success},{timeout},{bad_response},{success_rate},{timeout_rate}\n".format(**item))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    setLogLevel("info")
    main()
