#!/usr/bin/env python3
"""Measure bare NFD with an authenticated Repo reference's exact original wire."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import subprocess
import tempfile
import time
import uuid

from ndnsf_di_launcher import _bind_package_root

_bind_package_root()
from ndnsf_distributed_inference.ops import cluster
from ndnsf_distributed_inference.ops.deployment_config import load_deployment
from ndnsf_distributed_inference.ops.minindn import MiniNdnFacility
from ndnsf_distributed_inference.ops.processes import process_identity

FIELDS = ("mode,payload,window,sample,interests,received,timeouts,nacks,bad,payload_bytes,"
          "interest_bytes,data_wire_bytes,wall_s,consumer_cpu_s,nfd_cpu_s,producer_cpu_s,"
          "producer_total_packets,latency_samples,rtt_p50_us,rtt_p95_us").split(",")


def digest(path):
    with path.open("rb") as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            result.update(chunk)
    return result.hexdigest()


def run(args):
    if os.getuid() != 0:
        raise RuntimeError("MiniNDN requires root")
    config = load_deployment(args.config)
    if config["platform"]["kind"] != "minindn":
        raise RuntimeError("bare NFD replay requires the MiniNDN deployment topology")
    actors = {actor["kind"]: actor for actor in config["actors"]
              if actor["kind"] in ("requester", "repo") and not actor["external"]}
    sender_actor, receiver_actor = actors["requester"], actors["repo"]
    if args.direction == "read":
        sender_actor, receiver_actor = receiver_actor, sender_actor
    sender, receiver = sender_actor["node"], receiver_actor["node"]
    nodes = {node["id"]: node for node in config["nodes"]}
    if sender == receiver or args.router not in nodes or args.router in (sender, receiver):
        raise RuntimeError("replay needs two distinct endpoints and a router")
    placement = {}
    if args.cpu_placement is not None:
        placement = cluster._benchmark_cpu_placement(config, json.loads(args.cpu_placement.read_bytes()))
    producer_cpu = placement.get(sender_actor["id"], [-1])[0]
    consumer_cpu = placement.get(receiver_actor["id"], [-1])[0]
    reference = json.loads(args.reference.read_bytes())
    if (reference["schema"] != "ndnsf-repo-file-benchmark-reference-v1" or
            reference["authentication"] != "manifest"):
        raise RuntimeError("replay requires a manifest benchmark write reference")
    group = reference["groups"][args.group]
    payload = reference["contentSegmentBytes"]
    wire_digest = group["wireSha256"]
    if (payload not in (7000, 8400) or group["packetCount"] * payload != group["payloadBytes"]
            or not re.fullmatch("[0-9a-f]{64}", wire_digest)
            or not 0 < group["wireBytes"] <= 64 * (1 << 20)):
        raise RuntimeError("replay requires a bounded group of complete source segments")
    source = args.store / "payloads" / "sha256" / wire_digest[:2] / wire_digest
    if source.is_symlink() or not source.is_file() or source.stat().st_size != group["wireBytes"]:
        raise RuntimeError("the reference's original-wire file is unavailable")
    prefix = Path(config["release"]["prefix"])
    binary = prefix / "bin/ndnsf-nfd-wire-benchmark"
    manifest = json.loads(Path(config["release"]["manifest"]).read_bytes())
    binary_digest = digest(binary)
    if (not os.access(binary, os.X_OK) or binary.is_symlink() or
            manifest["files"][str(binary)]["installed"]["sha256"] != binary_digest):
        raise RuntimeError("replay executable differs from the installation record")

    output = args.output.resolve()
    output.mkdir(mode=0o700)
    root = Path(tempfile.mkdtemp(prefix="ndnsf-wire-", dir="/var/tmp"))
    wire = root / "group.wire"
    try:
        shutil.copyfile(source, wire)
        if digest(wire) != wire_digest:
            raise RuntimeError("original wire differs from the durable reference")
    except BaseException:
        shutil.rmtree(root)
        raise
    for node in nodes.values():
        node["nfd"]["socket"] = str(root / (node["id"] + ".sock"))
    config["network"]["routes"].append({"to": sender, "prefix": group["prefix"]})
    config_digest = hashlib.sha256(json.dumps(config, sort_keys=True).encode()).hexdigest()
    state = dict(schema="ndnsf-di-host-operation-v1", deployment_id="nfd-wire-replay",
                 boot_id=uuid.uuid4().hex, configuration_sha256=config_digest,
                 manager=process_identity(os.getpid()), operation="prepare", status="STARTING",
                 launches=[], source=str(args.config), evidence_root=str(output))
    children = []
    measurements = []
    deadline = time.monotonic() + 90 + len(args.windows) * args.samples * (args.seconds + 25)
    facility = MiniNdnFacility(config, root, state, deadline)

    def guard():
        cluster._check_running(children)
        if time.monotonic() >= deadline:
            raise RuntimeError("replay exceeded its deadline")

    def spawn(actor_id, kind, argv, env, node_id):
        settings = dict(launcher=facility.launcher(node_id), network_namespace=facility.namespace(node_id))
        if actor_id in placement:
            settings["cpu_affinity"] = placement[actor_id]
        return cluster._spawn(state, root, actor_id, kind, argv, env, guard, **settings)

    def interrupted(signum, unused_frame):
        raise KeyboardInterrupt("signal " + str(signum))

    signals = {number: signal.signal(number, interrupted) for number in (signal.SIGTERM, signal.SIGINT)}
    try:
        facility.start(guard)
        for node_id, node in nodes.items():
            directory = output / ("nfd-" + node_id)
            directory.mkdir()
            cluster._start_forwarder(config, node, state, root, directory, children,
                                     deadline, guard, spawn, disable_cs=True)
        facility.routes(output, guard)
        router_pid = next(child.pid for actor, child, _ in children
                          if actor["id"] == "nfd." + args.router)
        for directory in ("pib", "tpm"):
            (root / directory).mkdir(mode=0o700)
        env = dict(os.environ, NDN_CLIENT_PIB="pib-sqlite3:" + str(root / "pib"),
                   NDN_CLIENT_TPM="tpm-file:" + str(root / "tpm"))
        subprocess.run([str(prefix / "bin/ndnsec"), "key-gen", "-t", "e", "/spec271-nfd-fixture"],
                       env=env, stdout=subprocess.DEVNULL, check=True, timeout=10)
        for window in args.windows:
            for trial in range(args.samples):
                guard()
                cmd = [str(binary), nodes[receiver]["nfd"]["socket"], str(payload), str(window),
                       str(trial + 1), "4096", str(producer_cpu), str(consumer_cpu),
                       "digest", str(args.seconds),
                       nodes[sender]["nfd"]["socket"], str(router_pid), group["prefix"], str(wire)]
                row = dict(command=cmd, window=window, trial=trial + 1)
                measurements.append(row)
                result = subprocess.run(cmd, env=env, cwd=root, capture_output=True, text=True,
                                        timeout=args.seconds + 20)
                values = next(csv.reader(result.stdout.splitlines()), [])
                row.update(exit=result.returncode, stderr=result.stderr, metrics=dict(zip(FIELDS, values)))
                if len(values) == len(FIELDS):
                    row["payloadGbps"] = int(row["metrics"]["payload_bytes"]) * 8e-9 / float(
                        row["metrics"]["wall_s"])
        state["failure"] = None
    except BaseException as error:
        state["failure"] = str(error)
        raise
    finally:
        snapshot = cluster._close(state, root, facility)
        for number, handler in signals.items():
            signal.signal(number, handler)
        close_rows = snapshot["cleanup"] + snapshot["facility"]["cleanup"]
        closed = (not snapshot["facility"]["namespace_survivors"] and
                  not snapshot["facility"]["remaining_interfaces"] and
                  all(item["status"] == "ZERO" and not item["forced"] for item in close_rows))
        rates = {}
        for window in args.windows:
            samples = [row["payloadGbps"] for row in measurements
                       if row["window"] == window and row.get("exit") == 0 and "payloadGbps" in row]
            if len(samples) == args.samples:
                rates[window] = statistics.median(samples)
        result = dict(direction=args.direction, endpoints=dict(sender=sender, receiver=receiver),
                      cpuAffinity=sorted(os.sched_getaffinity(0)),
                      nfdAffinity={row["id"]: row["cpu_affinity"] for row in state["launches"]
                                   if "cpu_affinity" in row},
                      replayEndpointCpus=dict(producer=producer_cpu, consumer=consumer_cpu),
                      group=group, topology=config["platform"]["topology_sha256"],
                      configuration=config_digest, binarySha256=binary_digest, referenceSha256=digest(args.reference),
                      harnessSha256=digest(Path(__file__)), measurements=measurements, sustainedMedianGbps=rates,
                      failure=state.get("failure"), cleanup=close_rows, privateWorkRetained=None if closed else str(root))
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if closed:
            shutil.rmtree(root)
    return 0 if closed and rates else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("config", "reference", "store", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--direction", choices=("write", "read"), required=True)
    parser.add_argument("--cpu-placement", type=Path)
    parser.add_argument("--router", default="router")
    parser.add_argument("--group", type=int, default=0)
    parser.add_argument("--windows", type=int, nargs="+", default=(16, 32))
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--seconds", type=int, default=10)
    args = parser.parse_args()
    if (args.group < 0 or not 1 <= args.samples <= 10 or not 1 <= args.seconds <= 120 or
            not 1 <= len(args.windows) <= 8 or any(not 1 <= value <= 4096 for value in args.windows)):
        parser.error("invalid replay bounds")
    try:
        return run(args)
    except (KeyError, IndexError, OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
