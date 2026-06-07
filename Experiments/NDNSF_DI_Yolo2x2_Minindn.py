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
import time
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

TOPO = REPO / "Experiments/Topology/AI_testbed.conf"
OUT = REPO / "results/yolo_2x2_minindn_quick"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"
CONFIG = OUT / "yolo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-yolo-2x2-policy"
REPO_MANIFEST = OUT / "repo-manifests.json"
APP_ROOT = "/NDNSF-DistributeInference/example"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"


class Args(SimpleNamespace):
    pass


def log(message: str) -> None:
    info(message + "\n")


def python_cmd(script: str, argv: list[str]) -> str:
    args = " ".join([perf.shell_quote(str(PY_DIR / script))] +
                    [perf.shell_quote(arg) for arg in argv])
    return f"cd {perf.shell_quote(REPO)} && exec python3 {args}"


def start(node, name, cmd, env, procs):
    path = OUT / f"{name}.log"
    f = path.open("wb")
    log(f"start {name} on {node.name}: {cmd}")
    node_env = dict(env)
    node_env["NDNSF_ARTIFACT_CACHE_DIR"] = str(OUT / "artifact-cache" / node.name)
    p = getPopen(node, cmd, envDict=node_env, shell=True, stdout=f, stderr=subprocess.STDOUT)
    procs.append((p, f, path))
    return p, path


