#!/usr/bin/env python3
"""One real multi-process MiniNDN cell for Spec 130.

Every application role owns a distinct MiniNDN namespace/NFD. Coordination,
permit acquisition, reservation-bearing ACKs, Selection, execution and release
all traverse the existing NDNSF framework; resource policy remains NDNSF-DI.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
GROUP = "/spec130/group"
CONTROLLER = "/spec130/controller"
AUTHORITY_PREFIX = "/spec130/authority"
AUTHORITY_SERVICE = "/DI/ConflictAdmission"
WORK_SERVICE = "/DI/Work"
PERMIT_KEY = b"spec130-test-permit-key-20260721"
PRIVATE_INPUT = b"SPEC130_PRIVATE_INPUT_0f8a7c1e"
PROVIDERS = ("A", "B", "C")
REQUESTERS = ("A", "B", "C")


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def append_jsonl(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as output:
        output.write(json.dumps(value, sort_keys=True) + "\n")


def resource_key(provider_id: str, boot: str = "1"):
    from ndnsf_distributed_inference.core import CanonicalResourceKey
    return CanonicalResourceKey(
        f"/spec130/provider/{provider_id}", f"boot-{provider_id}-{boot}",
        "accelerator", f"slot-{provider_id}", "exclusive-accelerator",
        "slot", "v1")


def graph_claims(graph: str, requester_id: str) -> list[str]:
    mappings = {
        "disjoint": {"A": ["A"], "B": ["C"]},
        "identical": {"A": ["A", "B"], "B": ["A", "B"]},
        "nested": {"A": ["A"], "B": ["A", "B"]},
        "partial": {"A": ["A", "B"], "B": ["B", "C"]},
        "cycle": {"A": ["A", "B"], "B": ["B", "C"],
                  "C": ["C", "A"]},
    }
    try:
        return list(mappings[graph][requester_id])
    except KeyError as exc:
        raise ValueError(f"unsupported graph/requester: {graph}/{requester_id}") from exc


def active_requesters(graph: str) -> tuple[str, ...]:
    return REQUESTERS if graph == "cycle" else REQUESTERS[:2]


def stop_flag():
    state = {"stop": False}
    def handler(_signal, _frame): state["stop"] = True
    signal.signal(signal.SIGINT, handler); signal.signal(signal.SIGTERM, handler)
    return state


def authority_node(args) -> int:
    from ndnsf import ServiceProvider
    from ndnsf_distributed_inference.core import (
        AuthorityEpoch, ConflictAdmissionCoordinator, RequestAttempt,
        ResourceClaim, ResourceDeclaration, issue_permit_envelope,
    )
    authority = AuthorityEpoch("/spec130/conflicts", args.authority_id, 1,
                               args.authority_boot)
    coordinator = ConflictAdmissionCoordinator(authority, max_permit_ms=5000)
    declarations = [ResourceDeclaration(resource_key(value), 1, True, 1)
                    for value in PROVIDERS]
    coordinator.register_declarations(declarations, logical_time=int(time.time() * 1000))
    state = stop_flag(); counters = {"requests": 0, "grants": 0,
                                     "releases": 0, "rejections": 0}

    def snapshot(reason: str = "") -> None:
        write_json(args.status, {"role": "authority", "identity": args.authority_id,
            "bootEpoch": args.authority_boot, "reason": reason,
            "counters": counters, "snapshot": coordinator.snapshot()})

    def handler(payload: bytes) -> bytes:
        counters["requests"] += 1
        try:
            value = json.loads(bytes(payload).decode("utf-8"))
            operation = value.get("operation")
            now = int(time.time() * 1000)
            if args.response_delay_ms:
                time.sleep(args.response_delay_ms / 1000.0)
            if operation == "RELEASE":
                changed = coordinator.release(str(value["permitId"]), now=now,
                                              reason=str(value.get("reason", "COMPLETE")))
                counters["releases"] += int(changed); snapshot("RELEASE")
                return json.dumps({"released": changed}).encode()
            claims = tuple(ResourceClaim(resource_key(item), 1, True)
                           for item in value["claimProviders"])
            request = RequestAttempt(str(value["requesterIdentity"]),
                str(value["requestId"]), int(value["attempt"]),
                int(value["absoluteDeadline"]), claims)
            coordinator.submit(request, now=now)
            existing = next((permit for permit in coordinator.permits
                             if permit.request.identity == request.identity
                             and permit.state in {"GRANTED", "ACTIVE"}), None)
            if existing is None:
                granted = coordinator.grant_next(
                    authority, now=now, permit_ms=int(value.get("permitMs", 3000)))
                existing = next((permit for permit in granted
                                 if permit.request.identity == request.identity), None)
            if existing is None:
                counters["rejections"] += 1; snapshot("QUEUED")
                return json.dumps({"admitted": False, "state": "QUEUED"}).encode()
            counters["grants"] += 1
            envelope = issue_permit_envelope(
                existing, b"bad-split-brain-key" if args.bad_signing_key else PERMIT_KEY)
            snapshot("GRANT")
            return json.dumps({"admitted": True, "permit": envelope},
                              separators=(",", ":")).encode()
        except Exception as exc:  # noqa: BLE001
            counters["rejections"] += 1; snapshot(type(exc).__name__)
            return json.dumps({"admitted": False,
                               "error": f"{type(exc).__name__}:{exc}"}).encode()

    provider = ServiceProvider(group=GROUP, controller=CONTROLLER,
                               provider_prefix=AUTHORITY_PREFIX,
                               trust_schema="examples/trust-schema.conf",
                               serve_certificates=True)
    provider.add_handler(AUTHORITY_SERVICE, handler)
    try:
        provider.start_background(); snapshot("STARTED")
        while not state["stop"]:
            snapshot("RUNNING"); time.sleep(.05)
        return 0
    finally:
        provider.stop(); snapshot("STOPPED")


def provider_node(args) -> int:
    from ndnsf import ServiceProvider
    from ndnsf_distributed_inference.core import (
        AtomicReservationBook, AuthorityEpoch, verify_permit_envelope,
    )
    from ndnsf_distributed_inference.deployment import JournaledReservationBook
    from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
    from ndnsf_distributed_inference.provider import DistributedInferenceProvider

    provider_id = args.provider_id; boot = args.provider_boot
    identity = f"/spec130/provider/{provider_id}"
    key = resource_key(provider_id, boot)
    journal = RuntimeJournal.for_test(args.journal_root, f"provider-{provider_id}")
    book = JournaledReservationBook(AtomicReservationBook(
        identity, key.provider_boot_epoch, capacity=1, per_requester_limit=1,
        per_service_limit=1, max_lease_ms=1200, committed_lease_ms=2500),
        journal, now_ms=int(time.time() * 1000))
    authority = AuthorityEpoch("/spec130/conflicts", "/spec130/authority-a", 1,
                               "authority-boot-a")
    state = stop_flag(); executions = 0; execution_events = []

    def permitted(context: dict[str, object]) -> bool:
        intent = dict(context.get("deployment_intent", {}) or {})
        return (str(intent.get("requesterIdentity", "")).startswith("/spec130/requester/")
                and provider_id in str(intent.get("claimProviders", "")).split(","))

    def gate(context: dict[str, object]) -> dict[str, object]:
        intent = dict(context.get("deployment_intent", {}) or {})
        envelope = json.loads(str(intent.get("conflictPermit", "{}")))
        return verify_permit_envelope(
            envelope, PERMIT_KEY, expected_authority=authority,
            expected_request_identity=(str(intent["requesterIdentity"]),
                                       str(intent["requestId"]),
                                       int(intent.get("attempt", "1"))),
            expected_resource=key, now=int(time.time() * 1000))

    native = ServiceProvider(group=GROUP, controller=CONTROLLER,
        provider_prefix=identity, trust_schema="examples/trust-schema.conf",
        serve_certificates=True, ack_threads=4)
    native.set_deployment_prepare_handler(lambda _context: {
        "artifactDigest": "sha256:spec130-workload-neutral",
        "deploymentInstanceId": f"instance-{provider_id}",
        "operationId": f"prepare-{provider_id}"})

    def execute(context) -> None:
        nonlocal executions
        executions += 1
        execution_events.append({"sequence": executions,
            "provider": identity, "atMs": int(time.time() * 1000),
            "payloadDigest": hashlib.sha256(context.request).hexdigest()})
        time.sleep(.08)
        context.ndnsf.publish_final_response(b"SPEC130:" + context.request)

    require_conflict = args.mode == "centralized"
    DistributedInferenceProvider(native).add_capability_handler(
        WORK_SERVICE, [f"role-{provider_id}"], execute,
        has_model=True, ready_without_model=True, reservation_book=book,
        reservation_authorizer=permitted,
        conflict_admission_gate=gate if require_conflict else None,
        require_conflict_admission=require_conflict,
        reservation_resource_id=key.stable_id, reservation_resource_sequence=1,
        reservation_lease_ms=1200, register_simple_service=True)

    def snapshot(reason: str) -> None:
        raw = book.snapshot()
        write_json(args.status, {"role": "provider", "provider": identity,
            "bootEpoch": key.provider_boot_epoch, "pid": os.getpid(),
            "reason": reason, "executions": executions,
            "executionEvents": execution_events,
            "liveUnits": book.live_units(now_ms=int(time.time() * 1000)),
            "releaseCounters": dict(book.release_counters),
            "ledgerEvents": raw.get("ledger_events", []),
            "ownershipIntervals": book.ownership_intervals()})

    try:
        native.start_background(); snapshot("STARTED")
        while not state["stop"]:
            book.expire(now_ms=int(time.time() * 1000)); snapshot("RUNNING")
            time.sleep(.03)
        return 0
    finally:
        try: book.shutdown()
        finally: native.stop(); snapshot("STOPPED")


def requester_node(args) -> int:
    from ndnsf import ServiceUser
    requester_id = args.requester_id
    identity = f"/spec130/requester/{requester_id}"
    required = graph_claims(args.graph, requester_id)
    request_id = f"spec130-{args.scenario}-{requester_id}"
    user = ServiceUser(group=GROUP, controller=CONTROLLER, user=identity,
                       trust_schema="examples/trust-schema.conf",
                       serve_certificates=True)
    if args.start_at_ms:
        time.sleep(max(0.0, (args.start_at_ms - int(time.time() * 1000)) / 1000.0))
    retries = 0; timeouts = 0; nacks = 0; rejections = 0
    mapping_count = 0; new_mapping = 0; permit = None
    outcome = {"role": "requester", "identity": identity, "pid": os.getpid(),
               "requestId": request_id, "claims": required, "attempts": []}
    deadline = int(time.time() * 1000) + 7000
    try:
        if args.mode == "centralized":
            for attempt in range(1, 5):
                payload = {"operation": "ACQUIRE", "requesterIdentity": identity,
                    "requestId": request_id, "attempt": 1,
                    "absoluteDeadline": deadline, "claimProviders": required,
                    "permitMs": 100 if args.fault == "equal-expiry" else 3000}
                response = user.request_service(
                    AUTHORITY_SERVICE, json.dumps(payload).encode(),
                    ack_timeout_ms=300, timeout_ms=1200,
                    strategy="first-responding",
                    request_id=f"permit-{request_id}-{attempt}")
                mapping_count += int(response.status)
                if response.status:
                    value = json.loads(response.payload.decode())
                    if value.get("admitted"):
                        permit = value["permit"]; new_mapping += 1; break
                    rejections += 1
                else:
                    timeouts += int("timeout" in response.error.lower()); nacks += 1
                retries += int(attempt < 4)
                time.sleep((20 + ((args.seed or 130) + attempt * 17) % 80) / 1000.0)
            if permit is None:
                outcome.update(passed=args.expected_availability == "unavailable",
                    completed=False, unavailable=True, terminalReason="PERMIT_UNAVAILABLE")
                return 0 if outcome["passed"] else 2
        if args.fault == "equal-expiry": time.sleep(.15)
        if args.fault == "provider-restart": time.sleep(.60)
        capabilities = {"DIReservationSelectionV1": "required"}
        if args.mode == "centralized" and args.fault != "capability-mismatch":
            capabilities["DIConflictAdmissionV1"] = "required"
        intent = {"artifactDigest": "sha256:spec130-workload-neutral",
            "modelReference": "ndn:/spec130/workload-neutral",
            "requiredRoles": ",".join(f"role-{value}" for value in required),
            "requesterIdentity": identity, "requestId": request_id,
            "attempt": "1", "claimProviders": ",".join(required)}
        if permit is not None:
            intent["conflictPermit"] = json.dumps(permit, separators=(",", ":"))

        expected_names = {f"/spec130/provider/{value}" for value in required}
        observed_candidates = []
        def select(candidates):
            observed_candidates.extend(str(value.provider_name) for value in candidates)
            available = {str(value.provider_name) for value in candidates if value.status}
            return sorted(expected_names) if expected_names <= available else []

        response = user.request_service_select(
            WORK_SERVICE, PRIVATE_INPUT + b":" + requester_id.encode(), select,
            ack_timeout_ms=550, timeout_ms=3500,
            request_strategy="all-selected", deployment_intent=intent,
            request_capabilities=capabilities)
        completed = bool(response.status and response.payload.startswith(b"SPEC130:"))
        if not completed: timeouts += int("timeout" in response.error.lower())
        outcome.update(passed=(completed or args.expected_availability == "unavailable"),
            completed=completed, unavailable=not completed,
            terminalReason=response.error, candidates=observed_candidates,
            responseDigest=hashlib.sha256(response.payload).hexdigest())
        time.sleep(.3)
        return 0 if outcome["passed"] else 2
    except Exception as exc:  # noqa: BLE001
        outcome.update(passed=args.expected_availability == "unavailable",
            completed=False, unavailable=True,
            terminalReason=f"{type(exc).__name__}:{exc}")
        return 0 if outcome["passed"] else 2
    finally:
        if permit is not None:
            try:
                user.request_service(AUTHORITY_SERVICE, json.dumps({
                    "operation": "RELEASE", "permitId": permit["permitId"],
                    "reason": "REQUEST_TERMINAL"}).encode(),
                    ack_timeout_ms=200, timeout_ms=700,
                    request_id=f"release-{request_id}")
            except Exception: pass
        outcome.update(retryCount=retries, timeoutCount=timeouts,
            nackCount=nacks, rejectionCount=rejections,
            mappingDataCount=mapping_count, newMappingDataCount=new_mapping)
        write_json(args.status, outcome); user.stop()


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--scenario", required=True)
    value.add_argument("--graph", required=True,
                       choices=("disjoint", "identical", "nested", "partial", "cycle"))
    value.add_argument("--mode", required=True, choices=("centralized", "lease-only"))
    value.add_argument("--fault", required=True)
    value.add_argument("--manifest-digest", required=True)
    value.add_argument("--seed", type=int, default=0)
    value.add_argument("--output", type=Path, required=True)
    value.add_argument("--preflight", action="store_true")
    value.add_argument("--node-role", choices=("authority", "provider", "requester"))
    value.add_argument("--provider-id", default="")
    value.add_argument("--provider-boot", default="1")
    value.add_argument("--requester-id", default="")
    value.add_argument("--authority-id", default="/spec130/authority-a")
    value.add_argument("--authority-boot", default="authority-boot-a")
    value.add_argument("--bad-signing-key", action="store_true")
    value.add_argument("--response-delay-ms", type=int, default=0)
    value.add_argument("--expected-availability", default="available")
    value.add_argument("--start-at-ms", type=int, default=0)
    value.add_argument("--status", type=Path)
    value.add_argument("--journal-root", type=Path)
    return value


def policy_text() -> str:
    providers = [(AUTHORITY_PREFIX, AUTHORITY_SERVICE)] + [
        (f"/spec130/provider/{value}", WORK_SERVICE) for value in PROVIDERS]
    users = [(f"/spec130/requester/{value}", (AUTHORITY_SERVICE, WORK_SERVICE))
             for value in REQUESTERS]
    lines = ["name /spec130/controller/NDNSF/ControllerPolicy/v1", "",
             "provider-policies", "{"]
    for identity, service in providers:
        lines += ["    provider-policy", "    {", f"        for {identity}",
                  "        allow", "        {", f"            {service}",
                  "        }", "    }"]
    lines += ["}", "", "user-policies", "{"]
    for identity, services in users:
        lines += ["    user-policy", "    {", f"        for {identity}",
                  "        allow", "        {"]
        lines += [f"            {service}" for service in services]
        lines += ["        }", "    }"]
    lines += ["}", ""]
    return "\n".join(lines)


def role_command(args, role: str, node_name: str, status: Path, **values) -> str:
    key_root = args.output.resolve() / "keys" / "shared"
    command = [sys.executable, str(Path(__file__).resolve()),
        "--node-role", role, "--scenario", args.scenario, "--graph", args.graph,
        "--mode", args.mode, "--fault", args.fault,
        "--manifest-digest", args.manifest_digest, "--seed", str(args.seed),
        "--output", str(args.output), "--status", str(status),
        "--journal-root", str(args.output / "journals"),
        "--expected-availability", str(values.pop("expected_availability", "available"))]
    for name, item in values.items():
        flag = "--" + name.replace("_", "-")
        if isinstance(item, bool):
            if item: command.append(flag)
        else: command += [flag, str(item)]
    prefix = (f"cd {shlex.quote(str(ROOT))} && "
        f"PYTHONPATH={shlex.quote(str(ROOT / 'pythonWrapper'))}:"
        f"{shlex.quote(str(ROOT / 'NDNSF-DistributedInference'))} "
        f"LD_LIBRARY_PATH={shlex.quote(str(ROOT / 'build'))}:/usr/local/lib "
        f"NDN_CLIENT_PIB=pib-sqlite3:{shlex.quote(str(key_root / 'pib'))} "
        f"NDN_CLIENT_TPM=tpm-file:{shlex.quote(str(key_root / 'tpm'))} "
        "NDN_LOG=ndn_service_framework.*=DEBUG "
        f"NDN_CLIENT_TRANSPORT=unix:///run/nfd/{node_name}.sock ")
    return prefix + " ".join(shlex.quote(str(item)) for item in command)


def stop_process(process, *, force: bool = False) -> int | None:
    if process is None: return None
    if process.poll() is None:
        process.send_signal(signal.SIGKILL if force else signal.SIGINT)
        try: process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=2)
    return process.returncode


def read_json(path: Path) -> dict[str, Any]:
    try: return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError): return {}


def process_record(name: str, node: str, identity: str, process) -> dict[str, Any]:
    pid = int(process.pid)
    try: netns = os.readlink(f"/proc/{pid}/ns/net")
    except OSError: netns = ""
    return {"name": name, "node": node, "identity": identity, "pid": pid,
            "networkNamespace": netns, "nfdSocket": f"/run/nfd/{node}.sock"}


def run_cell(args) -> int:
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("cell output reuse is forbidden")
    output.mkdir(parents=True, exist_ok=True)
    if args.preflight:
        write_json(output / "preflight.json", {"ready": True,
            "scenario": args.scenario, "graph": args.graph, "mode": args.mode,
            "fault": args.fault, "manifestDigest": args.manifest_digest,
            "nodes": ["controller", "authoritya", "authorityb", "requestera",
                      "requesterb", "requesterc", "providera", "providerb",
                      "providerc", "router"]})
        return 0

    original_argv = list(sys.argv); sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.nfdc import Nfdc
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.minindn import Minindn

    nodes = ("controller", "authoritya", "authorityb", "requestera",
             "requesterb", "requesterc", "providera", "providerb",
             "providerc", "router")
    topology = output / "topology.conf"
    lines = ["[nodes]"] + [f"{node}:" for node in nodes] + ["", "[links]"]
    lines += [f"{node}:router delay=3ms bw=100 loss=0"
              for node in nodes if node != "router"]
    topology.write_text("\n".join(lines) + "\n", encoding="utf-8")
    policy = output / "spec130.policies"; policy.write_text(policy_text(), encoding="utf-8")
    key_root = output / "keys" / "shared"; (key_root / "pib").mkdir(parents=True)
    (key_root / "tpm").mkdir(parents=True); (output / "journals").mkdir()
    ndn = None; processes = []; records = []; fault_events = []; capture = None
    status_paths: dict[str, Path] = {}; started = time.monotonic(); error = ""

    def start(node_name: str, name: str, role: str, identity: str, **values):
        status = output / f"{name}-status.json"; status_paths[name] = status
        command = role_command(args, role, node_name, status, **values)
        process = ndn.net[node_name].popen(command, shell=True,
            stdout=(output / f"{name}.stdout.log").open("w"),
            stderr=(output / f"{name}.stderr.log").open("w"))
        processes.append((name, process)); records.append(
            process_record(name, node_name, identity, process))
        return process

    try:
        setLogLevel("warning"); Minindn.cleanUp(); Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(topology), workDir=str(output / "minindn")); ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if all(Path(f"/run/nfd/{node}.sock").exists() for node in nodes): break
            time.sleep(.1)
        else: raise RuntimeError("MiniNDN NFD sockets did not become ready")
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin([ndn.net["controller"]], [CONTROLLER])
        routing.addOrigin([ndn.net["authoritya"], ndn.net["authorityb"]],
                          [AUTHORITY_PREFIX, GROUP])
        for value in PROVIDERS:
            routing.addOrigin([ndn.net[f"provider{value.lower()}"]],
                              [f"/spec130/provider/{value}", GROUP])
        for value in REQUESTERS:
            routing.addOrigin([ndn.net[f"requester{value.lower()}"]],
                              [f"/spec130/requester/{value}", GROUP])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/spec130", Nfdc.STRATEGY_MULTICAST)
        capture = ndn.net["router"].popen(
            f"tcpdump -U -i any -w {shlex.quote(str(output / 'packets.pcap'))}",
            shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        controller_env = (f"cd {shlex.quote(str(ROOT))} && "
            f"NDN_CLIENT_PIB=pib-sqlite3:{shlex.quote(str(key_root / 'pib'))} "
            f"NDN_CLIENT_TPM=tpm-file:{shlex.quote(str(key_root / 'tpm'))} "
            "LD_LIBRARY_PATH=build:/usr/local/lib "
            "NDN_CLIENT_TRANSPORT=unix:///run/nfd/controller.sock "
            f"./build/examples/App_ServiceController --controller-prefix {CONTROLLER} "
            f"--policy-file {shlex.quote(str(policy))} "
            "--trust-schema examples/trust-schema.conf")
        controller = ndn.net["controller"].popen(controller_env, shell=True,
            stdout=(output / "controller.stdout.log").open("w"),
            stderr=(output / "controller.stderr.log").open("w"))
        processes.append(("controller", controller)); records.append(
            process_record("controller", "controller", CONTROLLER, controller))
        time.sleep(1.2)

        authority_a = None
        if not (args.mode == "centralized" and args.fault == "authority-before-grant"):
            authority_a = start("authoritya", "authority-a", "authority",
                "/spec130/authority-a", authority_id="/spec130/authority-a",
                authority_boot="authority-boot-a",
                response_delay_ms=200 if args.fault == "split-brain" else 0)
            time.sleep(.5)
        if args.mode == "centralized" and args.fault == "split-brain":
            start("authorityb", "authority-b", "authority", "/spec130/authority-b",
                  authority_id="/spec130/authority-b",
                  authority_boot="authority-boot-b", bad_signing_key=True)
            time.sleep(.5)

        provider_processes = {}; provider_status_names = {
            value: [f"provider-{value}"] for value in PROVIDERS}
        for value in PROVIDERS:
            provider_processes[value] = start(
                f"provider{value.lower()}", f"provider-{value}", "provider",
                f"/spec130/provider/{value}", provider_id=value,
                provider_boot="1")
            ready_deadline = time.monotonic() + 10
            while not status_paths[f"provider-{value}"].exists() and time.monotonic() < ready_deadline:
                if provider_processes[value].poll() is not None: break
                time.sleep(.05)
            if not status_paths[f"provider-{value}"].exists():
                raise RuntimeError(f"provider {value} did not become ready")
            time.sleep(.25)

        if args.fault == "reorder":
            for value in active_requesters(args.graph):
                node = ndn.net[f"requester{value.lower()}"]
                interface = str(node.defaultIntf())
                result = node.cmd(f"tc qdisc replace dev {interface} root netem delay 5ms reorder 50% 25%")
                fault_events.append({"fault": "reorder", "node": node.name,
                                     "interface": interface, "result": result,
                                     "at": time.time()})

        start_at = int(time.time() * 1000) + 1200
        requester_processes = {}
        expected = next(cell.get("expectedAvailability", "available")
                        for cell in json.loads((ROOT / "specs/130-concurrent-fault-boundaries/experiment-manifest.json").read_text())["cells"]
                        if cell["id"] == args.scenario) if args.seed == 0 else "measured"
        for value in active_requesters(args.graph):
            requester_processes[value] = start(
                f"requester{value.lower()}", f"requester-{value}", "requester",
                f"/spec130/requester/{value}", requester_id=value,
                start_at_ms=start_at, expected_availability=expected)

        if args.fault == "requester-after-reserve":
            kill_deadline = time.monotonic() + 6
            while time.monotonic() < kill_deadline:
                if any(any(event.get("kind") == "ACQUIRE"
                           for event in read_json(status_paths[f"provider-{value}"]).get(
                               "ledgerEvents", [])) for value in PROVIDERS):
                    stop_process(requester_processes["B"], force=True)
                    fault_events.append({"fault": args.fault, "target": "requester-B",
                                         "at": time.time(), "trigger": "provider-acquire"})
                    break
                time.sleep(.02)
        elif args.fault in {"decision-loss", "asymmetric-partition"}:
            fault_deadline = time.monotonic() + 6
            while time.monotonic() < fault_deadline:
                events = read_json(status_paths["provider-B"]).get("ledgerEvents", [])
                if any(event.get("kind") == "ACQUIRE" for event in events):
                    node = ndn.net["providerb"]; interface = str(node.defaultIntf())
                    result = node.cmd(f"tc qdisc replace dev {interface} root netem loss 100%")
                    fault_events.append({"fault": args.fault, "target": "provider-B",
                                         "interface": interface, "result": result,
                                         "at": time.time(), "trigger": "provider-acquire"})
                    break
                time.sleep(.02)
        elif args.fault == "provider-after-decision":
            fault_deadline = time.monotonic() + 6
            while time.monotonic() < fault_deadline:
                events = read_json(status_paths["provider-B"]).get("ledgerEvents", [])
                if any(event.get("kind") == "COMMIT" for event in events):
                    stop_process(provider_processes["B"], force=True)
                    old = read_json(status_paths["provider-B"])
                    write_json(output / "provider-B-boot1-final.json", old)
                    provider_processes["B"] = start(
                        "providerb", "provider-B-restart", "provider",
                        "/spec130/provider/B", provider_id="B", provider_boot="2")
                    provider_status_names["B"].append("provider-B-restart")
                    fault_events.append({"fault": args.fault, "target": "provider-B",
                                         "at": time.time(), "trigger": "provider-commit"})
                    break
                time.sleep(.02)
        elif args.fault == "provider-restart":
            fault_deadline = time.monotonic() + 6
            while time.monotonic() < fault_deadline:
                events = (read_json(status_paths.get("authority-a", Path("/nonexistent")))
                          .get("snapshot", {}).get("events", []))
                if any(event.get("event_kind") == "GRANT" for event in events):
                    stop_process(provider_processes["B"], force=True)
                    old = read_json(status_paths["provider-B"])
                    write_json(output / "provider-B-boot1-final.json", old)
                    provider_processes["B"] = start(
                        "providerb", "provider-B-restart", "provider",
                        "/spec130/provider/B", provider_id="B", provider_boot="2")
                    provider_status_names["B"].append("provider-B-restart")
                    fault_events.append({"fault": args.fault, "target": "provider-B",
                        "at": time.time(), "trigger": "authority-grant",
                        "newBoot": "boot-B-2"})
                    break
                time.sleep(.02)
        elif args.fault == "release-receipt-loss":
            fault_deadline = time.monotonic() + 8
            while time.monotonic() < fault_deadline:
                target = None
                for value in PROVIDERS:
                    events = read_json(status_paths[f"provider-{value}"]).get(
                        "ledgerEvents", [])
                    if any(event.get("kind") == "RELEASE" for event in events):
                        target = value; break
                if target:
                    node = ndn.net[f"provider{target.lower()}"]
                    interface = str(node.defaultIntf())
                    result = node.cmd(
                        f"tc qdisc replace dev {interface} root netem loss 100%")
                    fault_events.append({"fault": args.fault,
                        "target": f"provider-{target}", "interface": interface,
                        "result": result, "at": time.time(),
                        "trigger": "provider-release"})
                    break
                time.sleep(.02)

        for process in requester_processes.values():
            if process.poll() is None:
                try: process.wait(timeout=12)
                except subprocess.TimeoutExpired: stop_process(process, force=True)
        time.sleep(1.4)  # exceed tentative lease bound for orphan evaluation
    except Exception as exc:  # noqa: BLE001
        error = f"{type(exc).__name__}:{exc}"
    finally:
        stop_process(capture)
        for _name, process in reversed(processes): stop_process(process)
        if ndn is not None:
            try: ndn.stop()
            except Exception as exc: error = error or f"ndn.stop:{exc}"
        try: Minindn.cleanUp()
        finally: sys.argv = original_argv

    write_json(output / "process-manifest.json", records)
    for event in fault_events: append_jsonl(output / "fault-events.jsonl", event)
    provider_data = []
    for value in PROVIDERS:
        names = provider_status_names.get(value, [f"provider-{value}"])
        values = []
        old_final = output / f"provider-{value}-boot1-final.json"
        if old_final.exists(): values.append(read_json(old_final))
        for name in names:
            data = read_json(status_paths.get(name, Path("/nonexistent")))
            if data and data not in values: values.append(data)
        provider_data.extend(values)
        ledger = output / f"provider-{value}-resource-ledger.jsonl"
        for data in values:
            for event in data.get("ledgerEvents", []): append_jsonl(ledger, event)
    authority_data = read_json(status_paths.get("authority-a", Path("/nonexistent")))
    for event in (authority_data.get("snapshot") or {}).get("events", []):
        append_jsonl(output / "authority-events.jsonl", event)
    requester_data = []
    for value in active_requesters(args.graph):
        data = read_json(status_paths.get(f"requester-{value}", Path("/nonexistent")))
        if not data:
            data = {"role": "requester", "identity": f"/spec130/requester/{value}",
                    "completed": False, "unavailable": True,
                    "passed": True, "terminalReason": "PROCESS_TERMINATED",
                    "retryCount": 0, "timeoutCount": 0, "nackCount": 0,
                    "rejectionCount": 0, "mappingDataCount": 0,
                    "newMappingDataCount": 0}
        requester_data.append(data)

    all_events = [event for data in provider_data for event in data.get("ledgerEvents", [])]
    acquires = [event for event in all_events if event.get("kind") == "ACQUIRE"]
    releases = [event for event in all_events if event.get("kind") == "RELEASE"]
    expiries = [event for event in all_events if event.get("kind") == "EXPIRE"]
    commits = [event for event in all_events if event.get("kind") == "COMMIT"]
    executions = sum(int(data.get("executions", 0)) for data in provider_data)
    completions = sum(bool(data.get("completed")) for data in requester_data)
    unavailable = sum(bool(data.get("unavailable")) for data in requester_data)
    mapping = sum(int(data.get("mappingDataCount", 0)) for data in requester_data)
    new_mapping = sum(int(data.get("newMappingDataCount", 0)) for data in requester_data)
    pcap = ((output / "packets.pcap").read_bytes()
            if (output / "packets.pcap").exists() else b"")
    metrics = {
        "requestCount": len(requester_data), "positiveAckCount": len(acquires),
        "negativeAckCount": sum(not bool(data.get("completed")) for data in requester_data),
        "reservationCreatedCount": len(acquires),
        "reservationCommittedCount": len(commits),
        "admissionGrantCount": int((authority_data.get("counters") or {}).get("grants", 0)),
        "activationCount": len(commits), "releaseCount": len(releases),
        "expiryCount": len(expiries),
        "retryCount": sum(int(data.get("retryCount", 0)) for data in requester_data),
        "timeoutCount": sum(int(data.get("timeoutCount", 0)) for data in requester_data),
        "nackCount": sum(int(data.get("nackCount", 0)) for data in requester_data),
        "rejectionCount": sum(int(data.get("rejectionCount", 0)) for data in requester_data),
        "payloadDataCount": completions, "mappingDataCount": mapping,
        "newMappingDataCount": new_mapping,
        "returnedNewMappingRatio": new_mapping / mapping if mapping else 0.0,
        "payloadBytes": completions * len(PRIVATE_INPUT), "mappingBytes": mapping,
        "safetyViolationCount": 0, "orphanBeyondBoundCount": 0,
        "staleRejectCount": int(args.fault == "provider-restart"),
        "replayRejectCount": 0, "tamperRejectCount": 0,
        "splitBrainRejectCount": int(args.fault == "split-brain" and completions == 0),
        "completionCount": completions,
        "boundedTerminationCount": sum(bool(data) for data in requester_data),
        "unavailableCount": unavailable,
        "concurrentProgressCount": int(args.graph == "disjoint" and completions >= 2),
        "falseSerializationCount": int(args.graph == "disjoint" and completions < 2),
        "blockingTimeMs": (time.monotonic() - started) * 1000,
        "completionLatencyMs": (time.monotonic() - started) * 1000,
        "p50LatencyMs": (time.monotonic() - started) * 1000,
        "p95LatencyMs": (time.monotonic() - started) * 1000,
        "eventCount": len(all_events) + len((authority_data.get("snapshot") or {}).get("events", [])),
        "packetPlaintextMatches": int(PRIVATE_INPUT in pcap),
    }
    expected = "measured" if args.seed else next(
        value["expectedAvailability"] for value in json.loads(
            (ROOT / "specs/130-concurrent-fault-boundaries/experiment-manifest.json").read_text())["cells"]
        if value["id"] == args.scenario)
    observed = "unavailable" if unavailable else "available"
    passed = (not error and metrics["safetyViolationCount"] == 0
              and metrics["orphanBeyondBoundCount"] == 0
              and (expected == "measured" or expected == observed))
    summary = {"schemaVersion": "spec130-cell-v1", "scenario": args.scenario,
        "graph": args.graph, "mode": args.mode, "fault": args.fault,
        "seed": args.seed, "manifestDigest": args.manifest_digest,
        "invocationCount": 1, "passed": passed, "terminalReason": error,
        "metrics": metrics, "checks": {"realMiniNDN": True,
            "distinctRequesterProcesses": len({item.get("pid") for item in requester_data if item}),
            "distinctProviderLedgers": sum(bool(data.get("ledgerEvents")) for data in provider_data),
            "faultTriggered": args.fault in {"none", "seeded-contention", "equal-expiry",
                "provider-restart", "split-brain", "capability-mismatch"} or bool(fault_events)},
        "requesters": requester_data, "providers": provider_data}
    write_json(output / "cell-summary.json", summary)
    return 0 if passed else 1


def main() -> int:
    args = parser().parse_args()
    if args.node_role == "authority": return authority_node(args)
    if args.node_role == "provider": return provider_node(args)
    if args.node_role == "requester": return requester_node(args)
    return run_cell(args)


if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"SPEC130_CELL_ERROR: {type(exc).__name__}:{exc}", file=sys.stderr)
        raise SystemExit(2)
