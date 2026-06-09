#!/usr/bin/env python3
"""MiniNDN smoke test for YOLO-style NDNSF-DI layout split inference."""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import pwd
from pathlib import Path
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

TOPO = REPO / "Experiments/Topology/AI_Lab.conf"
OUT = REPO / "results/yolo_2x2_minindn_quick"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"
CONFIG = OUT / "yolo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-yolo-2x2-policy"
REPO_MANIFEST = OUT / "repo-manifests.json"
APP_ROOT = "/NDNSF-DistributeInference/example"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"
AI_LAB_CONTROLLER_NODE = "memphis"
AI_LAB_USER_NODE = "memphis"
AI_LAB_REPO_NODE = "neu"
AI_LAB_PROVIDER_NODES = ["ucla", "arizona", "wustl", "neu"]


class Args(SimpleNamespace):
    pass


def log(message: str) -> None:
    info(message + "\n")


def python_cmd(script: str, argv: list[str]) -> str:
    args = " ".join([perf.shell_quote(str(PY_DIR / script))] +
                    [perf.shell_quote(arg) for arg in argv])
    return f"cd {perf.shell_quote(REPO)} && exec python3 {args}"


def run_user_python_step(command: list[str], *, cwd: str, env: dict[str, str], writable_path: Path) -> None:
    """Run pre-MiniNDN Python generation in the invoking user's Python env."""
    sudo_user = os.environ.get("SUDO_USER", "")
    sudo_uid = os.environ.get("SUDO_UID", "")
    sudo_gid = os.environ.get("SUDO_GID", "")
    if os.geteuid() == 0 and sudo_user and sudo_uid and sudo_gid:
        writable_path.mkdir(parents=True, exist_ok=True)
        subprocess.run(["chown", "-R", f"{sudo_uid}:{sudo_gid}", str(writable_path)], check=True)
        user_home = pwd.getpwnam(sudo_user).pw_dir
        env = {**env, "HOME": user_home}
        env_args = []
        for key in ("PYTHONPATH", "LD_LIBRARY_PATH", "PATH", "HOME"):
            value = env.get(key, os.environ.get(key))
            if value:
                env_args.append(f"{key}={value}")
        subprocess.run(["sudo", "-n", "-u", sudo_user, "env", *env_args, *command],
                       cwd=cwd,
                       check=True)
        return
    subprocess.run(command, cwd=cwd, env=env, check=True)


def native_provider_cmd(argv: list[str],
                        service_name: str,
                        workers: int,
                        handler_threads: int,
                        ack_threads: int) -> str:
    exe = REPO / "build/examples/di-native-provider"
    provider_id = argv[argv.index("--provider-id") + 1]
    roles = argv[argv.index("--roles") + 1]
    args = [
        str(exe),
        "--serve",
        "--plan", str(Path(GEN_POLICY) / "native-execution-plan.json"),
        "--manifest", str(Path(GEN_POLICY) / "service-manifest.json"),
        "--service", service_name,
        "--provider", provider_identity(provider_id),
        "--group", APP_ROOT + "/group",
        "--controller", CONTROLLER_IDENTITY,
        "--trust-schema", "examples/trust-schema.conf",
        "--roles", roles,
        "--workers", str(workers),
        "--handler-threads", str(handler_threads),
        "--ack-threads", str(ack_threads),
    ]
    quoted = " ".join(perf.shell_quote(item) for item in args)
    return f"cd {perf.shell_quote(REPO)} && exec {quoted}"


def start(node, name, cmd, env, procs):
    path = OUT / f"{name}.log"
    f = path.open("wb")
    log(f"start {name} on {node.name}: {cmd}")
    node_env = dict(env)
    node_env["NDNSF_ARTIFACT_CACHE_DIR"] = str(OUT / "artifact-cache" / node.name)
    p = getPopen(node, cmd, envDict=node_env, shell=True, stdout=f, stderr=subprocess.STDOUT)
    procs.append((p, f, path))
    return p, path


def stop_process_group(procs: list[tuple[object, object, Path]]) -> None:
    for p, f, _ in reversed(procs):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        f.close()


def start_ndn_packet_traces(ndn, env: dict, node_names: list[str]):
    traces = []
    if not node_names:
        return traces
    trace_dir = OUT / "ndn-packet-trace"
    trace_dir.mkdir(parents=True, exist_ok=True)
    for node_name in node_names:
        if node_name not in ndn.net:
            continue
        pcap_path = trace_dir / f"{node_name}.pcap"
        decoded_path = trace_dir / f"{node_name}.ndndump.log"
        log_path = trace_dir / f"{node_name}.tcpdump.log"
        f = log_path.open("wb")
        cmd = (
            "exec tcpdump -i any -s 0 -U -w "
            f"{perf.shell_quote(str(pcap_path))} "
            "'(ether proto 0x8624) or (tcp port 6363) or "
            "(udp port 6363) or (udp port 56363)'"
        )
        log(f"start tcpdump NDN trace on {node_name}: {cmd}")
        proc = getPopen(ndn.net[node_name], cmd, envDict=env, shell=True,
                        stdout=f, stderr=subprocess.STDOUT)
        traces.append((proc, f, pcap_path, decoded_path, node_name))
    return traces


def stop_ndn_packet_traces(traces) -> None:
    for proc, f, pcap_path, decoded_path, _ in traces:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
        try:
            f.close()
        except Exception:
            pass
        if pcap_path.exists() and pcap_path.stat().st_size > 24:
            with decoded_path.open("wb") as out:
                # Decode the full pcap and filter in Python. ndndump's offline
                # -f path can produce truncated output for some captured UDP
                # packets, which hides exactly the packet names this diagnostic
                # is meant to observe.
                subprocess.run(
                    ["ndndump", "-r", str(pcap_path)],
                    stdout=out,
                    stderr=subprocess.STDOUT,
                    check=False,
                )


def parse_ndndump_line(line: str) -> dict:
    text = line.strip()
    if not text:
        return {}
    if text.startswith("ndndump:"):
        return {}
    timestamp = 0.0
    match = re.match(r"^([0-9]+(?:\.[0-9]+)?)\s+(.*)$", text)
    if match:
        timestamp = float(match.group(1))
        text = match.group(2)
    endpoint_match = re.search(
        r"\bIP\s+([^,\s]+)\s+>\s+([^,\s]+),\s+UDP(?:,\s+length\s+([0-9]+))?",
        text)
    src = endpoint_match.group(1) if endpoint_match else ""
    dst = endpoint_match.group(2) if endpoint_match else ""
    wire_length = int(endpoint_match.group(3)) if endpoint_match and endpoint_match.group(3) else 0
    pkt_type = ""
    if re.search(r"\bINTEREST\b", text, re.IGNORECASE):
        pkt_type = "Interest"
    elif re.search(r"\bDATA\b", text, re.IGNORECASE):
        pkt_type = "Data"
    name = ""
    # ndndump variants print either "Interest /name" or verbose text that
    # still contains an absolute NDN name token. Keep the extraction loose.
    name_match = re.search(r"((?:/[A-Za-z0-9%._~!$&'()*+,;=:@-]+)+)", text)
    if name_match:
        name = name_match.group(1)
    if name and "NDNSF" not in name:
        return {}
    if not pkt_type and not name:
        return {}
    category = "other"
    if "/ndnping/" in name:
        category = "ndnping"
    elif "/NDNSF/DI/ACTIVATION/" in name:
        category = "di-activation"
    elif "/group/v=2/params-sha256=" in name:
        category = "svs-sync"
    elif "/group/MAPPING/" in name:
        category = "svs-mapping"
    elif "/group/t=" in name or "/group/" in name:
        category = "svs-data"
    elif "/NDNSF/REQUEST/" in name:
        category = "request"
    elif "/NDNSF/ACK/" in name:
        category = "ack"
    elif "/NDNSF/SELECTION/" in name:
        category = "selection"
    elif "/NDNSF/RESPONSE/" in name:
        category = "response"
    elif "/NDNSF/DistributedRepo" in name:
        category = "repo"
    return {
        "timestamp": timestamp,
        "type": pkt_type,
        "name": name,
        "src": src,
        "dst": dst,
        "wireLength": wire_length,
        "category": category,
        "line": line.rstrip("\n"),
    }


def write_ndn_packet_trace_summary(traces) -> None:
    if not traces:
        return
    rows = []
    by_node = {}
    by_name = {}
    for _, _, pcap_path, path, node_name in traces:
        node_rows = []
        if path.exists():
            for line in path.read_text(errors="replace").splitlines():
                row = parse_ndndump_line(line)
                if not row:
                    continue
                row["node"] = node_name
                node_rows.append(row)
                rows.append(row)
                name = row.get("name", "")
                if name:
                    item = by_name.setdefault(name, {
                        "name": name,
                        "type": row.get("type", ""),
                        "category": row.get("category", ""),
                        "nodes": {},
                        "firstTimestamp": 0.0,
                        "lastTimestamp": 0.0,
                    })
                    ts = float(row.get("timestamp", 0.0) or 0.0)
                    if ts:
                        if not item["firstTimestamp"] or ts < item["firstTimestamp"]:
                            item["firstTimestamp"] = ts
                        if ts > item["lastTimestamp"]:
                            item["lastTimestamp"] = ts
                    node_item = item["nodes"].setdefault(node_name, {
                        "count": 0,
                        "firstTimestamp": 0.0,
                        "lastTimestamp": 0.0,
                        "endpoints": {},
                    })
                    node_item["count"] += 1
                    endpoint = f"{row.get('src', '')}>{row.get('dst', '')}"
                    if endpoint != ">":
                        node_item["endpoints"][endpoint] = (
                            node_item["endpoints"].get(endpoint, 0) + 1)
                    if ts:
                        if not node_item["firstTimestamp"] or ts < node_item["firstTimestamp"]:
                            node_item["firstTimestamp"] = ts
                        if ts > node_item["lastTimestamp"]:
                            node_item["lastTimestamp"] = ts
        counts = {}
        first_ts = 0.0
        last_ts = 0.0
        for row in node_rows:
            key = f"{row.get('category', 'other')}:{row.get('type', '') or 'unknown'}"
            counts[key] = counts.get(key, 0) + 1
            ts = float(row.get("timestamp", 0.0) or 0.0)
            if ts:
                first_ts = ts if not first_ts else min(first_ts, ts)
                last_ts = max(last_ts, ts)
        by_node[node_name] = {
            "path": str(path),
            "pcap": str(pcap_path),
            "count": len(node_rows),
            "counts": counts,
            "firstTimestamp": first_ts,
            "lastTimestamp": last_ts,
            "spanMs": (last_ts - first_ts) * 1000.0 if first_ts and last_ts else 0.0,
        }
    name_rows = list(by_name.values())
    name_rows.sort(key=lambda row: (row.get("firstTimestamp", 0.0), row.get("name", "")))
    span_values = {}
    for row in name_rows:
        first_ts = float(row.get("firstTimestamp", 0.0) or 0.0)
        last_ts = float(row.get("lastTimestamp", 0.0) or 0.0)
        if not first_ts or not last_ts:
            continue
        span_ms = max(0.0, (last_ts - first_ts) * 1000.0)
        key = f"{row.get('category', 'other')}:{row.get('type', '') or 'unknown'}"
        span_values.setdefault(key, []).append(span_ms)
    span_stats = {
        key: {
            "count": len(values),
            "p50Ms": percentile(values, 50),
            "p95Ms": percentile(values, 95),
            "maxMs": max(values) if values else 0.0,
        }
        for key, values in sorted(span_values.items())
    }
    slowest_spans = []
    most_repeated = []
    for row in name_rows:
        first_ts = float(row.get("firstTimestamp", 0.0) or 0.0)
        last_ts = float(row.get("lastTimestamp", 0.0) or 0.0)
        span_ms = max(0.0, (last_ts - first_ts) * 1000.0) if first_ts and last_ts else 0.0
        total_count = sum(int(node.get("count", 0)) for node in row.get("nodes", {}).values())
        item = {
            "name": row.get("name", ""),
            "type": row.get("type", ""),
            "category": row.get("category", ""),
            "spanMs": span_ms,
            "count": total_count,
            "nodes": row.get("nodes", {}),
        }
        slowest_spans.append(item)
        most_repeated.append(item)
    slowest_spans.sort(key=lambda row: (-row["spanMs"], row["category"], row["name"]))
    most_repeated.sort(key=lambda row: (-row["count"], -row["spanMs"], row["category"], row["name"]))
    summary = {
        "count": len(rows),
        "nodes": by_node,
        "names": name_rows[:500],
        "nameCount": len(name_rows),
        "crossNodeObservationSpanStats": span_stats,
        "slowestCrossNodeObservationSpans": slowest_spans[:50],
        "mostRepeatedNames": most_repeated[:50],
        "note": (
            "Packet trace is external ndndump observation. It does not expose "
            "ndn-svs internal scheduling, but it shows when each MiniNDN node's "
            "NFD-facing capture saw Sync/control Interest/Data names."
        ),
    }
    path = OUT / "ndn-packet-trace-summary.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    sync_count = sum(
        count for node in by_node.values()
        for key, count in node["counts"].items()
        if key.startswith("svs-")
    )
    control_count = sum(
        count for node in by_node.values()
        for key, count in node["counts"].items()
        if key.split(":", 1)[0] in {"request", "ack", "selection", "response"}
    )
    print(
        "YOLO_LAYOUT_NDN_PACKET_TRACE "
        f"packets={len(rows)} sync_packets={sync_count} "
        f"control_packets={control_count} unique_names={len(name_rows)} "
        f"path={path}"
    )


def wait_log(path: Path, needle: str, timeout: int = 30, proc=None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def print_user_workload_output(text: str, quiet: bool) -> None:
    if not quiet:
        print(text)
        return
    allowed_prefixes = (
        "Run YOLO",
        "NDNSF_DI_PLAN_CACHE",
        "NDNSF_DI_CLIENT_INFERENCE_TIMING",
        "YOLO_LAYOUT_CONTROL_TIMING",
        "YOLO_LAYOUT_RESULT",
        "YOLO_2X2_RESULT",
    )
    lines = [
        line for line in text.splitlines()
        if line.startswith(allowed_prefixes)
    ]
    if lines:
        print("\n".join(lines) + "\n")


def parse_ndnping_rtts(text: str) -> list[float]:
    values = []
    for match in re.finditer(r"time=([0-9]+(?:\.[0-9]+)?)\s*ms", text):
        values.append(float(match.group(1)))
    return values


def write_ndnping_rtt_summary(ndn,
                              providers: list[tuple[str, str, list[str]]],
                              env: dict,
                              procs) -> None:
    rows = []
    for node_name, provider_log_name, argv in providers:
        provider_id = argv[argv.index("--provider-id") + 1]
        provider_prefix = provider_identity(provider_id)
        ping_prefix = f"{provider_prefix}/ndnping"
        server_log = OUT / f"ndnpingserver-{provider_log_name}.log"
        client_log = OUT / f"ndnping-user-to-{provider_log_name}.log"
        server_file = server_log.open("wb")
        server_cmd = f"exec ndnpingserver {perf.shell_quote(ping_prefix)}"
        server = getPopen(ndn.net[node_name], server_cmd, envDict=env, shell=True,
                          stdout=server_file, stderr=subprocess.STDOUT)
        procs.append((server, server_file, server_log))
        time.sleep(0.5)
        client_file = client_log.open("wb")
        client_cmd = f"exec timeout 15s ndnping {perf.shell_quote(ping_prefix)} -c 5"
        client = getPopen(ndn.net["memphis"], client_cmd, envDict=env, shell=True,
                          stdout=client_file, stderr=subprocess.STDOUT)
        try:
            client.wait(timeout=20)
        except subprocess.TimeoutExpired:
            client.terminate()
            client.wait(timeout=5)
        client_file.close()
        text = client_log.read_text(errors="replace") if client_log.exists() else ""
        rtts = parse_ndnping_rtts(text)
        row = {
            "providerLog": provider_log_name,
            "providerNode": node_name,
            "providerPrefix": provider_prefix,
            "pingPrefix": ping_prefix,
            "returncode": client.returncode,
            "count": len(rtts),
            "rttsMs": rtts,
            "summaryMs": summarize_numeric(rtts),
            "clientLog": str(client_log),
            "serverLog": str(server_log),
        }
        rows.append(row)

    summary = {
        "sourceNode": "memphis",
        "count": len(rows),
        "rows": rows,
    }
    path = OUT / "ndnping-rtt-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    printable = " ".join(
        f"{row['providerLog']}={row['summaryMs']['mean']:.2f}ms"
        if row["summaryMs"]["count"] else f"{row['providerLog']}=NA"
        for row in rows
    )
    print(f"YOLO_LAYOUT_NDNPING_RTT {printable} path={path}")


def provider_metadata(providers: list[tuple[str, str, list[str]]]) -> dict[str, dict[str, str]]:
    metadata = {}
    for node_name, provider_log_name, argv in providers:
        provider_id = argv[argv.index("--provider-id") + 1]
        roles = []
        if "--roles" in argv:
            roles = str(argv[argv.index("--roles") + 1]).split(",")
        elif "--role" in argv:
            roles = [str(argv[argv.index("--role") + 1])]
        for role in roles:
            if not role:
                continue
            metadata[role] = {
                "providerId": provider_id,
                "providerLog": provider_log_name,
                "providerNode": node_name,
                "providerPrefix": provider_identity(provider_id),
            }
    return metadata


def load_native_dependency_edges(plan_path: Path) -> list[dict]:
    if not plan_path.exists():
        return []
    data = json.loads(plan_path.read_text(encoding="utf-8"))
    edges = []
    for service in data.get("services", []):
        for dependency in service.get("dependencies", []):
            producers = [str(item) for item in dependency.get("producers", [])]
            consumers = [str(item) for item in dependency.get("consumers", [])]
            for producer in producers:
                for consumer in consumers:
                    edges.append({
                        "producerRole": producer,
                        "consumerRole": consumer,
                        "scope": str(dependency.get("keyScope", "")),
                        "expectedSegments": int(dependency.get("expectedSegments", 0) or 0),
                        "expectedBytes": int(dependency.get("expectedBytes", 0) or 0),
                    })
    return edges


def write_dependency_edge_rtt_summary(ndn,
                                      providers: list[tuple[str, str, list[str]]],
                                      env: dict,
                                      procs,
                                      plan_path: Path) -> None:
    role_meta = provider_metadata(providers)
    rows = []
    for index, edge in enumerate(load_native_dependency_edges(plan_path)):
        producer = role_meta.get(edge["producerRole"])
        consumer = role_meta.get(edge["consumerRole"])
        if not producer or not consumer:
            continue
        ping_prefix = f"{producer['providerPrefix']}/ndnping"
        server_log = OUT / f"ndnpingserver-edge-{index}-{producer['providerLog']}.log"
        client_log = OUT / (
            f"ndnping-edge-{index}-{consumer['providerLog']}-to-"
            f"{producer['providerLog']}.log")
        server_file = server_log.open("wb")
        server_cmd = f"exec ndnpingserver {perf.shell_quote(ping_prefix)}"
        server = getPopen(ndn.net[producer["providerNode"]],
                          server_cmd,
                          envDict=env,
                          shell=True,
                          stdout=server_file,
                          stderr=subprocess.STDOUT)
        procs.append((server, server_file, server_log))
        time.sleep(0.3)
        client_file = client_log.open("wb")
        client_cmd = f"exec timeout 15s ndnping {perf.shell_quote(ping_prefix)} -c 5"
        client = getPopen(ndn.net[consumer["providerNode"]],
                          client_cmd,
                          envDict=env,
                          shell=True,
                          stdout=client_file,
                          stderr=subprocess.STDOUT)
        try:
            client.wait(timeout=20)
        except subprocess.TimeoutExpired:
            client.terminate()
            client.wait(timeout=5)
        client_file.close()
        text = client_log.read_text(errors="replace") if client_log.exists() else ""
        rtts = parse_ndnping_rtts(text)
        row = {
            **edge,
            "producerNode": producer["providerNode"],
            "producerLog": producer["providerLog"],
            "producerPrefix": producer["providerPrefix"],
            "consumerNode": consumer["providerNode"],
            "consumerLog": consumer["providerLog"],
            "pingPrefix": ping_prefix,
            "returncode": client.returncode,
            "count": len(rtts),
            "rttsMs": rtts,
            "summaryMs": summarize_numeric(rtts),
            "clientLog": str(client_log),
            "serverLog": str(server_log),
        }
        rows.append(row)
    summary = {
        "plan": str(plan_path),
        "count": len(rows),
        "rows": rows,
    }
    path = OUT / "dependency-edge-ndnping-rtt-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    printable = " ".join(
        f"{row['scope']}={row['summaryMs']['mean']:.2f}ms"
        if row["summaryMs"]["count"] else f"{row['scope']}=NA"
        for row in rows
    )
    print(f"YOLO_LAYOUT_DEPENDENCY_EDGE_RTT {printable} path={path}")


def ndnping_once_from_user(ndn, ping_prefix: str) -> dict:
    start = time.time()
    text = perf.node_cmd(
        ndn.net["memphis"],
        f"timeout 5s ndnping {perf.shell_quote(ping_prefix)} -c 1 2>&1",
    )
    finished = time.time()
    rtts = parse_ndnping_rtts(text)
    return {
        "epochStartS": start,
        "epochEndS": finished,
        "rttMs": rtts[0] if rtts else None,
        "count": len(rtts),
        "rawTail": text[-500:],
    }


def diff_nfd_data_counters(before: dict[str, dict[str, int]],
                           after: dict[str, dict[str, int]]) -> dict[str, dict[str, int]]:
    counter_names = ["nInData", "nOutData", "nInBytes", "nOutBytes"]
    delta = {}
    for name in sorted(set(before) | set(after)):
        start = before.get(name, {})
        end = after.get(name, {})
        delta[name] = {
            counter: max(0, int(end.get(counter, 0)) - int(start.get(counter, 0)))
            for counter in counter_names
        }
    return delta


def parse_inference_result_rows(text: str) -> list[dict]:
    rows = []
    pattern = re.compile(r"YOLO_LAYOUT_RESULT[^\n]*ok=true[^\n]*|YOLO_LAYOUT_RESULT[^\n]*status=false[^\n]*")
    for match in pattern.finditer(text):
        line = match.group(0)
        elapsed = re.search(r"inference_elapsed_ms=([0-9.]+)", line)
        if not elapsed:
            continue
        start_match = re.search(r"epoch_start_s=([0-9.]+)", line)
        end_match = re.search(r"epoch_end_s=([0-9.]+)", line)
        index_match = re.search(r"\bindex=([0-9]+)", line)
        ok_match = re.search(r"\bok=(true|false)", line)
        status_match = re.search(r"\bstatus=(true|false)", line)
        rows.append({
            "index": int(index_match.group(1)) if index_match else len(rows),
            "epochStartS": float(start_match.group(1)) if start_match else None,
            "epochEndS": float(end_match.group(1)) if end_match else None,
            "inferenceElapsedMs": float(elapsed.group(1)),
            "ok": (ok_match.group(1) == "true") if ok_match else (status_match.group(1) == "true" if status_match else None),
            "line": line,
        })
    return rows


def nearest_monitor_sample(samples: list[dict], epoch_s: float | None) -> dict | None:
    if epoch_s is None or not samples:
        return None
    return min(samples, key=lambda sample: abs(float(sample.get("epochS", 0.0)) - epoch_s))


def start_warm_rtt_nfd_monitor(ndn,
                               providers: list[tuple[str, str, list[str]]],
                               interval_s: float):
    stop_event = threading.Event()
    samples: list[dict] = []
    lock = threading.Lock()
    provider_rows = []
    for node_name, provider_log_name, argv in providers:
        provider_id = argv[argv.index("--provider-id") + 1]
        provider_prefix = provider_identity(provider_id)
        provider_rows.append({
            "providerLog": provider_log_name,
            "providerNode": node_name,
            "providerPrefix": provider_prefix,
            "pingPrefix": f"{provider_prefix}/ndnping",
        })

    def loop() -> None:
        last_nfd = snapshot_nfd_data_counters(ndn)
        sample_index = 0
        while not stop_event.wait(interval_s):
            sample_start = time.time()
            rtts = []
            for provider in provider_rows:
                ping = ndnping_once_from_user(ndn, provider["pingPrefix"])
                rtts.append({**provider, **ping})
            current_nfd = snapshot_nfd_data_counters(ndn)
            nfd_delta = diff_nfd_data_counters(last_nfd, current_nfd)
            last_nfd = current_nfd
            rtt_values = [
                float(item["rttMs"]) for item in rtts
                if item.get("rttMs") is not None
            ]
            nfd_totals = {
                "nInData": sum(item.get("nInData", 0) for item in nfd_delta.values()),
                "nOutData": sum(item.get("nOutData", 0) for item in nfd_delta.values()),
                "nInBytes": sum(item.get("nInBytes", 0) for item in nfd_delta.values()),
                "nOutBytes": sum(item.get("nOutBytes", 0) for item in nfd_delta.values()),
            }
            with lock:
                samples.append({
                    "sample": sample_index,
                    "epochS": sample_start,
                    "epochEndS": time.time(),
                    "rttSummaryMs": summarize_numeric(rtt_values),
                    "rtts": rtts,
                    "nfdDelta": nfd_delta,
                    "nfdTotals": nfd_totals,
                })
            sample_index += 1

    thread = threading.Thread(target=loop, name="warm-rtt-nfd-monitor", daemon=True)
    thread.start()
    return stop_event, thread, samples, lock


def write_warm_rtt_nfd_monitor_summary(layout: str,
                                       phase: str,
                                       log_path: Path,
                                       samples: list[dict],
                                       lock: threading.Lock) -> None:
    with lock:
        copied_samples = list(samples)
    text = log_path.read_text(errors="replace") if log_path.exists() else ""
    requests = parse_inference_result_rows(text)
    aligned = []
    for request in requests:
        midpoint = None
        if request.get("epochStartS") is not None and request.get("epochEndS") is not None:
            midpoint = (float(request["epochStartS"]) + float(request["epochEndS"])) / 2.0
        sample = nearest_monitor_sample(copied_samples, midpoint)
        aligned.append({
            "request": request,
            "nearestSample": sample,
            "nearestSampleDeltaMs": (
                abs(float(sample.get("epochS", 0.0)) - midpoint) * 1000.0
                if sample is not None and midpoint is not None else None
            ),
        })
    summary = {
        "layout": layout,
        "phase": phase,
        "sampleCount": len(copied_samples),
        "requestCount": len(requests),
        "samples": copied_samples,
        "requestsWithNearestSample": aligned,
        "note": (
            "This diagnostic samples ndnping-style RTT and NFD network-face counters "
            "during the warm window. It adds probe traffic and should be used for "
            "correlation, not as the canonical low-overhead benchmark mode."
        ),
    }
    path = OUT / "warm-rtt-nfd-monitor.json"
    history = []
    if path.exists():
        try:
            history = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            history = []
    if not isinstance(history, list):
        history = []
    history.append(summary)
    path.write_text(json.dumps(history, indent=2, sort_keys=True), encoding="utf-8")
    rtt_values = [
        float(row["rttMs"])
        for sample in copied_samples
        for row in sample.get("rtts", [])
        if row.get("rttMs") is not None
    ]
    print(
        "YOLO_LAYOUT_WARM_RTT_NFD_MONITOR "
        f"layout={layout} phase={phase} samples={len(copied_samples)} "
        f"requests={len(requests)} rtt_p50_ms={percentile(rtt_values, 50):.2f} "
        f"rtt_p95_ms={percentile(rtt_values, 95):.2f} path={path}"
    )


def validate_repo_manifest_references(path: Path) -> None:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    roles = manifest.get("roles", {})
    if not isinstance(roles, dict) or not roles:
        raise RuntimeError(f"repo manifest has no roles: {path}")
    for role, artifacts in roles.items():
        if not isinstance(artifacts, dict):
            raise RuntimeError(f"repo manifest role {role} is not a mapping")
        for artifact_name in ("model", "runner"):
            entry = artifacts.get(artifact_name)
            if not isinstance(entry, dict):
                raise RuntimeError(f"repo manifest role {role} missing {artifact_name}")
            repo_manifest = entry.get("repoManifest")
            reference = entry.get("largeDataReference")
            if not isinstance(repo_manifest, dict):
                raise RuntimeError(f"repo manifest role {role} {artifact_name} missing repoManifest")
            if not isinstance(reference, dict):
                raise RuntimeError(f"repo manifest role {role} {artifact_name} missing largeDataReference")
            if reference.get("source") != "repo-manifest":
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} unexpected source={reference.get('source')}")
            if reference.get("dataName") != repo_manifest.get("objectName"):
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} dataName/objectName mismatch")
            expected_digest = "sha256:" + str(repo_manifest.get("sha256", ""))
            if reference.get("digest") != expected_digest:
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} digest mismatch")
            if int(reference.get("plaintextSize", -1)) != int(repo_manifest.get("size", -2)):
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} size mismatch")
    print(f"YOLO_2X2_REPO_MANIFEST_REFERENCES_OK path={path}")


def read_node_traffic(node) -> dict[str, int]:
    script = r"""
rx=0
tx=0
for d in /sys/class/net/*; do
  iface=$(basename "$d")
  [ "$iface" = "lo" ] && continue
  r=$(cat "$d/statistics/rx_bytes" 2>/dev/null || echo 0)
  t=$(cat "$d/statistics/tx_bytes" 2>/dev/null || echo 0)
  rx=$((rx + r))
  tx=$((tx + t))
done
echo "$rx $tx"
"""
    output = node.cmd("sh -c {}".format(perf.shell_quote(script))).strip()
    numbers = re.findall(r"\d+", output)
    if len(numbers) < 2:
        return {"rxBytes": 0, "txBytes": 0}
    return {"rxBytes": int(numbers[-2]), "txBytes": int(numbers[-1])}


def snapshot_traffic(ndn) -> dict[str, dict[str, int]]:
    return {
        node.name: read_node_traffic(node)
        for node in ndn.net.hosts
    }


def write_traffic_delta(layout: str, phase: str,
                        before: dict[str, dict[str, int]],
                        after: dict[str, dict[str, int]],
                        request_count: int = 1,
                        measured_request_count: int | None = None,
                        preflight_request_count: int = 0) -> dict:
    nodes = {}
    total_rx = 0
    total_tx = 0
    for name in sorted(set(before) | set(after)):
        start = before.get(name, {"rxBytes": 0, "txBytes": 0})
        end = after.get(name, {"rxBytes": 0, "txBytes": 0})
        rx = max(0, int(end.get("rxBytes", 0)) - int(start.get("rxBytes", 0)))
        tx = max(0, int(end.get("txBytes", 0)) - int(start.get("txBytes", 0)))
        nodes[name] = {"rxBytes": rx, "txBytes": tx, "totalBytes": rx + tx}
        total_rx += rx
        total_tx += tx
    observed_request_count = max(1, request_count)
    measured_count = max(1, measured_request_count if measured_request_count is not None else request_count)
    preflight_count = max(0, preflight_request_count)
    total_bytes = total_rx + total_tx
    summary = {
        "layout": layout,
        "phase": phase,
        "requestCount": observed_request_count,
        "observedRequestCount": observed_request_count,
        "measuredRequestCount": measured_count,
        "preflightRequestCount": preflight_count,
        "rxBytes": total_rx,
        "txBytes": total_tx,
        "totalNodeBytes": total_bytes,
        "totalNodeBytesPerRequest": total_bytes / observed_request_count,
        "totalNodeBytesPerObservedRequest": total_bytes / observed_request_count,
        "totalNodeBytesPerMeasuredInference": total_bytes / measured_count,
        "notes": [
            "Traffic deltas cover the whole phase window, including preflight requests "
            "and any first-use certificate/control fetches in that phase.",
            "Use totalNodeBytesPerObservedRequest for a phase-average transport view. "
            "Use totalNodeBytesPerMeasuredInference only when preflightRequestCount is 0 "
            "or when intentionally amortizing preflight/control traffic over measured requests.",
        ],
        "nodes": nodes,
    }
    path = OUT / "traffic-stats.json"
    history = []
    if path.exists():
        try:
            history = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            history = []
    if not isinstance(history, list):
        history = []
    history.append(summary)
    path.write_text(json.dumps(history, indent=2, sort_keys=True), encoding="utf-8")
    print(
        "YOLO_LAYOUT_TRAFFIC "
        f"layout={layout} phase={phase} "
        f"request_count={observed_request_count} "
        f"observed_request_count={observed_request_count} "
        f"measured_request_count={measured_count} "
        f"preflight_request_count={preflight_count} "
        f"rx_bytes={total_rx} tx_bytes={total_tx} "
        f"total_node_bytes={total_bytes} "
        f"total_node_bytes_per_inference={total_bytes / observed_request_count:.1f} "
        f"total_node_bytes_per_request={total_bytes / observed_request_count:.1f} "
        f"total_node_bytes_per_observed_request={total_bytes / observed_request_count:.1f} "
        f"total_node_bytes_per_measured_inference={total_bytes / measured_count:.1f} "
        f"path={path}"
    )
    return summary


