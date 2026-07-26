#!/usr/bin/env python3
"""One app-neutral MiniNDN proof of semantic-name LiveStream prefetch."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import sys
import time
from typing import Optional


REPO = Path(__file__).resolve().parents[1]


def stop_process(process, grace: float = 2.0) -> Optional[int]:
    if process is None:
        return None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=grace)
        except Exception:
            process.kill()
            process.wait(timeout=grace)
    return process.returncode


def run(output: Path, loss: int, count: int, *, start: str = "latest",
        consumer_count: int = 1, fec: bool = False) -> dict:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    output.mkdir(parents=True, exist_ok=True)
    topology = output / "topology.conf"
    topology.write_text(
        "[nodes]\nprovider:\nconsumer:\n\n[links]\n"
        f"provider:consumer delay=10ms bw=100 loss={loss}\n", encoding="utf-8")
    descriptor = output / "descriptor.json"
    latest_descriptor = output / "descriptor-latest.json"
    ndn = None
    controller_process = None
    provider_process = None
    consumer_processes = []
    error = ""
    started = time.monotonic()
    try:
        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(output / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        provider, consumer = ndn.net["provider"], ndn.net["consumer"]
        transports = {
            "provider": "unix:///run/nfd/provider.sock",
            "consumer": "unix:///run/nfd/consumer.sock",
        }
        for node in (provider, consumer):
            socket = Path(f"/run/nfd/{node.name}.sock")
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")

        route = consumer.cmd(
            f"NDN_CLIENT_TRANSPORT={transports['consumer']} nfdc route add "
            f"/example/live udp4://{provider.IP()}:6363")
        (output / "consumer-route.txt").write_text(route, encoding="utf-8")
        common = (
            f"cd {shlex.quote(str(REPO))} && "
            f"PYTHONPATH={shlex.quote(str(REPO / 'pythonWrapper'))} "
            f"LD_LIBRARY_PATH={shlex.quote(str(REPO / 'build'))}:/usr/local/lib "
        )
        provider_cmd = (
            common + f"NDN_CLIENT_TRANSPORT={transports['provider']} "
            f"python3 examples/python/live_stream/provider.py "
            f"--descriptor {shlex.quote(str(descriptor))} "
            f"--latest-descriptor {shlex.quote(str(latest_descriptor))} "
            f"--count {count} --period-ms 100 "
            f"{'--fec ' if fec else ''}"
            f">{shlex.quote(str(output / 'provider.stdout'))} "
            f"2>{shlex.quote(str(output / 'provider.stderr'))}")
        controller_cmd = (
            common + f"NDN_CLIENT_TRANSPORT={transports['provider']} "
            f"{shlex.quote(str(REPO / 'build/examples/App_ServiceController'))} "
            f"--controller-prefix /example/live/controller "
            f"--policy-file examples/live-stream.policies "
            f"--trust-schema examples/trust-schema.conf "
            f">{shlex.quote(str(output / 'controller.stdout'))} "
            f"2>{shlex.quote(str(output / 'controller.stderr'))}")
        (output / "commands.json").write_text(
            json.dumps({"controller": controller_cmd, "provider": provider_cmd,
                        "consumerStart": start, "consumerCount": consumer_count,
                        "fec": fec}, indent=2) + "\n",
            encoding="utf-8")
        controller_process = provider.popen(controller_cmd, shell=True)
        time.sleep(1.0)
        if controller_process.poll() is not None:
            raise RuntimeError("Controller exited before Provider startup")
        provider_process = provider.popen(provider_cmd, shell=True)
        deadline = time.monotonic() + 8
        while not descriptor.exists() and time.monotonic() < deadline:
            if provider_process.poll() is not None:
                break
            time.sleep(0.02)
        if not descriptor.exists():
            raise RuntimeError("provider did not activate a descriptor")
        consumer_specs = [(start, descriptor, 0)]
        if consumer_count == 2:
            consumer_specs = [("beginning", descriptor, 0)]
            latest_deadline = time.monotonic() + 5
            latest_cursor = 0
            while time.monotonic() < latest_deadline:
                try:
                    payload = json.loads(latest_descriptor.read_text(encoding="utf-8"))
                    latest_cursor = int(payload["safeJoinCursor"])
                    if latest_cursor > 0:
                        break
                except (FileNotFoundError, json.JSONDecodeError, KeyError, ValueError):
                    pass
                time.sleep(0.02)
            if latest_cursor <= 0:
                raise RuntimeError("latest descriptor did not advance")
            consumer_specs.append(("latest", latest_descriptor, latest_cursor))

        consumer_commands = []
        for index, (mode, source_descriptor, minimum_cursor) in enumerate(consumer_specs, 1):
            receipt = output / f"receipt-{index}.json"
            command = (
                common + f"NDN_CLIENT_TRANSPORT={transports['consumer']} "
                f"python3 examples/python/live_stream/consumer.py "
                f"--descriptor {shlex.quote(str(source_descriptor))} "
                f"--receipt {shlex.quote(str(receipt))} --count {count} --timeout 12 "
                f"--start {mode} "
                f"{'--minimum-count 1 ' if mode == 'latest' and consumer_count == 2 else ''}"
                f"--minimum-first-cursor {minimum_cursor} "
                f"{'--fec ' if fec else ''}"
                f">{shlex.quote(str(output / f'consumer-{index}.stdout'))} "
                f"2>{shlex.quote(str(output / f'consumer-{index}.stderr'))}")
            consumer_commands.append(command)
            consumer_processes.append(consumer.popen(command, shell=True))
            # ndn-cxx opens the shared read-mostly PIB database during process
            # initialization.  Stagger independent consumers so simultaneous
            # SQLite initialization cannot turn a stream result into a false
            # application failure; both consumers still overlap for the live
            # transfer itself.
            if index < len(consumer_specs):
                time.sleep(0.5)
        (output / "consumer-commands.json").write_text(
            json.dumps(consumer_commands, indent=2) + "\n", encoding="utf-8")
        for process in consumer_processes:
            process.wait(timeout=15)
        provider_process.wait(timeout=8)
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
    finally:
        consumer_rcs = [stop_process(process) for process in consumer_processes]
        provider_rc = stop_process(provider_process)
        controller_rc = stop_process(controller_process)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exc:
                error = error or f"ndn.stop: {type(exc).__name__}: {exc}"
        try:
            Minindn.cleanUp()
        finally:
            sys.argv = original_argv

    payloads = []
    for index in range(1, consumer_count + 1):
        receipt = output / f"receipt-{index}.json"
        payloads.append(json.loads(receipt.read_text(encoding="utf-8"))
                        if receipt.exists() else {})
    names = [[item.get("name", "") for item in payload.get("received", [])]
             for payload in payloads]
    passed = (not error and provider_rc == 0 and len(consumer_rcs) == consumer_count and
              all(rc == 0 for rc in consumer_rcs) and
              all(payload.get("passed") is True for payload in payloads) and
              all(values and all(name.startswith(
                  "/example/live/provider/samples/v=1/sample/seq=")
                  for name in values) for values in names))
    summary = {
        "schemaVersion": "spec119-live-stream-minindn-v1",
        "passed": passed,
        "lossPercent": loss,
        "start": start,
        "consumerCount": consumer_count,
        "fecEnabled": fec,
        "expected": count,
        "received": [len(values) for values in names],
        "semanticNamesOnly": bool(names) and all(
            "NDNSF/STREAM-MAP" not in name for values in names for name in values),
        "providerReturnCode": provider_rc,
        "consumerReturnCodes": consumer_rcs,
        "controllerReturnCode": controller_rc,
        "runtimeSeconds": round(time.monotonic() - started, 3),
        "error": error,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--loss", type=int, default=0)
    parser.add_argument("--count", type=int, default=12)
    parser.add_argument("--start", choices=("latest", "beginning"), default="latest")
    parser.add_argument("--consumers", type=int, choices=(1, 2), default=1)
    parser.add_argument("--fec", action="store_true")
    parser.add_argument("--output", type=Path,
                        default=REPO / "results/spec119-live-stream-minindn")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    if args.check_only:
        print(json.dumps({"passed": 0 <= args.loss <= 100 and args.count > 0}))
        return 0
    summary = run(args.output.resolve(), args.loss, args.count,
                  start=args.start, consumer_count=args.consumers, fec=args.fec)
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
