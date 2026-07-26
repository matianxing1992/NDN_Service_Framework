#!/usr/bin/env python3
"""Spec 136: two-node bidirectional RSA PubSub runner."""

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
from typing import Any, Callable


REPO = Path(__file__).resolve().parents[1]
PEERS = ("peer-a", "peer-b")
RATES = (200, 250, 300, 350, 400)
SYNC_BATCH_WINDOW_MS = 5
PUBLICATION_FETCH_WINDOW = 64
MAX_SIGNER_UTILIZATION = 0.90
MIN_DELIVERY_RATIO = 0.98
FORMAL_MATRIX = (
    ("face-inline-rsa", 200),
    ("worker-rsa", 200),
    ("worker-rsa", 250),
    ("face-inline-rsa", 250),
    ("face-inline-rsa", 300),
    ("worker-rsa", 300),
    ("worker-rsa", 350),
    ("face-inline-rsa", 350),
    ("face-inline-rsa", 400),
    ("worker-rsa", 400),
)
TOPOLOGY = (
    "[nodes]\npeer-a:\npeer-b:\n\n[links]\n"
    "peer-a:peer-b delay=10ms bw=100 loss=0\n"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def restore_invoking_user_ownership(path: Path) -> None:
    uid = os.environ.get("SUDO_UID")
    gid = os.environ.get("SUDO_GID")
    if uid is None or gid is None:
        return
    owner = (int(uid), int(gid))
    for child in path.rglob("*"):
        os.chown(child, *owner)
    os.chown(path, *owner)


def host_command(host: Any, command: str) -> tuple[int, str]:
    marker = "__SPEC136_RC__="
    text = host.cmd(
        f"{command}; spec136_rc=$?; printf '\\n{marker}%s\\n' \"$spec136_rc\""
    )
    match = re.search(rf"\n{marker}(\d+)\s*$", text)
    if match is None:
        raise RuntimeError(f"missing return code: {command}")
    return int(match.group(1)), text[: match.start()].strip()


def stop_process(process: Any, grace: float = 3.0) -> int | None:
    if process is None:
        return None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=grace)
    return process.returncode


def clear_stale_sockets() -> None:
    active = Path("/proc/net/unix").read_text(encoding="utf-8", errors="replace")
    for peer in PEERS:
        socket = Path(f"/run/nfd/{peer}.sock")
        if not socket.exists():
            continue
        if str(socket) in active:
            raise RuntimeError(f"active MiniNDN socket exists: {socket}")
        socket.unlink()


