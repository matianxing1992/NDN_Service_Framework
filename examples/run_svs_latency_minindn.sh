#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
topology_file="${1:-${repo_root}/Experiments/Topology/testbed(loss=0%).conf}"
output_dir="${2:-${repo_root}/results/svs_latency_memphis_ucla_$(date +%Y%m%d_%H%M%S)}"
count="${SVS_LATENCY_COUNT:-30}"
interval_ms="${SVS_LATENCY_INTERVAL_MS:-100}"
timeout_ms="${SVS_LATENCY_TIMEOUT_MS:-30000}"
mode="${SVS_LATENCY_MODE:-echo}"
parallel_sync="${SVS_PARALLEL_SYNC:-0}"
parallel_workers="${SVS_PARALLEL_WORKERS:-2}"
parallel_queue="${SVS_PARALLEL_QUEUE:-128}"

cd "${repo_root}"
mkdir -p "${output_dir}"

sudo python3 - <<PY
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import Experiments.NDNSF_NewAPI_Minindn_Perf as perf

repo_root = Path(${repo_root@Q})
topology_file = Path(${topology_file@Q})
output_dir = Path(${output_dir@Q})
count = int(${count@Q})
interval_ms = int(${interval_ms@Q})
timeout_ms = int(${timeout_ms@Q})
mode = ${mode@Q}
parallel_sync = ${parallel_sync@Q} not in ("", "0", "false", "False", "no", "No")
parallel_workers = int(${parallel_workers@Q})
parallel_queue = int(${parallel_queue@Q})

def parse_ping_csv(*paths):
    rows = []
    marker = "SVS_LATENCY_CSV "
    for path in paths:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if marker in line:
                line = line.split(marker, 1)[1]
            if not line or line.startswith("role,") or not line.startswith("ping,echo,"):
                continue
            parts = line.split(",")
            if len(parts) < 10:
                continue
            rows.append({
                "seq": int(parts[2]),
                "forward_ms": float(parts[7]),
                "backward_ms": float(parts[8]),
                "rtt_ms": float(parts[9]),
            })
    return rows

def parse_oneway_csv(*paths):
    rows = []
    marker = "SVS_ONEWAY_CSV "
    for path in paths:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if marker in line:
                line = line.split(marker, 1)[1]
            if not line or line.startswith("role,") or not line.startswith("sub,receive,"):
                continue
            parts = line.split(",")
            if len(parts) < 7:
                continue
            rows.append({
                "seq": int(parts[2]),
                "publish_us": int(parts[3]),
                "receive_us": int(parts[4]),
                "oneway_ms": float(parts[5]),
            })
    return rows

def percentile(values, pct):
    if not values:
        return 0.0
    values = sorted(values)
    index = max(0, min(len(values) - 1, int(__import__("math").ceil(len(values) * pct / 100.0)) - 1))
    return values[index]

def stats(values):
    if not values:
        return {"count": 0, "avg": 0.0, "p50": 0.0, "p95": 0.0, "min": 0.0, "max": 0.0}
    return {
        "count": len(values),
        "avg": sum(values) / len(values),
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "min": min(values),
        "max": max(values),
    }