def parse_nfd_face_list(output: str) -> list[dict]:
    faces = []
    current = None
    for line in output.splitlines():
        stripped = line.strip()
        match = re.match(r"faceid=(\d+)\s+remote=([^\s]+)\s+local=([^\s]+)", stripped)
        if match:
            current = {
                "faceid": match.group(1),
                "remote": match.group(2),
                "local": match.group(3),
                "counters": {},
            }
            faces.append(current)
        if current is None:
            continue
        counter_match = re.search(
            r"counters=\{in=\{(\d+)i\s+(\d+)d\s+(\d+)n\s+(\d+)B\}\s+"
            r"out=\{(\d+)i\s+(\d+)d\s+(\d+)n\s+(\d+)B\}\}",
            stripped)
        if counter_match:
            (
                in_interests, in_data, in_nacks, in_bytes,
                out_interests, out_data, out_nacks, out_bytes,
            ) = [int(value) for value in counter_match.groups()]
            current["counters"].update({
                "nInInterests": in_interests,
                "nInData": in_data,
                "nInNacks": in_nacks,
                "nInBytes": in_bytes,
                "nOutInterests": out_interests,
                "nOutData": out_data,
                "nOutNacks": out_nacks,
                "nOutBytes": out_bytes,
            })
            continue
        for key, value in re.findall(r"([a-zA-Z-]+)=([0-9]+)", stripped):
            current["counters"][key] = int(value)
    return faces


def is_network_face(face: dict) -> bool:
    remote = str(face.get("remote", ""))
    return not (
        remote.startswith("internal://") or
        remote.startswith("fd://") or
        remote.startswith("unix://")
    )


def read_node_nfd_data_counters(node) -> dict[str, int]:
    output = perf.node_cmd(node, "nfdc face list 2>&1")
    totals = {
        "nInData": 0,
        "nOutData": 0,
        "nInBytes": 0,
        "nOutBytes": 0,
    }
    for face in parse_nfd_face_list(output):
        if not is_network_face(face):
            continue
        counters = face.get("counters", {})
        for key in totals:
            totals[key] += int(counters.get(key, 0))
    return totals


def snapshot_nfd_data_counters(ndn) -> dict[str, dict[str, int]]:
    return {
        node.name: read_node_nfd_data_counters(node)
        for node in ndn.net.hosts
    }


def write_nfd_data_delta(layout: str, phase: str,
                         before: dict[str, dict[str, int]],
                         after: dict[str, dict[str, int]],
                         request_count: int = 1,
                         measured_request_count: int | None = None,
                         preflight_request_count: int = 0) -> dict:
    counter_names = ["nInData", "nOutData", "nInBytes", "nOutBytes"]
    nodes = {}
    totals = {name: 0 for name in counter_names}
    for name in sorted(set(before) | set(after)):
        start = before.get(name, {})
        end = after.get(name, {})
        delta = {
            counter: max(0, int(end.get(counter, 0)) - int(start.get(counter, 0)))
            for counter in counter_names
        }
        nodes[name] = delta
        for counter in counter_names:
            totals[counter] += delta[counter]
    request_count = max(1, request_count)
    measured_count = max(1, measured_request_count if measured_request_count is not None else request_count)
    preflight_count = max(0, preflight_request_count)
    out_data = totals["nOutData"]
    in_data = totals["nInData"]
    summary = {
        "layout": layout,
        "phase": phase,
        "requestCount": request_count,
        "observedRequestCount": request_count,
        "measuredRequestCount": measured_count,
        "preflightRequestCount": preflight_count,
        "nInData": in_data,
        "nOutData": out_data,
        "nInBytes": totals["nInBytes"],
        "nOutBytes": totals["nOutBytes"],
        "nOutDataPerRequest": out_data / request_count,
        "nOutDataPerObservedRequest": out_data / request_count,
        "nOutDataPerMeasuredInference": out_data / measured_count,
        "nOutBytesPerRequest": totals["nOutBytes"] / request_count,
        "nOutBytesPerObservedRequest": totals["nOutBytes"] / request_count,
        "nOutBytesPerMeasuredInference": totals["nOutBytes"] / measured_count,
        "avgNfdOutBytesPerOutData": (totals["nOutBytes"] / out_data) if out_data else 0.0,
        "notes": [
            "Data packet counts use NFD network-face nOutData/nInData counters.",
            "NFD nOutBytes/nInBytes are face byte counters for all packet types, "
            "so avgNfdOutBytesPerOutData is an approximate transport-size ratio, "
            "not a Data-only wire-size measurement.",
            "Phase deltas include preflight requests and first-use certificate/control "
            "fetches. In signing A/B/A runs, extra Data from the repo or certificate "
            "publisher node can reflect certificate/cache warmup rather than inference "
            "hot-path activation traffic.",
            "Compare nOutDataPerObservedRequest for phase-average transport cost. "
            "Use nOutDataPerMeasuredInference only when preflightRequestCount is 0 "
            "or when deliberately amortizing preflight/control traffic.",
        ],
        "nodes": nodes,
    }
    path = OUT / "nfd-data-stats.json"
    history = []
    if path.exists():
        try:
            history = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            history = []
    if not isinstance(history, list):
        history = []
    history.append(summary)
    path.write_text(json.dumps(history, indent=2, sort_keys=True), encoding="utf-8")
    print(
        "YOLO_LAYOUT_NFD_DATA "
        f"layout={layout} phase={phase} request_count={request_count} "
        f"observed_request_count={request_count} "
        f"measured_request_count={measured_count} "
        f"preflight_request_count={preflight_count} "
        f"n_out_data={out_data} n_in_data={in_data} "
        f"n_out_data_per_request={out_data / request_count:.2f} "
        f"data_packets_per_inference={out_data / request_count:.2f} "
        f"n_out_data_per_observed_request={out_data / request_count:.2f} "
        f"n_out_data_per_measured_inference={out_data / measured_count:.2f} "
        f"n_out_bytes={totals['nOutBytes']} "
        f"n_out_bytes_per_request={totals['nOutBytes'] / request_count:.1f} "
        f"n_out_bytes_per_observed_request={totals['nOutBytes'] / request_count:.1f} "
        f"n_out_bytes_per_measured_inference={totals['nOutBytes'] / measured_count:.1f} "
        f"avg_nfd_out_bytes_per_out_data={summary['avgNfdOutBytesPerOutData']:.1f} "
        f"avg_data_packet_bytes={summary['avgNfdOutBytesPerOutData']:.1f} "
        f"path={path}"
    )
    return summary


def parse_inference_latencies(text: str) -> list[float]:
    layout_matches = list(re.finditer(
        r"YOLO_LAYOUT_RESULT[^\n]*inference_elapsed_ms=([0-9.]+)[^\n]*ok=true",
        text))
    if layout_matches:
        return [float(match.group(1)) for match in layout_matches]
    return [
        float(match.group(1))
        for match in re.finditer(
            r"YOLO_2X2_RESULT[^\n]*inference_elapsed_ms=([0-9.]+)[^\n]*ok=true",
            text)
    ]


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * (p / 100.0)
    lo = int(rank)
    hi = min(lo + 1, len(ordered) - 1)
    weight = rank - lo
    return ordered[lo] * (1.0 - weight) + ordered[hi] * weight


def write_latency_summary(layout: str, phase: str, text: str) -> list[float]:
    latencies = parse_inference_latencies(text)
    steady_latencies = latencies[1:] if phase == "warm" and len(latencies) > 1 else []
    steady_under_1s = [value for value in steady_latencies if value < 1000.0]
    summary = {
        "layout": layout,
        "phase": phase,
        "count": len(latencies),
        "minMs": min(latencies) if latencies else 0.0,
        "p50Ms": percentile(latencies, 50),
        "p95Ms": percentile(latencies, 95),
        "maxMs": max(latencies) if latencies else 0.0,
        "meanMs": (sum(latencies) / len(latencies)) if latencies else 0.0,
        "samplesMs": latencies,
    }
    if steady_latencies:
        summary["steadyAfterFirst"] = {
            "discardedInitialSamples": 1,
            "count": len(steady_latencies),
            "minMs": min(steady_latencies),
            "p50Ms": percentile(steady_latencies, 50),
            "p95Ms": percentile(steady_latencies, 95),
            "maxMs": max(steady_latencies),
            "meanMs": sum(steady_latencies) / len(steady_latencies),
        }
        summary["steadyUnder1sAfterFirst"] = {
            "discardedInitialSamples": 1,
            "tailThresholdMs": 1000.0,
            "count": len(steady_under_1s),
            "minMs": min(steady_under_1s) if steady_under_1s else 0.0,
            "p50Ms": percentile(steady_under_1s, 50),
            "p95Ms": percentile(steady_under_1s, 95),
            "maxMs": max(steady_under_1s) if steady_under_1s else 0.0,
            "meanMs": (sum(steady_under_1s) / len(steady_under_1s)) if steady_under_1s else 0.0,
        }
    path = OUT / "inference-latency-stats.json"
    history = []
    if path.exists():
        try:
            history = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            history = []
    if not isinstance(history, list):
        history = []
    history.append(summary)
    path.write_text(json.dumps(history, indent=2, sort_keys=True), encoding="utf-8")
    print(
        "YOLO_LAYOUT_LATENCY "
        f"layout={layout} phase={phase} count={len(latencies)} "
        f"min_ms={summary['minMs']:.2f} p50_ms={summary['p50Ms']:.2f} "
        f"p95_ms={summary['p95Ms']:.2f} max_ms={summary['maxMs']:.2f} "
        f"mean_ms={summary['meanMs']:.2f} path={path}"
    )
    if steady_latencies:
        steady = summary["steadyAfterFirst"]
        under = summary["steadyUnder1sAfterFirst"]
        print(
            "YOLO_LAYOUT_STEADY_LATENCY "
            f"layout={layout} phase={phase} discard_first=1 "
            f"count={steady['count']} min_ms={steady['minMs']:.2f} "
            f"p50_ms={steady['p50Ms']:.2f} p95_ms={steady['p95Ms']:.2f} "
            f"max_ms={steady['maxMs']:.2f} mean_ms={steady['meanMs']:.2f}"
        )
        print(
            "YOLO_LAYOUT_STEADY_LATENCY_UNDER_1S "
            f"layout={layout} phase={phase} discard_first=1 "
            f"count={under['count']} min_ms={under['minMs']:.2f} "
            f"p50_ms={under['p50Ms']:.2f} p95_ms={under['p95Ms']:.2f} "
            f"max_ms={under['maxMs']:.2f} mean_ms={under['meanMs']:.2f}"
        )
    return latencies


def summarize_numeric(values: list[float]) -> dict:
    return {
        "count": len(values),
        "min": min(values) if values else 0.0,
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "max": max(values) if values else 0.0,
        "mean": (sum(values) / len(values)) if values else 0.0,
    }


def parse_key_value_line(line: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=([^ \n]+)", line))


def parse_trace_row(line: str) -> dict[str, str]:
    if "[NDNSF_TRACE]" not in line:
        return {}
    return parse_key_value_line(line)


def parse_timing_or_trace_row(line: str) -> dict[str, str]:
    if "[NDNSF_TRACE]" not in line and "NDNSF_CONTROL_TIMING" not in line:
        return {}
    return parse_key_value_line(line)


def parse_numeric_prefix(value, default: float = 0.0) -> float:
    match = re.match(r"[-+]?[0-9]+(?:\.[0-9]+)?", str(value or ""))
    return float(match.group(0)) if match else default


def parse_int_prefix(value, default: int = 0) -> int:
    return int(parse_numeric_prefix(value, float(default)))


def request_id_from_message_name(name: str) -> str:
    if not name:
        return ""
    for marker in (
            "/NDNSF/REQUEST/",
            "/NDNSF/ACK/",
            "/NDNSF/SELECTION/",
            "/NDNSF/RESPONSE/"):
        if marker in name:
            tail = name.rsplit("/", 1)[-1]
            return "/" + tail if tail else ""
    return ""


def selection_provider_from_message_name(name: str) -> str:
    marker = "/NDNSF/SELECTION/"
    if not name or marker not in name:
        return ""
    tail = name.split(marker, 1)[1]
    provider_component = tail.split("/", 1)[0]
    return urllib.parse.unquote(provider_component)


def has_planned_name(row: dict) -> bool:
    value = str(row.get("planned_name", "")).strip()
    return bool(value and value.lower() not in {"false", "0", "none", "-"})


def write_svs_internal_timing_summaries(
        layout: str,
        phases: list[tuple[str, Path]],
        providers: list[tuple[str, str, list[str]]]) -> None:
    log_paths = [(phase, "user", path) for phase, path in phases]
    log_paths.extend(("provider", name, OUT / f"{name}.log") for _, name, _ in providers)
    rows = []
    for phase, role, path in log_paths:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "event=" not in line:
                continue
            if ("ndn_svs.SyncTimeline" not in line and
                    "ndn_svs.SVSPubSub" not in line):
                continue
            row = parse_key_value_line(line)
            event = str(row.get("event", ""))
            if not event:
                continue
            row["phase"] = phase
            row["role"] = role
            row["log"] = path.name
            for key in ("elapsed_us", "elapsed_ms", "mappings", "data",
                        "retained", "expired", "bytes", "processed",
                        "received", "matches", "queue_depth",
                        "lock_wait_us", "mapping_select_us",
                        "initial_mapping_encode_us", "piggy_pack_us",
                        "final_encode_us", "total_us",
                        "top_level_mappings", "child_mappings",
                        "child_data", "received_mappings",
                        "top_level_parse_us", "block_parse_us",
                        "child_mapping_parse_us", "child_data_parse_us",
                        "child_cache_us", "process_mapping_us"):
                if key in row:
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
            rows.append(row)

    def values_for(event: str, key: str, *, mode: str = "") -> list[float]:
        values = []
        for row in rows:
            if row.get("event") != event:
                continue
            if mode and row.get("mode") != mode:
                continue
            if key not in row:
                continue
            value = float(row.get(key, 0))
            if value >= 0:
                values.append(value)
        return values

    def count_event(event: str) -> int:
        return sum(1 for row in rows if row.get("event") == event)

    piggy_build_rows = [row for row in rows if row.get("event") == "piggyback_build"]
    piggy_recv_rows = [row for row in rows if row.get("event") == "piggyback_recv_children"]
    extra_build_timing_rows = [
        row for row in rows if row.get("event") == "extra_mapping_build_timing"
    ]
    extra_recv_timing_rows = [
        row for row in rows if row.get("event") == "extra_mapping_recv_timing"
    ]
    mapping_fetch_rows = [
        row for row in rows
        if str(row.get("event", "")).startswith("mapping_fetch")
    ]
    parallel_sign = summarize_numeric(
        values_for("response_sign_done", "elapsed_us", mode="parallel"))
    parallel_worker_sign = summarize_numeric(
        values_for("response_sign_done", "elapsed_us", mode="parallel-worker-result"))
    summary = {
        "layout": layout,
        "count": len(rows),
        "events": {
            event: sum(1 for row in rows if row.get("event") == event)
            for event in sorted({str(row.get("event", "")) for row in rows})
        },
        "syncProcessing": {
            "workerProcessingMs": summarize_numeric(
                values_for("sync_worker_processing_ms", "elapsed_ms")),
            "parallelTotalMs": summarize_numeric(
                values_for("sync_interest_parallel_total_ms", "elapsed_ms")),
            "parallelMainLoopBlockedMs": summarize_numeric(
                values_for("main_loop_blocked_ms", "elapsed_ms", mode="parallel")),
            "serialMainLoopBlockedMs": summarize_numeric(
                values_for("main_loop_blocked_ms", "elapsed_ms", mode="serial")),
        },
        "syncProduction": {
            "parallelEncodeUs": summarize_numeric(
                values_for("response_encode_done", "elapsed_us", mode="parallel")),
            "parallelSignUs": parallel_sign,
            "parallelWorkerSignUs": parallel_worker_sign,
            "parallelFacePutUs": summarize_numeric(
                values_for("face_put_done", "elapsed_us", mode="parallel")),
            "serialEncodeUs": summarize_numeric(
                values_for("response_encode_done", "elapsed_us")),
            "serialSignUs": summarize_numeric(
                values_for("response_sign_done", "elapsed_us")),
            "serialFacePutUs": summarize_numeric(
                values_for("face_put_done", "elapsed_us")),
        },
        "extraMapping": {
            "buildCount": len(piggy_build_rows),
            "builtMappings": sum(float(row.get("mappings", 0)) for row in piggy_build_rows),
            "builtData": sum(float(row.get("data", 0)) for row in piggy_build_rows),
            "builtBytes": summarize_numeric([
                float(row.get("bytes", 0)) for row in piggy_build_rows
            ]),
            "recvMappings": sum(float(row.get("mappings", 0)) for row in piggy_recv_rows),
            "recvData": sum(float(row.get("data", 0)) for row in piggy_recv_rows),
            "recvCount": len(piggy_recv_rows),
            "recvBytes": summarize_numeric([
                float(row.get("bytes", 0)) for row in rows
                if row.get("event") == "piggyback_recv_block"
            ]),
            "cacheSatisfy": count_event("piggyback_cache_satisfy"),
            "directSatisfy": count_event("piggyback_satisfy"),
            "drops": count_event("piggyback_drop"),
            "skips": count_event("piggyback_skip"),
            "processMappings": count_event("piggyback_process_mappings"),
            "buildTiming": {
                "count": len(extra_build_timing_rows),
                "lockWaitUs": summarize_numeric([
                    float(row.get("lock_wait_us", 0)) for row in extra_build_timing_rows
                ]),
                "mappingSelectUs": summarize_numeric([
                    float(row.get("mapping_select_us", 0)) for row in extra_build_timing_rows
                ]),
                "initialMappingEncodeUs": summarize_numeric([
                    float(row.get("initial_mapping_encode_us", 0)) for row in extra_build_timing_rows
                ]),
                "piggyPackUs": summarize_numeric([
                    float(row.get("piggy_pack_us", 0)) for row in extra_build_timing_rows
                ]),
                "finalEncodeUs": summarize_numeric([
                    float(row.get("final_encode_us", 0)) for row in extra_build_timing_rows
                ]),
                "totalUs": summarize_numeric([
                    float(row.get("total_us", 0)) for row in extra_build_timing_rows
                ]),
            },
            "recvTiming": {
                "count": len(extra_recv_timing_rows),
                "topLevelParseUs": summarize_numeric([
                    float(row.get("top_level_parse_us", 0)) for row in extra_recv_timing_rows
                ]),
                "blockParseUs": summarize_numeric([
                    float(row.get("block_parse_us", 0)) for row in extra_recv_timing_rows
                ]),
                "childMappingParseUs": summarize_numeric([
                    float(row.get("child_mapping_parse_us", 0)) for row in extra_recv_timing_rows
                ]),
                "childDataParseUs": summarize_numeric([
                    float(row.get("child_data_parse_us", 0)) for row in extra_recv_timing_rows
                ]),
                "childCacheUs": summarize_numeric([
                    float(row.get("child_cache_us", 0)) for row in extra_recv_timing_rows
                ]),
                "processMappingUs": summarize_numeric([
                    float(row.get("process_mapping_us", 0)) for row in extra_recv_timing_rows
                ]),
                "totalUs": summarize_numeric([
                    float(row.get("total_us", 0)) for row in extra_recv_timing_rows
                ]),
            },
        },
        "mappingFetch": {
            "count": len(mapping_fetch_rows),
            "suppressedInFlight": sum(
                1 for row in mapping_fetch_rows if row.get("reason") == "in_flight"),
            "suppressedBackoff": sum(
                1 for row in mapping_fetch_rows if row.get("reason") == "backoff"),
        },
        "rows": rows,
    }
    path = OUT / "svs-internal-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_SVS_INTERNAL_TIMING "
        f"layout={layout} count={summary['count']} "
        f"sync_worker_p50_ms="
        f"{summary['syncProcessing']['workerProcessingMs']['p50']:.3f} "
        f"sync_parallel_total_p50_ms="
        f"{summary['syncProcessing']['parallelTotalMs']['p50']:.3f} "
        f"sync_main_blocked_p50_ms="
        f"{summary['syncProcessing']['parallelMainLoopBlockedMs']['p50']:.3f} "
        f"sync_encode_p50_us="
        f"{summary['syncProduction']['parallelEncodeUs']['p50']:.1f} "
        f"sync_sign_p50_us="
        f"{max(parallel_sign['p50'], parallel_worker_sign['p50']):.1f} "
        f"sync_face_put_p50_us="
        f"{summary['syncProduction']['parallelFacePutUs']['p50']:.1f} "
        f"extra_mapping_build_count={summary['extraMapping']['buildCount']} "
        f"extra_mapping_built_mappings={summary['extraMapping']['builtMappings']:.0f} "
        f"extra_mapping_built_data={summary['extraMapping']['builtData']:.0f} "
        f"extra_mapping_build_bytes_p50="
        f"{summary['extraMapping']['builtBytes']['p50']:.0f} "
        f"extra_mapping_build_total_p50_us="
        f"{summary['extraMapping']['buildTiming']['totalUs']['p50']:.1f} "
        f"extra_mapping_parse_total_p50_us="
        f"{summary['extraMapping']['recvTiming']['totalUs']['p50']:.1f} "
        f"extra_mapping_parse_process_p50_us="
        f"{summary['extraMapping']['recvTiming']['processMappingUs']['p50']:.1f} "
        f"mapping_fetch_count={summary['mappingFetch']['count']} "
        f"path={path}"
    )


