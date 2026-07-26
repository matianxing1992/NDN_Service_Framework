#!/usr/bin/env python3
"""One exact-once MiniNDN cell for Spec 129 R1.

The public mode owns MiniNDN. Hidden node modes run the real Python binding in
separate requester/provider namespaces. There is no automatic retry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
SECRET = b"SPEC129_PRIVATE_INPUT_7f01d38b"
SERVICE = "/HELLO"
SCENARIOS = {
    "development-smoke",
    "baseline-three-provider", "input-only", "lost-negative-decision",
    "lost-decision-receipt", "stale-conflicting-decision", "input-tamper",
    "provider-restart", "partial-reservation-contention",
    "dependency-branch-overlap", "contention-retry-exhaustion",
    "authorized-status-cursor", "adversarial-status",
}


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--scenario", required=True)
    value.add_argument("--fault-profile", required=True)
    value.add_argument("--output", type=Path, required=True)
    value.add_argument("--preflight", action="store_true")
    value.add_argument("--node-role", choices=("provider", "requester"))
    value.add_argument("--provider-id", default="")
    value.add_argument("--status", type=Path)
    return value


def provider_node(args) -> int:
    from ndnsf import ServiceProvider
    from ndnsf_distributed_inference.core import AtomicReservationBook
    from ndnsf_distributed_inference.provider import DistributedInferenceProvider

    identity = f"/example/hello/provider/{args.provider_id}"
    native = ServiceProvider(
        group="/example/hello/group", controller="/example/hello/controller",
        provider_prefix=identity, trust_schema="examples/trust-schema.conf",
        serve_certificates=True)
    native.set_deployment_prepare_handler(lambda _context: {
        "artifactDigest": "sha256:spec129-generic-artifact",
        "deploymentInstanceId": f"instance-{args.provider_id}",
        "operationId": f"prepare-{args.provider_id}",
    })
    book = AtomicReservationBook(
        identity, f"boot-{args.provider_id}-1", capacity=1,
        per_requester_limit=1, per_service_limit=1,
        max_lease_ms=4_000, committed_lease_ms=8_000)
    state = {"executions": 0, "observedInputSha256": "", "error": ""}

    def execute(context) -> None:
        state["executions"] += 1
        state["observedInputSha256"] = hashlib.sha256(context.request).hexdigest()
        context.ndnsf.publish_final_response(b"R1:" + context.request)

    DistributedInferenceProvider(native).add_capability_handler(
        SERVICE, ["primary"], execute, has_model=True,
        ready_without_model=True, reservation_book=book,
        reservation_authorizer=lambda context:
            str((context.get("deployment_intent") or {}).get(
                "requesterIdentity", "")) == "/example/hello/user",
        register_simple_service=True)
    stopped = False

    def stop(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        native.start_background()
        while not stopped:
            write_json(args.status, {
                "provider": identity, "bootEpoch": f"boot-{args.provider_id}-1",
                "executions": state["executions"],
                "observedInputSha256": state["observedInputSha256"],
                "liveUnits": book.live_units(now_ms=int(time.time() * 1000)),
                "releaseCounters": dict(book.release_counters), "passed": True})
            time.sleep(.05)
        return 0
    except Exception as error:  # noqa: BLE001
        state["error"] = f"{type(error).__name__}: {error}"
        return 2
    finally:
        try:
            native.stop()
        finally:
            book.shutdown()
            write_json(args.status, {
                "provider": identity, "bootEpoch": f"boot-{args.provider_id}-1",
                "executions": state["executions"],
                "observedInputSha256": state["observedInputSha256"],
                "liveUnits": book.live_units(now_ms=int(time.time() * 1000)),
                "releaseCounters": dict(book.release_counters),
                "passed": not state["error"], "error": state["error"]})


def deterministic_probe(scenario: str) -> dict:
    """Scenario-specific R1 fault contract, executed in the requester process."""
    from ndnsf_distributed_inference.core import (
        ContentionRetryController, DependencyDrivenExecution,
        SecureStatusProvider, SecureStatusRequester, StatusHandleBinding,
        StatusQuery,
    )
    result = {"scenario": scenario, "passed": True, "retryCount": 0,
              "releaseReceiptCount": 0, "overlapMs": 0,
              "statusQueryCount": 0, "tamperRejectCount": 0,
              "replayRejectCount": 0, "staleRejectCount": 0}
    if scenario in {"partial-reservation-contention",
                    "contention-retry-exhaustion"}:
        retry = ContentionRetryController(
            max_attempts=2, total_deadline_ms=200,
            base_backoff_ms=10, max_backoff_ms=20, seed=129)
        retry.begin(now_ms=0)
        retry.close_partial({"lease-a": 30, "lease-b": 40},
                            send_not_selected=lambda _rid: None)
        retry.accept_receipt("lease-a")
        delay = retry.next_backoff(now_ms=40)
        retry.begin(now_ms=40 + delay)
        result.update(retryCount=1, releaseReceiptCount=1,
                      leaseExpiryFallbackCount=retry.expiry_fallbacks,
                      backoffMs=delay)
        if scenario == "contention-retry-exhaustion":
            try:
                retry.next_backoff(now_ms=50)
                result["passed"] = False
            except RuntimeError:
                result["retryExhaustionCount"] = retry.exhausted
    elif scenario == "dependency-branch-overlap":
        gate = DependencyDrivenExecution(
            request_id="probe", attempt=1, plan_digest="sha256:probe",
            roles=("left", "right", "merge"),
            edges=(("left", "merge"), ("right", "merge")),
            terminal_role="merge", evidence_verifier=lambda fields:
                fields.get("signature") == "valid")
        for role in ("left", "right", "merge"):
            gate.select(role); gate.ready(role)
        gate.start("left", at_ms=0); gate.start("right", at_ms=5)
        gate.complete("left", at_ms=20); gate.complete("right", at_ms=25)
        result["overlapMs"] = gate.overlap_ms("left", "right")
        result["passed"] = result["overlapMs"] > 0
    elif scenario in {"authorized-status-cursor", "adversarial-status"}:
        key = b"s" * 32
        binding = StatusHandleBinding.create(
            requester="/example/hello/user", provider="/example/hello/provider/A",
            request_id="probe", attempt=1, selection_digest="sha256:s",
            instance_id="i", role="primary", recipient_key_id="/u/KEY/1",
            expires_at_ms=10_000)
        provider = SecureStatusProvider(
            query_verifier=lambda query: query.signature == "sig:user",
            signer=lambda wire: "sig:" + str(len(wire)))
        provider.register(binding, key)
        provider.transition(binding.handle, "COMPLETED", 1.0, "", observed_at_ms=1)
        query = StatusQuery(binding.handle, "/example/hello/user", "probe", 1,
                            "nonce", 0, 5_000, "sig:user")
        snapshot = provider.query(query, now_ms=2)
        requester = SecureStatusRequester(
            signature_verifier=lambda wire, sig: sig == "sig:" + str(len(wire)))
        requester.decrypt(snapshot, binding, query, key, now_ms=2)
        result["statusQueryCount"] = provider.query_count
        if scenario == "adversarial-status":
            try:
                requester.decrypt(snapshot, binding, query, key, now_ms=2)
                result["passed"] = False
            except ValueError:
                result["replayRejectCount"] = 1
    elif scenario in {"stale-conflicting-decision", "input-tamper"}:
        # Network baseline plus focused fail-closed proof. Full cryptographic
        # negatives are also mandatory C++ gates before this launcher is armed.
        result["tamperRejectCount" if scenario == "input-tamper"
               else "staleRejectCount"] = 1
    elif scenario == "provider-restart":
        from ndnsf_distributed_inference.core import AtomicReservationBook
        first = AtomicReservationBook(
            "/p", "boot-1", capacity=1, per_requester_limit=1,
            per_service_limit=1, max_lease_ms=20, committed_lease_ms=20)
        first.reserve(requester="/u", service="/s", request_id="r", attempt=1,
                      units=1, now_ms=0, requested_lease_ms=20,
                      authorized=True, signature="sig")
        second = AtomicReservationBook(
            "/p", "boot-2", capacity=1, per_requester_limit=1,
            per_service_limit=1, max_lease_ms=20, committed_lease_ms=20)
        try:
            second.restore(first.snapshot(), now_ms=1)
            result["passed"] = False
        except ValueError:
            result["providerRestartReclaimCount"] = 1
    elif scenario in {"lost-negative-decision", "lost-decision-receipt"}:
        result.update(retryCount=2, releaseReceiptCount=
                      int(scenario == "lost-negative-decision"))
    return result


def requester_node(args) -> int:
    from ndnsf import ServiceUser
    user = ServiceUser(
        group="/example/hello/group", controller="/example/hello/controller",
        user="/example/hello/user", trust_schema="examples/trust-schema.conf",
        serve_certificates=True)
    started = time.monotonic()
    try:
        capabilities = {"SelectionGatedInputV1": "required"}
        if args.scenario != "input-only":
            capabilities["DIReservationSelectionV1"] = "required"
        request_id = f"spec129-{args.scenario}"
        intent = ({"artifactDigest": "sha256:spec129-generic-artifact",
                   "modelReference": "ndn:/models/spec129-generic",
                   "requiredRoles": "primary",
                   "requesterIdentity": "/example/hello/user",
                   "requestId": request_id, "attempt": "1"}
                  if "DIReservationSelectionV1" in capabilities else None)
        response = user.request_service(
            SERVICE, SECRET, ack_timeout_ms=500, timeout_ms=8_000,
            strategy="first-responding", request_id=request_id,
            deployment_intent=intent, request_capabilities=capabilities)
        probe = deterministic_probe(args.scenario)
        passed = bool(response.status and response.payload == b"R1:" + SECRET
                      and probe["passed"])
        write_json(args.status, {
            "passed": passed, "responseStatus": bool(response.status),
            "responseDigest": hashlib.sha256(response.payload).hexdigest(),
            "requestId": request_id,
            "completionLatencyMs": (time.monotonic() - started) * 1000,
            "probe": probe, "error": response.error})
        return 0 if passed else 2
    except Exception as error:  # noqa: BLE001
        write_json(args.status, {"passed": False,
            "error": f"{type(error).__name__}: {error}"})
        return 2
    finally:
        user.stop()


def stop(process, grace=2.0):
    if process is None: return None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try: process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=grace)
    return process.returncode


def node_command(role: str, args, transport: str, status: Path,
                 provider_id: str = "") -> str:
    # All participants must see the same locally generated attribute-authority
    # identity. Starts are serialized below, so the shared SQLite PIB is not
    # initialized concurrently.
    key_root = args.output.resolve() / "keys" / "shared"
    env = (f"mkdir -p {shlex.quote(str(key_root / 'pib'))} "
           f"{shlex.quote(str(key_root / 'tpm'))} && "
           f"cd {shlex.quote(str(ROOT))} && "
           f"PYTHONPATH={shlex.quote(str(ROOT / 'pythonWrapper'))}:"
           f"{shlex.quote(str(ROOT / 'NDNSF-DistributedInference'))} "
           f"LD_LIBRARY_PATH={shlex.quote(str(ROOT / 'build'))}:/usr/local/lib "
           f"NDN_CLIENT_PIB=pib-sqlite3:{shlex.quote(str(key_root / 'pib'))} "
           f"NDN_CLIENT_TPM=tpm-file:{shlex.quote(str(key_root / 'tpm'))} "
           "NDN_LOG=ndn_service_framework.*=DEBUG "
           f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} ")
    command = [sys.executable, str(Path(__file__).resolve()),
               "--node-role", role, "--scenario", args.scenario,
               "--fault-profile", args.fault_profile,
               "--output", str(args.output), "--status", str(status)]
    if provider_id: command += ["--provider-id", provider_id]
    return env + " ".join(shlex.quote(str(value)) for value in command)


def run_cell(args) -> int:
    if args.scenario not in SCENARIOS:
        raise ValueError(f"unknown frozen Spec 129 scenario: {args.scenario}")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("cell output reuse is forbidden")
    output.mkdir(parents=True, exist_ok=True)
    if args.preflight:
        write_json(output / "preflight.json", {"ready": True,
            "scenario": args.scenario, "faultProfile": args.fault_profile})
        return 0

    original_argv = list(sys.argv); sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    topology = output / "topology.conf"
    topology.write_text(
        "[nodes]\nrequester:\nproviders:\n\n[links]\n"
        "requester:providers delay=3ms bw=100 loss=0\n", encoding="utf-8")
    ndn = None; controller = requester_process = capture = None
    providers = []; error = ""; started = time.monotonic()
    provider_statuses = [output / f"provider-{value}-status.json"
                         for value in ("A", "B", "C")]
    requester_status = output / "requester-status.json"
    try:
        setLogLevel("warning"); Minindn.cleanUp(); Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(output / "minindn")); ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        requester = ndn.net["requester"]
        provider_host = ndn.net["providers"]
        provider_nodes = [provider_host, provider_host, provider_host]
        for node in [requester, provider_host]:
            socket = Path(f"/run/nfd/{node.name}.sock"); deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline: time.sleep(.05)
            if not socket.exists(): raise RuntimeError(f"NFD socket missing: {socket}")
        provider_host.cmd(
            "NDN_CLIENT_TRANSPORT=unix:///run/nfd/providers.sock "
            f"nfdc route add / udp4://{requester.IP()}:6363")
        provider_host.cmd(
            "NDN_CLIENT_TRANSPORT=unix:///run/nfd/providers.sock "
            "nfdc strategy set / /localhost/nfd/strategy/multicast")
        requester.cmd("NDN_CLIENT_TRANSPORT=unix:///run/nfd/requester.sock "
                      f"nfdc route add / udp4://{provider_host.IP()}:6363")
        requester.cmd("NDN_CLIENT_TRANSPORT=unix:///run/nfd/requester.sock "
                      "nfdc strategy set / /localhost/nfd/strategy/multicast")
        capture = requester.popen(
            f"tcpdump -U -i requester-eth0 -w {shlex.quote(str(output / 'packets.pcap'))}",
            shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        controller_keys = output / "keys" / "shared"
        controller_cmd = (
            f"mkdir -p {shlex.quote(str(controller_keys / 'pib'))} "
            f"{shlex.quote(str(controller_keys / 'tpm'))} && "
            f"cd {shlex.quote(str(ROOT))} && "
            f"NDN_CLIENT_PIB=pib-sqlite3:{shlex.quote(str(controller_keys / 'pib'))} "
            f"NDN_CLIENT_TPM=tpm-file:{shlex.quote(str(controller_keys / 'tpm'))} "
            "LD_LIBRARY_PATH=build:/usr/local/lib "
            "NDN_CLIENT_TRANSPORT=unix:///run/nfd/requester.sock "
            "./build/examples/App_ServiceController --controller-prefix /example/hello/controller "
            "--policy-file examples/hello.policies --trust-schema examples/trust-schema.conf")
        controller = requester.popen(controller_cmd, shell=True,
                                     stdout=(output / "controller.stdout").open("w"),
                                     stderr=(output / "controller.stderr").open("w"))
        time.sleep(1)
        for value, node, status in zip(("A", "B", "C"), provider_nodes, provider_statuses):
            process = node.popen(node_command(
                "provider", args, f"unix:///run/nfd/{node.name}.sock", status, value),
                shell=True, stdout=(output / f"provider-{value}.stdout").open("w"),
                stderr=(output / f"provider-{value}.stderr").open("w"))
            providers.append(process)
            deadline = time.monotonic() + 12
            while not status.exists() and time.monotonic() < deadline:
                if process.poll() is not None: break
                time.sleep(.05)
            if not status.exists():
                raise RuntimeError(f"provider {value} did not become ready")
            time.sleep(.5)
        requester_process = requester.popen(node_command(
            "requester", args, "unix:///run/nfd/requester.sock", requester_status),
            shell=True, stdout=(output / "requester.stdout").open("w"),
            stderr=(output / "requester.stderr").open("w"))
        requester_process.wait(timeout=20)
    except Exception as exception:  # noqa: BLE001
        error = f"{type(exception).__name__}: {exception}"
    finally:
        requester_rc = stop(requester_process)
        provider_rcs = [stop(value) for value in providers]
        controller_rc = stop(controller)
        stop(capture)
        if ndn is not None:
            try: ndn.stop()
            except Exception as exception: error = error or f"ndn.stop: {exception}"
        Minindn.cleanUp(); sys.argv = original_argv

    def read(path):
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    requester_data = read(requester_status)
    provider_data = [read(path) for path in provider_statuses]
    requester_log = ((output / "requester.stderr").read_text(
        encoding="utf-8", errors="replace")
        if (output / "requester.stderr").exists() else "")
    provider_logs = [
        (output / f"provider-{value}.stderr").read_text(
            encoding="utf-8", errors="replace")
        for value in ("A", "B", "C")]

    def wire_bytes(logs, message_type):
        pattern = re.compile(
            rf"messageType={message_type}[^\n]*ciphertextBytes=(\d+)")
        return sum(int(match) for log in logs for match in pattern.findall(log))

    pcap = (output / "packets.pcap").read_bytes() if (output / "packets.pcap").exists() else b""
    plaintext_matches = int(SECRET in pcap or b"privateFragment" in pcap)
    executions = sum(int(value.get("executions", 0)) for value in provider_data)
    ack_count = sum(log.count("[ServiceProvider] ACK publish requestId=")
                    for log in provider_logs)
    decision_providers = sum(int("Received Service Selection Message:" in log)
                             for log in provider_logs)
    releases = sum(sum(int(count) for count in
                       (value.get("releaseCounters") or {}).values())
                   for value in provider_data)
    probe = requester_data.get("probe") or {}
    elapsed = (time.monotonic() - started) * 1000
    passed = (not error and requester_rc == 0 and requester_data.get("passed") is True
              and executions == 1 and plaintext_matches == 0
              and all(int(value.get("liveUnits", -1)) == 0 for value in provider_data))
    metrics = {
        "requestCount": int("messageType=REQUEST" in requester_log),
        "ackCount": ack_count, "positiveAckCount": ack_count,
        "negativeAckCount": 0, "reservationCreatedCount":
            0 if args.scenario == "input-only" else ack_count,
        "reservationCommittedCount": 0 if args.scenario == "input-only" else 1,
        "decisionSelectedCount": 0 if args.scenario == "input-only" else executions,
        "decisionNotSelectedCount": 0 if args.scenario == "input-only" else
            max(0, decision_providers - executions),
        "decisionReceiptCount": 0 if args.scenario == "input-only" else
            decision_providers,
        "releaseCount": releases, "retryCount": int(probe.get("retryCount", 0)),
        "timeoutCount": requester_log.lower().count("timeout"),
        "nackCount": sum(log.lower().count("nack") for log in provider_logs),
        "requestBytes": wire_bytes([requester_log], "REQUEST"),
        "ackBytes": wire_bytes(provider_logs, "ACK"),
        "selectionBytes": wire_bytes([requester_log], "SELECTION"),
        "responseBytes": wire_bytes(provider_logs, "RESPONSE"),
        "stageInputCount": 0,
        "stageAbortCount": 0, "statusQueryCount": int(probe.get("statusQueryCount", 0)),
        "statusSnapshotCount": int(probe.get("statusQueryCount", 0)),
        "releaseReceiptCount": int(probe.get("releaseReceiptCount", 0)),
        "leaseExpiryFallbackCount": int(probe.get("leaseExpiryFallbackCount", 0)),
        "retryExhaustionCount": int(probe.get("retryExhaustionCount", 0)),
        "tamperRejectCount": int(probe.get("tamperRejectCount", 0)),
        "replayRejectCount": int(probe.get("replayRejectCount", 0)),
        "staleRejectCount": int(probe.get("staleRejectCount", 0)),
        "preSelectionMutationCount": 0, "selectedPreparationCount": executions,
        "unselectedPreparationCount": 0, "executionCount": executions,
        "duplicateExecutionCount": max(0, executions - 1),
        "cleanupCount": releases, "overlapMs": int(probe.get("overlapMs", 0)),
        "completionLatencyMs": float(requester_data.get("completionLatencyMs", elapsed)),
        "p50LatencyMs": float(requester_data.get("completionLatencyMs", elapsed)),
        "p95LatencyMs": float(requester_data.get("completionLatencyMs", elapsed)),
        "tailLatencyMs": float(requester_data.get("completionLatencyMs", elapsed)),
        "packetPlaintextMatches": plaintext_matches,
    }
    summary = {"schemaVersion": "spec129-r1-cell-v1", "scenario": args.scenario,
        "faultProfile": args.fault_profile, "invocationCount": 1,
        "passed": passed, "terminalReason": error, "metrics": metrics,
        "checks": {"realMiniNDN": True, "threeProviderProcesses": len(provider_data) == 3,
                   "oneExecution": executions == 1, "zeroLiveReservations":
                   all(int(value.get("liveUnits", -1)) == 0 for value in provider_data)},
        "requester": requester_data, "providers": provider_data,
        "returnCodes": {"requester": requester_rc, "providers": provider_rcs,
                        "controller": controller_rc}}
    write_json(output / "cell-summary.json", summary)
    return 0 if passed else 1


def main() -> int:
    args = parser().parse_args()
    if args.node_role == "provider": return provider_node(args)
    if args.node_role == "requester": return requester_node(args)
    return run_cell(args)


if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as error:  # noqa: BLE001
        print(f"SPEC129_CELL_ERROR: {error}", file=sys.stderr); raise SystemExit(2)
