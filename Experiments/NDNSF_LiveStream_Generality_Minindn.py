#!/usr/bin/env python3
"""Single generic Spec 127 MiniNDN cell; matrix ownership arrives in T004."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import signal
import sys
import time
from typing import Optional


REPO = Path(__file__).resolve().parents[1]
LIVE_STREAM_EXAMPLES = REPO / "examples/python/live_stream"
sys.path.insert(0, str(LIVE_STREAM_EXAMPLES))

from workload_common import build_workload_manifest


def planned_cell(output: Path, workload_id: str = "periodic-sensor",
                 campaign_label: str = "spec127",
                 recovery_scheme: str | None = None) -> dict:
    manifest = build_workload_manifest(workload_id)
    return {
        "schemaVersion": f"{campaign_label}-generality-cell-plan-v1",
        "cellId": f"{workload_id}-zero-loss-r01",
        "workloadId": manifest.workload_id,
        "manifestDigest": manifest.digest,
        "periodMs": manifest.period_ms,
        "warmupSeconds": manifest.warmup_seconds,
        "measurementSeconds": manifest.measurement_seconds,
        "expectedMeasuredSamples": manifest.expected_measured_samples,
        "networkProfile": "zero-loss",
        "outputPath": str(output.resolve()),
        "automaticRetry": False,
        "rerunAllowed": False,
        "campaignLabel": campaign_label,
        "recoveryScheme": recovery_scheme or manifest.fec_rule,
    }


def build_commands(output: Path, transports: dict[str, str],
                   workload_id: str = "periodic-sensor",
                   campaign_label: str = "spec127",
                   recovery_scheme: str | None = None) -> dict[str, str]:
    build_workload_manifest(workload_id)
    output = output.resolve()
    common = (
        f"cd {shlex.quote(str(REPO))} && "
        f"PYTHONPATH={shlex.quote(str(REPO / 'pythonWrapper'))} "
        f"LD_LIBRARY_PATH={shlex.quote(str(REPO / 'build'))}:/usr/local/lib ")
    descriptor = output / "descriptor.json"
    manifest = output / "workload-manifest.json"
    publication_log = output / "publication.jsonl"
    provider_status = output / "provider-status.json"
    consumer_status = output / "consumer-status.json"
    minimum_completion_ratio = (
        "0.999" if workload_id == "periodic-sensor" else "0.99")
    return {
        "controller": (
            common + f"NDN_CLIENT_TRANSPORT={transports['provider']} "
            f"{shlex.quote(str(REPO / 'build/examples/App_ServiceController'))} "
            "--controller-prefix /example/live/controller "
            "--policy-file examples/live-stream.policies "
            "--trust-schema examples/trust-schema.conf "
            f">{shlex.quote(str(output / 'controller.stdout'))} "
            f"2>{shlex.quote(str(output / 'controller.stderr'))}"),
        "provider": (
            common + f"NDN_CLIENT_TRANSPORT={transports['provider']} "
            "python3 examples/python/live_stream/workload_provider.py "
            f"--workload {shlex.quote(workload_id)} "
            f"--campaign-label {shlex.quote(campaign_label)} "
            + (f"--recovery-scheme {shlex.quote(recovery_scheme)} "
               if recovery_scheme else "") +
            f"--descriptor {shlex.quote(str(descriptor))} "
            f"--manifest-output {shlex.quote(str(manifest))} "
            f"--publication-log {shlex.quote(str(publication_log))} "
            f"--status {shlex.quote(str(provider_status))} "
            f">{shlex.quote(str(output / 'provider.stdout'))} "
            f"2>{shlex.quote(str(output / 'provider.stderr'))}"),
        "consumer": (
            common + f"NDN_CLIENT_TRANSPORT={transports['consumer']} "
            "python3 examples/python/live_stream/workload_consumer.py "
            f"--workload {shlex.quote(workload_id)} "
            f"--descriptor {shlex.quote(str(descriptor))} "
            f"--manifest {shlex.quote(str(manifest))} "
            f"--publication-log {shlex.quote(str(publication_log))} "
            f"--status {shlex.quote(str(consumer_status))} "
            "--timeout-seconds 75 "
            "--minimum-measured-completion-ratio "
            f"{minimum_completion_ratio} "
            f">{shlex.quote(str(output / 'consumer.stdout'))} "
            f"2>{shlex.quote(str(output / 'consumer.stderr'))}"),
    }


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


def netem_command(device: str, *, loss_percent: float, delay_ms: float,
                   jitter_ms: float, reorder_percent: float,
                   reorder_correlation_percent: float, reorder_gap: int) -> str:
    parts = ["tc", "qdisc", "replace", "dev", device, "root", "netem",
             "delay", f"{delay_ms:g}ms"]
    if jitter_ms:
        parts.append(f"{jitter_ms:g}ms")
    if loss_percent:
        parts.extend(("loss", f"{loss_percent:g}%"))
    if reorder_percent:
        parts.extend(("reorder", f"{reorder_percent:g}%",
                      f"{reorder_correlation_percent:g}%", "gap",
                      str(reorder_gap)))
    return " ".join(parts)


def run(output: Path, workload_id: str = "periodic-sensor", *,
        campaign_label: str = "spec127",
        recovery_scheme: str | None = None,
        loss_percent: float = 0.0, delay_ms: float = 1.0,
        jitter_ms: float = 0.0, reorder_percent: float = 0.0,
        reorder_correlation_percent: float = 0.0,
        reorder_gap: int = 0) -> dict:
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("refusing nonempty Spec 127 output directory")
    output.mkdir(parents=True, exist_ok=True)
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    topology = output / "topology.conf"
    topology.write_text(
        "[nodes]\nprovider:\nconsumer:\n\n[links]\n"
        "provider:consumer delay=1ms bw=100 loss=0\n", encoding="utf-8")
    transports = {
        "provider": "unix:///run/nfd/provider.sock",
        "consumer": "unix:///run/nfd/consumer.sock",
    }
    commands = build_commands(output, transports, workload_id,
                              campaign_label, recovery_scheme)
    (output / "cell-plan.json").write_text(
        json.dumps(planned_cell(output, workload_id, campaign_label,
                                recovery_scheme), indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    (output / "commands.json").write_text(
        json.dumps(commands, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    ndn = None
    provider = consumer = None
    controller_process = provider_process = consumer_process = None
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
        for node in (provider, consumer):
            socket = Path(f"/run/nfd/{node.name}.sock")
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")
        qdisc = {}
        for node in (provider, consumer):
            device = f"{node.name}-eth0"
            command = netem_command(
                device, loss_percent=loss_percent, delay_ms=delay_ms,
                jitter_ms=jitter_ms, reorder_percent=reorder_percent,
                reorder_correlation_percent=reorder_correlation_percent,
                reorder_gap=reorder_gap)
            qdisc[node.name] = {"command": command, "apply": node.cmd(command),
                                "show": node.cmd(f"tc -j qdisc show dev {device}")}
        (output / "effective-qdisc-before-apps.json").write_text(
            json.dumps(qdisc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        route = consumer.cmd(
            f"NDN_CLIENT_TRANSPORT={transports['consumer']} nfdc route add "
            f"/example/live udp4://{provider.IP()}:6363")
        (output / "consumer-route.txt").write_text(route, encoding="utf-8")
        controller_process = provider.popen(commands["controller"], shell=True)
        time.sleep(1.0)
        if controller_process.poll() is not None:
            raise RuntimeError("Controller exited before Provider startup")
        provider_process = provider.popen(commands["provider"], shell=True)
        descriptor = output / "descriptor.json"
        deadline = time.monotonic() + 10
        while not descriptor.exists() and time.monotonic() < deadline:
            if provider_process.poll() is not None:
                break
            time.sleep(0.02)
        if not descriptor.exists():
            raise RuntimeError("Provider did not publish a descriptor")
        consumer_process = consumer.popen(commands["consumer"], shell=True)
        consumer_process.wait(timeout=82)
        provider_process.wait(timeout=75)
    except Exception as exception:
        error = f"{type(exception).__name__}: {exception}"
    finally:
        consumer_rc = stop_process(consumer_process)
        provider_rc = stop_process(provider_process)
        controller_rc = stop_process(controller_process)
        if provider is not None and consumer is not None:
            final_qdisc = {}
            for node in (provider, consumer):
                device = f"{node.name}-eth0"
                final_qdisc[node.name] = node.cmd(
                    f"tc -j qdisc show dev {device}")
            (output / "effective-qdisc-at-end.json").write_text(
                json.dumps(final_qdisc, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exception:
                error = error or f"ndn.stop: {type(exception).__name__}: {exception}"
        try:
            Minindn.cleanUp()
        finally:
            sys.argv = original_argv

    def read_json(name: str) -> dict:
        path = output / name
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}

    provider_status = read_json("provider-status.json")
    consumer_status = read_json("consumer-status.json")
    passed = (
        not error and provider_rc == 0 and consumer_rc == 0
        and provider_status.get("passed") is True
        and consumer_status.get("passed") is True)
    summary = {
        "schemaVersion": f"{campaign_label}-generality-cell-v1",
        "cell": planned_cell(output, workload_id, campaign_label,
                              recovery_scheme),
        "passed": passed,
        "providerReturnCode": provider_rc,
        "consumerReturnCode": consumer_rc,
        "controllerReturnCode": controller_rc,
        "providerStatus": provider_status,
        "consumerStatus": consumer_status,
        "effectiveQdiscBeforeApps": read_json("effective-qdisc-before-apps.json"),
        "effectiveQdiscAtEnd": read_json("effective-qdisc-at-end.json"),
        "runtimeSeconds": round(time.monotonic() - started, 3),
        "error": error,
        "automaticRetry": False,
        "rerunAllowed": False,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path,
                        default=REPO / "results/spec127-generality-single-cell")
    parser.add_argument("--workload", default="periodic-sensor",
                        choices=("periodic-sensor", "variable-multisegment"))
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--loss-percent", type=float, default=0.0)
    parser.add_argument("--delay-ms", type=float, default=1.0)
    parser.add_argument("--jitter-ms", type=float, default=0.0)
    parser.add_argument("--reorder-percent", type=float, default=0.0)
    parser.add_argument("--reorder-correlation-percent", type=float, default=0.0)
    parser.add_argument("--reorder-gap", type=int, default=0)
    parser.add_argument("--campaign-label", default="spec127",
                        choices=("spec127", "spec128"))
    parser.add_argument("--recovery-scheme", default=None,
                        choices=("none", "xor-one-repair", "gf256-two-repair"))
    args = parser.parse_args()
    if args.check_only:
        value = planned_cell(args.output, args.workload, args.campaign_label,
                             args.recovery_scheme)
        value["commands"] = build_commands(args.output, {
            "provider": "unix:///run/nfd/provider.sock",
            "consumer": "unix:///run/nfd/consumer.sock",
        }, args.workload, args.campaign_label, args.recovery_scheme)
        print(json.dumps(value, sort_keys=True))
        return 0
    summary = run(
        args.output.resolve(), args.workload,
        campaign_label=args.campaign_label,
        recovery_scheme=args.recovery_scheme,
        loss_percent=args.loss_percent, delay_ms=args.delay_ms,
        jitter_ms=args.jitter_ms, reorder_percent=args.reorder_percent,
        reorder_correlation_percent=args.reorder_correlation_percent,
        reorder_gap=args.reorder_gap)
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
