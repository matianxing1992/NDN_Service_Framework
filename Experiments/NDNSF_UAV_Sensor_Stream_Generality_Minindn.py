#!/usr/bin/env python3
"""One fresh two-node Spec 144 UAV sensor-stream MiniNDN cell."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import signal
import sys
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))
from analyze_spec144_uav_sensor_stream import summarize_cell


PROFILES = {
    "zero-loss": dict(loss=0.0, delay=1.0, jitter=0.0, reorder=0.0,
                      correlation=0.0, gap=0),
    "loss": dict(loss=1.0, delay=1.0, jitter=0.0, reorder=0.0,
                 correlation=0.0, gap=0),
    "reorder": dict(loss=0.0, delay=20.0, jitter=10.0, reorder=25.0,
                    correlation=50.0, gap=5),
    "combined": dict(loss=1.0, delay=20.0, jitter=10.0, reorder=25.0,
                     correlation=50.0, gap=5),
}


def netem_command(device: str, profile: str) -> str:
    value = PROFILES[profile]
    parts = [
        "tc", "qdisc", "replace", "dev", device, "root", "netem",
        "delay", f"{value['delay']:g}ms",
    ]
    if value["jitter"]:
        parts += [f"{value['jitter']:g}ms", "distribution", "normal"]
    if value["loss"]:
        parts += ["loss", f"{value['loss']:g}%"]
    if value["reorder"]:
        parts += [
            "reorder", f"{value['reorder']:g}%",
            f"{value['correlation']:g}%", "gap", str(value["gap"]),
        ]
    return " ".join(parts)


def qdisc_matches_profile(shown: str, profile: str) -> bool:
    """Verify the effective tc JSON, not merely that some netem exists."""
    try:
        entries = json.loads(shown)
        entry = next(value for value in entries
                     if value.get("kind") == "netem" and value.get("root"))
        options = entry["options"]
        expected = PROFILES[profile]
        delay = options.get("delay", {})
        if abs(float(delay.get("delay", -1)) - expected["delay"] / 1000.0) > 1e-9:
            return False
        if abs(float(delay.get("jitter", 0)) - expected["jitter"] / 1000.0) > 1e-9:
            return False
        loss = float(options.get("loss-random", {}).get("loss", 0)) * 100.0
        if abs(loss - expected["loss"]) > 1e-9:
            return False
        reorder = options.get("reorder", {})
        if abs(float(reorder.get("reorder", 0)) * 100.0 -
               expected["reorder"]) > 1e-9:
            return False
        if abs(float(reorder.get("correlation", 0)) * 100.0 -
               expected["correlation"]) > 1e-9:
            return False
        return int(options.get("gap", 0)) == expected["gap"]
    except (KeyError, StopIteration, TypeError, ValueError, json.JSONDecodeError):
        return False


def planned_cell(output: Path, workload: str, profile: str,
                 repetition: int, *, formal: bool) -> dict:
    if workload not in {"telemetry", "acoustic"} or profile not in PROFILES:
        raise ValueError("invalid cell identity")
    return {
        "schemaVersion": "spec144-uav-sensor-cell-plan-v1",
        "cellId": f"{workload}-{profile}-r{repetition:02d}",
        "workload": workload,
        "profile": profile,
        "repetition": repetition,
        "outputPath": str(output.resolve()),
        "warmupSeconds": 5,
        "measurementSeconds": 60,
        "expectedMeasured": 1200 if workload == "telemetry" else 1500,
        "formal": bool(formal),
        "automaticRetry": False,
        "rerunAllowed": False,
    }


def build_commands(output: Path, transports: dict[str, str],
                   workload: str) -> dict[str, str]:
    binary = REPO / "build/examples/UavSensorStreamNode"
    common = (
        f"cd {shlex.quote(str(REPO))} && "
        f"LD_LIBRARY_PATH={shlex.quote(str(REPO / 'build'))}:/usr/local/lib "
        "NDN_LOG=ndn_service_framework.ServiceProvider=WARN "
        "NDNSF_TIMELINE_TRACE=1 "
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1 ")
    descriptor = output / "descriptor.json"
    publication = output / "publication.jsonl"
    return {
        "controller": (
            common + f"NDN_CLIENT_TRANSPORT={transports['provider']} "
            f"{shlex.quote(str(REPO / 'build/examples/App_ServiceController'))} "
            "--controller-prefix /example/uav/controller "
            "--policy-file NDNSF-UAV-APP/configs/uav_demo.policies "
            "--trust-schema examples/trust-schema.conf "
            f">{shlex.quote(str(output / 'controller.stdout'))} "
            f"2>{shlex.quote(str(output / 'controller.stderr'))}"),
        "provider": (
            common + f"NDN_CLIENT_TRANSPORT={transports['provider']} "
            f"{shlex.quote(str(binary))} --role provider "
            f"--workload {workload} --descriptor {shlex.quote(str(descriptor))} "
            "--warmup-seconds 5 --measurement-seconds 60 "
            f"--publication-log {shlex.quote(str(publication))} "
            f"--status {shlex.quote(str(output / 'provider-status.json'))} "
            f">{shlex.quote(str(output / 'provider.stdout'))} "
            f"2>{shlex.quote(str(output / 'provider.stderr'))}"),
        "consumer": (
            common + f"NDN_CLIENT_TRANSPORT={transports['consumer']} "
            f"{shlex.quote(str(binary))} --role consumer "
            f"--workload {workload} --descriptor {shlex.quote(str(descriptor))} "
            "--warmup-seconds 5 --measurement-seconds 60 "
            f"--admission-log {shlex.quote(str(output / 'admission.jsonl'))} "
            f"--status {shlex.quote(str(output / 'consumer-status.json'))} "
            f">{shlex.quote(str(output / 'consumer.stdout'))} "
            f"2>{shlex.quote(str(output / 'consumer.stderr'))}"),
    }


def _stop(process, grace: float = 2.0):
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


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def run(output: Path, workload: str, profile: str, repetition: int = 1,
        *, formal: bool = False) -> dict:
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("refusing reused Spec 144 cell destination")
    output.mkdir(parents=True, exist_ok=True)
    plan = planned_cell(output, workload, profile, repetition, formal=formal)
    (output / "cell-plan.json").write_text(
        json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    transports = {
        "provider": "unix:///run/nfd/provider.sock",
        "consumer": "unix:///run/nfd/consumer.sock",
    }
    commands = build_commands(output, transports, workload)
    (output / "commands.json").write_text(
        json.dumps(commands, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    topology = output / "topology.conf"
    topology.write_text(
        "[nodes]\nprovider:\nconsumer:\n\n[links]\n"
        "provider:consumer delay=1ms bw=100 loss=0\n", encoding="utf-8")
    ndn = provider = consumer = None
    controller_process = provider_process = consumer_process = None
    controller_rc = provider_rc = consumer_rc = None
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
        nfd_readiness = {}
        for node in (provider, consumer):
            socket = Path(f"/run/nfd/{node.name}.sock")
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")
            status = ""
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                status = node.cmd(
                    f"NDN_CLIENT_TRANSPORT={transports[node.name]} "
                    "nfdc status report 2>&1; printf '\\n__RC:%s\\n' $?")
                if "__RC:0" in status:
                    break
                time.sleep(0.1)
            if "__RC:0" not in status:
                raise RuntimeError(
                    f"NFD management not ready: {node.name}: {status[-240:]}")
            nfd_readiness[node.name] = status
        (output / "nfd-readiness.json").write_text(
            json.dumps(nfd_readiness, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        qdisc = {}
        for node in (provider, consumer):
            device = f"{node.name}-eth0"
            command = netem_command(device, profile)
            apply_output = node.cmd(command)
            shown = node.cmd(f"tc -j -s qdisc show dev {device}")
            if not qdisc_matches_profile(shown, profile):
                raise RuntimeError(
                    f"effective qdisc does not match {profile} on {device}")
            qdisc[node.name] = {
                "device": device, "command": command,
                "applyOutput": apply_output, "shown": shown,
            }
        (output / "effective-qdisc-before-apps.json").write_text(
            json.dumps(qdisc, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        route = consumer.cmd(
            f"NDN_CLIENT_TRANSPORT={transports['consumer']} nfdc route add "
            f"/example/uav udp4://{provider.IP()}:6363")
        (output / "consumer-route.txt").write_text(route, encoding="utf-8")
        controller_process = provider.popen(commands["controller"], shell=True)
        time.sleep(1.0)
        if controller_process.poll() is not None:
            raise RuntimeError("controller exited before provider startup")
        provider_process = provider.popen(commands["provider"], shell=True)
        descriptor = output / "descriptor.json"
        # The provider starts the real NDNSF NAC-ABE runtime before activating
        # the stream.  Allow the controller-backed key bootstrap to finish;
        # this is readiness time, not part of the 5 s + 60 s measurement.
        deadline = time.monotonic() + 35
        while not descriptor.exists() and time.monotonic() < deadline:
            if provider_process.poll() is not None:
                break
            time.sleep(0.02)
        if not descriptor.exists():
            raise RuntimeError("provider did not create descriptor")
        consumer_process = consumer.popen(commands["consumer"], shell=True)
        (output / "process-ownership.json").write_text(json.dumps({
            "launcherPid": __import__("os").getpid(),
            "controllerPid": controller_process.pid,
            "providerPid": provider_process.pid,
            "consumerPid": consumer_process.pid,
            "cleanupOwner": "single-cell-runner",
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        consumer_process.wait(timeout=82)
        consumer_rc = consumer_process.returncode
        provider_process.wait(timeout=75)
        provider_rc = provider_process.returncode
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
    finally:
        consumer_rc = _stop(consumer_process) if consumer_rc is None else consumer_rc
        provider_rc = _stop(provider_process) if provider_rc is None else provider_rc
        controller_rc = _stop(controller_process)
        if provider is not None and consumer is not None:
            final = {}
            for node in (provider, consumer):
                device = f"{node.name}-eth0"
                final[node.name] = node.cmd(
                    f"tc -j -s qdisc show dev {device}")
            (output / "effective-qdisc-at-end.json").write_text(
                json.dumps(final, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exc:
                error = error or f"ndn.stop: {type(exc).__name__}: {exc}"
        try:
            Minindn.cleanUp()
        finally:
            sys.argv = original_argv

    provider_status = _read_json(output / "provider-status.json")
    consumer_status = _read_json(output / "consumer-status.json")
    try:
        analysis = summarize_cell(
            provider_status, consumer_status, workload=workload, profile=profile)
    except Exception as exc:
        analysis = {
            "workload": workload, "profile": profile, "passed": False,
            "analysisError": f"{type(exc).__name__}: {exc}",
        }
    passed = (
        not error and provider_rc == 0 and consumer_rc == 0
        and provider_status.get("passed") is True
        and consumer_status.get("passed") is True
        and analysis.get("passed") is True)
    summary = {
        "schemaVersion": "spec144-uav-sensor-cell-v1",
        "cellId": plan["cellId"], "plan": plan,
        "passed": passed, "providerReturnCode": provider_rc,
        "consumerReturnCode": consumer_rc,
        "controllerReturnCode": controller_rc,
        "runtimeSeconds": round(time.monotonic() - started, 3),
        "error": error, "analysis": analysis,
        "automaticRetry": False, "rerunAllowed": False,
        "providerStatus": provider_status,
        "consumerStatus": consumer_status,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", required=True,
                        choices=("telemetry", "acoustic"))
    parser.add_argument("--profile", default="zero-loss",
                        choices=tuple(PROFILES))
    parser.add_argument("--repetition", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    if args.check_only:
        value = planned_cell(
            args.output, args.workload, args.profile, args.repetition,
            formal=args.formal)
        value["commands"] = build_commands(args.output, {
            "provider": "unix:///run/nfd/provider.sock",
            "consumer": "unix:///run/nfd/consumer.sock",
        }, args.workload)
        print(json.dumps(value, sort_keys=True))
        return 0
    value = run(args.output.resolve(), args.workload, args.profile,
                args.repetition, formal=args.formal)
    print(json.dumps(value, sort_keys=True))
    return 0 if value["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
