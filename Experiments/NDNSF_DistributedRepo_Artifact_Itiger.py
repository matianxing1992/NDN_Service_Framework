#!/usr/bin/env python3
"""Portable two-node roles for the Spec 167 TigerCluster artifact campaign."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import random
import socket
import sys
import time
from typing import Any

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))
sys.path.insert(0, str(REPO / "pythonWrapper"))
sys.path.insert(0, str(REPO / "NDNSF-DistributedRepo/pythonWrapper"))

from NDNSF_DistributedRepo_Artifact_Minindn import (  # noqa: E402
    _benchmark_cold_consumer_role,
    _benchmark_consumer_role,
    _benchmark_producer_role,
    _prepare_raw_payload,
    _write_json,
)

SUBJECTS = (
    "physical-network",
    "raw-segmented-ndn",
    "legacy-exact-packet",
    "digest-only",
    "signed-manifest",
)
REPOSITORY_SUBJECTS = SUBJECTS[2:]
PAYLOAD_SIZES = (64 << 20, 1 << 30)
REPETITIONS = 5
SHARED_PATH_ROOTS = (Path("/project"), Path("/home"))


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")


def validate_path_isolation(coord_dir: Path, data_dir: Path) -> tuple[Path, Path]:
    coord = coord_dir.resolve()
    data = data_dir.resolve()
    if coord == data:
        raise ValueError("coord-dir and data-dir must be different")
    for root in SHARED_PATH_ROOTS:
        if data == root or root in data.parents:
            raise ValueError(f"measured data-dir must be rank-local, not {root}")
    return coord, data


def build_schedule(seed: int = 16720260731) -> list[dict[str, Any]]:
    schedule: list[dict[str, Any]] = []
    for repetition in range(0, REPETITIONS + 1):
        block = [
            {
                "sizeBytes": size,
                "subject": subject,
                "repetition": repetition,
                "warmup": repetition == 0,
                "pairId": f"s{size}-rep{repetition}",
                "runId": f"s{size}-{subject}-rep{repetition}",
            }
            for size in PAYLOAD_SIZES
            for subject in SUBJECTS
        ]
        random.Random(seed + repetition).shuffle(block)
        for order, cell in enumerate(block):
            cell["orderInBlock"] = order
            schedule.append(cell)
    return schedule


def create_manifest(campaign_id: str, seed: int) -> dict[str, Any]:
    schedule = build_schedule(seed)
    return {
        "schemaVersion": 1,
        "campaignId": campaign_id,
        "subjects": list(SUBJECTS),
        "payloadSizes": list(PAYLOAD_SIZES),
        "replicas": 1,
        "concurrency": 1,
        "warmupsPerCell": 1,
        "measuredRepetitionsPerCell": REPETITIONS,
        "minimumMeasurementSeconds": 60,
        "randomizationSeed": seed,
        "schedule": schedule,
    }


def append_progress(path: Path, value: dict[str, Any]) -> None:
    payload = canonical_json_bytes(value) + b"\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        if os.write(fd, payload) != len(payload):
            raise OSError("short atomic progress write")
        os.fsync(fd)
    finally:
        os.close(fd)


def tcp_ceiling_server(
    coord_dir: Path, bind_host: str, port: int, timeout_s: float
) -> int:
    coord_dir.mkdir(parents=True, exist_ok=True)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    server.settimeout(timeout_s)
    server.bind((bind_host, port))
    server.listen(1)
    (coord_dir / "tcp-server.ready").write_text("ready\n", encoding="utf-8")
    received = 0
    started = 0.0
    try:
        connection, _ = server.accept()
        with connection:
            connection.settimeout(timeout_s)
            started = time.monotonic()
            while True:
                block = connection.recv(1 << 20)
                if not block:
                    break
                received += len(block)
    finally:
        server.close()
    elapsed = time.monotonic() - started if started else 0.0
    success = received > 0 and elapsed > 0
    _write_json(coord_dir / "tcp-result.json", {
        "schemaVersion": 1,
        "status": "SUCCESS" if success else "FAIL",
        "logicalBytes": received,
        "elapsedMs": elapsed * 1000.0,
        "logicalGoodputMbps": received * 8.0 / elapsed / 1_000_000.0 if elapsed else 0.0,
        "bindHost": bind_host,
        "port": port,
    })
    return 0 if success else 1


def tcp_ceiling_client(
    coord_dir: Path, peer_host: str, port: int, duration_s: float, timeout_s: float
) -> int:
    deadline = time.monotonic() + timeout_s
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
    while True:
        try:
            client.connect((peer_host, port))
            break
        except (ConnectionRefusedError, TimeoutError, OSError):
            if time.monotonic() >= deadline:
                client.close()
                raise TimeoutError("TCP ceiling server did not become reachable")
            time.sleep(0.1)
    block = bytes(1 << 20)
    sent = 0
    started = time.monotonic()
    stop_at = started + duration_s
    try:
        while time.monotonic() < stop_at:
            client.sendall(block)
            sent += len(block)
        client.shutdown(socket.SHUT_WR)
    finally:
        client.close()
    _write_json(coord_dir / "tcp-client.json", {
        "schemaVersion": 1,
        "status": "SUCCESS",
        "logicalBytes": sent,
        "elapsedMs": (time.monotonic() - started) * 1000.0,
        "peerHost": peer_host,
        "port": port,
    })
    return 0


def control_receive(
    coord_dir: Path, bind_host: str, port: int, timeout_s: float
) -> int:
    """Receive a bounded checksum-independent control bundle over the data LAN."""
    coord_dir.mkdir(parents=True, exist_ok=True)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.settimeout(timeout_s)
    server.bind((bind_host, port))
    server.listen(1)
    payload = bytearray()
    try:
        connection, _ = server.accept()
        with connection:
            connection.settimeout(timeout_s)
            while True:
                block = connection.recv(1 << 16)
                if not block:
                    break
                payload.extend(block)
                if len(payload) > 8 << 20:
                    raise ValueError("control bundle exceeds 8 MiB")
    finally:
        server.close()
    message = json.loads(payload)
    files = message.get("files")
    if not isinstance(files, dict) or not files:
        raise ValueError("control bundle has no files")
    for name, encoded in files.items():
        if Path(name).name != name or name in (".", ".."):
            raise ValueError(f"unsafe control file name: {name!r}")
        raw = base64.b64decode(encoded, validate=True)
        target = coord_dir / name
        if target.exists():
            raise FileExistsError(target)
        target.write_bytes(raw)
    return 0


def control_send(
    coord_dir: Path,
    peer_host: str,
    port: int,
    timeout_s: float,
    file_names: tuple[str, ...],
) -> int:
    files: dict[str, str] = {}
    for name in file_names:
        if Path(name).name != name or name in (".", ".."):
            raise ValueError(f"unsafe control file name: {name!r}")
        files[name] = base64.b64encode((coord_dir / name).read_bytes()).decode()
    payload = canonical_json_bytes({"schemaVersion": 1, "files": files})
    deadline = time.monotonic() + timeout_s
    while True:
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.settimeout(min(timeout_s, 5.0))
        try:
            client.connect((peer_host, port))
            client.sendall(payload)
            client.shutdown(socket.SHUT_WR)
            client.close()
            return 0
        except (ConnectionRefusedError, TimeoutError, OSError):
            client.close()
            if time.monotonic() >= deadline:
                raise TimeoutError("control receiver did not become reachable")
            time.sleep(0.1)


def prepare_payload(coord_dir: Path, data_dir: Path, payload_size: int) -> str:
    coord, data = validate_path_isolation(coord_dir, data_dir)
    return _prepare_raw_payload(coord, payload_size, data)


def warm_reuse(
    coord_dir: Path, data_dir: Path, result_name: str, expected_digest: str
) -> int:
    coord, data = validate_path_isolation(coord_dir, data_dir)
    payload = data / "stores" / result_name / "payload.bin"
    if not payload.is_file():
        raise FileNotFoundError(f"warm reuse payload missing: {payload}")
    started = time.monotonic()
    digest = hashlib.sha256()
    with payload.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    observed = digest.hexdigest()
    elapsed_ms = (time.monotonic() - started) * 1000.0
    success = observed == expected_digest
    _write_json(coord / f"{result_name}.warm.json", {
        "schemaVersion": 1,
        "status": "SUCCESS" if success else "FAIL",
        "cacheState": "warm-content-addressed-reuse",
        "payloadBytes": payload.stat().st_size,
        "duplicatePayloadBytesWritten": 0,
        "elapsedMs": elapsed_ms,
        "expectedContentDigest": expected_digest,
        "observedContentDigest": observed,
        "payloadPath": str(payload),
    })
    return 0 if success else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--role",
        required=True,
        choices=("manifest", "prepare", "producer", "consumer",
                 "cold-consumer", "warm-reuse", "tcp-server", "tcp-client",
                 "control-receive", "control-send"),
    )
    parser.add_argument("--coord-dir", type=Path)
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--campaign-id", default="")
    parser.add_argument("--seed", type=int, default=16720260731)
    parser.add_argument("--payload-size", type=int, choices=PAYLOAD_SIZES)
    parser.add_argument(
        "--subject", choices=SUBJECTS[1:], default="digest-only"
    )
    parser.add_argument("--result-name", default="result")
    parser.add_argument("--expected-digest", default="")
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=26369)
    parser.add_argument("--duration-seconds", type=float, default=60.0)
    parser.add_argument("--file-names", default="")
    parser.add_argument("--object-prefix", default="/spec164/raw")
    parser.add_argument("--cold-prefix", default="")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.role == "manifest":
        if not args.campaign_id or args.output is None:
            raise SystemExit("manifest requires --campaign-id and --output")
        manifest = create_manifest(args.campaign_id, args.seed)
        _write_json(args.output, manifest)
        print(hashlib.sha256(canonical_json_bytes(manifest)).hexdigest())
        return 0
    if args.coord_dir is None:
        raise SystemExit("role requires --coord-dir")
    if args.role == "tcp-server":
        return tcp_ceiling_server(
            args.coord_dir, args.host, args.port, args.timeout_seconds
        )
    if args.role == "tcp-client":
        return tcp_ceiling_client(
            args.coord_dir, args.host, args.port,
            args.duration_seconds, args.timeout_seconds,
        )
    if args.role == "control-receive":
        return control_receive(
            args.coord_dir, args.host, args.port, args.timeout_seconds
        )
    if args.role == "control-send":
        names = tuple(name for name in args.file_names.split(",") if name)
        if not names:
            raise SystemExit("control-send requires --file-names")
        return control_send(
            args.coord_dir, args.host, args.port, args.timeout_seconds, names
        )
    if args.coord_dir is None or args.data_dir is None:
        raise SystemExit("role requires --coord-dir and --data-dir")
    coord, data = validate_path_isolation(args.coord_dir, args.data_dir)
    if args.role == "prepare":
        if args.payload_size is None:
            raise SystemExit("prepare requires --payload-size")
        print(prepare_payload(coord, data, args.payload_size))
        return 0
    if args.role == "producer":
        return _benchmark_producer_role(
            coord, args.subject, args.timeout_seconds, data, args.object_prefix
        )
    if args.role == "consumer":
        return _benchmark_consumer_role(
            coord, args.subject, args.timeout_seconds, args.result_name, data,
            args.object_prefix, args.cold_prefix or None,
        )
    if args.role == "cold-consumer":
        return _benchmark_cold_consumer_role(
            coord, args.timeout_seconds, args.result_name, data
        )
    if not args.expected_digest:
        raise SystemExit("warm-reuse requires --expected-digest")
    return warm_reuse(coord, data, args.result_name, args.expected_digest)


if __name__ == "__main__":
    raise SystemExit(main())