def wait_file(path: Path, text: str, process: Any, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and text in path.read_text(
            encoding="utf-8", errors="replace"
        ):
            return
        if process.poll() is not None:
            raise RuntimeError(
                f"process exited before ready rc={process.returncode}: {path}"
            )
        time.sleep(0.1)
    raise RuntimeError(f"ready timeout: {path}")


def validate_peer_admission(
    summary: dict[str, Any],
    mode: str,
    rate: int,
    measure: int,
    peer: str,
    expected_schema: str = "spec136.peer-summary.v6",
) -> list[str]:
    errors = []
    scheduled = rate * measure
    attempted = int(summary.get("attemptedMeasured", 0))
    attempted_ratio = attempted / scheduled if scheduled else 0.0
    delivered = int(summary.get("deliveredMeasured", 0))
    delivery_ratio = delivered / attempted if attempted else 0.0
    if summary.get("schema") != expected_schema:
        errors.append(f"{peer}:unexpected summary schema")
    if summary.get("scheduledMeasured") != scheduled:
        errors.append(
            f"{peer}:scheduled={summary.get('scheduledMeasured')} expected={scheduled}"
        )
    if not 0.98 <= attempted_ratio <= 1.02:
        errors.append(
            f"{peer}:attempted-ratio={attempted_ratio:.6f} outside [0.98,1.02]"
        )
    if delivery_ratio < MIN_DELIVERY_RATIO:
        errors.append(
            f"LOAD_UNSUSTAINED:{peer}:delivery-ratio={delivery_ratio:.6f} below "
            f"{MIN_DELIVERY_RATIO:.2f}"
        )
    face_thread = int(summary.get("faceThreadHash", 0))
    pacer_thread = int(summary.get("pacerThreadHash", 0))
    call_thread = int(summary.get("publishCallThreadHash", 0))
    if face_thread == 0 or pacer_thread == 0 or face_thread == pacer_thread:
        errors.append(f"{peer}:Face/APP pacer thread identity invalid")
    if summary.get("pacerFailed") is not False:
        errors.append(f"{peer}:pacer failed: {summary.get('pacerError', '')}")
    if summary.get("syncInterestBatching") is not True:
        errors.append(f"{peer}:Sync batching is not enabled")
    if summary.get("syncInterestBatchWindowMs") != SYNC_BATCH_WINDOW_MS:
        errors.append(f"{peer}:unexpected Sync batch window")
    if summary.get("publicationFetchWindow") != PUBLICATION_FETCH_WINDOW:
        errors.append(
            f"{peer}:publicationFetchWindow="
            f"{summary.get('publicationFetchWindow')} expected "
            f"{PUBLICATION_FETCH_WINDOW}"
        )
    if summary.get("selfDeliveries", 0) != 0:
        errors.append(
            f"{peer}:received {summary.get('selfDeliveries')} self publications"
        )
    if summary.get("dataSignatureType") != 1:
        errors.append(f"{peer}:publication Data is not SignatureSha256WithRsa")
    if summary.get("interestSignatureType") != 1:
        errors.append(f"{peer}:Sync Interest is not SignatureSha256WithRsa")
    if int(summary.get("dataValid", 0)) <= 0:
        errors.append(f"{peer}:peer Data validation evidence is missing")
    if int(summary.get("interestValid", 0)) <= 0:
        errors.append(f"{peer}:peer Interest validation evidence is missing")
    if (
        int(summary.get("dataInvalid", 0)) != 0
        or int(summary.get("interestInvalid", 0)) != 0
        or int(summary.get("invalid", 0)) != 0
    ):
        errors.append(f"{peer}:invalid object reached the measured path")
    if int(summary.get("maxActiveSigners", 0)) != 1:
        errors.append(f"{peer}:serialized signer ownership was not proved")
    data_calls = int(summary.get("dataSignCalls", 0))
    interest_calls = int(summary.get("interestSignCalls", 0))
    data_service = int(summary.get("dataSignServiceNs", 0))
    interest_service = int(summary.get("interestSignServiceNs", 0))
    if data_calls <= 0 or interest_calls <= 0:
        errors.append(f"{peer}:signer service sample is missing")
    else:
        data_mean = data_service / data_calls
        interest_mean = interest_service / interest_calls
        estimated_utilization = (
            2 * rate * data_mean
            + min(rate, 1000 / SYNC_BATCH_WINDOW_MS) * interest_mean
        ) / 1_000_000_000
        if estimated_utilization > MAX_SIGNER_UTILIZATION:
            errors.append(
                f"{peer}:estimated-signer-utilization={estimated_utilization:.6f} "
                f"exceeds {MAX_SIGNER_UTILIZATION:.2f}"
            )
    if summary.get("workerOutstanding", 0) != 0:
        errors.append(f"{peer}:uncommitted work remains at drain end")
    if summary.get("faceDispatchAbandoned", 0) != 0:
        errors.append(f"{peer}:Face publication calls were abandoned")
    if mode == "face-inline-rsa":
        if call_thread != face_thread or summary.get("publishCallsOnFace", 0) <= 0:
            errors.append(f"{peer}:control call was not executed on Face")
        if summary.get("publishCallsOnPacer", 0) != 0:
            errors.append(f"{peer}:control call executed on APP pacer")
    else:
        if call_thread != pacer_thread or summary.get("publishCallsOnPacer", 0) <= 0:
            errors.append(f"{peer}:worker call was not executed on APP pacer")
        if summary.get("publishCallsOnFace", 0) != 0:
            errors.append(f"{peer}:worker call executed on Face")
    return errors


def install_routes(
    host: Any,
    transport: str,
    remote_ip: str,
    sync_prefix: str,
    remote_node_prefix: str,
) -> dict[str, Any]:
    env = f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)}"
    rc, output = host_command(
        host,
        f"{env} nfdc face create udp4://{remote_ip}:6363 persistency persistent",
    )
    match = re.search(r"face-created id=(\d+)", output)
    if rc != 0 or match is None:
        raise RuntimeError(f"face creation failed rc={rc}: {output}")
    face_id = int(match.group(1))
    records = []
    for prefix in (sync_prefix, remote_node_prefix):
        route_rc, route_output = host_command(
            host,
            f"{env} nfdc route add {shlex.quote(prefix)} nexthop {face_id}",
        )
        if route_rc != 0:
            raise RuntimeError(f"route failed {prefix}: {route_output}")
        records.append({"prefix": prefix, "output": route_output})
    strategy_rc, strategy_output = host_command(
        host,
        f"{env} nfdc strategy set {shlex.quote(sync_prefix)} "
        "/localhost/nfd/strategy/multicast",
    )
    if strategy_rc != 0:
        raise RuntimeError(f"strategy failed: {strategy_output}")
    return {"faceId": face_id, "routes": records}


def create_identity(
    host: Any, home: Path, identity: str, certificate: Path
) -> None:
    home.mkdir(parents=True, exist_ok=False)
    env = (
        f"HOME={shlex.quote(str(home))} "
        "NDN_CLIENT_PIB=pib-sqlite3 NDN_CLIENT_TPM=tpm-file"
    )
    rc, output = host_command(
        host,
        f"{env} ndnsec key-gen -t r -i {shlex.quote(identity)} "
        f">{shlex.quote(str(certificate))}",
    )
    if rc != 0 or not certificate.is_file() or certificate.stat().st_size == 0:
        raise RuntimeError(f"RSA identity creation failed: {output}")


