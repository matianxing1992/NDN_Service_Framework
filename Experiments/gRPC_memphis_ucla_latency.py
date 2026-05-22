#!/usr/bin/env python3

import argparse
import heapq
import json
import os
import re
import signal
import sys
import time
from pathlib import Path

from mininet.log import setLogLevel, info
from mininet.node import Controller

from minindn.minindn import Minindn
from minindn.util import getPopen


def parse_topology(path):
    nodes = []
    links = []
    section = None
    for raw_line in Path(path).read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line == "[nodes]":
            section = "nodes"
            continue
        if line == "[links]":
            section = "links"
            continue
        if section == "nodes":
            nodes.append(line.rstrip(":"))
        elif section == "links":
            endpoints = line.split()[0]
            a, b = endpoints.split(":")
            delay_ms = 1.0
            match = re.search(r"delay=([0-9.]+)ms", line)
            if match:
                delay_ms = float(match.group(1))
            links.append((a, b, delay_ms))
    return nodes, links


def shortest_path(nodes, links, src, dst):
    graph = {node: [] for node in nodes}
    for a, b, delay_ms in links:
        graph[a].append((b, delay_ms))
        graph[b].append((a, delay_ms))
    queue = [(0.0, src, [src])]
    seen = {}
    while queue:
        cost, node, path = heapq.heappop(queue)
        if node in seen and seen[node] <= cost:
            continue
        seen[node] = cost
        if node == dst:
            return path, cost
        for neighbor, weight in graph[node]:
            heapq.heappush(queue, (cost + weight, neighbor, path + [neighbor]))
    raise RuntimeError(f"no path from {src} to {dst}")


def endpoint_ip_on_path(ndn, path):
    if len(path) < 2:
        return ndn.net[path[-1]].IP()
    endpoint = ndn.net[path[-1]]
    previous = ndn.net[path[-2]]
    return endpoint.connectionsTo(previous)[0][0].IP()


def add_route_to_destination(ndn, path, dst_ip):
    for node in path:
        ndn.net[node].cmd("sysctl -w net.ipv4.ip_forward=1 >/dev/null")
    for i in range(len(path) - 1):
        src_host = ndn.net[path[i]]
        next_hop = ndn.net[path[i + 1]]
        next_hop_ip = next_hop.connectionsTo(src_host)[0][0].IP()
        src_host.cmd(f"ip route replace {dst_ip}/32 via {next_hop_ip}")


def wait_for_log(path, pattern, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if path.exists() and pattern in path.read_text(errors="replace"):
            return True
        time.sleep(0.1)
    return False


def run():
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology-file", default="Experiments/Topology/testbed(loss=0%).conf")
    parser.add_argument("--client-node", default="memphis")
    parser.add_argument("--server-node", default="ucla")
    parser.add_argument("--delay-ms", type=int, default=5000)
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--rate-rps", type=float, default=0.0)
    parser.add_argument("--duration-s", type=float, default=0.0)
    parser.add_argument("--timeout-s", type=float, default=20.0)
    parser.add_argument("--warmup-s", type=float, default=5.0)
    parser.add_argument("--server-workers", type=int, default=32)
    parser.add_argument("--failure-probability", type=float, default=0.0)
    parser.add_argument("--epoch-ms", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=100)
    parser.add_argument("--output-dir", default="")
    args = parser.parse_args()

    output_dir = Path(args.output_dir or
                      f"results/grpc_memphis_ucla_delay{args.delay_ms}ms_{int(time.time())}")
    output_dir.mkdir(parents=True, exist_ok=True)

    Minindn.cleanUp()
    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        Minindn.verifyDependencies()
    finally:
        sys.argv = original_argv

    nodes, links = parse_topology(args.topology_file)
    forward_path, forward_delay_ms = shortest_path(nodes, links, args.client_node, args.server_node)
    reverse_path = list(reversed(forward_path))

    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        ndn = Minindn(topoFile=args.topology_file, controller=Controller)
    finally:
        sys.argv = original_argv
    server_proc = None
    client_proc = None
    try:
        ndn.start()
        client = ndn.net[args.client_node]
        server = ndn.net[args.server_node]
        server_ip = endpoint_ip_on_path(ndn, forward_path)
        client_ip = endpoint_ip_on_path(ndn, reverse_path)
        add_route_to_destination(ndn, forward_path, server_ip)
        add_route_to_destination(ndn, reverse_path, client_ip)
        info(f"Configured gRPC IP path {' -> '.join(forward_path)} "
             f"one_way_delay_ms={forward_delay_ms}\n")
        info(f"client={args.client_node} ip={client_ip} "
             f"server={args.server_node} ip={server_ip}\n")

        grpc_dir = Path.cwd() / "Experiments" / "gRPC"
        server_log = output_dir / "server.log"
        client_log = output_dir / "client.log"
        server_cmd = (
            f"python3 {grpc_dir / 'greeter_server.py'} "
            f"--bind 0.0.0.0:50051 --delay-ms {args.delay_ms} "
            f"--workers {args.server_workers} --quiet "
            f"--failure-probability {args.failure_probability} "
            f"--epoch-ms {args.epoch_ms} --seed {args.seed}"
        )
        client_cmd = (
            f"python3 {grpc_dir / 'greeter_client.py'} "
            f"--target {server_ip}:50051 --count {args.count} "
            f"--rate-rps {args.rate_rps} --duration-s {args.duration_s} "
            f"--timeout-s {args.timeout_s} --warmup-s {args.warmup_s} --quiet"
        )

        with server_log.open("w") as out:
            server_proc = getPopen(server, server_cmd, stdout=out, stderr=out)
        if not wait_for_log(server_log, "GRPC_SERVER_READY", 10):
            raise RuntimeError(f"gRPC server did not become ready; see {server_log}")

        ping = client.cmd(f"ping -c 2 -W 2 {server_ip}")
        (output_dir / "ping.txt").write_text(ping)
        info(ping)

        with client_log.open("w") as out:
            client_proc = getPopen(client, client_cmd, stdout=out, stderr=out)
            run_budget_s = args.duration_s if args.duration_s > 0 else args.timeout_s * args.count
            rc = client_proc.wait(timeout=max(30, int(run_budget_s + args.timeout_s + 10)))
        if rc != 0:
            raise RuntimeError(f"gRPC client failed rc={rc}; see {client_log}")

        summary_line = ""
        for line in client_log.read_text(errors="replace").splitlines():
            if line.startswith("GRPC_CLIENT_SUMMARY"):
                summary_line = line
        summary = {
            "client_node": args.client_node,
            "server_node": args.server_node,
            "client_ip": client_ip,
            "server_ip": server_ip,
            "path": forward_path,
            "one_way_link_delay_ms": forward_delay_ms,
            "service_delay_ms": args.delay_ms,
            "count": args.count,
            "rate_rps": args.rate_rps,
            "duration_s": args.duration_s,
            "warmup_s": args.warmup_s,
            "server_workers": args.server_workers,
            "failure_probability": args.failure_probability,
            "epoch_ms": args.epoch_ms,
            "seed": args.seed,
            "summary_line": summary_line,
            "output_dir": str(output_dir),
        }
        (output_dir / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary, indent=2))
    finally:
        for proc in (client_proc, server_proc):
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
