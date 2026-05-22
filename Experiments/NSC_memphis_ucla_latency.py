#!/usr/bin/env python3

import argparse
import json
import re
import signal
import sys
import time
from pathlib import Path

from mininet.log import setLogLevel, info
from mininet.node import Controller

from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
from minindn.minindn import Minindn
from minindn.util import getPopen


def wait_for_log(path, pattern, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if path.exists() and pattern in path.read_text(errors="replace"):
            return True
        time.sleep(0.1)
    return False


def parse_summary(line):
    result = {}
    for key, value in re.findall(r"([a-zA-Z0-9_]+)=([0-9.]+)", line):
        result[key] = float(value) if "." in value else int(value)
    return result


def run():
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology-file", default="Experiments/Topology/testbed(loss=0%).conf")
    parser.add_argument("--client-node", default="memphis")
    parser.add_argument("--server-node", default="ucla")
    parser.add_argument("--service-delay-ms", type=int, default=5)
    parser.add_argument("--rate-series", default="10,50,100")
    parser.add_argument("--duration-s", type=float, default=10.0)
    parser.add_argument("--warmup-s", type=float, default=5.0)
    parser.add_argument("--failure-probability", type=float, default=0.0)
    parser.add_argument("--epoch-ms", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=100)
    parser.add_argument("--output-dir", default="")
    args = parser.parse_args()

    rates = [float(item) for item in args.rate_series.split(",") if item.strip()]
    output_dir = Path(args.output_dir or
                      f"results/nsc_memphis_ucla_delay{args.service_delay_ms}ms_{int(time.time())}")
    output_dir.mkdir(parents=True, exist_ok=True)

    Minindn.cleanUp()
    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        Minindn.verifyDependencies()
    finally:
        sys.argv = original_argv

    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        ndn = Minindn(topoFile=args.topology_file, controller=Controller)
    finally:
        sys.argv = original_argv

    producer_proc = None
    consumer_proc = None
    summaries = []
    try:
        ndn.start()
        info("Starting NFD on nodes\n")
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="ERROR")

        info("Installing NSC static NDN routes\n")
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin([ndn.net[args.server_node]], [f"/muas/{args.server_node}"])
        routing.addOrigin([ndn.net[args.client_node]], [f"/muas/{args.client_node}"])
        routing.calculateNPossibleRoutes()

        for node_name in (args.client_node, args.server_node):
            ndn.net[node_name].cmd(f"ndnsec key-gen -t r /muas/{node_name} > /dev/null")

        nsc_dir = Path.cwd() / "Experiments" / "NDN_NSC"
        producer_log = output_dir / "producer.log"
        producer_cmd = (
            f"{nsc_dir / 'producer'} /muas/{args.server_node} "
            f"/FlightControl /ManualControl {args.service_delay_ms} "
            f"{args.failure_probability} {args.epoch_ms} {args.seed}"
        )
        with producer_log.open("w") as out:
            producer_proc = getPopen(ndn.net[args.server_node], producer_cmd, stdout=out, stderr=out)
        if not wait_for_log(producer_log, "REGISTER PREFIX", 10):
            raise RuntimeError(f"NSC producer did not register prefix; see {producer_log}")

        for rate in rates:
            interval_ms = max(1, int(round(1000.0 / rate)))
            count = max(1, int(round(args.duration_s * rate)))
            warmup_count = max(0, int(round(args.warmup_s * rate)))
            rate_label = str(rate).rstrip("0").rstrip(".")
            run_id = f"run{int(time.time() * 1000)}_r{rate_label}".replace(".", "_")
            client_log = output_dir / f"consumer_rate_{rate_label}.log"
            consumer_cmd = (
                f"{nsc_dir / 'consumer'} /muas/{args.client_node} /muas/{args.server_node} "
                f"/FlightControl /ManualControl {interval_ms} {count} {run_id} {warmup_count}"
            )
            info(f"Running NSC rate={rate_label} rps count={count} warmup_count={warmup_count} interval_ms={interval_ms}\n")
            with client_log.open("w") as out:
                consumer_proc = getPopen(ndn.net[args.client_node], consumer_cmd, stdout=out, stderr=out)
                consumer_proc.wait(timeout=int(args.duration_s + args.warmup_s + 35))
            summary_line = ""
            for line in client_log.read_text(errors="replace").splitlines():
                if line.startswith("NSC_CLIENT_SUMMARY"):
                    summary_line = line
            if not summary_line:
                raise RuntimeError(f"NSC consumer produced no summary; see {client_log}")
            summary = parse_summary(summary_line)
            actual_rps = summary.get("success", 0) / args.duration_s
            summary.update({
                "offered_rps": rate,
                "actual_rps": actual_rps,
                "interval_ms": interval_ms,
                "warmup_s": args.warmup_s,
                "warmup_count": warmup_count,
                "run_id": run_id,
                "duration_s": args.duration_s,
                "client_log": str(client_log),
            })
            summaries.append(summary)
            consumer_proc = None

        report = {
            "client_node": args.client_node,
            "server_node": args.server_node,
            "service_delay_ms": args.service_delay_ms,
            "duration_s": args.duration_s,
            "warmup_s": args.warmup_s,
            "failure_probability": args.failure_probability,
            "epoch_ms": args.epoch_ms,
            "seed": args.seed,
            "summaries": summaries,
            "output_dir": str(output_dir),
        }
        (output_dir / "summary.json").write_text(json.dumps(report, indent=2))
        print(json.dumps(report, indent=2))
    finally:
        for proc in (consumer_proc, producer_proc):
            if proc and proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=2)
                except Exception:
                    proc.kill()
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    setLogLevel("info")
    run()