def local_peer_env(home: Path, library_dir: Path) -> dict[str, str]:
    env = os.environ.copy()
    env.update(
        {
            "HOME": str(home),
            "NDN_CLIENT_PIB": "pib-sqlite3",
            "NDN_CLIENT_TPM": "tpm-file",
            "LD_LIBRARY_PATH": str(library_dir),
            "NDN_LOG": "*=WARN",
        }
    )
    return env


def create_local_identity(
    home: Path, identity: str, certificate: Path, library_dir: Path
) -> None:
    home.mkdir(parents=True, exist_ok=False)
    with certificate.open("wb") as output:
        completed = subprocess.run(
            ["ndnsec", "key-gen", "-t", "r", "-i", identity],
            env=local_peer_env(home, library_dir),
            stdout=output,
            stderr=subprocess.PIPE,
            check=False,
        )
    if (
        completed.returncode != 0
        or not certificate.is_file()
        or certificate.stat().st_size == 0
    ):
        raise RuntimeError(
            "local RSA identity creation failed: "
            + completed.stderr.decode("utf-8", errors="replace")
        )


def run_local_pair(
    directory: Path,
    commands: dict[str, list[str]],
    environments: dict[str, dict[str, str]],
    timeout: float,
) -> dict[str, int]:
    directory.mkdir(parents=True, exist_ok=False)
    write_json(directory / "commands.json", commands)
    processes: dict[str, subprocess.Popen[bytes]] = {}
    streams: dict[str, tuple[Any, Any]] = {}
    try:
        for peer in PEERS:
            stdout = (directory / f"{peer}.stdout").open("wb")
            stderr = (directory / f"{peer}.stderr").open("wb")
            streams[peer] = (stdout, stderr)
            processes[peer] = subprocess.Popen(
                commands[peer],
                cwd=REPO,
                env=environments[peer],
                stdout=stdout,
                stderr=stderr,
            )
        deadline = time.monotonic() + timeout
        for peer in PEERS:
            remaining = max(0.1, deadline - time.monotonic())
            processes[peer].wait(timeout=remaining)
    except subprocess.TimeoutExpired as exc:
        for process in processes.values():
            stop_process(process)
        raise RuntimeError(f"local pair watchdog expired: {directory}") from exc
    finally:
        for stdout, stderr in streams.values():
            stdout.close()
            stderr.close()
    return {peer: int(processes[peer].returncode) for peer in PEERS}


def validate_security_summary(summary: dict[str, Any], peer: str) -> list[str]:
    errors = []
    if summary.get("schema") != "spec136.security-preflight.v1":
        errors.append(f"{peer}:unexpected security summary schema")
    if summary.get("passed") is not True:
        errors.append(f"{peer}:security preflight did not pass")
    if summary.get("dataSignatureType") != 1:
        errors.append(f"{peer}:Data is not SignatureSha256WithRsa")
    if summary.get("interestSignatureType") != 1:
        errors.append(f"{peer}:Interest is not SignatureSha256WithRsa")
    for key in (
        "validDataAccepted",
        "validInterestAccepted",
        "tamperedDataRejected",
        "tamperedInterestRejected",
    ):
        if summary.get(key) is not True:
            errors.append(f"{peer}:{key}=false")
    for key in (
        "tamperedDataReachedProcessing",
        "tamperedInterestReachedProcessing",
    ):
        if summary.get(key) is not False:
            errors.append(f"{peer}:{key}=true")
    return errors


def validate_noop_pacer_summary(
    summary: dict[str, Any], mode: str, peer: str
) -> list[str]:
    errors = []
    expected = 1000 * 60
    attempted = int(summary.get("attemptedMeasured", 0))
    face_thread = int(summary.get("faceThreadHash", 0))
    pacer_thread = int(summary.get("pacerThreadHash", 0))
    call_thread = int(summary.get("publishCallThreadHash", 0))
    if summary.get("schema") != "spec136.noop-pacer.v1":
        errors.append(f"{peer}:{mode}:unexpected no-op schema")
    if summary.get("passed") is not True:
        errors.append(f"{peer}:{mode}:no-op pacer did not pass")
    if summary.get("scheduledMeasured") != expected:
        errors.append(f"{peer}:{mode}:scheduled count mismatch")
    if not 0.98 <= attempted / expected <= 1.02:
        errors.append(f"{peer}:{mode}:attempted rate outside +/-2%")
    if face_thread == 0 or pacer_thread == 0 or face_thread == pacer_thread:
        errors.append(f"{peer}:{mode}:Face/APP thread evidence invalid")
    if mode == "face-inline-rsa":
        if call_thread != face_thread or summary.get("publishCallsOnFace") != expected:
            errors.append(f"{peer}:{mode}:control caller path invalid")
        if summary.get("publishCallsOnPacer") != 0:
            errors.append(f"{peer}:{mode}:control executed on pacer")
    else:
        if call_thread != pacer_thread or summary.get("publishCallsOnPacer") != expected:
            errors.append(f"{peer}:{mode}:treatment caller path invalid")
        if summary.get("publishCallsOnFace") != 0:
            errors.append(f"{peer}:{mode}:treatment executed on Face")
    for key in ("rsaSignCalls", "ndnPublications", "syncInterests", "fetches"):
        if summary.get(key) != 0:
            errors.append(f"{peer}:{mode}:{key} is not zero")
    return errors


