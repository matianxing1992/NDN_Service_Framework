#!/usr/bin/env python3
"""Spec 164 T011 MiniNDN interruption, recovery, and durability matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import threading
import time
from typing import Any

from NDNSF_DistributedRepo_Artifact_Minindn import (
    ARTIFACT_CHUNK_BYTES,
    PAYLOAD_SEGMENT_BYTES,
    PUBLISHER_ROOT,
    REPO,
    _load_fixture,
    _node_environment,
    _packet_producer,
    _prepare_fixture,
    _restore_output_ownership,
    _wait_for,
    _write_json,
)


SCRIPT = Path(__file__).resolve()
DEFAULT_TOPOLOGY = (
    REPO / "Experiments/Topology/spec164-artifact-recovery.conf"
)
DEFAULT_EVIDENCE_ROOT = (
    REPO
    / "specs/164-distributed-repo-large-artifact-transport/evidence/us2"
)


def _result_path(run_dir: Path, name: str) -> Path:
    return run_dir / "results" / f"{name}.json"


def _producer_role(
    run_dir: Path, ready_name: str, timeout_s: float
) -> int:
    config = json.loads(
        (run_dir / "fixture.json").read_text(encoding="utf-8")
    )
    payload = (run_dir / "payload.bin").read_bytes()
    producers = [
        _packet_producer(
            f"{PUBLISHER_ROOT}/root",
            (run_dir / "root.wire").read_bytes(),
            "/spec164/publisher",
        ),
        _packet_producer(
            f"{PUBLISHER_ROOT}/page/0",
            (run_dir / "page.wire").read_bytes(),
            "/spec164/publisher",
        ),
    ]
    for chunk in config["chunks"]:
        offset = int(chunk["offsetBytes"])
        length = int(chunk["lengthBytes"])
        producers.append(_packet_producer(
            f"{PUBLISHER_ROOT}/payload/chunk/{chunk['index']}",
            payload[offset:offset + length],
            "/spec164/publisher",
        ))
    (run_dir / ready_name).write_text("ready\n", encoding="utf-8")
    try:
        _wait_for(run_dir / "stop", timeout_s)
    except TimeoutError:
        return 2
    finally:
        for producer in reversed(producers):
            producer.stop()
    return 0


def _fetch_chunk(prefix: str, chunk, timeout_s: float):
    from ndnsf import fetch_adaptive_segmented_data_packets

    received: dict[int, bytes] = {}

    def accept(packet) -> None:
        received[int(packet.segment)] = bytes(packet.content)

    metrics = fetch_adaptive_segmented_data_packets(
        f"{prefix}/payload/chunk/{chunk.index}",
        accept,
        timeout_ms=int(timeout_s * 1000),
        initial_window=2,
        maximum_window=16,
        maximum_retries=3,
        persistence_backlog_limit=16,
    )
    payload = b"".join(received[index] for index in sorted(received))
    return payload, metrics


def _open_repo_session(
    run_dir: Path,
    repo_name: str,
    store_id: str,
    operation_id: str,
    *,
    capacity_bytes: int | None = None,
    validation_time_ms: int = 2900,
):
    from ndnsf import fetch_segmented_object
    from py_repoclient import (
        ArtifactReplicaSession,
        HmacReceiptAuthenticator,
        SqliteRepositoryPersistence,
        artifact_upload_lease_from_dict,
        decode_artifact_manifest_page,
        decode_signed_artifact_root,
    )

    (
        config,
        limits,
        artifact,
        chunks,
        capability,
        policy,
        _,
        _,
    ) = _load_fixture(run_dir)
    root_wire = fetch_segmented_object(
        f"{PUBLISHER_ROOT}/root", timeout_ms=10000
    )
    page_wire = fetch_segmented_object(
        f"{PUBLISHER_ROOT}/page/0", timeout_ms=10000
    )
    signed_root = decode_signed_artifact_root(root_wire, limits)
    page = decode_artifact_manifest_page(page_wire, limits)
    persistence = SqliteRepositoryPersistence(
        run_dir / "stores" / store_id / "repo.sqlite3",
        f"spec164-t011-{store_id}-{os.getpid()}",
        capacity_bytes=capacity_bytes,
        reservation_overhead_bytes=0,
    )
    repo_identity = f"/spec164/{repo_name}"
    authenticator = HmacReceiptAuthenticator(
        repo_identity,
        f"{repo_identity}/KEY/receipt-1",
        (run_dir / "receipt.key").read_bytes(),
    )
    lease = artifact_upload_lease_from_dict({
        "leaseId": f"lease-{operation_id}-{repo_name}",
        "operationId": operation_id,
        "repoNode": repo_identity,
        "artifact": config["artifact"],
        "reservedBytes": int(artifact.size_bytes),
        "issuedAtMs": 2500,
        "expiresAtMs": 20000,
        "replayId": f"replay-{operation_id}-{repo_name}",
    }, validation_time_ms)
    session = ArtifactReplicaSession(
        persistence=persistence,
        operation_id=operation_id,
        repo_node=repo_identity,
        generation=1,
        upload_lease=lease,
        lease_validation_time_ms=validation_time_ms,
        artifact=artifact,
        signed_root=signed_root,
        pages=[page],
        chunks=chunks,
        capability=capability,
        trust_policy=policy,
        receipt_authenticator=authenticator,
        limits=limits,
    )
    return (
        config,
        artifact,
        chunks,
        root_wire,
        page_wire,
        persistence,
        session,
    )


def _serve_committed(
    run_dir: Path,
    repo_name: str,
    artifact,
    chunks,
    root_wire: bytes,
    page_wire: bytes,
    session,
    timeout_s: float,
) -> None:
    prefix = f"/spec164/{repo_name}/artifact"
    committed = session.payload_store.committed_path(
        session.identity
    ).read_bytes()
    producers = [
        _packet_producer(
            f"{prefix}/root", root_wire, f"/spec164/{repo_name}"
        ),
        _packet_producer(
            f"{prefix}/page/0", page_wire, f"/spec164/{repo_name}"
        ),
    ]
    for chunk in chunks:
        offset = int(chunk.offset_bytes)
        length = int(chunk.length_bytes)
        producers.append(_packet_producer(
            f"{prefix}/payload/chunk/{chunk.index}",
            committed[offset:offset + length],
            f"/spec164/{repo_name}",
        ))
    (run_dir / f"{repo_name}.active").write_text(
        "active\n", encoding="utf-8"
    )
    try:
        _wait_for(run_dir / "stop", timeout_s)
    finally:
        for producer in reversed(producers):
            producer.stop()


def _repo_role(args: argparse.Namespace) -> int:
    run_dir = args.run_dir
    result_path = _result_path(run_dir, args.result_name)
    result_path.parent.mkdir(parents=True, exist_ok=True)
    capacity = (
        args.capacity_bytes if args.capacity_bytes >= 0 else None
    )
    base_now_ms = 4000 if args.repo_mode == "resume" else 3000
    (
        _,
        artifact,
        chunks,
        root_wire,
        page_wire,
        persistence,
        session,
    ) = _open_repo_session(
        run_dir,
        args.repo_name,
        args.store_id,
        args.operation_id,
        capacity_bytes=capacity,
        validation_time_ms=base_now_ms - 100,
    )
    transferred = 0
    wire_bytes = 0
    try:
        session.reserve(base_now_ms)
        missing_before = session.missing_chunks(base_now_ms + 1)
        selected = (
            missing_before[:1] if args.repo_mode == "first" else missing_before
        )
        for index in selected:
            payload, metrics = _fetch_chunk(
                PUBLISHER_ROOT, chunks[index], args.timeout_seconds
            )
            transferred += len(payload)
            wire_bytes += int(metrics.wire_bytes)
            session.receive_chunk(
                index, payload, now_ms=base_now_ms + 100 + index
            )
        if args.repo_mode == "first":
            _write_json(result_path, {
                "status": "INTERRUPTED",
                "repo": args.repo_name,
                "transferredBytes": transferred,
                "verifiedChunks": (
                    persistence.transfer_session(
                        args.operation_id
                    ).verified_chunks
                ),
                "missingChunks": list(
                    session.missing_chunks(base_now_ms + 150)
                ),
                "active": False,
            })
            return 0
        session.verify_complete(base_now_ms + 200)
        envelope = session.commit_and_activate(base_now_ms + 300)
        record = persistence.transfer_session(args.operation_id)
        _write_json(result_path, {
            "status": "ACTIVE",
            "repo": args.repo_name,
            "transferredBytes": transferred,
            "wireBytes": wire_bytes,
            "verifiedChunks": record.verified_chunks,
            "missingChunks": [],
            "active": True,
            "receipt": envelope.to_dict(),
            "lifecycle": [
                event.to_state
                for event in persistence.lifecycle_events(
                    args.operation_id
                )
                if event.accepted
            ],
        })
        if args.serve:
            _serve_committed(
                run_dir,
                args.repo_name,
                artifact,
                chunks,
                root_wire,
                page_wire,
                session,
                args.timeout_seconds,
            )
        return 0
    except BaseException as exc:
        try:
            session.fail(str(exc), base_now_ms + 400)
        except BaseException:
            pass
        _write_json(result_path, {
            "status": "FAILED",
            "repo": args.repo_name,
            "transferredBytes": transferred,
            "active": False,
            "error": str(exc),
        })
        return 0 if args.expect_failure else 1
    finally:
        persistence.close()


def _lease_expiry_role(args: argparse.Namespace) -> int:
    from ndnsf import fetch_segmented_object
    from py_repoclient import (
        ArtifactReplicaSession,
        HmacReceiptAuthenticator,
        SqliteRepositoryPersistence,
        artifact_upload_lease_from_dict,
        decode_artifact_manifest_page,
        decode_signed_artifact_root,
    )

    run_dir = args.run_dir
    config, limits, artifact, chunks, capability, policy, _, _ = (
        _load_fixture(run_dir)
    )
    root = decode_signed_artifact_root(fetch_segmented_object(
        f"{PUBLISHER_ROOT}/root", timeout_ms=10000
    ), limits)
    page = decode_artifact_manifest_page(fetch_segmented_object(
        f"{PUBLISHER_ROOT}/page/0", timeout_ms=10000
    ), limits)
    persistence = SqliteRepositoryPersistence(
        run_dir / "stores/lease-expiry/repo.sqlite3",
        f"lease-expiry-{os.getpid()}",
        reservation_overhead_bytes=0,
    )
    lease = artifact_upload_lease_from_dict({
        "leaseId": "lease-expiring",
        "operationId": "publication-expiring",
        "repoNode": "/spec164/repo2",
        "artifact": config["artifact"],
        "reservedBytes": int(artifact.size_bytes),
        "issuedAtMs": 2500,
        "expiresAtMs": 3050,
        "replayId": "replay-expiring",
    }, 2900)
    session = ArtifactReplicaSession(
        persistence=persistence,
        operation_id="publication-expiring",
        repo_node="/spec164/repo2",
        generation=1,
        upload_lease=lease,
        lease_validation_time_ms=2900,
        artifact=artifact,
        signed_root=root,
        pages=[page],
        chunks=chunks,
        capability=capability,
        trust_policy=policy,
        receipt_authenticator=HmacReceiptAuthenticator(
            "/spec164/repo2",
            "/spec164/repo2/KEY/receipt-1",
            (run_dir / "receipt.key").read_bytes(),
        ),
        limits=limits,
    )
    session.reserve(3000)
    expired = session.expire(3050)
    error = ""
    try:
        session.missing_chunks(3051)
    except BaseException as exc:
        error = str(exc)
    _write_json(_result_path(run_dir, args.result_name), {
        "status": "LEASE_EXPIRED",
        "expired": expired,
        "workRejected": bool(error),
        "error": error,
        "active": False,
        "transferredBytes": 0,
    })
    persistence.close()
    return 0


def _changed_and_concurrent_role(args: argparse.Namespace) -> int:
    from py_repoclient import (
        ArtifactStorageIdentity,
        LifecycleTransitionError,
    )

    (
        _,
        artifact,
        chunks,
        _,
        _,
        persistence,
        first,
    ) = _open_repo_session(
        args.run_dir,
        "repo2",
        "identity-concurrency",
        "same-digest-a",
    )
    first.reserve(3000)
    transferred = 0
    for chunk in chunks:
        payload, _ = _fetch_chunk(
            PUBLISHER_ROOT, chunk, args.timeout_seconds
        )
        transferred += len(payload)
        first.receive_chunk(int(chunk.index), payload, now_ms=3100)

    # A second overlapping operation sees the same verified CAS bytes and
    # schedules no duplicate network work.
    # One authoritative facade must own the DB, so construct the second session
    # against that same facade rather than opening another authority.
    config, limits, _, _, capability, policy, signed_root, page = _load_fixture(
        args.run_dir
    )
    from py_repoclient import (
        ArtifactReplicaSession,
        HmacReceiptAuthenticator,
        artifact_upload_lease_from_dict,
    )
    lease_b = artifact_upload_lease_from_dict({
        "leaseId": "lease-same-digest-b",
        "operationId": "same-digest-b",
        "repoNode": "/spec164/repo2",
        "artifact": config["artifact"],
        "reservedBytes": int(artifact.size_bytes),
        "issuedAtMs": 2500,
        "expiresAtMs": 20000,
        "replayId": "replay-same-digest-b",
    }, 2900)
    second = ArtifactReplicaSession(
        persistence=persistence,
        operation_id="same-digest-b",
        repo_node="/spec164/repo2",
        generation=1,
        upload_lease=lease_b,
        lease_validation_time_ms=2900,
        artifact=artifact,
        signed_root=signed_root,
        pages=[page],
        chunks=chunks,
        capability=capability,
        trust_policy=policy,
        receipt_authenticator=HmacReceiptAuthenticator(
            "/spec164/repo2",
            "/spec164/repo2/KEY/receipt-1",
            (args.run_dir / "receipt.key").read_bytes(),
        ),
        limits=limits,
    )
    second.reserve(3200)
    same_missing = list(second.missing_chunks(3201))

    prior = persistence.transfer_session("same-digest-a")
    changed = dict(prior.identity)
    changed["manifestRootDigest"] = "f" * 64
    changed_rejected = False
    try:
        persistence.save_transfer_session(
            operation_id=prior.operation_id,
            artifact_digest=prior.artifact_digest,
            generation=prior.generation,
            identity=changed,
            lease=prior.lease,
            state=prior.state,
            preserves_progress=prior.preserves_progress,
            verified_chunks=prior.verified_chunks,
            newly_verified_bytes=prior.newly_verified_bytes,
            avoided_retransmission_bytes=prior.avoided_retransmission_bytes,
            updated_at_ms=prior.updated_at_ms + 1,
        )
    except LifecycleTransitionError:
        changed_rejected = True

    payloads = (b"X" * int(artifact.size_bytes), b"Y" * int(artifact.size_bytes))
    identities = [
        ArtifactStorageIdentity(
            hashlib.sha256(payload).hexdigest(),
            len(payload),
            generation,
        )
        for generation, payload in zip((2, 3), payloads)
    ]
    errors: list[str] = []

    def write_distinct(identity, payload):
        try:
            store = persistence.artifact_payload_store
            store.begin(identity)
            store.write_range(identity, 0, payload)
            store.mark_verified(identity, 0, len(payload))
            if store.read_range(identity, 0, len(payload)) != payload:
                raise RuntimeError("different digest cross-contamination")
        except BaseException as exc:
            errors.append(str(exc))

    threads = [
        threading.Thread(target=write_distinct, args=item)
        for item in zip(identities, payloads)
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for identity in identities:
        persistence.artifact_payload_store.abort(identity)

    _write_json(_result_path(args.run_dir, args.result_name), {
        "status": "PASS" if (
            not same_missing and changed_rejected and not errors
        ) else "FAILED",
        "sameDigestConcurrentOperations": 2,
        "sameDigestLogicalBytes": int(artifact.size_bytes) * 2,
        "sameDigestNetworkBytes": transferred,
        "sameDigestSecondMissingChunks": same_missing,
        "changedIdentityRejected": changed_rejected,
        "differentDigestConcurrentOperations": 2,
        "differentDigestIsolated": not errors,
        "errors": errors,
    })
    first.cancel(preserve_progress=False, now_ms=3500)
    second.cancel(preserve_progress=False, now_ms=3500)
    persistence.close()
    return 0


def _consumer_role(args: argparse.Namespace) -> int:
    from py_repoclient import AtomicArtifactDestination

    config, _, artifact, chunks, _, _, _, _ = _load_fixture(args.run_dir)
    prefix = f"/spec164/{args.repo_name}/artifact"
    destination = args.run_dir / "consumer/artifact.bin"
    sink = AtomicArtifactDestination(
        destination,
        artifact,
        "retrieval-interrupted",
        max_range_bytes=ARTIFACT_CHUNK_BYTES,
    )
    missing = sink.missing_ranges(
        maximum_range_bytes=ARTIFACT_CHUNK_BYTES
    )
    selected = chunks[:1] if args.consumer_mode == "first" else [
        chunk
        for chunk in chunks
        if any(
            offset < int(chunk.offset_bytes) + int(chunk.length_bytes)
            and int(chunk.offset_bytes) < offset + length
            for offset, length in missing
        )
    ]
    transferred = 0
    for chunk in selected:
        payload, metrics = _fetch_chunk(prefix, chunk, args.timeout_seconds)
        sink.write_range(int(chunk.offset_bytes), payload)
        transferred += int(metrics.logical_bytes)
    if args.consumer_mode == "first":
        status = "INTERRUPTED"
        visible = False
    else:
        sink.finalize()
        status = "SUCCESS"
        visible = destination.is_file()
    _write_json(_result_path(args.run_dir, args.result_name), {
        "status": status,
        "transferredBytes": transferred,
        "missingRangesBefore": [list(item) for item in missing],
        "destinationVisible": visible,
        "contentDigest": artifact.content_digest,
    })
    return 0


def _run_process(
    ndn,
    host_name: str,
    role: str,
    run_dir: Path,
    arguments: list[str],
    processes: list,
):
    from minindn.util import getPopen

    log_path = run_dir / "logs" / f"{role}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    stream = log_path.open("wb")
    command = " ".join((
        "exec",
        shlex.quote(sys.executable),
        shlex.quote(str(SCRIPT)),
        "--role",
        shlex.quote(role),
        "--run-dir",
        shlex.quote(str(run_dir)),
        *[shlex.quote(str(value)) for value in arguments],
    ))
    process = getPopen(
        ndn.net[host_name],
        command,
        envDict=_node_environment(host_name),
        shell=True,
        stdout=stream,
        stderr=subprocess.STDOUT,
    )
    processes.append((role, process, stream))
    return process


def _wait_process(process, role: str, timeout_s: float) -> None:
    process.wait(timeout=timeout_s)
    if process.returncode != 0:
        raise RuntimeError(f"{role} exited with {process.returncode}")


def _sanitize(run_dir: Path) -> None:
    for path in (
        run_dir / "receipt.key",
        run_dir / "payload.bin",
        run_dir / "publisher-1.ready",
        run_dir / "publisher-2.ready",
        run_dir / "repo1.active",
        run_dir / "stop",
    ):
        path.unlink(missing_ok=True)
    shutil.rmtree(run_dir / "stores", ignore_errors=True)
    shutil.rmtree(run_dir / "consumer", ignore_errors=True)


def _run_matrix(args: argparse.Namespace) -> dict[str, Any]:
    from mininet.log import setLogLevel
    from mininet.node import Controller
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.nfdc import Nfdc
    from minindn.minindn import Minindn

    run_dir = args.output_dir.resolve()
    payload_size = max(
        2 * ARTIFACT_CHUNK_BYTES,
        min(args.payload_size, 2 * ARTIFACT_CHUNK_BYTES)
        if args.quick_smoke
        else args.payload_size,
    )
    _prepare_fixture(run_dir, payload_size)
    (run_dir / "results").mkdir()
    setLogLevel("warning")
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(
        topoFile=str(args.topology_file.resolve()),
        controller=Controller,
    )
    processes: list[tuple[str, Any, Any]] = []
    started = time.monotonic()
    publisher_restarts = 0
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        time.sleep(0.5)
        publisher = ndn.net["publisher"]
        consumer = ndn.net["consumer"]
        for repo_name in ("repo1", "repo2", "repo3"):
            repo = ndn.net[repo_name]
            publisher_ip = publisher.connectionsTo(repo)[0][0].IP()
            Nfdc.createFace(repo, publisher_ip)
            Nfdc.registerRoute(
                repo, "/spec164/publisher", publisher_ip, cost=0
            )
            repo_ip = repo.connectionsTo(consumer)[0][0].IP()
            Nfdc.createFace(consumer, repo_ip)
            Nfdc.registerRoute(
                consumer, f"/spec164/{repo_name}", repo_ip, cost=0
            )

        producer = _run_process(
            ndn,
            "publisher",
            "producer-1",
            run_dir,
            [
                "--ready-name", "publisher-1.ready",
                "--timeout-seconds", str(args.timeout_seconds),
            ],
            processes,
        )
        _wait_for(run_dir / "publisher-1.ready", args.timeout_seconds)

        operation_id = "publication-main"
        repo_first = _run_process(
            ndn,
            "repo1",
            "repo-first",
            run_dir,
            [
                "--repo-name", "repo1",
                "--store-id", "repo1-main",
                "--operation-id", operation_id,
                "--repo-mode", "first",
                "--result-name", "repo-first",
                "--timeout-seconds", str(args.timeout_seconds),
            ],
            processes,
        )
        _wait_process(repo_first, "repo-first", args.timeout_seconds)

        producer.terminate()
        producer.wait(timeout=2)
        publisher_restarts += 1
        producer = _run_process(
            ndn,
            "publisher",
            "producer-2",
            run_dir,
            [
                "--ready-name", "publisher-2.ready",
                "--timeout-seconds", str(args.timeout_seconds),
            ],
            processes,
        )
        _wait_for(run_dir / "publisher-2.ready", args.timeout_seconds)

        repo_resume = _run_process(
            ndn,
            "repo1",
            "repo-resume",
            run_dir,
            [
                "--repo-name", "repo1",
                "--store-id", "repo1-main",
                "--operation-id", operation_id,
                "--repo-mode", "resume",
                "--result-name", "repo-resume",
                "--serve",
                "--timeout-seconds", str(args.timeout_seconds),
            ],
            processes,
        )
        _wait_for(run_dir / "repo1.active", args.timeout_seconds)

        consumer_first = _run_process(
            ndn,
            "consumer",
            "consumer-first",
            run_dir,
            [
                "--repo-name", "repo1",
                "--consumer-mode", "first",
                "--result-name", "consumer-first",
                "--timeout-seconds", str(args.timeout_seconds),
            ],
            processes,
        )
        _wait_process(
            consumer_first, "consumer-first", args.timeout_seconds
        )
        consumer_resume = _run_process(
            ndn,
            "consumer",
            "consumer-resume",
            run_dir,
            [
                "--repo-name", "repo1",
                "--consumer-mode", "resume",
                "--result-name", "consumer-resume",
                "--timeout-seconds", str(args.timeout_seconds),
            ],
            processes,
        )
        _wait_process(
            consumer_resume, "consumer-resume", args.timeout_seconds
        )

        if not args.quick_smoke:
            lease = _run_process(
                ndn,
                "repo2",
                "lease-expiry",
                run_dir,
                [
                    "--result-name", "lease-expiry",
                    "--timeout-seconds", str(args.timeout_seconds),
                ],
                processes,
            )
            _wait_process(lease, "lease-expiry", args.timeout_seconds)
            concurrency = _run_process(
                ndn,
                "repo2",
                "identity-concurrency",
                run_dir,
                [
                    "--result-name", "identity-concurrency",
                    "--timeout-seconds", str(args.timeout_seconds),
                ],
                processes,
            )
            _wait_process(
                concurrency, "identity-concurrency", args.timeout_seconds
            )
            repo2 = _run_process(
                ndn,
                "repo2",
                "repo2-full",
                run_dir,
                [
                    "--repo-name", "repo2",
                    "--store-id", "repo2-main",
                    "--operation-id", operation_id,
                    "--repo-mode", "full",
                    "--result-name", "repo2",
                    "--timeout-seconds", str(args.timeout_seconds),
                ],
                processes,
            )
            repo3 = _run_process(
                ndn,
                "repo3",
                "repo3-low-space",
                run_dir,
                [
                    "--repo-name", "repo3",
                    "--store-id", "repo3-main",
                    "--operation-id", operation_id,
                    "--repo-mode", "full",
                    "--result-name", "repo3",
                    "--capacity-bytes", str(payload_size - 1),
                    "--expect-failure",
                    "--timeout-seconds", str(args.timeout_seconds),
                ],
                processes,
            )
            _wait_process(repo2, "repo2-full", args.timeout_seconds)
            _wait_process(repo3, "repo3-low-space", args.timeout_seconds)

        first = json.loads(
            _result_path(run_dir, "repo-first").read_text(encoding="utf-8")
        )
        resumed = json.loads(
            _result_path(run_dir, "repo-resume").read_text(encoding="utf-8")
        )
        consumer_one = json.loads(
            _result_path(run_dir, "consumer-first").read_text(encoding="utf-8")
        )
        consumer_two = json.loads(
            _result_path(run_dir, "consumer-resume").read_text(encoding="utf-8")
        )
        expected_first = int(
            json.loads(
                (run_dir / "fixture.json").read_text(encoding="utf-8")
            )["chunks"][0]["lengthBytes"]
        )
        interruption = {
            "case": "publisher-repository-consumer-interruption",
            "verdict": "PASS" if (
                publisher_restarts == 1
                and first["status"] == "INTERRUPTED"
                and first["transferredBytes"] == expected_first
                and resumed["status"] == "ACTIVE"
                and resumed["transferredBytes"]
                == payload_size - expected_first
                and consumer_one["transferredBytes"] == expected_first
                and consumer_two["transferredBytes"]
                == payload_size - expected_first
                and consumer_two["destinationVisible"]
            ) else "BLOCK",
            "publisherRestartCount": publisher_restarts,
            "repositoryProcessCount": 2,
            "consumerProcessCount": 2,
            "publicationFirstBytes": first["transferredBytes"],
            "publicationResumeBytes": resumed["transferredBytes"],
            "retrievalFirstBytes": consumer_one["transferredBytes"],
            "retrievalResumeBytes": consumer_two["transferredBytes"],
            "finalState": resumed["status"],
            "destinationVisible": consumer_two["destinationVisible"],
        }
        cases = [interruption]
        if not args.quick_smoke:
            lease_result = json.loads(
                _result_path(run_dir, "lease-expiry").read_text()
            )
            identity = json.loads(
                _result_path(run_dir, "identity-concurrency").read_text()
            )
            repo2_result = json.loads(
                _result_path(run_dir, "repo2").read_text()
            )
            repo3_result = json.loads(
                _result_path(run_dir, "repo3").read_text()
            )
            receipts = [
                value["receipt"]["receipt"]["receiptId"]
                for value in (resumed, repo2_result)
                if value.get("status") == "ACTIVE"
            ]
            cases.extend([
                {
                    "case": "lease-expiry",
                    "verdict": "PASS" if (
                        lease_result["status"] == "LEASE_EXPIRED"
                        and lease_result["workRejected"]
                        and not lease_result["active"]
                    ) else "BLOCK",
                    **lease_result,
                },
                {
                    "case": "changed-identity-and-concurrency",
                    "verdict": "PASS" if identity["status"] == "PASS"
                    else "BLOCK",
                    **identity,
                },
                {
                    "case": "three-replica-partial-commit",
                    "verdict": "PASS" if (
                        len(set(receipts)) == 2
                        and repo3_result["status"] == "FAILED"
                        and not repo3_result["active"]
                    ) else "BLOCK",
                    "requestedReplicas": 3,
                    "achievedReplicas": len(set(receipts)),
                    "distinctReceiptIds": sorted(set(receipts)),
                    "replicaStates": {
                        "repo1": resumed["status"],
                        "repo2": repo2_result["status"],
                        "repo3": repo3_result["status"],
                    },
                    "repo3Error": repo3_result.get("error", ""),
                },
            ])
        summary = {
            "schema": "ndnsf-repo-spec164-recovery-minindn-v1",
            "materialPassport": {
                "originSkill": "experiment-agent",
                "originMode": "run",
                "originDate": time.strftime("%Y-%m-%d", time.gmtime()),
                "verificationStatus": "VERIFIED",
                "versionLabel": "spec164_t011_recovery_v1",
            },
            "verdict": "PASS" if all(
                case["verdict"] == "PASS" for case in cases
            ) else "BLOCK",
            "topology": str(args.topology_file.resolve()),
            "payloadSize": payload_size,
            "chunkBytes": ARTIFACT_CHUNK_BYTES,
            "cases": cases,
            "elapsedMs": round((time.monotonic() - started) * 1000, 3),
            "quickSmoke": bool(args.quick_smoke),
            "performanceClaim": False,
        }
        _write_json(run_dir / "summary.json", summary)
        return summary
    finally:
        try:
            (run_dir / "stop").write_text("stop\n", encoding="utf-8")
        except OSError:
            pass
        for _, process, _ in processes:
            if process.poll() is None:
                process.terminate()
        for _, process, stream in processes:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
            stream.close()
        ndn.stop()
        Minindn.cleanUp()


def _parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=(
        "producer-1",
        "producer-2",
        "repo-first",
        "repo-resume",
        "repo2-full",
        "repo3-low-space",
        "lease-expiry",
        "identity-concurrency",
        "consumer-first",
        "consumer-resume",
    ))
    parser.add_argument("--run-dir", type=Path)
    parser.add_argument("--repo-name", default="repo1")
    parser.add_argument("--store-id", default="repo")
    parser.add_argument("--operation-id", default="publication-main")
    parser.add_argument("--repo-mode", choices=("first", "resume", "full"))
    parser.add_argument("--consumer-mode", choices=("first", "resume"))
    parser.add_argument("--result-name", default="result")
    parser.add_argument("--ready-name", default="publisher.ready")
    parser.add_argument("--capacity-bytes", type=int, default=-1)
    parser.add_argument("--expect-failure", action="store_true")
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    parser.add_argument("--topology-file", type=Path, default=DEFAULT_TOPOLOGY)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--payload-size", type=int, default=64 * 1024)
    parser.add_argument("--quick-smoke", action="store_true")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = _parse_args(argv)
    if args.role:
        if args.run_dir is None:
            raise SystemExit("--run-dir is required for roles")
        if args.role.startswith("producer"):
            return _producer_role(
                args.run_dir, args.ready_name, args.timeout_seconds
            )
        if args.role.startswith("consumer"):
            return _consumer_role(args)
        if args.role == "lease-expiry":
            return _lease_expiry_role(args)
        if args.role == "identity-concurrency":
            return _changed_and_concurrent_role(args)
        return _repo_role(args)

    sys.argv = [sys.argv[0]]
    if args.output_dir is None:
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        args.output_dir = DEFAULT_EVIDENCE_ROOT / (
            ("minindn-recovery-smoke-" if args.quick_smoke
             else "minindn-recovery-") + stamp
        )
    args.output_dir = args.output_dir.resolve()
    if args.output_dir.exists():
        raise SystemExit(
            f"output directory already exists: {args.output_dir}"
        )
    args.output_dir.parent.mkdir(parents=True, exist_ok=True)
    summary = _run_matrix(args)
    _sanitize(args.output_dir)
    _restore_output_ownership(args.output_dir)
    print(json.dumps(summary, sort_keys=True))
    if args.quick_smoke and summary["verdict"] == "PASS":
        print("SPEC164_ARTIFACT_RECOVERY_MININDN_SMOKE_OK")
    return 0 if summary["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
