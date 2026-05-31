#!/usr/bin/env python3

import argparse
import json
import math
import os
import random
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace

from mininet.log import info, setLogLevel
from mn_wifi.link import wmediumd
from mn_wifi.wmediumdConnector import interference
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.nfdc import Nfdc
from minindn.minindn import Minindn
from minindn.wifi.minindnwifi import MinindnWifi
from minindn.util import getPopen

import NDNSF_NewAPI_Minindn_Perf as perf


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_CONTROLLER = REPO_ROOT / "build/examples/App_ServiceController"
APP_PROVIDER = REPO_ROOT / "build/examples/App_WifiMobilityProvider"
APP_USER = REPO_ROOT / "build/examples/App_WifiMobilityUser"
GRPC_DIR = REPO_ROOT / "Experiments/gRPC"
NSC_DIR = REPO_ROOT / "Experiments/NDN_NSC"


def log(message):
    info(f"{message}\n")


def parse_csv_floats(value):
    return [float(item) for item in value.split(",") if item.strip()]


def provider_nodes():
    return ["ucla", "wustl", "uiuc"]


def all_app_nodes(ndn):
    return [ndn.net["memphis"], ndn.net["ucla"], ndn.net["wustl"], ndn.net["uiuc"]]


def make_perf_args():
    return SimpleNamespace(
        providers=3,
        provider_nodes="ucla,wustl,uiuc",
        user_node="memphis",
        controller_node="memphis",
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


def start_process(node, name, command, output_dir, env=None):
    log_path = output_dir / f"{name}.log"
    log(f"Starting {name} on {node.name} -> {log_path}")
    log_file = log_path.open("wb")
    process = getPopen(node, command, envDict=env, shell=True,
                       stdout=log_file, stderr=subprocess.STDOUT)
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


class RandomWaypointCoverage:
    def __init__(self, provider_names, seed, area_size=400.0, min_speed=1.0,
                 max_speed=2.0, ap_x=200.0, ap_y=200.0, initial_radius=50.0):
        self.rng = random.Random(seed)
        self.area_size = area_size
        self.min_speed = min_speed
        self.max_speed = max_speed
        self.states = {}
        for name in provider_names:
            angle = self.rng.uniform(0.0, 2.0 * math.pi)
            self.states[name] = {
                "x": ap_x + math.cos(angle) * initial_radius,
                "y": ap_y + math.sin(angle) * initial_radius,
                "target_x": self.rng.uniform(0, area_size),
                "target_y": self.rng.uniform(0, area_size),
                "speed": self.rng.uniform(min_speed, max_speed),
            }

    def step(self, dt):
        for state in self.states.values():
            dx = state["target_x"] - state["x"]
            dy = state["target_y"] - state["y"]
            dist = math.hypot(dx, dy)
            travel = state["speed"] * dt
            if dist <= travel or dist < 1e-6:
                state["x"] = state["target_x"]
                state["y"] = state["target_y"]
                state["target_x"] = self.rng.uniform(0, self.area_size)
                state["target_y"] = self.rng.uniform(0, self.area_size)
                state["speed"] = self.rng.uniform(self.min_speed, self.max_speed)
            else:
                state["x"] += dx / dist * travel
                state["y"] += dy / dist * travel

    def snapshot(self, ap_x=200.0, ap_y=200.0):
        rows = []
        for name, state in self.states.items():
            distance = math.hypot(state["x"] - ap_x, state["y"] - ap_y)
            rows.append((name, state["x"], state["y"], distance))
        return rows


def start_coverage_gate(ndn, output_dir, ap_range, stop_event, seed,
                        interval_s=1.0, block_network=False, pause_processes=None):
    providers = [ndn.net[name] for name in provider_nodes()]
    mobility = RandomWaypointCoverage(provider_nodes(), seed)
    trace_path = output_dir / "mobility_trace.csv"
    status_dir = output_dir / "availability"
    status_dir.mkdir(parents=True, exist_ok=True)
    in_range = {}
    pause_processes = pause_processes or {}

    def apply_gate(node, allowed):
        iface = f"{node.name}-wlan0"
        node.cmd("iptables -F NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
        node.cmd("iptables -N NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
        node.cmd("iptables -C INPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || "
                 "iptables -I INPUT 1 -j NDNSF_WIFI_RANGE")
        node.cmd("iptables -C OUTPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || "
                 "iptables -I OUTPUT 1 -j NDNSF_WIFI_RANGE")
        if not allowed:
            node.cmd(f"iptables -A NDNSF_WIFI_RANGE -i {iface} -j DROP")
            node.cmd(f"iptables -A NDNSF_WIFI_RANGE -o {iface} -j DROP")

    def cleanup_gate():
        for node in providers:
            node.cmd("iptables -D INPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
            node.cmd("iptables -D OUTPUT -j NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
            node.cmd("iptables -F NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")
            node.cmd("iptables -X NDNSF_WIFI_RANGE >/dev/null 2>&1 || true")

    def run():
        with trace_path.open("w") as out:
            out.write("time_s,provider,x,y,distance_m,in_range\n")
            start = time.time()
            while not stop_event.is_set():
                now = time.time() - start
                for name, x, y, distance in mobility.snapshot():
                    allowed = distance <= ap_range
                    node = ndn.net[name]
                    if in_range.get(name) != allowed:
                        if block_network:
                            apply_gate(node, allowed)
                        process = pause_processes.get(name)
                        if process is not None and process.poll() is None:
                            process.send_signal(signal.SIGCONT if allowed else signal.SIGSTOP)
                        in_range[name] = allowed
                    (status_dir / f"{name}.state").write_text("1\n" if allowed else "0\n")
                    out.write(f"{now:.3f},{name},{x:.2f},{y:.2f},{distance:.2f},{int(allowed)}\n")
                    out.flush()
                mobility.step(interval_s)
                stop_event.wait(interval_s)
        if block_network:
            cleanup_gate()
        for process in pause_processes.values():
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGCONT)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, trace_path, status_dir


def node_cmd(node, command):
    home = node.params["params"]["homeDir"]
    return node.cmd(f"HOME={perf.shell_quote(home)} {command}")


def initialize_keychains(nodes, controller, output_dir):
    security_dir = output_dir / "security"
    security_dir.mkdir(parents=True, exist_ok=True)
    identity_owners = {
        "/example/hello/controller": "memphis",
        "/example/hello/user": "memphis",
        "/example/hello/provider": "ucla",
        "/example/hello/provider/A": "ucla",
        "/example/hello/provider/B": "wustl",
        "/example/hello/provider/C": "uiuc",
    }
    nodes_by_name = {node.name: node for node in nodes}
    for node in nodes:
        for identity in ["/example/hello"] + list(identity_owners):
            node_cmd(node, f"ndnsec delete {perf.shell_quote(identity)} >/dev/null 2>&1 || true")
    root_cert = security_dir / "root.cert"
    node_cmd(controller, f"ndnsec key-gen -t r /example/hello > {perf.shell_quote(root_cert)}")
    node_cmd(controller, f"ndnsec cert-install -f {perf.shell_quote(root_cert)} >/dev/null 2>&1 || true")
    certs = []
    for index, (identity, owner_name) in enumerate(identity_owners.items()):
        owner = nodes_by_name[owner_name]
        cert = security_dir / f"identity-{index}.cert"
        req = security_dir / f"identity-{index}.req"
        node_cmd(owner, f"ndnsec key-gen -n -t r {perf.shell_quote(identity)} > {perf.shell_quote(req)}")
        node_cmd(controller, f"ndnsec cert-gen -s /example/hello -i ROOT {perf.shell_quote(req)} > {perf.shell_quote(cert)}")
        certs.append(cert)
    for node in nodes:
        node_cmd(node, f"ndnsec cert-install -f {perf.shell_quote(root_cert)} >/dev/null 2>&1 || true")
        for cert in certs:
            node_cmd(node, f"ndnsec cert-install -f {perf.shell_quote(cert)} >/dev/null 2>&1 || true")


def configure_ndn_multicast(nodes):
    identity_owners = {
        "/example/hello/controller": "memphis",
        "/example/hello/user": "memphis",
        "/example/hello/provider/A": "ucla",
        "/example/hello/provider/B": "wustl",
        "/example/hello/provider/C": "uiuc",
        "/muas/memphis": "memphis",
        "/muas/ucla": "ucla",
        "/muas/wustl": "wustl",
        "/muas/uiuc": "uiuc",
    }
    group_prefixes = ["/example/hello/group", "/muas"]

    face_uri = {}
    for node in nodes:
        for peer in nodes:
            if peer.name == node.name:
                continue
            uri = f"udp4://{peer.IP()}"
            face_uri[(node.name, peer.name)] = uri
            node.cmd(f"nfdc face create {uri} >/dev/null 2>&1 || true")

    for node in nodes:
        for prefix in group_prefixes:
            node.cmd(f"nfdc strategy set {prefix} /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true")
            for peer in nodes:
                if peer.name != node.name:
                    node.cmd(
                        f"nfdc route add prefix {prefix} nexthop {face_uri[(node.name, peer.name)]} cost 100 "
                        ">/dev/null 2>&1 || true")
        for prefix, owner_name in identity_owners.items():
            node.cmd(f"nfdc strategy set {prefix} /localhost/nfd/strategy/best-route >/dev/null 2>&1 || true")
            if owner_name != node.name:
                node.cmd(
                    f"nfdc route add prefix {prefix} nexthop {face_uri[(node.name, owner_name)]} cost 10 "
                    ">/dev/null 2>&1 || true")


def build_wifi_topology(ap_range, seed):
    Minindn.cleanUp()
    original_argv = sys.argv[:]
    try:
        sys.argv = [sys.argv[0]]
        Minindn.verifyDependencies()
        ndn = MinindnWifi(noTopo=True, link=wmediumd,
                          wmediumd_mode=interference)
    finally:
        sys.argv = original_argv
    net = ndn.net
    net.setPropagationModel(model="logDistance", exp=3)
    memphis = net.addHost("memphis", ip="10.0.0.1/24")
    ucla = net.addStation("ucla", ip="10.0.0.2/24", position="190,200,0")
    wustl = net.addStation("wustl", ip="10.0.0.3/24", position="200,190,0")
    uiuc = net.addStation("uiuc", ip="10.0.0.4/24", position="210,200,0")
    ap1 = net.addAccessPoint("ap1", ssid="ndnsf-wifi", mode="g", channel="1",
                             position="200,200,0", range=int(ap_range))
    c0 = net.addController("c0")
    net.configureNodes()
    net.addLink(memphis, ap1)
    net.addLink(ucla, ap1)
    net.addLink(wustl, ap1)
    net.addLink(uiuc, ap1)
    net.build()
    c0.start()
    ap1.start([c0])
    ndn.initParams([memphis, ucla, wustl, uiuc])
    time.sleep(3)
    return ndn


def app_env(output_dir, session_base):
    return perf.app_env(output_dir, session_base, make_perf_args())


def run_ndnsf(ndn, output_dir, args):
    processes = []
    gate_stop = threading.Event()
    gate_thread = None
    nodes = all_app_nodes(ndn)
    session_base = int(time.time()) + os.getpid()
    try:
        initialize_keychains(nodes, ndn.net["memphis"], output_dir)
        configure_ndn_multicast(nodes)
        env = app_env(output_dir, session_base)
        controller, lf, lp = start_process(
            ndn.net["memphis"], "ndnsf-controller",
            perf.managed_cmd(APP_CONTROLLER, []), output_dir, env)
        processes.append((controller, lf, lp))
        if not wait_for_log(lp, r"ServiceController listening on:", 20, controller):
            raise RuntimeError(f"controller not ready; see {lp}")
        time.sleep(3)
        for index, node_name in enumerate(provider_nodes()):
            provider_id = chr(ord("A") + index)
            proc, lf, lp = start_process(
                ndn.net[node_name], f"ndnsf-provider-{provider_id}",
                perf.managed_cmd(APP_PROVIDER, [
                    "--provider-id", provider_id,
                    "--failure-probability", "0",
                    "--availability-file", str(output_dir / "availability" / f"{node_name}.state"),
                    "--processing-delay-ms", str(args.processing_delay_ms),
                    "--handler-threads", "4",
                ]), output_dir, env)
            processes.append((proc, lf, lp))
            if not wait_for_log(lp, r"INTERMITTENT_PROVIDER_READY", 30, proc):
                raise RuntimeError(f"provider {provider_id} not ready; see {lp}")
            time.sleep(1)
        time.sleep(args.settle_seconds)
        gate_thread, trace_path, _ = start_coverage_gate(
            ndn, output_dir, args.ap_range, gate_stop, args.seed, block_network=False)
        log(f"RandomWaypoint coverage trace -> {trace_path}")
        user, lf, user_log = start_process(
            ndn.net["memphis"], "ndnsf-user",
            perf.managed_cmd(APP_USER, [
                "--rate-rps", str(args.rate_rps),
                "--duration-ms", str(args.duration_s * 1000),
                "--ack-timeout-ms", str(args.ack_timeout_ms),
                "--timeout-ms", str(args.timeout_ms),
                "--strategy", "all-selected",
                "--startup-delay-ms", "2000",
            ]), output_dir, env)
        processes.append((user, lf, user_log))
        user.wait(timeout=args.duration_s + args.timeout_ms / 1000.0 + 40)
        text = user_log.read_text(errors="replace")
        matches = re.findall(r"INTERMITTENT_USER_SUMMARY[^\n\r]*", text)
        if not matches:
            raise RuntimeError(f"NDNSF user summary missing; see {user_log}")
        line = matches[-1]
        vals = dict(re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", line))
        return {
            "system": "NDNSF",
            "sent": int(float(vals["sent"])),
            "success": int(float(vals["success"])),
            "timeout": int(float(vals["timeout"])),
            "success_rate": float(vals["success_rate"]),
            "actual_rps": float(vals["actual_rps"]),
        }
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=3)
        stop_processes(processes)


def run_grpc(ndn, output_dir, args):
    processes = []
    gate_stop = threading.Event()
    gate_thread = None
    try:
        server, lf, lp = start_process(
            ndn.net["ucla"], "grpc-server",
            f"cd {perf.shell_quote(REPO_ROOT)} && exec python3 {perf.shell_quote(GRPC_DIR / 'greeter_server.py')} "
            f"--bind 0.0.0.0:50051 --delay-ms {args.processing_delay_ms} --workers 32 --quiet",
            output_dir)
        processes.append((server, lf, lp))
        if not wait_for_log(lp, r"GRPC_SERVER_READY", 10, server):
            raise RuntimeError(f"grpc server not ready; see {lp}")
        gate_thread, trace_path, _ = start_coverage_gate(
            ndn, output_dir, args.ap_range, gate_stop, args.seed,
            block_network=False, pause_processes={"ucla": server})
        log(f"RandomWaypoint coverage trace -> {trace_path}")
        client, lf, client_log = start_process(
            ndn.net["memphis"], "grpc-client",
            f"cd {perf.shell_quote(REPO_ROOT)} && exec python3 {perf.shell_quote(GRPC_DIR / 'greeter_client.py')} "
            f"--target 10.0.0.2:50051 --rate-rps {args.rate_rps} "
            f"--duration-s {args.duration_s} --timeout-s {args.timeout_ms / 1000.0} "
            f"--warmup-s 0 --skip-channel-ready --quiet",
            output_dir)
        processes.append((client, lf, client_log))
        client.wait(timeout=args.duration_s + args.timeout_ms / 1000.0 + 30)
        text = client_log.read_text(errors="replace")
        line = [ln for ln in text.splitlines() if ln.startswith("GRPC_CLIENT_RATE")][-1]
        vals = dict(re.findall(r"([A-Za-z0-9_]+)=([0-9.]+)", line))
        sent = int(float(vals["sent"]))
        success = int(float(vals["success"]))
        failures = int(float(vals["failures"]))
        return {
            "system": "gRPC",
            "sent": sent,
            "success": success,
            "timeout": failures,
            "success_rate": 100.0 * success / sent if sent else 0.0,
            "actual_rps": float(vals["actual_success_rps"]),
        }
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=3)
        stop_processes(processes)


def run_nsc(ndn, output_dir, args):
    processes = []
    gate_stop = threading.Event()
    gate_thread = None
    nodes = all_app_nodes(ndn)
    try:
        configure_ndn_multicast(nodes)
        for node in nodes:
            node.cmd(f"ndnsec key-gen -t r /muas/{node.name} >/dev/null 2>&1 || true")
        producer, lf, lp = start_process(
            ndn.net["ucla"], "nsc-producer",
            f"cd {perf.shell_quote(REPO_ROOT)} && exec {perf.shell_quote(NSC_DIR / 'producer')} "
            f"/muas/ucla /FlightControl /ManualControl {args.processing_delay_ms}",
            output_dir)
        processes.append((producer, lf, lp))
        if not wait_for_log(lp, r"REGISTER PREFIX", 10, producer):
            raise RuntimeError(f"nsc producer not ready; see {lp}")
        gate_thread, trace_path, _ = start_coverage_gate(
            ndn, output_dir, args.ap_range, gate_stop, args.seed,
            block_network=False, pause_processes={"ucla": producer})
        log(f"RandomWaypoint coverage trace -> {trace_path}")
        count = int(args.duration_s * args.rate_rps)
        interval_ms = max(1, int(round(1000.0 / args.rate_rps)))
        consumer, lf, client_log = start_process(
            ndn.net["memphis"], "nsc-consumer",
            f"cd {perf.shell_quote(REPO_ROOT)} && exec {perf.shell_quote(NSC_DIR / 'consumer')} "
            f"/muas/memphis /muas/ucla /FlightControl /ManualControl "
            f"{interval_ms} {count} wifi{int(time.time())} 0",
            output_dir)
        processes.append((consumer, lf, client_log))
        consumer.wait(timeout=args.duration_s + 35)
        text = client_log.read_text(errors="replace")
        line = [ln for ln in text.splitlines() if ln.startswith("NSC_CLIENT_SUMMARY")][-1]
        vals = dict(re.findall(r"([A-Za-z0-9_]+)=([0-9.]+)", line))
        sent = int(float(vals["count"]))
        success = int(float(vals["success"]))
        timeout = int(float(vals["timeout"]))
        return {
            "system": "NSC",
            "sent": sent,
            "success": success,
            "timeout": timeout,
            "success_rate": float(vals["success_rate"]),
            "actual_rps": success / float(args.duration_s),
        }
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=3)
        stop_processes(processes)


def run_one(system, ap_range, args, output_dir):
    args.ap_range = ap_range
    ndn = build_wifi_topology(ap_range, args.seed)
    try:
        log("Starting NFD")
        AppManager(ndn, all_app_nodes(ndn), Nfd, logLevel="ERROR")
        time.sleep(2)
        if system == "ndnsf":
            return run_ndnsf(ndn, output_dir, args)
        if system == "grpc":
            return run_grpc(ndn, output_dir, args)
        if system == "nsc":
            return run_nsc(ndn, output_dir, args)
        raise RuntimeError(f"unknown system {system}")
    finally:
        ndn.net.stop()
        Minindn.cleanUp()
        subprocess.run(["mn", "-c"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
        subprocess.run(["modprobe", "-r", "mac80211_hwsim"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=False)
        time.sleep(3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ranges", default="100,150,200")
    parser.add_argument("--systems", default="ndnsf,grpc,nsc")
    parser.add_argument("--duration-s", type=int, default=60)
    parser.add_argument("--rate-rps", type=float, default=5.0)
    parser.add_argument("--processing-delay-ms", type=int, default=5)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--ack-timeout-ms", type=int, default=200)
    parser.add_argument("--settle-seconds", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20)
    parser.add_argument("--output-dir", default="")
    parser.add_argument("--single-run", action="store_true",
                        help=argparse.SUPPRESS)
    args = parser.parse_args()
    output_dir = Path(args.output_dir or f"results/wifi_router_mobility_{int(time.time())}").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    ranges = parse_csv_floats(args.ranges)
    systems = [s.strip() for s in args.systems.split(",") if s.strip()]
    if not args.single_run and (len(ranges) > 1 or len(systems) > 1):
        summaries = []
        script = Path(__file__).resolve()
        for ap_range in ranges:
            for system in systems:
                run_dir = output_dir / f"range_{int(ap_range)}" / system
                run_dir.mkdir(parents=True, exist_ok=True)
                cmd = [
                    sys.executable, str(script),
                    "--single-run",
                    "--ranges", str(ap_range),
                    "--systems", system,
                    "--duration-s", str(args.duration_s),
                    "--rate-rps", str(args.rate_rps),
                    "--processing-delay-ms", str(args.processing_delay_ms),
                    "--timeout-ms", str(args.timeout_ms),
                    "--ack-timeout-ms", str(args.ack_timeout_ms),
                    "--settle-seconds", str(args.settle_seconds),
                    "--seed", str(args.seed),
                    "--output-dir", str(run_dir),
                ]
                log(f"Driver launching {system} range={ap_range}m")
                subprocess.run(cmd, check=True)
                summaries.append(json.loads((run_dir / "summary.json").read_text())["summaries"][0])
        report = {"summaries": summaries, "output_dir": str(output_dir)}
        (output_dir / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
        with (output_dir / "summary.csv").open("w") as out:
            out.write("system,range_m,sent,success,timeout,success_rate,actual_rps\n")
            for item in summaries:
                out.write("{system},{range_m},{sent},{success},{timeout},{success_rate:.2f},{actual_rps:.2f}\n".format(**item))
        print(json.dumps(report, indent=2))
        return

    summaries = []
    for ap_range in ranges:
        for system in systems:
            run_dir = output_dir if args.single_run else output_dir / f"range_{int(ap_range)}" / system
            run_dir.mkdir(parents=True, exist_ok=True)
            log(f"Running {system} AP range={ap_range}m")
            summary = run_one(system, ap_range, args, run_dir)
            summary["range_m"] = ap_range
            summaries.append(summary)
            (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    report = {"summaries": summaries, "output_dir": str(output_dir)}
    (output_dir / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    with (output_dir / "summary.csv").open("w") as out:
        out.write("system,range_m,sent,success,timeout,success_rate,actual_rps\n")
        for item in summaries:
            out.write("{system},{range_m},{sent},{success},{timeout},{success_rate:.2f},{actual_rps:.2f}\n".format(**item))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    setLogLevel("info")
    main()
