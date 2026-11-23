#!/usr/bin/env python3
"""Measure the Spec 164 artifact and physical network ceiling subjects."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import sqlite3
import subprocess
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).resolve()
DEFAULT_TOPOLOGY = REPO / "Experiments/Topology/spec164-artifact-linear.conf"
DEFAULT_OUTPUT = REPO / "results/spec164-artifact-functional"
PAYLOAD_SEGMENT_BYTES = 4096
RAW_NDN_ROOT = "/spec164/raw/artifact"


def _py_path() -> str:
    return ":".join((
        str(REPO / "pythonWrapper"),
        str(REPO / "NDNSF-DistributedRepo/pythonWrapper"),
        os.environ.get("PYTHONPATH", ""),
    ))


def _write_json(path: Path, value: dict[str, Any]) -> None:
    temporary = Path(str(path) + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.replace(temporary, path)


def _restore_output_ownership(output_dir: Path) -> None:
    """Return sudo-created evidence to the invoking workspace user."""

    try:
        uid = int(os.environ["SUDO_UID"])
        gid = int(os.environ["SUDO_GID"])
    except (KeyError, ValueError):
        return
    for root, directories, files in os.walk(output_dir):
        for name in (*directories, *files):
            os.chown(Path(root) / name, uid, gid)
    os.chown(output_dir, uid, gid)


def _wait_for(path: Path, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if path.is_file():
            return
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {path}")


def _node_environment(host_name: str, timeline_sample_rate: float = 0.01) -> dict[str, str]:
    home = Path("/tmp/minindn") / host_name
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    (ndn_dir / "client.conf").write_text(
        f"transport=unix:///run/nfd/{host_name}.sock\n", encoding="utf-8"
    )
    environment = dict(os.environ)
    environment.update({
        "HOME": str(home),
        "NDN_CLIENT_CONF": str(ndn_dir / "client.conf"),
        "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{host_name}.sock",
        "PYTHONPATH": _py_path(),
        "PYTHONUNBUFFERED": "1",
        "LD_LIBRARY_PATH": ":".join((
            str(REPO / "build"),
            os.environ.get("LD_LIBRARY_PATH", ""),
        )),
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": str(timeline_sample_rate),
    })
    return environment


def _prepare_raw_payload(
    run_dir: Path, payload_size: int, data_dir: Path | None = None
) -> str:
    """Create deterministic input without allocating the full object in Python."""

    data_dir = run_dir if data_dir is None else data_dir
    if data_dir == run_dir:
        run_dir.mkdir(parents=True, exist_ok=False)
    else:
        # Portable multi-node roles create their small shared coordination
        # directory before both ranks enter it. Preserve the legacy MiniNDN
        # fail-closed create above, while allowing only a clean coordination
        # directory for the explicitly separated data path.
        run_dir.mkdir(parents=True, exist_ok=True)
        for name in ("raw-fixture.json", "manifest.json", "benchmark-producer.ready"):
            if (run_dir / name).exists():
                raise FileExistsError(f"coordination state already exists: {name}")
        data_dir.mkdir(parents=True, exist_ok=False)
    block = bytes((index * 131 + 17) % 256 for index in range(1 << 20))
    digest = hashlib.sha256()
    remaining = int(payload_size)
    with (data_dir / "payload.bin").open("wb") as stream:
        while remaining:
            piece = block[:min(remaining, len(block))]
            stream.write(piece)
            digest.update(piece)
            remaining -= len(piece)
    value = digest.hexdigest()
    _write_json(run_dir / "raw-fixture.json", {
        "payloadBytes": int(payload_size),
        "contentDigest": value,
        "packetPayloadBytes": PAYLOAD_SEGMENT_BYTES,
    })
    _write_json(run_dir / "manifest.json", {
        "formatVersion": "artifact-manifest-v2",
        "digestAlgorithm": "sha256",
        "contentDigest": value,
        "sizeBytes": int(payload_size),
    })
    private_key = run_dir / "manifest-private.pem"
    subprocess.run([
        "openssl", "genpkey", "-algorithm", "RSA",
        "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(private_key),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([
        "openssl", "pkey", "-in", str(private_key), "-pubout",
        "-out", str(run_dir / "manifest-public.pem"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([
        "openssl", "pkey", "-in", str(private_key), "-pubout",
        "-outform", "DER", "-out", str(run_dir / "manifest-public.der"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([
        "openssl", "dgst", "-sha256", "-sign", str(private_key),
        "-out", str(run_dir / "manifest.signature"), str(run_dir / "manifest.json"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    private_key.unlink()
    return value


def _raw_producer_role(
    run_dir: Path, timeout_s: float, data_dir: Path | None = None
) -> int:
    from ndnsf import FileSegmentedObjectProducer

    data_dir = run_dir if data_dir is None else data_dir
    producer = FileSegmentedObjectProducer(
        RAW_NDN_ROOT,
        str(data_dir / "payload.bin"),
        signing_identity="/spec164/publisher",
        max_segment_size=PAYLOAD_SEGMENT_BYTES,
        digest_signing=True,
    ).start()
    (run_dir / "raw-producer.ready").write_text("ready\n", encoding="utf-8")
    try:
        _wait_for(run_dir / "stop", timeout_s)
    except TimeoutError:
        return 2
    finally:
        producer.stop()
    return 0


def _benchmark_producer_role(
    run_dir: Path,
    subject: str,
    timeout_s: float,
    data_dir: Path | None = None,
    object_prefix: str = RAW_NDN_ROOT,
) -> int:
    from ndnsf import FileSegmentedObjectProducer

    data_dir = run_dir if data_dir is None else data_dir
    digest_signing = subject != "legacy-exact-packet"
    producer = FileSegmentedObjectProducer(
        object_prefix,
        str(data_dir / "payload.bin"),
        signing_identity="/spec164/publisher",
        max_segment_size=PAYLOAD_SEGMENT_BYTES,
        digest_signing=digest_signing,
    ).start()
    _write_json(run_dir / "benchmark-producer-info.json", {
        "subject": subject,
        "digestSigning": digest_signing,
        "publicKeyDerB64": base64.b64encode(producer.public_key_der).decode(),
        "segmentCount": producer.segment_count,
        "fileSize": producer.file_size,
    })
    (run_dir / "benchmark-producer.ready").write_text("ready\n", encoding="utf-8")
    try:
        _wait_for(run_dir / "stop", timeout_s)
    except TimeoutError:
        return 2
    finally:
        producer.stop()
        _write_json(run_dir / "benchmark-producer-result.json", {
            "dataCount": producer.data_count,
            "wireBytes": producer.wire_bytes,
            "signingMs": producer.signing_ms,
            "error": producer.error,
        })
    return 0


def _benchmark_consumer_role(
    run_dir: Path,
    subject: str,
    timeout_s: float,
    result_name: str,
    data_dir: Path | None = None,
    object_prefix: str = RAW_NDN_ROOT,
    cold_prefix: str | None = None,
) -> int:
    from ndnsf import (
        FileSegmentedObjectProducer,
        decode_data_packet,
        fetch_adaptive_segmented_data_packets,
        verify_data_packet_digest,
        verify_data_packet_signature,
        verify_detached_sha256_signature,
    )
    from spec164_artifact_campaign import self_resource_totals

    data_dir = run_dir if data_dir is None else data_dir
    _wait_for(run_dir / "benchmark-producer.ready", timeout_s)
    fixture = json.loads((run_dir / "raw-fixture.json").read_text(encoding="utf-8"))
    producer_info = json.loads(
        (run_dir / "benchmark-producer-info.json").read_text(encoding="utf-8")
    )
    public_key = base64.b64decode(producer_info["publicKeyDerB64"])
    store_dir = data_dir / "stores" / result_name
    store_dir.mkdir(parents=True, exist_ok=False)
    payload_path = store_dir / "payload.bin"
    database_path = store_dir / "metadata.sqlite3"
    connection = None
    if subject != "raw-segmented-ndn":
        connection = sqlite3.connect(str(database_path))
        if subject == "legacy-exact-packet":
            connection.execute(
                "CREATE TABLE exact_packet(segment INTEGER PRIMARY KEY, wire BLOB NOT NULL)"
            )
        else:
            connection.execute(
                "CREATE TABLE artifact(digest TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL, active INTEGER NOT NULL)"
            )
        connection.commit()

    phase_ms = {phase: 0.0 for phase in (
        "discovery", "ackCollection", "planning", "queueWait",
        "sessionStart", "transfer", "verification", "persistence",
        "replication", "commit", "activation",
    )}
    session_started = time.monotonic()
    phase_ms["sessionStart"] = (
        time.monotonic() - session_started) * 1000.0
    asymmetric_count = 0
    digest_count = 0
    asymmetric_ms = 0.0
    digest_ms = 0.0
    persistence_ms = 0.0
    verification_error = ""
    payload_stream = None
    raw_digest = hashlib.sha256()
    raw_cold_source = data_dir / "raw-cold-source.bin"
    if subject == "raw-segmented-ndn":
        payload_stream = raw_cold_source.open("w+b")
        payload_stream.truncate(int(fixture["payloadBytes"]))
    elif subject != "legacy-exact-packet":
        payload_stream = payload_path.open("w+b")
        payload_stream.truncate(int(fixture["payloadBytes"]))

    def accept(packet) -> None:
        nonlocal asymmetric_count, digest_count, asymmetric_ms, digest_ms
        nonlocal persistence_ms, verification_error
        verification_started = time.monotonic()
        if subject == "legacy-exact-packet":
            verified = verify_data_packet_signature(packet.wire, public_key)
            asymmetric_count += 1
            asymmetric_ms += (time.monotonic() - verification_started) * 1000.0
        else:
            verified = verify_data_packet_digest(packet.wire)
            digest_count += 1
            digest_ms += (time.monotonic() - verification_started) * 1000.0
        if not verified:
            verification_error = f"packet signature verification failed: {packet.name}"
            raise RuntimeError(verification_error)
        persistence_started = time.monotonic()
        if subject == "legacy-exact-packet":
            assert connection is not None
            connection.execute(
                "INSERT INTO exact_packet(segment, wire) VALUES (?, ?)",
                (int(packet.segment), sqlite3.Binary(packet.wire)),
            )
        elif subject == "raw-segmented-ndn":
            raw_digest.update(packet.content)
            assert payload_stream is not None
            payload_stream.seek(int(packet.segment) * PAYLOAD_SEGMENT_BYTES)
            payload_stream.write(packet.content)
        else:
            payload_stream.seek(int(packet.segment) * PAYLOAD_SEGMENT_BYTES)
            payload_stream.write(packet.content)
        persistence_ms += (time.monotonic() - persistence_started) * 1000.0

    started = time.monotonic()
    try:
        transfer_started = time.monotonic()
        metrics = fetch_adaptive_segmented_data_packets(
            object_prefix,
            accept,
            timeout_ms=max(1, int(timeout_s * 1000)),
            initial_window=2,
            maximum_window=16,
            maximum_retries=3,
            persistence_backlog_limit=16,
        )
        total_transfer_ms = (time.monotonic() - transfer_started) * 1000.0
        phase_ms["transfer"] = max(0.0, total_transfer_ms - persistence_ms)
        phase_ms["verification"] = asymmetric_ms + digest_ms
        phase_ms["persistence"] = persistence_ms
        commit_started = time.monotonic()
        if subject == "raw-segmented-ndn":
            assert payload_stream is not None
            payload_stream.flush()
            os.fsync(payload_stream.fileno())
            payload_stream.close()
            payload_stream = None
            if raw_digest.hexdigest() != fixture["contentDigest"]:
                raise RuntimeError("raw NDN content digest mismatch")
        elif payload_stream is not None:
            payload_stream.flush()
            os.fsync(payload_stream.fileno())
            payload_stream.close()
            payload_stream = None
            hash_started = time.monotonic()
            digest = hashlib.sha256()
            with payload_path.open("rb") as stream:
                for block in iter(lambda: stream.read(1 << 20), b""):
                    digest.update(block)
            digest_count += 1
            digest_ms += (time.monotonic() - hash_started) * 1000.0
            phase_ms["verification"] = asymmetric_ms + digest_ms
            if digest.hexdigest() != fixture["contentDigest"]:
                raise RuntimeError("artifact content digest mismatch")
            if subject == "signed-manifest":
                verify_started = time.monotonic()
                if not verify_detached_sha256_signature(
                    (run_dir / "manifest.json").read_bytes(),
                    (run_dir / "manifest.signature").read_bytes(),
                    (run_dir / "manifest-public.der").read_bytes(),
                ):
                    raise RuntimeError("manifest signature verification failed")
                asymmetric_count += 1
                asymmetric_ms += (time.monotonic() - verify_started) * 1000.0
                phase_ms["verification"] = asymmetric_ms + digest_ms
            assert connection is not None
            connection.execute(
                "INSERT INTO artifact(digest, size_bytes, active) VALUES (?, ?, 1)",
                (fixture["contentDigest"], int(fixture["payloadBytes"])),
            )
        if connection is not None:
            connection.commit()
        phase_ms["commit"] = (time.monotonic() - commit_started) * 1000.0
        phase_ms["activation"] = 0.0
        elapsed = time.monotonic() - started
        metadata_store_bytes_written = (
            database_path.stat().st_size if database_path.is_file() else 0)
        payload_store_bytes_written = (
            payload_path.stat().st_size if payload_path.is_file() else 0)
        if connection is not None:
            connection.close()
            connection = None

        # Prepare a repository-owned source for a separate consumer's cold
        # retrieval. Scalable subjects serve their committed payload directly.
        # The legacy subject reconstructs content from exact-packet metadata;
        # those bytes are therefore metadata-store reads, not payload reads.
        metadata_store_bytes_read = 0
        payload_store_bytes_read = 0
        cold_source = payload_path
        if subject == "raw-segmented-ndn":
            # The raw subject has no repository storage boundary. Its second
            # transfer remains a matched cold-network baseline only.
            cold_source = raw_cold_source
        elif subject == "legacy-exact-packet":
            cold_source = store_dir / "cold-source.bin"
            cold_connection = sqlite3.connect(str(database_path))
            try:
                rows = cold_connection.execute(
                    "SELECT wire FROM exact_packet ORDER BY segment").fetchall()
            finally:
                cold_connection.close()
            with cold_source.open("xb") as cold_stream:
                for (wire,) in rows:
                    encoded = bytes(wire)
                    metadata_store_bytes_read += len(encoded)
                    cold_stream.write(decode_data_packet(encoded).content)
                cold_stream.flush()
                os.fsync(cold_stream.fileno())
        else:
            cold_connection = sqlite3.connect(str(database_path))
            try:
                row = cold_connection.execute(
                    "SELECT digest, size_bytes, active FROM artifact"
                ).fetchone()
            finally:
                cold_connection.close()
            if row is None or not bool(row[2]):
                raise RuntimeError("cold retrieval metadata is not active")
            metadata_store_bytes_read = metadata_store_bytes_written
            payload_store_bytes_read = int(fixture["payloadBytes"])
        if hasattr(os, "posix_fadvise"):
            with cold_source.open("rb") as cold_stream:
                os.posix_fadvise(
                    cold_stream.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
        cold_prefix = cold_prefix or f"/spec164/repo-cold/{result_name}"
        # Publication workers can complete concurrently, but NFD management
        # commands signed by the same node identity must not race each other.
        # Announce durable store readiness first; the coordinator serializes
        # prefix registration and then launches every cold fetch concurrently.
        _write_json(run_dir / f"{result_name}.store-ready.json", {
            "prefix": cold_prefix,
            "contentDigest": fixture["contentDigest"],
            "payloadBytes": int(fixture["payloadBytes"]),
        })
        _wait_for(run_dir / f"{result_name}.serve-cold", timeout_s)
        cold_producer = FileSegmentedObjectProducer(
            cold_prefix,
            str(cold_source),
            signing_identity="/spec164/repo",
            max_segment_size=PAYLOAD_SEGMENT_BYTES,
            digest_signing=True,
        ).start()
        _write_json(run_dir / f"{result_name}.producer-ready.json", {
            "prefix": cold_prefix,
            "contentDigest": fixture["contentDigest"],
            "payloadBytes": int(fixture["payloadBytes"]),
        })
        try:
            _wait_for(run_dir / f"{result_name}.cold.json", timeout_s)
        finally:
            cold_producer.stop()
        cold_result = json.loads(
            (run_dir / f"{result_name}.cold.json").read_text(encoding="utf-8"))
        if cold_result.get("status") != "SUCCESS":
            raise RuntimeError("cold retrieval did not reconstruct the artifact")

        data_wire_bytes = int(metrics.data_wire_bytes)
        interest_wire_bytes = int(metrics.interest_wire_bytes)
        storage_bytes_read = (
            payload_store_bytes_read + metadata_store_bytes_read)
        storage_bytes_written = (
            payload_store_bytes_written + metadata_store_bytes_written)
        _write_json(run_dir / f"{result_name}.json", {
            "status": "SUCCESS",
            "subject": subject,
            "elapsedMs": elapsed * 1000.0,
            "logicalBytes": int(metrics.logical_bytes),
            "dataWireBytes": data_wire_bytes,
            "interestWireBytes": interest_wire_bytes,
            "wireBytes": data_wire_bytes + interest_wire_bytes,
            "interestCount": int(metrics.interest_count),
            "dataCount": int(metrics.delivered_segments),
            "timeoutCount": int(metrics.timeout_count),
            "retransmissionCount": int(metrics.retransmission_count),
            "retransmittedBytes": int(metrics.retransmitted_bytes),
            "windowMinimum": 1 if int(metrics.delivered_segments) else 0,
            "windowMaximum": int(metrics.maximum_in_flight),
            "asymmetricVerifyCount": asymmetric_count,
            "asymmetricVerifyMs": asymmetric_ms,
            "digestVerifyCount": digest_count,
            "digestVerifyMs": digest_ms,
            "metadataOperations": (
                int(metrics.delivered_segments)
                if subject == "legacy-exact-packet"
                else (0 if subject == "raw-segmented-ndn" else 1)
            ),
            "metadataRecords": (
                int(metrics.delivered_segments)
                if subject == "legacy-exact-packet"
                else (0 if subject == "raw-segmented-ndn" else 1)
            ),
            "payloadStoreBytesRead": payload_store_bytes_read,
            "payloadStoreBytesWritten": payload_store_bytes_written,
            "metadataStoreBytesRead": metadata_store_bytes_read,
            "metadataStoreBytesWritten": metadata_store_bytes_written,
            "storageBytesRead": storage_bytes_read,
            "storageBytesWritten": storage_bytes_written,
            "coldRetrieval": cold_result,
            "phaseLatencyMs": phase_ms,
            **self_resource_totals(),
        })
        return 0
    except BaseException as error:
        _write_json(run_dir / f"{result_name}.json", {
            "status": "FAILED",
            "subject": subject,
            "error": str(error),
            "verificationError": verification_error,
            "phaseLatencyMs": phase_ms,
            **self_resource_totals(),
        })
        return 1
    finally:
        if payload_stream is not None:
            payload_stream.close()
        if connection is not None:
            connection.close()


def _benchmark_cold_consumer_role(
    run_dir: Path,
    timeout_s: float,
    result_name: str,
    data_dir: Path | None = None,
) -> int:
    from ndnsf import fetch_adaptive_segmented_data_packets

    data_dir = run_dir if data_dir is None else data_dir
    ready_path = run_dir / f"{result_name}.store-ready.json"
    _wait_for(ready_path, timeout_s)
    ready = json.loads(ready_path.read_text(encoding="utf-8"))
    destination = data_dir / "cold-destinations" / f"{result_name}.bin"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise RuntimeError("cold retrieval destination already exists")
    stream = destination.open("x+b")
    stream.truncate(int(ready["payloadBytes"]))

    def accept(packet) -> None:
        stream.seek(int(packet.segment) * PAYLOAD_SEGMENT_BYTES)
        stream.write(packet.content)

    started = time.monotonic()
    try:
        metrics = fetch_adaptive_segmented_data_packets(
            ready["prefix"],
            accept,
            timeout_ms=max(1, int(timeout_s * 1000)),
            initial_window=2,
            maximum_window=16,
            maximum_retries=3,
            persistence_backlog_limit=16,
        )
        stream.flush()
        os.fsync(stream.fileno())
    finally:
        stream.close()
    elapsed = time.monotonic() - started
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    success = digest == ready["contentDigest"] and destination.is_file()
    data_wire_bytes = int(metrics.data_wire_bytes)
    interest_wire_bytes = int(metrics.interest_wire_bytes)
    _write_json(run_dir / f"{result_name}.cold.json", {
        "status": "SUCCESS" if success else "FAIL",
        "elapsedMs": elapsed * 1000.0,
        "logicalBytes": int(metrics.logical_bytes),
        "logicalGoodputMbps": (
            int(metrics.logical_bytes) * 8.0 / elapsed / 1_000_000.0),
        "dataWireBytes": data_wire_bytes,
        "interestWireBytes": interest_wire_bytes,
        "wireBytes": data_wire_bytes + interest_wire_bytes,
        "interestCount": int(metrics.interest_count),
        "dataCount": int(metrics.delivered_segments),
        "timeoutCount": int(metrics.timeout_count),
        "retransmissionCount": int(metrics.retransmission_count),
        "destination": str(destination),
        "destinationVisible": destination.is_file(),
        "contentDigest": digest,
    })
    return 0 if success else 1


def _raw_consumer_role(run_dir: Path, timeout_s: float) -> int:
    from ndnsf import fetch_adaptive_segmented_data_packets
    from spec164_artifact_campaign import self_resource_totals

    fixture = json.loads((run_dir / "raw-fixture.json").read_text(encoding="utf-8"))
    _wait_for(run_dir / "raw-producer.ready", timeout_s)
    digest = hashlib.sha256()
    started = time.monotonic()
    metrics = fetch_adaptive_segmented_data_packets(
        RAW_NDN_ROOT,
        lambda packet: digest.update(packet.content),
        timeout_ms=max(1, int(timeout_s * 1000)),
        initial_window=2,
        maximum_window=16,
        maximum_retries=3,
        persistence_backlog_limit=16,
    )
    elapsed = time.monotonic() - started
    logical_bytes = int(metrics.logical_bytes)
    result = {
        "status": "SUCCESS" if digest.hexdigest() == fixture["contentDigest"] else "FAIL",
        "elapsedMs": elapsed * 1000.0,
        "logicalBytes": logical_bytes,
        "wireBytes": int(metrics.wire_bytes),
        "logicalGoodputMbps": logical_bytes * 8.0 / elapsed / 1_000_000.0,
        "wireGoodputMbps": int(metrics.wire_bytes) * 8.0 / elapsed / 1_000_000.0,
        "interestCount": int(metrics.interest_count),
        "dataCount": int(metrics.delivered_segments),
        "timeoutCount": int(metrics.timeout_count),
        "retransmissionCount": int(metrics.retransmission_count),
        "retransmittedBytes": int(metrics.retransmitted_bytes),
        "windowMinimum": 1 if int(metrics.delivered_segments) else 0,
        "windowMaximum": int(metrics.maximum_in_flight),
        "contentDigest": digest.hexdigest(),
        **self_resource_totals(),
    }
    _write_json(run_dir / "raw-result.json", result)
    return 0 if result["status"] == "SUCCESS" else 1


def _available_memory_bytes() -> int:
    for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    return 0


def _run_raw_ndn_ceiling(
    *,
    output_dir: Path,
    topology_file: Path,
    payload_size: int,
    timeout_s: float,
    timeline_sample_rate: float,
    quick_smoke: bool,
) -> dict[str, Any]:
    """Run the matched raw segmented-NDN baseline over the repository topology."""

    from mininet.node import Controller
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.nfdc import Nfdc
    from minindn.minindn import Minindn
    from minindn.util import getPopen
    from spec164_artifact_campaign import read_proc_sample, stable_sample

    run_dir = output_dir / "raw-segmented-ndn"
    digest = _prepare_raw_payload(run_dir, payload_size)
    setLogLevel("warning")
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(topology_file), controller=Controller)
    processes = []
    logs = []
    samples = []
    operation_id = f"raw-segmented-ndn-{payload_size}-r1-c1"
    sample_resources = stable_sample(operation_id, timeline_sample_rate)
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        time.sleep(0.5)
        publisher = ndn.net["publisher"]
        repo = ndn.net["repo"]
        consumer = ndn.net["consumer"]
        publisher_ip = publisher.connectionsTo(repo)[0][0].IP()
        repo_ip = repo.connectionsTo(consumer)[0][0].IP()
        Nfdc.createFace(repo, publisher_ip)
        Nfdc.registerRoute(repo, "/spec164/raw", publisher_ip, cost=0)
        Nfdc.createFace(consumer, repo_ip)
        Nfdc.registerRoute(consumer, "/spec164/raw", repo_ip, cost=0)

        def start(host_name: str, role: str):
            stream = (run_dir / f"{role}.log").open("wb")
            command = " ".join((
                "exec", shlex.quote(sys.executable), shlex.quote(str(SCRIPT)),
                "--role", role, "--run-dir", shlex.quote(str(run_dir)),
                "--timeout-seconds", str(timeout_s),
            ))
            process = getPopen(
                ndn.net[host_name], command,
                envDict=_node_environment(host_name, timeline_sample_rate),
                shell=True, stdout=stream, stderr=subprocess.STDOUT,
            )
            processes.append((role, process))
            logs.append(stream)

        start("publisher", "raw-producer")
        start("consumer", "raw-consumer")
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline and not (run_dir / "raw-result.json").is_file():
            for role, process in processes:
                if process.poll() is None:
                    try:
                        samples.append(read_proc_sample(
                            process.pid,
                            operation_id=f"raw-{payload_size}",
                            phase="transfer",
                        ))
                    except (FileNotFoundError, ProcessLookupError):
                        pass
            time.sleep(0.1)
        _wait_for(run_dir / "raw-result.json", max(0.1, deadline - time.monotonic()))
        (run_dir / "stop").write_text("stop\n", encoding="utf-8")
        for role, process in processes:
            process.wait(timeout=max(0.1, timeout_s))
            if process.returncode != 0:
                raise RuntimeError(f"{role} exited with {process.returncode}")
        measured = json.loads((run_dir / "raw-result.json").read_text())
        result = {
            "subject": "raw-segmented-ndn",
            "verdict": "PASS" if measured["status"] == "SUCCESS" else "FAIL",
            "admissible": measured["status"] == "SUCCESS",
            "failureReason": "",
            "payloadBytes": payload_size,
            "packetPayloadBytes": PAYLOAD_SEGMENT_BYTES,
            "resourceSamples": samples,
            "measurement": measured,
            "performanceClaim": not quick_smoke and measured["status"] == "SUCCESS",
        }
        _write_json(run_dir / "summary.json", result)
        return result
    finally:
        try:
            (run_dir / "stop").write_text("stop\n", encoding="utf-8")
        except OSError:
            pass
        for _, process in processes:
            if process.poll() is None:
                process.terminate()
        for _, process in processes:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        for stream in logs:
            stream.close()
        ndn.stop()
        Minindn.cleanUp()


def _run_repository_subject(
    *,
    subject: str,
    output_dir: Path,
    topology_file: Path,
    payload_size: int,
    replicas: int,
    concurrency: int,
    timeout_s: float,
    timeline_sample_rate: float,
    quick_smoke: bool,
) -> dict[str, Any]:
    """Run one raw/repository matrix cell over the three-replica topology."""

    from mininet.node import Controller
    from mininet.log import setLogLevel
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.nfdc import Nfdc
    from minindn.minindn import Minindn
    from minindn.util import getPopen
    from spec164_artifact_campaign import read_proc_sample, stable_sample

    if replicas not in (1, 3):
        raise ValueError("repository benchmark replicas must be 1 or 3")
    if concurrency not in (1, 4, 16):
        raise ValueError("repository benchmark concurrency must be 1, 4, or 16")
    run_dir = output_dir / subject
    _prepare_raw_payload(run_dir, payload_size)
    setLogLevel("warning")
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(topology_file), controller=Controller)
    processes = []
    logs = []
    samples = []
    operation_id = f"{subject}-{payload_size}-r{replicas}-c{concurrency}"
    sample_resources = stable_sample(operation_id, timeline_sample_rate)
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        time.sleep(0.5)
        publisher = ndn.net["publisher"]
        host_names = {host.name for host in ndn.net.hosts}
        repo_names = [
            name for name in ("repo1", "repo2", "repo3") if name in host_names
        ]
        if len(repo_names) < replicas:
            raise RuntimeError("topology does not contain requested repository replicas")
        for repo_name in repo_names:
            repo_host = ndn.net[repo_name]
            publisher_ip = publisher.connectionsTo(repo_host)[0][0].IP()
            Nfdc.createFace(repo_host, publisher_ip)
            Nfdc.registerRoute(repo_host, "/spec164/raw", publisher_ip, cost=0)

        def start(host_name: str, role: str, result_name: str = ""):
            log_name = (
                f"cold-{result_name}"
                if role == "benchmark-cold-consumer" else (result_name or role)
            )
            stream = (run_dir / f"{log_name}.log").open("wb")
            pieces = [
                "exec", "env",
                "PYTHONPATH=" + shlex.quote(_py_path()),
                "LD_LIBRARY_PATH=" + shlex.quote(":".join((
                    str(REPO / "build"),
                    os.environ.get("LD_LIBRARY_PATH", ""),
                ))),
                shlex.quote(sys.executable), shlex.quote(str(SCRIPT)),
                "--role", role, "--run-dir", shlex.quote(str(run_dir)),
                "--benchmark-subject", subject,
                "--timeout-seconds", str(timeout_s),
            ]
            if result_name:
                pieces.extend(("--result-name", result_name))
            process = getPopen(
                ndn.net[host_name],
                " ".join(pieces),
                envDict=_node_environment(host_name, timeline_sample_rate),
                shell=True,
                stdout=stream,
                stderr=subprocess.STDOUT,
            )
            processes.append((log_name, process))
            logs.append(stream)

        start("publisher", "benchmark-producer")
        _wait_for(run_dir / "benchmark-producer.ready", timeout_s)
        consumer_specs = []
        if subject == "raw-segmented-ndn":
            for operation in range(concurrency):
                consumer_specs.append((
                    repo_names[operation % len(repo_names)],
                    f"consumer-op{operation}",
                ))
        else:
            for operation in range(concurrency):
                for replica in range(replicas):
                    consumer_specs.append((
                        repo_names[replica],
                        f"consumer-op{operation}-replica{replica}",
                    ))
        for host_name, result_name in consumer_specs:
            start(host_name, "benchmark-consumer", result_name)

        store_ready_paths = [
            run_dir / f"{name}.store-ready.json" for _, name in consumer_specs
        ]
        ready_deadline = time.monotonic() + timeout_s
        for path in store_ready_paths:
            _wait_for(path, max(0.1, ready_deadline - time.monotonic()))
        registration_deadline = time.monotonic() + timeout_s
        for _repo_name, result_name in consumer_specs:
            (run_dir / f"{result_name}.serve-cold").write_text(
                "serve\n", encoding="utf-8"
            )
            _wait_for(
                run_dir / f"{result_name}.producer-ready.json",
                max(0.1, registration_deadline - time.monotonic()),
            )
        cold_consumer = ndn.net["consumer"]
        cold_faces: dict[str, str] = {}
        for repo_name, result_name in consumer_specs:
            if repo_name not in cold_faces:
                repo_host = ndn.net[repo_name]
                repo_ip = repo_host.connectionsTo(cold_consumer)[0][0].IP()
                Nfdc.createFace(cold_consumer, repo_ip)
                cold_faces[repo_name] = repo_ip
            Nfdc.registerRoute(
                cold_consumer,
                f"/spec164/repo-cold/{result_name}",
                cold_faces[repo_name],
                cost=0,
            )
            start("consumer", "benchmark-cold-consumer", result_name)

        result_paths = [run_dir / f"{name}.json" for _, name in consumer_specs]
        cold_result_paths = [
            run_dir / f"{name}.cold.json" for _, name in consumer_specs
        ]
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline and not all(
                path.is_file() for path in result_paths + cold_result_paths):
            for process_name, process in processes:
                if sample_resources and process.poll() is None:
                    try:
                        samples.append(read_proc_sample(
                            process.pid,
                            operation_id=operation_id,
                            phase="transfer",
                        ))
                    except (FileNotFoundError, ProcessLookupError):
                        pass
            time.sleep(0.1)
        for path in result_paths + cold_result_paths:
            _wait_for(path, max(0.1, deadline - time.monotonic()))
        (run_dir / "stop").write_text("stop\n", encoding="utf-8")
        for process_name, process in processes:
            process.wait(timeout=max(0.1, timeout_s))
            if process.returncode != 0:
                raise RuntimeError(
                    f"{process_name} exited with {process.returncode}"
                )
        measured = [
            json.loads(path.read_text(encoding="utf-8")) for path in result_paths
        ]
        cold_measured = [
            json.loads(path.read_text(encoding="utf-8"))
            for path in cold_result_paths
        ]
        producer = json.loads(
            (run_dir / "benchmark-producer-result.json").read_text(encoding="utf-8")
        )
        success = all(item["status"] == "SUCCESS" for item in measured)
        elapsed_ms = max(float(item["elapsedMs"]) for item in measured)
        logical_bytes = payload_size * concurrency
        data_wire_bytes = sum(int(item["dataWireBytes"]) for item in measured)
        interest_wire_bytes = sum(
            int(item["interestWireBytes"]) for item in measured)
        wire_bytes = data_wire_bytes + interest_wire_bytes
        cold_elapsed_ms = max(
            float(item["elapsedMs"]) for item in cold_measured)
        cold_logical_bytes = payload_size * concurrency
        cold_data_wire_bytes = sum(
            int(item["dataWireBytes"]) for item in cold_measured)
        cold_interest_wire_bytes = sum(
            int(item["interestWireBytes"]) for item in cold_measured)
        phase_latency = {
            phase: max(float(item["phaseLatencyMs"][phase]) for item in measured)
            for phase in measured[0]["phaseLatencyMs"]
        }
        result = {
            "subject": subject,
            "verdict": "PASS" if success else "FAIL",
            "admissible": success,
            "failureReason": "" if success else "one or more workers failed",
            "payloadBytes": payload_size,
            "replicas": replicas,
            "concurrency": concurrency,
            "packetPayloadBytes": PAYLOAD_SEGMENT_BYTES,
            "elapsedMs": elapsed_ms,
            "logicalBytes": logical_bytes,
            "dataWireBytes": data_wire_bytes,
            "interestWireBytes": interest_wire_bytes,
            "wireBytes": wire_bytes,
            "logicalGoodputMbps": logical_bytes * 8.0 / elapsed_ms / 1000.0,
            "wireGoodputMbps": wire_bytes * 8.0 / elapsed_ms / 1000.0,
            "retransmittedBytes": sum(
                int(item["retransmittedBytes"]) for item in measured
            ),
            "payloadStoreBytesRead": sum(
                int(item["payloadStoreBytesRead"]) for item in measured
            ),
            "payloadStoreBytesWritten": sum(
                int(item["payloadStoreBytesWritten"]) for item in measured
            ),
            "metadataStoreBytesRead": sum(
                int(item["metadataStoreBytesRead"]) for item in measured
            ),
            "metadataStoreBytesWritten": sum(
                int(item["metadataStoreBytesWritten"]) for item in measured
            ),
            "storageBytesRead": sum(
                int(item["storageBytesRead"]) for item in measured
            ),
            "storageBytesWritten": sum(
                int(item["storageBytesWritten"]) for item in measured
            ),
            "coldRetrievalElapsedMs": cold_elapsed_ms,
            "coldRetrievalLogicalBytes": cold_logical_bytes,
            "coldRetrievalLogicalGoodputMbps": (
                cold_logical_bytes * 8.0 / cold_elapsed_ms / 1000.0),
            "coldRetrievalDataWireBytes": cold_data_wire_bytes,
            "coldRetrievalInterestWireBytes": cold_interest_wire_bytes,
            "coldRetrievalWireBytes": (
                cold_data_wire_bytes + cold_interest_wire_bytes),
            "coldDestinationVisible": all(
                bool(item["destinationVisible"]) for item in cold_measured),
            "interestCount": sum(int(item["interestCount"]) for item in measured),
            "dataCount": sum(int(item["dataCount"]) for item in measured),
            "timeoutCount": sum(int(item["timeoutCount"]) for item in measured),
            "retransmissionCount": sum(
                int(item["retransmissionCount"]) for item in measured
            ),
            "windowMinimum": min(int(item["windowMinimum"]) for item in measured),
            "windowMaximum": max(int(item["windowMaximum"]) for item in measured),
            "asymmetricVerifyCount": sum(
                int(item["asymmetricVerifyCount"]) for item in measured
            ),
            "asymmetricVerifyMs": sum(
                float(item["asymmetricVerifyMs"]) for item in measured
            ),
            "digestVerifyCount": sum(
                int(item["digestVerifyCount"]) for item in measured
            ),
            "digestVerifyMs": sum(
                float(item["digestVerifyMs"]) for item in measured
            ),
            "metadataOperations": sum(
                int(item["metadataOperations"]) for item in measured
            ),
            "metadataRecords": sum(int(item["metadataRecords"]) for item in measured),
            "requestedReplicas": replicas,
            "selectedReplicas": replicas,
            "committedReplicas": replicas if success else 0,
            "phaseLatencyMs": phase_latency,
            "cpuUserSeconds": sum(float(item["cpuUserSeconds"]) for item in measured),
            "cpuSystemSeconds": sum(
                float(item["cpuSystemSeconds"]) for item in measured
            ),
            "peakRssBytes": max(int(item["peakRssBytes"]) for item in measured),
            "resourceSamples": samples,
            "producerMetrics": producer,
            "workers": measured,
            "coldWorkers": cold_measured,
            "performanceClaim": not quick_smoke and success,
        }
        _write_json(run_dir / "summary.json", result)
        return result
    finally:
        try:
            (run_dir / "stop").write_text("stop\n", encoding="utf-8")
        except OSError:
            pass
        for _, process in processes:
            if process.poll() is None:
                process.terminate()
        for _, process in processes:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        for stream in logs:
            stream.close()
        ndn.stop()
        Minindn.cleanUp()
        shutil.rmtree(run_dir / "stores", ignore_errors=True)
        shutil.rmtree(run_dir / "cold-destinations", ignore_errors=True)
        (run_dir / "payload.bin").unlink(missing_ok=True)
        (run_dir / "stop").unlink(missing_ok=True)
        (run_dir / "benchmark-producer.ready").unlink(missing_ok=True)
        for path in run_dir.glob("*.store-ready.json"):
            path.unlink(missing_ok=True)
        for pattern in ("*.serve-cold", "*.producer-ready.json"):
            for path in run_dir.glob(pattern):
                path.unlink(missing_ok=True)


def _run_physical_network_ceiling(
    *,
    output_dir: Path,
    topology_file: Path,
    measurement_window_seconds: float,
    quick_smoke: bool,
) -> dict[str, Any]:
    """Measure both directions of each physical/virtual link with iperf2."""

    from mininet.node import Controller
    from mininet.log import setLogLevel
    from minindn.minindn import Minindn
    from minindn.util import getPopen
    from spec164_artifact_campaign import final_iperf2_result

    run_dir = output_dir / "physical-network"
    run_dir.mkdir(parents=True, exist_ok=False)
    setLogLevel("warning")
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(topology_file), controller=Controller)
    servers = []
    try:
        ndn.start()
        publisher, repo, consumer = (
            ndn.net["publisher"], ndn.net["repo"], ndn.net["consumer"]
        )
        links = (("publisher-repo", publisher, repo), ("repo-consumer", repo, consumer))
        measurements = []
        port = 5201
        for link_name, left, right in links:
            for direction, client, server in (
                ("forward", left, right), ("reverse", right, left)
            ):
                server_ip = server.connectionsTo(client)[0][0].IP()
                server_process = getPopen(
                    server, ["iperf", "-s", "-p", str(port), "-y", "C"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                servers.append(server_process)
                time.sleep(0.2)
                client_process = getPopen(
                    client,
                    [
                        "iperf", "-c", server_ip, "-p", str(port), "-y", "C",
                        "-t", str(measurement_window_seconds),
                    ],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                )
                output, _ = client_process.communicate(
                    timeout=measurement_window_seconds + 15
                )
                server_process.terminate()
                server_process.wait(timeout=2)
                parsed = final_iperf2_result(output)
                measurements.append({
                    "link": link_name,
                    "direction": direction,
                    "intervalSeconds": parsed.interval_seconds,
                    "transferredBytes": parsed.transferred_bytes,
                    "goodputMbps": parsed.goodput_mbps,
                })
                port += 1
        result = {
            "subject": "physical-network",
            "verdict": "PASS",
            "admissible": True,
            "measurementWindowSeconds": measurement_window_seconds,
            "measurements": measurements,
            "pathBottleneckMbps": min(item["goodputMbps"] for item in measurements),
            "performanceClaim": not quick_smoke,
        }
        _write_json(run_dir / "summary.json", result)
        return result
    finally:
        for process in servers:
            if process.poll() is None:
                process.terminate()
        ndn.stop()
        Minindn.cleanUp()


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology-file", type=Path, default=DEFAULT_TOPOLOGY)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--payload-size", type=int, default=64 * 1024)
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    parser.add_argument(
        "--performance-subject",
        choices=(
            "physical-network", "raw-segmented-ndn", "legacy-exact-packet",
            "digest-only", "signed-manifest",
        ),
        help="Run one matched ceiling subject.",
    )
    parser.add_argument(
        "--measurement-window-seconds",
        type=float,
        default=60.0,
        help="Rate-over-time window; formal physical ceilings require >=60s.",
    )
    parser.add_argument(
        "--timeline-sample-rate",
        type=float,
        default=0.01,
        help="Stable operation trace/resource sampling rate in [0,1].",
    )
    parser.add_argument(
        "--freeze-campaign",
        action="store_true",
        help="Freeze an immutable campaign manifest before running the subject.",
    )
    parser.add_argument("--campaign-id", default="")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--replicas", type=int, choices=(1, 3), default=1)
    parser.add_argument("--concurrency", type=int, choices=(1, 4, 16), default=1)
    parser.add_argument(
        "--benchmark-subject",
        choices=(
            "raw-segmented-ndn", "legacy-exact-packet",
            "digest-only", "signed-manifest",
        ),
        default="raw-segmented-ndn",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--result-name", default="", help=argparse.SUPPRESS)
    parser.add_argument(
        "--object-prefix", default=RAW_NDN_ROOT, help=argparse.SUPPRESS
    )
    parser.add_argument("--cold-prefix", default="", help=argparse.SUPPRESS)
    parser.add_argument(
        "--quick-smoke",
        action="store_true",
        help="Run a short connectivity smoke without a performance claim.",
    )
    parser.add_argument(
        "--role",
        choices=(
            "raw-producer", "raw-consumer",
            "benchmark-producer", "benchmark-consumer",
            "benchmark-cold-consumer",
        ),
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--run-dir", type=Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--data-dir",
        type=Path,
        help=(
            "Optional rank-local payload/store directory for internal "
            "performance roles; --run-dir remains the coordination directory."
        ),
    )
    args = parser.parse_args(argv)
    if not args.role and not args.performance_subject:
        parser.error("--performance-subject is required")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    if args.role:
        if args.run_dir is None:
            raise SystemExit("--run-dir is required for an internal role")
        if args.role == "raw-producer":
            return _raw_producer_role(
                args.run_dir, args.timeout_seconds, args.data_dir
            )
        if args.role == "raw-consumer":
            return _raw_consumer_role(args.run_dir, args.timeout_seconds)
        if args.role == "benchmark-producer":
            return _benchmark_producer_role(
                args.run_dir,
                args.benchmark_subject,
                args.timeout_seconds,
                args.data_dir,
                args.object_prefix,
            )
        if not args.result_name:
            raise SystemExit("--result-name is required for benchmark-consumer")
        if args.role == "benchmark-cold-consumer":
            return _benchmark_cold_consumer_role(
                args.run_dir,
                args.timeout_seconds,
                args.result_name,
                args.data_dir,
            )
        return _benchmark_consumer_role(
            args.run_dir,
            args.benchmark_subject,
            args.timeout_seconds,
            args.result_name,
            args.data_dir,
            args.object_prefix,
            args.cold_prefix or None,
        )

    # MiniNDN parses process-global argv in its constructor. The experiment
    # parser above already owns these options, so do not let MiniNDN reinterpret
    # them as topology-runner arguments.
    sys.argv = [sys.argv[0]]
    from spec164_artifact_campaign import (
        build_cells,
        create_campaign_manifest,
        freeze_campaign,
        validate_measurement_window,
    )

    validate_measurement_window(
        args.measurement_window_seconds, args.quick_smoke
    )
    if not 0.0 <= args.timeline_sample_rate <= 1.0:
        raise SystemExit("--timeline-sample-rate must be in [0,1]")
    output_dir = args.output_dir.resolve()
    if output_dir.exists():
        raise SystemExit(f"output directory already exists: {output_dir}")
    output_dir.mkdir(parents=True)
    if args.freeze_campaign:
        campaign_id = args.campaign_id or (
            "spec164-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        )
        manifest = create_campaign_manifest(
            campaign_id=campaign_id,
            repo_root=REPO,
            topology={
                "file": str(args.topology_file.resolve()),
                "kind": "publisher-repo-consumer-linear",
                "delayMsPerLink": 1,
                "bandwidthMbpsPerLink": 1000,
            },
            admissibility={
                "predeclaredExclusionsOnly": True,
                "retainFailedAndNegativeRuns": True,
                "rawAndRepositoryGeometryMatched": True,
            },
            cells=build_cells(),
            repetitions=args.repetitions,
            timeline_sample_rate=args.timeline_sample_rate,
            quick_smoke=args.quick_smoke,
            measurement_window_seconds=args.measurement_window_seconds,
        )
        freeze_campaign(output_dir, manifest)
    try:
        if args.performance_subject == "physical-network":
            result = _run_physical_network_ceiling(
                output_dir=output_dir,
                topology_file=args.topology_file.resolve(),
                measurement_window_seconds=(
                    min(1.0, args.measurement_window_seconds)
                    if args.quick_smoke else args.measurement_window_seconds
                ),
                quick_smoke=args.quick_smoke,
            )
        else:
            result = _run_repository_subject(
                subject=args.performance_subject,
                output_dir=output_dir,
                topology_file=args.recovery_topology_file.resolve(),
                payload_size=(
                    min(args.payload_size, 1 * 1024 * 1024)
                    if args.quick_smoke else args.payload_size
                ),
                replicas=args.replicas,
                concurrency=args.concurrency,
                timeout_s=(
                    min(args.timeout_seconds, 8.0)
                    if args.quick_smoke else args.timeout_seconds
                ),
                timeline_sample_rate=args.timeline_sample_rate,
                quick_smoke=args.quick_smoke,
            )
        _write_json(output_dir / "summary.json", {
            "schema": "ndnsf-repo-spec164-ceiling-v2",
            "quickSmoke": bool(args.quick_smoke),
            "performanceClaim": bool(result.get("performanceClaim", False)),
            "result": result,
        })
        _restore_output_ownership(output_dir)
        if args.quick_smoke and result["verdict"] == "PASS":
            print("SPEC164_ARTIFACT_CEILING_SMOKE_OK")
        else:
            print(json.dumps(result, sort_keys=True))
        return 0 if result["verdict"] == "PASS" else 1
    except Exception:
        _restore_output_ownership(output_dir)
        raise


if __name__ == "__main__":
    raise SystemExit(main())
