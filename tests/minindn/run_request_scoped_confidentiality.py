#!/usr/bin/env python3
"""Spec179 MiniNDN revocation/security-gate contract and launcher.

The default invocation is a non-privileged contract check.  ``--execute`` is
the explicit release gate for a real MiniNDN run; it never fabricates packet or
revocation evidence when the host cannot satisfy the tracked role-control or
privileged MiniNDN prerequisites.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple


ROOT = Path(__file__).resolve().parents[2]
TOPOLOGY = Path(__file__).with_name("spec179-topology.conf")
POLICY = ROOT / "examples/spec179-revocation.policies"
POLICY_GRANT_ONLY = ROOT / "examples/spec179-revocation-grantonly.policies"
TRUST_SCHEMA = ROOT / "examples/trust-schema.conf"
SERVICE_NAME = "/HELLO"
CONTROLLER_PREFIX = "/example/hello/controller"
PROVIDER_ROOT = "/example/hello/provider"
USER_ROOT = "/example/hello/user"
GROUP_PREFIX = "/example/hello/group"

TOPOLOGY_NODES = [
    "relay", "controller", "user-a", "user-b", "provider-a",
    "provider-b", "cache",
]

# These are the minimum scenarios in validation-matrix.md.  Each entry is a
# network behavior, not a claim that the current host has already executed it.
SCENARIOS = [
    "user-identity-revocation",
    "provider-identity-revocation",
    "service-scoped-revocation-with-unaffected-control",
    "inflight-revocation",
    "offline-rejoin-epoch-skip",
    "controller-cache-provider-status-retrieval",
    "controller-unavailable-expiry",
    "large-response-invalidation",
    "targeted-refill-invalidation",
    "stream-invalidation",
    "hintless-scheduled-refresh",
    "controller-restart",
    "selection-response-tamper-and-replay",
    "grant-only-advance",
    "grant-after-permission-exhaustion",
    "revocation-rotation-failure-retry",
]

TARGETS = ["identity", "certificate", "service"]
CUT_POINTS = [
    "before-request", "request-ack", "ack-selection", "selection-execution",
    "execution-response", "active-stream",
]
MODES = ["normal", "large-response", "targeted", "stream"]
RECOVERIES = [
    "none", "provider-restart", "controller-restart", "offline-rejoin",
    "controller-unavailable",
]

# Keep this payload deterministic so the large-response gate proves the
# request-scoped segmented path rather than merely exercising a larger log.
LARGE_RESPONSE_BYTES = 9000


def _topology_nodes() -> List[str]:
    nodes: List[str] = []
    active = False
    for raw in TOPOLOGY.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            active = True
            continue
        if line.startswith("["):
            active = False
        if active and line and not line.startswith("#"):
            nodes.append(line.split(":", 1)[0])
    return nodes


DEFAULT_BUILD_DIR = ROOT / "build-clang-spec179-nac3"


def _resolve_build_dir(build_dir: str | Path | None = None) -> Path:
    """Resolve the exact build tree used by a network campaign."""
    value = build_dir or os.environ.get("NDNSF_BUILD_DIR") or DEFAULT_BUILD_DIR
    path = Path(value)
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


def _binary_candidates(name: str, build_dir: str | Path | None = None) -> List[Path]:
    # A network result is invalid if the harness silently mixes an older
    # binary with the current source/build.  There is deliberately no stale
    # build fallback here.
    return [_resolve_build_dir(build_dir) / "examples" / name]


def _find_binary(name: str, build_dir: str | Path | None = None) -> str | None:
    for candidate in _binary_candidates(name, build_dir):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def _build_provenance(report: Mapping[str, Any]) -> Dict[str, Any]:
    """Pin the executable/library bytes actually selected for this campaign."""
    artifacts = {Path(path).resolve() for path in report["roleBinaries"].values() if path}
    library = Path(report["buildDir"]) / "libndn-service-framework.so"
    if library.is_file():
        artifacts.add(library.resolve())
        linked = subprocess.run(["ldd", str(library)], text=True,
                                capture_output=True, check=False)
        for match in re.finditer(r"=> (/\S+)", linked.stdout):
            path = Path(match.group(1))
            if path.is_file():
                artifacts.add(path.resolve())
    def digest(path: Path) -> str:
        value = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                value.update(chunk)
        return value.hexdigest()

    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                              text=True, capture_output=True, check=False)
    diff = subprocess.run(["git", "diff", "HEAD", "--"], cwd=ROOT,
                          capture_output=True, check=False)
    return {"sourceRevision": revision.stdout.strip(),
            "workingDiffSha256": hashlib.sha256(diff.stdout).hexdigest(),
            "artifactsSha256": {str(path): digest(path) for path in sorted(artifacts)}}


def _face_lookup(created_faces: Mapping[Any, Iterable[Tuple[str, str, int]]]) -> Dict[Tuple[str, str], str]:
    """Return the link-local address selected by MiniNDN for each face."""
    lookup: Dict[Tuple[str, str], str] = {}
    for node, links in created_faces.items():
        for peer, ip, _cost in links:
            lookup[(node.name, peer)] = ip
    return lookup


def _socket_for(node: Any) -> str:
    home = Path(node.params["params"]["homeDir"])
    return str(home / (node.name + ".sock"))


def _wait_socket(path: Path, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not path.exists():
        time.sleep(0.05)
    if not path.exists():
        raise RuntimeError("NFD socket not ready: %s" % path)


def _minindn_work_dir(output: Path) -> Path:
    """Use a short private path so NFD Unix socket names stay below sun_path."""
    digest = hashlib.sha256(
        str(output.resolve()).encode("utf-8")
    ).hexdigest()[:12]
    return Path(tempfile.gettempdir()) / ("s179-" + digest)


def _run_nfdc(node: Any, socket: str, command: str) -> str:
    return node.cmd("NDN_CLIENT_TRANSPORT=unix://%s nfdc %s" % (socket, command))


def _set_startup_permission_loss(node: Any, enabled: bool) -> Dict[str, Any]:
    """Bound initial permission retries to a real isolated transport outage."""
    if node.name != "user-b" or not getattr(node, "inNamespace", False):
        raise RuntimeError("permission loss requires the isolated user-b namespace")
    command = ["iptables", "--wait", "5", "-I" if enabled else "-D", "OUTPUT",
               "-p", "udp", "--dport", "6363", "-m", "comment", "--comment",
               "spec179-permission-bootstrap", "-j", "DROP"]
    stdout, stderr, code = node.pexec(command)
    if code != 0:
        raise RuntimeError("permission loss command failed: %s %s" % (stdout, stderr))
    return {"node": node.name, "enabled": enabled, "timeUs": time.time_ns() // 1000,
            "scope": "outbound UDP port 6363 in user-b namespace"}


def _configure_nfd(ndn: Any, nodes: Sequence[Any]) -> None:
    """Start one private NFD per MiniNDN application node."""
    from minindn.apps.nfd import Nfd

    for node in nodes:
        nfd = Nfd(node, logLevel="WARN")
        home = Path(node.params["params"]["homeDir"])
        desired = str(home / (node.name + ".sock"))
        conf = Path(nfd.confFile)
        conf.write_text(
            conf.read_text(encoding="utf-8").replace(str(nfd.sockFile), desired),
            encoding="utf-8",
        )
        nfd.sockFile = desired
        Path(nfd.clientConf).write_text("transport=unix://%s\n" % desired,
                                       encoding="utf-8")
        nfd.start()
        node.params["spec179_nfd"] = nfd
    for node in nodes:
        _wait_socket(Path(_socket_for(node)))


def _configure_routes(ndn: Any, nodes: Sequence[Any]) -> List[Dict[str, Any]]:
    """Install the star-topology FIB needed by the C++ role examples.

    MiniNDN creates UDP faces but does not install application routes.  The
    relay owns the more-specific Controller/User/Provider prefixes; every
    application node sends the common /example/hello namespace to the relay.
    """
    created = ndn.setupFaces()
    addresses = _face_lookup(created)
    by_name = {node.name: node for node in nodes}
    relay = by_name["relay"]
    route_records: List[Dict[str, Any]] = []

    def face_id(node: Any, peer_name: str) -> str:
        ip = addresses[(node.name, peer_name)]
        listing = _run_nfdc(node, _socket_for(node), "face list")
        pattern = r"faceid=(\d+).*remote=(?:udp|udp4)://%s:6363" % re.escape(ip)
        match = re.search(pattern, listing)
        if match is None:
            raise RuntimeError("face missing node=%s peer=%s ip=%s: %s" %
                               (node.name, peer_name, ip, listing))
        return match.group(1)

    relay_routes = [
        ("controller", CONTROLLER_PREFIX),
        ("user-a", USER_ROOT + "/A"),
        ("user-b", USER_ROOT + "/B"),
        ("provider-a", PROVIDER_ROOT + "/A"),
        ("provider-b", PROVIDER_ROOT + "/B"),
        # SVS Sync Interests are bidirectional: users must receive Provider
        # publications (ACK/Response), while Providers receive User
        # publications (Request/Selection).  The relay therefore fans the
        # shared group prefix to every participant, not only Providers.
        ("user-a", GROUP_PREFIX),
        ("user-b", GROUP_PREFIX),
        ("provider-a", GROUP_PREFIX),
        ("provider-b", GROUP_PREFIX),
    ]
    for peer_name, prefix in relay_routes:
        fid = face_id(relay, peer_name)
        result = _run_nfdc(relay, _socket_for(relay),
                           "route add %s nexthop %s" % (prefix, fid))
        route_records.append({"node": "relay", "peer": peer_name,
                              "prefix": prefix, "faceId": int(fid),
                              "result": result.strip()})

    # A shared SVS group prefix is a logical multicast channel.  Best-route
    # forwarding would select one equal-cost nexthop, making the direction
    # that happens to lose the selection invisible (e.g., Users would not
    # learn Provider ACKs).  Use NFD's multicast strategy only on this
    # harness-owned group prefix; application Data prefixes remain unicast.
    strategy_result = _run_nfdc(
        relay, _socket_for(relay),
        "strategy set %s /localhost/nfd/strategy/multicast" % GROUP_PREFIX)
    route_records.append({"node": "relay", "prefix": GROUP_PREFIX,
                          "strategy": "multicast",
                          "result": strategy_result.strip()})

    for node in nodes:
        if node.name == "relay":
            continue
        fid = face_id(node, "relay")
        result = _run_nfdc(node, _socket_for(node),
                           "route add /example/hello nexthop %s" % fid)
        route_records.append({"node": node.name, "peer": "relay",
                              "prefix": "/example/hello", "faceId": int(fid),
                              "result": result.strip()})
    return route_records


def _wait_marker(path: Path, marker: str,
                 processes: Sequence[Tuple[str, Any, Any]], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
        if marker in text:
            return
        for name, proc, _log in processes:
            if proc.poll() is not None and proc.returncode not in (0, -signal.SIGINT):
                if path.name == "%s.log" % name:
                    raise RuntimeError("%s exited before marker %s" % (name, marker))
        time.sleep(0.1)
    raise RuntimeError("%s did not emit marker %r" % (path, marker))


def _launch_process(ndn: Any, node_name: str, process_name: str, command: str,
                    output: Path, shared_keychain: Path,
                    processes: List[Tuple[str, Any, Any]],
                    extra_env: Mapping[str, str] | None = None) -> Any:
    """Launch one role with an explicit transport and shared PIB/TPM.

    The Controller must encrypt permission snapshots to identities created by
    the same keychain.  A shared, campaign-scoped PIB/TPM is therefore an
    explicit part of this fixture; NFD sockets remain per-node isolated.
    """
    from minindn.util import getPopen

    node = ndn.net[node_name]
    log_path = output / (process_name + ".log")
    log = log_path.open("wb")
    socket = _socket_for(node)
    home = Path(node.params["params"]["homeDir"])
    env = {
        "HOME": str(home),
        "NDN_CLIENT_TRANSPORT": "unix://%s" % socket,
        "NDN_CLIENT_PIB": "pib-sqlite3:%s" % (shared_keychain / "pib"),
        "NDN_CLIENT_TPM": "tpm-file:%s" % (shared_keychain / "tpm"),
        # App_User/App_Provider expose their readiness markers through
        # ndn-cxx logging rather than stdout.  Keep the role log observable so
        # a missing marker distinguishes startup failure from a quiet logger.
        # TRACE is intentional for this bounded release gate: targeted and
        # stream lifecycle markers are emitted at trace level, and the
        # evidence collector must not infer those paths from CSV alone.
        "NDN_LOG": os.environ.get("NDNSF_MININDN_LOG", "ndn_service_framework.*=TRACE"),
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "1",
        "PYTHONUNBUFFERED": "1",
    }
    if extra_env:
        env.update({str(key): str(value) for key, value in extra_env.items()})
    proc = getPopen(node, command, envDict=env, shell=True,
                    stdout=log, stderr=subprocess.STDOUT)
    processes.append((process_name, proc, log))
    return proc


def _read_csv_rows(path: Path) -> List[Dict[str, str]]:
    if not path.is_file():
        return []
    try:
        with path.open(newline="", encoding="utf-8", errors="replace") as stream:
            return list(csv.DictReader(stream))
    except (OSError, csv.Error):
        return []


def _collect_runtime_evidence(output: Path, scenario: str,
                              config: Mapping[str, Any],
                              processes: Sequence[Tuple[str, Any, Any]],
                              started: float) -> Dict[str, Any]:
    """Collect only evidence observable in logs/CSV; missing values stay null."""
    log_text: Dict[str, str] = {}
    for path in sorted(output.glob("*.log")):
        log_text[path.name] = path.read_text(encoding="utf-8", errors="replace")
    all_text = "\n".join(log_text.values())
    versions = re.findall(r"(?:generation|controllerGenerationTimestamp)=(\d+).*?epoch=(\d+)",
                         all_text)
    applied = re.findall(r"NDNSF_REVOCATION_APPLIED success=(\d+).*?generation=(\d+) epoch=(\d+)",
                         all_text)

    lifecycle_rows: List[Dict[str, str]] = []
    for path in sorted(output.glob("user-*/request_lifecycle.csv")):
        lifecycle_rows.extend(_read_csv_rows(path))
    provider_rows: List[Dict[str, str]] = []
    for path in sorted(output.glob("provider-*/provider-lifecycle.csv")):
        provider_rows.extend(_read_csv_rows(path))

    selected = [row.get("selected_provider", "") for row in lifecycle_rows
                if row.get("selected_provider") not in (None, "", "-")]
    terminal_reasons = [row.get("final_cleanup_reason", "") for row in lifecycle_rows
                        if row.get("final_cleanup_reason") not in (None, "", "-")]
    # App_Provider records the lifecycle transition that proves handler
    # execution, but older/current callback snapshots may leave the timestamp
    # fields at zero on EXECUTION_DONE/RESPONSE_PUBLISHED rows.  Treat the
    # semantic state as authoritative and count each provider/request once;
    # otherwise a real response path is reported as executionCount=0.
    execution_keys = {
        (row.get("provider_name", ""), row.get("request_id", ""))
        for row in provider_rows
        if row.get("state") == "EXECUTION_DONE" and row.get("request_id")
    }
    request_publication_count = len(re.findall(
        r"NDNSF_PUBLICATION_AUDIT.*type=REQUEST.*validated=true", all_text))
    response_publication_count = len(re.findall(
        r"NDNSF_PUBLICATION_AUDIT.*type=RESPONSE.*validated=true", all_text))
    selection_publication_count = len(re.findall(
        r"NDNSF_PUBLICATION_AUDIT.*type=SELECTION.*validated=true", all_text))
    response_decrypted_count = len(re.findall(
        r"NDNSF_REQUEST_SCOPED_RESPONSE_DECRYPTED\b", all_text))
    large_response_published_count = len(re.findall(
        r"NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_PUBLISHED\b", all_text))
    large_response_resolved_count = len(re.findall(
        r"NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_RESOLVED\b", all_text))
    targeted_created_count = len(re.findall(
        r"event=TARGETED_REQUEST_CREATED\b", all_text))
    targeted_fast_path_count = len(re.findall(
        r"event=TARGETED_REQUEST_CREATED\b.*fastPath=1", all_text))
    targeted_bootstrap_count = len(re.findall(
        r"event=TARGETED_TOKEN_BATCH_ATTACHED\b", all_text))
    targeted_batch_stored_count = len(re.findall(
        r"event=TARGETED_TOKEN_BATCH_STORED\b", all_text))
    targeted_refill_count = len(re.findall(
        r"event=TARGETED_TOKEN_REFILL_REQUESTED\b", all_text))
    targeted_refill_failed_count = len(re.findall(
        r"event=TARGETED_TOKEN_REFILL_FAILED\b", all_text))
    targeted_accepted_count = len(re.findall(
        r"event=TARGETED_REQUEST_ACCEPTED\b", all_text))
    stream_event_count = len(re.findall(
        r"event=STREAM_EVENT_(?:PUBLISHED|OBSERVED|DELIVERED)\b", all_text))
    stream_event_published_count = len(re.findall(
        r"SPEC179_STREAM_EVENT_PUBLISHED\b", all_text))
    stream_event_received_count = len(re.findall(
        r"SPEC179_STREAM_EVENT_RECEIVED\b", all_text))
    stream_complete_count = len(re.findall(
        r"SPEC179_STREAM_(?:COMPLETE|COMPLETE_RECEIVED)\b", all_text))
    stream_error_count = len(re.findall(
        r"SPEC179_STREAM_ERROR\b", all_text))
    refresh_lines = sum(
        len(re.findall(pattern, all_text, flags=re.IGNORECASE))
        for pattern in (r"refresh", r"controller.?status", r"policy.?status"))

    # ---- grant-only-advance scenario evidence ----
    # The controller grants one /HELLO attribute to a wave-1-ungranted user
    # while the ABE pair stays byte-identical.  Per-identity DKEY refresh
    # events are attributed by log file (identity leaf), so the assertions
    # are exact: the granted identity performs exactly one epoch>=2 refresh
    # in the advance wave; every unaffected identity performs none.
    grant_only: Dict[str, Any] = {}
    if config.get("grantOnlyAdvance"):
        ident_events: Dict[Tuple[str, str], Dict[str, Any]] = {}
        for name, text in log_text.items():
            match = re.match(r"^(user|provider|controller)-(.+?)\.log$", name)
            if not match:
                continue
            group, leaf = match.group(1), match.group(2)
            requested = [
                (int(epoch), reason)
                for epoch, reason in re.findall(
                    r"NDNSF_NAC_DKEY_REFRESH_REQUESTED\b[^\n]*?\bepoch=(\d+)[^\n]*?\breason=([A-Za-z-]+)",
                    text)]
            ident_events[(group, leaf)] = {
                "requested": requested,
                "pending": len(re.findall(r"NDNSF_NAC_DKEY_REFRESH_PENDING\b", text)),
                "notRequired": len(re.findall(
                    r"NDNSF_NAC_DKEY_REFRESH_NOT_REQUIRED\b", text)),
                "cacheInvalidated": len(re.findall(
                    r"NDNSF_CONTROLLER_CACHE_INVALIDATED\b", text)),
            }
        grant_markers = re.findall(
            r"NDNSF_GRANT_ONLY_APPLIED success=(\d+) identity=(\S+) "
            r"service=(\S+) generation=(\d+) epoch=(\d+)",
            log_text.get("controller-1.log", ""))
        # Controller-level grant-only classification: the advance must leave
        # the ABE pair untouched (grant() adds only the target identity's
        # /PERMISSION attribute; abeParametersUnchanged=true on the same
        # controller marker line records that no key/params rotation ran).
        abe_marker = re.search(
            r"NDNSF_CONTROLLER_GRANT\b[^\n]*\babeParametersUnchanged=(true|false)",
            log_text.get("controller-1.log", ""))
        grant_abe_unchanged = (abe_marker.group(1) == "true"
                               if abe_marker else None)
        grant_time_us = None
        # The app-level NDNSF_GRANT_ONLY_APPLIED (std::cout) and the
        # controller-level NDNSF_CONTROLLER_GRANT (NDN_LOG) describe the same
        # instant but stdout interleaving can drop the timestamp from either;
        # take the last timestamped marker line.
        grant_ts_lines = re.findall(
            r"^([0-9]+\.[0-9]+)\s+.*NDNSF_(?:GRANT_ONLY_APPLIED|CONTROLLER_GRANT)\b",
            log_text.get("controller-1.log", ""), flags=re.MULTILINE)
        if grant_ts_lines:
            grant_time_us = int(round(float(grant_ts_lines[-1]) * 1_000_000))
        granted_leaf = str(config["grantIdentity"]).rstrip("/").rsplit("/", 1)[-1]
        granted_key = ("user", granted_leaf)
        unaffected_keys = sorted(key for key in ident_events
                                 if key[0] in ("user", "provider")
                                 and key != granted_key)
        granted_ev = ident_events.get(granted_key, {"requested": [], "pending": 0,
                                                    "notRequired": 0,
                                                    "cacheInvalidated": 0})
        granted_epoch_ge2 = [(epoch, reason) for epoch, reason
                             in granted_ev["requested"] if epoch >= 2]
        unaffected_epoch_ge2 = [
            {"identity": "%s-%s" % key, "fetches":
             [(epoch, reason) for epoch, reason in ev["requested"] if epoch >= 2]}
            for key, ev in ident_events.items()
            if key in unaffected_keys]
        # Request rows: a wave-1-ungranted identity is bootstrap-held until the
        # advance installs (its PERMISSIONS/USER stays unanswered and the user
        # library logs "Waiting for decryption key"), so every enqueued request
        # row must post-date the grant marker -- deterministic preclusion of
        # the new capability before installation, not a mid-window race.
        granted_dir = output / ("user-%s" % granted_leaf)
        granted_rows = [row for row in _read_csv_rows(granted_dir / "request-results.csv")
                        if row.get("request_id")]
        granted_successes = [row for row in granted_rows
                             if row.get("success", "").strip() == "1"]
        granted_lifecycle = _read_csv_rows(granted_dir / "request_lifecycle.csv")
        grant_row_enqueue_us: Dict[str, int] = {}
        for row in granted_lifecycle:
            rid = row.get("request_id", "")
            if not rid:
                continue
            try:
                enqueue_us = int(row.get("enqueue_timestamp_us", "0") or 0)
            except ValueError:
                continue
            if enqueue_us > 0:
                grant_row_enqueue_us[rid] = min(
                    enqueue_us, grant_row_enqueue_us.get(rid, enqueue_us))
        granted_first_enqueue_us = (
            min(grant_row_enqueue_us.values()) if grant_row_enqueue_us else None)
        granted_pre_grant_rows = (
            sum(1 for enqueue_us in grant_row_enqueue_us.values()
                if grant_time_us is not None and enqueue_us < grant_time_us)
            if grant_time_us is not None else None)
        # Retain every terminal row, including failures. Log timestamps do
        # not prove process termination; using the earliest provider log as
        # a cutoff silently discarded all failed requests. Scenario lifetime
        # must include bootstrap, workload and drain instead of censoring reds.
        # Old runs held construction until the first DKEY. New runtimes let
        # the App run but reject protected requests before authority arrives.
        # Retain both forms for historical reanalysis; a pending marker alone
        # is never sufficient negative evidence.
        granted_log_text = log_text.get("user-%s.log" % granted_leaf, "")
        resolution_us = None
        for marker_pattern in ("NDNSF_NAC_DKEY_REFRESH_PENDING",
                               "NDNSF_NAC_DKEY_REFRESH_REQUESTED"):
            marker_ts = re.search(
                r"^([0-9]+\.[0-9]+)\s+.*" + marker_pattern + r"\b",
                granted_log_text, flags=re.MULTILINE)
            if marker_ts:
                resolution_us = int(round(float(marker_ts.group(1)) * 1_000_000))
                break
        waiting_ts_us = [int(round(float(ts) * 1_000_000)) for ts in re.findall(
            r"^([0-9]+\.[0-9]+)\s+.*Waiting for decryption key",
            granted_log_text, flags=re.MULTILINE)]
        denial_ts_us = [int(round(float(ts) * 1_000_000)) for ts in re.findall(
            r"^([0-9]+\.[0-9]+)\s+.*(?:NDNSF_USER_REVOCATION_REJECT|"
            r"Reject request (?:under revoked Controller status|without installed ControllerVersion|"
            r"without user permission|without decryption readiness))", granted_log_text, flags=re.MULTILINE)]
        blocked_ts_us = waiting_ts_us + denial_ts_us
        granted_pre_resolve_waiting = (
            sum(1 for ts_us in blocked_ts_us if ts_us < resolution_us)
            if resolution_us is not None else len(blocked_ts_us))
        # Unaffected identities must observe the new epoch and decline to
        # refetch: NOT_REQUIRED reason=same-generation-no-grant and/or
        # CACHE_INVALIDATED with abeGenerationChanged=false at epoch>=2.
        unaffected_epoch2_observed = {}
        for key in unaffected_keys:
            text = log_text.get("%s-%s.log" % (key[0], key[1]), "")
            unaffected_epoch2_observed["%s-%s" % key] = len([
                epoch for epoch in re.findall(
                    r"NDNSF_(?:NAC_DKEY_REFRESH_NOT_REQUIRED|CONTROLLER_CACHE_INVALIDATED)\b"
                    r"[^\n]*?\bepoch=(\d+)", text)
                if int(epoch) >= 2])
        granted_reasons = sorted({
            row.get("final_cleanup_reason", "")
            for row in granted_lifecycle
            if row.get("final_cleanup_reason") not in (None, "", "-")})
        control_all_rows = [row for row in _read_csv_rows(output / "user-A" / "request-results.csv")
                            if row.get("request_id")]
        control_rows = [row for row in control_all_rows
                        if row.get("success", "").strip() == "1"]
        control_enqueue = {}
        for row in _read_csv_rows(output / "user-A" / "request_lifecycle.csv"):
            try:
                ts = int(row.get("enqueue_timestamp_us", "0") or 0)
            except ValueError:
                continue
            if ts > 0:
                key = row.get("request_id")
                control_enqueue[key] = min(ts, control_enqueue.get(key, ts))
        grant_only = {
            "grantApplied": bool(grant_markers and grant_markers[-1][0] == "1"),
            "grantMarker": {
                "success": int(grant_markers[-1][0]),
                "identity": grant_markers[-1][1],
                "service": grant_markers[-1][2],
                "generation": int(grant_markers[-1][3]),
                "epoch": int(grant_markers[-1][4]),
            } if grant_markers else None,
            "grantAbeUnchanged": grant_abe_unchanged,
            "grantTimeUs": grant_time_us,
            "grantedIdentity": "%s-%s" % granted_key,
            "grantedEpochGE2Fetches": len(granted_epoch_ge2),
            "grantedEpochGE2Detail": granted_epoch_ge2,
            "grantedPending": granted_ev["pending"],
            "grantedNotRequired": granted_ev["notRequired"],
            "grantedCacheInvalidated": granted_ev["cacheInvalidated"],
            "unaffectedEpochGE2Fetches": sum(
                len(fetches) for fetches in
                (entry["fetches"] for entry in unaffected_epoch_ge2)),
            "unaffectedDetail": unaffected_epoch_ge2,
            "unaffectedEpoch2Observed": unaffected_epoch2_observed,
            "unaffectedPending": {("%s-%s" % key): ev["pending"]
                                  for key, ev in ident_events.items()
                                  if key in unaffected_keys},
            "unaffectedCacheInvalidated": {("%s-%s" % key): ev["cacheInvalidated"]
                                           for key, ev in ident_events.items()
                                           if key in unaffected_keys},
            "grantedRows": len(granted_rows),
            "grantedSuccessRows": len(granted_successes),
            "grantedFailures": len(granted_rows) - len(granted_successes),
            "grantedPreGrantRows": granted_pre_grant_rows,
            "grantedFirstEnqueueUs": granted_first_enqueue_us,
            "grantedPreResolveWaiting": granted_pre_resolve_waiting,
            "grantedPreResolveDenials": sum(
                1 for ts in denial_ts_us if resolution_us is None or ts < resolution_us),
            "grantedTerminalReasons": granted_reasons,
            "unaffectedControlSuccessRows": len(control_rows),
            "unaffectedControlRows": len(control_all_rows),
            "unaffectedControlFailures": len(control_all_rows) - len(control_rows),
            "unaffectedControlPostGrantSuccessRows": sum(
                1 for row in control_rows if grant_time_us is not None and
                control_enqueue.get(row["request_id"], 0) > grant_time_us),
        }
        if config.get("permissionRefetchAfterMs"):
            def marker_time_us(pattern: str) -> int | None:
                match = re.search(r"^([0-9]+\.[0-9]+)\s+.*" + pattern,
                                  granted_log_text, flags=re.MULTILINE)
                return int(round(float(match.group(1)) * 1_000_000)) if match else None

            exhausted_us = marker_time_us(r"PermissionResponse timeout:.*final=1")
            refetch_us = marker_time_us(r"NDNSF_APP_PERMISSION_REFETCH\b")
            grant_only["permissionExhaustedTimeUs"] = exhausted_us
            grant_only["explicitRefetchTimeUs"] = refetch_us
            grant_only["explicitRenewalObserved"] = bool(
                grant_time_us is not None and refetch_us is not None and
                granted_first_enqueue_us is not None and
                grant_time_us < refetch_us <= granted_first_enqueue_us)
            grant_only["unaffectedControlPostRefetchSuccessRows"] = sum(
                1 for row in control_rows if refetch_us is not None and
                control_enqueue.get(row["request_id"], 0) > refetch_us)
            grant_only["lateRenewalObserved"] = bool(
                exhausted_us is not None and grant_time_us is not None and
                refetch_us is not None and granted_first_enqueue_us is not None and
                exhausted_us < grant_time_us < refetch_us <= granted_first_enqueue_us and
                any(exhausted_us < ts < refetch_us for ts in denial_ts_us))
    trace_material = "\n".join(
        "%s:%s" % (name, text) for name, text in sorted(log_text.items()))
    trace_hash = "sha256:" + hashlib.sha256(trace_material.encode("utf-8")).hexdigest()
    return {
        "statusSource": "controller" if applied else None,
        "controllerVersion": {
            "generation": int(applied[-1][1]),
            "epoch": int(applied[-1][2]),
        } if applied else ({"generation": int(versions[-1][0]),
                            "epoch": int(versions[-1][1])} if versions else None),
        "refreshAttempts": refresh_lines if refresh_lines else None,
        "invalidatedCacheCount": None,
        "executionCount": len(execution_keys),
        "requestPublicationCount": request_publication_count,
        "selectionPublicationCount": selection_publication_count,
        "responsePublicationCount": response_publication_count,
        "responseDecryptedCount": response_decrypted_count,
        "largeResponsePublishedCount": large_response_published_count,
        "largeResponseResolvedCount": large_response_resolved_count,
        "targetedRequestCount": targeted_created_count,
        "targetedFastPathCount": targeted_fast_path_count,
        "targetedBootstrapCount": targeted_bootstrap_count,
        "targetedBatchStoredCount": targeted_batch_stored_count,
        "targetedRefillCount": targeted_refill_count,
        "targetedRefillFailedCount": targeted_refill_failed_count,
        "targetedAcceptedCount": targeted_accepted_count,
        "streamEventCount": stream_event_count,
        "streamEventPublishedCount": stream_event_published_count,
        "streamEventReceivedCount": stream_event_received_count,
        "streamCompleteCount": stream_complete_count,
        "streamErrorCount": stream_error_count,
        "terminalOwner": selected[-1] if selected else None,
        "terminalReason": terminal_reasons[-1] if terminal_reasons else None,
        "redactedTraceHash": trace_hash,
        "revocationApplied": bool(applied and applied[-1][0] == "1"),
        "processReturnCodes": {name: proc.poll() for name, proc, _log in processes},
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "scenarioMode": config["mode"],
        "scenarioRecovery": config["recovery"],
        "observedLogs": sorted(log_text),
        # Startup logs alone are not packet evidence.  Require a validated
        # Request publication before calling this a cross-process run.
        "networkEvidence": request_publication_count > 0,
        **({"grantOnly": grant_only} if config.get("grantOnlyAdvance") else {}),
    }


def _scenario_config(scenario: str) -> Dict[str, Any]:
    """Map a named matrix scenario to an executable, bounded probe.

    Mode labels must correspond to process arguments and observable evidence;
    a normal Request run is not accepted as proof for Targeted, segmented, or
    streamed paths.
    """
    config: Dict[str, Any] = {
        "revokeKind": "identity",
        "revokeIdentity": USER_ROOT + "/A",
        "revokeService": "",
        "revokeAttribute": "",
        "revokeCertificateDigest": "",
        # Role startup and permission/bootstrap traffic consume roughly
        # 3--4 seconds in the wired MiniNDN fixture.  Leave a real pre-
        # revocation window so at least one request can reach Selection and
        # execution before the negative half of the scenario begins.
        "revokeAfterMs": 5500,
        "providerRequestDelayMs": 0,
        "controllerRestart": False,
        "mode": "normal",
        "recovery": "none",
        # Open-loop request window (1 rps per user).  Revocation scenarios
        # stretch it to 16 s so the request stream straddles the revocation:
        # a bounded run of successes before it, then bounded local denials
        # after the affected role discovers the new epoch on its own refresh.
        "requestCount": 16,
        "requestDurationMs": 16000,
        # Roles run for lifetime_ms + 3000 ms (see user_command).  The user
        # open loop needs role startup (~3 s) + requestDurationMs + drain
        # (6 s) of *live* time before it flushes its results CSV, so the
        # default lifetime must clear that sum or the run-for deadline stops
        # the face before the benchmark finalizes and the request-results
        # CSV stays empty.  Scenarios with delayed/in-flight windows that
        # must drain inside the role window override this upward.
        "lifetimeMs": 35000,
        # role -> NDNSF_POLICY_REVALIDATION_PERIOD_MS.  A fixed period forces
        # the scheduled-refresh path to re-fetch even while the signed status
        # is far from expiry; absent roles keep the production near-expiry
        # schedule, which never fires inside a ~20 s probe.  Without the
        # knob a revocation is discovered by nobody inside the window and the
        # run is "applied-but-dormant" -- not evidence.  The revocation
        # family knobs every role; hintless/unavailable/offline scenarios
        # replace the dict with only the roles that must refresh.
        "knobMs": {"userA": 1000, "userB": 1000,
                   "providerA": 1000, "providerB": 1000},
        # S2: SIGINT provider A after its epoch-2 discovery window has
        # closed, then relaunch it under the same shared PIB (provider-A-2).
        "restartProviderAfterMs": 0,
        # S5: SIGSTOP user A at stopUserAMs, SIGCONT at contUserAMs.
        "stopUserAMs": 0,
        "contUserAMs": 0,
        # S12: controller-2 performs its own revocation this many ms after
        # the restarted authority begins its bounded window.
        "controller2RevokeAfterMs": 0,
    }
    if scenario == "revocation-rotation-failure-retry":
        config.update({"revokeAfterMs": 20000, "revokeRetryAfterMs": 26000,
                       "rotationFaultOnce": True, "lifetimeMs": 80000,
                       "requestCount": 40, "requestDurationMs": 40000,
                       "recovery": "same-target-rekey-retry"})
    elif scenario == "provider-identity-revocation":
        config.update({"revokeIdentity": PROVIDER_ROOT + "/A",
                       "recovery": "provider-restart",
                       "restartProviderAfterMs": 8500})
    elif scenario == "service-scoped-revocation-with-unaffected-control":
        # Withdraw provider/A's provision authority for /HELLO only: the ABE
        # attribute removed is /SERVICE/<service> while provider/B and both
        # users keep their own authority untouched.
        config.update({"revokeKind": "service",
                       "revokeIdentity": PROVIDER_ROOT + "/A",
                       "revokeService": SERVICE_NAME,
                       "revokeAttribute": "/SERVICE" + SERVICE_NAME})
    elif scenario == "inflight-revocation":
        # Revoke user/A while its requests are admitted with a 3 s provider
        # delay: pre-revoke rows are still in flight across the boundary.
        config.update({"revokeAfterMs": 5200,
                       "providerRequestDelayMs": 3000,
                       "lifetimeMs": 35000})
    elif scenario == "offline-rejoin-epoch-skip":
        # Freeze user/A across two controller version advances (grant C ->
        # epoch 2, revoke provider/A -> epoch 3).  user/A is frozen across
        # both advances and on SIGCONT must jump straight to epoch 3 without
        # ever installing epoch 2.  Every retained role carries the knob:
        # the epoch-3 revocation of provider/A's identity is an
        # authorization-reducing global ABE rotation, so user/B and
        # provider/B must converge on epoch 3 too or the unaffected control
        # channel dies (an epoch-2 provider rejects epoch-3 user requests
        # and vice versa, observed as full-stream timeouts).  Timings allow
        # for the launcher's process-start skew: each role is forked behind
        # a startup marker, so SIGSTOP lands ~1.5 s after stopUserAMs on
        # user/A's own clock; the values below put the freeze between the
        # request-stream start (~3 s) and the grant propagation (~7 s), and
        # SIGCONT after the revocation has been published (~10 s).
        config.update({"recovery": "offline-rejoin",
                       "revokeIdentity": PROVIDER_ROOT + "/A",
                       "revokeAfterMs": 10000,
                       "grantAfterMs": 7000,
                       "grantIdentity": PROVIDER_ROOT + "/C",
                       "grantService": SERVICE_NAME,
                       "knobMs": {"userA": 800, "userB": 800,
                                  "providerA": 800, "providerB": 800},
                       "lifetimeMs": 35000,
                       "stopUserAMs": 3300,
                       "contUserAMs": 9500})
    elif scenario == "controller-cache-provider-status-retrieval":
        config.update({"recovery": "status-source"})
    elif scenario == "controller-unavailable-expiry":
        # Early revocation (1.8 s) followed by a SIGINTed controller; only
        # user/A is knobbed.  user/B and the providers keep their epoch-1
        # material (no knob, far from expiry), so they must keep serving
        # without the authority while user/A is denied locally.
        config.update({"recovery": "controller-unavailable",
                       "revokeAfterMs": 1800,
                       "knobMs": {"userA": 1000}})
    elif scenario == "large-response-invalidation":
        config.update({"mode": "large-response"})
    elif scenario == "targeted-refill-invalidation":
        config.update({"mode": "targeted", "recovery": "targeted-refill"})
    elif scenario == "stream-invalidation":
        config.update({"mode": "stream"})
    elif scenario == "hintless-scheduled-refresh":
        # Only user/A may learn the revocation: the knob exists on user/A
        # alone, and no hint source exists to propagate the new epoch.
        config.update({"recovery": "scheduled-refresh",
                       "knobMs": {"userA": 1000}})
    elif scenario == "controller-restart":
        config.update({"controllerRestart": True,
                       "recovery": "controller-restart",
                       "controller2RevokeAfterMs": 2000})
    elif scenario == "selection-response-tamper-and-replay":
        config.update({"recovery": "tamper-replay"})
    elif scenario in {"grant-only-advance", "grant-after-permission-exhaustion"}:
        # One ControllerVersion advance that grants user/B its first /HELLO
        # while the global ABE pair stays byte-identical.  user/B is absent
        # from the wave-1 policy variant, so its pre-advance requests are
        # deterministic publication denials ("new attribute denied before its
        # material installs") and its post-install requests succeed.  The
        # granted identity performs exactly one lazy DKEY fetch; unaffected
        # identities (user/A, provider/A, provider/B) perform none in the
        # advance wave.
        config.update({
            "revokeAfterMs": -1,
            "grantOnlyAdvance": True,
            "policyFile": str(POLICY_GRANT_ONLY),
            "grantAfterMs": 8000,
            "permissionRefetchAfterMs": 12000,
            "requestDurationMs": 24000,
            "lifetimeMs": 45000,
            "grantIdentity": USER_ROOT + "/B",
            "grantService": SERVICE_NAME,
            "recovery": "grant-only-advance",
            # Permission discovery is App-owned. Explicit renewal arms the
            # target-only DKEY refresh; constructor readiness is not a grant.
            # Four refresh opportunities per one-second request interval;
            # default-policy instantaneous convergence is not claimed.
            "knobMs": {"userA": 250, "userB": 250,
                       "providerA": 250, "providerB": 250},
        })
        if scenario == "grant-after-permission-exhaustion":
            config.update({
                "grantAfterMs": 30000,
                "permissionRefetchAfterMs": 40000,
                "initialPermissionTransportLoss": True,
                "lifetimeMs": 100000,
                "requestCount": 32,
                "requestDurationMsByUser": {"A": 60000, "B": 60000},
                "knobMs": {"userA": 1000, "userB": 1000,
                           "providerA": 1000, "providerB": 1000},
                "recovery": "explicit-permission-renewal",
            })
    return config


def preflight(build_dir: str | Path | None = None) -> Dict[str, Any]:
    resolved_build_dir = _resolve_build_dir(build_dir)
    required_files = [TOPOLOGY, POLICY, POLICY_GRANT_ONLY, TRUST_SCHEMA]
    missing_files = [str(path) for path in required_files if not path.is_file()]
    commands = {
        command: shutil.which(command) is not None
        for command in ("nfd", "nfdc", "infoconv")
    }
    try:
        import minindn  # noqa: F401
        minindn_error = ""
    except Exception as exc:  # pragma: no cover - host dependent
        minindn_error = f"{type(exc).__name__}: {exc}"

    binaries = {
        name: _find_binary(name, resolved_build_dir)
        for name in ("App_ServiceController", "App_User", "App_Provider")
    }
    # Do not infer revocation support from a binary merely existing.  The
    # Controller must provide a deterministic scheduled withdrawal and bounded
    # lifetime for a cross-process test to be meaningful.
    controller_source = ROOT / "examples/App_ServiceController.cpp"
    source_text = controller_source.read_text(encoding="utf-8") if controller_source.is_file() else ""
    runtime_controls = {
        "scheduled_controller_revocation": "--revoke-after-ms" in source_text,
        "bounded_controller_lifetime": "--run-for-ms" in source_text,
        "configurable_user_identity": "--user-identity" in (
            (ROOT / "examples/App_User.cpp").read_text(encoding="utf-8")
            if (ROOT / "examples/App_User.cpp").is_file() else ""
        ),
    }
    return {
        "schemaVersion": "spec179-minindn-preflight-v1",
        "buildDir": str(resolved_build_dir),
        "topology": str(TOPOLOGY),
        "nodes": _topology_nodes() if TOPOLOGY.is_file() else [],
        "requiredNodes": TOPOLOGY_NODES,
        "missingFiles": missing_files,
        "commands": commands,
        "minindnAvailable": not minindn_error,
        "minindnError": minindn_error,
        "runningAsRoot": os.geteuid() == 0,
        "roleBinaries": binaries,
        "runtimeControls": runtime_controls,
        "networkEvidence": False,
    }


def contract_report(scenario: str) -> Dict[str, Any]:
    return {
        "schema": "ndnsf-spec179-revocation-contract/v1",
        "mode": "dry-run",
        "status": "contract-only",
        "selectedScenario": scenario,
        "networkEvidence": False,
        "topology": {
            "nodes": _topology_nodes(),
            "controller": "/example/hello/controller",
            "users": ["/example/hello/user/A", "/example/hello/user/B"],
            "providers": ["/example/hello/provider/A", "/example/hello/provider/B"],
            "cache": "relay cache copy",
        },
        "scenarios": SCENARIOS,
        "matrix": {
            "revokedSubject": TARGETS,
            "cutPoint": CUT_POINTS,
            "mode": MODES,
            "recovery": RECOVERIES,
            "requiredControl": "same-version unaffected identity/service",
        },
        "requiredEvidence": [
            "statusSource",
            "controllerVersion",
            "refreshAttempts",
            "invalidatedCacheCount",
            "executionCount",
            "terminalOwner",
            "terminalReason",
            "redactedTraceHash",
        ],
        "claimBoundary": (
            "A dry-run validates the scenario and coverage contract only; it is "
            "not cross-process revocation evidence."
        ),
    }


def execute_gate(output: Path, scenario: str = SCENARIOS[0],
                 lifetime_ms: int = 15000,
                 build_dir: str | Path | None = None) -> Dict[str, Any]:
    # Scenario evaluators live in the sibling checks module; make it
    # importable no matter which cwd launches the campaign.
    import sys as _sys
    _checks_dir = Path(__file__).resolve().parent
    if str(_checks_dir) not in _sys.path:
        _sys.path.insert(0, str(_checks_dir))
    from spec179_scenario_checks import evaluate as scenario_evaluate
    report = preflight(build_dir)
    output.mkdir(parents=True, exist_ok=True)
    # Fresh durable authority state per run.  The generation store persists
    # revocations and a writer lease; a rerun into the same output directory
    # would reload the previous run's revocations and its scheduled
    # revocation would be rejected as a duplicate (revoke() -> false).
    for stale in ("controller-generation.state",
                  "controller-generation.state.lock"):
        stale_path = output / stale
        if stale_path.is_file():
            stale_path.unlink()
    (output / "preflight.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if scenario not in SCENARIOS:
        raise ValueError("unknown Spec179 scenario: %s" % scenario)
    if lifetime_ms <= 0:
        raise ValueError("lifetime_ms must be positive")
    if report["missingFiles"] or not report["minindnAvailable"]:
        report.update({
            "schema": "ndnsf-spec179-revocation-result/v1",
            "status": "preflight-failed",
            "reason": "MiniNDN dependency or tracked fixture is missing",
            "networkEvidence": False,
        })
        return report
    if not all(report["commands"].values()):
        report.update({
            "schema": "ndnsf-spec179-revocation-result/v1",
            "status": "preflight-failed",
            "reason": "nfd/nfdc/infoconv command dependency is missing",
            "networkEvidence": False,
        })
        return report
    if not report["runningAsRoot"]:
        report.update({
            "schema": "ndnsf-spec179-revocation-result/v1",
            "status": "requires-root",
            "reason": "MiniNDN/Mininet requires root; rerun with sudo",
            "networkEvidence": False,
        })
        return report
    if not all(report["roleBinaries"].values()):
        report.update({
            "schema": "ndnsf-spec179-revocation-result/v1",
            "status": "preflight-failed",
            "reason": "current C++ role binaries are not available",
            "networkEvidence": False,
        })
        return report
    if not all(report["runtimeControls"].values()):
        # This is intentionally a hard readiness result.  Launching the roles
        # without a real Controller withdrawal would only demonstrate traffic,
        # not the Spec179 revocation behavior.
        report.update({
            "schema": "ndnsf-spec179-revocation-result/v1",
            "status": "not-ready",
            "reason": (
                "the C++ role examples lack deterministic scheduled Controller "
                "revocation/lifetime and configurable identity controls"
            ),
            "networkEvidence": False,
        })
        return report
    config = _scenario_config(scenario)
    provenance = _build_provenance(report)
    # Scenario lifetimes that must drain delayed/in-flight traffic inside the
    # role window override the caller-provided default.
    if int(config.get("lifetimeMs", 0) or 0) > 0:
        lifetime_ms = int(config["lifetimeMs"])
    started = time.monotonic()
    processes: List[Tuple[str, Any, Any]] = []
    ndn = None
    launcher_error = ""
    route_path = output / "routes.json"
    manifest_path = output / "manifest.json"
    shared_keychain = output / "keys" / "shared"
    shared_keychain.mkdir(parents=True, exist_ok=True)
    (shared_keychain / "pib").mkdir(parents=True, exist_ok=True)
    (shared_keychain / "tpm").mkdir(parents=True, exist_ok=True)

    def q(value: Any) -> str:
        return shlex.quote(str(value))

    controller_binary = Path(report["roleBinaries"]["App_ServiceController"])
    user_binary = Path(report["roleBinaries"]["App_User"])
    provider_binary = Path(report["roleBinaries"]["App_Provider"])
    ensure_identities = ",".join([
        PROVIDER_ROOT + "/A", PROVIDER_ROOT + "/B",
        USER_ROOT + "/A", USER_ROOT + "/B",
    ])
    controller_state = output / "controller-generation.state"

    def controller_command(revoke: bool, run_for_ms: int,
                           revoke_after_ms: Optional[int] = None) -> str:
        args = [
            q(controller_binary),
            "--policy-file", q(str(config.get("policyFile", POLICY))),
            "--trust-schema", q(TRUST_SCHEMA),
            "--controller-prefix", q(CONTROLLER_PREFIX),
            "--ensure-identities", q(ensure_identities),
            "--run-for-ms", str(run_for_ms),
        ]
        # A scheduled additional grant is a distinct launcher surface: the
        # offline-rejoin scenario grants provider/C mid-run (epoch advance
        # without an ABE rotation) and grant-only-advance grants user/B.
        if int(config.get("grantAfterMs", -1)) >= 0:
            args += ["--grant-additional-after-ms",
                     str(int(config.get("grantAfterMs", 8000))),
                     "--grant-additional-identity",
                     q(config.get("grantIdentity", USER_ROOT + "/B")),
                     "--grant-additional-service",
                     q(str(config.get("grantService", SERVICE_NAME)))]
        if revoke:
            args += ["--revoke-after-ms",
                     str(revoke_after_ms if revoke_after_ms is not None
                         else int(config["revokeAfterMs"])),
                     "--revoke-kind", q(config["revokeKind"])]
            if config["revokeIdentity"]:
                args += ["--revoke-identity", q(config["revokeIdentity"])]
            if config["revokeService"]:
                args += ["--revoke-service", q(config["revokeService"])]
            if config["revokeAttribute"]:
                args += ["--revoke-attribute", q(config["revokeAttribute"])]
            if config["revokeCertificateDigest"]:
                args += ["--revoke-certificate-digest",
                         q(config["revokeCertificateDigest"])]
            if config.get("revokeRetryAfterMs"):
                args += ["--revoke-retry-after-ms", str(config["revokeRetryAfterMs"])]
        return "cd %s && exec %s" % (q(ROOT), " ".join(args))

    def provider_command(provider_id: str, provider_log: Path) -> str:
        provider_log.parent.mkdir(parents=True, exist_ok=True)
        args = [
            q(provider_binary), "--provider-id", q(provider_id),
            "--provider-root", q(PROVIDER_ROOT),
            "--group-prefix", q(GROUP_PREFIX),
            "--controller-prefix", q(CONTROLLER_PREFIX),
            "--trust-schema", q(TRUST_SCHEMA),
            "--run-for-ms", str(lifetime_ms + 3000),
            "--benchmark", "--timeline-trace", "--provider-lifecycle-csv",
            q(provider_log),
            "--performance-mode",
        ]
        delay = int(config.get("providerRequestDelayMs", 0))
        if delay > 0:
            args += ["--provider-request-delay-ms", str(delay)]
        if config["mode"] == "large-response":
            args += ["--response-payload", q("L" * LARGE_RESPONSE_BYTES)]
        if config["mode"] == "stream":
            args += ["--stream"]
        return "cd %s && exec %s" % (q(ROOT), " ".join(args))

    def user_command(user_id: str, user_log: Path) -> str:
        duration_ms = int(config.get("requestDurationMsByUser", {}).get(
            user_id, config.get("requestDurationMs", 8000)))
        args = [
            q(user_binary), "--user-identity", q(USER_ROOT + "/" + user_id),
            "--provider-root", q(PROVIDER_ROOT),
            "--known-provider-ids",
            "A" if config["mode"] == "targeted" else "A,B",
            "--group-prefix", q(GROUP_PREFIX),
            "--controller-prefix", q(CONTROLLER_PREFIX),
            "--trust-schema", q(TRUST_SCHEMA),
            "--benchmark", "--workload-mode", "open-loop",
            "--rate-rps", "1", "--count", str(int(config.get("requestCount", 8))),
            "--warmup", "0",
            "--interval-ms", "1000", "--duration",
            str(max(1, (duration_ms + 999) // 1000)),
            "--ack-timeout-ms", "1000", "--timeout-ms", "5000",
            "--request-timeout-ms", "5000", "--drain-seconds", "6",
            "--strategy", "first-responding", "--timeline-trace",
            "--disable-adaptive-admission-control", "--run-for-ms",
            str(lifetime_ms + 3000), "--output-csv", q(user_log),
        ]
        if config["mode"] == "targeted":
            args += ["--targeted", "--targeted-provider",
                     q(PROVIDER_ROOT + "/A")]
        if config["mode"] == "stream":
            args += ["--stream"]
        return "cd %s && exec %s" % (q(ROOT), " ".join(args))

    try:
        from minindn.minindn import Minindn

        saved_argv = sys.argv
        sys.argv = [saved_argv[0]]
        try:
            ndn = Minindn(
                topoFile=str(TOPOLOGY),
                workDir=str(_minindn_work_dir(output)),
            )
        finally:
            sys.argv = saved_argv
        ndn.start()
        nodes = list(ndn.net.hosts)
        names = {node.name for node in nodes}
        if set(TOPOLOGY_NODES) != names:
            raise RuntimeError("MiniNDN topology mismatch: expected=%s actual=%s" %
                               (sorted(TOPOLOGY_NODES), sorted(names)))
        _configure_nfd(ndn, nodes)
        routes = _configure_routes(ndn, nodes)
        route_path.write_text(json.dumps(routes, indent=2, sort_keys=True) + "\n",
                              encoding="utf-8")

        controller_run_ms = min(5000, lifetime_ms)
        controller = _launch_process(
            ndn, "controller", "controller-1", controller_command(
                not bool(config["controllerRestart"]) and
                not bool(config.get("grantOnlyAdvance")),
                controller_run_ms if config["controllerRestart"] else lifetime_ms),
            output, shared_keychain, processes,
            {"NDNSF_CONTROLLER_GENERATION_STATE": str(controller_state),
             **({"NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE": "once"}
                if config.get("rotationFaultOnce") else {})})
        _wait_marker(output / "controller-1.log", "ServiceController started...",
                     processes, 30)

        for provider_id, node_name in (("A", "provider-a"), ("B", "provider-b")):
            provider_env = {}
            knob = int((config.get("knobMs") or {}).get(
                "provider" + provider_id, 0))
            if knob > 0:
                # Fixed-period scheduled refresh: force the role to re-fetch
                # the signed status even though it is far from expiry, so the
                # revocation is actually discovered inside the run window.
                provider_env["NDNSF_POLICY_REVALIDATION_PERIOD_MS"] = str(knob)
            if config["mode"] == "targeted":
                # Two one-time pairs force a real fast-path consumption and a
                # bounded refill within the eight-request workload.
                provider_env["NDNSF_TARGETED_TOKEN_BATCH_SIZE"] = "2"
            _launch_process(
                ndn, node_name, "provider-%s" % provider_id,
                provider_command(provider_id,
                                 output / ("provider-%s" % provider_id) /
                                 "provider-lifecycle.csv"),
                output, shared_keychain, processes, provider_env)
            # The role examples share a campaign PIB/TPM.  Start each process
            # only after the previous one has completed its KeyChain
            # initialization; concurrent sqlite writers otherwise race before
            # App_Provider's initialization lock is acquired.
            _wait_marker(output / ("provider-%s.log" % provider_id),
                         "provider identity=", processes, 30)

        for user_id, node_name in (("A", "user-a"), ("B", "user-b")):
            user_dir = output / ("user-%s" % user_id)
            user_dir.mkdir(parents=True, exist_ok=True)
            user_env = {}
            if user_id == "B" and config.get("permissionRefetchAfterMs"):
                user_env.update({
                    "NDNSF_PERMISSION_FETCH_MAX_ATTEMPTS": "2",
                    "NDNSF_PERMISSION_FETCH_LIFETIME_MS": "500",
                    "NDNSF_PERMISSION_FETCH_RETRY_BACKOFF_MS": "0",
                    "NDNSF_PERMISSION_REFETCH_AFTER_MS": str(config["permissionRefetchAfterMs"]),
                })
            knob = int((config.get("knobMs") or {}).get("user" + user_id, 0))
            if knob > 0:
                user_env["NDNSF_POLICY_REVALIDATION_PERIOD_MS"] = str(knob)
            if config["mode"] == "targeted":
                user_env["NDNSF_TARGETED_TOKEN_BATCH_SIZE"] = "2"
            loss_node = (next(node for node in nodes if node.name == node_name)
                         if user_id == "B" and config.get("initialPermissionTransportLoss")
                         else None)
            loss_events = []
            loss_path = output / "permission-startup-loss.json"
            if loss_node is not None:
                loss_events.append(_set_startup_permission_loss(loss_node, True))
                loss_path.write_text(json.dumps(loss_events, indent=2) + "\n", encoding="utf-8")
            try:
                _launch_process(
                    ndn, node_name, "user-%s" % user_id,
                    user_command(user_id, user_dir / "request-results.csv"),
                    output, shared_keychain, processes, user_env)
                # Serialize campaign PIB initialization through App readiness.
                _wait_marker(output / ("user-%s.log" % user_id),
                             "[App_User] token_mode=", processes, 30)
                if loss_node is not None:
                    _wait_marker(output / "user-B.log", "final=1", processes, 10)
            finally:
                if loss_node is not None:
                    loss_events.append(_set_startup_permission_loss(loss_node, False))
                    loss_path.write_text(json.dumps(loss_events, indent=2) + "\n", encoding="utf-8")

        if config["controllerRestart"]:
            deadline = time.monotonic() + (controller_run_ms / 1000.0) + 15
            while controller.poll() is None and time.monotonic() < deadline:
                time.sleep(0.1)
            if controller.poll() is None:
                raise RuntimeError("controller-1 did not finish bounded restart window")
            # Controller-2 resumes under a *new* generation (epoch resets to
            # 1) and performs its own revocation at a short bounded offset.
            c2_revoke_ms = int(config.get("controller2RevokeAfterMs") or
                               config["revokeAfterMs"])
            _launch_process(
                ndn, "controller", "controller-2",
                controller_command(True, lifetime_ms, revoke_after_ms=c2_revoke_ms),
                output, shared_keychain, processes,
                {"NDNSF_CONTROLLER_GENERATION_STATE": str(controller_state)})
            _wait_marker(output / "controller-2.log", "ServiceController started...",
                         processes, 30)

        # Controller-unavailable is a distinct recovery condition.  Stop the
        # authority only after the initial material has been fetched; any later
        # refresh must therefore fail closed instead of silently inventing a
        # newer local version.
        if config["recovery"] == "controller-unavailable":
            time.sleep(4.0)
            for name, proc, _log in processes:
                if name.startswith("controller-") and proc.poll() is None:
                    proc.send_signal(signal.SIGINT)

        # Provider restart (S2): after the revoked provider has discovered
        # epoch 2 inside its scheduled-refresh window, stop it and relaunch
        # the same identity so a *fresh* bootstrap happens under the current
        # epoch -- the restarted process must not resurrect the authority.
        if config["recovery"] == "provider-restart":
            time.sleep(float(config.get("restartProviderAfterMs", 8500)) / 1000.0)
            for name, proc, _log in processes:
                if name == "provider-A" and proc.poll() is None:
                    proc.send_signal(signal.SIGINT)
                    try:
                        proc.wait(timeout=10)
                    except Exception:
                        proc.kill()
            restart_env = {}
            knob = int((config.get("knobMs") or {}).get("providerA", 0))
            if knob > 0:
                restart_env["NDNSF_POLICY_REVALIDATION_PERIOD_MS"] = str(knob)
            _launch_process(
                ndn, "provider-a", "provider-A-2",
                provider_command("A", output / "provider-A" /
                                 "provider-lifecycle.csv"),
                output, shared_keychain, processes, restart_env)
            _wait_marker(output / "provider-A-2.log", "provider identity=",
                         processes, 30)

        # Offline rejoin (S5): freeze user/A with SIGSTOP across the grant
        # (epoch 2) and the revocation (epoch 3), then SIGCONT it -- its
        # resumed scheduled refresh must jump straight to epoch 3 and its
        # install history must never contain epoch 2.
        if config["recovery"] == "offline-rejoin":
            stop_at = float(config.get("stopUserAMs", 4500)) / 1000.0
            cont_at = float(config.get("contUserAMs", 8500)) / 1000.0
            time.sleep(stop_at)
            user_a = next((proc for name, proc, _log in processes
                           if name == "user-A"), None)
            if user_a is not None and user_a.poll() is None:
                user_a.send_signal(signal.SIGSTOP)
            time.sleep(max(0.0, cont_at - stop_at))
            try:
                # Fast asynchronous startup removes the old implicit launch
                # skew. Resume only after the real epoch-3 mutation, not a
                # wall-clock estimate that can still expose epoch 2.
                _wait_marker(output / "controller-1.log", "NDNSF_CONTROLLER_REVOKED",
                             processes, 20)
            finally:
                if user_a is not None and user_a.poll() is None:
                    user_a.send_signal(signal.SIGCONT)

        # Let the measured user window drain.  The role processes also have a
        # bounded lifetime, so this wait cannot create an unbounded campaign.
        time.sleep(max(1.0, (lifetime_ms + 3500) / 1000.0))
    except Exception as exc:
        launcher_error = "%s: %s" % (type(exc).__name__, exc)
        (output / "launcher-error.json").write_text(
            json.dumps({"error": launcher_error}, indent=2) + "\n",
            encoding="utf-8")
    finally:
        for _name, proc, log in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except Exception:
                    proc.kill()
            log.close()
        if ndn is not None:
            try:
                ndn.stop()
            finally:
                type(ndn).cleanUp()

    manifest_path.write_text(json.dumps({
        "schemaVersion": "ndnsf-spec179-minindn-manifest-v1",
        "scenario": scenario,
        "config": config,
        "topology": str(TOPOLOGY),
        "policy": str(config.get("policyFile", POLICY)),
        "trustSchema": str(TRUST_SCHEMA),
        "buildDir": report["buildDir"],
        "provenance": provenance,
        "minindnWorkDir": str(_minindn_work_dir(output)),
        "commands": [
            "App_ServiceController", "App_Provider", "App_User",
        ],
        "claimBoundary": (
            "mode-specific network probe; recovery assertions remain separate"
        ),
        "largeResponseBytes": LARGE_RESPONSE_BYTES if config["mode"] == "large-response" else None,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    evidence = _collect_runtime_evidence(output, scenario, config, processes, started)
    process_ok = all(code in (0, -signal.SIGINT, None)
                     for code in evidence["processReturnCodes"].values())
    evidence["status"] = "completed" if not launcher_error and process_ok else "launcher-failed"
    evidence["scenario"] = scenario
    evidence["launcherError"] = launcher_error or None
    evidence["routes"] = str(route_path) if route_path.is_file() else None
    evidence["manifest"] = str(manifest_path)
    # A network run is evidence of a real packet campaign, but it is a passing
    # release gate only when the requested mode/recovery and the revocation
    # or grant-only assertions are observed.  Do not promote a normal probe
    # to all scenarios.
    grant_only_gate = False
    if config.get("grantOnlyAdvance"):
        grant = evidence.get("grantOnly", {})
        grant_only_gate = bool(
            grant.get("grantApplied") is True and
            grant.get("grantAbeUnchanged") is True and
            grant.get("grantTimeUs") is not None and
            grant.get("grantedEpochGE2Fetches") == 1 and
            grant.get("unaffectedEpochGE2Fetches") == 0 and
            sum(grant.get("unaffectedEpoch2Observed", {}).values()) >= 1 and
            grant.get("grantedRows", 0) >= 1 and
            grant.get("grantedRows") == grant.get("grantedSuccessRows") and
            grant.get("grantedPreGrantRows") == 0 and
            grant.get("grantedPreResolveWaiting", 0) >= 1 and
            grant.get("unaffectedControlSuccessRows", 0) >= 1 and
            grant.get("unaffectedControlFailures") == 0 and
            grant.get("unaffectedControlPostGrantSuccessRows", 0) >= 1 and
            (not config.get("permissionRefetchAfterMs") or
             (grant.get("explicitRenewalObserved") is True and
              grant.get("unaffectedControlPostRefetchSuccessRows", 0) >= 1)) and
            (not config.get("initialPermissionTransportLoss") or
             grant.get("lateRenewalObserved") is True))

    # Spec179 per-scenario checks (sibling module) are the authority for the
    # revocation family.  grant-only-advance has no scenario evaluator and
    # keeps its dedicated grant_only_gate above.
    scenario_result = scenario_evaluate(scenario, output, config)
    evidence["scenarioChecks"] = (scenario_result or {}).get("checks")
    evidence["scenarioDetails"] = (scenario_result or {}).get("details")
    evidence["scenarioEvidence"] = (scenario_result or {}).get("evidence")
    evidence["scenarioReason"] = (scenario_result or {}).get("reason")
    scenario_passed = bool(scenario_result and scenario_result.get("passed"))

    # Streamed runs resolve their terminal through stream markers and keep
    # no request-results CSV, so the row/terminal clauses are exempted for
    # stream mode.  Every launch must still complete, cross the wire
    # (networkEvidence), and execute at least one request/stream.
    stream_mode = config["mode"] == "stream"
    rows_exist = any(_read_csv_rows(path)
                     for path in output.glob("user-*/request-results.csv"))
    common_gate = (
        evidence["status"] == "completed" and
        evidence["networkEvidence"] and
        (evidence["revocationApplied"] or grant_only_gate) and
        evidence["executionCount"] > 0 and
        evidence["requestPublicationCount"] > 0 and
        (bool(evidence["terminalOwner"]) or stream_mode) and
        (rows_exist or stream_mode)
    )
    evidence["grantOnlyGateOk"] = grant_only_gate

    # Per-mode network counters prove the mode's traffic crossed the wire
    # inside the window; the scenario module owns the mode semantics.
    if config["mode"] == "normal":
        mode_gate = True
    elif config["mode"] == "large-response":
        # Segmented large responses resolve on the segmented path without a
        # provider-side plaintext decryption observable, so resolution --
        # not decryption -- is the terminal that proves the mode crossed
        # the wire.
        mode_gate = (
            evidence["largeResponsePublishedCount"] > 0 and
            evidence["largeResponseResolvedCount"] > 0
        )
    elif config["mode"] == "targeted":
        # spec179 request-scoped confidentiality routes every Targeted call
        # through the bounded bootstrap: the request carries a recipient-bound
        # key envelope, so no request can ride the token-only fast path in
        # this runtime (RequestServiceTargeted attaches a fresh token batch to
        # each bootstrap response for later non-request-scoped runtimes, but
        # never consumes a cached token on the scoped path).  The token-mode
        # counters (fast-path acceptance, token refill) therefore cannot fire
        # here; require the token-mode proof when a non-scoped runtime
        # produced it, or the request-scoped bootstrap proof otherwise --
        # repeated batch storage proves the refill cycle kept re-bootstrapping
        # across requests.
        token_mode_proof = (
            evidence["targetedAcceptedCount"] > 0 and
            evidence["targetedFastPathCount"] > 0 and
            evidence["targetedRefillCount"] > 0)
        scoped_mode_proof = (
            evidence["targetedBootstrapCount"] > 0 and
            evidence["targetedBatchStoredCount"] >= 2)
        mode_gate = (
            evidence["targetedRequestCount"] > 0 and
            (token_mode_proof or scoped_mode_proof))
    else:
        mode_gate = (
            evidence["streamEventPublishedCount"] > 0 and
            evidence["streamEventReceivedCount"] > 0 and
            (evidence["streamCompleteCount"] > 0 or
             evidence["streamErrorCount"] > 0)
        )
    if config.get("grantOnlyAdvance"):
        evidence["gatePassed"] = bool(grant_only_gate and common_gate and mode_gate)
    else:
        evidence["gatePassed"] = bool(scenario_passed and common_gate and mode_gate)
    result_path = output / "result.json"
    result_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=SCENARIOS, default=SCENARIOS[0])
    parser.add_argument("--output", default="")
    parser.add_argument("--execute", action="store_true",
                        help="run the explicit MiniNDN gate (requires root)")
    parser.add_argument("--lifetime-ms", type=int, default=15000,
                        help="bounded role lifetime for an explicit run")
    parser.add_argument(
        "--build-dir", default=str(DEFAULT_BUILD_DIR),
        help=("exact C++ build tree for all role binaries; no stale-build "
              "fallback is permitted"),
    )
    args = parser.parse_args()

    if args.execute:
        result = execute_gate(
            Path(args.output or (ROOT / "results/spec179-minindn")),
            args.scenario, args.lifetime_ms, args.build_dir)
        print(json.dumps(result, indent=2, sort_keys=True))
        if result["status"] in {"requires-root", "preflight-failed", "not-ready"}:
            return 3
        return 0 if result["status"] == "completed" and result.get("gatePassed") is True else 4

    result = contract_report(args.scenario)
    if args.output:
        path = Path(args.output)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
