#!/usr/bin/env python3
"""Installed native actors in real MiniNDN; deterministic local availability."""
import argparse
import fcntl
import hashlib
import json
import os
import pwd
import random
import re
import shlex
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "targeted-invocation"))
from run_minindn import (seed_keys, prepare_trust_schema, keychain_environment,
                         prepare_tls, wait_marker, stop, digest, save, records)

IDENTITIES = {"gs": ["/example/hello/controller", "/example/hello/user"],
              "drone1": ["/example/hello/provider/A"], "drone2": ["/example/hello/provider/B"]}
GROUP = "/example/hello/group/spec204"
ARMS = ["SOLICIT", "ADS100", "ADS1000", "ADS_EVENT"]


def availability_traces(profile, window_ms, seed):
    """Freeze complementary, balanced traces; users never receive future states."""
    if profile not in ("stable", "slow", "fast") or window_ms not in (3000, 60000):
        raise ValueError("unsupported availability profile/window")
    rows = [[{"offset_ms": 0, "available": True}] for _ in range(2)]
    if profile != "stable":
        count = max(1, window_ms // (10000 if profile == "slow" else 1000))
        weights = [3 + index % 5 for index in range(count)]
        random.Random(seed).shuffle(weights)
        half, total = window_ms // 2, sum(weights)
        durations = [half * weight // total for weight in weights]
        remainder_order = sorted(range(count), key=lambda i: (-(half * weights[i] % total), i))
        for index in remainder_order[:half - sum(durations)]:
            durations[index] += 1
        rows = [[], []]
        offset = 0
        for duration in durations:
            for phase in range(2):
                available_index = (seed + phase) % 2
                for index in range(2):
                    rows[index].append({"offset_ms": offset, "available": index == available_index})
                offset += duration
        if offset != window_ms or min(durations) <= 0:
            raise ValueError("availability duration partition")
    return [{"version": 1, "producer": IDENTITIES[node][0], "window_ms": window_ms,
             "tail": "hold", "changes": rows[index]}
            for index, node in enumerate(("drone1", "drone2"))]


def set_link_loss(ndn, node, blocked):
    """Use existing netem qdiscs for silent loss without closing NFD UDP faces."""
    first, second = ndn.net["relay"].connectionsTo(ndn.net[node])[0]
    for host, interface in ((ndn.net["relay"], first), (ndn.net[node], second)):
        name = shlex.quote(interface.name)
        before = host.cmd("tc qdisc show dev " + name)
        if "qdisc netem 10: parent 5:1" not in before or "delay 10.0ms" not in before:
            raise RuntimeError("unexpected netem baseline: " + node + ": " + before)
        loss = "100%" if blocked else "0%"
        command = ("tc qdisc change dev " + name +
                   " parent 5:1 handle 10: netem delay 10ms loss " + loss)
        output = host.cmd(command + " 2>&1; printf '\\nEXIT:%s\\n' \"$?\"")
        if "\nEXIT:" not in output:
            raise RuntimeError("netem result missing: " + output)
        message, code = output.rsplit("\nEXIT:", 1)
        if int(code.strip()) != 0:
            raise RuntimeError("netem change failed: " + message)
        after = host.cmd("tc qdisc show dev " + name)
        if "delay 10.0ms" not in after or (blocked and "loss 100%" not in after):
            raise RuntimeError("netem readback mismatch: " + node + ": " + after)
        if not blocked and "loss 100%" in after:
            raise RuntimeError("netem restore mismatch: " + node + ": " + after)


def start_link_trace(ndn, traces, origin_ns, log_path, on_up=None):
    """Apply shared offline trace as silent loss; caller owns restore/join."""
    events = {}
    for index, trace in enumerate(traces):
        for row in trace["changes"]:
            events.setdefault(row["offset_ms"], []).append((index, row["available"]))
    cancel = threading.Event()
    failures = []

    def apply():
        try:
            with log_path.open("w") as log:
                for offset in sorted(events):
                    remaining = (origin_ns + offset * 1000000 - time.monotonic_ns()) / 1e9
                    if cancel.wait(max(0.0, remaining)):
                        return
                    for index, available in events[offset]:
                        node = ("drone1", "drone2")[index]
                        set_link_loss(ndn, node, not available)
                        first, second = ndn.net["relay"].connectionsTo(ndn.net[node])[0]
                        observed = first.isUp() and second.isUp() and available
                        if available and observed and on_up is not None:
                            on_up(node)
                        row = {"node": node, "mechanism": "bidirectional-netem-loss",
                               "offset_ms": offset,
                               "expected_up": available, "observed_up": observed,
                               "applied_ns": time.monotonic_ns()}
                        log.write(json.dumps(row, sort_keys=True) + "\n")
                        log.flush()
                        if observed != available:
                            raise RuntimeError("offline link state readback mismatch: " + node)
        except Exception as error:
            failures.append(str(error))

    thread = threading.Thread(target=apply, name="spec206-link-trace")
    thread.start()
    return thread, cancel, failures


def stop_link_trace(ndn, active):
    if active is None:
        return
    thread, cancel, failures = active
    cancel.set()
    thread.join(timeout=5)
    errors = list(failures)
    if thread.is_alive():
        errors.append("offline link worker did not stop")
    for node in ("drone1", "drone2"):
        try:
            set_link_loss(ndn, node, False)
            first, second = ndn.net["relay"].connectionsTo(ndn.net[node])[0]
            if not (first.isUp() and second.isUp()):
                errors.append("offline link restore failed: " + node)
        except Exception as error:
            errors.append("offline link restore error: " + node + ": " + str(error))
    if errors:
        raise RuntimeError(errors[0])


def run(args, parser):
    from mininet.topo import Topo
    from minindn.minindn import Minindn
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.util import getPopen

    root = Path(args.workspace).resolve()
    root.mkdir(parents=True, exist_ok=True)
    lock = (root / ".active.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=False)
    task_classes = ("compute", "compute", "memory", "memory")
    task_plan = {
        "version": 1, "window_ms": args.window_ms, "period_ms": args.period_ms,
        "tasks": [{"task": index + 1, "arrival_ms": arrival,
                   "class": (task_classes[(index + args.seed) % 4]
                             if args.comparison_condition == "task-busy" else
                             "memory" if (index + args.seed) % 2 else "compute")}
                  for index, arrival in enumerate(range(0, args.window_ms, args.period_ms))]}
    task_plan_path = output / "task-plan.json"
    save(task_plan_path, task_plan)
    comparison = args.comparison_condition is not None
    coarse_comparison = args.spec207_coarse
    cross_baselines = comparison and not coarse_comparison and not args.participation_study
    calibrated_handler = (coarse_comparison and args.comparison_condition in ("task-busy", "offline")
                          and args.handler_ms == 80)
    if comparison:
        if (args.availability_profile not in ("legacy", "slow", "fast") or args.dwell_ms != 0 or
                (args.handler_ms != 5 and not calibrated_handler)
                or args.force_unavailable or
                args.recovery_condition not in ("none", "prepared")):
            raise ValueError("unsupported comparison handler profile")
        args.recovery_condition = "prepared"
    trace_references = []
    if comparison or args.availability_profile != "legacy":
        if (args.recovery_condition != "prepared" or args.dwell_ms != 0
                or args.force_unavailable or (args.handler_ms != 5 and not calibrated_handler)):
            raise ValueError("unsupported trace or handler profile")
        profile = (("stable" if args.comparison_condition == "stable" else
                    "slow" if args.availability_profile == "legacy" else
                    args.availability_profile) if comparison else args.availability_profile)
        for index, trace in enumerate(availability_traces(profile,
                                                        args.window_ms, args.seed)):
            path = output / f"availability-{index}.json"
            save(path, trace)
            trace_references.append({"producer": trace["producer"], "file": path.name,
                                     "sha256": digest(path)})
    origin_path = root / "origin-ns"
    origin_path.unlink(missing_ok=True)
    policy, trust = output / "hello.policies", output / "trust-schema.conf"
    shutil.copy2(REPO / "examples/hello.policies", policy)
    seed = seed_keys(root, fixed_root=True)
    prepare_trust_schema(seed, trust, svs_group="spec204")
    tls = prepare_tls(root) if cross_baselines else None
    topo = Topo()
    for name in ("gs", "relay", "drone1", "drone2"):
        topo.addHost(name)
    for left, right in [("gs", "relay"), ("relay", "drone1"), ("relay", "drone2")]:
        topo.addLink(left, right, delay="10ms", loss=0, bw=1000)
    ndn = Minindn(parser=parser, topo=topo, workDir=str(root / "nodes"))
    if ndn.args.resultDir:
        raise ValueError("--result-dir would move reusable state")
    now_before, wall, now_after = time.monotonic_ns(), time.time_ns(), time.monotonic_ns()
    manifest = {"schema": "ndnsf-dynamic205-v2", "status": "PARTIAL", "seed": args.seed,
                "task_plan": {"file": task_plan_path.name, "sha256": digest(task_plan_path)},
                "arrival_policy": "fixed arrivals; inflight=1; queue=0; rejected remains in cohort",
                "record_oracle": {"qualified": False},
                "dwell_ms": args.dwell_ms, "window_ms": args.window_ms, "settling_idle_ms": 10000,
                "warmup": "one successful targeted call to each Provider, then 10s idle settling",
                "delay_ms_per_link": 10, "loss_percent": 0, "logical_deadline_ms": 2000,
                "max_attempts": 3, "attempt_ms": 600, "ack_ms": 250, "refresh_ms": 120,
                "post_selection_wait": True,
                "advertisement_ttl_ms": 120000 if coarse_comparison else 2500,
                "status_ttl_ms": 2500,
                "publication_policy": ("initial and persistent service change only; transient busy omitted"
                                       if coarse_comparison else "periodic by arm"),
                "runs": [], "children": [],
                "period_ms": args.period_ms,
                "handler_ms":args.handler_ms,"force_unavailable":args.force_unavailable,
                "purpose":"negative" if args.force_unavailable or args.handler_ms==800 else "performance" if args.window_ms==60000 else "wiring",
                "clock_anchor": {"wall_ns": wall, "mono_ns": (now_before + now_after) // 2,
                                 "uncertainty_ns": (now_after - now_before) // 2},
                "scope": "protected native invocation; simulated availability, not real flight",
                "advertisement_transport": "public SVSPubSub, Producer-cert signed; experiment baseline, not unmodified NDNSD",
                "trust_profile": "fixed root; root-issued actor certificates; per-node private keys",
                "git_head": subprocess.check_output(["runuser", "-u",pwd.getpwuid(REPO.stat().st_uid).pw_name,
                    "--","git","-C",str(REPO),"rev-parse","HEAD"],text=True).strip()}
    if args.recovery_condition != "none":
        manifest["schema"] = "ndnsf-dynamic205-e2-v1"
        manifest["recovery_condition"] = args.recovery_condition
    if trace_references:
        manifest["schema"] = ("ndnsf-feedback-ablation-v1" if args.participation_study else
                              "ndnsf-spec207-coarse-v1" if coarse_comparison else
                              "ndnsf-spec206-comparison-v1" if comparison else
                              "ndnsf-dynamic205-e3-v1")
        manifest["availability_trace"] = {"profile": profile,
                                          "files": trace_references}
    if comparison:
        manifest["condition"] = args.comparison_condition
        manifest["baseline_attempt_ms"] = args.comparison_attempt_ms
        manifest["capture_policy"] = "relay any interface; count each observed link transmission"
    sources = list(HERE.glob("*.cpp")) + list(HERE.glob("*.hpp")) + list(HERE.glob("*.py")) + [HERE / "wscript"]
    sources.extend([HERE.parent/"targeted-invocation/common.hpp",HERE.parent/"targeted-invocation/run_minindn.py"])
    manifest["source_sha256"] = {p.name: digest(p) for p in sources}
    if cross_baselines:
        baseline_sources = [HERE.parent / "NDN_NSC" / (role + ".cpp")
                            for role in ("consumer", "producer")]
        baseline_sources += [HERE.parent / "gRPC" / name for name in
                             ("greeter_failover_client.py", "greeter_server.py",
                              "helloworld.proto", "helloworld_pb2.py", "helloworld_pb2_grpc.py")]
        baseline_sources.append(HERE.parent / "gRPC_memphis_ucla_latency.py")
        manifest["baseline_sources"] = {str(p.relative_to(REPO)): digest(p)
                                        for p in baseline_sources}
        manifest["tls_public_sha256"] = {p.name: digest(p) for p in tls.glob("*.crt")}
    binaries = ["/usr/local/bin/ndnsf-dynamic-provider", "/usr/local/bin/ndnsf-dynamic-user",
                "/usr/local/bin/ndnsf-dynamic-oracle",
                "/usr/local/bin/App_ServiceController"]
    if cross_baselines:
        binaries += ["/usr/local/bin/spec203-nsc-consumer", "/usr/local/bin/spec203-nsc-producer"]
    manifest["binaries"] = {p: digest(p) for p in binaries}
    manifest["libraries"] = {}
    for binary in binaries:
        closure = subprocess.check_output(["ldd", binary], text=True,
                                          env=dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib"))
        if "not found" in closure or str(REPO) in closure:
            raise RuntimeError("invalid installed closure")
        for word in closure.split():
            if word.startswith("/") and Path(word).is_file():
                manifest["libraries"][str(Path(word).resolve())] = digest(word)
    manifest["configuration"] = {p.name: digest(p) for p in [policy, trust]}
    manifest["contract_sha256"] = digest(REPO / "specs/205-compact-protected-requests/contracts.md")
    manifest["inherited_environment"] = {k: v for k, v in os.environ.items() if k.startswith(("NDNSF_", "NDN_"))}
    children = []
    nfd_manager, controller, writer = None, None, None
    generation = root / "controller-generation.state"
    generation_lock = Path(str(generation) + ".lock")

    def launch(node, label, command, extra_env=None):
        log = output / (label + ".log")
        handle = log.open("wb")
        env = {"NDN_LOG": "ndn_service_framework.*=ERROR", "LD_LIBRARY_PATH": "/usr/local/lib",
               "NDNSF_CONFIG": str(Path(node.params["params"]["homeDir"]) / "ndnsf.conf"),
               "NDNSF_ENABLE_NDNSD": "0"}
        if args.recovery_condition != "none":
            # Preserve existing production rejection/mode evidence for both E2 conditions.
            env["NDN_LOG"] = "ndn_service_framework.*=WARN"
        if label == "controller":
            env["NDN_LOG"] = "ndn_service_framework.*=INFO"
            env["NDNSF_CONTROLLER_GENERATION_STATE"] = str(generation)
        if extra_env:
            env.update(extra_env)
        if node.name in ("gs", "drone1", "drone2"):
            owned = keychain_environment(seed / node.name)
            env.update({key: owned[key] for key in ("NDN_CLIENT_PIB", "NDN_CLIENT_TPM")})
        manifest.setdefault("actor_environment", {})[label] = env
        proc = getPopen(node, command, envDict=env, stdout=handle, stderr=subprocess.STDOUT)
        children.append((label, proc, handle))
        return proc, log

    def run_baseline(mode, label, trace_references, task_plan_path, grpc_targets):
        if mode == "GRPC":
            route_readback = []
            for node in ("drone1", "drone2"):
                path = ["gs", "relay", node]
                address = grpc_targets[node].split(":")[0]
                client_address = endpoint_ip_on_path(ndn, list(reversed(path)))
                add_route_to_destination(ndn, path, address)
                add_route_to_destination(ndn, list(reversed(path)), client_address)
                probe = ndn.net["gs"].cmd("ping -n -c 1 -W 1 " + address)
                route_readback.append(node + " " + probe)
                if "1 received" not in probe:
                    raise RuntimeError("gRPC path unavailable before measurement: " + node)
            (output / (label + "-ip-route-readback.log")).write_text(
                "\n".join(route_readback) + "\n")
        origin = ((time.monotonic_ns() + 10000000000 + 999999) // 1000000) * 1000000
        origin_path.write_text(str(origin) + "\n")
        providers = []
        active_links = None
        trace_files = [output / reference["file"] for reference in trace_references]
        try:
            for index, node in enumerate(("drone1", "drone2")):
                provider_label = label + "-" + node
                if mode == "NSC":
                    command = ["/usr/local/bin/spec203-nsc-producer", "/muas/" + node,
                               "/FlightControl", "/ManualControl", "5"]
                    extra = {"SPEC203_RECORDS": str(output / (provider_label + ".jsonl"))}
                    if args.comparison_condition != "offline":
                        extra.update({"SPEC206_BUSY_TRACE": str(trace_files[index]),
                                      "SPEC206_ORIGIN": str(origin_path)})
                    if args.comparison_condition == "task-busy":
                        extra["SPEC206_TASK_BUSY"] = "1"
                    marker = "SPEC203_NSC_PROVIDER_READY"
                else:
                    command = [sys.executable, str(HERE.parent / "gRPC" / "greeter_server.py"),
                               "--bind", grpc_targets[node], "--provider-id", node,
                               "--delay-ms", "5", "--quiet",
                               "--tls-ca", str(tls / "ca.crt"),
                               "--tls-cert", str(tls / (node + ".crt")),
                               "--tls-key", str(tls / (node + ".key")),
                               "--records", str(output / (provider_label + ".jsonl"))]
                    if args.comparison_condition != "offline":
                        command += ["--busy-trace", str(trace_files[index]),
                                    "--origin", str(origin_path)]
                    if args.comparison_condition == "task-busy":
                        command += ["--task-busy"]
                    extra, marker = {}, "GRPC_SERVER_READY"
                provider, provider_log = launch(ndn.net[node], provider_label,
                                                command, extra)
                providers.append(provider)
                wait_marker(provider, provider_log, marker)
            if args.comparison_condition == "offline":
                traces = [json.loads(path.read_text()) for path in trace_files]
                def restore_grpc_route(node):
                    path = ["gs", "relay", node]
                    address = grpc_targets[node].split(":")[0]
                    client_address = endpoint_ip_on_path(ndn, list(reversed(path)))
                    add_route_to_destination(ndn, path, address)
                    add_route_to_destination(ndn, list(reversed(path)), client_address)
                active_links = start_link_trace(
                    ndn, traces, origin, output / (label + "-link-state.jsonl"),
                    restore_grpc_route if mode == "GRPC" else None)
            if mode == "NSC":
                count = len(task_plan["tasks"])
                command = ["/usr/local/bin/spec203-nsc-consumer", "/muas/gs1",
                           "/muas/drone1,/muas/drone2", "/FlightControl", "/ManualControl",
                           str(args.period_ms), str(count), label, "0", "2000",
                           str(args.comparison_attempt_ms),
                           str(origin // 1000000), "50"]
                extra = {"SPEC203_RECORDS": str(output / (label + "-user.jsonl")),
                         "SPEC203_WINDOW_MS": str(args.window_ms),
                         "SPEC206_FIXED_ARRIVALS": "1",
                         "SPEC206_TASK_PLAN": str(task_plan_path)}
                marker = "SPEC203_NSC_READY"
            else:
                command = [sys.executable, str(HERE.parent / "gRPC" /
                                              "greeter_failover_client.py")]
                for node in ("drone1", "drone2"):
                    command += ["--target", node + "=" + grpc_targets[node]]
                command += ["--count", str(len(task_plan["tasks"])),
                            "--interval-ms", str(args.period_ms),
                            "--global-deadline-s", "2", "--attempt-timeout-s",
                            str(args.comparison_attempt_ms / 1000),
                            "--measurement-start-monotonic-s", f"{origin / 1e9:.9f}",
                            "--measurement-start-tolerance-s", "0.05",
                            "--planned-deadline", "--task-plan", str(task_plan_path),
                            "--require-all-prewarmed", "--quiet",
                            "--tls-ca", str(tls / "ca.crt"),
                            "--tls-cert", str(tls / "gs.crt"),
                            "--tls-key", str(tls / "gs.key")]
                extra, marker = {}, "GRPC_MEASUREMENT_START"
            user, user_log = launch(ndn.net["gs"], label + "-user", command, extra)
            wait_marker(user, user_log, marker, seconds=20)
            user.wait(timeout=30 + args.window_ms / 1000)
            if user.returncode:
                raise RuntimeError(mode + " user failed: " + label)
            if active_links and active_links[2]:
                raise RuntimeError("offline link state failed: " + active_links[2][0])
            return {"id": label, "arm": mode, "origin_ns": origin,
                    "user_exit": user.returncode,
                    "user_log": user_log.name,
                    "provider_records": [label + "-" + node + ".jsonl"
                                         for node in ("drone1", "drone2")],
                    "security": "signed NDN" if mode == "NSC" else "mutual TLS"}
        finally:
            cleanup_errors = []
            try:
                stop_link_trace(ndn, active_links)
            except Exception as error:
                cleanup_errors.append(str(error))
            for provider in reversed(providers):
                try:
                    if stop(provider):
                        cleanup_errors.append(mode + " provider shutdown failed")
                except Exception as error:
                    cleanup_errors.append(str(error))
            if cleanup_errors:
                manifest.setdefault("cleanup_errors", []).extend(cleanup_errors)
                if sys.exc_info()[0] is None:
                    raise RuntimeError(cleanup_errors[0])

    try:
        ndn.start()
        nfd_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        for owner, prefixes in IDENTITIES.items():
            routing.addOrigin([ndn.net[owner]], prefixes + [GROUP])
        if cross_baselines:
            routing.addOrigin([ndn.net["gs"]], ["/muas/gs1"])
            routing.addOrigin([ndn.net["drone1"]], ["/muas/drone1"])
            routing.addOrigin([ndn.net["drone2"]], ["/muas/drone2"])
        routing.calculateRoutes()
        if cross_baselines:
            sys.path.insert(0, str(HERE.parent))
            from gRPC_memphis_ucla_latency import endpoint_ip_on_path, add_route_to_destination
            grpc_targets = {}
            for node in ("drone1", "drone2"):
                path = ["gs", "relay", node]
                address = endpoint_ip_on_path(ndn, path)
                client_address = endpoint_ip_on_path(ndn, list(reversed(path)))
                add_route_to_destination(ndn, path, address)
                add_route_to_destination(ndn, list(reversed(path)), client_address)
                grpc_targets[node] = address + ":50051"
            manifest["grpc_targets"] = grpc_targets
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_BEST_ROUTE)
            Nfdc.setStrategy(node, GROUP, Nfdc.STRATEGY_MULTICAST)
            if cross_baselines:
                Nfdc.setStrategy(node, "/muas", Nfdc.STRATEGY_BEST_ROUTE)
            listing = node.cmd("nfdc strategy list")
            (output / (node.name + "-routing.txt")).write_text(listing + node.cmd("nfdc fib list")
                + node.cmd("nfdc route list") + node.cmd("tc qdisc show"))
            for prefix, strategy in [("/example/hello", "best-route"), (GROUP, "multicast")]:
                lines = [line for line in listing.splitlines() if "prefix=" + prefix + " " in line]
                if len(lines) != 1 or "/strategy/" + strategy + "/" not in lines[0]:
                    raise RuntimeError("strategy readback: " + node.name)
        controller, log = launch(ndn.net["gs"], "controller", ["/usr/local/bin/App_ServiceController",
            "--policy-file", str(policy), "--trust-schema", str(trust)])
        wait_marker(controller, log, "ServiceController listening on:")
        if "NDNSF_CONTROLLER_VERSION generation=" not in log.read_text(errors="replace"):
            raise RuntimeError("controller generation ownership unavailable; preserve existing writer lock")
        writer = generation_lock.read_bytes()
        if args.participation_study:
            base_arms = ["FEEDBACK", "ACK_ALL"]
            offset = args.seed % len(base_arms)
            arms = base_arms[offset:] + base_arms[:offset]
        elif coarse_comparison:
            base_arms = ["SOLICIT", "ADS_COARSE"]
            offset = args.seed % len(base_arms)
            arms = base_arms[offset:] + base_arms[:offset]
        elif comparison:
            base_arms = ["SOLICIT", "NSC", "GRPC"]
            offset = args.seed % len(base_arms)
            arms = base_arms[offset:] + base_arms[:offset]
        else:
            offset = (args.seed - 20401) % 4
            arms = ARMS[offset:] + ARMS[:offset]
        if args.arm:
            arms = [args.arm]
        manifest["arm_order"] = arms
        for index, mode in enumerate(arms):
            if comparison:
                origin_path.write_text("0\n")
            else:
                origin_path.unlink(missing_ok=True)
            label = f"b{args.seed}-d{args.dwell_ms}-p{args.period_ms}-{mode}"
            native_mode = "SOLICIT" if mode in ("FEEDBACK", "ACK_ALL") else mode
            # Unique actor instance across trace conditions and retries.
            session = str(time.time_ns())
            group = GROUP + "/" + hashlib.sha256((label+session).encode()).hexdigest()[:16]
            captures, providers = [], []
            provider_logs = []
            capture_peers = ("relay-any",) if comparison else ("gs", "drone1", "drone2")
            for peer in capture_peers:
                interface = ("any" if comparison else
                             ndn.net["relay"].connectionsTo(ndn.net[peer])[0][0].name)
                capture, log = launch(ndn.net["relay"], label + "-capture-" + peer,
                    ["tcpdump", "-n", "-U", "-s", "0", "-i", interface, "-w",
                     str(output / (label + "-" + peer + ".pcap")), "ip"])
                wait_marker(capture, log, "listening on", seconds=5)
                captures.append(capture)
                manifest.setdefault("capture", {})[label + "-" + peer] = {
                    "interface": interface, "endpoint": "relay", "filter": "ip", "snaplen": 0,
                    "directions": "both", "pcap": label + "-" + peer + ".pcap",
                    "started_ns": time.monotonic_ns(), "guard_ns": 250000000}
            time.sleep(.25)
            try:
                if comparison and mode in ("NSC", "GRPC"):
                    baseline_run = run_baseline(mode, label, trace_references,
                                                task_plan_path, grpc_targets)
                    manifest["runs"].append(baseline_run)
                    save(output / "manifest.json", manifest)
                    continue
                for provider_index, node_name in enumerate(("drone1", "drone2")):
                    proc, log = launch(ndn.net[node_name], label + "-" + node_name,
                        ["/usr/local/bin/ndnsf-dynamic-provider", "--identity", IDENTITIES[node_name][0],
                         "--group", group, "--trust", str(trust), "--mode", native_mode, "--session", session,
                         "--origin", str(origin_path), "--index", str(provider_index), "--dwell-ms", str(args.dwell_ms),
                         "--recovery-condition", args.recovery_condition,
                         "--handler-ms",str(args.handler_ms),"--force-unavailable","1" if args.force_unavailable else "0",
                         "--records", str(output / (label + "-" + node_name + ".jsonl"))]
                        + (["--task-busy", "1"] if args.comparison_condition == "task-busy" else [])
                        + (["--ack-all", "1"] if mode == "ACK_ALL" else [])
                        + (["--availability-trace", str(output / trace_references[provider_index]["file"]),
                            "--window-ms", str(args.window_ms)]
                           if trace_references and args.comparison_condition != "offline" else []))
                    providers.append(proc)
                    provider_logs.append(log)
                    wait_marker(proc, log, "SPEC204_PROVIDER_READY")
                user, log = launch(ndn.net["gs"], label + "-user", ["/usr/local/bin/ndnsf-dynamic-user",
                    "--group", group, "--trust", str(trust), "--mode", native_mode, "--session", session,
                    "--origin", str(origin_path), "--window-ms", str(args.window_ms),
                    "--period-ms",str(args.period_ms),
                    "--task-plan",str(task_plan_path),
                    "--records", str(output / (label + "-user.jsonl"))]
                    + (["--participation-study", "1"] if args.participation_study else []))
                wait_marker(user, log, "SPEC204_USER_WARM_READY")
                if args.recovery_condition != "none":
                    for provider in providers:
                        provider.send_signal(signal.SIGUSR1)
                    for provider, provider_log in zip(providers, provider_logs):
                        wait_marker(provider, provider_log, "SPEC205_RECOVERY_PREPARED", seconds=5)
                origin = time.monotonic_ns() + 10000000000
                if comparison:
                    origin = ((origin + 999999) // 1000000) * 1000000
                active_links = None
                try:
                    if args.comparison_condition == "offline":
                        traces = [json.loads((output / reference["file"]).read_text())
                                  for reference in trace_references]
                        active_links = start_link_trace(
                            ndn, traces, origin, output / (label + "-link-state.jsonl"))
                    origin_path.write_text(str(origin) + "\n")
                    user.wait(timeout=30 + args.window_ms / 1000)
                    if user.returncode:
                        raise RuntimeError("native user failed: " + label)
                    if active_links and active_links[2]:
                        raise RuntimeError("offline link state failed: " + active_links[2][0])
                finally:
                    stop_link_trace(ndn, active_links)
                for provider in reversed(providers):
                    if stop(provider):
                        raise RuntimeError("provider shutdown failed")
            finally:
                time.sleep(.25)
                for peer, capture in zip(capture_peers, captures):
                    manifest["capture"][label + "-" + peer]["stopped_ns"] = time.monotonic_ns()
                    exit_code=stop(capture)
                    capture_log=(output / (label + "-capture-" + peer + ".log")).read_text(errors="replace")
                    drops=re.findall(r"(\d+) packets dropped by kernel",capture_log)
                    manifest["capture"][label + "-" + peer].update(
                        {"exit":exit_code,"drops":int(drops[0]) if len(drops)==1 else None})
                    if exit_code or len(drops)!=1 or int(drops[0]):
                        raise RuntimeError("capture exit/drop qualification failed")
            manifest["runs"].append({"id": label, "arm": mode, "session": session, "group": group,
                                     "origin_ns": origin, "user_exit": user.returncode})
            save(output / "manifest.json", manifest)
            if comparison and not args.participation_study:
                continue  # Existing cross-system comparisons use their independent oracle.
            oracle = "/usr/local/bin/ndnsf-dynamic-oracle"
            inputs = ["task-plan.json"] + [label + suffix for suffix in
                      ("-user.jsonl", "-drone1.jsonl", "-drone2.jsonl")]
            inputs += [reference["file"] for reference in trace_references]
            receipt = {"binary_sha256": digest(oracle),
                       "inputs": {name: digest(output / name) for name in inputs},
                       "context": {"run": dict(manifest["runs"][-1]),
                                   "manifest": {key: manifest[key] for key in
                                                ("schema", "window_ms", "period_ms", "dwell_ms",
                                                 "force_unavailable", "task_plan")}}}
            if args.recovery_condition != "none":
                receipt["context"]["manifest"]["recovery_condition"] = args.recovery_condition
                receipt["context"]["manifest"]["clock_anchor"] = manifest["clock_anchor"]
            if trace_references:
                receipt["context"]["manifest"]["availability_trace"] = manifest["availability_trace"]
            if receipt["binary_sha256"] != manifest["binaries"][oracle]:
                raise RuntimeError("installed archive oracle changed during run")
            result = subprocess.run([oracle, "--manifest", str(output / "manifest.json"),
                                     "--run-id", label], capture_output=True, text=True,
                                    timeout=60, env=dict(os.environ, LD_LIBRARY_PATH="/usr/local/lib"))
            oracle_log = output / (label + "-record-oracle.log")
            oracle_log.write_text(result.stdout + result.stderr)
            receipt.update({"exit": result.returncode, "output": oracle_log.name,
                            "output_sha256": digest(oracle_log)})
            manifest["runs"][-1]["record_oracle"] = receipt
            save(output / "manifest.json", manifest)
            if result.returncode:
                raise RuntimeError("native archive oracle rejected " + label)
            checked = json.loads(result.stdout)
            if (checked.get("event") != "dynamic_record_oracle_complete"
                    or checked.get("qualified") is not True or checked.get("run_id") != label
                    or checked.get("tasks") != len(task_plan["tasks"])):
                raise RuntimeError("invalid native archive oracle receipt")
            receipt["result"] = checked
            if receipt["inputs"] != {name: digest(output / name) for name in inputs}:
                raise RuntimeError("archive inputs changed during verification")
            save(output / "manifest.json", manifest)
        if comparison and not args.participation_study:
            manifest["status"] = "ACTORS_COMPLETE_UNQUALIFIED"
        else:
            manifest["record_oracle"]["qualified"] = all(
                row.get("record_oracle", {}).get("result", {}).get("qualified") is True
                for row in manifest["runs"]) and len(manifest["runs"]) == len(arms)
            if not manifest["record_oracle"]["qualified"]:
                raise RuntimeError("incomplete native archive qualification")
            manifest["status"] = "RUN_COMPLETE"
    except Exception as error:
        manifest["first_failure"] = str(error)
        raise
    finally:
        for label, proc, handle in reversed(children):
            try:
                manifest["children"].append({"label": label, "exit": stop(proc)})
            except Exception as error:
                manifest.setdefault("cleanup_errors", []).append(str(error))
                manifest["status"] = "PARTIAL"
            handle.close()
        try:
            if nfd_manager:
                for app in nfd_manager:
                    if app.process:
                        try:
                            code=stop(app.process)
                            manifest["children"].append({"label":app.node.name+"-nfd","exit":code})
                        except Exception as error:
                            manifest.setdefault("cleanup_errors", []).append(app.node.name+": "+str(error))
                            manifest["status"]="PARTIAL"
            if controller and controller.poll() is not None and writer is not None and generation_lock.exists():
                if generation_lock.read_bytes() != writer:
                    raise RuntimeError("controller writer changed")
                (output / "controller-writer-release.lock").write_bytes(writer)
                generation_lock.unlink()
            if generation.exists():
                shutil.copy2(generation, output / generation.name)
        except Exception as error:
            manifest.setdefault("cleanup_errors", []).append(str(error))
            manifest["status"] = "PARTIAL"
        finally:
            try:
                ndn.stop()
            except Exception as error:
                manifest.setdefault("cleanup_errors", []).append("network stop: "+str(error))
                manifest["status"]="PARTIAL"
            finally:
                save(output / "manifest.json", manifest)
                fcntl.flock(lock, fcntl.LOCK_UN)
                lock.close()
    print("DYNAMIC_CELL_COMPLETE", output, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", default="/var/tmp/ndnsf-spec204-dynamic")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--seed", type=int, default=20401)
    parser.add_argument("--dwell-ms", type=int, choices=(0, 500, 5000), default=0)
    parser.add_argument("--availability-profile", choices=("legacy", "stable", "slow", "fast"),
                        default="legacy")
    parser.add_argument("--window-ms", type=int, choices=(3000, 60000), default=60000)
    parser.add_argument("--period-ms",type=int,choices=(100,1000,4700),default=4700)
    parser.add_argument("--arm", choices=ARMS + ["ADS_COARSE", "NSC", "GRPC", "FEEDBACK", "ACK_ALL"])
    parser.add_argument("--participation-study", action="store_true",
                        help="paired 2-second task-feedback/ACK-all ablation")
    parser.add_argument("--spec207-coarse", action="store_true",
                        help="paired SOLICIT/ADS_COARSE comparison")
    parser.add_argument("--comparison-condition", choices=("offline", "busy", "stable", "task-busy"),
                        help="Spec206 matched basic/NSC/gRPC comparison")
    parser.add_argument("--comparison-attempt-ms", type=int, choices=(250, 600), default=600,
                        help="shared NSC/gRPC attempt timeout; NDNSF retains its native budget")
    parser.add_argument("--recovery-condition", choices=("none", "prepared", "reregister"),
                        default="none")
    parser.add_argument("--handler-ms",type=int,choices=(5,80,800),default=5)
    parser.add_argument("--force-unavailable",action="store_true")
    args, _ = parser.parse_known_args()
    if args.arm in ("NSC", "GRPC") and args.comparison_condition is None:
        parser.error("NSC/GRPC arms require --comparison-condition")
    if args.spec207_coarse and args.comparison_condition not in ("task-busy", "offline"):
        parser.error("coarse comparison requires task-busy or offline condition")
    if args.participation_study and (args.spec207_coarse or args.comparison_condition != "task-busy"
                                     or args.handler_ms != 5 or args.window_ms != 60000
                                     or args.period_ms != 4700):
        parser.error("participation study requires task-busy, 60 s, 4700 ms arrivals, 5 ms handler")
    allowed_comparison_arms = ((None, "FEEDBACK", "ACK_ALL") if args.participation_study else
                               (None, "SOLICIT", "ADS_COARSE") if args.spec207_coarse else
                               (None, "SOLICIT", "NSC", "GRPC"))
    if args.comparison_condition and args.arm not in allowed_comparison_arms:
        parser.error("Spec206 comparison uses SOLICIT, NSC or GRPC")
    if args.arm == "ADS_COARSE" and not args.spec207_coarse:
        parser.error("ADS_COARSE requires --spec207-coarse")
    if not 0 <= args.seed <= 2147483647:
        parser.error("seed must be in 0..2147483647")
    if args.handler_ms == 80 and not (args.spec207_coarse
                                      and args.comparison_condition in ("task-busy", "offline")):
        parser.error("80 ms handler is limited to the paired coarse-advertisement study")
    if args.period_ms == 1000 and not (args.spec207_coarse
                                       and args.comparison_condition in ("task-busy", "offline")
                                       and args.handler_ms == 80):
        parser.error("1 Hz arrivals require the calibrated paired study")
    if(args.force_unavailable or args.handler_ms==800) and args.window_ms!=3000:
        raise SystemExit("negative profiles require3s window; never enter formal matrix")
    if os.geteuid() != 0:
        raise SystemExit("MiniNDN requires root: sudo -n python3")
    run(args, parser)