def write_provider_timing_summaries(layout: str,
                                    providers: list[tuple[str, str, list[str]]],
                                    *,
                                    require_runtime_timing: bool = True) -> bool:
    onnx_rows = []
    dependency_rows = []
    output_rows = []
    collab_fetch_rows = []
    pending_ims_rows = []
    handler_rows = []
    for _, name, _ in providers:
        path = OUT / f"{name}.log"
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "NDNSF_DI_ONNX_TIMING" in line:
                row = parse_key_value_line(line)
                for key in ("collect_ms", "session_ms", "run_ms", "publish_ms"):
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
                row["providerLog"] = path.name
                onnx_rows.append(row)
            elif "NDNSF_DI_DEPENDENCY_INPUT_TIMING" in line:
                row = parse_key_value_line(line)
                for key in (
                    "future_wait_ms",
                    "ref_wait_ms",
                    "fetch_ms",
                    "decode_ms",
                    "prefetch_total_ms",
                    "prefetch_overlap_ms",
                ):
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
                row["bytes"] = parse_int_prefix(row.get("bytes", "0"))
                row["expected_segments"] = parse_int_prefix(row.get("expected_segments", "0"))
                row["expected_bytes"] = parse_int_prefix(row.get("expected_bytes", "0"))
                row["planned_name"] = str(row.get("planned_name", "false"))
                row["providerLog"] = path.name
                dependency_rows.append(row)
            elif "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING" in line:
                row = parse_key_value_line(line)
                row["publish_ms"] = parse_numeric_prefix(row.get("publish_ms", "0"))
                row["bytes"] = parse_int_prefix(row.get("bytes", "0"))
                row["expected_segments"] = parse_int_prefix(row.get("expected_segments", "0"))
                row["expected_bytes"] = parse_int_prefix(row.get("expected_bytes", "0"))
                row["output_ready_epoch_ms"] = parse_int_prefix(row.get("output_ready_epoch_ms", "0"))
                row["publish_done_epoch_ms"] = parse_int_prefix(row.get("publish_done_epoch_ms", "0"))
                row["planned_name"] = str(row.get("planned_name", "false"))
                row["providerLog"] = path.name
                output_rows.append(row)
            elif "NDNSF_COLLAB_LARGE_FETCH_TIMING" in line:
                row = parse_key_value_line(line)
                for key in (
                    "elapsed_ms",
                    "first_segment_ms",
                    "last_segment_received_ms",
                    "last_segment_validated_ms",
                    "fetch_start_to_interest_ms",
                    "fetch_start_to_data_ms",
                    "interest_to_data_ms",
                    "fetch_start_to_validated_ms",
                    "interest_to_validated_ms",
                    "data_to_validated_ms",
                    "init_cwnd",
                ):
                    if key in row:
                        row[key] = parse_numeric_prefix(row.get(key, "0"))
                for key in (
                    "timestamp_us",
                    "encoded_bytes",
                    "error_code",
                    "timeout_ms",
                    "interest_lifetime_ms",
                    "received_segments",
                    "validated_segments",
                    "received_wire_bytes",
                    "nacks",
                    "segment_timeouts",
                    "first_segment_epoch_ms",
                    "complete_epoch_ms",
                    "segment",
                    "wire_bytes",
                ):
                    if key in row:
                        row[key] = parse_int_prefix(row.get(key, "0"))
                row["providerLog"] = path.name
                collab_fetch_rows.append(row)
            elif "NDNSF_PENDING_IMS_TIMING" in line:
                row = parse_key_value_line(line)
                for key in ("pending_age_ms",):
                    if key in row:
                        row[key] = parse_numeric_prefix(row.get(key, "0"))
                for key in ("lifetime_ms", "pending", "remaining_before"):
                    if key in row:
                        row[key] = parse_int_prefix(row.get(key, "0"))
                row["providerLog"] = path.name
                pending_ims_rows.append(row)
            elif "NDNSF_DI_PROVIDER_HANDLER_TIMING" in line:
                row = parse_key_value_line(line)
                for key in (
                    "queue_wait_ms",
                    "worker_queue_wait_ms",
                    "input_fetch_wait_ms",
                    "runner_publish_ms",
                    "total_ms",
                    "handler_ms",
                ):
                    if key in row:
                        row[key] = parse_numeric_prefix(row.get(key, "0"))
                for key in (
                    "submitted_epoch_ms",
                    "worker_start_epoch_ms",
                    "start_epoch_ms",
                    "end_epoch_ms",
                ):
                    if key in row:
                        row[key] = parse_int_prefix(row.get(key, "0"))
                row["providerLog"] = path.name
                handler_rows.append(row)

    onnx_summary = {
        "layout": layout,
        "count": len(onnx_rows),
        "sessionCache": {
            "hit": sum(1 for row in onnx_rows if row.get("session_cache") == "hit"),
            "miss": sum(1 for row in onnx_rows if row.get("session_cache") == "miss"),
        },
        "collectMs": summarize_numeric([row["collect_ms"] for row in onnx_rows]),
        "sessionMs": summarize_numeric([row["session_ms"] for row in onnx_rows]),
        "runMs": summarize_numeric([row["run_ms"] for row in onnx_rows]),
        "publishMs": summarize_numeric([row["publish_ms"] for row in onnx_rows]),
        "rows": onnx_rows,
    }
    onnx_path = OUT / "onnx-timing-stats.json"
    onnx_path.write_text(json.dumps(onnx_summary, indent=2, sort_keys=True),
                         encoding="utf-8")
    print(
        "YOLO_LAYOUT_ONNX_TIMING "
        f"layout={layout} count={onnx_summary['count']} "
        f"session_hit={onnx_summary['sessionCache']['hit']} "
        f"session_miss={onnx_summary['sessionCache']['miss']} "
        f"run_p50_ms={onnx_summary['runMs']['p50']:.2f} "
        f"session_p50_ms={onnx_summary['sessionMs']['p50']:.2f} "
        f"path={onnx_path}"
    )
    onnx_by_role = {}
    for row in onnx_rows:
        onnx_by_role.setdefault(row.get("role", "-"), []).append(row)
    for role, rows in sorted(onnx_by_role.items()):
        collect = summarize_numeric([row["collect_ms"] for row in rows])
        session = summarize_numeric([row["session_ms"] for row in rows])
        run = summarize_numeric([row["run_ms"] for row in rows])
        publish = summarize_numeric([row["publish_ms"] for row in rows])
        print(
            "YOLO_LAYOUT_ONNX_ROLE_TIMING "
            f"layout={layout} role={role} count={len(rows)} "
            f"collect_p50_ms={collect['p50']:.2f} "
            f"session_p50_ms={session['p50']:.2f} "
            f"run_p50_ms={run['p50']:.2f} "
            f"publish_p50_ms={publish['p50']:.2f}"
        )
    onnx_by_session = {}
    for row in onnx_rows:
        onnx_by_session.setdefault(row.get("session", "-"), []).append(row)
    for session, rows in sorted(onnx_by_session.items()):
        collect = summarize_numeric([row["collect_ms"] for row in rows])
        session_ms = summarize_numeric([row["session_ms"] for row in rows])
        run = summarize_numeric([row["run_ms"] for row in rows])
        publish = summarize_numeric([row["publish_ms"] for row in rows])
        print(
            "YOLO_LAYOUT_ONNX_SESSION_TIMING "
            f"layout={layout} session={session} count={len(rows)} "
            f"collect_sum_ms={sum(row['collect_ms'] for row in rows):.2f} "
            f"session_sum_ms={sum(row['session_ms'] for row in rows):.2f} "
            f"run_sum_ms={sum(row['run_ms'] for row in rows):.2f} "
            f"publish_sum_ms={sum(row['publish_ms'] for row in rows):.2f} "
            f"collect_p50_ms={collect['p50']:.2f} "
            f"session_p50_ms={session_ms['p50']:.2f} "
            f"run_p50_ms={run['p50']:.2f} "
            f"publish_p50_ms={publish['p50']:.2f}"
        )

    dep_summary = {
        "layout": layout,
        "count": len(dependency_rows),
        "totalBytes": sum(row["bytes"] for row in dependency_rows),
        "totalExpectedSegments": sum(row["expected_segments"] for row in dependency_rows),
        "totalExpectedBytes": sum(row["expected_bytes"] for row in dependency_rows),
        "bytes": summarize_numeric([float(row["bytes"]) for row in dependency_rows]),
        "expectedSegments": summarize_numeric([
            float(row["expected_segments"]) for row in dependency_rows
        ]),
        "expectedBytes": summarize_numeric([
            float(row["expected_bytes"]) for row in dependency_rows
        ]),
        "futureWaitMs": summarize_numeric([row["future_wait_ms"] for row in dependency_rows]),
        "referenceWaitMs": summarize_numeric([row["ref_wait_ms"] for row in dependency_rows]),
        "fetchMs": summarize_numeric([row["fetch_ms"] for row in dependency_rows]),
        "decodeMs": summarize_numeric([row["decode_ms"] for row in dependency_rows]),
        "prefetchTotalMs": summarize_numeric([row["prefetch_total_ms"] for row in dependency_rows]),
        "prefetchOverlapMs": summarize_numeric([
            row["prefetch_overlap_ms"] for row in dependency_rows
        ]),
        "plannedNameFetches": sum(1 for row in dependency_rows if has_planned_name(row)),
        "rows": dependency_rows,
    }
    dep_path = OUT / "dependency-input-timing-stats.json"
    dep_path.write_text(json.dumps(dep_summary, indent=2, sort_keys=True),
                        encoding="utf-8")
    print(
        "YOLO_LAYOUT_DEPENDENCY_TIMING "
        f"layout={layout} count={dep_summary['count']} "
        f"ref_wait_p50_ms={dep_summary['referenceWaitMs']['p50']:.2f} "
        f"fetch_p50_ms={dep_summary['fetchMs']['p50']:.2f} "
        f"decode_p50_ms={dep_summary['decodeMs']['p50']:.2f} "
        f"prefetch_overlap_p50_ms={dep_summary['prefetchOverlapMs']['p50']:.2f} "
        f"planned_name_fetches={dep_summary['plannedNameFetches']} "
        f"total_bytes={dep_summary['totalBytes']} "
        f"total_expected_bytes={dep_summary['totalExpectedBytes']} "
        f"path={dep_path}"
    )
    dependency_by_consumer = {}
    for row in dependency_rows:
        consumer = row.get("providerLog", "-")
        producer = row.get("producer", "-")
        dependency_by_consumer.setdefault((consumer, producer), []).append(row)
    edge_fetch_p50_sum = 0.0
    edge_decode_p50_sum = 0.0
    edge_count = 0
    for (consumer, producer), rows in sorted(dependency_by_consumer.items()):
        fetch = summarize_numeric([row["fetch_ms"] for row in rows])
        prefetch = summarize_numeric([row["prefetch_total_ms"] for row in rows])
        overlap = summarize_numeric([row["prefetch_overlap_ms"] for row in rows])
        decode = summarize_numeric([row["decode_ms"] for row in rows])
        bytes_summary = summarize_numeric([float(row["bytes"]) for row in rows])
        edge_fetch_p50_sum += fetch["p50"]
        edge_decode_p50_sum += decode["p50"]
        edge_count += 1
        print(
            "YOLO_LAYOUT_DEPENDENCY_EDGE_TIMING "
            f"layout={layout} consumer_log={consumer} producer={producer} "
            f"count={len(rows)} bytes_p50={bytes_summary['p50']:.0f} "
            f"fetch_p50_ms={fetch['p50']:.2f} fetch_p95_ms={fetch['p95']:.2f} "
            f"prefetch_p50_ms={prefetch['p50']:.2f} "
            f"prefetch_overlap_p50_ms={overlap['p50']:.2f} "
            f"planned_name_fetches={sum(1 for row in rows if has_planned_name(row))} "
            f"decode_p50_ms={decode['p50']:.2f}"
        )
    dependency_by_scope = {}
    for row in dependency_rows:
        dependency_by_scope.setdefault(row.get("scope", "-"), []).append(row)
    for scope, rows in sorted(dependency_by_scope.items()):
        fetch = summarize_numeric([row["fetch_ms"] for row in rows])
        overlap = summarize_numeric([row["prefetch_overlap_ms"] for row in rows])
        decode = summarize_numeric([row["decode_ms"] for row in rows])
        print(
            "YOLO_LAYOUT_DEPENDENCY_STAGE_TIMING "
            f"layout={layout} scope={scope} count={len(rows)} "
            f"fetch_p50_ms={fetch['p50']:.2f} fetch_p95_ms={fetch['p95']:.2f} "
            f"prefetch_overlap_p50_ms={overlap['p50']:.2f} "
            f"decode_p50_ms={decode['p50']:.2f}"
        )
    dependency_by_session = {}
    for row in dependency_rows:
        dependency_by_session.setdefault(row.get("session", "-"), []).append(row)
    for session, rows in sorted(dependency_by_session.items()):
        fetch_sum = sum(row["fetch_ms"] for row in rows)
        decode_sum = sum(row["decode_ms"] for row in rows)
        overlap_sum = sum(row["prefetch_overlap_ms"] for row in rows)
        bytes_sum = sum(row["bytes"] for row in rows)
        print(
            "YOLO_LAYOUT_DEPENDENCY_SESSION_TIMING "
            f"layout={layout} session={session} count={len(rows)} "
            f"bytes={bytes_sum} "
            f"fetch_sum_ms={fetch_sum:.2f} "
            f"prefetch_overlap_sum_ms={overlap_sum:.2f} "
            f"planned_name_fetches={sum(1 for row in rows if has_planned_name(row))} "
            f"decode_sum_ms={decode_sum:.2f}"
        )

    output_summary = {
        "layout": layout,
        "count": len(output_rows),
        "totalBytes": sum(row["bytes"] for row in output_rows),
        "totalExpectedSegments": sum(row["expected_segments"] for row in output_rows),
        "totalExpectedBytes": sum(row["expected_bytes"] for row in output_rows),
        "bytes": summarize_numeric([float(row["bytes"]) for row in output_rows]),
        "expectedSegments": summarize_numeric([
            float(row["expected_segments"]) for row in output_rows
        ]),
        "expectedBytes": summarize_numeric([
            float(row["expected_bytes"]) for row in output_rows
        ]),
        "publishMs": summarize_numeric([row["publish_ms"] for row in output_rows]),
        "rows": output_rows,
    }
    output_path = OUT / "dependency-output-timing-stats.json"
    output_path.write_text(json.dumps(output_summary, indent=2, sort_keys=True),
                           encoding="utf-8")
    print(
        "YOLO_LAYOUT_DEPENDENCY_OUTPUT_TIMING "
        f"layout={layout} count={output_summary['count']} "
        f"publish_p50_ms={output_summary['publishMs']['p50']:.2f} "
        f"total_bytes={output_summary['totalBytes']} "
        f"total_expected_bytes={output_summary['totalExpectedBytes']} "
        f"path={output_path}"
    )
    output_by_scope = {}
    for row in output_rows:
        output_by_scope.setdefault((row.get("role", "-"), row.get("scope", "-")), []).append(row)
    for (role, scope), rows in sorted(output_by_scope.items()):
        publish = summarize_numeric([row["publish_ms"] for row in rows])
        bytes_summary = summarize_numeric([float(row["bytes"]) for row in rows])
        print(
            "YOLO_LAYOUT_DEPENDENCY_OUTPUT_EDGE_TIMING "
            f"layout={layout} role={role} scope={scope} "
            f"count={len(rows)} bytes_p50={bytes_summary['p50']:.0f} "
            f"publish_p50_ms={publish['p50']:.2f} "
            f"publish_p95_ms={publish['p95']:.2f}"
        )
    output_by_session = {}
    for row in output_rows:
        output_by_session.setdefault(row.get("session", "-"), []).append(row)
    for session, rows in sorted(output_by_session.items()):
        print(
            "YOLO_LAYOUT_DEPENDENCY_OUTPUT_SESSION_TIMING "
            f"layout={layout} session={session} count={len(rows)} "
            f"bytes={sum(row['bytes'] for row in rows)} "
            f"publish_sum_ms={sum(row['publish_ms'] for row in rows):.2f}"
        )
    input_volume_by_scope = {
        scope: {
            "count": len(rows),
            "actualBytes": sum(row["bytes"] for row in rows),
            "expectedBytes": sum(row["expected_bytes"] for row in rows),
            "expectedSegments": sum(row["expected_segments"] for row in rows),
            "plannedNameFetches": sum(1 for row in rows if has_planned_name(row)),
        }
        for scope, rows in sorted(dependency_by_scope.items())
    }
    output_volume_by_scope = {}
    for row in output_rows:
        scope = row.get("scope", "-")
        item = output_volume_by_scope.setdefault(scope, {
            "count": 0,
            "actualBytes": 0,
            "expectedBytes": 0,
            "expectedSegments": 0,
            "plannedNamePublishes": 0,
        })
        item["count"] += 1
        item["actualBytes"] += row["bytes"]
        item["expectedBytes"] += row["expected_bytes"]
        item["expectedSegments"] += row["expected_segments"]
        if has_planned_name(row):
            item["plannedNamePublishes"] += 1
    input_volume_by_session = {
        session: {
            "count": len(rows),
            "actualBytes": sum(row["bytes"] for row in rows),
            "expectedBytes": sum(row["expected_bytes"] for row in rows),
            "expectedSegments": sum(row["expected_segments"] for row in rows),
            "plannedNameFetches": sum(1 for row in rows if has_planned_name(row)),
        }
        for session, rows in sorted(dependency_by_session.items())
    }
    output_volume_by_session = {}
    for session, rows in sorted(output_by_session.items()):
        output_volume_by_session[session] = {
            "count": len(rows),
            "actualBytes": sum(row["bytes"] for row in rows),
            "expectedBytes": sum(row["expected_bytes"] for row in rows),
            "expectedSegments": sum(row["expected_segments"] for row in rows),
            "plannedNamePublishes": sum(1 for row in rows if has_planned_name(row)),
        }
    volume_summary = {
        "layout": layout,
        "inputs": {
            "count": len(dependency_rows),
            "actualBytes": dep_summary["totalBytes"],
            "expectedBytes": dep_summary["totalExpectedBytes"],
            "expectedSegments": dep_summary["totalExpectedSegments"],
            "byScope": input_volume_by_scope,
            "bySession": input_volume_by_session,
        },
        "outputs": {
            "count": len(output_rows),
            "actualBytes": output_summary["totalBytes"],
            "expectedBytes": output_summary["totalExpectedBytes"],
            "expectedSegments": output_summary["totalExpectedSegments"],
            "byScope": output_volume_by_scope,
            "bySession": output_volume_by_session,
        },
    }
    volume_path = OUT / "dependency-volume-stats.json"
    volume_path.write_text(
        json.dumps(volume_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_DEPENDENCY_VOLUME "
        f"layout={layout} "
        f"input_count={volume_summary['inputs']['count']} "
        f"input_actual_bytes={volume_summary['inputs']['actualBytes']} "
        f"input_expected_bytes={volume_summary['inputs']['expectedBytes']} "
        f"input_expected_segments={volume_summary['inputs']['expectedSegments']} "
        f"output_count={volume_summary['outputs']['count']} "
        f"output_actual_bytes={volume_summary['outputs']['actualBytes']} "
        f"output_expected_bytes={volume_summary['outputs']['expectedBytes']} "
        f"output_expected_segments={volume_summary['outputs']['expectedSegments']} "
        f"path={volume_path}"
    )
    unplanned_output_rows = [
        row for row in output_rows if not has_planned_name(row)
    ]
    unplanned_input_rows = [
        row for row in dependency_rows if not has_planned_name(row)
    ]
    mismatched_planned_output_rows = [
        row for row in output_rows
        if has_planned_name(row) and row.get("data_name") != row.get("planned_name")
    ]
    ref_wait_input_rows = [
        row for row in dependency_rows
        if row.get("ref_wait_ms", 0.0) > 0.0
    ]
    has_runtime_timing = bool(output_rows or dependency_rows)
    validation_skipped = not require_runtime_timing and not has_runtime_timing
    planned_transport_ok = validation_skipped or (
        len(output_rows) > 0 and
        len(dependency_rows) > 0 and
        not unplanned_output_rows and
        not unplanned_input_rows and
        not mismatched_planned_output_rows and
        not ref_wait_input_rows
    )
    transport_summary = {
        "layout": layout,
        "ok": planned_transport_ok,
        "validationSkipped": validation_skipped,
        "runtimeTimingObserved": has_runtime_timing,
        "outputCount": len(output_rows),
        "plannedOutputCount": len(output_rows) - len(unplanned_output_rows),
        "unplannedOutputCount": len(unplanned_output_rows),
        "inputCount": len(dependency_rows),
        "plannedInputCount": len(dependency_rows) - len(unplanned_input_rows),
        "unplannedInputCount": len(unplanned_input_rows),
        "mismatchedPlannedOutputCount": len(mismatched_planned_output_rows),
        "refWaitInputCount": len(ref_wait_input_rows),
        "refWaitMs": summarize_numeric([
            float(row.get("ref_wait_ms", 0.0)) for row in dependency_rows
        ]),
        "pendingImsRememberCount": sum(
            1 for row in pending_ims_rows if row.get("event") == "remember"),
        "pendingImsSatisfyCount": sum(
            1 for row in pending_ims_rows if row.get("event") == "satisfy"),
        "unplannedOutputs": unplanned_output_rows,
        "unplannedInputs": unplanned_input_rows,
        "mismatchedPlannedOutputs": mismatched_planned_output_rows,
        "refWaitInputs": ref_wait_input_rows,
    }
    transport_path = OUT / "native-dataflow-transport-stats.json"
    transport_path.write_text(
        json.dumps(transport_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_NATIVE_DATAFLOW_TRANSPORT "
        f"layout={layout} ok={str(planned_transport_ok).lower()} "
        f"validation={'skipped' if validation_skipped else 'checked'} "
        f"outputs={transport_summary['outputCount']} "
        f"planned_outputs={transport_summary['plannedOutputCount']} "
        f"unplanned_outputs={transport_summary['unplannedOutputCount']} "
        f"inputs={transport_summary['inputCount']} "
        f"planned_inputs={transport_summary['plannedInputCount']} "
        f"unplanned_inputs={transport_summary['unplannedInputCount']} "
        f"ref_wait_inputs={transport_summary['refWaitInputCount']} "
        f"ref_wait_p50_ms={transport_summary['refWaitMs']['p50']:.2f} "
        f"pending_ims_remember={transport_summary['pendingImsRememberCount']} "
        f"pending_ims_satisfy={transport_summary['pendingImsSatisfyCount']} "
        f"path={transport_path}"
    )
    role_run_p50_sum = 0.0
    role_publish_p50_sum = 0.0
    role_session_p50_sum = 0.0
    for rows in onnx_by_role.values():
        role_run_p50_sum += summarize_numeric([row["run_ms"] for row in rows])["p50"]
        role_publish_p50_sum += summarize_numeric([row["publish_ms"] for row in rows])["p50"]
        role_session_p50_sum += summarize_numeric([row["session_ms"] for row in rows])["p50"]
    print(
        "YOLO_LAYOUT_PIPELINE_ESTIMATE "
        f"layout={layout} dependency_edges={edge_count} "
        f"dependency_fetch_p50_sum_ms={edge_fetch_p50_sum:.2f} "
        f"dependency_decode_p50_sum_ms={edge_decode_p50_sum:.2f} "
        f"onnx_session_p50_sum_ms={role_session_p50_sum:.2f} "
        f"onnx_run_p50_sum_ms={role_run_p50_sum:.2f} "
        f"activation_publish_p50_sum_ms={role_publish_p50_sum:.2f}"
    )

    complete_fetch_rows = [
        row for row in collab_fetch_rows
        if row.get("event") == "complete" and "elapsed_ms" in row
    ]
    error_fetch_rows = [
        row for row in collab_fetch_rows
        if row.get("event") == "error"
    ]
    segment_interest_rows = [
        row for row in collab_fetch_rows
        if row.get("event") == "segment_interest"
    ]
    segment_received_rows = [
        row for row in collab_fetch_rows
        if row.get("event") == "segment_received"
    ]
    segment_validated_rows = [
        row for row in collab_fetch_rows
        if row.get("event") == "segment_validated"
    ]
    collab_fetch_summary = {
        "layout": layout,
        "count": len(collab_fetch_rows),
        "complete": len(complete_fetch_rows),
        "errors": len(error_fetch_rows),
        "segmentInterests": len(segment_interest_rows),
        "segmentReceived": len(segment_received_rows),
        "segmentValidated": len(segment_validated_rows),
        "elapsedMs": summarize_numeric([
            row["elapsed_ms"] for row in complete_fetch_rows
        ]),
        "encodedBytes": summarize_numeric([
            float(row.get("encoded_bytes", 0)) for row in complete_fetch_rows
        ]),
        "firstSegmentMs": summarize_numeric([
            row.get("first_segment_ms", 0.0) for row in complete_fetch_rows
        ]),
        "lastSegmentReceivedMs": summarize_numeric([
            row.get("last_segment_received_ms", 0.0) for row in complete_fetch_rows
        ]),
        "lastSegmentValidatedMs": summarize_numeric([
            row.get("last_segment_validated_ms", 0.0) for row in complete_fetch_rows
        ]),
        "receivedSegments": summarize_numeric([
            float(row.get("received_segments", 0)) for row in complete_fetch_rows
        ]),
        "validatedSegments": summarize_numeric([
            float(row.get("validated_segments", 0)) for row in complete_fetch_rows
        ]),
        "receivedWireBytes": summarize_numeric([
            float(row.get("received_wire_bytes", 0)) for row in complete_fetch_rows
        ]),
        "segmentFetchStartToInterestMs": summarize_numeric([
            row.get("fetch_start_to_interest_ms", 0.0) for row in segment_interest_rows
        ]),
        "segmentFetchStartToDataMs": summarize_numeric([
            row.get("fetch_start_to_data_ms", 0.0) for row in segment_received_rows
        ]),
        "segmentInterestToDataMs": summarize_numeric([
            row.get("interest_to_data_ms", 0.0) for row in segment_received_rows
        ]),
        "segmentFetchStartToValidatedMs": summarize_numeric([
            row.get("fetch_start_to_validated_ms", 0.0) for row in segment_validated_rows
        ]),
        "segmentInterestToValidatedMs": summarize_numeric([
            row.get("interest_to_validated_ms", 0.0) for row in segment_validated_rows
        ]),
        "segmentDataToValidatedMs": summarize_numeric([
            row.get("data_to_validated_ms", 0.0) for row in segment_validated_rows
        ]),
        "segmentWireBytes": summarize_numeric([
            float(row.get("wire_bytes", 0)) for row in segment_received_rows
        ]),
        "nacks": sum(row.get("nacks", 0) for row in collab_fetch_rows),
        "segmentTimeouts": sum(row.get("segment_timeouts", 0) for row in collab_fetch_rows),
        "interestLifetimeMs": summarize_numeric([
            float(row.get("interest_lifetime_ms", 0)) for row in collab_fetch_rows
            if "interest_lifetime_ms" in row
        ]),
        "initCwnd": summarize_numeric([
            float(row.get("init_cwnd", 0)) for row in collab_fetch_rows
            if "init_cwnd" in row
        ]),
        "rows": collab_fetch_rows,
    }
    collab_fetch_path = OUT / "collab-large-fetch-stats.json"
    collab_fetch_path.write_text(
        json.dumps(collab_fetch_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_COLLAB_LARGE_FETCH_TIMING "
        f"layout={layout} count={collab_fetch_summary['count']} "
        f"complete={collab_fetch_summary['complete']} "
        f"errors={collab_fetch_summary['errors']} "
        f"elapsed_p50_ms={collab_fetch_summary['elapsedMs']['p50']:.2f} "
        f"elapsed_p95_ms={collab_fetch_summary['elapsedMs']['p95']:.2f} "
        f"first_segment_p50_ms={collab_fetch_summary['firstSegmentMs']['p50']:.2f} "
        f"last_validated_p50_ms={collab_fetch_summary['lastSegmentValidatedMs']['p50']:.2f} "
        f"segments_received={collab_fetch_summary['segmentReceived']} "
        f"seg_interest_to_data_p50_ms="
        f"{collab_fetch_summary['segmentInterestToDataMs']['p50']:.2f} "
        f"seg_data_to_validated_p50_ms="
        f"{collab_fetch_summary['segmentDataToValidatedMs']['p50']:.2f} "
        f"segment_timeouts={collab_fetch_summary['segmentTimeouts']} "
        f"encoded_bytes_p50={collab_fetch_summary['encodedBytes']['p50']:.0f} "
        f"interest_lifetime_p50_ms={collab_fetch_summary['interestLifetimeMs']['p50']:.0f} "
        f"init_cwnd_p50={collab_fetch_summary['initCwnd']['p50']:.0f} "
        f"path={collab_fetch_path}"
    )

    segment_events: dict[tuple[str, str, str], dict] = {}
    for row in collab_fetch_rows:
        event = row.get("event", "")
        segment_name = row.get("segmentName", "")
        request_id = row.get("requestId", "")
        key_scope = row.get("keyScope", "")
        if not segment_name or not request_id or event not in {
                "segment_active_put",
                "segment_interest",
                "segment_received",
                "segment_validated",
        }:
            continue
        key = (request_id, key_scope, segment_name)
        item = segment_events.setdefault(key, {
            "requestId": request_id,
            "keyScope": key_scope,
            "segmentName": segment_name,
            "dataName": row.get("dataName", ""),
            "producerLog": "",
            "consumerLog": "",
            "activePutTimestampUs": 0,
            "interestTimestampUs": 0,
            "dataTimestampUs": 0,
            "validatedTimestampUs": 0,
            "interestFetchStartDeltaMs": 0.0,
            "dataFetchStartDeltaMs": 0.0,
            "validatedFetchStartDeltaMs": 0.0,
            "interestToDataMs": 0.0,
            "interestToValidatedMs": 0.0,
            "dataToValidatedMs": 0.0,
            "wireBytes": 0,
        })
        provider_log = row.get("providerLog", "")
        if event == "segment_active_put":
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            current = int(item.get("activePutTimestampUs", 0) or 0)
            if timestamp_us > 0:
                item["activePutTimestampUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            item["producerLog"] = provider_log
            item["wireBytes"] = parse_int_prefix(row.get("wire_bytes", "0"))
        elif event == "segment_interest":
            item["consumerLog"] = provider_log
            item["interestTimestampUs"] = parse_int_prefix(row.get("timestamp_us", "0"))
            item["interestFetchStartDeltaMs"] = parse_numeric_prefix(
                row.get("fetch_start_to_interest_ms", "0"))
        elif event == "segment_received":
            item["consumerLog"] = provider_log
            item["dataTimestampUs"] = parse_int_prefix(row.get("timestamp_us", "0"))
            item["dataFetchStartDeltaMs"] = parse_numeric_prefix(
                row.get("fetch_start_to_data_ms", "0"))
            item["interestToDataMs"] = parse_numeric_prefix(
                row.get("interest_to_data_ms", "0"))
            if not item["wireBytes"]:
                item["wireBytes"] = parse_int_prefix(row.get("wire_bytes", "0"))
        elif event == "segment_validated":
            item["consumerLog"] = provider_log
            item["validatedTimestampUs"] = parse_int_prefix(row.get("timestamp_us", "0"))
            item["validatedFetchStartDeltaMs"] = parse_numeric_prefix(
                row.get("fetch_start_to_validated_ms", "0"))
            item["interestToValidatedMs"] = parse_numeric_prefix(
                row.get("interest_to_validated_ms", "0"))
            item["dataToValidatedMs"] = parse_numeric_prefix(
                row.get("data_to_validated_ms", "0"))

    segment_rows = []
    for item in segment_events.values():
        row = dict(item)
        interest_delta = float(row.get("interestFetchStartDeltaMs", 0.0) or 0.0)
        data_delta = float(row.get("dataFetchStartDeltaMs", 0.0) or 0.0)
        validated_delta = float(row.get("validatedFetchStartDeltaMs", 0.0) or 0.0)
        interest_to_data = float(row.get("interestToDataMs", 0.0) or 0.0)
        active_put_ts = int(row.get("activePutTimestampUs", 0) or 0)
        interest_ts = int(row.get("interestTimestampUs", 0) or 0)
        data_ts = int(row.get("dataTimestampUs", 0) or 0)
        validated_ts = int(row.get("validatedTimestampUs", 0) or 0)
        if active_put_ts > 0:
            row["activePutFetchStartDeltaMs"] = 0.0
            row["interestToActivePutMs"] = (
                (active_put_ts - interest_ts) / 1000.0
                if interest_ts > 0 and active_put_ts >= interest_ts else 0.0)
            row["activePutToDataMs"] = (
                (data_ts - active_put_ts) / 1000.0
                if data_ts > 0 and data_ts >= active_put_ts else 0.0)
            row["activePutToValidatedMs"] = (
                (validated_ts - active_put_ts) / 1000.0
                if validated_ts > 0 and validated_ts >= active_put_ts else 0.0)
        else:
            row["activePutFetchStartDeltaMs"] = 0.0
            row["interestToActivePutMs"] = 0.0
            row["activePutToDataMs"] = 0.0
            row["activePutToValidatedMs"] = 0.0
        segment_rows.append(row)

    active_rows = [row for row in segment_rows if row.get("activePutTimestampUs", 0) > 0]
    activation_segment_summary = {
        "layout": layout,
        "count": len(segment_rows),
        "activePutCount": len(active_rows),
        "interestToDataMs": summarize_numeric([
            row["interestToDataMs"] for row in segment_rows
            if row["interestToDataMs"] > 0
        ]),
        "dataToValidatedMs": summarize_numeric([
            row["dataToValidatedMs"] for row in segment_rows
            if row["dataToValidatedMs"] > 0
        ]),
        "interestToActivePutMs": summarize_numeric([
            row["interestToActivePutMs"] for row in segment_rows
            if row["interestToActivePutMs"] > 0
        ]),
        "activePutToDataMs": summarize_numeric([
            row["activePutToDataMs"] for row in segment_rows
            if row["activePutToDataMs"] > 0
        ]),
        "activePutToValidatedMs": summarize_numeric([
            row["activePutToValidatedMs"] for row in segment_rows
            if row["activePutToValidatedMs"] > 0
        ]),
        "wireBytes": summarize_numeric([
            float(row["wireBytes"]) for row in segment_rows
            if row["wireBytes"] > 0
        ]),
        "rows": segment_rows,
    }
    activation_segment_path = OUT / "activation-segment-timeline-stats.json"
    activation_segment_path.write_text(
        json.dumps(activation_segment_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_ACTIVATION_SEGMENT_TIMELINE "
        f"layout={layout} count={activation_segment_summary['count']} "
        f"active_put_count={activation_segment_summary['activePutCount']} "
        f"interest_to_data_p50_ms="
        f"{activation_segment_summary['interestToDataMs']['p50']:.2f} "
        f"interest_to_active_put_p50_ms="
        f"{activation_segment_summary['interestToActivePutMs']['p50']:.2f} "
        f"active_put_to_data_p50_ms="
        f"{activation_segment_summary['activePutToDataMs']['p50']:.2f} "
        f"data_to_validated_p50_ms="
        f"{activation_segment_summary['dataToValidatedMs']['p50']:.2f} "
        f"wire_bytes_p50={activation_segment_summary['wireBytes']['p50']:.0f} "
        f"path={activation_segment_path}"
    )
    activation_by_scope = {}
    for row in segment_rows:
        activation_by_scope.setdefault(row.get("keyScope", "-"), []).append(row)
    for scope, rows in sorted(activation_by_scope.items()):
        active_rows_for_scope = [
            row for row in rows if int(row.get("activePutTimestampUs", 0) or 0) > 0
        ]
        print(
            "YOLO_LAYOUT_ACTIVATION_SEGMENT_SCOPE_TIMELINE "
            f"layout={layout} scope={scope} count={len(rows)} "
            f"active_put_count={len(active_rows_for_scope)} "
            f"interest_to_active_put_p50_ms="
            f"{summarize_numeric([row['interestToActivePutMs'] for row in rows if row['interestToActivePutMs'] > 0])['p50']:.2f} "
            f"active_put_to_data_p50_ms="
            f"{summarize_numeric([row['activePutToDataMs'] for row in rows if row['activePutToDataMs'] > 0])['p50']:.2f} "
            f"data_to_validated_p50_ms="
            f"{summarize_numeric([row['dataToValidatedMs'] for row in rows if row['dataToValidatedMs'] > 0])['p50']:.2f}"
        )

    pending_satisfy_rows = [
        row for row in pending_ims_rows
        if row.get("event") == "satisfy" and "pending_age_ms" in row
    ]
    pending_ims_summary = {
        "layout": layout,
        "count": len(pending_ims_rows),
        "remember": sum(1 for row in pending_ims_rows if row.get("event") == "remember"),
        "satisfy": len(pending_satisfy_rows),
        "pendingAgeMs": summarize_numeric([
            row["pending_age_ms"] for row in pending_satisfy_rows
        ]),
        "rows": pending_ims_rows,
    }
    pending_ims_path = OUT / "pending-ims-timing-stats.json"
    pending_ims_path.write_text(
        json.dumps(pending_ims_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_PENDING_IMS_TIMING "
        f"layout={layout} count={pending_ims_summary['count']} "
        f"remember={pending_ims_summary['remember']} "
        f"satisfy={pending_ims_summary['satisfy']} "
        f"pending_age_p50_ms={pending_ims_summary['pendingAgeMs']['p50']:.2f} "
        f"pending_age_p95_ms={pending_ims_summary['pendingAgeMs']['p95']:.2f} "
        f"path={pending_ims_path}"
    )

    output_by_data_name = {
        row.get("data_name"): row
        for row in output_rows
        if row.get("data_name") and row.get("data_name") != "-"
    }
    frontier_rows = []
    for row in complete_fetch_rows:
        output = output_by_data_name.get(row.get("dataName"))
        if output is None:
            continue
        output_ready = parse_int_prefix(output.get("output_ready_epoch_ms", 0))
        publish_done = parse_int_prefix(output.get("publish_done_epoch_ms", 0))
        first_segment = parse_int_prefix(row.get("first_segment_epoch_ms", 0))
        complete = parse_int_prefix(row.get("complete_epoch_ms", 0))
        if output_ready <= 0 or first_segment <= 0:
            continue
        frontier_rows.append({
            "dataName": row.get("dataName"),
            "producerLog": output.get("providerLog"),
            "consumerLog": row.get("providerLog"),
            "scope": output.get("scope", row.get("keyScope", "")),
            "producerRole": output.get("role", ""),
            "outputReadyEpochMs": output_ready,
            "publishDoneEpochMs": publish_done,
            "firstSegmentEpochMs": first_segment,
            "completeEpochMs": complete,
            "readyToFirstSegmentMs": first_segment - output_ready,
            "publishDoneToFirstSegmentMs": first_segment - publish_done
                if publish_done > 0 else 0,
            "readyToCompleteMs": complete - output_ready
                if complete > 0 else 0,
            "publishMs": output.get("publish_ms", 0.0),
            "fetchElapsedMs": row.get("elapsed_ms", 0.0),
            "encodedBytes": row.get("encoded_bytes", 0),
            "receivedSegments": row.get("received_segments", 0),
        })
    frontier_summary = {
        "layout": layout,
        "count": len(frontier_rows),
        "readyToFirstSegmentMs": summarize_numeric([
            float(row["readyToFirstSegmentMs"]) for row in frontier_rows
        ]),
        "publishDoneToFirstSegmentMs": summarize_numeric([
            float(row["publishDoneToFirstSegmentMs"]) for row in frontier_rows
        ]),
        "readyToCompleteMs": summarize_numeric([
            float(row["readyToCompleteMs"]) for row in frontier_rows
        ]),
        "rows": frontier_rows,
    }
    frontier_by_scope = {}
    for row in frontier_rows:
        frontier_by_scope.setdefault(row.get("scope", "-"), []).append(row)
    frontier_summary["byScope"] = {
        scope: {
            "count": len(rows),
            "readyToFirstSegmentMs": summarize_numeric([
                float(row["readyToFirstSegmentMs"]) for row in rows
            ]),
            "publishDoneToFirstSegmentMs": summarize_numeric([
                float(row["publishDoneToFirstSegmentMs"]) for row in rows
            ]),
            "readyToCompleteMs": summarize_numeric([
                float(row["readyToCompleteMs"]) for row in rows
            ]),
        }
        for scope, rows in sorted(frontier_by_scope.items())
    }
    merge_sessions = {}
    for row in frontier_rows:
        scope = str(row.get("scope", ""))
        producer_role = str(row.get("producerRole", ""))
        is_merge_input = (
            scope.endswith("-to-merge")
            or producer_role.startswith("/Head/")
        )
        if not is_merge_input:
            continue
        session = _session_from_data_name(str(row.get("dataName", "")))
        if session:
            merge_sessions.setdefault(session, []).append(row)
    expected_merge_inputs = max(
        1,
        len({
            str(row.get("scope", ""))
            for row in frontier_rows
            if str(row.get("scope", "")).endswith("-to-merge")
        }),
    )
    merge_batch_rows = []
    for session, rows in sorted(merge_sessions.items()):
        first_values = [float(row["readyToFirstSegmentMs"]) for row in rows]
        complete_values = [float(row["readyToCompleteMs"]) for row in rows]
        merge_batch_rows.append({
            "session": session,
            "inputCount": len(rows),
            "expectedInputCount": expected_merge_inputs,
            "completeInputSet": len(rows) >= expected_merge_inputs,
            "firstSegmentSpreadMs": max(first_values) - min(first_values)
                if first_values else 0.0,
            "completeSpreadMs": max(complete_values) - min(complete_values)
                if complete_values else 0.0,
            "maxReadyToCompleteMs": max(complete_values) if complete_values else 0.0,
        })
    frontier_summary["mergeBatch"] = {
        "sessions": len(merge_batch_rows),
        "completeInputSets": sum(1 for row in merge_batch_rows
                                 if row["completeInputSet"]),
        "firstSegmentSpreadMs": summarize_numeric([
            row["firstSegmentSpreadMs"] for row in merge_batch_rows
        ]),
        "completeSpreadMs": summarize_numeric([
            row["completeSpreadMs"] for row in merge_batch_rows
        ]),
        "maxReadyToCompleteMs": summarize_numeric([
            row["maxReadyToCompleteMs"] for row in merge_batch_rows
        ]),
        "rows": merge_batch_rows,
    }
    frontier_path = OUT / "dependency-frontier-timing-stats.json"
    frontier_path.write_text(
        json.dumps(frontier_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_DEPENDENCY_FRONTIER_TIMING "
        f"layout={layout} count={frontier_summary['count']} "
        f"ready_to_first_p50_ms={frontier_summary['readyToFirstSegmentMs']['p50']:.2f} "
        f"publish_done_to_first_p50_ms="
        f"{frontier_summary['publishDoneToFirstSegmentMs']['p50']:.2f} "
        f"ready_to_complete_p50_ms={frontier_summary['readyToCompleteMs']['p50']:.2f} "
        f"path={frontier_path}"
    )
    for scope, summary in frontier_summary["byScope"].items():
        print(
            "YOLO_LAYOUT_DEPENDENCY_FRONTIER_SCOPE_TIMING "
            f"layout={layout} scope={scope} count={summary['count']} "
            f"ready_to_first_p50_ms={summary['readyToFirstSegmentMs']['p50']:.2f} "
            f"publish_done_to_first_p50_ms="
            f"{summary['publishDoneToFirstSegmentMs']['p50']:.2f} "
            f"ready_to_complete_p50_ms={summary['readyToCompleteMs']['p50']:.2f}"
        )
    merge_batch = frontier_summary["mergeBatch"]
    print(
        "YOLO_LAYOUT_MERGE_BATCH_TIMING "
        f"layout={layout} sessions={merge_batch['sessions']} "
        f"complete_input_sets={merge_batch['completeInputSets']} "
        f"first_segment_spread_p50_ms={merge_batch['firstSegmentSpreadMs']['p50']:.2f} "
        f"complete_spread_p50_ms={merge_batch['completeSpreadMs']['p50']:.2f} "
        f"max_ready_to_complete_p50_ms={merge_batch['maxReadyToCompleteMs']['p50']:.2f}"
    )

    handler_start_rows = [row for row in handler_rows if row.get("event") == "start"]
    handler_end_rows = [row for row in handler_rows if row.get("event") == "end"]
    handler_summary = {
        "layout": layout,
        "count": len(handler_rows),
        "starts": len(handler_start_rows),
        "ends": len(handler_end_rows),
        "queueWaitMs": summarize_numeric([
            row["queue_wait_ms"] for row in handler_start_rows
            if "queue_wait_ms" in row
        ]),
        "inputFetchWaitMs": summarize_numeric([
            row["input_fetch_wait_ms"] for row in handler_end_rows
            if "input_fetch_wait_ms" in row
        ]),
        "runnerPublishMs": summarize_numeric([
            row["runner_publish_ms"] for row in handler_end_rows
            if "runner_publish_ms" in row
        ]),
        "handlerMs": summarize_numeric([
            row["handler_ms"] for row in handler_end_rows
            if "handler_ms" in row
        ]),
        "totalMs": summarize_numeric([
            row["total_ms"] for row in handler_end_rows
            if "total_ms" in row
        ]),
        "rows": handler_rows,
    }
    handler_by_session = {}
    for row in handler_rows:
        session = row.get("session", "-")
        if session and session != "-":
            handler_by_session.setdefault(session, []).append(row)
    dataflow_rows = []
    for session, rows in sorted(handler_by_session.items()):
        starts = [
            parse_int_prefix(row.get("start_epoch_ms", 0)) for row in rows
            if row.get("event") == "start" and parse_int_prefix(row.get("start_epoch_ms", 0)) > 0
        ]
        submitted = [
            parse_int_prefix(row.get("submitted_epoch_ms", 0)) for row in rows
            if parse_int_prefix(row.get("submitted_epoch_ms", 0)) > 0
        ]
        ends = [
            parse_int_prefix(row.get("end_epoch_ms", 0)) for row in rows
            if row.get("event") == "end" and parse_int_prefix(row.get("end_epoch_ms", 0)) > 0
        ]
        if not starts or not ends:
            continue
        roles = sorted({
            row.get("role", "")
            for row in rows
            if row.get("role")
        })
        submitted_start = min(submitted) if submitted else min(starts)
        dataflow_rows.append({
            "session": session,
            "roleCount": len(roles),
            "roles": roles,
            "submittedStartEpochMs": submitted_start,
            "startEpochMs": min(starts),
            "endEpochMs": max(ends),
            "dataflowMs": max(ends) - min(starts),
            "submittedDataflowMs": max(ends) - submitted_start,
        })
    handler_summary["dataflow"] = {
        "count": len(dataflow_rows),
        "dataflowMs": summarize_numeric([
            float(row["dataflowMs"]) for row in dataflow_rows
        ]),
        "submittedDataflowMs": summarize_numeric([
            float(row["submittedDataflowMs"]) for row in dataflow_rows
        ]),
        "rows": dataflow_rows,
    }

    parallel_head_rows = []
    for session, rows in sorted(handler_by_session.items()):
        head_end_rows = {
            str(row.get("role", "")): row
            for row in rows
            if row.get("event") == "end" and
            str(row.get("role", "")).startswith("/Head/Shard/")
        }
        if len(head_end_rows) < 2:
            continue
        intervals = {}
        for role, row in sorted(head_end_rows.items()):
            submitted = float(row.get("submitted_epoch_ms", 0) or 0)
            active_start = float(row.get("worker_start_epoch_ms", submitted) or submitted)
            if active_start <= 0:
                active_start = submitted
            start = submitted + float(row.get("input_fetch_wait_ms", 0.0) or 0.0)
            end = submitted + float(row.get("total_ms", 0.0) or 0.0)
            intervals[role] = {
                "activeStartEpochMsApprox": active_start,
                "startEpochMsApprox": start,
                "endEpochMsApprox": end,
                "activeDurationMs": max(0.0, end - active_start),
                "durationMs": max(0.0, end - start),
            }
        active_starts = [item["activeStartEpochMsApprox"] for item in intervals.values()]
        starts = [item["startEpochMsApprox"] for item in intervals.values()]
        ends = [item["endEpochMsApprox"] for item in intervals.values()]
        active_overlap = max(0.0, min(ends) - max(active_starts))
        active_span = max(ends) - min(active_starts) if active_starts and ends else 0.0
        compute_overlap = max(0.0, min(ends) - max(starts))
        compute_span = max(ends) - min(starts) if starts and ends else 0.0
        serial_sum = sum(item["durationMs"] for item in intervals.values())
        active_serial_sum = sum(item["activeDurationMs"] for item in intervals.values())
        compute_parallel_efficiency = serial_sum / compute_span if compute_span > 0 else 0.0
        active_parallel_efficiency = active_serial_sum / active_span if active_span > 0 else 0.0
        parallel_head_rows.append({
            "session": session,
            "roleCount": len(intervals),
            "roles": sorted(intervals),
            "intervals": intervals,
            "activeOverlapMs": active_overlap,
            "activeSpanMs": active_span,
            "activeSerialSumMs": active_serial_sum,
            "activeParallelEfficiency": active_parallel_efficiency,
            "computeOverlapMs": compute_overlap,
            "computeSpanMs": compute_span,
            "computeSerialSumMs": serial_sum,
            "computeParallelEfficiency": compute_parallel_efficiency,
            # Backward-compatible names describe runner compute interval only.
            "overlapMs": compute_overlap,
            "spanMs": compute_span,
            "serialSumMs": serial_sum,
            "parallelEfficiency": compute_parallel_efficiency,
            "activeOverlapped": active_overlap > 0.0,
            "computeOverlapped": compute_overlap > 0.0,
            "overlapped": active_overlap > 0.0,
        })
    handler_summary["parallelHeadFrontier"] = {
        "sessions": len(parallel_head_rows),
        "overlappedSessions": sum(1 for row in parallel_head_rows if row["overlapped"]),
        "activeOverlapMs": summarize_numeric([
            float(row["activeOverlapMs"]) for row in parallel_head_rows
        ]),
        "activeSpanMs": summarize_numeric([
            float(row["activeSpanMs"]) for row in parallel_head_rows
        ]),
        "activeParallelEfficiency": summarize_numeric([
            float(row["activeParallelEfficiency"]) for row in parallel_head_rows
        ]),
        "computeOverlappedSessions": sum(1 for row in parallel_head_rows
                                         if row["computeOverlapped"]),
        "overlapMs": summarize_numeric([
            float(row["overlapMs"]) for row in parallel_head_rows
        ]),
        "spanMs": summarize_numeric([
            float(row["spanMs"]) for row in parallel_head_rows
        ]),
        "parallelEfficiency": summarize_numeric([
            float(row["parallelEfficiency"]) for row in parallel_head_rows
        ]),
        "rows": parallel_head_rows,
    }

    merge_handler_by_session = {}
    for row in handler_end_rows:
        if str(row.get("role", "")) != "/Merge":
            continue
        session = str(row.get("session", ""))
        if not session:
            continue
        merge_handler_by_session[session] = row
        if session.startswith("/"):
            merge_handler_by_session[session[1:]] = row
        else:
            merge_handler_by_session["/" + session] = row

    frontier_by_session_scope: dict[tuple[str, str], list[dict]] = {}
    for row in frontier_rows:
        scope = str(row.get("scope", ""))
        session = _session_from_data_name(str(row.get("dataName", "")))
        if session and not session.startswith("/"):
            session = "/" + session
        if session:
            frontier_by_session_scope.setdefault((session, scope), []).append(row)

    all_segments_by_session_scope: dict[tuple[str, str], list[dict]] = {}
    segments_by_session_scope: dict[tuple[str, str], list[dict]] = {}
    for row in segment_rows:
        scope = str(row.get("keyScope", ""))
        session = str(row.get("requestId", ""))
        if session:
            all_segments_by_session_scope.setdefault((session, scope), []).append(row)
            if scope.endswith("-to-merge"):
                segments_by_session_scope.setdefault((session, scope), []).append(row)

    def min_positive(values: list[float]) -> float:
        positives = [value for value in values if value > 0]
        return min(positives) if positives else 0.0

    def max_positive(values: list[float]) -> float:
        positives = [value for value in values if value > 0]
        return max(positives) if positives else 0.0

    def us_to_ms_delta(end_us: float, start_us: float) -> float:
        if end_us <= 0 or start_us <= 0 or end_us < start_us:
            return 0.0
        return (end_us - start_us) / 1000.0

    def epoch_ms_delta(end_ms: float, start_ms: float) -> float:
        if end_ms <= 0 or start_ms <= 0 or end_ms < start_ms:
            return 0.0
        return end_ms - start_ms

    def summarize_edge_segments(session: str, scope: str, segments: list[dict]) -> dict:
        frontier_items = frontier_by_session_scope.get((session, scope), [])
        interest_us = min_positive([
            float(row.get("interestTimestampUs", 0) or 0) for row in segments
        ])
        active_put_us = min_positive([
            float(row.get("activePutTimestampUs", 0) or 0) for row in segments
        ])
        first_data_us = min_positive([
            float(row.get("dataTimestampUs", 0) or 0) for row in segments
        ])
        last_validated_us = max_positive([
            float(row.get("validatedTimestampUs", 0) or 0) for row in segments
        ])
        output_ready_ms = min_positive([
            float(row.get("outputReadyEpochMs", 0) or 0) for row in frontier_items
        ])
        publish_done_ms = min_positive([
            float(row.get("publishDoneEpochMs", 0) or 0) for row in frontier_items
        ])
        return {
            "session": session,
            "scope": scope,
            "producerLog": segments[0].get("producerLog", "") if segments else "",
            "consumerLog": segments[0].get("consumerLog", "") if segments else "",
            "segmentCount": len(segments),
            "activePutSegmentCount": sum(
                1 for row in segments
                if int(row.get("activePutTimestampUs", 0) or 0) > 0),
            "interestTimestampUs": int(interest_us),
            "activePutTimestampUs": int(active_put_us),
            "firstDataTimestampUs": int(first_data_us),
            "lastValidatedTimestampUs": int(last_validated_us),
            "outputReadyEpochMs": int(output_ready_ms),
            "publishDoneEpochMs": int(publish_done_ms),
            "interestToActivePutMs": us_to_ms_delta(active_put_us, interest_us),
            "activePutToFirstDataMs": us_to_ms_delta(first_data_us, active_put_us),
            "firstDataToLastValidatedMs": us_to_ms_delta(last_validated_us, first_data_us),
            "interestToLastValidatedMs": us_to_ms_delta(last_validated_us, interest_us),
            "outputReadyToFirstDataMs": epoch_ms_delta(first_data_us / 1000.0, output_ready_ms),
            "publishDoneToLastValidatedMs": epoch_ms_delta(last_validated_us / 1000.0, publish_done_ms),
        }

    dag_edge_rows = [
        summarize_edge_segments(session, scope, segments)
        for (session, scope), segments in sorted(all_segments_by_session_scope.items())
    ]

    merge_critical_rows = []
    for (session, scope), segments in sorted(segments_by_session_scope.items()):
        merge_handler = merge_handler_by_session.get(session, {})
        last_validated_us = max_positive([
            float(row.get("validatedTimestampUs", 0) or 0) for row in segments
        ])
        merge_start_ms = float(merge_handler.get("start_epoch_ms", 0) or 0)
        merge_end_ms = float(merge_handler.get("end_epoch_ms", 0) or 0)
        merge_row = {
            **summarize_edge_segments(session, scope, segments),
            "mergeHandlerStartEpochMs": int(merge_start_ms),
            "mergeHandlerEndEpochMs": int(merge_end_ms),
            "mergeStartToLastValidatedMs": epoch_ms_delta(last_validated_us / 1000.0, merge_start_ms),
            "mergeStartToEndMs": epoch_ms_delta(merge_end_ms, merge_start_ms),
            "lastValidatedToMergeEndMs": epoch_ms_delta(merge_end_ms, last_validated_us / 1000.0),
        }
        merge_critical_rows.append(merge_row)

    critical_by_session: dict[str, list[dict]] = {}
    for row in merge_critical_rows:
        critical_by_session.setdefault(row["session"], []).append(row)
    merge_session_rows = []
    for session, rows in sorted(critical_by_session.items()):
        merge_handler = merge_handler_by_session.get(session, {})
        merge_start_ms = float(merge_handler.get("start_epoch_ms", 0) or 0)
        merge_end_ms = float(merge_handler.get("end_epoch_ms", 0) or 0)
        last_validated_ms = max_positive([
            float(row.get("lastValidatedTimestampUs", 0) or 0) / 1000.0
            for row in rows
        ])
        first_interest_ms = min_positive([
            float(row.get("interestTimestampUs", 0) or 0) / 1000.0
            for row in rows
        ])
        first_active_put_ms = min_positive([
            float(row.get("activePutTimestampUs", 0) or 0) / 1000.0
            for row in rows
        ])
        merge_session_rows.append({
            "session": session,
            "edgeCount": len(rows),
            "mergeHandlerStartEpochMs": int(merge_start_ms),
            "mergeHandlerEndEpochMs": int(merge_end_ms),
            "firstInterestToFirstActivePutMs": epoch_ms_delta(
                first_active_put_ms, first_interest_ms),
            "firstInterestToLastValidatedMs": epoch_ms_delta(
                last_validated_ms, first_interest_ms),
            "mergeStartToLastValidatedMs": epoch_ms_delta(
                last_validated_ms, merge_start_ms),
            "lastValidatedToMergeEndMs": epoch_ms_delta(
                merge_end_ms, last_validated_ms),
            "mergeStartToEndMs": epoch_ms_delta(merge_end_ms, merge_start_ms),
            "maxInterestToActivePutMs": max(
                [float(row.get("interestToActivePutMs", 0.0) or 0.0)
                 for row in rows],
                default=0.0),
            "maxActivePutToFirstDataMs": max(
                [float(row.get("activePutToFirstDataMs", 0.0) or 0.0)
                 for row in rows],
                default=0.0),
            "maxPublishDoneToLastValidatedMs": max(
                [float(row.get("publishDoneToLastValidatedMs", 0.0) or 0.0)
                 for row in rows],
                default=0.0),
        })

    all_edges_by_session: dict[str, list[dict]] = {}
    for row in dag_edge_rows:
        all_edges_by_session.setdefault(row["session"], []).append(row)
    dag_session_rows = []
    for session, rows in sorted(all_edges_by_session.items()):
        merge_handler = merge_handler_by_session.get(session, {})
        merge_end_ms = float(merge_handler.get("end_epoch_ms", 0) or 0)
        first_interest_ms = min_positive([
            float(row.get("interestTimestampUs", 0) or 0) / 1000.0 for row in rows
        ])
        first_active_put_ms = min_positive([
            float(row.get("activePutTimestampUs", 0) or 0) / 1000.0 for row in rows
        ])
        last_validated_ms = max_positive([
            float(row.get("lastValidatedTimestampUs", 0) or 0) / 1000.0 for row in rows
        ])
        max_interest_to_active = max(
            [float(row.get("interestToActivePutMs", 0.0) or 0.0) for row in rows],
            default=0.0)
        max_active_to_data = max(
            [float(row.get("activePutToFirstDataMs", 0.0) or 0.0) for row in rows],
            default=0.0)
        max_publish_to_validated = max(
            [float(row.get("publishDoneToLastValidatedMs", 0.0) or 0.0) for row in rows],
            default=0.0)
        slowest_edge = max(
            rows,
            key=lambda row: float(row.get("interestToLastValidatedMs", 0.0) or 0.0),
            default={},
        )
        dag_session_rows.append({
            "session": session,
            "edgeCount": len(rows),
            "firstInterestToFirstActivePutMs": epoch_ms_delta(
                first_active_put_ms, first_interest_ms),
            "firstInterestToLastValidatedMs": epoch_ms_delta(
                last_validated_ms, first_interest_ms),
            "firstInterestToMergeEndMs": epoch_ms_delta(
                merge_end_ms, first_interest_ms),
            "maxInterestToActivePutMs": max_interest_to_active,
            "maxActivePutToFirstDataMs": max_active_to_data,
            "maxPublishDoneToLastValidatedMs": max_publish_to_validated,
            "slowestEdgeScope": slowest_edge.get("scope", ""),
            "slowestEdgeInterestToLastValidatedMs": slowest_edge.get(
                "interestToLastValidatedMs", 0.0),
        })

    dag_summary = {
        "layout": layout,
        "edgeRows": len(dag_edge_rows),
        "sessionRows": len(dag_session_rows),
        "interestToActivePutMs": summarize_numeric([
            row["interestToActivePutMs"] for row in dag_edge_rows
            if row["interestToActivePutMs"] > 0
        ]),
        "activePutToFirstDataMs": summarize_numeric([
            row["activePutToFirstDataMs"] for row in dag_edge_rows
            if row["activePutToFirstDataMs"] > 0
        ]),
        "publishDoneToLastValidatedMs": summarize_numeric([
            row["publishDoneToLastValidatedMs"] for row in dag_edge_rows
            if row["publishDoneToLastValidatedMs"] > 0
        ]),
        "firstInterestToLastValidatedMs": summarize_numeric([
            row["firstInterestToLastValidatedMs"] for row in dag_session_rows
            if row["firstInterestToLastValidatedMs"] > 0
        ]),
        "firstInterestToMergeEndMs": summarize_numeric([
            row["firstInterestToMergeEndMs"] for row in dag_session_rows
            if row["firstInterestToMergeEndMs"] > 0
        ]),
        "rows": dag_edge_rows,
        "sessions": dag_session_rows,
    }
    dag_path = OUT / "dag-activation-critical-path.json"
    dag_path.write_text(json.dumps(dag_summary, indent=2, sort_keys=True),
                        encoding="utf-8")
    dag_csv = OUT / "dag-activation-critical-path.csv"
    dag_csv_fields = [
        "session", "scope", "producerLog", "consumerLog", "segmentCount",
        "activePutSegmentCount", "interestToActivePutMs",
        "activePutToFirstDataMs", "firstDataToLastValidatedMs",
        "interestToLastValidatedMs", "outputReadyToFirstDataMs",
        "publishDoneToLastValidatedMs",
    ]
    with dag_csv.open("w", encoding="utf-8") as fh:
        fh.write(",".join(dag_csv_fields) + "\n")
        for row in dag_edge_rows:
            fh.write(",".join(str(row.get(field, "")) for field in dag_csv_fields) + "\n")
    print(
        "YOLO_LAYOUT_DAG_ACTIVATION_CRITICAL_PATH "
        f"layout={layout} edge_rows={dag_summary['edgeRows']} "
        f"sessions={dag_summary['sessionRows']} "
        f"interest_to_active_put_p50_ms="
        f"{dag_summary['interestToActivePutMs']['p50']:.2f} "
        f"active_put_to_first_data_p50_ms="
        f"{dag_summary['activePutToFirstDataMs']['p50']:.2f} "
        f"publish_done_to_last_validated_p50_ms="
        f"{dag_summary['publishDoneToLastValidatedMs']['p50']:.2f} "
        f"path={dag_path}"
    )

    merge_critical_summary = {
        "layout": layout,
        "edgeRows": len(merge_critical_rows),
        "sessionRows": len(merge_session_rows),
        "interestToActivePutMs": summarize_numeric([
            row["interestToActivePutMs"] for row in merge_critical_rows
            if row["interestToActivePutMs"] > 0
        ]),
        "activePutToFirstDataMs": summarize_numeric([
            row["activePutToFirstDataMs"] for row in merge_critical_rows
            if row["activePutToFirstDataMs"] > 0
        ]),
        "publishDoneToLastValidatedMs": summarize_numeric([
            row["publishDoneToLastValidatedMs"] for row in merge_critical_rows
            if row["publishDoneToLastValidatedMs"] > 0
        ]),
        "mergeStartToLastValidatedMs": summarize_numeric([
            row["mergeStartToLastValidatedMs"] for row in merge_session_rows
            if row["mergeStartToLastValidatedMs"] > 0
        ]),
        "lastValidatedToMergeEndMs": summarize_numeric([
            row["lastValidatedToMergeEndMs"] for row in merge_session_rows
            if row["lastValidatedToMergeEndMs"] > 0
        ]),
        "mergeStartToEndMs": summarize_numeric([
            row["mergeStartToEndMs"] for row in merge_session_rows
            if row["mergeStartToEndMs"] > 0
        ]),
        "rows": merge_critical_rows,
        "sessions": merge_session_rows,
    }
    merge_critical_path = OUT / "merge-activation-critical-path.json"
    merge_critical_path.write_text(
        json.dumps(merge_critical_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    merge_critical_csv = OUT / "merge-activation-critical-path.csv"
    csv_fields = [
        "session", "scope", "producerLog", "consumerLog", "segmentCount",
        "activePutSegmentCount", "interestToActivePutMs",
        "activePutToFirstDataMs", "firstDataToLastValidatedMs",
        "interestToLastValidatedMs", "outputReadyToFirstDataMs",
        "publishDoneToLastValidatedMs", "mergeStartToLastValidatedMs",
        "lastValidatedToMergeEndMs", "mergeStartToEndMs",
    ]
    with merge_critical_csv.open("w", encoding="utf-8") as fh:
        fh.write(",".join(csv_fields) + "\n")
        for row in merge_critical_rows:
            fh.write(",".join(str(row.get(field, "")) for field in csv_fields) + "\n")
    print(
        "YOLO_LAYOUT_MERGE_ACTIVATION_CRITICAL_PATH "
        f"layout={layout} edge_rows={merge_critical_summary['edgeRows']} "
        f"sessions={merge_critical_summary['sessionRows']} "
        f"interest_to_active_put_p50_ms="
        f"{merge_critical_summary['interestToActivePutMs']['p50']:.2f} "
        f"active_put_to_first_data_p50_ms="
        f"{merge_critical_summary['activePutToFirstDataMs']['p50']:.2f} "
        f"merge_start_to_last_validated_p50_ms="
        f"{merge_critical_summary['mergeStartToLastValidatedMs']['p50']:.2f} "
        f"last_validated_to_merge_end_p50_ms="
        f"{merge_critical_summary['lastValidatedToMergeEndMs']['p50']:.2f} "
        f"path={merge_critical_path}"
    )

    handler_path = OUT / "provider-handler-timing-stats.json"
    handler_path.write_text(
        json.dumps(handler_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_PROVIDER_HANDLER_TIMING "
        f"layout={layout} starts={handler_summary['starts']} "
        f"ends={handler_summary['ends']} "
        f"queue_wait_p50_ms={handler_summary['queueWaitMs']['p50']:.2f} "
        f"input_fetch_wait_p50_ms="
        f"{handler_summary['inputFetchWaitMs']['p50']:.2f} "
        f"runner_publish_p50_ms="
        f"{handler_summary['runnerPublishMs']['p50']:.2f} "
        f"handler_p50_ms={handler_summary['handlerMs']['p50']:.2f} "
        f"total_p50_ms={handler_summary['totalMs']['p50']:.2f} "
        f"dataflow_p50_ms={handler_summary['dataflow']['dataflowMs']['p50']:.2f} "
        f"submitted_dataflow_p50_ms="
        f"{handler_summary['dataflow']['submittedDataflowMs']['p50']:.2f} "
        f"path={handler_path}"
    )
    parallel_head = handler_summary["parallelHeadFrontier"]
    print(
        "YOLO_LAYOUT_PARALLEL_HEAD_FRONTIER "
        f"layout={layout} sessions={parallel_head['sessions']} "
        f"overlapped_sessions={parallel_head['overlappedSessions']} "
        f"active_overlap_p50_ms={parallel_head['activeOverlapMs']['p50']:.2f} "
        f"active_span_p50_ms={parallel_head['activeSpanMs']['p50']:.2f} "
        f"active_parallel_efficiency_p50="
        f"{parallel_head['activeParallelEfficiency']['p50']:.2f} "
        f"compute_overlapped_sessions="
        f"{parallel_head['computeOverlappedSessions']} "
        f"compute_overlap_p50_ms={parallel_head['overlapMs']['p50']:.2f} "
        f"compute_span_p50_ms={parallel_head['spanMs']['p50']:.2f} "
        f"compute_parallel_efficiency_p50="
        f"{parallel_head['parallelEfficiency']['p50']:.2f}"
    )
    return planned_transport_ok


def _session_from_data_name(name: str) -> str:
    marker = "/NDNSF/DI/ACTIVATION/"
    if marker not in name:
        return ""
    rest = name.split(marker, 1)[1]
    return rest.split("/", 1)[0]


def write_client_timing_summaries(layout: str,
                                  phases: list[tuple[str, Path]]) -> None:
    rows = []
    for phase, path in phases:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "NDNSF_DI_CLIENT_INFERENCE_TIMING" not in line:
                continue
            row = parse_key_value_line(line)
            for key in ("plan_ms", "scope_key_ms", "request_ms", "total_ms"):
                if key in row:
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
            row["phase"] = phase
            row["log"] = path.name
            rows.append(row)
    summary = {
        "layout": layout,
        "count": len(rows),
        "requestMs": summarize_numeric([
            row["request_ms"] for row in rows if "request_ms" in row
        ]),
        "totalMs": summarize_numeric([
            row["total_ms"] for row in rows if "total_ms" in row
        ]),
        "planMs": summarize_numeric([
            row["plan_ms"] for row in rows if "plan_ms" in row
        ]),
        "scopeKeyMs": summarize_numeric([
            row["scope_key_ms"] for row in rows if "scope_key_ms" in row
        ]),
        "rows": rows,
    }
    path = OUT / "client-inference-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_CLIENT_INFERENCE_TIMING "
        f"layout={layout} count={summary['count']} "
        f"request_p50_ms={summary['requestMs']['p50']:.2f} "
        f"total_p50_ms={summary['totalMs']['p50']:.2f} "
        f"plan_p50_ms={summary['planMs']['p50']:.2f} "
        f"scope_key_p50_ms={summary['scopeKeyMs']['p50']:.2f} "
        f"path={path}"
    )


def write_hybrid_crypto_timing_summaries(layout: str,
                                         phases: list[tuple[str, Path]],
                                         providers: list[tuple[str, str, list[str]]]) -> None:
    rows = []
    log_paths = [(phase, path) for phase, path in phases]
    log_paths.extend(("provider", OUT / f"{name}.log") for _, name, _ in providers)
    for phase, path in log_paths:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "NDNSF_CRYPTO_TIMING" not in line:
                continue
            row = parse_key_value_line(line)
            row["phase"] = phase
            row["log"] = path.name
            for key in (
                    "steady_us",
                    "timestamp_us",
                    "entryToCacheLookupUs",
                    "entryToKeyReadyUs",
                    "keyReadyToAesStartUs",
                    "aesUs",
                    "aesDoneToCallbackUs",
                    "unwrapUs",
                    "cipherBytes",
                    "keyBytes"):
                if key in row:
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
            rows.append(row)

    groups: dict[str, list[dict]] = {}
    for row in rows:
        group_key = "|".join([
            row.get("role", ""),
            row.get("messageType", ""),
            row.get("event", ""),
            f"hit={row.get('hit', '')}",
            f"source={row.get('source', '')}",
        ])
        groups.setdefault(group_key, []).append(row)

    group_summaries = {}
    for key, group_rows in sorted(groups.items()):
        summary = {
            "count": len(group_rows),
            "role": group_rows[0].get("role", "") if group_rows else "",
            "messageType": group_rows[0].get("messageType", "") if group_rows else "",
            "event": group_rows[0].get("event", "") if group_rows else "",
            "hit": group_rows[0].get("hit", "") if group_rows else "",
            "source": group_rows[0].get("source", "") if group_rows else "",
            "entryToCacheLookupUs": summarize_numeric([
                row["entryToCacheLookupUs"] for row in group_rows
                if "entryToCacheLookupUs" in row
            ]),
            "entryToKeyReadyUs": summarize_numeric([
                row["entryToKeyReadyUs"] for row in group_rows
                if "entryToKeyReadyUs" in row
            ]),
            "keyReadyToAesStartUs": summarize_numeric([
                row["keyReadyToAesStartUs"] for row in group_rows
                if "keyReadyToAesStartUs" in row
            ]),
            "aesUs": summarize_numeric([
                row["aesUs"] for row in group_rows
                if "aesUs" in row
            ]),
            "aesDoneToCallbackUs": summarize_numeric([
                row["aesDoneToCallbackUs"] for row in group_rows
                if "aesDoneToCallbackUs" in row
            ]),
            "unwrapUs": summarize_numeric([
                row["unwrapUs"] for row in group_rows
                if "unwrapUs" in row
            ]),
            "cipherBytes": summarize_numeric([
                row["cipherBytes"] for row in group_rows
                if "cipherBytes" in row
            ]),
        }
        group_summaries[key] = summary

    summary = {
        "layout": layout,
        "count": len(rows),
        "groups": group_summaries,
        "rows": rows,
    }
    path = OUT / "hybrid-crypto-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")

    provider_request = [
        row for row in rows
        if row.get("role") == "provider" and row.get("messageType") == "REQUEST"
    ]
    user_ack = [
        row for row in rows
        if row.get("role") == "user" and row.get("messageType") == "ACK"
    ]
    provider_request_aes = summarize_numeric([
        row["aesUs"] for row in provider_request
        if row.get("event") == "hybrid_decrypt_aes_done" and "aesUs" in row
    ])
    provider_request_key = summarize_numeric([
        row["entryToKeyReadyUs"] for row in provider_request
        if row.get("event") == "hybrid_decrypt_aes_done" and "entryToKeyReadyUs" in row
    ])
    user_ack_aes = summarize_numeric([
        row["aesUs"] for row in user_ack
        if row.get("event") == "hybrid_decrypt_aes_done" and "aesUs" in row
    ])
    user_ack_callback = summarize_numeric([
        row["aesDoneToCallbackUs"] for row in user_ack
        if row.get("event") == "hybrid_decrypt_callback_dispatch" and "aesDoneToCallbackUs" in row
    ])

    def cache_counts(role: str, message_type: str) -> tuple[int, int]:
        cache_rows = [
            row for row in rows
            if row.get("event") == "hybrid_decrypt_key_cache" and
            row.get("role") == role and
            row.get("messageType") == message_type
        ]
        hits = sum(1 for row in cache_rows if row.get("hit") == "true")
        misses = sum(1 for row in cache_rows if row.get("hit") == "false")
        return hits, misses

    provider_request_hits, provider_request_misses = cache_counts("provider", "REQUEST")
    provider_selection_hits, provider_selection_misses = cache_counts("provider", "SELECTION")
    user_ack_hits, user_ack_misses = cache_counts("user", "ACK")
    user_response_hits, user_response_misses = cache_counts("user", "RESPONSE")
    print(
        "YOLO_LAYOUT_HYBRID_CRYPTO_TIMING "
        f"layout={layout} count={summary['count']} "
        f"provider_request_key_ready_p50_us={provider_request_key['p50']:.0f} "
        f"provider_request_aes_p50_us={provider_request_aes['p50']:.0f} "
        f"user_ack_aes_p50_us={user_ack_aes['p50']:.0f} "
        f"user_ack_callback_p50_us={user_ack_callback['p50']:.0f} "
        f"provider_request_cache_hit={provider_request_hits} "
        f"provider_request_cache_miss={provider_request_misses} "
        f"provider_selection_cache_hit={provider_selection_hits} "
        f"provider_selection_cache_miss={provider_selection_misses} "
        f"user_ack_cache_hit={user_ack_hits} "
        f"user_ack_cache_miss={user_ack_misses} "
        f"user_response_cache_hit={user_response_hits} "
        f"user_response_cache_miss={user_response_misses} "
        f"path={path}"
    )


def write_control_timing_summaries(layout: str,
                                   phases: list[tuple[str, Path]],
                                   providers: list[tuple[str, str, list[str]]],
                                   service_name: str) -> None:
    rows = []
    log_paths = [(phase, path) for phase, path in phases]
    log_paths.extend(("provider", OUT / f"{name}.log") for _, name, _ in providers)
    for phase, path in log_paths:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "NDNSF_CONTROL_TIMING" not in line:
                continue
            row = parse_key_value_line(line)
            if str(row.get("serviceName", "")) != service_name:
                continue
            row["phase"] = phase
            row["log"] = path.name
            for key in (
                    "steady_us",
                    "timestamp_us",
                    "queuedDurationMs",
                    "inflightDurationMs",
                    "endToEndLatencyMs",
                    "pendingAtDecision",
                    "selectionLagUs",
                    "eventLoopLagUs"):
                if key in row:
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
            rows.append(row)

    def event_time(group_rows: list[dict], event: str) -> float:
        values = [
            float(row.get("steady_us", 0))
            for row in group_rows
            if row.get("event") == event and float(row.get("steady_us", 0)) > 0
        ]
        return min(values) if values else 0.0

    def event_epoch_s(group_rows: list[dict], event: str) -> float:
        values = [
            float(row.get("timestamp_us", 0)) / 1000000.0
            for row in group_rows
            if row.get("event") == event and float(row.get("timestamp_us", 0)) > 0
        ]
        return min(values) if values else 0.0

    def delta_ms(end_us: float, start_us: float) -> float:
        if end_us <= 0 or start_us <= 0 or end_us < start_us:
            return 0.0
        return (end_us - start_us) / 1000.0

    user_groups: dict[tuple[str, str], list[dict]] = {}
    provider_groups: dict[tuple[str, str, str], list[dict]] = {}
    for row in rows:
        request_id = str(row.get("requestId", ""))
        if not request_id:
            continue
        if row.get("role") == "user":
            user_groups.setdefault((row["phase"], request_id), []).append(row)
        elif row.get("role") == "provider":
            provider = str(row.get("providerName", row.get("log", "")))
            provider_groups.setdefault((request_id, provider, row["log"]), []).append(row)

    user_summary_rows = []
    for (phase, request_id), group_rows in user_groups.items():
        request_published = event_time(group_rows, "REQUEST_PUBLISHED")
        ack_matched = event_time(group_rows, "ACK_MATCHED")
        provider_selected = event_time(group_rows, "PROVIDER_SELECTED")
        selection_published = event_time(group_rows, "SELECTION_PUBLISHED")
        response_observed = event_time(group_rows, "RESPONSE_OBSERVED")
        response_decrypted = event_time(group_rows, "RESPONSE_DECRYPTED")
        callback_fired = event_time(group_rows, "CALLBACK_FIRED")
        completed = event_time(group_rows, "COMPLETED")
        user_summary_rows.append({
            "phase": phase,
            "requestId": request_id,
            "requestPublishedToAckMatchedMs": delta_ms(ack_matched, request_published),
            "ackMatchedToProviderSelectedMs": delta_ms(provider_selected, ack_matched),
            "providerSelectedToSelectionPublishedMs": delta_ms(selection_published, provider_selected),
            "selectionPublishedToResponseObservedMs": delta_ms(response_observed, selection_published),
            "responseObservedToDecryptedMs": delta_ms(response_decrypted, response_observed),
            "responseDecryptedToCallbackMs": delta_ms(callback_fired, response_decrypted),
            "requestPublishedToCallbackMs": delta_ms(callback_fired, request_published),
            "requestPublishedToCompletedMs": delta_ms(completed, request_published),
        })

    provider_summary_rows = []
    for (request_id, provider, log), group_rows in provider_groups.items():
        observed = event_time(group_rows, "REQUEST_OBSERVED")
        admission = event_time(group_rows, "ACK_ADMISSION_CHECKED")
        ack_published = event_time(group_rows, "ACK_PUBLISHED")
        selection_received = event_time(group_rows, "SELECTION_RECEIVED")
        execution_started = event_time(group_rows, "EXECUTION_STARTED")
        execution_done = event_time(group_rows, "EXECUTION_DONE")
        response_published = event_time(group_rows, "RESPONSE_PUBLISHED")
        provider_summary_rows.append({
            "requestId": request_id,
            "providerName": provider,
            "log": log,
            "requestObservedToAckAdmissionMs": delta_ms(admission, observed),
            "ackAdmissionToAckPublishedMs": delta_ms(ack_published, admission),
            "requestObservedToAckPublishedMs": delta_ms(ack_published, observed),
            "ackPublishedToSelectionReceivedMs": delta_ms(selection_received, ack_published),
            "selectionReceivedToExecutionStartedMs": delta_ms(execution_started, selection_received),
            "executionStartedToDoneMs": delta_ms(execution_done, execution_started),
            "executionDoneToResponsePublishedMs": delta_ms(response_published, execution_done),
            "selectionReceivedToResponsePublishedMs": delta_ms(response_published, selection_received),
        })
    final_provider_rows = [
        row for row in provider_summary_rows
        if row["selectionReceivedToResponsePublishedMs"] > 0
    ]

    summary = {
        "layout": layout,
        "count": len(rows),
        "userRequests": len(user_summary_rows),
        "providerRequests": len(provider_summary_rows),
        "finalProviderRequests": len(final_provider_rows),
        "user": {
            "requestPublishedToAckMatchedMs": summarize_numeric([
                row["requestPublishedToAckMatchedMs"] for row in user_summary_rows
            ]),
            "ackMatchedToProviderSelectedMs": summarize_numeric([
                row["ackMatchedToProviderSelectedMs"] for row in user_summary_rows
            ]),
            "providerSelectedToSelectionPublishedMs": summarize_numeric([
                row["providerSelectedToSelectionPublishedMs"] for row in user_summary_rows
            ]),
            "selectionPublishedToResponseObservedMs": summarize_numeric([
                row["selectionPublishedToResponseObservedMs"] for row in user_summary_rows
            ]),
            "responseObservedToDecryptedMs": summarize_numeric([
                row["responseObservedToDecryptedMs"] for row in user_summary_rows
            ]),
            "responseDecryptedToCallbackMs": summarize_numeric([
                row["responseDecryptedToCallbackMs"] for row in user_summary_rows
            ]),
            "requestPublishedToCallbackMs": summarize_numeric([
                row["requestPublishedToCallbackMs"] for row in user_summary_rows
            ]),
            "requestPublishedToCompletedMs": summarize_numeric([
                row["requestPublishedToCompletedMs"] for row in user_summary_rows
            ]),
        },
        "provider": {
            "requestObservedToAckPublishedMs": summarize_numeric([
                row["requestObservedToAckPublishedMs"] for row in provider_summary_rows
            ]),
            "ackPublishedToSelectionReceivedMs": summarize_numeric([
                row["ackPublishedToSelectionReceivedMs"] for row in provider_summary_rows
            ]),
            "selectionReceivedToExecutionStartedMs": summarize_numeric([
                row["selectionReceivedToExecutionStartedMs"] for row in provider_summary_rows
            ]),
            "executionStartedToDoneMs": summarize_numeric([
                row["executionStartedToDoneMs"] for row in provider_summary_rows
            ]),
            "executionDoneToResponsePublishedMs": summarize_numeric([
                row["executionDoneToResponsePublishedMs"] for row in provider_summary_rows
            ]),
            "selectionReceivedToResponsePublishedMs": summarize_numeric([
                row["selectionReceivedToResponsePublishedMs"] for row in provider_summary_rows
            ]),
        },
        "finalProvider": {
            "requestObservedToAckPublishedMs": summarize_numeric([
                row["requestObservedToAckPublishedMs"] for row in final_provider_rows
            ]),
            "ackPublishedToSelectionReceivedMs": summarize_numeric([
                row["ackPublishedToSelectionReceivedMs"] for row in final_provider_rows
            ]),
            "selectionReceivedToExecutionStartedMs": summarize_numeric([
                row["selectionReceivedToExecutionStartedMs"] for row in final_provider_rows
            ]),
            "executionStartedToDoneMs": summarize_numeric([
                row["executionStartedToDoneMs"] for row in final_provider_rows
            ]),
            "executionDoneToResponsePublishedMs": summarize_numeric([
                row["executionDoneToResponsePublishedMs"] for row in final_provider_rows
            ]),
            "selectionReceivedToResponsePublishedMs": summarize_numeric([
                row["selectionReceivedToResponsePublishedMs"] for row in final_provider_rows
            ]),
        },
        "userRows": user_summary_rows,
        "providerRows": provider_summary_rows,
        "finalProviderRows": final_provider_rows,
        "rows": rows,
    }
    path = OUT / "control-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_CONTROL_TIMING "
        f"layout={layout} count={summary['count']} "
        f"user_request_published_to_ack_matched_p50_ms="
        f"{summary['user']['requestPublishedToAckMatchedMs']['p50']:.2f} "
        f"user_ack_to_selected_p50_ms="
        f"{summary['user']['ackMatchedToProviderSelectedMs']['p50']:.2f} "
        f"user_selection_to_response_p50_ms="
        f"{summary['user']['selectionPublishedToResponseObservedMs']['p50']:.2f} "
        f"user_response_decrypt_p50_ms="
        f"{summary['user']['responseObservedToDecryptedMs']['p50']:.2f} "
        f"user_request_to_callback_p50_ms="
        f"{summary['user']['requestPublishedToCallbackMs']['p50']:.2f} "
        f"provider_request_to_ack_p50_ms="
        f"{summary['provider']['requestObservedToAckPublishedMs']['p50']:.2f} "
        f"provider_ack_to_selection_p50_ms="
        f"{summary['provider']['ackPublishedToSelectionReceivedMs']['p50']:.2f} "
        f"provider_selection_to_response_p50_ms="
        f"{summary['provider']['selectionReceivedToResponsePublishedMs']['p50']:.2f} "
        f"final_provider_selection_to_response_p50_ms="
        f"{summary['finalProvider']['selectionReceivedToResponsePublishedMs']['p50']:.2f} "
        f"final_provider_execution_p50_ms="
        f"{summary['finalProvider']['executionStartedToDoneMs']['p50']:.2f} "
        f"path={path}"
    )


def write_outer_control_waterfall_summaries(
        layout: str,
        phases: list[tuple[str, Path]],
        providers: list[tuple[str, str, list[str]]],
        service_name: str) -> None:
    rows = []
    log_paths = [(phase, path) for phase, path in phases]
    log_paths.extend(("provider", OUT / f"{name}.log") for _, name, _ in providers)
    for phase, path in log_paths:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "NDNSF_CONTROL_TIMING" not in line:
                continue
            row = parse_key_value_line(line)
            if str(row.get("serviceName", "")) != service_name:
                continue
            row["phase"] = phase
            row["log"] = path.name
            for key in ("steady_us", "timestamp_us"):
                if key in row:
                    row[key] = parse_numeric_prefix(row.get(key, "0"))
            rows.append(row)

    def event_time(group_rows: list[dict], event: str) -> float:
        values = [
            float(row.get("steady_us", 0))
            for row in group_rows
            if row.get("event") == event and float(row.get("steady_us", 0)) > 0
        ]
        return min(values) if values else 0.0

    def event_epoch_s(group_rows: list[dict], event: str) -> float:
        values = [
            float(row.get("timestamp_us", 0)) / 1000000.0
            for row in group_rows
            if row.get("event") == event and float(row.get("timestamp_us", 0)) > 0
        ]
        return min(values) if values else 0.0

    def delta_ms(end_us: float, start_us: float) -> float:
        if end_us <= 0 or start_us <= 0 or end_us < start_us:
            return 0.0
        return (end_us - start_us) / 1000.0

    user_groups: dict[tuple[str, str], list[dict]] = {}
    provider_groups: dict[tuple[str, str, str], list[dict]] = {}
    for row in rows:
        request_id = str(row.get("requestId", ""))
        if not request_id:
            continue
        if row.get("role") == "user":
            user_groups.setdefault((str(row["phase"]), request_id), []).append(row)
        elif row.get("role") == "provider":
            provider = str(row.get("providerName", row.get("log", "")))
            provider_groups.setdefault((request_id, provider, str(row["log"])), []).append(row)

    user_rows = []
    for (phase, request_id), group_rows in user_groups.items():
        request_published = event_time(group_rows, "REQUEST_PUBLISHED")
        ack_observed = event_time(group_rows, "ACK_OBSERVED")
        ack_matched = event_time(group_rows, "ACK_MATCHED")
        provider_selected = event_time(group_rows, "PROVIDER_SELECTED")
        selection_published = event_time(group_rows, "SELECTION_PUBLISHED")
        selection_direct_put = event_time(group_rows, "SELECTION_DIRECT_PUT")
        response_observed = event_time(group_rows, "RESPONSE_OBSERVED")
        response_decrypted = event_time(group_rows, "RESPONSE_DECRYPTED")
        callback_fired = event_time(group_rows, "CALLBACK_FIRED")
        request_published_epoch_s = event_epoch_s(group_rows, "REQUEST_PUBLISHED")
        response_observed_epoch_s = event_epoch_s(group_rows, "RESPONSE_OBSERVED")
        callback_fired_epoch_s = event_epoch_s(group_rows, "CALLBACK_FIRED")
        selection_published_epoch_s = event_epoch_s(group_rows, "SELECTION_PUBLISHED")
        user_rows.append({
            "phase": phase,
            "requestId": request_id,
            "requestPublishedEpochS": request_published_epoch_s,
            "selectionPublishedEpochS": selection_published_epoch_s,
            "responseObservedEpochS": response_observed_epoch_s,
            "callbackFiredEpochS": callback_fired_epoch_s,
            "requestPublishedToFirstAckObservedMs": delta_ms(ack_observed, request_published),
            "firstAckObservedToAckMatchedMs": delta_ms(ack_matched, ack_observed),
            "ackMatchedToProviderSelectedMs": delta_ms(provider_selected, ack_matched),
            "providerSelectedToSelectionPublishedMs": delta_ms(selection_published, provider_selected),
            "selectionPublishedToSelectionDirectPutMs": delta_ms(selection_direct_put, selection_published),
            "selectionPublishedToResponseObservedMs": delta_ms(response_observed, selection_published),
            "responseObservedToDecryptedMs": delta_ms(response_decrypted, response_observed),
            "responseDecryptedToCallbackMs": delta_ms(callback_fired, response_decrypted),
            "requestPublishedToCallbackMs": delta_ms(callback_fired, request_published),
        })

    provider_rows = []
    for (request_id, provider, log), group_rows in provider_groups.items():
        request_observed = event_time(group_rows, "REQUEST_OBSERVED")
        request_received = event_time(group_rows, "REQUEST_RECEIVED")
        ack_admission = event_time(group_rows, "ACK_ADMISSION_CHECKED")
        ack_published = event_time(group_rows, "ACK_PUBLISHED")
        selection_prefetch = event_time(group_rows, "SELECTION_DIRECT_PREFETCH_DATA")
        selection_observed = event_time(group_rows, "SELECTION_OBSERVED")
        selection_received = event_time(group_rows, "SELECTION_RECEIVED")
        execution_started = event_time(group_rows, "EXECUTION_STARTED")
        execution_done = event_time(group_rows, "EXECUTION_DONE")
        response_published = event_time(group_rows, "RESPONSE_PUBLISHED")
        request_observed_epoch_s = event_epoch_s(group_rows, "REQUEST_OBSERVED")
        ack_published_epoch_s = event_epoch_s(group_rows, "ACK_PUBLISHED")
        selection_received_epoch_s = event_epoch_s(group_rows, "SELECTION_RECEIVED")
        response_published_epoch_s = event_epoch_s(group_rows, "RESPONSE_PUBLISHED")
        provider_rows.append({
            "requestId": request_id,
            "providerName": provider,
            "log": log,
            "requestObservedEpochS": request_observed_epoch_s,
            "ackPublishedEpochS": ack_published_epoch_s,
            "selectionReceivedEpochS": selection_received_epoch_s,
            "responsePublishedEpochS": response_published_epoch_s,
            "requestReceivedToObservedMs": delta_ms(request_observed, request_received),
            "requestObservedToAckAdmissionMs": delta_ms(ack_admission, request_observed),
            "ackAdmissionToAckPublishedMs": delta_ms(ack_published, ack_admission),
            "requestObservedToAckPublishedMs": delta_ms(ack_published, request_observed),
            "ackPublishedToSelectionPrefetchDataMs": delta_ms(selection_prefetch, ack_published),
            "selectionPrefetchDataToSelectionObservedMs": delta_ms(selection_observed, selection_prefetch),
            "ackPublishedToSelectionObservedMs": delta_ms(selection_observed, ack_published),
            "selectionObservedToSelectionReceivedMs": delta_ms(selection_received, selection_observed),
            "selectionReceivedToExecutionStartedMs": delta_ms(execution_started, selection_received),
            "executionStartedToDoneMs": delta_ms(execution_done, execution_started),
            "executionDoneToResponsePublishedMs": delta_ms(response_published, execution_done),
            "selectionReceivedToResponsePublishedMs": delta_ms(response_published, selection_received),
        })

    final_provider_rows = [
        row for row in provider_rows
        if row["selectionReceivedToResponsePublishedMs"] > 0
    ]
    summary = {
        "layout": layout,
        "count": len(rows),
        "userRequests": len(user_rows),
        "providerRequests": len(provider_rows),
        "finalProviderRequests": len(final_provider_rows),
        "user": {
            "requestPublishedToFirstAckObservedMs": summarize_numeric([
                row["requestPublishedToFirstAckObservedMs"] for row in user_rows
            ]),
            "firstAckObservedToAckMatchedMs": summarize_numeric([
                row["firstAckObservedToAckMatchedMs"] for row in user_rows
            ]),
            "ackMatchedToProviderSelectedMs": summarize_numeric([
                row["ackMatchedToProviderSelectedMs"] for row in user_rows
            ]),
            "providerSelectedToSelectionPublishedMs": summarize_numeric([
                row["providerSelectedToSelectionPublishedMs"] for row in user_rows
            ]),
            "selectionPublishedToSelectionDirectPutMs": summarize_numeric([
                row["selectionPublishedToSelectionDirectPutMs"] for row in user_rows
            ]),
            "selectionPublishedToResponseObservedMs": summarize_numeric([
                row["selectionPublishedToResponseObservedMs"] for row in user_rows
            ]),
            "responseObservedToDecryptedMs": summarize_numeric([
                row["responseObservedToDecryptedMs"] for row in user_rows
            ]),
            "responseDecryptedToCallbackMs": summarize_numeric([
                row["responseDecryptedToCallbackMs"] for row in user_rows
            ]),
            "requestPublishedToCallbackMs": summarize_numeric([
                row["requestPublishedToCallbackMs"] for row in user_rows
            ]),
        },
        "provider": {
            "requestObservedToAckPublishedMs": summarize_numeric([
                row["requestObservedToAckPublishedMs"] for row in provider_rows
            ]),
            "ackPublishedToSelectionPrefetchDataMs": summarize_numeric([
                row["ackPublishedToSelectionPrefetchDataMs"] for row in provider_rows
            ]),
            "selectionPrefetchDataToSelectionObservedMs": summarize_numeric([
                row["selectionPrefetchDataToSelectionObservedMs"] for row in provider_rows
            ]),
            "ackPublishedToSelectionObservedMs": summarize_numeric([
                row["ackPublishedToSelectionObservedMs"] for row in provider_rows
            ]),
            "selectionObservedToSelectionReceivedMs": summarize_numeric([
                row["selectionObservedToSelectionReceivedMs"] for row in provider_rows
            ]),
            "selectionReceivedToExecutionStartedMs": summarize_numeric([
                row["selectionReceivedToExecutionStartedMs"] for row in provider_rows
            ]),
            "executionStartedToDoneMs": summarize_numeric([
                row["executionStartedToDoneMs"] for row in provider_rows
            ]),
            "executionDoneToResponsePublishedMs": summarize_numeric([
                row["executionDoneToResponsePublishedMs"] for row in provider_rows
            ]),
            "selectionReceivedToResponsePublishedMs": summarize_numeric([
                row["selectionReceivedToResponsePublishedMs"] for row in provider_rows
            ]),
        },
        "finalProvider": {
            "requestObservedToAckPublishedMs": summarize_numeric([
                row["requestObservedToAckPublishedMs"] for row in final_provider_rows
            ]),
            "ackPublishedToSelectionObservedMs": summarize_numeric([
                row["ackPublishedToSelectionObservedMs"] for row in final_provider_rows
            ]),
            "selectionReceivedToExecutionStartedMs": summarize_numeric([
                row["selectionReceivedToExecutionStartedMs"] for row in final_provider_rows
            ]),
            "executionStartedToDoneMs": summarize_numeric([
                row["executionStartedToDoneMs"] for row in final_provider_rows
            ]),
            "executionDoneToResponsePublishedMs": summarize_numeric([
                row["executionDoneToResponsePublishedMs"] for row in final_provider_rows
            ]),
            "selectionReceivedToResponsePublishedMs": summarize_numeric([
                row["selectionReceivedToResponsePublishedMs"] for row in final_provider_rows
            ]),
        },
        "userRows": user_rows,
        "providerRows": provider_rows,
        "finalProviderRows": final_provider_rows,
    }
    path = OUT / "outer-control-waterfall-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_OUTER_CONTROL_WATERFALL "
        f"layout={layout} count={summary['count']} "
        f"user_request_to_first_ack_p50_ms="
        f"{summary['user']['requestPublishedToFirstAckObservedMs']['p50']:.2f} "
        f"user_ack_match_to_selection_p50_ms="
        f"{summary['user']['ackMatchedToProviderSelectedMs']['p50']:.2f} "
        f"user_selection_to_response_p50_ms="
        f"{summary['user']['selectionPublishedToResponseObservedMs']['p50']:.2f} "
        f"user_response_to_callback_p50_ms="
        f"{summary['user']['responseObservedToDecryptedMs']['p50'] + summary['user']['responseDecryptedToCallbackMs']['p50']:.2f} "
        f"provider_request_to_ack_p50_ms="
        f"{summary['provider']['requestObservedToAckPublishedMs']['p50']:.2f} "
        f"provider_ack_to_selection_observed_p50_ms="
        f"{summary['provider']['ackPublishedToSelectionObservedMs']['p50']:.2f} "
        f"final_provider_selection_to_exec_p50_ms="
        f"{summary['finalProvider']['selectionReceivedToExecutionStartedMs']['p50']:.2f} "
        f"final_provider_exec_to_response_p50_ms="
        f"{summary['finalProvider']['executionStartedToDoneMs']['p50'] + summary['finalProvider']['executionDoneToResponsePublishedMs']['p50']:.2f} "
        f"path={path}"
    )


def write_outer_control_rtt_correlation_summary(layout: str) -> None:
    monitor_path = OUT / "warm-rtt-nfd-monitor.json"
    control_path = OUT / "outer-control-waterfall-stats.json"
    if not monitor_path.exists() or not control_path.exists():
        return
    try:
        monitor_history = json.loads(monitor_path.read_text(encoding="utf-8"))
        control = json.loads(control_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return
    if not isinstance(monitor_history, list) or not monitor_history:
        return
    monitor = monitor_history[-1]
    monitor_rows = monitor.get("requestsWithNearestSample", [])
    control_rows = [
        row for row in control.get("userRows", [])
        if str(row.get("phase", "")).startswith("warm")
        and float(row.get("requestPublishedEpochS", 0.0) or 0.0) > 0.0
    ]
    if not monitor_rows or not control_rows:
        return

    def corr(a: list[float], b: list[float]) -> float:
        if len(a) != len(b) or len(a) < 2:
            return 0.0
        ma = sum(a) / len(a)
        mb = sum(b) / len(b)
        da = sum((value - ma) ** 2 for value in a)
        db = sum((value - mb) ** 2 for value in b)
        if da <= 0.0 or db <= 0.0:
            return 0.0
        return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / ((da * db) ** 0.5)

    def nearest_control(epoch_s: float | None) -> dict | None:
        if epoch_s is None:
            return None
        return min(
            control_rows,
            key=lambda row: abs(float(row.get("requestPublishedEpochS", 0.0)) - epoch_s),
        )

    rows = []
    for monitor_row in monitor_rows:
        request = monitor_row.get("request", {})
        start_epoch = request.get("epochStartS")
        control_row = nearest_control(float(start_epoch) if start_epoch is not None else None)
        sample = monitor_row.get("nearestSample") or {}
        rtt_values = [
            float(item["rttMs"])
            for item in sample.get("rtts", [])
            if item.get("rttMs") is not None
        ]
        nfd_totals = sample.get("nfdTotals") or {}
        if control_row is None:
            continue
        control_delta_ms = abs(
            float(control_row.get("requestPublishedEpochS", 0.0)) -
            float(start_epoch or 0.0)
        ) * 1000.0
        rows.append({
            "requestIndex": request.get("index"),
            "requestId": control_row.get("requestId", ""),
            "phase": control_row.get("phase", ""),
            "inferenceElapsedMs": float(request.get("inferenceElapsedMs", 0.0)),
            "requestEpochStartS": start_epoch,
            "requestEpochEndS": request.get("epochEndS"),
            "controlMatchDeltaMs": control_delta_ms,
            "nearestMonitorSampleDeltaMs": monitor_row.get("nearestSampleDeltaMs"),
            "rttMeanMs": (sum(rtt_values) / len(rtt_values)) if rtt_values else 0.0,
            "rttMaxMs": max(rtt_values) if rtt_values else 0.0,
            "nfdOutData": int(nfd_totals.get("nOutData", 0)),
            "nfdOutBytes": int(nfd_totals.get("nOutBytes", 0)),
            "requestPublishedToFirstAckObservedMs": float(
                control_row.get("requestPublishedToFirstAckObservedMs", 0.0) or 0.0),
            "firstAckObservedToAckMatchedMs": float(
                control_row.get("firstAckObservedToAckMatchedMs", 0.0) or 0.0),
            "ackMatchedToProviderSelectedMs": float(
                control_row.get("ackMatchedToProviderSelectedMs", 0.0) or 0.0),
            "providerSelectedToSelectionPublishedMs": float(
                control_row.get("providerSelectedToSelectionPublishedMs", 0.0) or 0.0),
            "selectionPublishedToResponseObservedMs": float(
                control_row.get("selectionPublishedToResponseObservedMs", 0.0) or 0.0),
            "responseObservedToDecryptedMs": float(
                control_row.get("responseObservedToDecryptedMs", 0.0) or 0.0),
            "responseDecryptedToCallbackMs": float(
                control_row.get("responseDecryptedToCallbackMs", 0.0) or 0.0),
            "requestPublishedToCallbackMs": float(
                control_row.get("requestPublishedToCallbackMs", 0.0) or 0.0),
        })
    if not rows:
        return

    latency = [row["inferenceElapsedMs"] for row in rows]
    fields = [
        "rttMeanMs",
        "rttMaxMs",
        "nfdOutData",
        "nfdOutBytes",
        "requestPublishedToFirstAckObservedMs",
        "firstAckObservedToAckMatchedMs",
        "ackMatchedToProviderSelectedMs",
        "providerSelectedToSelectionPublishedMs",
        "selectionPublishedToResponseObservedMs",
        "responseObservedToDecryptedMs",
        "responseDecryptedToCallbackMs",
        "requestPublishedToCallbackMs",
    ]
    correlations = {
        field: corr(latency, [float(row.get(field, 0.0) or 0.0) for row in rows])
        for field in fields
    }
    sorted_fields = sorted(
        correlations.items(), key=lambda item: abs(item[1]), reverse=True)
    summary = {
        "layout": layout,
        "count": len(rows),
        "latencyMs": summarize_numeric(latency),
        "controlMatchDeltaMs": summarize_numeric([
            row["controlMatchDeltaMs"] for row in rows
        ]),
        "nearestMonitorSampleDeltaMs": summarize_numeric([
            float(row.get("nearestMonitorSampleDeltaMs", 0.0) or 0.0)
            for row in rows
        ]),
        "correlationWithInferenceLatency": correlations,
        "strongestCorrelations": [
            {"field": field, "correlation": value}
            for field, value in sorted_fields
        ],
        "slowestRequests": sorted(
            rows, key=lambda row: row["inferenceElapsedMs"], reverse=True)[:20],
        "fastestRequests": sorted(
            rows, key=lambda row: row["inferenceElapsedMs"])[:10],
        "rows": rows,
        "note": (
            "Rows are aligned by request epoch timestamps. Use controlMatchDeltaMs "
            "and nearestMonitorSampleDeltaMs to judge alignment quality before "
            "drawing conclusions from correlations."
        ),
    }
    path = OUT / "outer-control-rtt-correlation-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    top = summary["strongestCorrelations"][:4]
    top_text = ",".join(
        f"{item['field']}={item['correlation']:.3f}" for item in top)
    print(
        "YOLO_LAYOUT_OUTER_CONTROL_RTT_CORRELATION "
        f"layout={layout} count={len(rows)} "
        f"latency_p50_ms={summary['latencyMs']['p50']:.2f} "
        f"latency_p95_ms={summary['latencyMs']['p95']:.2f} "
        f"top={top_text} path={path}"
    )


def write_native_session_breakdown_summary(layout: str) -> None:
    paths = {
        "correlation": OUT / "outer-control-rtt-correlation-stats.json",
        "waterfall": OUT / "outer-control-waterfall-stats.json",
        "handler": OUT / "provider-handler-timing-stats.json",
        "dependency": OUT / "dependency-input-timing-stats.json",
        "onnx": OUT / "onnx-timing-stats.json",
    }
    if not all(path.exists() for path in paths.values()):
        return
    try:
        correlation = json.loads(paths["correlation"].read_text(encoding="utf-8"))
        waterfall = json.loads(paths["waterfall"].read_text(encoding="utf-8"))
        handler = json.loads(paths["handler"].read_text(encoding="utf-8"))
        dependency = json.loads(paths["dependency"].read_text(encoding="utf-8"))
        onnx = json.loads(paths["onnx"].read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return

    request_rows = {
        str(row.get("requestId", "")): row
        for row in correlation.get("rows", [])
        if row.get("requestId")
    }
    user_rows_by_session = {
        str(row.get("requestId", "")): row
        for row in waterfall.get("userRows", [])
        if row.get("requestId")
    }
    provider_rows_by_session: dict[str, list[dict]] = {}
    for row in waterfall.get("providerRows", []):
        provider_rows_by_session.setdefault(str(row.get("requestId", "")), []).append(row)
    handler_rows_by_session: dict[str, list[dict]] = {}
    for row in handler.get("rows", []):
        if row.get("event") != "end":
            continue
        handler_rows_by_session.setdefault(str(row.get("session", "")), []).append(row)
    dependency_rows_by_session: dict[str, list[dict]] = {}
    for row in dependency.get("rows", []):
        dependency_rows_by_session.setdefault(str(row.get("session", "")), []).append(row)
    onnx_rows_by_session: dict[str, list[dict]] = {}
    for row in onnx.get("rows", []):
        onnx_rows_by_session.setdefault(str(row.get("session", "")), []).append(row)

    sessions = []
    for request_id, request in sorted(request_rows.items()):
        provider_rows = provider_rows_by_session.get(request_id, [])
        handler_rows = handler_rows_by_session.get(request_id, [])
        dependency_rows = dependency_rows_by_session.get(request_id, [])
        onnx_rows = onnx_rows_by_session.get(request_id, [])
        final_provider_rows = [
            row for row in provider_rows
            if float(row.get("selectionReceivedToResponsePublishedMs", 0.0) or 0.0) > 0.0
        ]
        final_handler_rows = [
            row for row in handler_rows
            if str(row.get("role", "")) == "/Merge"
        ]
        max_handler = max(
            handler_rows,
            key=lambda row: float(row.get("handler_ms", 0.0) or 0.0),
            default={},
        )
        max_dependency = max(
            dependency_rows,
            key=lambda row: float(row.get("fetch_ms", 0.0) or 0.0),
            default={},
        )
        max_selection_observed = max(
            provider_rows,
            key=lambda row: float(row.get("ackPublishedToSelectionObservedMs", 0.0) or 0.0),
            default={},
        )
        max_request_ack = max(
            provider_rows,
            key=lambda row: float(row.get("requestObservedToAckPublishedMs", 0.0) or 0.0),
            default={},
        )
        handler_total_max = max(
            [float(row.get("handler_ms", 0.0) or 0.0) for row in handler_rows],
            default=0.0,
        )
        dependency_fetch_max = max(
            [float(row.get("fetch_ms", 0.0) or 0.0) for row in dependency_rows],
            default=0.0,
        )
        final_provider_selection_to_response = max(
            [
                float(row.get("selectionReceivedToResponsePublishedMs", 0.0) or 0.0)
                for row in final_provider_rows
            ],
            default=0.0,
        )
        user_waterfall_row = user_rows_by_session.get(request_id, {})
        selection_published_epoch_s = float(
            user_waterfall_row.get("selectionPublishedEpochS", 0.0) or 0.0)
        response_observed_epoch_s = float(
            user_waterfall_row.get("responseObservedEpochS", 0.0) or 0.0)
        final_selection_received_epoch_s = min(
            [
                float(row.get("selectionReceivedEpochS", 0.0) or 0.0)
                for row in final_provider_rows
                if float(row.get("selectionReceivedEpochS", 0.0) or 0.0) > 0.0
            ],
            default=0.0,
        )
        final_response_published_epoch_s = min(
            [
                float(row.get("responsePublishedEpochS", 0.0) or 0.0)
                for row in final_provider_rows
                if float(row.get("responsePublishedEpochS", 0.0) or 0.0) > 0.0
            ],
            default=0.0,
        )
        def epoch_delta_ms(end_s: float, start_s: float) -> float:
            if end_s <= 0.0 or start_s <= 0.0 or end_s < start_s:
                return 0.0
            return (end_s - start_s) * 1000.0
        onnx_run_sum = sum(float(row.get("run_ms", 0.0) or 0.0) for row in onnx_rows)
        row = {
            "requestId": request_id,
            "requestIndex": request.get("requestIndex"),
            "inferenceElapsedMs": float(request.get("inferenceElapsedMs", 0.0) or 0.0),
            "requestPublishedToFirstAckObservedMs": float(
                request.get("requestPublishedToFirstAckObservedMs", 0.0) or 0.0),
            "ackMatchedToProviderSelectedMs": float(
                request.get("ackMatchedToProviderSelectedMs", 0.0) or 0.0),
            "selectionPublishedToResponseObservedMs": float(
                request.get("selectionPublishedToResponseObservedMs", 0.0) or 0.0),
            "rttMeanMs": float(request.get("rttMeanMs", 0.0) or 0.0),
            "rttMaxMs": float(request.get("rttMaxMs", 0.0) or 0.0),
            "nfdOutData": int(request.get("nfdOutData", 0) or 0),
            "providerRequestToAckMaxMs": max(
                [float(item.get("requestObservedToAckPublishedMs", 0.0) or 0.0)
                 for item in provider_rows],
                default=0.0,
            ),
            "providerAckToSelectionObservedMaxMs": max(
                [float(item.get("ackPublishedToSelectionObservedMs", 0.0) or 0.0)
                 for item in provider_rows],
                default=0.0,
            ),
            "providerExecutionMaxMs": max(
                [float(item.get("executionStartedToDoneMs", 0.0) or 0.0)
                 for item in provider_rows],
                default=0.0,
            ),
            "selectionPublishedToFinalProviderSelectionReceivedMs": epoch_delta_ms(
                final_selection_received_epoch_s, selection_published_epoch_s),
            "selectionPublishedToFinalProviderResponsePublishedMs": epoch_delta_ms(
                final_response_published_epoch_s, selection_published_epoch_s),
            "finalProviderSelectionToResponseMs": final_provider_selection_to_response,
            "finalProviderResponsePublishedToUserObservedMs": epoch_delta_ms(
                response_observed_epoch_s, final_response_published_epoch_s),
            "handlerMaxMs": handler_total_max,
            "handlerMaxRole": max_handler.get("role", ""),
            "handlerMaxProviderLog": max_handler.get("providerLog", ""),
            "finalMergeHandlerMs": max(
                [float(item.get("handler_ms", 0.0) or 0.0) for item in final_handler_rows],
                default=0.0,
            ),
            "dependencyFetchMaxMs": dependency_fetch_max,
            "dependencyFetchSumMs": sum(
                float(item.get("fetch_ms", 0.0) or 0.0) for item in dependency_rows),
            "dependencyFetchMaxRole": max_dependency.get("role", ""),
            "dependencyFetchMaxScope": max_dependency.get("scope", ""),
            "dependencyFetchMaxProviderLog": max_dependency.get("providerLog", ""),
            "selectionObservedMaxProviderLog": max_selection_observed.get("log", ""),
            "requestAckMaxProviderLog": max_request_ack.get("log", ""),
            "onnxRunSumMs": onnx_run_sum,
            "providerRows": provider_rows,
            "handlerRows": handler_rows,
            "dependencyRows": dependency_rows,
            "onnxRows": onnx_rows,
        }
        components = {
            "request_to_first_ack": row["requestPublishedToFirstAckObservedMs"],
            "ack_to_selected": row["ackMatchedToProviderSelectedMs"],
            "selection_to_response": row["selectionPublishedToResponseObservedMs"],
            "selection_to_final_provider": row[
                "selectionPublishedToFinalProviderSelectionReceivedMs"],
            "final_response_to_user": row[
                "finalProviderResponsePublishedToUserObservedMs"],
            "max_dependency_fetch": row["dependencyFetchMaxMs"],
            "max_handler": row["handlerMaxMs"],
            "final_provider_selection_to_response": row["finalProviderSelectionToResponseMs"],
        }
        row["dominantComponent"] = max(components.items(), key=lambda item: item[1])[0]
        row["dominantComponentMs"] = components[row["dominantComponent"]]
        sessions.append(row)

    if not sessions:
        return
    summary = {
        "layout": layout,
        "count": len(sessions),
        "latencyMs": summarize_numeric([
            row["inferenceElapsedMs"] for row in sessions
        ]),
        "providerAckToSelectionObservedMaxMs": summarize_numeric([
            row["providerAckToSelectionObservedMaxMs"] for row in sessions
        ]),
        "providerExecutionMaxMs": summarize_numeric([
            row["providerExecutionMaxMs"] for row in sessions
        ]),
        "selectionPublishedToFinalProviderSelectionReceivedMs": summarize_numeric([
            row["selectionPublishedToFinalProviderSelectionReceivedMs"]
            for row in sessions
        ]),
        "selectionPublishedToFinalProviderResponsePublishedMs": summarize_numeric([
            row["selectionPublishedToFinalProviderResponsePublishedMs"]
            for row in sessions
        ]),
        "finalProviderResponsePublishedToUserObservedMs": summarize_numeric([
            row["finalProviderResponsePublishedToUserObservedMs"]
            for row in sessions
        ]),
        "dependencyFetchMaxMs": summarize_numeric([
            row["dependencyFetchMaxMs"] for row in sessions
        ]),
        "dependencyFetchSumMs": summarize_numeric([
            row["dependencyFetchSumMs"] for row in sessions
        ]),
        "handlerMaxMs": summarize_numeric([
            row["handlerMaxMs"] for row in sessions
        ]),
        "dominantComponentCounts": {
            component: sum(1 for row in sessions if row["dominantComponent"] == component)
            for component in sorted({row["dominantComponent"] for row in sessions})
        },
        "slowestSessions": sorted(
            sessions, key=lambda row: row["inferenceElapsedMs"], reverse=True)[:20],
        "fastestSessions": sorted(
            sessions, key=lambda row: row["inferenceElapsedMs"])[:10],
        "sessions": sessions,
        "note": (
            "This table joins user outer-control timing with provider role, dependency, "
            "and ONNX timing by request/session id. It is intended to explain individual "
            "outliers rather than replace the low-overhead latency benchmark."
        ),
    }
    path = OUT / "native-session-breakdown-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    slow = summary["slowestSessions"][0]
    print(
        "YOLO_LAYOUT_NATIVE_SESSION_BREAKDOWN "
        f"layout={layout} count={len(sessions)} "
        f"latency_p50_ms={summary['latencyMs']['p50']:.2f} "
        f"latency_p95_ms={summary['latencyMs']['p95']:.2f} "
        f"dependency_fetch_max_p50_ms={summary['dependencyFetchMaxMs']['p50']:.2f} "
        f"provider_ack_to_selection_max_p50_ms="
        f"{summary['providerAckToSelectionObservedMaxMs']['p50']:.2f} "
        f"selection_to_final_provider_p50_ms="
        f"{summary['selectionPublishedToFinalProviderSelectionReceivedMs']['p50']:.2f} "
        f"final_response_to_user_p50_ms="
        f"{summary['finalProviderResponsePublishedToUserObservedMs']['p50']:.2f} "
        f"slowest_request={slow['requestId']} "
        f"slowest_latency_ms={slow['inferenceElapsedMs']:.2f} "
        f"slowest_dominant={slow['dominantComponent']} "
        f"slowest_dominant_ms={slow['dominantComponentMs']:.2f} "
        f"path={path}"
    )


def write_ack_selection_timing_summaries(layout: str,
                                         phases: list[tuple[str, Path]]) -> None:
    requests: dict[tuple[str, str], dict] = {}
    events_of_interest = {
        "COLLAB_REQUEST_CREATED",
        "REQUEST_PUBLISHED",
        "FIRST_ACK_OBSERVED",
        "ACK_DECRYPT_IN_FLIGHT",
        "ACK_DECRYPT_IN_FLIGHT_DONE",
        "ACK_RECEIVED",
        "ACK_MATCHED_PENDING_CALL",
        "ACK_SELECTION_COLLAB_ROLE_COVERAGE_CHECK",
        "ACK_SELECTION_EARLY_COLLAB_ROLE_COVERAGE",
        "ACK_SELECTION_EARLY_LEARNED_PROVIDERS",
        "ACK_SELECTION_BEGIN",
        "ACK_SELECTION_END",
        "PROVIDER_SELECTED",
        "CUSTOM_ACK_SELECTED",
        "SELECTION_PUBLISHED",
        "RESPONSE_RECEIVED",
        "CALLBACK_FIRED",
        "REQUEST_PENDING_COMPLETED",
        "REQUEST_PENDING_TIMEOUT",
    }

    for phase, path in phases:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "[NDNSF_TRACE]" not in line:
                continue
            row = parse_trace_row(line)
            event = row.get("event", "")
            request_id = row.get("requestId", "")
            if event not in events_of_interest or not request_id:
                continue
            key = (phase, request_id)
            item = requests.setdefault(key, {
                "phase": phase,
                "requestId": request_id,
                "log": path.name,
                "events": [],
                "acks": [],
                "ackDecryptStart": [],
                "ackDecryptDone": [],
                "ackReceived": [],
                "selections": [],
            })
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            row["timestamp_us"] = timestamp_us
            item["events"].append(row)
            if event == "ACK_DECRYPT_IN_FLIGHT":
                item["ackDecryptStart"].append(row)
            if event == "ACK_DECRYPT_IN_FLIGHT_DONE":
                item["ackDecryptDone"].append(row)
            if event == "ACK_RECEIVED":
                item["ackReceived"].append(row)
            if event == "ACK_MATCHED_PENDING_CALL":
                item["acks"].append(row)
            if event == "SELECTION_PUBLISHED":
                item["selections"].append(row)

    rows = []
    for item in requests.values():
        events = item["events"]

        def first_event(name: str):
            found = [row for row in events if row.get("event") == name]
            return min(found, key=lambda row: row.get("timestamp_us", 0)) if found else None

        def event_time(name: str) -> int:
            row = first_event(name)
            return int(row.get("timestamp_us", 0)) if row else 0

        publish_us = event_time("REQUEST_PUBLISHED")
        first_ack_us = event_time("FIRST_ACK_OBSERVED")
        ack_decrypt_start_times = [
            int(row.get("timestamp_us", 0))
            for row in item["ackDecryptStart"]
            if int(row.get("timestamp_us", 0)) > 0
        ]
        ack_decrypt_done_times = [
            int(row.get("timestamp_us", 0))
            for row in item["ackDecryptDone"]
            if int(row.get("timestamp_us", 0)) > 0
        ]
        ack_received_times = [
            int(row.get("timestamp_us", 0))
            for row in item["ackReceived"]
            if int(row.get("timestamp_us", 0)) > 0
        ]
        ack_times = [
            int(row.get("timestamp_us", 0))
            for row in item["acks"]
            if int(row.get("timestamp_us", 0)) > 0
        ]
        selection_begin_us = event_time("ACK_SELECTION_BEGIN")
        selection_end_us = event_time("ACK_SELECTION_END")
        first_selection_us = min(
            [int(row.get("timestamp_us", 0)) for row in item["selections"]]
            or [0]
        )
        response_us = event_time("RESPONSE_RECEIVED")
        callback_us = event_time("CALLBACK_FIRED")
        early_role_us = event_time("ACK_SELECTION_EARLY_COLLAB_ROLE_COVERAGE")
        early_learned_us = event_time("ACK_SELECTION_EARLY_LEARNED_PROVIDERS")
        last_ack_us = max(ack_times) if ack_times else 0
        first_ack_decrypt_start_us = min(ack_decrypt_start_times) if ack_decrypt_start_times else 0
        first_ack_decrypt_done_us = min(ack_decrypt_done_times) if ack_decrypt_done_times else 0
        first_ack_received_us = min(ack_received_times) if ack_received_times else 0
        last_ack_received_us = max(ack_received_times) if ack_received_times else 0

        def delta_ms(end_us: int, start_us: int) -> float:
            if end_us <= 0 or start_us <= 0 or end_us < start_us:
                return 0.0
            return (end_us - start_us) / 1000.0

        row = {
            "phase": item["phase"],
            "requestId": item["requestId"],
            "log": item["log"],
            "ackCount": len(item["acks"]),
            "ackDecryptStartCount": len(item["ackDecryptStart"]),
            "ackReceivedCount": len(item["ackReceived"]),
            "selectionCount": len(item["selections"]),
            "publishedToFirstAckDecryptStartMs": delta_ms(first_ack_decrypt_start_us, publish_us),
            "firstAckDecryptStartToDoneMs": delta_ms(first_ack_decrypt_done_us, first_ack_decrypt_start_us),
            "firstAckDecryptDoneToReceivedMs": delta_ms(first_ack_received_us, first_ack_decrypt_done_us),
            "publishedToFirstAckReceivedMs": delta_ms(first_ack_received_us, publish_us),
            "publishedToLastAckReceivedMs": delta_ms(last_ack_received_us, publish_us),
            "publishedToFirstAckMs": delta_ms(first_ack_us, publish_us),
            "publishedToLastAckMs": delta_ms(last_ack_us, publish_us),
            "lastAckToSelectionBeginMs": delta_ms(selection_begin_us, last_ack_us),
            "selectionBeginToFirstSelectionMs": delta_ms(first_selection_us, selection_begin_us),
            "selectionBeginToEndMs": delta_ms(selection_end_us, selection_begin_us),
            "firstSelectionToResponseMs": delta_ms(response_us, first_selection_us),
            "responseToCallbackMs": delta_ms(callback_us, response_us),
            "earlyRoleCoverage": early_role_us > 0,
            "earlyLearnedProviders": early_learned_us > 0,
            "events": events,
        }
        rows.append(row)

    summary = {
        "layout": layout,
        "count": len(rows),
        "ackCount": summarize_numeric([float(row["ackCount"]) for row in rows]),
        "ackDecryptStartCount": summarize_numeric([
            float(row["ackDecryptStartCount"]) for row in rows
        ]),
        "publishedToFirstAckDecryptStartMs": summarize_numeric([
            row["publishedToFirstAckDecryptStartMs"] for row in rows
        ]),
        "firstAckDecryptStartToDoneMs": summarize_numeric([
            row["firstAckDecryptStartToDoneMs"] for row in rows
        ]),
        "firstAckDecryptDoneToReceivedMs": summarize_numeric([
            row["firstAckDecryptDoneToReceivedMs"] for row in rows
        ]),
        "publishedToFirstAckReceivedMs": summarize_numeric([
            row["publishedToFirstAckReceivedMs"] for row in rows
        ]),
        "publishedToLastAckReceivedMs": summarize_numeric([
            row["publishedToLastAckReceivedMs"] for row in rows
        ]),
        "publishedToFirstAckMs": summarize_numeric([
            row["publishedToFirstAckMs"] for row in rows
        ]),
        "publishedToLastAckMs": summarize_numeric([
            row["publishedToLastAckMs"] for row in rows
        ]),
        "lastAckToSelectionBeginMs": summarize_numeric([
            row["lastAckToSelectionBeginMs"] for row in rows
        ]),
        "selectionBeginToFirstSelectionMs": summarize_numeric([
            row["selectionBeginToFirstSelectionMs"] for row in rows
        ]),
        "firstSelectionToResponseMs": summarize_numeric([
            row["firstSelectionToResponseMs"] for row in rows
        ]),
        "earlyRoleCoverageCount": sum(1 for row in rows if row["earlyRoleCoverage"]),
        "earlyLearnedProvidersCount": sum(1 for row in rows if row["earlyLearnedProviders"]),
        "rows": rows,
    }
    path = OUT / "ack-selection-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_ACK_SELECTION_TIMING "
        f"layout={layout} count={summary['count']} "
        f"ack_count_p50={summary['ackCount']['p50']:.0f} "
        f"published_to_first_ack_decrypt_start_p50_ms="
        f"{summary['publishedToFirstAckDecryptStartMs']['p50']:.2f} "
        f"first_ack_decrypt_start_to_done_p50_ms="
        f"{summary['firstAckDecryptStartToDoneMs']['p50']:.2f} "
        f"published_to_first_ack_received_p50_ms="
        f"{summary['publishedToFirstAckReceivedMs']['p50']:.2f} "
        f"published_to_first_ack_p50_ms={summary['publishedToFirstAckMs']['p50']:.2f} "
        f"published_to_last_ack_p50_ms={summary['publishedToLastAckMs']['p50']:.2f} "
        f"last_ack_to_selection_begin_p50_ms="
        f"{summary['lastAckToSelectionBeginMs']['p50']:.2f} "
        f"selection_begin_to_first_selection_p50_ms="
        f"{summary['selectionBeginToFirstSelectionMs']['p50']:.2f} "
        f"first_selection_to_response_p50_ms="
        f"{summary['firstSelectionToResponseMs']['p50']:.2f} "
        f"early_role_coverage={summary['earlyRoleCoverageCount']} "
        f"path={path}"
    )


def write_provider_selection_timing_summaries(layout: str,
                                              providers: list[tuple[str, str, list[str]]]) -> None:
    events_of_interest = {
        "SELECTION_DECRYPT_START",
        "SELECTION_DECRYPT_DONE",
        "SELECTION_RECEIVED",
        "PROVIDER_EXECUTE_START",
        "SELECTION_NO_PENDING",
        "SELECTION_REJECTED_PROVIDER_TOKEN",
        "SELECTION_DUPLICATE_DROPPED",
    }
    by_request_provider: dict[tuple[str, str], dict] = {}
    for _, name, _ in providers:
        path = OUT / f"{name}.log"
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "[NDNSF_TRACE]" not in line:
                continue
            row = parse_trace_row(line)
            event = row.get("event", "")
            request_id = row.get("requestId", "")
            provider = row.get("providerName", "")
            if event not in events_of_interest or not request_id:
                continue
            if not provider:
                provider = name
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            row["timestamp_us"] = timestamp_us
            row["providerLog"] = path.name
            key = (request_id, provider)
            item = by_request_provider.setdefault(key, {
                "requestId": request_id,
                "providerName": provider,
                "providerLog": path.name,
                "events": [],
            })
            item["events"].append(row)

    rows = []
    for item in by_request_provider.values():
        events = item["events"]

        def first_event(name: str):
            found = [row for row in events if row.get("event") == name]
            return min(found, key=lambda row: row.get("timestamp_us", 0)) if found else None

        def event_time(name: str) -> int:
            row = first_event(name)
            return int(row.get("timestamp_us", 0)) if row else 0

        def delta_ms(end_us: int, start_us: int) -> float:
            if end_us <= 0 or start_us <= 0 or end_us < start_us:
                return 0.0
            return (end_us - start_us) / 1000.0

        decrypt_start_us = event_time("SELECTION_DECRYPT_START")
        decrypt_done_us = event_time("SELECTION_DECRYPT_DONE")
        selection_received_us = event_time("SELECTION_RECEIVED")
        execute_start_us = event_time("PROVIDER_EXECUTE_START")
        rows.append({
            "requestId": item["requestId"],
            "providerName": item["providerName"],
            "providerLog": item["providerLog"],
            "decryptStartUs": decrypt_start_us,
            "decryptDoneUs": decrypt_done_us,
            "selectionReceivedUs": selection_received_us,
            "executeStartUs": execute_start_us,
            "decryptMs": delta_ms(decrypt_done_us, decrypt_start_us),
            "decryptDoneToSelectionReceivedMs": delta_ms(selection_received_us, decrypt_done_us),
            "selectionReceivedToExecuteStartMs": delta_ms(execute_start_us, selection_received_us),
            "decryptStartToExecuteStartMs": delta_ms(execute_start_us, decrypt_start_us),
            "duplicateDropped": any(row.get("event") == "SELECTION_DUPLICATE_DROPPED"
                                    for row in events),
            "noPending": any(row.get("event") == "SELECTION_NO_PENDING"
                             for row in events),
            "providerTokenRejected": any(
                row.get("event") == "SELECTION_REJECTED_PROVIDER_TOKEN"
                for row in events),
            "events": events,
        })

    summary = {
        "layout": layout,
        "count": len(rows),
        "decryptMs": summarize_numeric([row["decryptMs"] for row in rows]),
        "decryptDoneToSelectionReceivedMs": summarize_numeric([
            row["decryptDoneToSelectionReceivedMs"] for row in rows
        ]),
        "selectionReceivedToExecuteStartMs": summarize_numeric([
            row["selectionReceivedToExecuteStartMs"] for row in rows
        ]),
        "decryptStartToExecuteStartMs": summarize_numeric([
            row["decryptStartToExecuteStartMs"] for row in rows
        ]),
        "duplicateDropped": sum(1 for row in rows if row["duplicateDropped"]),
        "noPending": sum(1 for row in rows if row["noPending"]),
        "providerTokenRejected": sum(1 for row in rows if row["providerTokenRejected"]),
        "rows": rows,
    }
    path = OUT / "provider-selection-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_PROVIDER_SELECTION_TIMING "
        f"layout={layout} count={summary['count']} "
        f"decrypt_p50_ms={summary['decryptMs']['p50']:.2f} "
        f"decrypt_done_to_selection_received_p50_ms="
        f"{summary['decryptDoneToSelectionReceivedMs']['p50']:.2f} "
        f"selection_received_to_execute_start_p50_ms="
        f"{summary['selectionReceivedToExecuteStartMs']['p50']:.2f} "
        f"decrypt_start_to_execute_start_p50_ms="
        f"{summary['decryptStartToExecuteStartMs']['p50']:.2f} "
        f"duplicates={summary['duplicateDropped']} "
        f"no_pending={summary['noPending']} "
        f"path={path}"
    )


def write_provider_request_ack_timing_summaries(
        layout: str,
        providers: list[tuple[str, str, list[str]]]) -> None:
    events_of_interest = {
        "REQUEST_RECEIVED",
        "REQUEST_DECRYPT_DONE",
        "ACK_PUBLISHED",
    }
    by_request_provider: dict[tuple[str, str], dict] = {}
    for _, name, _ in providers:
        path = OUT / f"{name}.log"
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "[NDNSF_TRACE]" not in line:
                continue
            row = parse_trace_row(line)
            event = row.get("event", "")
            request_id = row.get("requestId", "")
            if event not in events_of_interest or not request_id:
                continue
            provider = row.get("providerName", "") or name
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            row["timestamp_us"] = timestamp_us
            row["providerLog"] = path.name
            key = (request_id, name)
            item = by_request_provider.setdefault(key, {
                "requestId": request_id,
                "providerName": provider,
                "providerLog": path.name,
                "events": [],
            })
            if row.get("providerName"):
                item["providerName"] = row["providerName"]
            item["events"].append(row)

    rows = []
    for item in by_request_provider.values():
        events = item["events"]

        def first_event(name: str):
            found = [row for row in events if row.get("event") == name]
            return min(found, key=lambda row: row.get("timestamp_us", 0)) if found else None

        def event_time(name: str) -> int:
            row = first_event(name)
            return int(row.get("timestamp_us", 0)) if row else 0

        def delta_ms(end_us: int, start_us: int) -> float:
            if end_us <= 0 or start_us <= 0 or end_us < start_us:
                return 0.0
            return (end_us - start_us) / 1000.0

        received_us = event_time("REQUEST_RECEIVED")
        decrypt_done_us = event_time("REQUEST_DECRYPT_DONE")
        ack_published_us = event_time("ACK_PUBLISHED")
        rows.append({
            "requestId": item["requestId"],
            "providerName": item["providerName"],
            "providerLog": item["providerLog"],
            "requestReceivedUs": received_us,
            "requestDecryptDoneUs": decrypt_done_us,
            "ackPublishedUs": ack_published_us,
            "requestReceivedToDecryptDoneMs": delta_ms(decrypt_done_us, received_us),
            "requestDecryptDoneToAckPublishedMs": delta_ms(ack_published_us, decrypt_done_us),
            "requestReceivedToAckPublishedMs": delta_ms(ack_published_us, received_us),
            "events": events,
        })

    summary = {
        "layout": layout,
        "count": len(rows),
        "requestReceivedToDecryptDoneMs": summarize_numeric([
            row["requestReceivedToDecryptDoneMs"] for row in rows
        ]),
        "requestDecryptDoneToAckPublishedMs": summarize_numeric([
            row["requestDecryptDoneToAckPublishedMs"] for row in rows
        ]),
        "requestReceivedToAckPublishedMs": summarize_numeric([
            row["requestReceivedToAckPublishedMs"] for row in rows
        ]),
        "rows": rows,
    }
    path = OUT / "provider-request-ack-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_PROVIDER_REQUEST_ACK_TIMING "
        f"layout={layout} count={summary['count']} "
        f"request_received_to_decrypt_done_p50_ms="
        f"{summary['requestReceivedToDecryptDoneMs']['p50']:.2f} "
        f"request_decrypt_done_to_ack_published_p50_ms="
        f"{summary['requestDecryptDoneToAckPublishedMs']['p50']:.2f} "
        f"request_received_to_ack_published_p50_ms="
        f"{summary['requestReceivedToAckPublishedMs']['p50']:.2f} "
        f"path={path}"
    )


def write_control_path_timing_summaries(
        layout: str,
        phases: list[tuple[str, Path]],
        providers: list[tuple[str, str, list[str]]]) -> None:
    user_events: dict[str, dict] = {}
    for phase, path in phases:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if "[NDNSF_TRACE]" not in line:
                continue
            row = parse_trace_row(line)
            event = row.get("event", "")
            request_id = row.get("requestId", "")
            if event == "SVS_PUBLISH_BEGIN":
                request_id = request_id_from_message_name(row.get("messageName", ""))
            if not request_id:
                continue
            item = user_events.setdefault(request_id, {
                "phase": phase,
                "requestId": request_id,
                "requestPublishedUs": 0,
                "requestSvsPublishBeginUs": 0,
                "ackPreDecrypt": {},
                "ackReceived": {},
            })
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            if event == "REQUEST_PUBLISHED":
                item["requestPublishedUs"] = min(
                    [value for value in (item["requestPublishedUs"], timestamp_us)
                     if value > 0] or [timestamp_us])
            elif event == "SVS_PUBLISH_BEGIN" and "/NDNSF/REQUEST/" in row.get("messageName", ""):
                item["requestSvsPublishBeginUs"] = min(
                    [value for value in (item["requestSvsPublishBeginUs"], timestamp_us)
                     if value > 0] or [timestamp_us])
            elif event == "ACK_MATCH_ATTEMPT" and row.get("phase") == "pre_decrypt":
                provider = row.get("providerName", "")
                if provider:
                    current = item["ackPreDecrypt"].get(provider, 0)
                    item["ackPreDecrypt"][provider] = min(
                        [value for value in (current, timestamp_us) if value > 0]
                        or [timestamp_us])
            elif event == "ACK_RECEIVED":
                provider = row.get("providerName", "")
                if provider:
                    current = item["ackReceived"].get(provider, 0)
                    item["ackReceived"][provider] = min(
                        [value for value in (current, timestamp_us) if value > 0]
                        or [timestamp_us])

    rows = []
    for _, name, _ in providers:
        path = OUT / f"{name}.log"
        if not path.exists():
            continue
        provider_events: dict[str, dict] = {}
        for line in path.read_text(errors="replace").splitlines():
            if "[NDNSF_TRACE]" not in line:
                continue
            row = parse_trace_row(line)
            event = row.get("event", "")
            request_id = row.get("requestId", "")
            if event == "SVS_PUBLISH_BEGIN":
                request_id = request_id_from_message_name(row.get("messageName", ""))
            if not request_id:
                continue
            item = provider_events.setdefault(request_id, {
                "requestReceivedUs": 0,
                "requestDecryptDoneUs": 0,
                "ackPublishedUs": 0,
                "ackSvsPublishBeginUs": 0,
                "providerName": row.get("providerName", "") or name,
            })
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            if row.get("providerName"):
                item["providerName"] = row["providerName"]
            if event == "REQUEST_RECEIVED":
                current = item["requestReceivedUs"]
                item["requestReceivedUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif event == "REQUEST_DECRYPT_DONE":
                current = item["requestDecryptDoneUs"]
                item["requestDecryptDoneUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif event == "ACK_PUBLISHED":
                current = item["ackPublishedUs"]
                item["ackPublishedUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif (event == "SVS_PUBLISH_BEGIN" and
                  "/NDNSF/ACK/" in row.get("messageName", "")):
                current = item["ackSvsPublishBeginUs"]
                item["ackSvsPublishBeginUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])

        for request_id, provider_event in provider_events.items():
            user = user_events.get(request_id, {})
            provider_name = provider_event.get("providerName", "")
            published_us = int(user.get("requestPublishedUs", 0) or 0)
            request_svs_us = int(user.get("requestSvsPublishBeginUs", 0) or 0)
            received_us = int(provider_event.get("requestReceivedUs", 0) or 0)
            decrypt_done_us = int(provider_event.get("requestDecryptDoneUs", 0) or 0)
            ack_published_us = int(provider_event.get("ackPublishedUs", 0) or 0)
            ack_svs_us = int(provider_event.get("ackSvsPublishBeginUs", 0) or 0)
            ack_pre_decrypt_us = int(user.get("ackPreDecrypt", {}).get(provider_name, 0) or 0)
            ack_received_us = int(user.get("ackReceived", {}).get(provider_name, 0) or 0)

            def delta_ms(end_us: int, start_us: int) -> float:
                if end_us <= 0 or start_us <= 0 or end_us < start_us:
                    return 0.0
                return (end_us - start_us) / 1000.0

            rows.append({
                "phase": user.get("phase", ""),
                "requestId": request_id,
                "providerLog": path.name,
                "providerName": provider_name,
                "requestPublishedUs": published_us,
                "requestSvsPublishBeginUs": request_svs_us,
                "providerRequestReceivedUs": received_us,
                "providerRequestDecryptDoneUs": decrypt_done_us,
                "providerAckPublishedUs": ack_published_us,
                "providerAckSvsPublishBeginUs": ack_svs_us,
                "userAckPreDecryptUs": ack_pre_decrypt_us,
                "userAckReceivedUs": ack_received_us,
                "requestPublishedToRequestSvsPublishBeginMs": delta_ms(request_svs_us, published_us),
                "requestPublishedToProviderReceivedMs": delta_ms(received_us, published_us),
                "providerReceivedToRequestDecryptDoneMs": delta_ms(decrypt_done_us, received_us),
                "requestDecryptDoneToAckPublishedMs": delta_ms(ack_published_us, decrypt_done_us),
                "ackPublishedToAckSvsPublishBeginMs": delta_ms(ack_svs_us, ack_published_us),
                "ackPublishedToUserPreDecryptMs": delta_ms(ack_pre_decrypt_us, ack_published_us),
                "userPreDecryptToAckReceivedMs": delta_ms(ack_received_us, ack_pre_decrypt_us),
                "requestPublishedToAckReceivedMs": delta_ms(ack_received_us, published_us),
            })

    summary = {
        "layout": layout,
        "count": len(rows),
        "requestPublishedToRequestSvsPublishBeginMs": summarize_numeric([
            row["requestPublishedToRequestSvsPublishBeginMs"] for row in rows
        ]),
        "requestPublishedToProviderReceivedMs": summarize_numeric([
            row["requestPublishedToProviderReceivedMs"] for row in rows
        ]),
        "providerReceivedToRequestDecryptDoneMs": summarize_numeric([
            row["providerReceivedToRequestDecryptDoneMs"] for row in rows
        ]),
        "requestDecryptDoneToAckPublishedMs": summarize_numeric([
            row["requestDecryptDoneToAckPublishedMs"] for row in rows
        ]),
        "ackPublishedToAckSvsPublishBeginMs": summarize_numeric([
            row["ackPublishedToAckSvsPublishBeginMs"] for row in rows
        ]),
        "ackPublishedToUserPreDecryptMs": summarize_numeric([
            row["ackPublishedToUserPreDecryptMs"] for row in rows
        ]),
        "userPreDecryptToAckReceivedMs": summarize_numeric([
            row["userPreDecryptToAckReceivedMs"] for row in rows
        ]),
        "requestPublishedToAckReceivedMs": summarize_numeric([
            row["requestPublishedToAckReceivedMs"] for row in rows
        ]),
        "rows": rows,
    }
    path = OUT / "control-path-timing-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_CONTROL_PATH_TIMING "
        f"layout={layout} count={summary['count']} "
        f"request_publish_to_provider_received_p50_ms="
        f"{summary['requestPublishedToProviderReceivedMs']['p50']:.2f} "
        f"provider_received_to_request_decrypt_done_p50_ms="
        f"{summary['providerReceivedToRequestDecryptDoneMs']['p50']:.2f} "
        f"ack_published_to_user_pre_decrypt_p50_ms="
        f"{summary['ackPublishedToUserPreDecryptMs']['p50']:.2f} "
        f"user_pre_decrypt_to_ack_received_p50_ms="
        f"{summary['userPreDecryptToAckReceivedMs']['p50']:.2f} "
        f"request_publish_to_ack_received_p50_ms="
        f"{summary['requestPublishedToAckReceivedMs']['p50']:.2f} "
        f"path={path}"
    )


def write_svs_control_propagation_summaries(
        layout: str,
        phases: list[tuple[str, Path]],
        providers: list[tuple[str, str, list[str]]]) -> None:
    provider_names = {
        name: provider_identity(argv[argv.index("--provider-id") + 1])
        for _, name, argv in providers
        if "--provider-id" in argv
    }
    user_events: dict[str, dict] = {}
    for phase, path in phases:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            row = parse_timing_or_trace_row(line)
            if not row:
                continue
            event = row.get("event", "")
            message_name = row.get("messageName", "")
            request_id = row.get("requestId", "")
            if event == "SVS_PUBLISH_BEGIN":
                request_id = request_id_from_message_name(message_name)
            if not request_id:
                continue
            item = user_events.setdefault(request_id, {
                "phase": phase,
                "requestId": request_id,
                "requestSvsBeginUs": 0,
                "selectionSvsBeginUs": 0,
                "selectionSvsBeginByProvider": {},
                "ackPreDecryptByProvider": {},
                "responseObservedByProvider": {},
            })
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            if event == "SVS_PUBLISH_BEGIN" and "/NDNSF/REQUEST/" in message_name:
                current = int(item.get("requestSvsBeginUs", 0) or 0)
                item["requestSvsBeginUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif event == "SVS_PUBLISH_BEGIN" and "/NDNSF/SELECTION/" in message_name:
                provider = row.get("providerName", "") or selection_provider_from_message_name(message_name)
                if provider and not provider.startswith("/"):
                    provider = ""
                if provider:
                    current = int(item["selectionSvsBeginByProvider"].get(provider, 0) or 0)
                    item["selectionSvsBeginByProvider"][provider] = min(
                        [value for value in (current, timestamp_us) if value > 0]
                        or [timestamp_us])
                else:
                    current = int(item.get("selectionSvsBeginUs", 0) or 0)
                    item["selectionSvsBeginUs"] = min(
                        [value for value in (current, timestamp_us) if value > 0]
                        or [timestamp_us])
            elif ((event == "ACK_MATCH_ATTEMPT" and row.get("phase") == "pre_decrypt") or
                  event == "ACK_OBSERVED"):
                provider = row.get("providerName", "")
                if provider:
                    current = int(item["ackPreDecryptByProvider"].get(provider, 0) or 0)
                    item["ackPreDecryptByProvider"][provider] = min(
                        [value for value in (current, timestamp_us) if value > 0]
                        or [timestamp_us])
            elif event == "RESPONSE_OBSERVED":
                provider = row.get("providerName", "")
                if provider:
                    current = int(item["responseObservedByProvider"].get(provider, 0) or 0)
                    item["responseObservedByProvider"][provider] = min(
                        [value for value in (current, timestamp_us) if value > 0]
                        or [timestamp_us])

    provider_events: dict[tuple[str, str], dict] = {}
    for _, name, _ in providers:
        path = OUT / f"{name}.log"
        if not path.exists():
            continue
        default_provider = provider_names.get(name, name)
        for line in path.read_text(errors="replace").splitlines():
            row = parse_timing_or_trace_row(line)
            if not row:
                continue
            event = row.get("event", "")
            message_name = row.get("messageName", "")
            request_id = row.get("requestId", "")
            if event == "SVS_PUBLISH_BEGIN":
                request_id = request_id_from_message_name(message_name)
            if not request_id:
                continue
            provider = row.get("providerName", "") or default_provider
            item = provider_events.setdefault((request_id, provider), {
                "requestId": request_id,
                "providerName": provider,
                "providerLog": path.name,
                "requestReceivedUs": 0,
                "ackSvsBeginUs": 0,
                "selectionReceivedUs": 0,
                "responseSvsBeginUs": 0,
            })
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            if event == "REQUEST_RECEIVED":
                current = int(item.get("requestReceivedUs", 0) or 0)
                item["requestReceivedUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif event == "SVS_PUBLISH_BEGIN" and "/NDNSF/ACK/" in message_name:
                current = int(item.get("ackSvsBeginUs", 0) or 0)
                item["ackSvsBeginUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif event in {"SELECTION_OBSERVED", "SELECTION_RECEIVED"}:
                current = int(item.get("selectionReceivedUs", 0) or 0)
                item["selectionReceivedUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            elif event == "SVS_PUBLISH_BEGIN" and "/NDNSF/RESPONSE/" in message_name:
                current = int(item.get("responseSvsBeginUs", 0) or 0)
                item["responseSvsBeginUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])

    def delta_ms(end_us: int, start_us: int) -> float:
        if end_us <= 0 or start_us <= 0 or end_us < start_us:
            return 0.0
        return (end_us - start_us) / 1000.0

    rows = []
    for (request_id, provider), pitem in sorted(provider_events.items()):
        uitem = user_events.get(request_id, {})
        request_svs_us = int(uitem.get("requestSvsBeginUs", 0) or 0)
        selection_svs_us = int(
            uitem.get("selectionSvsBeginByProvider", {}).get(provider, 0) or 0)
        if selection_svs_us <= 0:
            selection_svs_us = int(uitem.get("selectionSvsBeginUs", 0) or 0)
        ack_pre_us = int(
            uitem.get("ackPreDecryptByProvider", {}).get(provider, 0) or 0)
        response_observed_us = int(
            uitem.get("responseObservedByProvider", {}).get(provider, 0) or 0)
        row = {
            "phase": uitem.get("phase", ""),
            "requestId": request_id,
            "providerName": provider,
            "providerLog": pitem.get("providerLog", ""),
            "requestSvsBeginUs": request_svs_us,
            "requestReceivedUs": int(pitem.get("requestReceivedUs", 0) or 0),
            "ackSvsBeginUs": int(pitem.get("ackSvsBeginUs", 0) or 0),
            "ackPreDecryptUs": ack_pre_us,
            "selectionSvsBeginUs": selection_svs_us,
            "selectionReceivedUs": int(pitem.get("selectionReceivedUs", 0) or 0),
            "responseSvsBeginUs": int(pitem.get("responseSvsBeginUs", 0) or 0),
            "responseObservedUs": response_observed_us,
        }
        row["requestSvsToProviderRequestReceivedMs"] = delta_ms(
            row["requestReceivedUs"], row["requestSvsBeginUs"])
        row["ackSvsToUserPreDecryptMs"] = delta_ms(
            row["ackPreDecryptUs"], row["ackSvsBeginUs"])
        row["selectionSvsToProviderSelectionReceivedMs"] = delta_ms(
            row["selectionReceivedUs"], row["selectionSvsBeginUs"])
        row["responseSvsToUserObservedMs"] = delta_ms(
            row["responseObservedUs"], row["responseSvsBeginUs"])
        rows.append(row)

    final_response_rows = [row for row in rows if row["responseSvsToUserObservedMs"] > 0]

    def summarize_rows(group_rows: list[dict]) -> dict:
        return {
            "count": len(group_rows),
            "requestSvsToProviderRequestReceivedMs": summarize_numeric([
                row["requestSvsToProviderRequestReceivedMs"]
                for row in group_rows if row["requestSvsToProviderRequestReceivedMs"] > 0
            ]),
            "ackSvsToUserPreDecryptMs": summarize_numeric([
                row["ackSvsToUserPreDecryptMs"]
                for row in group_rows if row["ackSvsToUserPreDecryptMs"] > 0
            ]),
            "selectionSvsToProviderSelectionReceivedMs": summarize_numeric([
                row["selectionSvsToProviderSelectionReceivedMs"]
                for row in group_rows if row["selectionSvsToProviderSelectionReceivedMs"] > 0
            ]),
            "responseSvsToUserObservedMs": summarize_numeric([
                row["responseSvsToUserObservedMs"]
                for row in group_rows if row["responseSvsToUserObservedMs"] > 0
            ]),
        }

    phase_summaries = {
        phase: summarize_rows([row for row in rows if row.get("phase") == phase])
        for phase in sorted({str(row.get("phase", "")) for row in rows if row.get("phase")})
    }
    summary = {
        "layout": layout,
        "count": len(rows),
        "finalResponseCount": len(final_response_rows),
        "requestSvsToProviderRequestReceivedMs": summarize_numeric([
            row["requestSvsToProviderRequestReceivedMs"]
            for row in rows if row["requestSvsToProviderRequestReceivedMs"] > 0
        ]),
        "ackSvsToUserPreDecryptMs": summarize_numeric([
            row["ackSvsToUserPreDecryptMs"]
            for row in rows if row["ackSvsToUserPreDecryptMs"] > 0
        ]),
        "selectionSvsToProviderSelectionReceivedMs": summarize_numeric([
            row["selectionSvsToProviderSelectionReceivedMs"]
            for row in rows if row["selectionSvsToProviderSelectionReceivedMs"] > 0
        ]),
        "responseSvsToUserObservedMs": summarize_numeric([
            row["responseSvsToUserObservedMs"]
            for row in final_response_rows
        ]),
        "phases": phase_summaries,
        "rows": rows,
    }
    path = OUT / "svs-control-propagation-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_SVS_CONTROL_PROPAGATION "
        f"layout={layout} count={summary['count']} "
        f"request_svs_to_provider_p50_ms="
        f"{summary['requestSvsToProviderRequestReceivedMs']['p50']:.2f} "
        f"ack_svs_to_user_p50_ms="
        f"{summary['ackSvsToUserPreDecryptMs']['p50']:.2f} "
        f"selection_svs_to_provider_p50_ms="
        f"{summary['selectionSvsToProviderSelectionReceivedMs']['p50']:.2f} "
        f"response_svs_to_user_p50_ms="
        f"{summary['responseSvsToUserObservedMs']['p50']:.2f} "
        f"path={path}"
    )


def write_selection_direct_prefetch_summaries(
        layout: str,
        phases: list[tuple[str, Path]],
        providers: list[tuple[str, str, list[str]]]) -> None:
    user_puts: dict[str, dict] = {}
    for phase, path in phases:
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            row = parse_timing_or_trace_row(line)
            if not row or row.get("event") != "SELECTION_DIRECT_PUT":
                continue
            request_id = row.get("requestId", "")
            if not request_id:
                continue
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            steady_us = parse_int_prefix(row.get("steady_us", "0"))
            item = user_puts.setdefault(request_id, {
                "phase": phase,
                "requestId": request_id,
                "putTimestampUs": 0,
                "putSteadyUs": 0,
                "messageName": row.get("messageName", ""),
                "contentBytes": parse_int_prefix(row.get("contentBytes", "0")),
            })
            if timestamp_us > 0:
                current = int(item.get("putTimestampUs", 0) or 0)
                item["putTimestampUs"] = min(
                    [value for value in (current, timestamp_us) if value > 0]
                    or [timestamp_us])
            if steady_us > 0:
                current = int(item.get("putSteadyUs", 0) or 0)
                item["putSteadyUs"] = min(
                    [value for value in (current, steady_us) if value > 0]
                    or [steady_us])

    provider_rows: dict[tuple[str, str], dict] = {}
    prefetch_issued = 0
    prefetch_timeouts = 0
    prefetch_nacks = 0
    for _, name, _ in providers:
        path = OUT / f"{name}.log"
        if not path.exists():
            continue
        for line in path.read_text(errors="replace").splitlines():
            row = parse_timing_or_trace_row(line)
            if not row:
                continue
            event = row.get("event", "")
            if not event.startswith("SELECTION_DIRECT_PREFETCH") and event not in {
                    "SELECTION_OBSERVED", "SELECTION_RECEIVED"}:
                continue
            request_id = row.get("requestId", "")
            provider = row.get("providerName", "") or name
            if not request_id or not provider:
                continue
            if event == "SELECTION_DIRECT_PREFETCH_ISSUED":
                prefetch_issued += 1
            elif event == "SELECTION_DIRECT_PREFETCH_TIMEOUT":
                prefetch_timeouts += 1
            elif event == "SELECTION_DIRECT_PREFETCH_NACK":
                prefetch_nacks += 1
            timestamp_us = parse_int_prefix(row.get("timestamp_us", "0"))
            steady_us = parse_int_prefix(row.get("steady_us", "0"))
            item = provider_rows.setdefault((request_id, provider), {
                "requestId": request_id,
                "providerName": provider,
                "providerLog": path.name,
                "prefetchIssuedTimestampUs": 0,
                "prefetchIssuedSteadyUs": 0,
                "prefetchDataTimestampUs": 0,
                "prefetchDataSteadyUs": 0,
                "selectionObservedTimestampUs": 0,
                "selectionObservedSteadyUs": 0,
                "contentBytes": 0,
            })
            if event == "SELECTION_DIRECT_PREFETCH_ISSUED":
                for field, value in (
                        ("prefetchIssuedTimestampUs", timestamp_us),
                        ("prefetchIssuedSteadyUs", steady_us)):
                    if value <= 0:
                        continue
                    current = int(item.get(field, 0) or 0)
                    item[field] = min([v for v in (current, value) if v > 0] or [value])
            elif event == "SELECTION_DIRECT_PREFETCH_DATA":
                for field, value in (
                        ("prefetchDataTimestampUs", timestamp_us),
                        ("prefetchDataSteadyUs", steady_us)):
                    if value <= 0:
                        continue
                    current = int(item.get(field, 0) or 0)
                    item[field] = min([v for v in (current, value) if v > 0] or [value])
                item["contentBytes"] = parse_int_prefix(row.get("contentBytes", "0"))
            elif event in {"SELECTION_OBSERVED", "SELECTION_RECEIVED"}:
                for field, value in (
                        ("selectionObservedTimestampUs", timestamp_us),
                        ("selectionObservedSteadyUs", steady_us)):
                    if value <= 0:
                        continue
                    current = int(item.get(field, 0) or 0)
                    item[field] = min([v for v in (current, value) if v > 0] or [value])

    def delta_ms(end_us: int, start_us: int) -> float:
        if end_us <= 0 or start_us <= 0 or end_us < start_us:
            return 0.0
        return (end_us - start_us) / 1000.0

    rows = []
    for (request_id, provider), pitem in sorted(provider_rows.items()):
        uitem = user_puts.get(request_id, {})
        row = {
            "phase": uitem.get("phase", ""),
            "requestId": request_id,
            "providerName": provider,
            "providerLog": pitem.get("providerLog", ""),
            "putTimestampUs": int(uitem.get("putTimestampUs", 0) or 0),
            "putSteadyUs": int(uitem.get("putSteadyUs", 0) or 0),
            "prefetchIssuedTimestampUs": int(pitem.get("prefetchIssuedTimestampUs", 0) or 0),
            "prefetchDataTimestampUs": int(pitem.get("prefetchDataTimestampUs", 0) or 0),
            "selectionObservedTimestampUs": int(pitem.get("selectionObservedTimestampUs", 0) or 0),
            "messageName": uitem.get("messageName", ""),
            "contentBytes": int(pitem.get("contentBytes", 0) or uitem.get("contentBytes", 0) or 0),
        }
        row["prefetchIssuedToDirectDataMs"] = delta_ms(
            int(pitem.get("prefetchDataTimestampUs", 0) or 0),
            int(pitem.get("prefetchIssuedTimestampUs", 0) or 0))
        row["directPutToDirectDataMs"] = delta_ms(
            int(pitem.get("prefetchDataTimestampUs", 0) or 0),
            int(uitem.get("putTimestampUs", 0) or 0))
        row["directDataToSelectionObservedMs"] = delta_ms(
            int(pitem.get("selectionObservedTimestampUs", 0) or 0),
            int(pitem.get("prefetchDataTimestampUs", 0) or 0))
        row["directPutToSelectionObservedMs"] = delta_ms(
            int(pitem.get("selectionObservedTimestampUs", 0) or 0),
            int(uitem.get("putTimestampUs", 0) or 0))
        rows.append(row)

    matched_rows = [row for row in rows if row["directPutToDirectDataMs"] > 0]
    summary = {
        "layout": layout,
        "requestPutCount": len(user_puts),
        "providerRowCount": len(rows),
        "matchedProviderRowCount": len(matched_rows),
        "prefetchIssuedCount": prefetch_issued,
        "prefetchTimeoutCount": prefetch_timeouts,
        "prefetchNackCount": prefetch_nacks,
        "prefetchIssuedToDirectDataMs": summarize_numeric([
            row["prefetchIssuedToDirectDataMs"]
            for row in rows if row["prefetchIssuedToDirectDataMs"] > 0
        ]),
        "directPutToDirectDataMs": summarize_numeric([
            row["directPutToDirectDataMs"]
            for row in rows if row["directPutToDirectDataMs"] > 0
        ]),
        "directDataToSelectionObservedMs": summarize_numeric([
            row["directDataToSelectionObservedMs"]
            for row in rows if row["directDataToSelectionObservedMs"] > 0
        ]),
        "directPutToSelectionObservedMs": summarize_numeric([
            row["directPutToSelectionObservedMs"]
            for row in rows if row["directPutToSelectionObservedMs"] > 0
        ]),
        "contentBytes": summarize_numeric([
            row["contentBytes"] for row in rows if row["contentBytes"] > 0
        ]),
        "rows": rows,
    }
    path = OUT / "selection-direct-prefetch-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_SELECTION_DIRECT_PREFETCH "
        f"layout={layout} request_puts={summary['requestPutCount']} "
        f"matched_provider_rows={summary['matchedProviderRowCount']} "
        f"issued={summary['prefetchIssuedCount']} "
        f"timeouts={summary['prefetchTimeoutCount']} "
        f"nacks={summary['prefetchNackCount']} "
        f"put_to_data_p50_ms={summary['directPutToDirectDataMs']['p50']:.2f} "
        f"data_to_selection_observed_p50_ms="
        f"{summary['directDataToSelectionObservedMs']['p50']:.2f} "
        f"put_to_selection_observed_p50_ms="
        f"{summary['directPutToSelectionObservedMs']['p50']:.2f} "
        f"path={path}"
    )


def _load_json_file(path: Path, default):
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return default


def write_execution_frontier_summary(layout: str, policy_path: Path) -> None:
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError("PyYAML is required to summarize DI execution frontiers") from exc

    if not policy_path.exists():
        raise RuntimeError(f"missing DI policy for frontier summary: {policy_path}")
    config = yaml.safe_load(policy_path.read_text(encoding="utf-8")) or {}
    service = None
    for candidate in config.get("services", []) or []:
        if candidate.get("roles") and candidate.get("dependencies") is not None:
            service = candidate
            break
    collaboration = config.get("collaboration") or {}
    source = service if service is not None else collaboration
    roles = [str(role) for role in source.get("roles", [])]
    dependencies = source.get("dependencies", []) or []

    input_scopes_by_role: dict[str, set[str]] = {role: set() for role in roles}
    output_scopes_by_role: dict[str, set[str]] = {role: set() for role in roles}
    edge_rows = []
    for index, dep in enumerate(dependencies):
        producers = [str(item) for item in dep.get("producers", [])]
        consumers = [str(item) for item in dep.get("consumers", [])]
        scope = str(dep.get("key_scope", dep.get("keyScope", f"edge-{index}")))
        for producer in producers:
            output_scopes_by_role.setdefault(producer, set()).add(scope)
        for consumer in consumers:
            input_scopes_by_role.setdefault(consumer, set()).add(scope)
        edge_rows.append({
            "index": index,
            "scope": scope,
            "producers": producers,
            "consumers": consumers,
            "tensors": list(dep.get("tensors", []) or []),
            "expectedSegments": int(dep.get("expected_segments",
                                            dep.get("expectedSegments", 0)) or 0),
            "expectedBytes": int(dep.get("expected_bytes",
                                         dep.get("expectedBytes", 0)) or 0),
        })

    produced_scopes: set[str] = set()
    remaining = set(roles)
    frontier_rows = []
    while remaining:
        ready = sorted([
            role for role in remaining
            if input_scopes_by_role.get(role, set()).issubset(produced_scopes)
        ])
        if not ready:
            raise RuntimeError(
                "DI policy dependency graph has no ready frontier; "
                f"remaining={sorted(remaining)} produced_scopes={sorted(produced_scopes)}")
        frontier_rows.append({
            "index": len(frontier_rows),
            "roles": ready,
            "parallelWidth": len(ready),
            "inputScopes": {
                role: sorted(input_scopes_by_role.get(role, set()))
                for role in ready
            },
            "outputScopes": {
                role: sorted(output_scopes_by_role.get(role, set()))
                for role in ready
            },
        })
        for role in ready:
            remaining.remove(role)
            produced_scopes.update(output_scopes_by_role.get(role, set()))

    max_parallel_width = max([row["parallelWidth"] for row in frontier_rows] or [0])
    summary = {
        "layout": layout,
        "policy": str(policy_path),
        "service": str(source.get("name", "")),
        "roleCount": len(roles),
        "edgeCount": len(edge_rows),
        "maxParallelWidth": max_parallel_width,
        "hasParallelFrontier": max_parallel_width > 1,
        "frontiers": frontier_rows,
        "edges": edge_rows,
    }
    path = OUT / "execution-frontier-summary.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_EXECUTION_FRONTIER "
        f"layout={layout} roles={summary['roleCount']} edges={summary['edgeCount']} "
        f"frontiers={len(frontier_rows)} max_parallel_width={max_parallel_width} "
        f"has_parallel_frontier={str(summary['hasParallelFrontier']).lower()} "
        f"path={path}"
    )
    for row in frontier_rows:
        print(
            "YOLO_LAYOUT_EXECUTION_FRONTIER_ROW "
            f"layout={layout} index={row['index']} width={row['parallelWidth']} "
            f"roles={','.join(row['roles'])}"
        )


def write_end_to_end_breakdown(layout: str, *, print_rows: bool = True) -> None:
    client = _load_json_file(OUT / "client-inference-timing-stats.json", {})
    handler = _load_json_file(OUT / "provider-handler-timing-stats.json", {})
    onnx = _load_json_file(OUT / "onnx-timing-stats.json", {})
    dep_in = _load_json_file(OUT / "dependency-input-timing-stats.json", {})
    dep_out = _load_json_file(OUT / "dependency-output-timing-stats.json", {})

    client_rows = sorted(
        client.get("rows", []),
        key=lambda row: (str(row.get("phase", "")), str(row.get("log", ""))),
    )
    # Preserve cold -> warm order when both phases are present.
    phase_order = {"cold": 0, "warm": 1}
    client_rows = sorted(
        client_rows,
        key=lambda row: (phase_order.get(str(row.get("phase", "")), 99),
                         str(row.get("log", ""))),
    )

    dataflow_rows = sorted(
        handler.get("dataflow", {}).get("rows", []),
        key=lambda row: int(row.get("startEpochMs", 0)),
    )

    def rows_by_session(summary: dict, key: str = "rows") -> dict[str, list[dict]]:
        grouped: dict[str, list[dict]] = {}
        for row in summary.get(key, []):
            session = str(row.get("session", ""))
            if session:
                grouped.setdefault(session, []).append(row)
        return grouped

    onnx_by_session = rows_by_session(onnx)
    dep_in_by_session = rows_by_session(dep_in)
    dep_out_by_session = rows_by_session(dep_out)
    handler_by_session = rows_by_session(handler)

    rows = []
    for index, client_row in enumerate(client_rows):
        dataflow = dataflow_rows[index] if index < len(dataflow_rows) else {}
        session = str(dataflow.get("session", ""))
        onnx_rows = onnx_by_session.get(session, [])
        input_rows = dep_in_by_session.get(session, [])
        output_rows = dep_out_by_session.get(session, [])
        handler_rows = handler_by_session.get(session, [])
        handler_end_rows = [
            row for row in handler_rows
            if row.get("event") == "end"
        ]
        handler_start_rows = [
            row for row in handler_rows
            if row.get("event") == "start"
        ]
        request_ms = float(client_row.get("request_ms", 0.0))
        total_ms = float(client_row.get("total_ms", 0.0))
        plan_ms = float(client_row.get("plan_ms", client_row.get("scope_key_ms", 0.0)))
        role_run_window_ms = float(dataflow.get("dataflowMs", 0.0))
        dataflow_ms = float(dataflow.get("submittedDataflowMs", role_run_window_ms))
        dependency_fetch_sum = sum(float(row.get("fetch_ms", 0.0)) for row in input_rows)
        dependency_fetch_max = max(
            [float(row.get("fetch_ms", 0.0)) for row in input_rows] or [0.0])
        dependency_publish_sum = sum(float(row.get("publish_ms", 0.0)) for row in output_rows)
        dependency_publish_max = max(
            [float(row.get("publish_ms", 0.0)) for row in output_rows] or [0.0])
        onnx_run_sum = sum(float(row.get("run_ms", 0.0)) for row in onnx_rows)
        onnx_collect_sum = sum(float(row.get("collect_ms", 0.0)) for row in onnx_rows)
        onnx_package_sum = sum(float(row.get("publish_ms", 0.0)) for row in onnx_rows)
        pre_run_wait_max = max(
            [float(row.get("queue_wait_ms", 0.0)) for row in handler_start_rows] or [0.0])
        handler_max = max(
            [float(row.get("handler_ms", 0.0)) for row in handler_end_rows] or [0.0])
        rows.append({
            "index": index,
            "phase": client_row.get("phase", ""),
            "session": session,
            "totalMs": total_ms,
            "planOrScopeKeyMs": plan_ms,
            "requestMs": request_ms,
            "providerDataflowMs": dataflow_ms,
            "roleRunWindowMs": role_run_window_ms,
            "outerControlResidualMs": max(0.0, request_ms - dataflow_ms),
            "nativeHotPathApproxMs": dataflow_ms,
            "preRunDependencyWaitMaxMs": pre_run_wait_max,
            "handlerMaxMs": handler_max,
            "dependencyFetchSumMs": dependency_fetch_sum,
            "dependencyFetchMaxMs": dependency_fetch_max,
            "dependencyPublishSumMs": dependency_publish_sum,
            "dependencyPublishMaxMs": dependency_publish_max,
            "onnxCollectSumMs": onnx_collect_sum,
            "onnxRunSumMs": onnx_run_sum,
            "onnxPackageSumMs": onnx_package_sum,
            "dependencyInputBytes": sum(int(row.get("bytes", 0)) for row in input_rows),
            "dependencyOutputBytes": sum(int(row.get("bytes", 0)) for row in output_rows),
            "roleCount": int(dataflow.get("roleCount", 0)),
            "dependencyInputCount": len(input_rows),
            "dependencyOutputCount": len(output_rows),
            "onnxRoleCount": len(onnx_rows),
        })

    summary = {
        "layout": layout,
        "count": len(rows),
        "requestMs": summarize_numeric([row["requestMs"] for row in rows]),
        "providerDataflowMs": summarize_numeric([
            row["providerDataflowMs"] for row in rows
        ]),
        "roleRunWindowMs": summarize_numeric([
            row["roleRunWindowMs"] for row in rows
        ]),
        "outerControlResidualMs": summarize_numeric([
            row["outerControlResidualMs"] for row in rows
        ]),
        "preRunDependencyWaitMaxMs": summarize_numeric([
            row["preRunDependencyWaitMaxMs"] for row in rows
        ]),
        "dependencyFetchSumMs": summarize_numeric([
            row["dependencyFetchSumMs"] for row in rows
        ]),
        "dependencyFetchMaxMs": summarize_numeric([
            row["dependencyFetchMaxMs"] for row in rows
        ]),
        "dependencyPublishSumMs": summarize_numeric([
            row["dependencyPublishSumMs"] for row in rows
        ]),
        "onnxRunSumMs": summarize_numeric([
            row["onnxRunSumMs"] for row in rows
        ]),
        "rows": rows,
    }
    path = OUT / "end-to-end-breakdown-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_E2E_BREAKDOWN "
        f"layout={layout} count={summary['count']} "
        f"request_p50_ms={summary['requestMs']['p50']:.2f} "
        f"provider_dataflow_p50_ms={summary['providerDataflowMs']['p50']:.2f} "
        f"role_run_window_p50_ms={summary['roleRunWindowMs']['p50']:.2f} "
        f"outer_control_residual_p50_ms={summary['outerControlResidualMs']['p50']:.2f} "
        f"pre_run_dependency_wait_max_p50_ms="
        f"{summary['preRunDependencyWaitMaxMs']['p50']:.2f} "
        f"dependency_fetch_sum_p50_ms={summary['dependencyFetchSumMs']['p50']:.2f} "
        f"dependency_fetch_max_p50_ms={summary['dependencyFetchMaxMs']['p50']:.2f} "
        f"dependency_publish_sum_p50_ms={summary['dependencyPublishSumMs']['p50']:.2f} "
        f"onnx_run_sum_p50_ms={summary['onnxRunSumMs']['p50']:.2f} "
        f"path={path}"
    )
    if not print_rows:
        return
    for row in rows:
        print(
            "YOLO_LAYOUT_E2E_BREAKDOWN_ROW "
            f"layout={layout} index={row['index']} phase={row['phase']} "
            f"session={row['session']} "
            f"request_ms={row['requestMs']:.2f} "
            f"provider_dataflow_ms={row['providerDataflowMs']:.2f} "
            f"role_run_window_ms={row['roleRunWindowMs']:.2f} "
            f"outer_control_residual_ms={row['outerControlResidualMs']:.2f} "
            f"dependency_fetch_sum_ms={row['dependencyFetchSumMs']:.2f} "
            f"dependency_fetch_max_ms={row['dependencyFetchMaxMs']:.2f} "
            f"onnx_run_sum_ms={row['onnxRunSumMs']:.2f} "
            f"input_bytes={row['dependencyInputBytes']} "
            f"output_bytes={row['dependencyOutputBytes']}"
        )


def write_plan_cache_summary(layout: str,
                             phases: list[tuple[str, Path]]) -> None:
    rows = []
    for phase, path in phases:
        text = path.read_text(errors="replace") if path.exists() else ""
        entries = [
            parse_key_value_line(line)
            for line in text.splitlines()
            if "NDNSF_DI_PLAN_CACHE" in line
        ]
        client_timing_entries = [
            parse_key_value_line(line)
            for line in text.splitlines()
            if "NDNSF_DI_CLIENT_INFERENCE_TIMING" in line
        ]
        preflight_timing_entries = [
            parse_key_value_line(line)
            for line in text.splitlines()
            if "NDNSF_DI_CLIENT_PREFLIGHT_TIMING" in line
        ]
        plan_session_invocations = sum(
            1 for entry in client_timing_entries
            if entry.get("mode") == "plan-session")
        plan_session_preflights = sum(
            1 for entry in preflight_timing_entries
            if entry.get("mode") == "plan-session")
        rows.append({
            "phase": phase,
            "log": str(path),
            "entries": entries,
            "clientTimingEntries": client_timing_entries,
            "preflightTimingEntries": preflight_timing_entries,
            "hits": sum(1 for entry in entries if entry.get("hit") == "true"),
            "misses": sum(1 for entry in entries if entry.get("hit") == "false"),
            "planSessionInvocations": plan_session_invocations,
            "planSessionPreflights": plan_session_preflights,
            "planSessionObservedInvocations": plan_session_invocations + plan_session_preflights,
            "artifactPublishes": text.count("inference-artifact--"),
            "scopeKeyPublishes": text.count("inference-scope-key-"),
            "inputPublishes": text.count("inference-input-image"),
        })
    summary = {
        "layout": layout,
        "phases": rows,
    }
    path = OUT / "plan-cache-stats.json"
    path.write_text(json.dumps(summary, indent=2, sort_keys=True),
                    encoding="utf-8")
    print(
        "YOLO_LAYOUT_PLAN_CACHE "
        f"layout={layout} "
        f"hits={sum(row['hits'] for row in rows)} "
        f"misses={sum(row['misses'] for row in rows)} "
        f"plan_session_invocations="
        f"{sum(row['planSessionInvocations'] for row in rows)} "
        f"plan_session_preflights="
        f"{sum(row['planSessionPreflights'] for row in rows)} "
        f"plan_session_observed_invocations="
        f"{sum(row['planSessionObservedInvocations'] for row in rows)} "
        f"path={path}"
    )
    for row in rows:
        print(
            "YOLO_LAYOUT_PLAN_CACHE_PHASE "
            f"layout={layout} phase={row['phase']} "
            f"entries={len(row['entries'])} "
            f"hits={row['hits']} "
            f"misses={row['misses']} "
            f"plan_session_invocations={row['planSessionInvocations']} "
            f"plan_session_preflights={row['planSessionPreflights']} "
            f"plan_session_observed_invocations={row['planSessionObservedInvocations']} "
            f"artifact_publishes={row['artifactPublishes']} "
            f"scope_key_publishes={row['scopeKeyPublishes']} "
            f"input_publishes={row['inputPublishes']} "
            f"log={row['log']}"
        )


def user_wait_timeout(count: int, timeout_ms: int, duration_s: float = 0.0) -> int:
    timeout_by_count = int((max(1, count) * max(1, timeout_ms)) / 1000) + 30
    timeout_by_duration = int(max(0.0, duration_s)) + int(max(1, timeout_ms) / 1000) + 60
    return max(90, timeout_by_count, timeout_by_duration)


def load_policy_roles(path: Path) -> list[str]:
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError("PyYAML is required to read generated DI policies") from exc
    config = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    for service in config.get("services", []):
        name = str(service.get("name", ""))
        if name != "/NDNSF/DistributedRepo":
            roles = [str(role) for role in service.get("roles", []) if str(role)]
            if roles:
                return roles
    raise RuntimeError(f"no inference service roles found in {path}")


def load_policy_service_name(path: Path) -> str:
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError("PyYAML is required to read generated DI policies") from exc
    config = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    for service in config.get("services", []):
        name = str(service.get("name", ""))
        if name and name != "/NDNSF/DistributedRepo":
            return name
    raise RuntimeError(f"no inference service found in {path}")


def provider_role_assignments(roles: list[str]) -> list[tuple[str, str, list[str]]]:
    nodes = list(AI_LAB_PROVIDER_NODES)
    provider_ids = ["", "A", "B", "C", "E", "F", "G", "H", "I"]
    if len(roles) > len(nodes):
        raise RuntimeError(
            f"layout needs {len(roles)} role providers but MiniNDN smoke "
            f"defines only {len(nodes)} provider nodes")
    result = []
    for index, role in enumerate(roles):
        node_name = nodes[index]
        provider_id = provider_ids[index]
        name = "provider-root" if not provider_id else f"provider-{provider_id}"
        result.append((node_name, name, [
            "--provider-id", provider_id,
            "--roles", role,
        ]))
    return result


def stop(procs):
    for p, f, _ in reversed(procs):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        f.close()


def provider_identity(provider_id: str) -> str:
    return PROVIDER_PREFIX if not provider_id else PROVIDER_PREFIX + "/" + provider_id


def key_name_from_certificate_name(cert_name: str) -> str:
    marker = "/KEY/"
    if marker not in cert_name:
        raise RuntimeError(f"certificate name does not contain KEY component: {cert_name}")
    prefix, suffix = cert_name.split(marker, 1)
    key_id = suffix.split("/", 1)[0]
    return prefix + marker + key_id


def initialize_di_keychains(ndn,
                            output_dir: Path,
                            provider_identities: list[str],
                            *,
                            dual_signing_certs: bool = True) -> None:
    """Install root-signed keys that match the generated DI policy namespace."""
    log("Installing root-signed DI keychain material on MiniNDN nodes")
    security_dir = output_dir / "security"
    security_dir.mkdir(parents=True, exist_ok=True)
    identities = [
        CONTROLLER_IDENTITY,
        PROVIDER_PREFIX + "/D",
        USER_IDENTITY,
        *provider_identities,
    ]
    identities = list(dict.fromkeys(identities))

    for node in ndn.net.hosts:
        for identity in [APP_ROOT] + identities:
            perf.node_cmd(node, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(identity)))

    controller = ndn.net[AI_LAB_CONTROLLER_NODE]
    root_cert_path = security_dir / "root.cert"
    perf.node_cmd(controller, "ndnsec key-gen -t r {} > {}".format(
        perf.shell_quote(APP_ROOT), perf.shell_quote(root_cert_path)))
    perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
        perf.shell_quote(root_cert_path)))
    log("di_root_cert identity={} name={} file={}".format(
        APP_ROOT, perf.certificate_name_from_file(root_cert_path), root_cert_path))

    exported_keys = []
    for index, identity in enumerate(identities):
        rsa_cert_path = security_dir / f"di-identity-{index}-rsa.cert"
        rsa_req_path = security_dir / f"di-identity-{index}-rsa.req"
        rsa_key_path = security_dir / f"di-identity-{index}-rsa.ndnkey"
        perf.node_cmd(controller, "ndnsec key-gen -t r {} > {}".format(
            perf.shell_quote(identity), perf.shell_quote(rsa_req_path)))
        perf.node_cmd(controller, "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
            perf.shell_quote(APP_ROOT), perf.shell_quote(rsa_req_path), perf.shell_quote(rsa_cert_path)))
        perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(rsa_cert_path)))

        rsa_cert_name = perf.certificate_name_from_file(rsa_cert_path)
        rsa_key_name = key_name_from_certificate_name(rsa_cert_name)
        perf.node_cmd(controller, "ndnsec set-default -k -n {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(rsa_key_name)))
        perf.node_cmd(controller, "ndnsec-export -P 123456 -o {} -k {}".format(
            perf.shell_quote(rsa_key_path), perf.shell_quote(rsa_key_name)))
        ec_key_path = None
        if dual_signing_certs:
            ec_cert_path = security_dir / f"di-identity-{index}-ec.cert"
            ec_req_path = security_dir / f"di-identity-{index}-ec.req"
            ec_key_path = security_dir / f"di-identity-{index}-ec.ndnkey"
            perf.node_cmd(controller, "ndnsec key-gen -n -t e {} > {}".format(
                perf.shell_quote(identity), perf.shell_quote(ec_req_path)))
            perf.node_cmd(controller, "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
                perf.shell_quote(APP_ROOT), perf.shell_quote(ec_req_path), perf.shell_quote(ec_cert_path)))
            perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(ec_cert_path)))
            ec_cert_name = perf.certificate_name_from_file(ec_cert_path)
            ec_key_name = key_name_from_certificate_name(ec_cert_name)
            perf.node_cmd(controller, "ndnsec-export -P 123456 -o {} -k {}".format(
                perf.shell_quote(ec_key_path), perf.shell_quote(ec_key_name)))
            log("di_identity_certs identity={} rsaCert={} ecSigningCert={}".format(
                identity, rsa_cert_name, ec_cert_name))
        else:
            log("di_identity_certs identity={} rsaCert={} ecSigningCert=disabled".format(
                identity, rsa_cert_name))
        exported_keys.append((rsa_key_path, ec_key_path, rsa_key_name))

    for node in ndn.net.hosts:
        perf.node_cmd(node, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(root_cert_path)))
        for rsa_key_path, ec_key_path, rsa_key_name in exported_keys:
            perf.node_cmd(node, "ndnsec import -P 123456 {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(rsa_key_path)))
            if ec_key_path is not None:
                perf.node_cmd(node, "ndnsec import -P 123456 {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(ec_key_path)))
            perf.node_cmd(node, "ndnsec set-default -k -n {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(rsa_key_name)))


def main() -> None:
    parser = argparse.ArgumentParser()
    # Benchmark recipe note:
    #   sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true; sleep 3
    #   sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=0 \
    #     python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
    #       --layout 2x2 --parallel-detect-scale-shards --native-providers \
    #       --cold-requests 0 --preflight-requests 1 --warm-duration-s 60 \
    #       --warm-interval-ms 1000 \
    #       --ack-timeout-ms 300 --timeout-ms 10000 --quiet-perf-logs
    # Do not set a global NDN_LOG around this MiniNDN command. NFD inherits it
    # and may fail on application-style filters; use --quiet-perf-logs instead.
    parser.add_argument("--layout", default="2x2",
                        help="YOLO stage-by-shard layout, e.g. 1x3, 2x3, 3x2, 3x3")
    parser.add_argument("--model", default="yolo26n.pt",
                        help="YOLO model weights/path passed to split_model.py and user.py")
    parser.add_argument("--input-size", type=int, default=32,
                        help="Square YOLO input size passed to split_model.py and user.py")
    parser.add_argument("--results-dir", default="",
                        help="Override the default results/yolo_<layout>_minindn_quick output directory")
    parser.add_argument("--cold-requests", type=int, default=1,
                        help=("Sequential requests in the cold user process. "
                              "Use 0 for canonical warm benchmarks so preflight "
                              "and measured requests stay in the same user process."))
    parser.add_argument("--warm-requests", type=int, default=1,
                        help="Sequential requests in the warm user process")
    parser.add_argument("--warm-duration-s", type=float, default=0.0,
                        help="Run the warm user for this many seconds instead of a fixed request count")
    parser.add_argument("--warm-interval-ms", type=int, default=0,
                        help="Minimum interval between warm sequential request starts")
    parser.add_argument("--preflight-requests", type=int, default=0,
                        help="Warm the warm user plan session before measured warm requests")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500,
                        help="ACK collection timeout passed to the DI user")
    parser.add_argument("--timeout-ms", type=int, default=60000,
                        help="End-to-end service timeout passed to the DI user")
    parser.add_argument("--provider-handler-workers", type=int, default=2,
                        help="Python worker count passed to each DI provider")
    parser.add_argument("--native-providers", action="store_true",
                        help="replace Python compute providers with build/examples/di-native-provider")
    parser.add_argument("--user-async-workers", type=int, default=1,
                        help="Async worker count passed to the DI user process")
    parser.add_argument("--parallel-output-shards", action="store_true",
                        help="Use the experimental true-NxM YOLO output-shard prototype")
    parser.add_argument("--parallel-detect-scale-shards", action="store_true",
                        help="Use the YOLO Detect-scale DAG splitter")
    parser.add_argument("--control-trace", action="store_true",
                        help="Enable NDNSF user control-plane TRACE logs and write ACK/selection timing stats")
    parser.add_argument("--quiet-perf-logs", action="store_true",
                        help="suppress NDNSF library INFO/DEBUG/TRACE logs and print only DI result lines")
    parser.add_argument("--crypto-timing", action="store_true",
                        help="enable narrow hybrid decrypt cache/unwrap/AES/callback timing logs")
    parser.add_argument("--control-timing", action="store_true",
                        help="enable NDNSF request/provider lifecycle timing plus ACK/selection/control-path summaries")
    parser.add_argument("--svs-internal-timing", action="store_true",
                        help=("enable narrow ndn-svs internal timing logs for Sync Interest "
                              "parse/decode/compare/missing-data/extra-block/encode/sign/face-put"))
    parser.add_argument("--dependency-timing", action="store_true",
                        help="enable dependency fetch and pending IMS timing logs")
    parser.add_argument("--disable-native-runtime-timing", action="store_true",
                        help=("pure performance run: do not enable C++ native provider "
                              "role/ONNX/dependency timing unless another diagnostic flag "
                              "requires it"))
    parser.add_argument("--disable-exact-segment-fetch", action="store_true",
                        help="disable deterministic exact segment fetch for planned native DI activations")
    parser.add_argument("--single-rsa-certs", action="store_true",
                        help="MiniNDN control run: create only RSA identity certs instead of RSA+ECDSA split certs")
    parser.add_argument("--disable-split-signing", action="store_true",
                        help="MiniNDN control run: keep dual certs installed but force RSA signing")
    parser.add_argument("--signing-ab-phases", default="",
                        help=("Run same-topology signing comparison phases, e.g. rsa,ecdsa,rsa. "
                              "Each phase restarts providers and runs a warm workload; requires dual certs."))
    parser.add_argument("--ndnsf-handler-threads", type=int, default=-1,
                        help="Override NDNSF_HANDLER_THREADS; -1 keeps env/default serial experiment setting")
    parser.add_argument("--ndnsf-ack-threads", type=int, default=-1,
                        help="Override NDNSF_ACK_THREADS; -1 keeps env/default serial experiment setting")
    parser.add_argument("--warm-rtt-monitor-interval-s", type=float, default=0.0,
                        help=("Diagnostic mode: during each warm phase, sample user->provider "
                              "ndnping RTT and NFD network-face counters at this interval. "
                              "Adds probe traffic; do not use for canonical latency numbers."))
    parser.add_argument("--parallel-svs-runtime", action="store_true",
                        help="Enable NDNSF async/parallel SVS publish/sync/production for a runtime-overhead comparison")
    parser.add_argument("--serial-svs-runtime", action="store_true",
                        help="Force the older serial NDNSF/SVS runtime even when native providers are used")
    parser.add_argument("--sync-svs-publish", action="store_true",
                        help="keep parallel SVS processing/production but publish local control messages synchronously")
    parser.add_argument("--ndn-packet-trace", action="store_true",
                        help=("last-resort external NDN packet capture for SVS/control diagnostics; "
                              "prefer 60s --control-timing runs for latency analysis"))
    parser.add_argument("--ndn-packet-trace-nodes",
                        default="memphis,ucla,arizona,wustl,neu",
                        help="comma-separated node list for --ndn-packet-trace")
    parser.add_argument("--ndn-packet-trace-window",
                        choices=["all", "warm"],
                        default="warm",
                        help="capture only the warm inference window by default; use all only for startup/repo/keychain diagnostics")
    args_cli = parser.parse_args()
    if args_cli.parallel_output_shards and args_cli.parallel_detect_scale_shards:
        raise SystemExit("--parallel-output-shards and --parallel-detect-scale-shards are mutually exclusive")
    if args_cli.parallel_svs_runtime and args_cli.serial_svs_runtime:
        raise SystemExit("--parallel-svs-runtime and --serial-svs-runtime are mutually exclusive")
    layout = args_cli.layout.strip().lower().replace("*", "x")
    cold_requests = max(0, args_cli.cold_requests)
    warm_requests = max(0, args_cli.warm_requests)
    warm_duration_s = max(0.0, float(args_cli.warm_duration_s or 0.0))
    warm_interval_ms = max(0, args_cli.warm_interval_ms)
    preflight_requests = max(0, args_cli.preflight_requests)
    signing_ab_phases = [
        item.strip().lower()
        for item in args_cli.signing_ab_phases.split(",")
        if item.strip()
    ]
    if signing_ab_phases:
        invalid = [item for item in signing_ab_phases if item not in {"rsa", "ecdsa"}]
        if invalid:
            raise RuntimeError(
                "--signing-ab-phases accepts only rsa and ecdsa entries; "
                f"invalid={invalid}")
        if args_cli.single_rsa_certs and "ecdsa" in signing_ab_phases:
            raise RuntimeError("--signing-ab-phases ecdsa requires dual certs; remove --single-rsa-certs")
    ack_timeout_ms = max(1, args_cli.ack_timeout_ms)
    timeout_ms = max(1, args_cli.timeout_ms)
    provider_handler_workers = max(1, args_cli.provider_handler_workers)
    user_async_workers = max(1, args_cli.user_async_workers)
    sys.argv = [sys.argv[0]]
    safe_layout = layout.replace("/", "-")
    if args_cli.parallel_output_shards:
        safe_layout += "_parallel_output"
    if args_cli.parallel_detect_scale_shards:
        safe_layout += "_parallel_detect_scale"
    global OUT, CONFIG, GEN_POLICY, REPO_MANIFEST
    OUT = Path(args_cli.results_dir).expanduser() if args_cli.results_dir else (
        REPO / f"results/yolo_{safe_layout}_minindn_quick")
    if not OUT.is_absolute():
        OUT = REPO / OUT
    CONFIG = OUT / "yolo_policy.yaml"
    GEN_POLICY = f"/tmp/ndnsf-di-yolo-{safe_layout}-policy"
    REPO_MANIFEST = OUT / "repo-manifests.json"

    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    for stats_path in OUT.glob("*-stats.json"):
        try:
            stats_path.unlink()
        except FileNotFoundError:
            pass
    for stats_path in (
            OUT / "traffic-stats.json",
            OUT / "repo-manifests.json"):
        try:
            stats_path.unlink()
        except FileNotFoundError:
            pass
    py_path = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(PY_DIR),
        os.environ.get("PYTHONPATH", ""),
    ])
    split_command = [
        "python3",
        str(PY_DIR / "split_model.py"),
        "--auto-split",
        "--layout",
        layout,
        "--model",
        args_cli.model,
        "--input-size",
        str(args_cli.input_size),
        "--out-dir",
        str(OUT / "model"),
        "--policy",
        str(CONFIG),
        "--dynamic-provisioning",
        "--trust-anchor-file",
        str(OUT / "security/root.cert"),
    ]
    if args_cli.parallel_output_shards:
        split_command.append("--parallel-output-shards")
    if args_cli.parallel_detect_scale_shards:
        split_command.append("--parallel-detect-scale-shards")
    run_user_python_step(split_command,
                         cwd=str(REPO),
                         env={**os.environ, "PYTHONPATH": py_path},
                         writable_path=OUT)
    service_name = load_policy_service_name(CONFIG)
    write_execution_frontier_summary(layout, CONFIG)
    if args_cli.native_providers:
        native_exe = REPO / "build/examples/di-native-provider"
        if not native_exe.exists():
            raise RuntimeError(
                f"{native_exe} does not exist; build it with "
                "./waf build --targets=di-native-provider")
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    control_detail_trace = args_cli.control_trace
    args = Args(
        controller_node=AI_LAB_CONTROLLER_NODE,
        user_node=AI_LAB_USER_NODE,
        providers=len(AI_LAB_PROVIDER_NODES),
        provider_nodes=",".join(AI_LAB_PROVIDER_NODES),
        serve_provider_certs=False,
        debug_ack=control_detail_trace,
        # Keep app_env() on the debug_ack log profile only for explicit
        # full trace. --control-timing uses narrow NDNSF_CONTROL_TIMING rows
        # so it can be used during 60s benchmark runs without full TRACE cost.
        timeline_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_parallel_sync_processing=False,
        svs_parallel_workers=4,
        svs_parallel_queue=256,
        svs_sync_publish=True,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=1,
        performance_mode=args_cli.quiet_perf_logs,
    )
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        providers = provider_role_assignments(load_policy_roles(CONFIG))
        provider_identities = [
            provider_identity(argv[argv.index("--provider-id") + 1])
            for _, _, argv in providers
        ]

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net[AI_LAB_CONTROLLER_NODE]], [
            "/NDNSF-DistributeInference/example/controller",
            "/NDNSF-DistributeInference/example/controller/DKEY",
            "/NDNSF-DistributeInference/example/controller/KEY",
            "/NDNSF-DistributeInference/example/group",
        ])
        rh.addOrigin([ndn.net[AI_LAB_USER_NODE]], ["/NDNSF-DistributeInference/example/user", "/NDNSF-DistributeInference/example/group"])
        origins = [
            (
                node_name,
                provider_identity(argv[argv.index("--provider-id") + 1]),
            )
            for node_name, _, argv in providers
        ]
        origins.append((AI_LAB_REPO_NODE, "/NDNSF-DistributeInference/example/provider/D"))
        for node_name, prefix in origins:
            rh.addOrigin([ndn.net[node_name]], [prefix, prefix + "/KEY", "/NDNSF-DistributeInference/example/group"])
        rh.addOrigin([ndn.net[AI_LAB_REPO_NODE]], ["/NDNSF/DistributedRepo/Object"])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            # Keep SVS group traffic multicast, but let provider/controller/user
            # object prefixes use best-route. Native DI activation Data is named
            # under the producer provider prefix, so treating the whole example
            # namespace as multicast makes deterministic activation fetches pay
            # unnecessary NFD forwarding overhead.
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example", Nfdc.STRATEGY_BEST_ROUTE)
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        trace_nodes = [
            item.strip()
            for item in args_cli.ndn_packet_trace_nodes.split(",")
            if item.strip()
        ] if args_cli.ndn_packet_trace else []
        packet_traces = []
        if args_cli.ndn_packet_trace and args_cli.ndn_packet_trace_window == "all":
            packet_traces = start_ndn_packet_traces(ndn, os.environ.copy(), trace_nodes)

        initialize_di_keychains(ndn,
                                OUT,
                                provider_identities,
                                dual_signing_certs=not args_cli.single_rsa_certs)
        subprocess.run(["rm", "-rf", str(OUT / "artifact-cache")], check=False)
        session = int(time.time()) + os.getpid()
        env = perf.app_env(OUT, session, args)
        # Native providers use C++ execution and benefit from a slightly wider
        # NDNSF control/runtime queue. Keep the Python provider path
        # conservative.
        default_runtime_threads = "4" if args_cli.native_providers else "1"
        env["NDNSF_HANDLER_THREADS"] = str(
            args_cli.ndnsf_handler_threads
            if args_cli.ndnsf_handler_threads >= 0
            else int(os.environ.get("NDNSF_HANDLER_THREADS", default_runtime_threads)))
        env["NDNSF_ACK_THREADS"] = str(
            args_cli.ndnsf_ack_threads
            if args_cli.ndnsf_ack_threads >= 0
            else int(os.environ.get("NDNSF_ACK_THREADS", default_runtime_threads)))
        use_parallel_runtime = (
            args_cli.parallel_svs_runtime or
            (args_cli.native_providers and not args_cli.serial_svs_runtime)
        )
        if use_parallel_runtime:
            env["NDNSF_SVS_ASYNC_PUBLISH"] = "0" if args_cli.sync_svs_publish else "1"
            env["NDNSF_SVS_PARALLEL_SYNC"] = "1"
            env["NDNSF_SVS_PARALLEL_WORKERS"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_WORKERS", "4")
            env["NDNSF_SVS_PARALLEL_QUEUE"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_QUEUE", "256")
            env["NDNSF_SVS_PARALLEL_PRODUCTION"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_PRODUCTION", "4")
            env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING", "0")
            env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK", "1")
        else:
            env["NDNSF_SVS_ASYNC_PUBLISH"] = os.environ.get(
                "NDNSF_SVS_ASYNC_PUBLISH", "0")
            env["NDNSF_SVS_PARALLEL_SYNC"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_SYNC", "0")
            env["NDNSF_SVS_PARALLEL_PRODUCTION"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_PRODUCTION", "0")
            env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING", "0")
            env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = os.environ.get(
                "NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK", "0")
        if args_cli.native_providers and not args_cli.disable_exact_segment_fetch:
            env["NDNSF_COLLAB_LARGE_EXACT_SEGMENT_FETCH"] = "1"
            env["NDNSF_SELECTION_DIRECT_PREFETCH"] = os.environ.get(
                "NDNSF_SELECTION_DIRECT_PREFETCH", "1")
        needs_native_runtime_timing = (
            (args_cli.native_providers and not args_cli.disable_native_runtime_timing) or
            not args_cli.quiet_perf_logs or
            args_cli.dependency_timing or args_cli.control_timing or
            args_cli.control_trace
        )
        if needs_native_runtime_timing:
            env["NDNSF_DI_RUNTIME_TIMING"] = "1"
        if args_cli.dependency_timing:
            env["NDNSF_COLLAB_LARGE_FETCH_TIMING"] = "1"
            env["NDNSF_PENDING_IMS_TIMING"] = "1"
        if args_cli.crypto_timing:
            env["NDNSF_HYBRID_CRYPTO_TIMING"] = "1"
            env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = os.environ.get(
                "NDNSF_TIMELINE_TRACE_SAMPLE_RATE", "10")
        if args_cli.disable_split_signing:
            env["NDNSF_DISABLE_SPLIT_SIGNING"] = "1"
        if args_cli.control_timing:
            env["NDNSF_CONTROL_TIMING"] = "1"
            env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = os.environ.get(
                "NDNSF_TIMELINE_TRACE_SAMPLE_RATE", "10")
        if args_cli.svs_internal_timing:
            env["NDNSF_CONTROL_TIMING"] = "1"
            env["NDN_LOG"] = os.environ.get(
                "NDN_LOG",
                "ndn_svs.SyncTimeline=TRACE:ndn_svs.SVSPubSub=TRACE:"
                "ndn_service_framework.*=WARN:"
                "ndn_service_framework.ServiceController=INFO")
        if args_cli.control_trace:
            env["NDNSF_TIMELINE_TRACE"] = "1"
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(PY_DIR),
            "/home/tianxing/.local/lib/python3.8/site-packages",
            "/usr/local/lib/python3.8/dist-packages",
            "/usr/lib/python3/dist-packages",
            os.environ.get("PYTHONPATH", ""),
        ])
        print(
            "YOLO_LAYOUT_RUN_CONFIG "
            f"layout={layout} model={args_cli.model} input_size={args_cli.input_size} "
            f"topology={TOPO} native_providers={str(args_cli.native_providers).lower()} "
            f"parallel_detect_scale_shards={str(args_cli.parallel_detect_scale_shards).lower()} "
            f"ack_timeout_ms={ack_timeout_ms} timeout_ms={timeout_ms} "
            f"provider_handler_workers={provider_handler_workers} "
            f"user_async_workers={user_async_workers} "
            f"exact_segment_fetch={env.get('NDNSF_COLLAB_LARGE_EXACT_SEGMENT_FETCH', '0')} "
            f"exact_segment_window="
            f"{env.get('NDNSF_COLLAB_LARGE_EXACT_SEGMENT_WINDOW', os.environ.get('NDNSF_COLLAB_LARGE_EXACT_SEGMENT_WINDOW', 'default'))} "
            f"exact_segment_interest_lifetime_ms="
            f"{env.get('NDNSF_COLLAB_LARGE_EXACT_SEGMENT_INTEREST_LIFETIME_MS', os.environ.get('NDNSF_COLLAB_LARGE_EXACT_SEGMENT_INTEREST_LIFETIME_MS', 'default'))} "
            f"collab_large_interest_lifetime_ms="
            f"{env.get('NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS', os.environ.get('NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS', 'default'))}"
        )
        bootstrap_env = dict(env)
        if args_cli.quiet_perf_logs and not args_cli.svs_internal_timing:
            bootstrap_env["NDN_LOG"] = os.environ.get(
                "NDN_LOG",
                "ndn_service_framework.*=WARN:"
                "ndn_service_framework.ServiceController=INFO")

        write_ndnping_rtt_summary(ndn, providers, env, procs)

        common = ["--config", str(CONFIG), "--generated-policy-dir", GEN_POLICY]
        _, controller_log = start(ndn.net[AI_LAB_CONTROLLER_NODE], "controller",
                                  python_cmd("controller.py", common), bootstrap_env, procs)
        if not wait_log(controller_log, "ServiceController listening", 20):
            raise RuntimeError(f"controller did not become ready; see {controller_log}")
        time.sleep(4)
        _, repo_log = start(
            ndn.net[AI_LAB_REPO_NODE],
            "repo",
            python_cmd("repo_node.py", common + [
                "--provider-id", "D",
                "--repo-node", "/NDNSF-DistributeInference/example/provider/D",
                "--failure-domain", "repo-rack",
                "--storage-dir", f"/tmp/yolo-{safe_layout}-repo-store",
                "--handler-threads", "1",
                "--ack-threads", "1",
            ]),
            env,
            procs,
        )
        if not wait_log(repo_log, "Installed provider permission", 60):
            raise RuntimeError(f"repo did not install permissions; see {repo_log}")
        deployer_proc, deployer_log = start(ndn.net[AI_LAB_CONTROLLER_NODE], "controller-deployer",
                                            python_cmd("controller.py", common + [
                                                "--deploy-only",
                                                "--deploy-to-repo-manifest",
                                                str(REPO_MANIFEST),
                                                "--replication-factor",
                                                "1",
                                            ]), env, procs)
        if not wait_log(deployer_log, "YOLO_2X2_CONTROLLER_REPO_DEPLOYED", 360,
                        proc=deployer_proc):
            deployer_rc = deployer_proc.poll()
            deployer_tail = deployer_log.read_text(errors="replace")[-2000:] if deployer_log.exists() else ""
            raise RuntimeError(
                "controller did not deploy repo artifacts; "
                f"returncode={deployer_rc}; see {deployer_log}\n{deployer_tail}")
        validate_repo_manifest_references(REPO_MANIFEST)

        def start_compute_providers(phase_suffix: str,
                                    provider_env: dict[str, str]
                                    ) -> list[tuple[object, object, Path]]:
            provider_procs: list[tuple[object, object, Path]] = []
            for node_name, name, argv in providers:
                if args_cli.native_providers:
                    cmd = native_provider_cmd(
                        argv,
                        service_name=service_name,
                        workers=provider_handler_workers,
                        handler_threads=int(env["NDNSF_HANDLER_THREADS"]),
                        ack_threads=int(env["NDNSF_ACK_THREADS"]))
                    ready = "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY"
                    ready_timeout = 120
                else:
                    cmd = python_cmd("provider.py", common + argv + [
                        "--dynamic-provisioning",
                        "--temp-dir",
                        f"/tmp/{name}",
                        "--handler-workers",
                        str(provider_handler_workers),
                    ])
                    ready = "Installed provider permission"
                    ready_timeout = 30
                log_name = f"{name}{phase_suffix}"
                _, lp = start(ndn.net[node_name], log_name, cmd,
                              provider_env, provider_procs)
                if not wait_log(lp, ready, ready_timeout):
                    raise RuntimeError(
                        f"{log_name} did not become ready; expected {ready}; see {lp}")
                time.sleep(0.5)
            return provider_procs

        provider_procs = start_compute_providers("", env)
        procs.extend(provider_procs)
        write_dependency_edge_rtt_summary(
            ndn,
            providers,
            env,
            procs,
            Path(GEN_POLICY) / "native-execution-plan.json")

        def remove_provider_procs(entries: list[tuple[object, object, Path]]) -> None:
            if not entries:
                return
            stop_process_group(entries)
            entry_ids = {id(entry) for entry in entries}
            procs[:] = [entry for entry in procs if id(entry) not in entry_ids]

        def signing_phase_env(mode: str) -> dict[str, str]:
            phase_env = dict(env)
            if mode == "rsa":
                phase_env["NDNSF_DISABLE_SPLIT_SIGNING"] = "1"
            elif mode == "ecdsa":
                phase_env.pop("NDNSF_DISABLE_SPLIT_SIGNING", None)
            else:
                raise RuntimeError(f"unsupported signing phase mode: {mode}")
            return phase_env

        def build_warm_user_args() -> list[str]:
            warm_user_args = common + [
                "--repo-manifest-file",
                str(REPO_MANIFEST),
                "--model", args_cli.model,
                "--input-size", str(args_cli.input_size),
                "--ack-timeout-ms", str(ack_timeout_ms),
                "--timeout-ms", str(timeout_ms),
                "--async-requests", str(user_async_workers),
            ]
            if preflight_requests > 0:
                warm_user_args.extend(["--preflight-requests", str(preflight_requests)])
            if args_cli.native_providers:
                warm_user_args.append("--native-tensor-input")
            if warm_duration_s > 0:
                warm_user_args.extend([
                    "--sequential-duration-s", str(warm_duration_s),
                    "--sequential-interval-ms", str(warm_interval_ms),
                ])
            else:
                warm_user_args.extend(["--sequential-requests", str(warm_requests)])
            return warm_user_args

        def run_warm_phase(phase: str,
                           workload_env: dict[str, str],
                           *,
                           trace_for_phase: bool) -> tuple[Path, list[float], int]:
            nonlocal packet_traces
            if (args_cli.ndn_packet_trace and
                    args_cli.ndn_packet_trace_window == "warm" and
                    trace_for_phase and not packet_traces):
                packet_traces = start_ndn_packet_traces(ndn, os.environ.copy(), trace_nodes)
                # Give tcpdump a brief moment to open the pcap files before the
                # warm request stream starts.
                time.sleep(0.5)
            traffic_start = snapshot_traffic(ndn)
            nfd_start = snapshot_nfd_data_counters(ndn)
            monitor = None
            if args_cli.warm_rtt_monitor_interval_s > 0:
                monitor = start_warm_rtt_nfd_monitor(
                    ndn, providers, args_cli.warm_rtt_monitor_interval_s)
            proc, log_path = start(
                ndn.net["memphis"],
                f"user-{phase}",
                python_cmd("user.py", build_warm_user_args()),
                workload_env,
                procs,
            )
            try:
                proc.wait(timeout=user_wait_timeout(warm_requests, timeout_ms, warm_duration_s))
            finally:
                if monitor is not None:
                    stop_event, thread, _, _ = monitor
                    stop_event.set()
                    thread.join(timeout=max(2.0, args_cli.warm_rtt_monitor_interval_s + 1.0))
                if packet_traces and args_cli.ndn_packet_trace_window == "warm" and trace_for_phase:
                    stop_ndn_packet_traces(packet_traces)
            nfd_end = snapshot_nfd_data_counters(ndn)
            traffic_end = snapshot_traffic(ndn)
            text = log_path.read_text(errors="replace")
            latencies = write_latency_summary(layout, phase, text)
            if monitor is not None:
                _, _, samples, lock = monitor
                write_warm_rtt_nfd_monitor_summary(layout, phase, log_path, samples, lock)
            measured_count = len(latencies) or warm_requests
            observed_count = measured_count + preflight_requests
            write_traffic_delta(layout, phase, traffic_start, traffic_end,
                                observed_count,
                                measured_request_count=measured_count,
                                preflight_request_count=preflight_requests)
            write_nfd_data_delta(layout, phase, nfd_start, nfd_end,
                                 observed_count,
                                 measured_request_count=measured_count,
                                 preflight_request_count=preflight_requests)
            print_user_workload_output(text, args_cli.quiet_perf_logs)
            return log_path, latencies, measured_count

        time.sleep(2)
        user_common = common + [
            "--repo-manifest-file",
            str(REPO_MANIFEST),
            "--model", args_cli.model,
            "--input-size", str(args_cli.input_size),
            "--ack-timeout-ms", str(ack_timeout_ms),
            "--timeout-ms", str(timeout_ms),
            "--async-requests", str(user_async_workers),
            "--sequential-requests", str(cold_requests),
        ]
        if args_cli.native_providers:
            user_common.append("--native-tensor-input")
        phase_logs: list[tuple[str, Path]] = []
        user_log: Path | None = None
        cold_text = ""
        cold_latencies: list[float] = []
        if cold_requests > 0:
            cold_traffic_start = snapshot_traffic(ndn)
            cold_nfd_start = snapshot_nfd_data_counters(ndn)
            user_proc, user_log = start(
                ndn.net["memphis"],
                "user-cold",
                python_cmd("user.py", user_common),
                env,
                procs,
            )
            user_proc.wait(timeout=user_wait_timeout(cold_requests, timeout_ms))
            cold_nfd_end = snapshot_nfd_data_counters(ndn)
            cold_traffic_end = snapshot_traffic(ndn)
            write_traffic_delta(layout, "cold", cold_traffic_start, cold_traffic_end,
                                cold_requests)
            write_nfd_data_delta(layout, "cold", cold_nfd_start, cold_nfd_end,
                                 cold_requests)
            cold_text = user_log.read_text(errors="replace")
            print_user_workload_output(cold_text, args_cli.quiet_perf_logs)
            cold_latencies = write_latency_summary(layout, "cold", cold_text)
            if len(cold_latencies) < cold_requests or "ok=false" in cold_text:
                raise RuntimeError(
                    f"YOLO {layout} cold provisioning failed or returned too few results "
                    f"count={len(cold_latencies)} expected={cold_requests} "
                    f"rc={user_proc.returncode}; log={user_log}")
            phase_logs.append(("cold", user_log))
        warm_log = None
        warm_latencies: list[float] = []
        measured_warm_count = 0
        skip_warm_phase = warm_requests <= 0 and warm_duration_s <= 0 and not signing_ab_phases
        if signing_ab_phases:
            remove_provider_procs(provider_procs)
            provider_procs = []
            for index, mode in enumerate(signing_ab_phases, start=1):
                phase = f"warm-{mode}-{index}"
                phase_env = signing_phase_env(mode)
                provider_procs = start_compute_providers(
                    f"-{phase}", phase_env)
                procs.extend(provider_procs)
                log_path, latencies, measured_count = run_warm_phase(
                    phase,
                    phase_env,
                    trace_for_phase=(index == 1))
                phase_logs.append((phase, log_path))
                warm_log = log_path
                warm_latencies = latencies
                measured_warm_count = measured_count
                remove_provider_procs(provider_procs)
                provider_procs = []
            # Keep providers alive after the comparison for consistent teardown
            # and for any late summary hooks that inspect provider logs.
            provider_procs = start_compute_providers("", env)
            procs.extend(provider_procs)
        elif not skip_warm_phase:
            warm_log, warm_latencies, measured_warm_count = run_warm_phase(
                "warm",
                env,
                trace_for_phase=True)
            phase_logs.append(("warm", warm_log))
        write_plan_cache_summary(layout, [
            *phase_logs,
        ])
        write_client_timing_summaries(layout, phase_logs)
        if args_cli.crypto_timing:
            write_hybrid_crypto_timing_summaries(layout, phase_logs, providers)
        if args_cli.control_timing:
            write_control_timing_summaries(layout, phase_logs, providers, service_name)
            write_outer_control_waterfall_summaries(layout, phase_logs, providers, service_name)
            write_outer_control_rtt_correlation_summary(layout)
        if args_cli.svs_internal_timing:
            write_svs_internal_timing_summaries(layout, phase_logs, providers)
        if args_cli.control_trace:
            write_ack_selection_timing_summaries(layout, phase_logs)
            write_provider_selection_timing_summaries(layout, providers)
            write_provider_request_ack_timing_summaries(layout, providers)
            write_control_path_timing_summaries(layout, phase_logs, providers)
        if args_cli.control_timing or args_cli.control_trace:
            write_svs_control_propagation_summaries(layout, phase_logs, providers)
            write_selection_direct_prefetch_summaries(layout, phase_logs, providers)
        if packet_traces:
            stop_ndn_packet_traces(packet_traces)
            write_ndn_packet_trace_summary(packet_traces)
        native_dataflow_transport_ok = write_provider_timing_summaries(
            layout,
            providers,
            require_runtime_timing=bool(env.get("NDNSF_DI_RUNTIME_TIMING")),
        )
        write_end_to_end_breakdown(layout, print_rows=not args_cli.quiet_perf_logs)
        if args_cli.control_timing:
            write_native_session_breakdown_summary(layout)
        provider_text = "\n".join(
            (OUT / f"{name}.log").read_text(errors="replace")
            for _, name, _ in providers
        )
        warm_text = warm_log.read_text(errors="replace") if warm_log else ""
        if skip_warm_phase:
            common_success = bool(cold_latencies) and "ok=false" not in cold_text
        else:
            common_success = (
                len(warm_latencies) >= (1 if warm_duration_s > 0 else warm_requests) and
                "ok=false" not in warm_text
            )
        if args_cli.native_providers:
            success = (
                common_success and
                "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY" in provider_text and
                native_dataflow_transport_ok
            )
        else:
            success = (
                common_success and
                "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS" in provider_text and
                "NDNSF_EXECUTION_ARTIFACT_CACHE_HIT" in provider_text
            )
        if not success:
            raise RuntimeError(
                f"YOLO {layout} dynamic provisioning/cache validation failed; "
                f"cold={user_log} warm={warm_log}")
        if args_cli.native_providers:
            print(
                f"YOLO_LAYOUT_NATIVE_PROVIDERS_MININDN_OK "
                f"layout={layout} cold={user_log} warm={warm_log}"
            )
            if layout == "2x2":
                print(f"YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK cold={user_log} warm={warm_log}")
        else:
            print(
                f"YOLO_LAYOUT_DYNAMIC_PROVISIONING_MININDN_OK "
                f"layout={layout} cold={user_log} warm={warm_log}"
            )
            if layout == "2x2":
                print(f"YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK cold={user_log} warm={warm_log}")
    finally:
        if 'packet_traces' in locals():
            stop_ndn_packet_traces(packet_traces)
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