def run_svs_latency(ndn, args, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    user = ndn.net["memphis"]
    provider = ndn.net["ucla"]
    ping_log = output_dir / ("memphis-pub.log" if mode == "oneway" else "memphis-ping.csv")
    pong_log = output_dir / ("ucla-sub.log" if mode == "oneway" else "ucla-pong.csv")
    ping_err_path = output_dir / ("memphis-pub.err" if mode == "oneway" else "memphis-ping.err")
    pong_err_path = output_dir / ("ucla-sub.err" if mode == "oneway" else "ucla-pong.err")

    ping_cmd = (
        "export NDN_LOG=ndn_service_framework.AppSvsLatency=INFO:ndn_svs.SyncTimeline=INFO; "
        "exec {app} --role {ping_role} --sync-prefix /example/hello/group "
        "--node-prefix /example/hello/user/svs-latency "
        "--peer-prefix /example/hello/provider/A/svs-latency "
        "--count {count} --interval-ms {interval_ms} --timeout-ms {timeout_ms} --csv {parallel_args}"
    ).format(app=repo_root / "build/examples/App_SvsLatency",
             ping_role="pub" if mode == "oneway" else "ping",
             count=count, interval_ms=interval_ms, timeout_ms=timeout_ms,
             parallel_args=("--parallel-sync --parallel-workers {} --parallel-queue {}"
                            .format(parallel_workers, parallel_queue) if parallel_sync else ""))
    pong_cmd = (
        "export NDN_LOG=ndn_service_framework.AppSvsLatency=INFO:ndn_svs.SyncTimeline=INFO; "
        "exec {app} --role {pong_role} --sync-prefix /example/hello/group "
        "--node-prefix /example/hello/provider/A/svs-latency "
        "--peer-prefix /example/hello/user/svs-latency "
        "--count {count} --interval-ms {interval_ms} --timeout-ms {timeout_ms} --csv {parallel_args}"
    ).format(app=repo_root / "build/examples/App_SvsLatency",
             pong_role="sub" if mode == "oneway" else "pong",
             count=count, interval_ms=interval_ms, timeout_ms=timeout_ms,
             parallel_args=("--parallel-sync --parallel-workers {} --parallel-queue {}"
                            .format(parallel_workers, parallel_queue) if parallel_sync else ""))

    processes = []
    with pong_log.open("wb") as pong_out, ping_log.open("wb") as ping_out, \
         pong_err_path.open("wb") as pong_err, ping_err_path.open("wb") as ping_err:
        pong = perf.getPopen(provider, pong_cmd, shell=True,
                             stdout=pong_out, stderr=pong_err)
        processes.append((pong, pong_out))
        time.sleep(2)
        ping = perf.getPopen(user, ping_cmd, shell=True,
                             stdout=ping_out, stderr=ping_err)
        processes.append((ping, ping_out))
        try:
            if mode == "oneway":
                pong.wait(timeout=(timeout_ms / 1000.0) + 15)
            else:
                ping.wait(timeout=(timeout_ms / 1000.0) + 15)
        finally:
            perf.stop_processes(processes)

    if mode == "oneway":
        rows = parse_oneway_csv(pong_log, pong_err_path)
        summary = {
            "mode": mode,
            "count_requested": count,
            "count_received": len(rows),
            "oneway_memphis_to_ucla_ms": stats([row["oneway_ms"] for row in rows]),
            "publisher_log": str(ping_log),
            "subscriber_log": str(pong_log),
            "publisher_err": str(ping_err_path),
            "subscriber_err": str(pong_err_path),
            "publisher_returncode": ping.returncode,
            "subscriber_returncode": pong.returncode,
        }
    else:
        rows = parse_ping_csv(ping_log, ping_err_path)
        summary = {
            "mode": mode,
            "count_requested": count,
            "count_received": len(rows),
            "forward_memphis_to_ucla_ms": stats([row["forward_ms"] for row in rows]),
            "backward_ucla_to_memphis_ms": stats([row["backward_ms"] for row in rows]),
            "rtt_ms": stats([row["rtt_ms"] for row in rows]),
            "ping_log": str(ping_log),
            "pong_log": str(pong_log),
            "ping_err": str(ping_err_path),
            "pong_err": str(pong_err_path),
            "ping_returncode": ping.returncode,
            "pong_returncode": pong.returncode,
        }
    (output_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\\n")
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)
    if len(rows) != count:
        raise RuntimeError("Only received {} of {} SVS publications".format(len(rows), count))

def main():
    perf.setLogLevel("info")
    parser = perf.build_parser()
    argv = [
        "svs-latency",
        "--topology-file", str(topology_file),
        "--user-node", "memphis",
        "--controller-node", "memphis",
        "--provider-nodes", "ucla",
        "--connectivity-check",
        "--nlsr-converge-seconds", "30",
        "--output-dir", str(output_dir),
    ]
    sys.argv = argv
    pre_args, _ = parser.parse_known_args()
    perf.normalize_placement_args(pre_args)
    perf.ensure_runtime(pre_args)
    perf.ensure_minindn_root()

    print("Cleaning MiniNDN state", flush=True)
    perf.Minindn.cleanUp()
    perf.Minindn.verifyDependencies()

    ndn = perf.Minindn(parser=parser, topoFile=str(topology_file))
    args = perf.normalize_placement_args(ndn.args)
    args.experiment_started_at = time.time()
    try:
        ndn.start()
        perf.validate_node_placement({host.name for host in ndn.net.hosts}, args)
        print("Starting NFD/NLSR", flush=True)
        perf.AppManager(ndn, ndn.net.hosts, perf.Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, output_dir)
        perf.AppManager(ndn, ndn.net.hosts, perf.Nlsr)
        time.sleep(2)
        perf.configure_static_routes(ndn, args)
        perf.advertise_nlsr_prefixes(ndn, args)
        perf.wait_for_nlsr_convergence(ndn, args, output_dir)
        run_svs_latency(ndn, args, output_dir)
    finally:
        ndn.stop()
        perf.Minindn.cleanUp()

main()
PY

echo "SVS latency results: ${output_dir}"