def run_cell(
    campaign: Path,
    binary: Path,
    library_dir: Path,
    ordinal: int,
    mode: str,
    rate: int,
    timing: tuple[int, int, int],
    *,
    experiment_namespace: str = "spec136",
    summary_schema: str = "spec136.peer-summary.v6",
    record_delivery_samples: bool = False,
    extra_peer_arguments: tuple[str, ...] = (),
    admission_validator: Callable[
        [dict[str, Any], str, int, int, str, str], list[str]
    ] | None = None,
    terminal_schema: str | None = None,
    ndn_log: str = "*=WARN",
    terminal_classifier: Callable[
        ..., dict[str, object]
    ] | None = None,
) -> dict[str, Any]:
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn

    warmup, measure, drain = timing
    cell_id = f"{ordinal:02d}-{mode}-{rate}"
    cell = campaign / cell_id
    cell.mkdir(parents=True, exist_ok=False)
    topology = cell / "topology.conf"
    topology.write_text(TOPOLOGY, encoding="utf-8")
    sync_prefix = f"/{experiment_namespace}/sync/{cell_id}"
    node_prefixes = {
        peer: f"/{experiment_namespace}/{peer}/{cell_id}" for peer in PEERS
    }
    identities = {
        peer: f"/{experiment_namespace}/identity/{peer}" for peer in PEERS
    }
    homes = {peer: cell / f"{peer}-home" for peer in PEERS}
    certificates = {peer: cell / f"{peer}.cert" for peer in PEERS}
    transports = {peer: f"unix:///run/nfd/{peer}.sock" for peer in PEERS}
    processes: dict[str, Any] = {}
    ndn = None
    nfd_manager = None
    error = ""
    admission_errors: list[str] = []
    try:
        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        clear_stale_sockets()
        ndn = Minindn(topoFile=str(topology), workDir=str(cell / "minindn"))
        ndn.start()
        nfd_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        hosts = {peer: ndn.net[peer] for peer in PEERS}
        for peer in PEERS:
            socket = Path(f"/run/nfd/{peer}.sock")
            deadline = time.monotonic() + 15
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                raise RuntimeError(f"NFD socket missing: {socket}")
        for peer in PEERS:
            other = PEERS[1] if peer == PEERS[0] else PEERS[0]
            route = install_routes(
                hosts[peer],
                transports[peer],
                hosts[other].IP(),
                sync_prefix,
                node_prefixes[other],
            )
            write_json(cell / f"{peer}-routes.json", route)
            create_identity(
                hosts[peer], homes[peer], identities[peer], certificates[peer]
            )

        commands = {}
        for peer in PEERS:
            other = PEERS[1] if peer == PEERS[0] else PEERS[0]
            stdout = cell / f"{peer}.stdout"
            stderr = cell / f"{peer}.stderr"
            summary = cell / f"{peer}-summary.json"
            delivery_samples = cell / f"{peer}-delivery-latency.csv"
            sample_argument = (
                f"--delivery-samples {shlex.quote(str(delivery_samples))} "
                if record_delivery_samples
                else ""
            )
            extra_arguments = " ".join(
                shlex.quote(argument) for argument in extra_peer_arguments
            )
            if extra_arguments:
                extra_arguments += " "
            command = (
                f"env HOME={shlex.quote(str(homes[peer]))} "
                "NDN_CLIENT_PIB=pib-sqlite3 NDN_CLIENT_TPM=tpm-file "
                f"NDN_CLIENT_TRANSPORT={shlex.quote(transports[peer])} "
                f"LD_LIBRARY_PATH={shlex.quote(str(library_dir))} "
                f"NDN_LOG={shlex.quote(ndn_log)} "
                f"{shlex.quote(str(binary))} "
                f"--mode {mode} --sync-prefix {shlex.quote(sync_prefix)} "
                f"--node-prefix {shlex.quote(node_prefixes[peer])} "
                f"--peer-id {peer} --remote-peer-id {other} "
                f"--identity {shlex.quote(identities[peer])} "
                f"--peer-certificate {shlex.quote(str(certificates[other]))} "
                f"--rate {rate} --warmup {warmup} --measure {measure} "
                f"--drain {drain} --summary {shlex.quote(str(summary))} "
                f"--summary-schema {shlex.quote(summary_schema)} "
                f"{extra_arguments}"
                f"{sample_argument}"
                f">{shlex.quote(str(stdout))} 2>{shlex.quote(str(stderr))}"
            )
            commands[peer] = command
        write_json(cell / "commands.json", commands)

        for peer in PEERS:
            processes[peer] = hosts[peer].popen(commands[peer], shell=True)
        for peer in PEERS:
            wait_file(
                cell / f"{peer}.stdout", "SPEC136_READY", processes[peer]
            )

        deadline = time.monotonic() + warmup + measure + drain + 30
        while (
            time.monotonic() < deadline
            and any(process.poll() is None for process in processes.values())
        ):
            time.sleep(0.2)
        if any(process.poll() is None for process in processes.values()):
            raise RuntimeError("cell watchdog expired")
        if any(process.returncode != 0 for process in processes.values()):
            raise RuntimeError(
                f"peer return codes: "
                f"{[(peer, process.returncode) for peer, process in processes.items()]}"
            )
        for peer in PEERS:
            summary = cell / f"{peer}-summary.json"
            if not summary.is_file():
                raise RuntimeError(f"summary missing: {summary}")
            admission_errors.extend(
                (admission_validator or validate_peer_admission)(
                    json.loads(summary.read_text(encoding="utf-8")),
                    mode,
                    rate,
                    measure,
                    peer,
                    summary_schema,
                )
            )
            if record_delivery_samples:
                delivery_samples = cell / f"{peer}-delivery-latency.csv"
                if not delivery_samples.is_file():
                    admission_errors.append(
                        f"{peer}:delivery latency samples missing"
                    )
    except Exception as exc:
        error = f"{type(exc).__name__}: {exc}"
    finally:
        return_codes = {
            peer: stop_process(process) for peer, process in processes.items()
        }
        if ndn is not None:
            try:
                ndn.stop()
            except Exception as exc:
                error = error or f"ndn.stop failed: {exc}"
        try:
            Minindn.cleanUp()
        except Exception as exc:
            error = error or f"MiniNDN cleanup failed: {exc}"
        try:
            clear_stale_sockets()
        except Exception as exc:
            error = error or f"named socket cleanup failed: {exc}"
        nfd_manager = None
        sys.argv = original_argv

    load_errors = [
        item for item in admission_errors if item.startswith("LOAD_UNSUSTAINED:")
    ]
    profile_errors = [
        item for item in admission_errors if item.startswith("PROFILE_INVALID:")
    ]
    harness_errors = [
        item
        for item in admission_errors
        if not item.startswith(("LOAD_UNSUSTAINED:", "PROFILE_INVALID:"))
    ]
    classification = (
        terminal_classifier(error=error, admission_errors=admission_errors)
        if terminal_classifier is not None
        else {}
    )
    if terminal_classifier is None:
        status = (
            "PROCESS_FAILED"
            if error
            else "HARNESS_INVALID"
            if harness_errors or profile_errors
            else "LOAD_UNSUSTAINED"
            if load_errors
            else "COMPLETE"
        )
    else:
        status = str(
            classification.get("validity")
            if classification.get("validity") != "PROFILE_VALID"
            else classification.get("outcome", "COMPLETE")
        )
    terminal = {
        "schema": terminal_schema
        or (
            "spec140.cell-terminal.v1"
            if experiment_namespace == "spec140"
            else "spec136.cell-terminal.v1"
        ),
        "cellId": cell_id,
        "mode": mode,
        "ratePerPeer": rate,
        "bothPeersPublishAndSubscribe": True,
        "returnCodes": return_codes,
        "status": status,
        "error": error,
        "admissionErrors": harness_errors,
        "profileErrors": profile_errors,
        "loadErrors": load_errors,
        **classification,
    }
    write_json(cell / "terminal.json", terminal)
    return terminal


