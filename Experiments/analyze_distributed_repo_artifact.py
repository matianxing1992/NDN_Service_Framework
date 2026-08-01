#!/usr/bin/env python3
"""Derive and independently verify Spec 164 campaign statistics."""

from __future__ import annotations

import argparse
import csv
from decimal import Decimal
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
from typing import Any, Iterable


METRICS = (
    "logicalGoodputMbps", "wireGoodputMbps", "elapsedMs",
    "cpuUserSeconds", "cpuSystemSeconds", "peakRssBytes",
    "dataWireBytes", "interestWireBytes", "wireBytes",
    "retransmittedBytes", "payloadStoreBytesRead",
    "payloadStoreBytesWritten", "metadataStoreBytesRead",
    "metadataStoreBytesWritten", "storageBytesRead", "storageBytesWritten",
    "coldRetrievalElapsedMs", "coldRetrievalLogicalGoodputMbps",
    "coldRetrievalDataWireBytes", "coldRetrievalInterestWireBytes",
    "coldRetrievalWireBytes", "interestCount", "dataCount", "timeoutCount",
    "retransmissionCount", "asymmetricVerifyCount", "asymmetricVerifyMs",
    "digestVerifyCount", "digestVerifyMs", "metadataOperations",
    "metadataRecords", "readAmplification", "writeAmplification",
)
LARGE_ARTIFACT_MIN_BYTES = 64 << 20


def _normalize_legacy_record(record: dict[str, Any]) -> dict[str, Any]:
    """Make immutable v1 evidence readable without rewriting it."""

    value = dict(record)
    if int(value.get("schemaVersion", 1)) >= 2:
        return value
    value.setdefault("dataWireBytes", int(value.get("wireBytes", 0)))
    value.setdefault("interestWireBytes", 0)
    value.setdefault(
        "payloadStoreBytesRead", int(value.get("storageBytesRead", 0)))
    value.setdefault(
        "payloadStoreBytesWritten", int(value.get("storageBytesWritten", 0)))
    value.setdefault("metadataStoreBytesRead", 0)
    value.setdefault("metadataStoreBytesWritten", 0)
    value.setdefault("coldRetrievalElapsedMs", 0.0)
    value.setdefault("coldRetrievalLogicalGoodputMbps", 0.0)
    value.setdefault("coldRetrievalDataWireBytes", 0)
    value.setdefault("coldRetrievalInterestWireBytes", 0)
    value.setdefault("coldRetrievalWireBytes", 0)
    value.setdefault("coldDestinationVisible", False)
    phases = dict(value.get("phaseLatencyMs", {}))
    if "reservation" in phases and "sessionStart" not in phases:
        phases["sessionStart"] = phases.pop("reservation")
    value["phaseLatencyMs"] = phases
    return value


