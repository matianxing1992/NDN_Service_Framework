#!/usr/bin/env python3
"""Frozen campaign and measurement primitives for Spec 164.

This module intentionally contains no MiniNDN orchestration.  It owns the
experiment contract shared by physical, raw-NDN, and repository subjects:
factor expansion, immutable manifests, append-only run records, stable trace
sampling, process resource sampling, and iperf2 result parsing.
"""

from __future__ import annotations

from contextlib import AbstractContextManager
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import random
import socket
import subprocess
from typing import Any, Iterable, Mapping, Optional


CAMPAIGN_SCHEMA_VERSION = 2
RUN_SCHEMA_VERSION = 2
SAMPLE_SCHEMA_VERSION = 2
SUBJECTS = (
    "physical-network",
    "raw-segmented-ndn",
    "legacy-exact-packet",
    "digest-only",
    "signed-manifest",
)
REPOSITORY_SUBJECTS = SUBJECTS[2:]
PAYLOAD_SIZES = (1 << 20, 64 << 20, 1 << 30, 16 << 30)
REPLICA_COUNTS = (1, 3)
CONCURRENCY_LEVELS = (1, 4, 16)
REPETITIONS = 5
PACKET_PAYLOAD_BYTES = 4096
MEASUREMENT_WINDOW_SECONDS = 60
PHASES = (
    "discovery",
    "ackCollection",
    "planning",
    "queueWait",
    "sessionStart",
    "transfer",
    "verification",
    "persistence",
    "replication",
    "commit",
    "activation",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")


def sha256_hex(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def stable_sample(operation_id: str, rate: float) -> bool:
    """Return a process-independent sampling decision for an operation ID."""

    if not 0.0 <= rate <= 1.0:
        raise ValueError("sample rate must be between 0 and 1")
    if rate == 0.0:
        return False
    if rate == 1.0:
        return True
    value = int.from_bytes(
        hashlib.sha256(operation_id.encode("utf-8")).digest()[:8], "big"
    )
    return value < int(rate * (1 << 64))


def validate_measurement_window(seconds: float, quick_smoke: bool) -> None:
    if seconds <= 0:
        raise ValueError("measurement window must be positive")
    if not quick_smoke and seconds < MEASUREMENT_WINDOW_SECONDS:
        raise ValueError(
            "formal rate-over-time measurements require at least 60 seconds"
        )


def build_cells(
    *,
    payload_sizes: Iterable[int] = PAYLOAD_SIZES,
    replica_counts: Iterable[int] = REPLICA_COUNTS,
    concurrency_levels: Iterable[int] = CONCURRENCY_LEVELS,
) -> list[dict[str, Any]]:
    """Expand matched cells with an explicit raw-NDN pair for every repo cell."""

    cells: list[dict[str, Any]] = []
    for size in payload_sizes:
        if int(size) <= 0:
            raise ValueError("payload sizes must be positive")
        for replicas in replica_counts:
            if int(replicas) not in REPLICA_COUNTS:
                raise ValueError("replica count is outside the frozen matrix")
            for concurrency in concurrency_levels:
                if int(concurrency) not in CONCURRENCY_LEVELS:
                    raise ValueError("concurrency is outside the frozen matrix")
                pair_id = f"s{int(size)}-r{int(replicas)}-c{int(concurrency)}"
                raw_id = f"{pair_id}-raw"
                cells.append({
                    "cellId": raw_id,
                    "pairId": pair_id,
                    "subject": "raw-segmented-ndn",
                    "payloadBytes": int(size),
                    "replicas": int(replicas),
                    "concurrency": int(concurrency),
                    "pairedRawCellId": None,
                })
                for subject in REPOSITORY_SUBJECTS:
                    cells.append({
                        "cellId": f"{pair_id}-{subject}",
                        "pairId": pair_id,
                        "subject": subject,
                        "payloadBytes": int(size),
                        "replicas": int(replicas),
                        "concurrency": int(concurrency),
                        "pairedRawCellId": raw_id,
                    })
    return cells


def build_run_schedule(
    cells: Iterable[Mapping[str, Any]],
    *,
    repetitions: int = REPETITIONS,
    randomization_seed: int,
) -> list[dict[str, Any]]:
    """Build one warmup plus measured randomized matched blocks."""

    if repetitions < 1:
        raise ValueError("repetitions must be positive")
    grouped: dict[str, list[dict[str, Any]]] = {}
    for value in cells:
        cell = dict(value)
        grouped.setdefault(str(cell["pairId"]), []).append(cell)
    schedule: list[dict[str, Any]] = []
    generator = random.Random(int(randomization_seed))
    for repetition in range(-1, repetitions):
        pair_ids = sorted(grouped)
        generator.shuffle(pair_ids)
        for block_index, pair_id in enumerate(pair_ids):
            block = list(grouped[pair_id])
            generator.shuffle(block)
            for order_in_block, cell in enumerate(block):
                schedule.append({
                    "runId": (
                        f"{cell['cellId']}-"
                        f"{'warmup' if repetition < 0 else f'rep{repetition + 1}'}"
                    ),
                    "cellId": cell["cellId"],
                    "pairId": pair_id,
                    "subject": cell["subject"],
                    "repetition": 0 if repetition < 0 else repetition + 1,
                    "warmup": repetition < 0,
                    "blockIndex": block_index,
                    "orderInBlock": order_in_block,
                })
    return schedule


class CampaignLock(AbstractContextManager):
    """Process-level single-writer lock retained for the full campaign update."""

    def __init__(self, campaign_dir: Path):
        self._path = Path(campaign_dir) / ".campaign.lock"
        self._stream = None

    def __enter__(self) -> "CampaignLock":
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self._path.open("a+", encoding="utf-8")
        try:
            fcntl.flock(self._stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            self._stream.close()
            self._stream = None
            raise RuntimeError("campaign already has an active writer") from error
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if self._stream is not None:
            fcntl.flock(self._stream.fileno(), fcntl.LOCK_UN)
            self._stream.close()
            self._stream = None


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _command_output(command: list[str], cwd: Path) -> str:
    try:
        result = subprocess.run(
            command, cwd=cwd, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False, timeout=20
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"
    return result.stdout.strip()


def material_passport(repo_root: Path) -> dict[str, Any]:
    """Capture immutable environment/source facts without mutating the tree."""

    repo_root = Path(repo_root).resolve()
    return {
        "capturedAt": utc_now(),
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "kernel": platform.release(),
        "gitHead": _command_output(["git", "rev-parse", "HEAD"], repo_root),
        "gitStatusPorcelainV1": _command_output(
            ["git", "status", "--porcelain=v1"], repo_root
        ).splitlines(),
        "sourceHashes": {
            str(path.relative_to(repo_root)): sha256_hex(path.read_bytes())
            for path in (
                repo_root / "Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py",
                repo_root / "Experiments/spec164_artifact_campaign.py",
                repo_root / "Experiments/Topology/spec164-artifact-linear.conf",
            )
            if path.is_file()
        },
    }


def create_campaign_manifest(
    *,
    campaign_id: str,
    repo_root: Path,
    topology: Mapping[str, Any],
    admissibility: Mapping[str, Any],
    cells: list[dict[str, Any]] | None = None,
    repetitions: int = REPETITIONS,
    timeline_sample_rate: float = 0.01,
    quick_smoke: bool = False,
    measurement_window_seconds: float = MEASUREMENT_WINDOW_SECONDS,
    randomization_seed: Optional[int] = None,
) -> dict[str, Any]:
    validate_measurement_window(measurement_window_seconds, quick_smoke)
    if repetitions < 1:
        raise ValueError("repetitions must be positive")
    if not 0.0 <= timeline_sample_rate <= 1.0:
        raise ValueError("timeline sample rate must be between 0 and 1")
    selected_cells = list(cells if cells is not None else build_cells())
    seed = (
        int(randomization_seed)
        if randomization_seed is not None
        else int.from_bytes(hashlib.sha256(campaign_id.encode()).digest()[:8], "big")
    )
    return {
        "schemaVersion": CAMPAIGN_SCHEMA_VERSION,
        "campaignId": campaign_id,
        "createdAt": utc_now(),
        "quickSmoke": bool(quick_smoke),
        "performanceClaim": False if quick_smoke else True,
        "measurementWindowSeconds": float(measurement_window_seconds),
        "repetitions": int(repetitions),
        "packetPayloadBytes": PACKET_PAYLOAD_BYTES,
        "timelineSampleRate": float(timeline_sample_rate),
        "subjects": list(SUBJECTS),
        "phases": list(PHASES),
        "topology": dict(topology),
        "admissibility": dict(admissibility),
        "cells": selected_cells,
        "randomizationSeed": seed,
        "runSchedule": build_run_schedule(
            selected_cells, repetitions=repetitions, randomization_seed=seed
        ),
        "materialPassport": material_passport(repo_root),
    }


def freeze_campaign(campaign_dir: Path, manifest: Mapping[str, Any]) -> str:
    """Create manifest + detached seal exactly once."""

    campaign_dir = Path(campaign_dir)
    manifest_path = campaign_dir / "campaign-manifest.json"
    seal_path = campaign_dir / "campaign-manifest.sha256"
    with CampaignLock(campaign_dir):
        if manifest_path.exists() or seal_path.exists():
            raise FileExistsError("campaign manifest is immutable once frozen")
        encoded = canonical_json_bytes(dict(manifest)) + b"\n"
        digest = sha256_hex(encoded)
        _atomic_write(manifest_path, encoded)
        _atomic_write(seal_path, f"{digest}  campaign-manifest.json\n".encode())
    return digest


def load_frozen_campaign(campaign_dir: Path) -> dict[str, Any]:
    campaign_dir = Path(campaign_dir)
    manifest_path = campaign_dir / "campaign-manifest.json"
    seal_path = campaign_dir / "campaign-manifest.sha256"
    encoded = manifest_path.read_bytes()
    expected = seal_path.read_text(encoding="utf-8").split()[0]
    actual = sha256_hex(encoded)
    if actual != expected:
        raise RuntimeError("campaign manifest seal mismatch")
    return json.loads(encoded)


RUN_CSV_FIELDS = (
    "schemaVersion", "campaignId", "runId", "cellId", "pairId", "subject",
    "repetition", "warmup", "admissible", "verdict", "payloadBytes",
    "replicas", "concurrency", "elapsedMs", "logicalGoodputMbps",
    "wireGoodputMbps", "cpuUserSeconds", "cpuSystemSeconds", "peakRssBytes",
    "logicalBytes", "dataWireBytes", "interestWireBytes", "wireBytes",
    "retransmittedBytes", "payloadStoreBytesRead", "payloadStoreBytesWritten",
    "metadataStoreBytesRead", "metadataStoreBytesWritten", "storageBytesRead",
    "storageBytesWritten", "coldRetrievalElapsedMs",
    "coldRetrievalLogicalGoodputMbps", "coldRetrievalDataWireBytes",
    "coldRetrievalInterestWireBytes", "coldRetrievalWireBytes",
    "coldDestinationVisible", "interestCount", "dataCount", "timeoutCount",
    "retransmissionCount", "windowMinimum", "windowMaximum",
    "asymmetricVerifyCount", "asymmetricVerifyMs", "digestVerifyCount",
    "digestVerifyMs", "metadataOperations", "metadataRecords",
    "requestedReplicas", "selectedReplicas", "committedReplicas",
    "readAmplification", "writeAmplification", "failureReason",
)


def validate_run_record(record: Mapping[str, Any]) -> None:
    missing = [field for field in RUN_CSV_FIELDS if field not in record]
    if missing:
        raise ValueError(f"run record missing fields: {', '.join(missing)}")
    if record["subject"] not in SUBJECTS:
        raise ValueError("run record has unknown subject")
    if record["verdict"] not in ("PASS", "FAIL", "INADMISSIBLE"):
        raise ValueError("run verdict must be PASS, FAIL, or INADMISSIBLE")
    if bool(record["warmup"]) and bool(record.get("performanceClaim", False)):
        raise ValueError("warmup runs cannot support a performance claim")
    byte_fields = (
        "dataWireBytes", "interestWireBytes", "wireBytes",
        "payloadStoreBytesRead", "payloadStoreBytesWritten",
        "metadataStoreBytesRead", "metadataStoreBytesWritten",
        "storageBytesRead", "storageBytesWritten",
        "coldRetrievalDataWireBytes", "coldRetrievalInterestWireBytes",
        "coldRetrievalWireBytes",
    )
    if any(
            isinstance(record[field], bool) or int(record[field]) != record[field]
            or int(record[field]) < 0
            for field in byte_fields):
        raise ValueError("run record byte counters must be non-negative integers")
    if int(record["wireBytes"]) != (
            int(record["dataWireBytes"]) + int(record["interestWireBytes"])):
        raise ValueError("run record wire byte counters are inconsistent")
    if int(record["storageBytesRead"]) != (
            int(record["payloadStoreBytesRead"])
            + int(record["metadataStoreBytesRead"])):
        raise ValueError("run record storage read counters are inconsistent")
    if int(record["storageBytesWritten"]) != (
            int(record["payloadStoreBytesWritten"])
            + int(record["metadataStoreBytesWritten"])):
        raise ValueError("run record storage write counters are inconsistent")
    if int(record["coldRetrievalWireBytes"]) != (
            int(record["coldRetrievalDataWireBytes"])
            + int(record["coldRetrievalInterestWireBytes"])):
        raise ValueError("run record cold wire byte counters are inconsistent")
    if (record["subject"] != "physical-network"
            and record["verdict"] == "PASS"
            and not bool(record["coldDestinationVisible"])):
        raise ValueError("successful transfer lacks cold destination visibility")


def append_run_record(campaign_dir: Path, record: Mapping[str, Any]) -> None:
    """Append a validated run to JSONL and CSV without permitting replacement."""

    validate_run_record(record)
    campaign_dir = Path(campaign_dir)
    manifest = load_frozen_campaign(campaign_dir)
    if record["campaignId"] != manifest["campaignId"]:
        raise ValueError("run campaignId does not match frozen manifest")
    jsonl_path = campaign_dir / "campaign-runs.jsonl"
    csv_path = campaign_dir / "campaign-runs.csv"
    with CampaignLock(campaign_dir):
        existing_ids: set[str] = set()
        if jsonl_path.is_file():
            for line in jsonl_path.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    existing_ids.add(str(json.loads(line)["runId"]))
        if str(record["runId"]) in existing_ids:
            raise FileExistsError("runId already exists in campaign")
        with jsonl_path.open("a", encoding="utf-8") as stream:
            stream.write(canonical_json_bytes(dict(record)).decode() + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        write_header = not csv_path.exists()
        with csv_path.open("a", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=RUN_CSV_FIELDS)
            if write_header:
                writer.writeheader()
            writer.writerow({field: record[field] for field in RUN_CSV_FIELDS})
            stream.flush()
            os.fsync(stream.fileno())


def read_proc_sample(pid: int, *, operation_id: str, phase: str) -> dict[str, Any]:
    if phase not in PHASES:
        raise ValueError("unknown repository phase")
    status: dict[str, str] = {}
    for line in Path(f"/proc/{pid}/status").read_text(encoding="utf-8").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            status[key] = value.strip()
    stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split()
    page_size = os.sysconf("SC_PAGE_SIZE")
    ticks = os.sysconf("SC_CLK_TCK")

    def kib(field: str) -> int:
        raw = status.get(field, "0 kB").split()[0]
        return int(raw) * 1024

    return {
        "schemaVersion": SAMPLE_SCHEMA_VERSION,
        "timestamp": utc_now(),
        "monotonicNs": __import__("time").monotonic_ns(),
        "operationId": operation_id,
        "phase": phase,
        "pid": int(pid),
        "cpuUserSeconds": int(stat[13]) / ticks,
        "cpuSystemSeconds": int(stat[14]) / ticks,
        "rssBytes": int(stat[23]) * page_size,
        "peakRssBytes": kib("VmHWM"),
        "readBytes": _proc_io_value(pid, "read_bytes"),
        "writeBytes": _proc_io_value(pid, "write_bytes"),
    }


def _proc_io_value(pid: int, key: str) -> int:
    try:
        lines = Path(f"/proc/{pid}/io").read_text(encoding="utf-8").splitlines()
    except (FileNotFoundError, PermissionError):
        return 0
    for line in lines:
        if line.startswith(f"{key}:"):
            return int(line.split(":", 1)[1].strip())
    return 0


def self_resource_totals() -> dict[str, Any]:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    # Linux ru_maxrss is KiB.
    return {
        "cpuUserSeconds": usage.ru_utime,
        "cpuSystemSeconds": usage.ru_stime,
        "peakRssBytes": int(usage.ru_maxrss) * 1024,
    }


@dataclass(frozen=True)
class IperfResult:
    interval_seconds: float
    transferred_bytes: int
    bits_per_second: float

    @property
    def goodput_mbps(self) -> float:
        return self.bits_per_second / 1_000_000.0


def parse_iperf2_csv(line: str) -> IperfResult:
    """Parse the final iperf2 CSV report (timestamp,...,interval,bytes,bps)."""

    fields = next(csv.reader([line.strip()]))
    if len(fields) < 9:
        raise ValueError("iperf2 CSV result has fewer than 9 fields")
    start_text, end_text = fields[-3].split("-", 1)
    return IperfResult(
        interval_seconds=float(end_text) - float(start_text),
        transferred_bytes=int(fields[-2]),
        bits_per_second=float(fields[-1]),
    )


def final_iperf2_result(output: str) -> IperfResult:
    candidates = [
        line for line in output.splitlines()
        if line.count(",") >= 8 and "-" in line.split(",")[-3]
    ]
    if not candidates:
        raise ValueError("iperf2 output contains no CSV measurement")
    return parse_iperf2_csv(candidates[-1])