def wait_log(path: Path, needle: str, timeout: int = 30, proc=None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def prepare_fractional_delay_routing_costs(net) -> None:
    """Keep sub-ms Mininet links but give MiniNDN routing integer costs.

    MiniNDN can create TC links such as delay=0.1ms, but its
    NdnRoutingHelper currently parses the topology metadata with int(...).
    The real link has already been created by this point, so only the routing
    cost metadata needs to be rounded up to a valid integer millisecond value.
    """
    patched = []
    for src, dst, info_dict in net.topo.links(withInfo=True):
        delay = str(info_dict.get("delay", "0ms"))
        if not delay.endswith("ms"):
            continue
        value = delay[:-2]
        try:
            delay_ms = float(value)
        except ValueError:
            continue
        if delay_ms <= 0 or delay_ms.is_integer():
            continue
        route_cost_ms = max(1, int(delay_ms + 0.999999))
        info_dict["ndnsf_actual_delay"] = delay
        info_dict["delay"] = f"{route_cost_ms}ms"
        patched.append((src, dst, delay, info_dict["delay"]))
    for src, dst, actual, route_cost in patched:
        log(
            "NDNSF_DI_ROUTING_DELAY_COST_PATCH "
            f"link={src}:{dst} actual_delay={actual} route_cost_delay={route_cost}"
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
                        request_count: int = 1) -> dict:
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
    summary = {
        "layout": layout,
        "phase": phase,
        "requestCount": max(1, request_count),
        "rxBytes": total_rx,
        "txBytes": total_tx,
        "totalNodeBytes": total_rx + total_tx,
        "totalNodeBytesPerRequest": (total_rx + total_tx) / max(1, request_count),
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
        f"request_count={max(1, request_count)} "
        f"rx_bytes={total_rx} tx_bytes={total_tx} "
        f"total_node_bytes={total_rx + total_tx} "
        f"total_node_bytes_per_inference={(total_rx + total_tx) / max(1, request_count):.1f} "
        f"total_node_bytes_per_request={(total_rx + total_tx) / max(1, request_count):.1f} "
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
                         request_count: int = 1) -> dict:
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
    out_data = totals["nOutData"]
    in_data = totals["nInData"]
    summary = {
        "layout": layout,
        "phase": phase,
        "requestCount": request_count,
        "nInData": in_data,
        "nOutData": out_data,
        "nInBytes": totals["nInBytes"],
        "nOutBytes": totals["nOutBytes"],
        "nOutDataPerRequest": out_data / request_count,
        "nOutBytesPerRequest": totals["nOutBytes"] / request_count,
        "avgNfdOutBytesPerOutData": (totals["nOutBytes"] / out_data) if out_data else 0.0,
        "notes": [
            "Data packet counts use NFD network-face nOutData/nInData counters.",
            "NFD nOutBytes/nInBytes are face byte counters for all packet types, "
            "so avgNfdOutBytesPerOutData is an approximate transport-size ratio, "
            "not a Data-only wire-size measurement.",
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
        f"n_out_data={out_data} n_in_data={in_data} "
        f"n_out_data_per_request={out_data / request_count:.2f} "
        f"data_packets_per_inference={out_data / request_count:.2f} "
        f"n_out_bytes={totals['nOutBytes']} "
        f"n_out_bytes_per_request={totals['nOutBytes'] / request_count:.1f} "
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


def parse_numeric_prefix(value, default: float = 0.0) -> float:
    match = re.match(r"[-+]?[0-9]+(?:\.[0-9]+)?", str(value or ""))
    return float(match.group(0)) if match else default


def parse_int_prefix(value, default: int = 0) -> int:
    return int(parse_numeric_prefix(value, float(default)))


def write_provider_timing_summaries(layout: str,
                                    providers: list[tuple[str, str, list[str]]]) -> None:
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
                row["planned_name"] = str(row.get("planned_name", "false"))
                row["providerLog"] = path.name
                dependency_rows.append(row)
            elif "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING" in line:
                row = parse_key_value_line(line)
                row["publish_ms"] = parse_numeric_prefix(row.get("publish_ms", "0"))
                row["bytes"] = parse_int_prefix(row.get("bytes", "0"))
                row["expected_segments"] = parse_int_prefix(row.get("expected_segments", "0"))
                row["output_ready_epoch_ms"] = parse_int_prefix(row.get("output_ready_epoch_ms", "0"))
                row["publish_done_epoch_ms"] = parse_int_prefix(row.get("publish_done_epoch_ms", "0"))
                row["providerLog"] = path.name
                output_rows.append(row)
            elif "NDNSF_COLLAB_LARGE_FETCH_TIMING" in line:
                row = parse_key_value_line(line)
                for key in (
                    "elapsed_ms",
                    "first_segment_ms",
                    "last_segment_received_ms",
                    "last_segment_validated_ms",
                ):
                    if key in row:
                        row[key] = parse_numeric_prefix(row.get(key, "0"))
                for key in (
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
                for key in ("queue_wait_ms", "handler_ms"):
                    if key in row:
                        row[key] = parse_numeric_prefix(row.get(key, "0"))
                for key in ("submitted_epoch_ms", "start_epoch_ms", "end_epoch_ms"):
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
        "bytes": summarize_numeric([float(row["bytes"]) for row in dependency_rows]),
        "expectedSegments": summarize_numeric([
            float(row["expected_segments"]) for row in dependency_rows
        ]),
        "futureWaitMs": summarize_numeric([row["future_wait_ms"] for row in dependency_rows]),
        "referenceWaitMs": summarize_numeric([row["ref_wait_ms"] for row in dependency_rows]),
        "fetchMs": summarize_numeric([row["fetch_ms"] for row in dependency_rows]),
        "decodeMs": summarize_numeric([row["decode_ms"] for row in dependency_rows]),
        "prefetchTotalMs": summarize_numeric([row["prefetch_total_ms"] for row in dependency_rows]),
        "prefetchOverlapMs": summarize_numeric([
            row["prefetch_overlap_ms"] for row in dependency_rows
        ]),
        "plannedNameFetches": sum(1 for row in dependency_rows
                                  if row.get("planned_name") == "true"),
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
            f"planned_name_fetches={sum(1 for row in rows if row.get('planned_name') == 'true')} "
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
            f"planned_name_fetches={sum(1 for row in rows if row.get('planned_name') == 'true')} "
            f"decode_sum_ms={decode_sum:.2f}"
        )

    output_summary = {
        "layout": layout,
        "count": len(output_rows),
        "totalBytes": sum(row["bytes"] for row in output_rows),
        "totalExpectedSegments": sum(row["expected_segments"] for row in output_rows),
        "bytes": summarize_numeric([float(row["bytes"]) for row in output_rows]),
        "expectedSegments": summarize_numeric([
            float(row["expected_segments"]) for row in output_rows
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
    collab_fetch_summary = {
        "layout": layout,
        "count": len(collab_fetch_rows),
        "complete": len(complete_fetch_rows),
        "errors": len(error_fetch_rows),
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
        "nacks": sum(row.get("nacks", 0) for row in collab_fetch_rows),
        "segmentTimeouts": sum(row.get("segment_timeouts", 0) for row in collab_fetch_rows),
        "interestLifetimeMs": summarize_numeric([
            float(row.get("interest_lifetime_ms", 0)) for row in collab_fetch_rows
            if "interest_lifetime_ms" in row
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
        f"segment_timeouts={collab_fetch_summary['segmentTimeouts']} "
        f"encoded_bytes_p50={collab_fetch_summary['encodedBytes']['p50']:.0f} "
        f"interest_lifetime_p50_ms={collab_fetch_summary['interestLifetimeMs']['p50']:.0f} "
        f"path={collab_fetch_path}"
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
        if row.get("scope") != "detect-heads-to-merge":
            continue
        session = _session_from_data_name(str(row.get("dataName", "")))
        if session:
            merge_sessions.setdefault(session, []).append(row)
    merge_batch_rows = []
    for session, rows in sorted(merge_sessions.items()):
        first_values = [float(row["readyToFirstSegmentMs"]) for row in rows]
        complete_values = [float(row["readyToCompleteMs"]) for row in rows]
        merge_batch_rows.append({
            "session": session,
            "inputCount": len(rows),
            "completeInputSet": len(rows) >= 3,
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
        "handlerMs": summarize_numeric([
            row["handler_ms"] for row in handler_end_rows
            if "handler_ms" in row
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
        dataflow_rows.append({
            "session": session,
            "roleCount": len(roles),
            "roles": roles,
            "startEpochMs": min(starts),
            "endEpochMs": max(ends),
            "dataflowMs": max(ends) - min(starts),
        })
    handler_summary["dataflow"] = {
        "count": len(dataflow_rows),
        "dataflowMs": summarize_numeric([
            float(row["dataflowMs"]) for row in dataflow_rows
        ]),
        "rows": dataflow_rows,
    }
    handler_path = OUT / "provider-handler-timing-stats.json"
    handler_path.write_text(
        json.dumps(handler_summary, indent=2, sort_keys=True),
        encoding="utf-8")
    print(
        "YOLO_LAYOUT_PROVIDER_HANDLER_TIMING "
        f"layout={layout} starts={handler_summary['starts']} "
        f"ends={handler_summary['ends']} "
        f"queue_wait_p50_ms={handler_summary['queueWaitMs']['p50']:.2f} "
        f"handler_p50_ms={handler_summary['handlerMs']['p50']:.2f} "
        f"dataflow_p50_ms={handler_summary['dataflow']['dataflowMs']['p50']:.2f} "
        f"path={handler_path}"
    )


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
        rows.append({
            "phase": phase,
            "log": str(path),
            "entries": entries,
            "hits": sum(1 for entry in entries if entry.get("hit") == "true"),
            "misses": sum(1 for entry in entries if entry.get("hit") == "false"),
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
        f"path={path}"
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


def provider_role_assignments(roles: list[str]) -> list[tuple[str, str, list[str]]]:
    nodes = ["ucla", "wustl", "uiuc", "umich", "arizona", "caida", "pku", "neu", "csu"]
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


def initialize_di_keychains(ndn, output_dir: Path, provider_identities: list[str]) -> None:
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

    controller = ndn.net["csu"]
    root_cert_path = security_dir / "root.cert"
    perf.node_cmd(controller, "ndnsec key-gen -t r {} > {}".format(
        perf.shell_quote(APP_ROOT), perf.shell_quote(root_cert_path)))
    perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
        perf.shell_quote(root_cert_path)))
    log("di_root_cert identity={} name={} file={}".format(
        APP_ROOT, perf.certificate_name_from_file(root_cert_path), root_cert_path))

    exported_keys = []
    for index, identity in enumerate(identities):
        cert_path = security_dir / f"di-identity-{index}.cert"
        req_path = security_dir / f"di-identity-{index}.req"
        key_path = security_dir / f"di-identity-{index}.ndnkey"
        perf.node_cmd(controller, "ndnsec key-gen -n -t r {} > {}".format(
            perf.shell_quote(identity), perf.shell_quote(req_path)))
        perf.node_cmd(controller, "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
            perf.shell_quote(APP_ROOT), perf.shell_quote(req_path), perf.shell_quote(cert_path)))
        perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(cert_path)))
        perf.node_cmd(controller, "ndnsec-export -P 123456 -o {} -i {}".format(
            perf.shell_quote(key_path), perf.shell_quote(identity)))
        exported_keys.append(key_path)

    for node in ndn.net.hosts:
        perf.node_cmd(node, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(root_cert_path)))
        for key_path in exported_keys:
            perf.node_cmd(node, "ndnsec import -P 123456 {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(key_path)))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", default="2x2",
                        help="YOLO stage-by-shard layout, e.g. 1x3, 2x3, 3x2, 3x3")
    parser.add_argument("--cold-requests", type=int, default=1,
                        help="Sequential requests in the cold user process")
    parser.add_argument("--warm-requests", type=int, default=1,
                        help="Sequential requests in the warm user process")
    parser.add_argument("--warm-duration-s", type=float, default=0.0,
                        help="Run the warm user for this many seconds instead of a fixed request count")
    parser.add_argument("--warm-interval-ms", type=int, default=0,
                        help="Minimum interval between warm sequential request starts")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500,
                        help="ACK collection timeout passed to the DI user")
    parser.add_argument("--timeout-ms", type=int, default=60000,
                        help="End-to-end service timeout passed to the DI user")
    parser.add_argument("--provider-handler-workers", type=int, default=2,
                        help="Python worker count passed to each DI provider")
    parser.add_argument("--user-async-workers", type=int, default=1,
                        help="Async worker count passed to the DI user process")
    parser.add_argument("--parallel-output-shards", action="store_true",
                        help="Use the experimental true-NxM YOLO output-shard prototype")
    parser.add_argument("--parallel-detect-scale-shards", action="store_true",
                        help="Use the YOLO Detect-scale DAG splitter")
    args_cli = parser.parse_args()
    if args_cli.parallel_output_shards and args_cli.parallel_detect_scale_shards:
        raise SystemExit("--parallel-output-shards and --parallel-detect-scale-shards are mutually exclusive")
    layout = args_cli.layout.strip().lower().replace("*", "x")
    cold_requests = max(1, args_cli.cold_requests)
    warm_requests = max(1, args_cli.warm_requests)
    warm_duration_s = max(0.0, float(args_cli.warm_duration_s or 0.0))
    warm_interval_ms = max(0, args_cli.warm_interval_ms)
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
    OUT = REPO / f"results/yolo_{safe_layout}_minindn_quick"
    CONFIG = OUT / "yolo_policy.yaml"
    GEN_POLICY = f"/tmp/ndnsf-di-yolo-{safe_layout}-policy"
    REPO_MANIFEST = OUT / "repo-manifests.json"

    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    for stats_path in (
            OUT / "traffic-stats.json",
            OUT / "inference-latency-stats.json",
            OUT / "nfd-data-stats.json"):
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
    subprocess.run(split_command, cwd=str(REPO), env={**os.environ, "PYTHONPATH": py_path}, check=True)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    args = Args(
        controller_node="csu",
        user_node="memphis",
        providers=5,
        provider_nodes="ucla,wustl,uiuc,umich,neu",
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
        performance_mode=False,
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

        prepare_fractional_delay_routing_costs(ndn.net)
        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net["csu"]], [
            "/NDNSF-DistributeInference/example/controller",
            "/NDNSF-DistributeInference/example/controller/DKEY",
            "/NDNSF-DistributeInference/example/controller/KEY",
            "/NDNSF-DistributeInference/example/group",
        ])
        rh.addOrigin([ndn.net["memphis"]], ["/NDNSF-DistributeInference/example/user", "/NDNSF-DistributeInference/example/group"])
        origins = [
            (
                node_name,
                provider_identity(argv[argv.index("--provider-id") + 1]),
            )
            for node_name, _, argv in providers
        ]
        origins.append(("neu", "/NDNSF-DistributeInference/example/provider/D"))
        for node_name, prefix in origins:
            rh.addOrigin([ndn.net[node_name]], [prefix, prefix + "/KEY", "/NDNSF-DistributeInference/example/group"])
        rh.addOrigin([ndn.net["neu"]], ["/NDNSF/DistributedRepo/Object"])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        initialize_di_keychains(ndn, OUT, provider_identities)
        subprocess.run(["rm", "-rf", str(OUT / "artifact-cache")], check=False)
        session = int(time.time()) + os.getpid()
        env = perf.app_env(OUT, session, args)
        env["NDNSF_HANDLER_THREADS"] = "1"
        env["NDNSF_ACK_THREADS"] = "1"
        env["NDNSF_SVS_ASYNC_PUBLISH"] = "0"
        env["NDNSF_SVS_PARALLEL_SYNC"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = "0"
        env["NDNSF_COLLAB_LARGE_FETCH_TIMING"] = "1"
        env["NDNSF_PENDING_IMS_TIMING"] = "1"
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(PY_DIR),
            "/home/tianxing/.local/lib/python3.8/site-packages",
            "/usr/local/lib/python3.8/dist-packages",
            "/usr/lib/python3/dist-packages",
            os.environ.get("PYTHONPATH", ""),
        ])

        common = ["--config", str(CONFIG), "--generated-policy-dir", GEN_POLICY]
        _, controller_log = start(ndn.net["csu"], "controller",
                                  python_cmd("controller.py", common), env, procs)
        if not wait_log(controller_log, "ServiceController listening", 20):
            raise RuntimeError(f"controller did not become ready; see {controller_log}")
        time.sleep(4)
        _, repo_log = start(
            ndn.net["neu"],
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
        deployer_proc, deployer_log = start(ndn.net["csu"], "controller-deployer",
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

        for node_name, name, argv in providers:
            _, lp = start(ndn.net[node_name], name,
                          python_cmd("provider.py", common + argv + [
                              "--dynamic-provisioning",
                              "--temp-dir",
                              f"/tmp/{name}",
                              "--handler-workers",
                              str(provider_handler_workers),
                          ]),
                          env, procs)
            if not wait_log(lp, "Installed provider permission", 20):
                raise RuntimeError(f"{name} did not install permissions; see {lp}")
            time.sleep(0.5)

        time.sleep(2)
        user_common = common + [
            "--repo-manifest-file",
            str(REPO_MANIFEST),
            "--ack-timeout-ms", str(ack_timeout_ms),
            "--timeout-ms", str(timeout_ms),
            "--async-requests", str(user_async_workers),
            "--sequential-requests", str(cold_requests),
        ]
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
        print(cold_text)
        cold_latencies = write_latency_summary(layout, "cold", cold_text)
        if len(cold_latencies) < cold_requests or "ok=false" in cold_text:
            raise RuntimeError(
                f"YOLO {layout} cold provisioning failed or returned too few results "
                f"count={len(cold_latencies)} expected={cold_requests} "
                f"rc={user_proc.returncode}; log={user_log}")

        warm_traffic_start = snapshot_traffic(ndn)
        warm_nfd_start = snapshot_nfd_data_counters(ndn)
        warm_user_args = common + [
            "--repo-manifest-file",
            str(REPO_MANIFEST),
            "--ack-timeout-ms", str(ack_timeout_ms),
            "--timeout-ms", str(timeout_ms),
            "--async-requests", str(user_async_workers),
        ]
        if warm_duration_s > 0:
            warm_user_args.extend([
                "--sequential-duration-s", str(warm_duration_s),
                "--sequential-interval-ms", str(warm_interval_ms),
            ])
        else:
            warm_user_args.extend(["--sequential-requests", str(warm_requests)])
        warm_proc, warm_log = start(
            ndn.net["memphis"],
            "user-warm",
            python_cmd("user.py", warm_user_args),
            env,
            procs,
        )
        warm_proc.wait(timeout=user_wait_timeout(warm_requests, timeout_ms, warm_duration_s))
        warm_nfd_end = snapshot_nfd_data_counters(ndn)
        warm_traffic_end = snapshot_traffic(ndn)
        warm_text = warm_log.read_text(errors="replace")
        warm_latencies = write_latency_summary(layout, "warm", warm_text)
        warm_count = len(warm_latencies)
        write_traffic_delta(layout, "warm", warm_traffic_start, warm_traffic_end,
                            warm_count or warm_requests)
        write_nfd_data_delta(layout, "warm", warm_nfd_start, warm_nfd_end,
                             warm_count or warm_requests)
        print(warm_text)
        write_plan_cache_summary(layout, [
            ("cold", user_log),
            ("warm", warm_log),
        ])
        write_client_timing_summaries(layout, [
            ("cold", user_log),
            ("warm", warm_log),
        ])
        write_provider_timing_summaries(layout, providers)
        provider_text = "\n".join(
            (OUT / f"{name}.log").read_text(errors="replace")
            for _, name, _ in providers
        )
        success = (
            len(warm_latencies) >= (1 if warm_duration_s > 0 else warm_requests) and
            "ok=false" not in warm_text and
            "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS" in provider_text and
            "NDNSF_EXECUTION_ARTIFACT_CACHE_HIT" in provider_text
        )
        if not success:
            raise RuntimeError(
                f"YOLO {layout} dynamic provisioning/cache validation failed; "
                f"cold={user_log} warm={warm_log}")
        print(
            f"YOLO_LAYOUT_DYNAMIC_PROVISIONING_MININDN_OK "
            f"layout={layout} cold={user_log} warm={warm_log}"
        )
        if layout == "2x2":
            print(f"YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK cold={user_log} warm={warm_log}")
    finally:
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