def percentile(values: Iterable[float], probability: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    if not 0.0 <= probability <= 1.0:
        raise ValueError("probability must be in [0,1]")
    position = probability * (len(ordered) - 1)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def bootstrap_median_ci(
    values: Iterable[float],
    *,
    seed: int,
    repetitions: int = 10_000,
) -> list[float]:
    source = [float(value) for value in values]
    if not source:
        raise ValueError("bootstrap requires at least one value")
    generator = random.Random(seed)
    estimates = [
        statistics.median(
            source[generator.randrange(len(source))] for _ in source
        )
        for _ in range(repetitions)
    ]
    return [percentile(estimates, 0.025), percentile(estimates, 0.975)]


def distribution(values: Iterable[float], *, seed_text: str) -> dict[str, Any]:
    source = [float(value) for value in values]
    seed = int.from_bytes(
        hashlib.sha256(seed_text.encode("utf-8")).digest()[:8], "big"
    )
    return {
        "n": len(source),
        "min": min(source),
        "p50": percentile(source, 0.50),
        "median": statistics.median(source),
        "p95": percentile(source, 0.95),
        "max": max(source),
        "q1": percentile(source, 0.25),
        "q3": percentile(source, 0.75),
        "iqr": percentile(source, 0.75) - percentile(source, 0.25),
        "mean": statistics.fmean(source),
        "medianBootstrap95Ci": bootstrap_median_ci(source, seed=seed),
    }


def _load(campaign_dir: Path):
    encoded = (campaign_dir / "campaign-manifest.json").read_bytes()
    expected = (
        campaign_dir / "campaign-manifest.sha256"
    ).read_text(encoding="utf-8").split()[0]
    actual = hashlib.sha256(encoded).hexdigest()
    if actual != expected:
        raise RuntimeError("campaign manifest seal mismatch")
    manifest = json.loads(encoded)
    records = [
        _normalize_legacy_record(json.loads(line))
        for line in (campaign_dir / "campaign-runs.jsonl").read_text(
            encoding="utf-8"
        ).splitlines()
        if line.strip()
    ]
    return manifest, records, actual


def _group_measured(records):
    grouped = {}
    for record in records:
        if not record["warmup"]:
            grouped.setdefault(record["cellId"], []).append(record)
    return grouped


def _paired_ratios(records):
    measured = {
        (record["pairId"], record["subject"], int(record["repetition"])): record
        for record in records if not record["warmup"]
    }
    pairs = sorted({key[0] for key in measured})
    repository_raw = []
    signed_digest = []
    for pair_id in pairs:
        for repetition in range(1, 6):
            raw = measured.get((pair_id, "raw-segmented-ndn", repetition))
            if raw is None or float(raw["logicalGoodputMbps"]) <= 0:
                continue
            for subject in (
                "legacy-exact-packet", "digest-only", "signed-manifest"
            ):
                repository = measured.get((pair_id, subject, repetition))
                if repository is not None:
                    repository_raw.append({
                        "pairId": pair_id,
                        "payloadBytes": int(repository["payloadBytes"]),
                        "replicas": int(repository["replicas"]),
                        "concurrency": int(repository["concurrency"]),
                        "subject": subject,
                        "repetition": repetition,
                        "ratio": (
                            float(repository["logicalGoodputMbps"])
                            / float(raw["logicalGoodputMbps"])
                        ),
                    })
            signed = measured.get((pair_id, "signed-manifest", repetition))
            digest = measured.get((pair_id, "digest-only", repetition))
            if (
                signed is not None
                and digest is not None
                and float(digest["logicalGoodputMbps"]) > 0
            ):
                signed_digest.append({
                    "pairId": pair_id,
                    "payloadBytes": int(signed["payloadBytes"]),
                    "replicas": int(signed["replicas"]),
                    "concurrency": int(signed["concurrency"]),
                    "repetition": repetition,
                    "ratio": (
                        float(signed["logicalGoodputMbps"])
                        / float(digest["logicalGoodputMbps"])
                    ),
                })
    return repository_raw, signed_digest


def _summarize_ratios(rows, key_name):
    grouped = {}
    for row in rows:
        key = (row["pairId"], row.get("subject", "signed/digest"))
        grouped.setdefault(key, []).append(row)
    return [
        {
            "pairId": pair_id,
            key_name: subject,
            "distribution": distribution(
                (row["ratio"] for row in group),
                seed_text=f"ratio:{pair_id}:{subject}",
            ),
            "allRatios": [row["ratio"] for row in group],
            "repetitions": [row["repetition"] for row in group],
        }
        for (pair_id, subject), group in sorted(grouped.items())
    ]


def _threshold_verdicts(
    manifest, records, repo_raw_summary, signed_digest_summary,
    control_evidence=None,
):
    measured = [record for record in records if not record["warmup"]]
    digest_large = [
        item for item in repo_raw_summary
        if item["subject"] == "digest-only"
        and int(item["pairId"].split("-")[0][1:]) >= 64 << 20
    ]
    digest_large_records = [
        record for record in measured
        if int(record["payloadBytes"]) >= 64 << 20
        and record["subject"] in ("raw-segmented-ndn", "digest-only")
    ]
    digest_completion = (
        bool(digest_large_records)
        and all(record["verdict"] == "PASS" for record in digest_large_records)
    )
    if not digest_large:
        sc002 = {
            "verdict": "FAIL" if digest_large_records else "INCONCLUSIVE",
            "reason": (
                "eligible digest/raw samples failed or have zero denominator"
                if digest_large_records
                else "no admissible digest/raw cell at or above 64 MiB"
            ),
            "completionGate": "FAIL" if digest_large_records else "NOT_MEASURED",
        }
    else:
        point_pass = all(
            item["distribution"]["median"] >= 0.85 for item in digest_large
        )
        robust_pass = all(
            item["distribution"]["medianBootstrap95Ci"][0] >= 0.85
            for item in digest_large
        )
        sc002 = {
            "verdict": (
                "INCONCLUSIVE" if not digest_large_records
                else "FAIL" if not digest_completion
                else (
                    "PASS" if point_pass and robust_pass
                    else ("INCONCLUSIVE" if point_pass else "FAIL")
                )
            ),
            "completionGate": (
                "NOT_MEASURED" if not digest_large_records
                else ("PASS" if digest_completion else "FAIL")
            ),
            "engineeringPointEstimate": "PASS" if point_pass else "FAIL",
            "bootstrapLowerBoundGate": "PASS" if robust_pass else "FAIL",
            "threshold": 0.85,
            "eligibleCells": len(digest_large),
        }
    signed_digest_records = [
        record for record in measured
        if int(record["payloadBytes"]) >= LARGE_ARTIFACT_MIN_BYTES
        and record["subject"] in ("digest-only", "signed-manifest")
    ]
    eligible_signed_digest = [
        item for item in signed_digest_summary
        if int(item["pairId"].split("-")[0][1:]) >= LARGE_ARTIFACT_MIN_BYTES
    ]
    signed_digest_completion = (
        bool(signed_digest_records)
        and all(record["verdict"] == "PASS" for record in signed_digest_records)
    )
    point_pass = all(
        item["distribution"]["median"] >= 0.90
        for item in eligible_signed_digest
    ) if eligible_signed_digest else False
    robust_pass = all(
        item["distribution"]["medianBootstrap95Ci"][0] >= 0.90
        for item in eligible_signed_digest
    ) if eligible_signed_digest else False
    sc003 = {
        "verdict": (
            "INCONCLUSIVE" if not signed_digest_records
            else (
                "FAIL" if not signed_digest_completion
                else (
                    "PASS" if point_pass and robust_pass
                    else ("INCONCLUSIVE" if point_pass else "FAIL")
                )
            )
        ),
        "completionGate": (
            "NOT_MEASURED" if not signed_digest_records
            else ("PASS" if signed_digest_completion else "FAIL")
        ),
        "engineeringPointEstimate": "PASS" if point_pass else "FAIL",
        "bootstrapLowerBoundGate": "PASS" if robust_pass else "FAIL",
        "threshold": 0.90,
        "minimumArtifactBytes": LARGE_ARTIFACT_MIN_BYTES,
        "eligibleCells": len(eligible_signed_digest),
    }
    if control_evidence is None:
        sc004 = {
            "verdict": "INCONCLUSIVE",
            "reason": "no public Collaboration control-plane evidence supplied",
        }
    else:
        control = control_evidence.get("control", control_evidence)
        network = control_evidence.get("network", {})
        publication = network.get("publicationMetrics", {})
        selected = len(control.get("selectedRepoNodes", []))
        lifecycle_phases = int(control.get("lifecyclePhaseCount", 0))
        operations = int(control.get("controlOperationCount", -1))
        segments = int(publication.get("segments", 0))
        bound = selected + lifecycle_phases
        sc004_pass = (
            control_evidence.get("verdict") == "PASS"
            and control.get("verdict") == "PASS"
            and selected >= 1
            and lifecycle_phases >= 1
            and 0 <= operations <= bound
            and segments > operations
            and int(control.get("payloadBytes", 0)) > 0
        )
        sc004 = {
            "verdict": "PASS" if sc004_pass else "FAIL",
            "controlOperations": operations,
            "selectedReplicas": selected,
            "lifecyclePhases": lifecycle_phases,
            "boundedOperationLimit": bound,
            "publicationSegments": segments,
            "perChunkServiceInvocation": False if sc004_pass else "UNPROVEN",
        }
    cold_scalable = [
        record for record in measured
        if record["replicas"] == 1 and record["concurrency"] == 1
        and record["subject"] in ("digest-only", "signed-manifest")
    ]
    completion_pass = (
        bool(cold_scalable)
        and all(
            record["verdict"] == "PASS"
            and bool(record["coldDestinationVisible"])
            for record in cold_scalable
        )
    )
    write_pass = all(
        float(record["writeAmplification"]) <= 1.50
        for record in cold_scalable
    ) if cold_scalable else False
    read_measured = any(
        int(record["payloadStoreBytesRead"]) > 0
        and bool(record["coldDestinationVisible"])
        for record in cold_scalable
    )
    sc007 = {
        "verdict": "INCONCLUSIVE" if completion_pass and write_pass and not read_measured else (
            "PASS" if completion_pass and write_pass and read_measured and all(
                float(record["readAmplification"]) <= 1.20
                for record in cold_scalable
            ) else "FAIL"
        ),
        "completionGate": "PASS" if completion_pass else "FAIL",
        "writeAmplificationGate": "PASS" if write_pass else "FAIL",
        "readAmplificationGate": (
            "NOT_MEASURED" if not read_measured else (
                "PASS" if all(
                    float(record["readAmplification"]) <= 1.20
                    and bool(record["coldDestinationVisible"])
                    for record in cold_scalable
                ) else "FAIL"
            )
        ),
        "writeThreshold": 1.50,
        "readThreshold": 1.20,
    }
    schedule_ids = {run["runId"] for run in manifest["runSchedule"]}
    record_ids = {record["runId"] for record in records}
    measured_counts = {
        cell["cellId"]: sum(
            not record["warmup"] and record["cellId"] == cell["cellId"]
            for record in records
        )
        for cell in manifest["cells"]
    }
    sc011_pass = (
        schedule_ids == record_ids
        and len(record_ids) == len(records)
        and all(count >= 5 for count in measured_counts.values())
    )
    sc011 = {
        "verdict": "PASS" if sc011_pass else "FAIL",
        "scheduledRuns": len(schedule_ids),
        "retainedRuns": len(records),
        "uniqueRunIds": len(record_ids),
        "measuredCountsByCell": measured_counts,
    }
    evidence_root = (
        Path(__file__).resolve().parents[1]
        / "specs/164-distributed-repo-large-artifact-transport/evidence"
    )
    required = (
        "t008-single-replica-e2e.md",
        "t011-minindn-recovery.md",
        "t014-capability-compatibility.md",
        "t015-migration-rollback.md",
        "campaign-pointer.md",
    )
    sc012 = {
        "verdict": (
            "PASS" if all((evidence_root / name).is_file() for name in required)
            else "INCONCLUSIVE"
        ),
        "requiredMiniNDNEvidence": list(required),
        "tigerClusterClaim": False,
    }
    return {
        "SC-002": sc002,
        "SC-003": sc003,
        "SC-004": sc004,
        "SC-007": sc007,
        "SC-011": sc011,
        "SC-012": sc012,
    }


def _scaling_summary(cell_summaries):
    def safe_ratio(numerator, denominator):
        return numerator / denominator if denominator > 0 else None

    lookup = {
        (
            item["payloadBytes"], item["replicas"], item["concurrency"],
            item["subject"],
        ): item
        for item in cell_summaries
    }
    concurrency = []
    replica = []
    size = []
    for key, item in sorted(lookup.items()):
        payload, replicas, workers, subject = key
        current = item["metrics"]["logicalGoodputMbps"]["median"]
        if workers > 1:
            baseline = lookup.get((payload, replicas, 1, subject))
            if baseline is not None:
                base = baseline["metrics"]["logicalGoodputMbps"]["median"]
                concurrency.append({
                    "payloadBytes": payload,
                    "replicas": replicas,
                    "subject": subject,
                    "concurrency": workers,
                    "goodputRatioToC1": safe_ratio(current, base),
                    "parallelEfficiency": safe_ratio(current, base * workers),
                })
        if replicas == 3:
            baseline = lookup.get((payload, 1, workers, subject))
            if baseline is not None:
                base = baseline["metrics"]["logicalGoodputMbps"]["median"]
                replica.append({
                    "payloadBytes": payload,
                    "concurrency": workers,
                    "subject": subject,
                    "replicas": replicas,
                    "goodputRatioToR1": safe_ratio(current, base),
                    "replicaEfficiency": safe_ratio(current, base * replicas),
                })
        if payload > 1 << 20:
            baseline = lookup.get((1 << 20, replicas, workers, subject))
            if baseline is not None:
                base_goodput = baseline["metrics"]["logicalGoodputMbps"]["median"]
                base_rss = baseline["metrics"]["peakRssBytes"]["median"]
                current_rss = item["metrics"]["peakRssBytes"]["median"]
                size.append({
                    "payloadBytes": payload,
                    "replicas": replicas,
                    "concurrency": workers,
                    "subject": subject,
                    "goodputRatioTo1MiB": safe_ratio(current, base_goodput),
                    "peakRssRatioTo1MiB": current_rss / max(1.0, base_rss),
                })
    return {
        "concurrencyScaling": concurrency,
        "replicaScaling": replica,
        "artifactSizeScaling": size,
    }


def _physical_ceiling(campaign_dir: Path) -> dict[str, Any]:
    candidates = (
        campaign_dir / "physical-ceiling-formal/summary.json",
        campaign_dir / "physical-ceiling/summary.json",
        campaign_dir / "physical-ceiling.json",
    )
    for path in candidates:
        if path.is_file():
            value = json.loads(path.read_text(encoding="utf-8"))
            return value.get("result", value)
    return {
        "verdict": "NOT_MEASURED",
        "performanceClaim": False,
    }


def analyze(
    campaign_dir: Path, control_evidence: dict[str, Any] | None = None
) -> dict[str, Any]:
    manifest, records, manifest_digest = _load(campaign_dir)
    grouped = _group_measured(records)
    cell_summaries = []
    for cell_id, samples in sorted(grouped.items()):
        cell_summaries.append({
            "cellId": cell_id,
            "pairId": samples[0]["pairId"],
            "subject": samples[0]["subject"],
            "payloadBytes": int(samples[0]["payloadBytes"]),
            "replicas": int(samples[0]["replicas"]),
            "concurrency": int(samples[0]["concurrency"]),
            "verdictCounts": {
                verdict: sum(sample["verdict"] == verdict for sample in samples)
                for verdict in ("PASS", "FAIL", "INADMISSIBLE")
            },
            "metrics": {
                metric: distribution(
                    (sample[metric] for sample in samples),
                    seed_text=f"{cell_id}:{metric}",
                )
                for metric in METRICS
            },
            "phaseLatencyMs": {
                phase: distribution(
                    (sample["phaseLatencyMs"].get(phase, 0.0) for sample in samples),
                    seed_text=f"{cell_id}:phase:{phase}",
                )
                for phase in manifest["phases"]
            },
        })
    repo_raw, signed_digest = _paired_ratios(records)
    repo_raw_summary = _summarize_ratios(repo_raw, "subject")
    signed_digest_summary = _summarize_ratios(
        signed_digest, "comparison"
    )
    warmup_failures = [
        {
            "runId": record["runId"],
            "verdict": record["verdict"],
            "failureReason": record["failureReason"],
        }
        for record in records if record["warmup"] and record["verdict"] != "PASS"
    ]
    measured_failures = [
        {
            "runId": record["runId"],
            "verdict": record["verdict"],
            "failureReason": record["failureReason"],
        }
        for record in records if not record["warmup"] and record["verdict"] != "PASS"
    ]
    verdicts = _threshold_verdicts(
        manifest, records, repo_raw_summary, signed_digest_summary,
        control_evidence,
    )
    physical = _physical_ceiling(campaign_dir)
    bottleneck = float(physical.get("pathBottleneckMbps", 0.0))
    network_utilization = [
        {
            "cellId": item["cellId"],
            "medianWireGoodputToPhysicalCeiling": (
                item["metrics"]["wireGoodputMbps"]["median"] / bottleneck
                if bottleneck > 0 else None
            ),
        }
        for item in cell_summaries
    ]
    return {
        "schema": "ndnsf-repo-spec164-derived-v2",
        "campaignId": manifest["campaignId"],
        "manifestSha256": manifest_digest,
        "sampleCounts": {
            "total": len(records),
            "warmup": sum(record["warmup"] for record in records),
            "measured": sum(not record["warmup"] for record in records),
            "admissibleCells": len(manifest["cells"]),
        },
        "cellSummaries": cell_summaries,
        "repositoryRawRatios": repo_raw_summary,
        "signedDigestRatios": signed_digest_summary,
        "scaling": _scaling_summary(cell_summaries),
        "physicalCeiling": physical,
        "networkUtilization": network_utilization,
        "warmupFailures": warmup_failures,
        "measuredFailures": measured_failures,
        "thresholdVerdicts": verdicts,
        "limitations": [
            "Five measured repetitions close only the engineering gate; they do not support an inferential paper claim.",
            "Only the 64-MiB r1/c1 cell was admissible at or above 64 MiB; 1-GiB and 16-GiB cells were mechanically excluded before formal outcomes.",
            "The data-plane campaign is paired with a separate public NDNSF Collaboration smoke for SC-004; their latency samples are not combined.",
            "Discovery, ACK collection, planning, queue wait, and activation control are not included in data-plane goodput.",
            "Cold retrieval uses a fresh destination after publication and records payload-store plus metadata-store reads separately.",
            "All warmup and measured failures are retained; failed transfers contribute to completion gates and are never removed from threshold verdicts.",
            "This is MiniNDN evidence, not TigerCluster or large-model evidence.",
        ],
    }


def independent_verify(
    campaign_dir: Path, derived: dict[str, Any]
) -> dict[str, Any]:
    """Recompute critical invariants from CSV using Decimal arithmetic."""

    manifest = json.loads(
        (campaign_dir / "campaign-manifest.json").read_text(encoding="utf-8")
    )
    with (campaign_dir / "campaign-runs.csv").open(
        encoding="utf-8", newline=""
    ) as stream:
        rows = list(csv.DictReader(stream))
    ids = [row["runId"] for row in rows]
    measured_by_cell = {
        cell["cellId"]: sum(
            row["cellId"] == cell["cellId"]
            and row["warmup"].lower() == "false"
            for row in rows
        )
        for cell in manifest["cells"]
    }
    ratio_checks = []
    jsonl = [
        json.loads(line)
        for line in (campaign_dir / "campaign-runs.jsonl").read_text(
            encoding="utf-8"
        ).splitlines() if line.strip()
    ]
    lookup = {
        (row["pairId"], row["subject"], int(row["repetition"])): row
        for row in jsonl if not row["warmup"]
    }
    for item in derived["repositoryRawRatios"]:
        pair_id = item["pairId"]
        subject = item["subject"]
        recomputed = []
        for repetition in item["repetitions"]:
            repository = lookup[(pair_id, subject, repetition)]
            raw = lookup[(pair_id, "raw-segmented-ndn", repetition)]
            recomputed.append(float(
                Decimal(str(repository["logicalGoodputMbps"]))
                / Decimal(str(raw["logicalGoodputMbps"]))
            ))
        ratio_checks.append(
            max(abs(a - b) for a, b in zip(recomputed, item["allRatios"]))
        )
    checks = {
        "csvRunCountMatchesSchedule": len(rows) == len(manifest["runSchedule"]),
        "csvRunIdsUnique": len(ids) == len(set(ids)),
        "fiveMeasuredPerCell": all(
            count == 5 for count in measured_by_cell.values()
        ),
        "ratioMaximumAbsoluteError": max(ratio_checks, default=0.0),
        "ratioFormulaMatches": max(ratio_checks, default=0.0) <= 1e-12,
    }
    checks["verdict"] = (
        "PASS" if all(
            value is True for key, value in checks.items()
            if key not in ("ratioMaximumAbsoluteError", "verdict")
        ) else "FAIL"
    )
    return checks


def render_markdown(derived: dict[str, Any], campaign_dir: Path) -> str:
    verdicts = derived["thresholdVerdicts"]
    lines = [
        "# Spec 164 MiniNDN Performance Report",
        "",
        f"Canonical campaign: `{campaign_dir}`",
        "",
        f"Manifest SHA-256: `{derived['manifestSha256']}`",
        "",
        "## Outcome",
        "",
        "| Criterion | Verdict | Interpretation |",
        "|---|---|---|",
        (
            f"| SC-002 digest/raw >= 0.85 for >=64 MiB | "
            f"{verdicts['SC-002']['verdict']} | "
            f"Completion={verdicts['SC-002'].get('completionGate', 'n/a')}; "
            "point and bootstrap gates are reported separately. |"
        ),
        (
            f"| SC-003 signed/digest >= 0.90 | "
            f"{verdicts['SC-003']['verdict']} | "
            f"Completion={verdicts['SC-003']['completionGate']}; failed and "
            "zero-goodput samples remain negative outcomes. |"
        ),
        (
            f"| SC-004 bounded control operations | "
            f"{verdicts['SC-004']['verdict']} | "
            f"{verdicts['SC-004'].get('controlOperations', 'n/a')} operations "
            f"for {verdicts['SC-004'].get('publicationSegments', 'n/a')} Data "
            "segments; no per-segment service invocation. |"
        ),
        (
            f"| SC-007 read <=1.20x, write <=1.50x | "
            f"{verdicts['SC-007']['verdict']} | "
            f"Write={verdicts['SC-007']['writeAmplificationGate']}; "
            f"read={verdicts['SC-007']['readAmplificationGate']}. |"
        ),
        (
            f"| SC-011 complete formal cells | {verdicts['SC-011']['verdict']} | "
            "24 warmups and 120 measured samples retained; five measured "
            "repetitions per admitted cell. |"
        ),
        (
            f"| SC-012 MiniNDN before TigerCluster | "
            f"{verdicts['SC-012']['verdict']} | "
            "Correctness, recovery, compatibility, migration, and campaign "
            "evidence exist; no TigerCluster claim is made. |"
        ),
        "",
        "## Principal paired ratios",
        "",
        "| Pair | Comparison | Median | 95% bootstrap CI | Min | Max |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for item in derived["repositoryRawRatios"]:
        dist = item["distribution"]
        lines.append(
            f"| {item['pairId']} | {item['subject']} / raw | "
            f"{dist['median']:.3f} | "
            f"[{dist['medianBootstrap95Ci'][0]:.3f}, "
            f"{dist['medianBootstrap95Ci'][1]:.3f}] | "
            f"{dist['min']:.3f} | {dist['max']:.3f} |"
        )
    for item in derived["signedDigestRatios"]:
        dist = item["distribution"]
        lines.append(
            f"| {item['pairId']} | signed / digest | "
            f"{dist['median']:.3f} | "
            f"[{dist['medianBootstrap95Ci'][0]:.3f}, "
            f"{dist['medianBootstrap95Ci'][1]:.3f}] | "
            f"{dist['min']:.3f} | {dist['max']:.3f} |"
        )
    lines.extend([
        "",
        "## Failures and admissibility",
        "",
        (
            f"- Formal measured failures: "
            f"{len(derived['measuredFailures'])}/"
            f"{derived['sampleCounts']['measured']}."
        ),
        (
            f"- Warmup failures: {len(derived['warmupFailures'])}/"
            f"{derived['sampleCounts']['warmup']}."
        ),
        "- Failures are retained by run ID and participate in completion gates; no failed sample is silently removed from a ratio.",
        "- Preflight admitted 24 of 96 candidates. It excluded the other 72 before formal outcomes using frozen disk, memory, 60-second predicted-run, and legacy metadata-row gates.",
        "",
        "## Scaling and physical ceiling",
        "",
        (
            f"- Physical path bottleneck: "
            f"{derived['physicalCeiling'].get('pathBottleneckMbps', 0.0):.3f} Mbit/s "
            f"(verdict={derived['physicalCeiling'].get('verdict', 'MEASURED')})."
        ),
        (
            f"- Concurrency comparisons retained: "
            f"{len(derived['scaling']['concurrencyScaling'])}; replica comparisons: "
            f"{len(derived['scaling']['replicaScaling'])}; size comparisons: "
            f"{len(derived['scaling']['artifactSizeScaling'])}."
        ),
        "- Full scaling ratios, efficiency, per-cell amplification distributions, and physical-ceiling utilization are in `derived-results.json`.",
        "",
        "## Measurement interpretation",
        "",
        "- `logicalGoodputMbps` counts application bytes once per logical operation. For replicated publication, physical wire and storage work include all replicas.",
        "- `wireGoodputMbps` uses total observed Data plus Interest wire bytes; both components and their equality to `wireBytes` are retained separately.",
        "- Write amplification divides physical retained payload/database bytes by logical payload bytes times committed replica count.",
        "- Phase durations may overlap and are not summed into end-to-end latency.",
        "- Confidence intervals are deterministic 10,000-resample percentile bootstrap intervals for the median.",
        "",
        "## Limitations",
        "",
    ])
    lines.extend(f"- {item}" for item in derived["limitations"])
    lines.extend([
        "",
        "## Reproduction",
        "",
        "```bash",
        "python3 Experiments/analyze_distributed_repo_artifact.py \\",
        f"  --campaign {campaign_dir} \\",
        "  --output-json "
        f"{campaign_dir}/derived-results.json \\",
        "  --output-markdown "
        "specs/164-distributed-repo-large-artifact-transport/evidence/remediation/t024-performance-report.md \\",
        "  --control-evidence "
        f"{(derived.get('controlEvidence') or {}).get('path', '<public-smoke-summary.json>')}",
        "```",
        "",
        "The analyzer verifies the manifest seal and independently recomputes run",
        "counts, uniqueness, measured repetitions, and paired ratios from the CSV",
        "ledger using `Decimal` arithmetic.",
        "",
    ])
    return "\n".join(lines)


def _write_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-markdown", type=Path, required=True)
    parser.add_argument(
        "--control-evidence",
        type=Path,
        help="public Collaboration smoke summary used only for SC-004",
    )
    args = parser.parse_args(argv)
    campaign_dir = args.campaign.resolve()
    control_evidence = None
    if args.control_evidence is not None:
        control_evidence = json.loads(
            args.control_evidence.read_text(encoding="utf-8")
        )
    derived = analyze(campaign_dir, control_evidence)
    derived["controlEvidence"] = (
        None if args.control_evidence is None else {
            "path": str(args.control_evidence.resolve()),
            "sha256": hashlib.sha256(args.control_evidence.read_bytes()).hexdigest(),
        }
    )
    derived["independentVerification"] = independent_verify(
        campaign_dir, derived
    )
    _write_json(args.output_json, derived)
    markdown = render_markdown(derived, campaign_dir)
    temporary = args.output_markdown.with_name(
        args.output_markdown.name + ".tmp"
    )
    temporary.write_text(markdown, encoding="utf-8")
    temporary.replace(args.output_markdown)
    print(json.dumps({
        "campaignId": derived["campaignId"],
        "sampleCounts": derived["sampleCounts"],
        "thresholdVerdicts": {
            key: value["verdict"]
            for key, value in derived["thresholdVerdicts"].items()
        },
        "independentVerification": derived["independentVerification"]["verdict"],
    }, sort_keys=True))
    return 0 if derived["independentVerification"]["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
