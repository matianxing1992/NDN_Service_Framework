#!/usr/bin/env python3
"""Offline analysis and decision authority for Spec 138."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
EXPERIMENTS = Path(__file__).resolve().parent
if str(EXPERIMENTS) not in sys.path:
    sys.path.insert(0, str(EXPERIMENTS))
import analyze_svs_serial_production_offload as base  # noqa: E402


SCHEMA = "spec138.analysis.v1"
RATES = (1000, 800, 600, 400, 200)
MODES = ("face-serial", "worker-serial")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid JSON authority {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON authority must be an object: {path}")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = sorted({key for row in rows for key in row})
    with path.open("w", encoding="utf-8", newline="") as output:
        if not fields:
            output.write("\n")
            return
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def cell_metrics(
    directory: Path, campaign_id: str, config: dict[str, Any]
) -> dict[str, Any]:
    peers = {
        peer: base.parse_peer_directory(
            directory,
            campaign_id,
            config["cellId"],
            peer,
            int(config["rate"]),
        )
        for peer in base.PEERS
    }
    producer = peers["peer-a"]
    receiver = peers["peer-b"]
    pstats = producer["validated"]["stats"]
    psummary = producer["validated"]["summary"]
    rsummary = receiver["validated"]["summary"]
    committed = int(
        psummary.get("committedMeasured", psummary["attemptedMeasured"])
    )
    delivered = int(rsummary["deliveredMeasured"])
    warmup = int(config["warmup"])
    measure = int(config["measure"])
    active_seconds = warmup + measure
    triggers = max(1, int(pstats["triggers"]))
    metrics = {
        "cellId": config["cellId"],
        "mode": config["mode"],
        "rate": int(config["rate"]),
        "warmupSeconds": warmup,
        "measureSeconds": measure,
        "drainSeconds": int(config["drain"]),
        "attemptedMeasured": int(psummary["attemptedMeasured"]),
        "attemptedPps": int(psummary["attemptedMeasured"]) / measure,
        "attemptedRateError": producer["validated"]["attemptedRateError"],
        "committedMeasured": committed,
        "deliveredMeasured": delivered,
        "deliveryRatio": delivered / committed if committed else 0.0,
        "heartbeatP99Ns": int(psummary["heartbeatP99Ns"]),
        "deliveryP99Ns": int(rsummary.get("deliveryP99Ns", 0)),
        "faceCpuNs": int(pstats["faceCpuNs"]),
        "workerCpuNs": int(pstats["workerCpuNs"]),
        "serialCpuNs": int(pstats["serialCpuNs"]),
        "faceCpuPerProductionNs": int(pstats["faceCpuNs"]) / triggers,
        "faceProductionCpuFraction": int(pstats["faceCpuNs"])
        / (active_seconds * 1_000_000_000),
        "queueWaitNs": int(pstats["queueWaitNs"]),
        "workerServiceNs": int(pstats["workerServiceNs"]),
        "extraBuildNs": int(pstats["extraBuildNs"]),
        "encodeNs": int(pstats["encodeNs"]),
        "signNs": int(pstats["signNs"]),
        "faceQueueWaitNs": int(pstats["faceQueueWaitNs"]),
        "faceFinalizeNs": int(pstats["faceFinalizeNs"]),
        "triggers": int(pstats["triggers"]),
        "completed": int(pstats["completed"])
        if config["mode"] == "worker-serial"
        else int(pstats["serialCompleted"]),
        "staleSent": int(pstats["staleSent"]),
        "staleDropped": int(pstats["staleDropped"]),
        "fallbacks": sum(
            peer["validated"]["fallbacks"] for peer in peers.values()
        ),
        "maxActiveSigners": max(
            peer["validated"]["maxActiveSigners"] for peer in peers.values()
        ),
        "ownerViolations": sum(
            peer["validated"]["ownerViolations"] for peer in peers.values()
        ),
        "productionAccountingRemainder": sum(
            peer["validated"]["productionAccountingRemainder"]
            for peer in peers.values()
        ),
        "publicationAccountingRemainder": sum(
            peer["validated"]["publicationAccountingRemainder"]
            for peer in peers.values()
        ),
        "shutdownDrained": all(
            peer["validated"]["shutdownDrained"] for peer in peers.values()
        ),
        "resourceComplete": all(
            peer["validated"]["resourceComplete"] for peer in peers.values()
        ),
    }
    metrics["pressureGate"] = (
        metrics["faceProductionCpuFraction"] >= 0.10
        or metrics["heartbeatP99Ns"] >= 2_000_000
    )
    metrics["admissionChecks"] = admission_checks(metrics)
    metrics["admissible"] = all(metrics["admissionChecks"].values())
    return metrics


def admission_checks(metrics: dict[str, Any]) -> dict[str, bool]:
    return {
        "attempted_rate_within_2_percent":
            float(metrics["attemptedRateError"]) <= 0.02,
        "delivery_ratio_at_least_99_percent":
            float(metrics["deliveryRatio"]) >= 0.99,
        "max_active_sync_signers_one":
            int(metrics["maxActiveSigners"]) == 1,
        "production_fallback_zero": int(metrics["fallbacks"]) == 0,
        "production_accounting_remainder_zero":
            int(metrics["productionAccountingRemainder"]) == 0,
        "publication_accounting_remainder_zero":
            int(metrics["publicationAccountingRemainder"]) == 0,
        "thread_owner_valid": int(metrics["ownerViolations"]) == 0,
        "event_and_resource_files_complete": bool(metrics["resourceComplete"]),
        "shutdown_drained": bool(metrics["shutdownDrained"]),
    }


def select_control_rate(rows: list[dict[str, Any]]) -> dict[str, Any]:
    if [int(row["rate"]) for row in rows] != list(RATES[: len(rows)]):
        raise RuntimeError("control calibration order escaped registered ladder")
    selected = next(
        (
            int(row["rate"])
            for row in rows
            if bool(row["admissible"]) and bool(row["pressureGate"])
        ),
        None,
    )
    return {
        "schema": "spec138.rate-selection.v1",
        "selectedRate": selected,
        "reason": (
            "HIGHEST_VALID_CONTROL_PRESSURE"
            if selected is not None
            else "NO_TESTABLE_PRESSURE_POINT"
        ),
        "controlOnly": True,
        "registeredRates": list(RATES),
        "calibration": rows,
    }


def paired_contrast(
    pair: int, face: dict[str, Any], worker: dict[str, Any]
) -> dict[str, Any]:
    def relief(left: float, right: float) -> float:
        return (left - right) / left if left else 0.0

    return {
        "pair": pair,
        "faceCell": face["cellId"],
        "workerCell": worker["cellId"],
        "pairAdmissible": bool(face["admissible"] and worker["admissible"]),
        "faceCpuRelief": relief(
            float(face["faceCpuPerProductionNs"]),
            float(worker["faceCpuPerProductionNs"]),
        ),
        "heartbeatP99Improvement": relief(
            float(face["heartbeatP99Ns"]), float(worker["heartbeatP99Ns"])
        ),
        "deliveryRatioChange":
            float(worker["deliveryRatio"]) - float(face["deliveryRatio"]),
        "deliveryP99Improvement": relief(
            float(face["deliveryP99Ns"]), float(worker["deliveryP99Ns"])
        ),
        "workerQueueWaitNs": int(worker["queueWaitNs"]),
        "workerServiceNs": int(worker["workerServiceNs"]),
    }


def classify(
    run_rows: list[dict[str, Any]], pairs: list[dict[str, Any]]
) -> dict[str, Any]:
    all_admissible = len(run_rows) == 6 and all(
        bool(row["admissible"]) for row in run_rows
    )
    cpu_passes = sum(
        bool(row["pairAdmissible"]) and row["faceCpuRelief"] >= 0.50
        for row in pairs
    )
    heartbeat_passes = sum(
        bool(row["pairAdmissible"])
        and row["heartbeatP99Improvement"] >= 0.20
        for row in pairs
    )
    delivery_ratio_no_harm = all(
        row["deliveryRatioChange"] >= -0.01 for row in pairs
    )
    delivery_p99_harm_pairs = sum(
        row["deliveryP99Improvement"] < -0.10 for row in pairs
    )
    worker_worse_pairs = sum(
        row["heartbeatP99Improvement"] < -0.20
        and row["deliveryP99Improvement"] < -0.10
        for row in pairs
    )
    queue_clean = all(
        int(row["fallbacks"]) == 0 and bool(row["shutdownDrained"])
        for row in run_rows
    )
    necessary = (
        all_admissible
        and cpu_passes >= 2
        and heartbeat_passes >= 2
        and delivery_ratio_no_harm
        and delivery_p99_harm_pairs < 2
        and queue_clean
    )
    if not all_admissible:
        verdict = "INADMISSIBLE"
    elif not delivery_ratio_no_harm or delivery_p99_harm_pairs >= 2:
        verdict = "TRADE_OFF"
    elif worker_worse_pairs >= 2:
        verdict = "WORKER_WORSE"
    elif necessary:
        verdict = "NECESSARY_AT_TESTED_BOUNDARY"
    elif cpu_passes >= 2:
        verdict = "FACE_RELIEF_ONLY"
    else:
        verdict = "NOT_NECESSARY_AT_TESTED_BOUNDARY"
    return {
        "schema": "spec138.conclusion.v1",
        "verdict": verdict,
        "allSixAdmissible": all_admissible,
        "faceCpuReliefPairsAtLeast50Percent": cpu_passes,
        "heartbeatImprovementPairsAtLeast20Percent": heartbeat_passes,
        "deliveryRatioNoHarm": delivery_ratio_no_harm,
        "deliveryP99HarmPairsAbove10Percent": delivery_p99_harm_pairs,
        "queueClean": queue_clean,
        "necessaryPredicate": necessary,
    }


def verify_artifact_records(cell: Path) -> None:
    authority = load_json(cell / "artifact-hashes.json")
    for record in authority.get("records", []):
        path = cell / record["path"]
        if not path.is_file() or sha256_file(path) != record["sha256"]:
            raise RuntimeError(f"cell artifact changed: {path}")


def traffic_metrics(cell: Path, config: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "cellId": config["cellId"],
        "mode": config["mode"],
    }
    for peer in base.PEERS:
        path = cell / f"{peer}-ndndump.log"
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = [line for line in text.splitlines() if line.strip()]
        result[f"{peer}CaptureLines"] = len(lines)
        result[f"{peer}InterestLines"] = sum(
            "Interest" in line or "INTEREST" in line for line in lines
        )
        result[f"{peer}DataLines"] = sum(
            "Data" in line or "DATA" in line for line in lines
        )
        result[f"{peer}SyncNameLines"] = sum(
            "/spec137/sync/" in line for line in lines
        )
        result[f"{peer}PublicationNameLines"] = sum(
            "/spec137/publication" in line for line in lines
        )
    return result


def analyze_campaign(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    manifest = load_json(campaign / "campaign-manifest.json")
    seal = (campaign / ".sealed").read_text(encoding="utf-8").strip()
    if seal != sha256_file(campaign / "campaign-manifest.json"):
        raise RuntimeError("campaign manifest seal mismatch")
    if manifest.get("schema") != "spec138.campaign.v1":
        raise RuntimeError("campaign schema mismatch")
    receipts = sorted((campaign / "receipts").glob("*.json"))
    if len(receipts) != 6:
        raise RuntimeError(f"expected six formal receipts, got {len(receipts)}")
    run_rows: list[dict[str, Any]] = []
    traffic_rows: list[dict[str, Any]] = []
    host_rows: list[dict[str, Any]] = []
    for config, receipt_path in zip(manifest["cells"], receipts):
        receipt = load_json(receipt_path)
        if (
            receipt.get("ordinal") != config["ordinal"]
            or receipt.get("mode") != config["mode"]
            or receipt.get("retryCount") != 0
        ):
            raise RuntimeError(f"receipt/config mismatch: {receipt_path}")
        cell = campaign / "formal/cells" / config["cellId"]
        verify_artifact_records(cell)
        row = cell_metrics(cell, campaign.name, config)
        row["ordinal"] = config["ordinal"]
        row["pair"] = config["pair"]
        row["receiptAdmissible"] = bool(receipt["admissible"])
        if row["admissible"] != row["receiptAdmissible"]:
            raise RuntimeError(f"receipt admission mismatch: {receipt_path}")
        run_rows.append(row)
        traffic_rows.append(traffic_metrics(cell, config))
        qpath = Path(receipt["quiescenceRecord"])
        qvalue = load_json(qpath)
        host_rows.append(
            {
                "ordinal": config["ordinal"],
                "cellId": config["cellId"],
                "mode": config["mode"],
                "quiescencePassed": qvalue["passed"],
                "maxSelectedBusyRatio": max(
                    qvalue["final"]["busyRatios"].values()
                ),
                "attemptCount": len(qvalue["attempts"]),
            }
        )
    pairs: list[dict[str, Any]] = []
    for pair in (1, 2, 3):
        rows = [row for row in run_rows if row["pair"] == pair]
        face = next(row for row in rows if row["mode"] == "face-serial")
        worker = next(row for row in rows if row["mode"] == "worker-serial")
        pairs.append(paired_contrast(pair, face, worker))
    conclusion = classify(run_rows, pairs)
    output = campaign / "analysis"
    write_csv(output / "run-metrics.csv", run_rows)
    write_csv(output / "paired-contrasts.csv", pairs)
    write_csv(
        output / "stage-breakdown.csv",
        [
            {
                key: row[key]
                for key in (
                    "ordinal",
                    "pair",
                    "cellId",
                    "mode",
                    "faceCpuNs",
                    "workerCpuNs",
                    "serialCpuNs",
                    "queueWaitNs",
                    "workerServiceNs",
                    "extraBuildNs",
                    "encodeNs",
                    "signNs",
                    "faceQueueWaitNs",
                    "faceFinalizeNs",
                )
            }
            for row in run_rows
        ],
    )
    write_csv(output / "traffic-breakdown.csv", traffic_rows)
    write_csv(output / "host-load.csv", host_rows)
    write_json(output / "conclusion.json", conclusion)
    return {
        "schema": SCHEMA,
        "campaign": str(campaign),
        "runs": run_rows,
        "pairs": pairs,
        "traffic": traffic_rows,
        "host": host_rows,
        "conclusion": conclusion,
    }


def write_report(path: Path, result: dict[str, Any]) -> None:
    conclusion = result["conclusion"]
    lines = [
        "# Spec 138 Single-Worker Necessity Report",
        "",
        "## Verdict",
        "",
        f"**`{conclusion['verdict']}`**",
        "",
        "This result evaluates asynchronous single-worker NDN-SVS Sync-production "
        "offload. `publishAsync()` is common to both modes; RSA, multi-worker, "
        "cross-version, and universal-necessity claims are excluded.",
        "",
        "## Run-Level Results",
        "",
        "| Ordinal | Pair | Mode | Attempted pps | Delivery | Face CPU/prod ns | Heartbeat p99 ms | Delivery p99 ms | Admissible |",
        "|---:|---:|---|---:|---:|---:|---:|---:|---|",
    ]
    for row in result["runs"]:
        lines.append(
            f"| {row['ordinal']} | {row['pair']} | {row['mode']} | "
            f"{row['attemptedPps']:.3f} | {row['deliveryRatio']:.6f} | "
            f"{row['faceCpuPerProductionNs']:.0f} | "
            f"{row['heartbeatP99Ns'] / 1e6:.3f} | "
            f"{row['deliveryP99Ns'] / 1e6:.3f} | "
            f"{'yes' if row['admissible'] else 'no'} |"
        )
    lines.extend(
        [
            "",
            "## Paired Effects",
            "",
            "| Pair | Face CPU relief | Heartbeat p99 improvement | Delivery-ratio change | Delivery-p99 improvement |",
            "|---:|---:|---:|---:|---:|",
        ]
    )
    for row in result["pairs"]:
        lines.append(
            f"| {row['pair']} | {row['faceCpuRelief']:.2%} | "
            f"{row['heartbeatP99Improvement']:.2%} | "
            f"{row['deliveryRatioChange']:+.4f} | "
            f"{row['deliveryP99Improvement']:.2%} |"
        )
    lines.extend(
        [
            "",
            "## Registered Decision Predicates",
            "",
            f"- All six admissible: `{conclusion['allSixAdmissible']}`",
            "- Face CPU relief >=50% pairs: "
            f"`{conclusion['faceCpuReliefPairsAtLeast50Percent']}`",
            "- Heartbeat improvement >=20% pairs: "
            f"`{conclusion['heartbeatImprovementPairsAtLeast20Percent']}`",
            f"- Delivery-ratio no-harm: `{conclusion['deliveryRatioNoHarm']}`",
            "- Delivery-p99 harm >10% pairs: "
            f"`{conclusion['deliveryP99HarmPairsAbove10Percent']}`",
            f"- One-worker queue clean: `{conclusion['queueClean']}`",
            "",
            "The raw run, paired, stage, traffic, host-load, and conclusion "
            "artifacts are under the campaign `analysis/` directory.",
            "",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(argv)
    result = analyze_campaign(args.campaign)
    write_report(args.report, result)
    print(json.dumps(result["conclusion"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