def tree_sha256(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        digest.update(str(path.relative_to(root)).encode("utf-8"))
        digest.update(b"\0")
        digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def seal_formal_manifest(
    path: Path,
    build_manifest_path: Path,
    preflight_summary_path: Path,
) -> Path:
    path = path.resolve()
    if path.exists():
        raise RuntimeError(f"refusing to overwrite sealed manifest: {path}")
    build_manifest_path = build_manifest_path.resolve()
    build_record = json.loads(build_manifest_path.read_text(encoding="utf-8"))
    if build_record.get("schema") != "spec136.build-manifest.v1":
        raise RuntimeError("unexpected build manifest schema")
    for key in ("binary", "library"):
        artifact = Path(build_record[key])
        if not artifact.is_file() or sha256(artifact) != build_record[f"{key}Sha256"]:
            raise RuntimeError(f"build manifest {key} identity changed")
    for key in ("benchmark", "runner", "analyzer", "builder"):
        source = build_record["sources"][key]
        source_path = Path(source["path"])
        if not source_path.is_file() or sha256(source_path) != source["sha256"]:
            raise RuntimeError(f"build manifest source changed: {key}")
    preflight = json.loads(preflight_summary_path.read_text(encoding="utf-8"))
    if preflight.get("status") != "PASS":
        raise RuntimeError("formal manifest cannot seal before preflight passes")

    canonical_confirmation = (
        REPO
        / "results/spec136-rsa-single-worker/"
        "confirmation-400-r3-fixed-20260723T220300Z"
    )
    record = {
        "schema": "spec136.sealed-formal-manifest.v1",
        "sealedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "executionRequiresExplicitFormalCommand": True,
        "binary": build_record["binary"],
        "binarySha256": build_record["binarySha256"],
        "library": build_record["library"],
        "librarySha256": build_record["librarySha256"],
        "buildManifest": str(build_manifest_path),
        "buildManifestSha256": sha256(build_manifest_path),
        "preflightSummary": str(preflight_summary_path.resolve()),
        "preflightSummarySha256": sha256(preflight_summary_path),
        "matrix": [
            {"ordinal": index, "mode": mode, "ratePerPeer": rate}
            for index, (mode, rate) in enumerate(FORMAL_MATRIX, 1)
        ],
        "timing": {"warmup": 10, "measure": 60, "drain": 10},
        "twoNodes": True,
        "bothPeersPublishAndSubscribe": True,
        "pacerKind": "independent-app-thread",
        "controlCallerPath": "app-post-to-face",
        "treatmentCallerPath": "app-direct-to-worker-backed-publishAsync",
        "syncInterestBatching": True,
        "syncInterestBatchWindowMs": SYNC_BATCH_WINDOW_MS,
        "publicationFetchWindow": PUBLICATION_FETCH_WINDOW,
        "maxEstimatedSignerUtilization": MAX_SIGNER_UTILIZATION,
        "rateUnit": "publications-per-second-per-peer",
        "hostCpuCount": os.cpu_count(),
        "effectiveCpuAffinity": sorted(os.sched_getaffinity(0)),
        "formalRunnerCommand": [
            "sudo",
            "-n",
            "-E",
            "taskset",
            "-c",
            "0-3",
            "python3",
            str(Path(__file__).resolve()),
            "--formal",
            "--sealed-manifest",
            str(path),
        ],
        "protectedConfirmation": {
            "path": str(canonical_confirmation),
            "treeSha256": tree_sha256(canonical_confirmation),
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    write_json(path, record)
    return path


def validate_sealed_manifest(path: Path, binary: Path, library: Path) -> None:
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("schema") != "spec136.sealed-formal-manifest.v1":
        raise RuntimeError("unexpected sealed manifest schema")
    if record.get("executionRequiresExplicitFormalCommand") is not True:
        raise RuntimeError(
            "sealed manifest does not require explicit formal execution"
        )
    if record.get("matrix") != [
        {"ordinal": index, "mode": mode, "ratePerPeer": rate}
        for index, (mode, rate) in enumerate(FORMAL_MATRIX, 1)
    ]:
        raise RuntimeError("sealed formal matrix changed")
    if sha256(binary) != record.get("binarySha256"):
        raise RuntimeError("sealed binary identity changed")
    if sha256(library) != record.get("librarySha256"):
        raise RuntimeError("sealed library identity changed")
    preflight = Path(record["preflightSummary"])
    if not preflight.is_file() or sha256(preflight) != record["preflightSummarySha256"]:
        raise RuntimeError("sealed preflight evidence changed")
    build_manifest = Path(record["buildManifest"])
    if (
        not build_manifest.is_file()
        or sha256(build_manifest) != record["buildManifestSha256"]
    ):
        raise RuntimeError("sealed build manifest changed")


def run_preflight(
    campaign: Path,
    binary: Path,
    library_dir: Path,
    build_manifest_path: Path,
    sealed_manifest_path: Path,
) -> dict[str, Any]:
    errors: list[str] = []
    identities = {
        peer: f"/spec136/preflight/identity/{peer}" for peer in PEERS
    }
    assets = campaign / "security-assets"
    assets.mkdir(parents=True, exist_ok=False)
    homes = {peer: assets / f"{peer}-home" for peer in PEERS}
    certificates = {peer: assets / f"{peer}.cert" for peer in PEERS}
    for peer in PEERS:
        create_local_identity(
            homes[peer], identities[peer], certificates[peer], library_dir
        )

    security_dir = campaign / "security-probes"
    security_commands: dict[str, list[str]] = {}
    security_envs = {
        peer: local_peer_env(homes[peer], library_dir) for peer in PEERS
    }
    for peer in PEERS:
        other = PEERS[1] if peer == PEERS[0] else PEERS[0]
        security_commands[peer] = [
            str(binary),
            "--security-preflight",
            "--identity",
            identities[peer],
            "--peer-certificate",
            str(certificates[other]),
            "--summary",
            str(security_dir / f"{peer}-summary.json"),
        ]
    security_return_codes = run_local_pair(
        security_dir, security_commands, security_envs, timeout=20
    )
    security_summaries = {}
    for peer in PEERS:
        if security_return_codes[peer] != 0:
            errors.append(
                f"{peer}:security process rc={security_return_codes[peer]}"
            )
            continue
        summary_path = security_dir / f"{peer}-summary.json"
        if not summary_path.is_file():
            errors.append(f"{peer}:security summary missing")
            continue
        security_summaries[peer] = json.loads(
            summary_path.read_text(encoding="utf-8")
        )
        errors.extend(
            validate_security_summary(security_summaries[peer], peer)
        )

    pacer_summaries: dict[str, dict[str, Any]] = {}
    for mode in ("face-inline-rsa", "worker-rsa"):
        mode_dir = campaign / f"noop-pacer-{mode}-1000"
        commands = {
            peer: [
                str(binary),
                "--pacer-only",
                "--mode",
                mode,
                "--peer-id",
                peer,
                "--rate",
                "1000",
                "--warmup",
                "1",
                "--measure",
                "60",
                "--summary",
                str(mode_dir / f"{peer}-summary.json"),
            ]
            for peer in PEERS
        }
        return_codes = run_local_pair(
            mode_dir, commands, security_envs, timeout=75
        )
        for peer in PEERS:
            key = f"{mode}:{peer}"
            if return_codes[peer] != 0:
                errors.append(f"{key}:no-op process rc={return_codes[peer]}")
                continue
            summary_path = mode_dir / f"{peer}-summary.json"
            if not summary_path.is_file():
                errors.append(f"{key}:no-op summary missing")
                continue
            pacer_summaries[key] = json.loads(
                summary_path.read_text(encoding="utf-8")
            )
            errors.extend(
                validate_noop_pacer_summary(
                    pacer_summaries[key], mode, peer
                )
            )

    network_terminals = []
    for ordinal, mode in enumerate(("face-inline-rsa", "worker-rsa"), 1):
        terminal = run_cell(
            campaign, binary, library_dir, ordinal, mode, 200, (1, 3, 2)
        )
        network_terminals.append(terminal)
        if terminal["status"] != "COMPLETE":
            errors.append(
                f"{terminal['cellId']}:network smoke status="
                f"{terminal['status']}"
            )

    summary = {
        "schema": "spec136.preflight-summary.v1",
        "status": "PASS" if not errors else "FAIL",
        "errors": errors,
        "binary": str(binary),
        "binarySha256": sha256(binary),
        "library": str((library_dir / "libndn-svs.so").resolve()),
        "librarySha256": sha256(library_dir / "libndn-svs.so"),
        "buildManifest": str(build_manifest_path.resolve()),
        "buildManifestSha256": sha256(build_manifest_path),
        "effectiveCpuAffinity": sorted(os.sched_getaffinity(0)),
        "securityReturnCodes": security_return_codes,
        "security": security_summaries,
        "noopPacer": pacer_summaries,
        "networkTerminals": network_terminals,
    }
    summary_path = campaign / "preflight-summary.json"
    if not errors:
        summary["sealedFormalManifest"] = str(
            sealed_manifest_path.resolve()
        )
    write_json(summary_path, summary)
    if errors:
        return summary
    seal_formal_manifest(
        sealed_manifest_path, build_manifest_path, summary_path
    )
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--binary",
        type=Path,
        default=REPO / "build/spec136-rsa-single-worker-r6/svs-rsa-single-worker",
    )
    parser.add_argument(
        "--library-dir",
        type=Path,
        default=Path("/home/tianxing/NDN/ndn-svs/build"),
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--formal", action="store_true")
    parser.add_argument(
        "--build-manifest",
        type=Path,
        default=REPO / "build/spec136-rsa-single-worker-r6/build-manifest.json",
    )
    parser.add_argument(
        "--sealed-manifest",
        type=Path,
        default=(
            REPO
            / "build/spec136-rsa-single-worker-r6/"
            "sealed-formal-manifest.json"
        ),
    )
    parser.add_argument(
        "--confirmation-400",
        action="store_true",
        help="Run only the two 400 pps cells with 10/60/10 second timing.",
    )
    parser.add_argument(
        "--smoke-rates",
        default="200",
        help="Comma-separated per-peer rates for non-formal smoke cells.",
    )
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("MiniNDN runner must execute as root")
    binary = args.binary.resolve()
    library_dir = args.library_dir.resolve()
    library = library_dir / "libndn-svs.so"
    if not binary.is_file() or not library.is_file():
        raise SystemExit("binary or local libndn-svs.so is missing")
    if args.preflight and (args.formal or args.confirmation_400):
        raise SystemExit(
            "--preflight is mutually exclusive with formal/confirmation execution"
        )
    if args.preflight:
        build_manifest = args.build_manifest.resolve()
        if not build_manifest.is_file():
            raise SystemExit(f"build manifest is missing: {build_manifest}")
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        campaign = (
            args.output.resolve()
            if args.output
            else REPO
            / "results/spec136-rsa-single-worker"
            / f"preflight-r4-{stamp}"
        )
        campaign.mkdir(parents=True, exist_ok=False)
        try:
            summary = run_preflight(
                campaign,
                binary,
                library_dir,
                build_manifest,
                args.sealed_manifest.resolve(),
            )
        finally:
            restore_invoking_user_ownership(campaign)
            sealed_path = args.sealed_manifest.resolve()
            if sealed_path.exists():
                restore_invoking_user_ownership(sealed_path)
        print(campaign)
        return 0 if summary["status"] == "PASS" else 1

    try:
        smoke_rates = tuple(
            int(value.strip()) for value in args.smoke_rates.split(",") if value.strip()
        )
    except ValueError as exc:
        raise SystemExit("--smoke-rates must contain positive integers") from exc
    if not smoke_rates or any(rate <= 0 for rate in smoke_rates):
        raise SystemExit("--smoke-rates must contain positive integers")
    if args.formal and args.smoke_rates != "200":
        raise SystemExit("--smoke-rates cannot modify the frozen formal matrix")
    if args.formal and args.confirmation_400:
        raise SystemExit("--formal and --confirmation-400 are mutually exclusive")
    if args.formal:
        sealed_manifest = args.sealed_manifest.resolve()
        if not sealed_manifest.is_file():
            raise SystemExit(
                "formal execution requires the sealed manifest from T004b"
            )
        validate_sealed_manifest(sealed_manifest, binary, library)
    if args.confirmation_400 and args.smoke_rates != "200":
        raise SystemExit("--smoke-rates cannot modify --confirmation-400")

    label = (
        "formal"
        if args.formal
        else "confirmation-400"
        if args.confirmation_400
        else "smoke"
    )
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    campaign = (
        args.output.resolve()
        if args.output
        else REPO / "results/spec136-rsa-single-worker" / f"{label}-{stamp}"
    )
    campaign.mkdir(parents=True, exist_ok=False)
    matrix = (
        FORMAL_MATRIX
        if args.formal
        else (("face-inline-rsa", 400), ("worker-rsa", 400))
        if args.confirmation_400
        else tuple(
            (mode, rate)
            for rate in smoke_rates
            for mode in ("face-inline-rsa", "worker-rsa")
        )
    )
    timing = (10, 60, 10) if args.formal or args.confirmation_400 else (1, 3, 2)
    manifest = {
        "schema": "spec136.campaign.v2",
        "formal": args.formal,
        "campaignKind": label,
        "binary": str(binary),
        "binarySha256": sha256(binary),
        "library": str(library),
        "librarySha256": sha256(library),
        "matrix": [
            {"ordinal": index, "mode": mode, "ratePerPeer": rate}
            for index, (mode, rate) in enumerate(matrix, 1)
        ],
        "timing": {"warmup": timing[0], "measure": timing[1], "drain": timing[2]},
        "twoNodes": True,
        "bothPeersPublishAndSubscribe": True,
        "pacerKind": "independent-app-thread",
        "controlCallerPath": "app-post-to-face",
        "treatmentCallerPath": "app-direct-to-worker-backed-publishAsync",
        "syncInterestBatching": True,
        "syncInterestBatchWindowMs": SYNC_BATCH_WINDOW_MS,
        "publicationFetchWindow": PUBLICATION_FETCH_WINDOW,
        "maxEstimatedSignerUtilization": MAX_SIGNER_UTILIZATION,
        "rateUnit": "publications-per-second-per-peer",
    }
    if args.formal:
        manifest["sealedFormalManifest"] = str(args.sealed_manifest.resolve())
        manifest["sealedFormalManifestSha256"] = sha256(
            args.sealed_manifest.resolve()
        )
    write_json(campaign / "campaign-manifest.json", manifest)

    terminals = []
    for ordinal, (mode, rate) in enumerate(matrix, 1):
        terminal = run_cell(
            campaign, binary, library_dir, ordinal, mode, rate, timing
        )
        terminals.append(terminal)
    write_json(campaign / "campaign-terminals.json", terminals)
    restore_invoking_user_ownership(campaign.parent)
    print(campaign)
    return 0 if len(terminals) == len(matrix) and all(
        row["status"] == "COMPLETE" for row in terminals
    ) else 1


if __name__ == "__main__":
    raise SystemExit(main())
