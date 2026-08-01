#!/usr/bin/env python3
"""Run the immutable, matched Spec 164 MiniNDN performance campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))
sys.path.insert(0, str(REPO / "pythonWrapper"))
sys.path.insert(0, str(REPO / "NDNSF-DistributedRepo/pythonWrapper"))

from NDNSF_DistributedRepo_Artifact_Minindn import (  # noqa: E402
    _restore_output_ownership,
    _run_physical_network_ceiling,
    _run_repository_subject,
    _write_json,
)
from spec164_artifact_campaign import (  # noqa: E402
    CONCURRENCY_LEVELS,
    PACKET_PAYLOAD_BYTES,
    PAYLOAD_SIZES,
    REPLICA_COUNTS,
    REPOSITORY_SUBJECTS,
    RUN_SCHEMA_VERSION,
    append_run_record,
    build_cells,
    create_campaign_manifest,
    freeze_campaign,
    load_frozen_campaign,
    sha256_hex,
)


TOPOLOGY = REPO / "Experiments/Topology/spec164-artifact-recovery.conf"
LINEAR_TOPOLOGY = REPO / "Experiments/Topology/spec164-artifact-linear.conf"
PREFLIGHT_BYTES = 1 << 20
DISK_SAFETY_BYTES = 8 << 30
MAX_PREDICTED_RUN_SECONDS = 60.0
MAX_METADATA_RECORDS = 2_000_000
WORKER_MEMORY_BYTES = 96 << 20
BASE_MEMORY_BYTES = 768 << 20


def _available_memory_bytes() -> int:
    for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    return 0


def _campaign_id() -> str:
    return "spec164-artifact-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def _run_preflight(campaign_dir: Path, timeout_s: float) -> dict[str, Any]:
    root = campaign_dir / "preflight"
    root.mkdir(parents=True, exist_ok=False)
    subject_results = {}
    for subject in ("raw-segmented-ndn", *REPOSITORY_SUBJECTS):
        target = root / subject
        target.mkdir()
        result = _run_repository_subject(
            subject=subject,
            output_dir=target,
            topology_file=TOPOLOGY,
            payload_size=PREFLIGHT_BYTES,
            replicas=1,
            concurrency=1,
            timeout_s=timeout_s,
            timeline_sample_rate=1.0,
            quick_smoke=True,
        )
        result["performanceClaim"] = False
        subject_results[subject] = result
    scale_target = root / "signed-r3c4"
    scale_target.mkdir()
    scale_result = _run_repository_subject(
        subject="signed-manifest",
        output_dir=scale_target,
        topology_file=TOPOLOGY,
        payload_size=64 << 10,
        replicas=3,
        concurrency=4,
        timeout_s=timeout_s,
        timeline_sample_rate=1.0,
        quick_smoke=True,
    )
    scale_result["performanceClaim"] = False
    return {
        "schema": "ndnsf-repo-spec164-preflight-v2",
        "performanceClaim": False,
        "subjectResults": subject_results,
        "replicaConcurrencyProbe": scale_result,
    }


def _evaluate_cells(
    cells: list[dict[str, Any]],
    preflight: dict[str, Any],
    *,
    disk_available_bytes: int,
    memory_available_bytes: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    baselines = preflight["subjectResults"]
    decisions = []
    repository_admissible_pairs: set[str] = set()
    for cell in cells:
        if cell["subject"] == "raw-segmented-ndn":
            continue
        size = int(cell["payloadBytes"])
        replicas = int(cell["replicas"])
        concurrency = int(cell["concurrency"])
        workers = replicas * concurrency
        subject = str(cell["subject"])
        baseline = baselines[subject]
        storage_factor = 1.5 if subject == "legacy-exact-packet" else 1.1
        required_disk = size + int(size * workers * storage_factor)
        required_memory = BASE_MEMORY_BYTES + workers * WORKER_MEMORY_BYTES
        predicted_seconds = (
            (
                float(baseline["elapsedMs"])
                + float(baseline["coldRetrievalElapsedMs"])
            ) / 1000.0
            * (size / PREFLIGHT_BYTES)
            * workers
        )
        metadata_records = (
            ((size + PACKET_PAYLOAD_BYTES - 1) // PACKET_PAYLOAD_BYTES) * workers
            if subject == "legacy-exact-packet" else workers
        )
        reasons = []
        if baseline["verdict"] != "PASS":
            reasons.append("SUBJECT_PREFLIGHT_FAILED")
        if preflight["replicaConcurrencyProbe"]["verdict"] != "PASS":
            reasons.append("REPLICA_CONCURRENCY_PREFLIGHT_FAILED")
        if required_disk > max(0, disk_available_bytes - DISK_SAFETY_BYTES):
            reasons.append("DISK_WORKING_SET_EXCEEDS_FROZEN_BUDGET")
        if required_memory > int(memory_available_bytes * 0.80):
            reasons.append("PROCESS_WORKING_SET_EXCEEDS_FROZEN_BUDGET")
        if predicted_seconds > MAX_PREDICTED_RUN_SECONDS:
            reasons.append("PREDICTED_RUN_EXCEEDS_60_SECOND_HOST_BUDGET")
        if metadata_records > MAX_METADATA_RECORDS:
            reasons.append("LEGACY_METADATA_ROWS_EXCEED_FROZEN_LIMIT")
        admissible = not reasons
        if admissible:
            repository_admissible_pairs.add(str(cell["pairId"]))
        decisions.append({
            **cell,
            "admissible": admissible,
            "reasonCodes": reasons,
            "requiredDiskBytes": required_disk,
            "requiredMemoryBytes": required_memory,
            "predictedSeconds": predicted_seconds,
            "predictedMetadataRecords": metadata_records,
        })

    for cell in cells:
        if cell["subject"] != "raw-segmented-ndn":
            continue
        pair_admissible = str(cell["pairId"]) in repository_admissible_pairs
        decisions.append({
            **cell,
            "admissible": pair_admissible,
            "reasonCodes": [] if pair_admissible else [
                "NO_ADMISSIBLE_PAIRED_REPOSITORY_SUBJECT"
            ],
            "requiredDiskBytes": int(cell["payloadBytes"]),
            "requiredMemoryBytes": BASE_MEMORY_BYTES,
            "predictedSeconds": (
                (
                    float(baselines["raw-segmented-ndn"]["elapsedMs"])
                    + float(baselines["raw-segmented-ndn"][
                        "coldRetrievalElapsedMs"])
                ) / 1000.0
                * (int(cell["payloadBytes"]) / PREFLIGHT_BYTES)
                * int(cell["concurrency"])
            ),
            "predictedMetadataRecords": 0,
        })
    order = {cell["cellId"]: index for index, cell in enumerate(cells)}
    decisions.sort(key=lambda item: order[item["cellId"]])
    admissible_cells = [
        {key: decision[key] for key in (
            "cellId", "pairId", "subject", "payloadBytes", "replicas",
            "concurrency", "pairedRawCellId",
        )}
        for decision in decisions if decision["admissible"]
    ]
    return decisions, admissible_cells


def _write_cell_csv(path: Path, decisions: list[dict[str, Any]]) -> None:
    fields = (
        "cellId", "pairId", "subject", "payloadBytes", "replicas",
        "concurrency", "admissible", "reasonCodes", "requiredDiskBytes",
        "requiredMemoryBytes", "predictedSeconds", "predictedMetadataRecords",
    )
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for decision in decisions:
            row = {field: decision[field] for field in fields}
            row["reasonCodes"] = ";".join(row["reasonCodes"])
            writer.writerow(row)


def _run_record(
    campaign_id: str,
    scheduled: dict[str, Any],
    cell: dict[str, Any],
    result: dict[str, Any],
) -> dict[str, Any]:
    logical = int(result.get("logicalBytes", 0))
    storage_written = int(result.get("storageBytesWritten", 0))
    storage_denominator = max(
        1,
        int(cell["payloadBytes"]) * int(cell["concurrency"])
        * (int(cell["replicas"]) if cell["subject"] != "raw-segmented-ndn" else 1),
    )
    return {
        "schemaVersion": RUN_SCHEMA_VERSION,
        "campaignId": campaign_id,
        "runId": scheduled["runId"],
        "cellId": scheduled["cellId"],
        "pairId": scheduled["pairId"],
        "subject": scheduled["subject"],
        "repetition": int(scheduled["repetition"]),
        "warmup": bool(scheduled["warmup"]),
        "admissible": True,
        "verdict": result.get("verdict", "FAIL"),
        "payloadBytes": int(cell["payloadBytes"]),
        "replicas": int(cell["replicas"]),
        "concurrency": int(cell["concurrency"]),
        "elapsedMs": float(result.get("elapsedMs", 0.0)),
        "logicalGoodputMbps": float(result.get("logicalGoodputMbps", 0.0)),
        "wireGoodputMbps": float(result.get("wireGoodputMbps", 0.0)),
        "cpuUserSeconds": float(result.get("cpuUserSeconds", 0.0)),
        "cpuSystemSeconds": float(result.get("cpuSystemSeconds", 0.0)),
        "peakRssBytes": int(result.get("peakRssBytes", 0)),
        "logicalBytes": logical,
        "dataWireBytes": int(result.get("dataWireBytes", 0)),
        "interestWireBytes": int(result.get("interestWireBytes", 0)),
        "wireBytes": int(result.get("wireBytes", 0)),
        "retransmittedBytes": int(result.get("retransmittedBytes", 0)),
        "payloadStoreBytesRead": int(
            result.get("payloadStoreBytesRead", 0)),
        "payloadStoreBytesWritten": int(
            result.get("payloadStoreBytesWritten", 0)),
        "metadataStoreBytesRead": int(
            result.get("metadataStoreBytesRead", 0)),
        "metadataStoreBytesWritten": int(
            result.get("metadataStoreBytesWritten", 0)),
        "storageBytesRead": int(result.get("storageBytesRead", 0)),
        "storageBytesWritten": storage_written,
        "coldRetrievalElapsedMs": float(
            result.get("coldRetrievalElapsedMs", 0.0)),
        "coldRetrievalLogicalGoodputMbps": float(
            result.get("coldRetrievalLogicalGoodputMbps", 0.0)),
        "coldRetrievalDataWireBytes": int(
            result.get("coldRetrievalDataWireBytes", 0)),
        "coldRetrievalInterestWireBytes": int(
            result.get("coldRetrievalInterestWireBytes", 0)),
        "coldRetrievalWireBytes": int(
            result.get("coldRetrievalWireBytes", 0)),
        "coldDestinationVisible": bool(
            result.get("coldDestinationVisible", False)),
        "interestCount": int(result.get("interestCount", 0)),
        "dataCount": int(result.get("dataCount", 0)),
        "timeoutCount": int(result.get("timeoutCount", 0)),
        "retransmissionCount": int(result.get("retransmissionCount", 0)),
        "windowMinimum": int(result.get("windowMinimum", 0)),
        "windowMaximum": int(result.get("windowMaximum", 0)),
        "asymmetricVerifyCount": int(result.get("asymmetricVerifyCount", 0)),
        "asymmetricVerifyMs": float(result.get("asymmetricVerifyMs", 0.0)),
        "digestVerifyCount": int(result.get("digestVerifyCount", 0)),
        "digestVerifyMs": float(result.get("digestVerifyMs", 0.0)),
        "metadataOperations": int(result.get("metadataOperations", 0)),
        "metadataRecords": int(result.get("metadataRecords", 0)),
        "requestedReplicas": int(result.get("requestedReplicas", cell["replicas"])),
        "selectedReplicas": int(result.get("selectedReplicas", 0)),
        "committedReplicas": int(result.get("committedReplicas", 0)),
        "readAmplification": (
            float(result.get("storageBytesRead", 0)) / storage_denominator
        ),
        "writeAmplification": storage_written / storage_denominator,
        "failureReason": str(result.get("failureReason", "")),
        "phaseLatencyMs": result.get("phaseLatencyMs", {}),
        "resourceSamples": result.get("resourceSamples", []),
        "producerMetrics": result.get("producerMetrics", {}),
        "performanceClaim": (
            not bool(scheduled["warmup"]) and result.get("verdict") == "PASS"
        ),
    }


def _failure_result(error: BaseException) -> dict[str, Any]:
    return {
        "verdict": "FAIL",
        "admissible": True,
        "failureReason": f"{type(error).__name__}: {error}",
        "performanceClaim": False,
    }


def _execute_schedule(campaign_dir: Path, manifest: dict[str, Any]) -> None:
    cells = {cell["cellId"]: cell for cell in manifest["cells"]}
    existing = set()
    runs_path = campaign_dir / "campaign-runs.jsonl"
    if runs_path.is_file():
        existing = {
            json.loads(line)["runId"]
            for line in runs_path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        }
    total = len(manifest["runSchedule"])
    for index, scheduled in enumerate(manifest["runSchedule"], start=1):
        if scheduled["runId"] in existing:
            continue
        cell = cells[scheduled["cellId"]]
        run_root = campaign_dir / "runs" / scheduled["runId"]
        run_root.mkdir(parents=True, exist_ok=False)
        timeout_s = min(
            180.0,
            max(
                30.0,
                float(manifest["admissibility"]["predictedSecondsByCell"][
                    cell["cellId"]
                ]) * 3.0 + 15.0,
            ),
        )
        try:
            result = _run_repository_subject(
                subject=cell["subject"],
                output_dir=run_root,
                topology_file=TOPOLOGY,
                payload_size=int(cell["payloadBytes"]),
                replicas=int(cell["replicas"]),
                concurrency=int(cell["concurrency"]),
                timeout_s=timeout_s,
                timeline_sample_rate=float(manifest["timelineSampleRate"]),
                quick_smoke=False,
            )
        except BaseException as error:
            result = _failure_result(error)
            _write_json(run_root / "failure.json", result)
        record = _run_record(
            manifest["campaignId"], scheduled, cell, result
        )
        append_run_record(campaign_dir, record)
        _write_json(campaign_dir / "campaign-status.json", {
            "campaignId": manifest["campaignId"],
            "completedRuns": index,
            "totalRuns": total,
            "lastRunId": scheduled["runId"],
            "lastVerdict": record["verdict"],
            "updatedAtEpochSeconds": time.time(),
        })
        print(
            f"SPEC164_CAMPAIGN_PROGRESS {index}/{total} "
            f"{scheduled['runId']} {record['verdict']}",
            flush=True,
        )


def _parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--resume-campaign", type=Path)
    parser.add_argument("--preflight-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--timeline-sample-rate", type=float, default=0.01)
    parser.add_argument("--skip-physical-ceiling", action="store_true")
    parser.add_argument(
        "--freeze-only",
        action="store_true",
        help="Run preflight and freeze the manifest, but do not inspect formal outcomes.",
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = _parse_args(argv)
    if bool(args.output_dir) == bool(args.resume_campaign):
        raise SystemExit("choose exactly one of --output-dir or --resume-campaign")
    # MiniNDN parses global argv.
    sys.argv = [sys.argv[0]]
    if args.resume_campaign:
        campaign_dir = args.resume_campaign.resolve()
        manifest = load_frozen_campaign(campaign_dir)
    else:
        campaign_dir = args.output_dir.resolve()
        if campaign_dir.exists():
            raise SystemExit(f"campaign output already exists: {campaign_dir}")
        campaign_dir.mkdir(parents=True)
        preflight = _run_preflight(
            campaign_dir, args.preflight_timeout_seconds
        )
        disk_available = shutil.disk_usage(campaign_dir).free
        memory_available = _available_memory_bytes()
        decisions, admissible_cells = _evaluate_cells(
            build_cells(),
            preflight,
            disk_available_bytes=disk_available,
            memory_available_bytes=memory_available,
        )
        preflight.update({
            "diskAvailableBytes": disk_available,
            "memoryAvailableBytes": memory_available,
            "diskSafetyBytes": DISK_SAFETY_BYTES,
            "maximumPredictedRunSeconds": MAX_PREDICTED_RUN_SECONDS,
            "maximumMetadataRecords": MAX_METADATA_RECORDS,
            "workerMemoryBytes": WORKER_MEMORY_BYTES,
            "baseMemoryBytes": BASE_MEMORY_BYTES,
            "cellDecisions": decisions,
            "admissibleCellCount": len(admissible_cells),
            "inadmissibleCellCount": len(decisions) - len(admissible_cells),
        })
        _write_json(campaign_dir / "preflight.json", preflight)
        _write_cell_csv(campaign_dir / "campaign-cells.csv", decisions)
        preflight_digest = sha256_hex(
            (campaign_dir / "preflight.json").read_bytes()
        )
        predicted = {
            item["cellId"]: item["predictedSeconds"]
            for item in decisions if item["admissible"]
        }
        manifest = create_campaign_manifest(
            campaign_id=_campaign_id(),
            repo_root=REPO,
            topology={
                "file": str(TOPOLOGY),
                "kind": "publisher-three-repos-consumer",
                "delayMsPerLink": 1,
                "bandwidthMbpsPerLink": 1000,
            },
            admissibility={
                "preflightSha256": preflight_digest,
                "predeclaredExclusionsOnly": True,
                "retainFailedAndNegativeRuns": True,
                "diskSafetyBytes": DISK_SAFETY_BYTES,
                "maximumPredictedRunSeconds": MAX_PREDICTED_RUN_SECONDS,
                "maximumMetadataRecords": MAX_METADATA_RECORDS,
                "predictedSecondsByCell": predicted,
            },
            cells=admissible_cells,
            repetitions=5,
            timeline_sample_rate=args.timeline_sample_rate,
            quick_smoke=False,
            measurement_window_seconds=60.0,
        )
        freeze_campaign(campaign_dir, manifest)
        if args.freeze_only:
            _restore_output_ownership(campaign_dir)
            print(f"SPEC164_ARTIFACT_CAMPAIGN_FROZEN {campaign_dir}", flush=True)
            return 0
    physical_present = any((
        (campaign_dir / "physical-ceiling/summary.json").is_file(),
        (campaign_dir / "physical-ceiling-formal/summary.json").is_file(),
        (campaign_dir / "physical-ceiling.json").is_file(),
    ))
    if not args.skip_physical_ceiling and not physical_present:
        physical_root = campaign_dir / "physical-ceiling"
        physical_root.mkdir()
        physical = _run_physical_network_ceiling(
            output_dir=physical_root,
            topology_file=LINEAR_TOPOLOGY,
            measurement_window_seconds=60.0,
            quick_smoke=False,
        )
        _write_json(campaign_dir / "physical-ceiling.json", physical)
    _execute_schedule(campaign_dir, manifest)
    _write_json(campaign_dir / "campaign-complete.json", {
        "campaignId": manifest["campaignId"],
        "manifestSha256": (
            campaign_dir / "campaign-manifest.sha256"
        ).read_text(encoding="utf-8").split()[0],
        "completed": True,
        "completedAtEpochSeconds": time.time(),
    })
    _restore_output_ownership(campaign_dir)
    print(f"SPEC164_ARTIFACT_CAMPAIGN_OK {campaign_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
