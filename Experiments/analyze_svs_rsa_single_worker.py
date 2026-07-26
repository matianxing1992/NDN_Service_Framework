#!/usr/bin/env python3
"""Analyze the paired Spec 136 MiniNDN RSA single-worker campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any


PEERS = ("peer-a", "peer-b")
MODES = ("face-inline-rsa", "worker-rsa")
RSA_SIGNATURE_TYPE = 1
SYNC_BATCH_WINDOW_MS = 5
PUBLICATION_FETCH_WINDOW = 64
MAX_SIGNER_UTILIZATION = 0.90
MIN_DELIVERY_RATIO = 0.98


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator else 0.0


def mean_time_us(rows: list[dict[str, Any]], ns_key: str, count_key: str) -> float:
    return ratio(
        sum(row[ns_key] for row in rows),
        sum(row[count_key] for row in rows),
    ) / 1000.0


def validate_peer(
    summary: dict[str, Any], mode: str, rate: int, peer: str, measure_seconds: int
) -> list[str]:
    errors = []
    expected_workers = 1 if mode == "worker-rsa" else 0
    expected = {
        "schema": "spec136.peer-summary.v6",
        "peer": peer,
        "mode": mode,
        "ratePerPeer": rate,
        "publicationWorkers": expected_workers,
        "dataSignatureType": RSA_SIGNATURE_TYPE,
        "interestSignatureType": RSA_SIGNATURE_TYPE,
    }
    for key, value in expected.items():
        if summary.get(key) != value:
            errors.append(f"{peer}:{key}={summary.get(key)!r}, expected {value!r}")
    scheduled = rate * measure_seconds
    attempted = int(summary.get("attemptedMeasured", 0))
    attempted_ratio = ratio(attempted, scheduled)
    delivered = int(summary.get("deliveredMeasured", 0))
    delivery_ratio = ratio(delivered, attempted)
    if summary.get("scheduledMeasured") != scheduled:
        errors.append(
            f"{peer}:scheduledMeasured={summary.get('scheduledMeasured')}, "
            f"expected {scheduled}"
        )
    if not 0.98 <= attempted_ratio <= 1.02:
        errors.append(
            f"{peer}:attempted admission ratio={attempted_ratio:.6f}, "
            "expected [0.98, 1.02]"
        )
    if delivery_ratio < MIN_DELIVERY_RATIO:
        errors.append(
            f"LOAD_UNSUSTAINED:{peer}:delivery ratio={delivery_ratio:.6f}, "
            f"expected >= {MIN_DELIVERY_RATIO:.2f}"
        )
    if summary.get("pacerKind") != "independent-app-thread":
        errors.append(f"{peer}:independent APP pacer not proved")
    if summary.get("syncInterestBatching") is not True:
        errors.append(f"{peer}:Sync batching is not enabled")
    if summary.get("syncInterestBatchWindowMs") != SYNC_BATCH_WINDOW_MS:
        errors.append(f"{peer}:unexpected Sync batch window")
    if summary.get("publicationFetchWindow") != PUBLICATION_FETCH_WINDOW:
        errors.append(
            f"{peer}:publicationFetchWindow="
            f"{summary.get('publicationFetchWindow')}, expected "
            f"{PUBLICATION_FETCH_WINDOW}"
        )
    face_thread = int(summary.get("faceThreadHash", 0))
    pacer_thread = int(summary.get("pacerThreadHash", 0))
    call_thread = int(summary.get("publishCallThreadHash", 0))
    if face_thread == 0 or pacer_thread == 0 or face_thread == pacer_thread:
        errors.append(f"{peer}:Face and APP pacer thread identities are invalid")
    if summary.get("pacerFailed") is not False:
        errors.append(f"{peer}:pacer failed: {summary.get('pacerError', '')}")
    if mode == "face-inline-rsa":
        if call_thread != face_thread or summary.get("publishCallsOnFace", 0) <= 0:
            errors.append(f"{peer}:control publication API did not execute on Face")
        if summary.get("publishCallsOnPacer", 0) != 0:
            errors.append(f"{peer}:control publication API executed on APP pacer")
    else:
        if call_thread != pacer_thread or summary.get("publishCallsOnPacer", 0) <= 0:
            errors.append(f"{peer}:worker publication API did not execute on APP pacer")
        if summary.get("publishCallsOnFace", 0) != 0:
            errors.append(f"{peer}:worker publication API executed on Face")
    for key in ("attemptedMeasured", "acceptedMeasured", "deliveredMeasured"):
        if summary.get(key, 0) <= 0:
            errors.append(f"{peer}:{key} is not positive")
    for key in (
        "publishErrors",
        "invalid",
        "selfDeliveries",
        "dataInvalid",
        "interestInvalid",
        "workerFailed",
        "workerCancelled",
        "workerOutstanding",
        "faceDispatchAbandoned",
    ):
        if summary.get(key, 0) != 0:
            errors.append(f"{peer}:{key}={summary.get(key)}")
    if summary.get("maxActiveSigners") != 1:
        errors.append(
            f"{peer}:maxActiveSigners={summary.get('maxActiveSigners')}, expected 1"
        )
    if summary.get("dataSignCalls", 0) <= 0 or summary.get("dataValid", 0) <= 0:
        errors.append(f"{peer}:RSA Data sign/validation was not observed")
    if (
        summary.get("interestSignCalls", 0) <= 0
        or summary.get("interestValid", 0) <= 0
    ):
        errors.append(f"{peer}:RSA Interest sign/validation was not observed")
    if mode == "worker-rsa":
        if summary.get("workerPrepared", 0) <= 0:
            errors.append(f"{peer}:worker prepared no publications")
        if summary.get("workerMaxPending", 0) <= 0:
            errors.append(f"{peer}:worker queue was never used")
    elif summary.get("workerPrepared", 0) != 0:
        errors.append(f"{peer}:inline mode used worker preparation")
    data_calls = int(summary.get("dataSignCalls", 0))
    interest_calls = int(summary.get("interestSignCalls", 0))
    if data_calls > 0 and interest_calls > 0:
        data_mean = summary.get("dataSignServiceNs", 0) / data_calls
        interest_mean = summary.get("interestSignServiceNs", 0) / interest_calls
        signer_utilization = (
            2 * rate * data_mean
            + min(rate, 1000 / SYNC_BATCH_WINDOW_MS) * interest_mean
        ) / 1_000_000_000
        if signer_utilization > MAX_SIGNER_UTILIZATION:
            errors.append(
                f"{peer}:estimated signer utilization={signer_utilization:.6f}, "
                f"expected <= {MAX_SIGNER_UTILIZATION:.2f}"
            )
    return errors


def aggregate_cell(
    cell: Path,
    terminal: dict[str, Any],
    measure_seconds: int,
) -> tuple[dict[str, Any], list[str], list[str]]:
    mode = terminal["mode"]
    rate = int(terminal["ratePerPeer"])
    summaries = [load_json(cell / f"{peer}-summary.json") for peer in PEERS]
    errors = []
    load_outcomes = []
    terminal_status = terminal.get("status")
    interpreted_terminal_status = terminal_status
    if terminal_status in ("HARNESS_INVALID", "harness_invalid"):
        legacy_admission = list(terminal.get("admissionErrors", []))
        if legacy_admission and all(
            "delivery-ratio=" in item for item in legacy_admission
        ):
            interpreted_terminal_status = "LOAD_UNSUSTAINED"
        else:
            errors.append(f"{cell.name}:terminal={terminal_status}")
    elif terminal_status not in (
        "complete",
        "COMPLETE",
        "LOAD_UNSUSTAINED",
        "load_unsustained",
    ):
        errors.append(f"{cell.name}:terminal={terminal_status}")
    if terminal.get("bothPeersPublishAndSubscribe") is not True:
        errors.append(f"{cell.name}:not bidirectional")
    for peer, summary in zip(PEERS, summaries):
        for issue in validate_peer(summary, mode, rate, peer, measure_seconds):
            if issue.startswith("LOAD_UNSUSTAINED:"):
                load_outcomes.append(issue)
            else:
                errors.append(issue)
    if load_outcomes and interpreted_terminal_status in ("complete", "COMPLETE"):
        interpreted_terminal_status = "LOAD_UNSUSTAINED"

    peer_attempted = {
        peer: summary["attemptedMeasured"] / measure_seconds
        for peer, summary in zip(PEERS, summaries)
    }
    peer_admission = {
        peer: 0.98 * rate <= attempted_pps <= 1.02 * rate
        for peer, attempted_pps in peer_attempted.items()
    }
    attempted = sum(row["attemptedMeasured"] for row in summaries)
    accepted = sum(row["acceptedMeasured"] for row in summaries)
    delivered = sum(row["deliveredMeasured"] for row in summaries)
    delivered_warmup = sum(row["deliveredWarmup"] for row in summaries)
    skipped = sum(row["skippedMeasured"] for row in summaries)
    committed = sum(row["workerCommitted"] for row in summaries)
    worker_accepted = sum(row["workerAccepted"] for row in summaries)
    worker_started = sum(row.get("workerStarted", 0) for row in summaries)
    worker_prepared = sum(row["workerPrepared"] for row in summaries)
    worker_queue_total = sum(row["workerQueueWaitNsTotal"] for row in summaries)
    worker_service_total = sum(row["workerServiceNsTotal"] for row in summaries)
    row = {
        "cellId": terminal["cellId"],
        "mode": mode,
        "ratePerPeer": rate,
        "recordedTerminalStatus": terminal_status,
        "interpretedTerminalStatus": interpreted_terminal_status,
        "aggregateOfferedPps": 2 * rate,
        "scheduledMeasured": sum(row["scheduledMeasured"] for row in summaries),
        "attemptedMeasured": attempted,
        "peerAAttemptedPps": peer_attempted["peer-a"],
        "peerBAttemptedPps": peer_attempted["peer-b"],
        "bothPeersAttemptedAdmitted": all(peer_admission.values()),
        "acceptedMeasured": accepted,
        "committedAllPhases": committed,
        "deliveredMeasured": delivered,
        "deliveredWarmup": delivered_warmup,
        "skippedMeasured": skipped,
        "attemptedPpsPerPeer": ratio(attempted, 2 * measure_seconds),
        "deliveredPpsPerPeer": ratio(delivered, 2 * measure_seconds),
        "deliveryRatio": ratio(delivered, attempted),
        "loadSustained": ratio(delivered, attempted) >= MIN_DELIVERY_RATIO,
        "releaseP99Us": max(row["releaseP99Ns"] for row in summaries) / 1000.0,
        "heartbeatP99Us": max(row["heartbeatP99Ns"] for row in summaries) / 1000.0,
        "heartbeatSkipped": sum(row["heartbeatSkipped"] for row in summaries),
        "deliveryP99Us": max(row["deliveryP99Ns"] for row in summaries) / 1000.0,
        "duplicates": sum(row["duplicates"] for row in summaries),
        "dataSignMeanUs": mean_time_us(summaries, "dataSignNs", "dataSignCalls"),
        "dataSignWaitMeanUs": mean_time_us(
            summaries, "dataSignWaitNs", "dataSignCalls"
        ),
        "dataSignServiceMeanUs": mean_time_us(
            summaries, "dataSignServiceNs", "dataSignCalls"
        ),
        "interestSignMeanUs": mean_time_us(
            summaries, "interestSignNs", "interestSignCalls"
        ),
        "interestSignWaitMeanUs": mean_time_us(
            summaries, "interestSignWaitNs", "interestSignCalls"
        ),
        "interestSignServiceMeanUs": mean_time_us(
            summaries, "interestSignServiceNs", "interestSignCalls"
        ),
        "dataVerifyMeanUs": mean_time_us(summaries, "dataVerifyNs", "dataValid"),
        "interestVerifyMeanUs": mean_time_us(
            summaries, "interestVerifyNs", "interestValid"
        ),
        "workerAccepted": worker_accepted,
        "workerStarted": worker_started,
        "workerPrepared": worker_prepared,
        "workerQueueWaitMeanUs": ratio(
            worker_queue_total, worker_started or worker_prepared
        )
        / 1000.0,
        "workerQueueWaitMaxUs": max(
            row["workerQueueWaitNsMax"] for row in summaries
        )
        / 1000.0,
        "workerServiceMeanUs": ratio(worker_service_total, worker_started or worker_prepared)
        / 1000.0,
        "workerServiceMaxUs": max(row["workerServiceNsMax"] for row in summaries)
        / 1000.0,
        "workerMaxPending": max(row["workerMaxPending"] for row in summaries),
        "maxEstimatedSignerUtilization": max(
            (
                2
                * rate
                * ratio(row["dataSignServiceNs"], row["dataSignCalls"])
                + min(rate, 1000 / SYNC_BATCH_WINDOW_MS)
                * ratio(row["interestSignServiceNs"], row["interestSignCalls"])
            )
            / 1_000_000_000
            for row in summaries
        ),
        "workerCommittedAtMeasureEnd": sum(
            row["workerCommittedAtMeasureEnd"] for row in summaries
        ),
        "workerOutstandingAtMeasureEnd": sum(
            row["workerOutstandingAtMeasureEnd"] for row in summaries
        ),
        "workerOutstandingAtDrainEnd": sum(
            row["workerOutstanding"] for row in summaries
        ),
        "faceDispatchPendingAtMeasureEnd": sum(
            row["faceDispatchPendingAtMeasureEnd"] for row in summaries
        ),
        "faceDispatchAbandoned": sum(
            row["faceDispatchAbandoned"] for row in summaries
        ),
        "publicationFetchWindow": max(
            row["publicationFetchWindow"] for row in summaries
        ),
        "publicationFetchQueuedAtMeasureEnd": sum(
            row["publicationFetchQueuedAtMeasureEnd"] for row in summaries
        ),
        "publicationFetchPendingAtMeasureEnd": sum(
            row["publicationFetchPendingAtMeasureEnd"] for row in summaries
        ),
        "publicationFetchQueuedAtDrainEnd": sum(
            row["publicationFetchQueuedAtDrainEnd"] for row in summaries
        ),
        "publicationFetchPendingAtDrainEnd": sum(
            row["publicationFetchPendingAtDrainEnd"] for row in summaries
        ),
        "publicationFetchDispatchedAtMeasureEnd": sum(
            row["publicationFetchDispatchedAtMeasureEnd"] for row in summaries
        ),
        "publicationFetchDispatchedAtDrainEnd": sum(
            row["publicationFetchDispatchedAtDrainEnd"] for row in summaries
        ),
        "publicationFetchDataAtMeasureEnd": sum(
            row["publicationFetchDataAtMeasureEnd"] for row in summaries
        ),
        "publicationFetchDataAtDrainEnd": sum(
            row["publicationFetchDataAtDrainEnd"] for row in summaries
        ),
        "publicationFetchNacksAtMeasureEnd": sum(
            row["publicationFetchNacksAtMeasureEnd"] for row in summaries
        ),
        "publicationFetchNacksAtDrainEnd": sum(
            row["publicationFetchNacksAtDrainEnd"] for row in summaries
        ),
        "publicationFetchTimeoutsAtMeasureEnd": sum(
            row["publicationFetchTimeoutsAtMeasureEnd"] for row in summaries
        ),
        "publicationFetchTimeoutsAtDrainEnd": sum(
            row["publicationFetchTimeoutsAtDrainEnd"] for row in summaries
        ),
        "mappingFetchQueuedAtMeasureEnd": sum(
            row["mappingFetchQueuedAtMeasureEnd"] for row in summaries
        ),
        "mappingFetchPendingAtMeasureEnd": sum(
            row["mappingFetchPendingAtMeasureEnd"] for row in summaries
        ),
        "mappingFetchQueuedAtDrainEnd": sum(
            row["mappingFetchQueuedAtDrainEnd"] for row in summaries
        ),
        "mappingFetchPendingAtDrainEnd": sum(
            row["mappingFetchPendingAtDrainEnd"] for row in summaries
        ),
        "mappingFetchDispatchedAtMeasureEnd": sum(
            row["mappingFetchDispatchedAtMeasureEnd"] for row in summaries
        ),
        "mappingFetchDispatchedAtDrainEnd": sum(
            row["mappingFetchDispatchedAtDrainEnd"] for row in summaries
        ),
        "mappingFetchDataAtMeasureEnd": sum(
            row["mappingFetchDataAtMeasureEnd"] for row in summaries
        ),
        "mappingFetchDataAtDrainEnd": sum(
            row["mappingFetchDataAtDrainEnd"] for row in summaries
        ),
        "mappingFetchNacksAtMeasureEnd": sum(
            row["mappingFetchNacksAtMeasureEnd"] for row in summaries
        ),
        "mappingFetchNacksAtDrainEnd": sum(
            row["mappingFetchNacksAtDrainEnd"] for row in summaries
        ),
        "mappingFetchTimeoutsAtMeasureEnd": sum(
            row["mappingFetchTimeoutsAtMeasureEnd"] for row in summaries
        ),
        "mappingFetchTimeoutsAtDrainEnd": sum(
            row["mappingFetchTimeoutsAtDrainEnd"] for row in summaries
        ),
        "maxRssKiB": max(row["maxRssKiB"] for row in summaries),
        "securityAndAccountingValid": not errors,
    }
    return row, errors, sorted(set(load_outcomes))


def classify_rate(control: dict[str, Any], worker: dict[str, Any]) -> dict[str, Any]:
    ratio_delta = worker["deliveryRatio"] - control["deliveryRatio"]
    delivered_from_zero = (
        control["deliveredPpsPerPeer"] == 0
        and worker["deliveredPpsPerPeer"] > 0
    )
    delivered_gain = (
        1.0
        if delivered_from_zero
        else ratio(worker["deliveredPpsPerPeer"], control["deliveredPpsPerPeer"])
        - 1.0
    )
    heartbeat_gain = 1.0 - ratio(
        worker["heartbeatP99Us"], control["heartbeatP99Us"]
    )
    delivery_tail_gain = 1.0 - ratio(
        worker["deliveryP99Us"], control["deliveryP99Us"]
    )
    delivery_tail_comparable = (
        abs(worker["deliveryRatio"] - control["deliveryRatio"]) <= 0.01
    )
    control_misses_rate = control["deliveredPpsPerPeer"] < 0.98 * control["ratePerPeer"]
    worker_sustains_rate = worker["deliveredPpsPerPeer"] >= 0.98 * worker["ratePerPeer"]
    capacity_extension = control_misses_rate and worker_sustains_rate
    correctness = (
        control["securityAndAccountingValid"]
        and worker["securityAndAccountingValid"]
        and ratio_delta >= -0.01
    )
    useful = correctness and (
        capacity_extension
        or (
            not control_misses_rate
            and worker_sustains_rate
            and (
                heartbeat_gain >= 0.20
                or (delivery_tail_comparable and delivery_tail_gain >= 0.20)
            )
        )
    )
    return {
        "ratePerPeer": control["ratePerPeer"],
        "verdict": "USEFUL_AT_RATE" if useful else "NOT_USEFUL_AT_RATE",
        "deliveryRatioDelta": ratio_delta,
        "deliveredPpsGain": delivered_gain,
        "deliveredFromZero": delivered_from_zero,
        "heartbeatP99Gain": heartbeat_gain,
        "deliveryP99Gain": delivery_tail_gain,
        "deliveryP99Comparable": delivery_tail_comparable,
        "controlMissesOfferedRate": control_misses_rate,
        "workerSustainsOfferedRate": worker_sustains_rate,
        "capacityExtension": capacity_extension,
    }


def overall_verdict(pairs: list[dict[str, Any]]) -> str:
    useful_rates = {
        row["ratePerPeer"] for row in pairs if row["verdict"] == "USEFUL_AT_RATE"
    }
    rates = sorted(row["ratePerPeer"] for row in pairs)
    if any(a in useful_rates and b in useful_rates for a, b in zip(rates, rates[1:])):
        return "ASYNC_SINGLE_WORKER_USEFUL"
    if any(row["deliveryRatioDelta"] < -0.01 for row in pairs):
        return "REGRESSION"
    if any(row["verdict"] == "USEFUL_AT_RATE" for row in pairs):
        return "TRADE_OFF"
    return "NO_CLEAR_BENEFIT"


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(
    path: Path,
    campaign_kind: str,
    formal: bool,
    verdict: str,
    cells: list[dict[str, Any]],
    errors: list[str],
    load_outcomes: list[str],
    pairs: list[dict[str, Any]],
) -> None:
    lines = [
        "# Spec 136 RSA Single-Worker Analysis",
        "",
        f"- Campaign type: {campaign_kind}",
        f"- Verdict: `{verdict}`",
        f"- Evidence class: `{'FORMAL' if formal else 'NON_FORMAL_DESCRIPTIVE'}`",
        f"- Validation errors: {len(errors)}",
        "",
        "| Cell | Mode | Rate/peer | peer-a attempted | peer-b attempted | "
        "Delivered pps/peer | "
        "Delivery ratio | Signer util | Heartbeat p99 us | Delivery p99 us |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in cells:
        lines.append(
            f"| {row['cellId']} | {row['mode']} | {row['ratePerPeer']} | "
            f"{row['peerAAttemptedPps']:.2f} | "
            f"{row['peerBAttemptedPps']:.2f} | "
            f"{row['deliveredPpsPerPeer']:.2f} | {row['deliveryRatio']:.4f} | "
            f"{row['maxEstimatedSignerUtilization']:.4f} | "
            f"{row['heartbeatP99Us']:.2f} | {row['deliveryP99Us']:.2f} |"
        )
    lines.extend(
        [
            "",
            "## Terminal provenance",
            "",
            "| Cell | Recorded terminal | Analyzer interpretation | Duplicates | "
            "Heartbeat skipped |",
            "|---|---|---|---:|---:|",
        ]
    )
    for row in cells:
        lines.append(
            f"| {row['cellId']} | `{row['recordedTerminalStatus']}` | "
            f"`{row['interpretedTerminalStatus']}` | {row['duplicates']} | "
            f"{row['heartbeatSkipped']} |"
        )
    lines.extend(
        [
            "",
            "## Queue and phase diagnostics",
            "",
            "| Cell | Warmup delivered | Measured delivered | Committed @ measure end | "
            "Outstanding @ measure end | Outstanding @ drain end | "
            "Publication fetch queued @ measure/drain | "
            "Publication fetch pending @ measure/drain | "
            "Publication fetch dispatched @ measure/drain | "
            "Publication Data/Nack/timeout @ measure/drain | "
            "Mapping fetch queued @ measure/drain | "
            "Face calls pending @ measure end | Face calls abandoned |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in cells:
        lines.append(
            f"| {row['cellId']} | {row['deliveredWarmup']} | "
            f"{row['deliveredMeasured']} | "
            f"{row['workerCommittedAtMeasureEnd']} | "
            f"{row['workerOutstandingAtMeasureEnd']} | "
            f"{row['workerOutstandingAtDrainEnd']} | "
            f"{row['publicationFetchQueuedAtMeasureEnd']}/"
            f"{row['publicationFetchQueuedAtDrainEnd']} | "
            f"{row['publicationFetchPendingAtMeasureEnd']}/"
            f"{row['publicationFetchPendingAtDrainEnd']} | "
            f"{row['publicationFetchDispatchedAtMeasureEnd']}/"
            f"{row['publicationFetchDispatchedAtDrainEnd']} | "
            f"{row['publicationFetchDataAtMeasureEnd']}/"
            f"{row['publicationFetchDataAtDrainEnd']};"
            f"{row['publicationFetchNacksAtMeasureEnd']}/"
            f"{row['publicationFetchNacksAtDrainEnd']};"
            f"{row['publicationFetchTimeoutsAtMeasureEnd']}/"
            f"{row['publicationFetchTimeoutsAtDrainEnd']} | "
            f"{row['mappingFetchQueuedAtMeasureEnd']}/"
            f"{row['mappingFetchQueuedAtDrainEnd']} | "
            f"{row['faceDispatchPendingAtMeasureEnd']} | "
            f"{row['faceDispatchAbandoned']} |"
        )
    if errors:
        lines.extend(["", "## Validation errors", ""])
        lines.extend(f"- {error}" for error in errors)
    if load_outcomes:
        lines.extend(["", "## Load outcomes", ""])
        lines.extend(f"- {outcome}" for outcome in load_outcomes)
    if pairs:
        lines.extend(["", "## Paired interpretation", ""])
        for pair in pairs:
            lines.append(
                f"- {pair['ratePerPeer']} pps/peer: `{pair['verdict']}`; "
                f"control_sustained={not pair['controlMissesOfferedRate']}; "
                f"worker_sustained={pair['workerSustainsOfferedRate']}; "
                f"heartbeat_p99_gain={pair['heartbeatP99Gain']:.4f}; "
                f"delivered_only_p99_gain={pair['deliveryP99Gain']:.4f}; "
                f"delivery_p99_comparable={pair['deliveryP99Comparable']}."
            )
            if not pair["deliveryP99Comparable"]:
                lines.append(
                    "  Delivery p99 is conditional on delivered publications and "
                    "is not an independent latency contrast because completion "
                    "ratios differ by more than one percentage point."
                )
    if campaign_kind == "smoke":
        lines.extend(
            [
                "",
                "This is a short harness smoke only. It is not performance evidence.",
            ]
        )
    elif campaign_kind == "confirmation-400":
        lines.extend(
            [
                "",
                "This is a non-formal, single-pair descriptive confirmation. It "
                "does not satisfy the Spec 136 formal-matrix or final-claim gate.",
            ]
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    args = parser.parse_args()
    campaign = args.campaign.resolve()
    manifest = load_json(campaign / "campaign-manifest.json")
    terminals = load_json(campaign / "campaign-terminals.json")
    formal = bool(manifest["formal"])
    campaign_kind = str(
        manifest.get("campaignKind", "formal" if formal else "smoke")
    )
    timing = manifest["timing"]
    errors = []
    load_outcomes = []

    for key in ("binary", "library"):
        path = Path(manifest[key])
        if not path.is_file():
            errors.append(f"{key} missing: {path}")
        elif sha256(path) != manifest[f"{key}Sha256"]:
            errors.append(f"{key} hash changed after manifest creation")
    if manifest.get("twoNodes") is not True:
        errors.append("manifest does not declare two nodes")
    if manifest.get("bothPeersPublishAndSubscribe") is not True:
        errors.append("manifest does not declare bidirectional PubSub")
    if len(terminals) != len(manifest["matrix"]):
        errors.append("terminal count does not match manifest matrix")

    cells = []
    for expected, terminal in zip(manifest["matrix"], terminals):
        cell_id = terminal["cellId"]
        if terminal["mode"] != expected["mode"] or int(
            terminal["ratePerPeer"]
        ) != int(expected["ratePerPeer"]):
            errors.append(f"{cell_id}:terminal does not match manifest")
        try:
            row, cell_errors, cell_load_outcomes = aggregate_cell(
                campaign / cell_id, terminal, int(timing["measure"])
            )
            cells.append(row)
            errors.extend(cell_errors)
            load_outcomes.extend(cell_load_outcomes)
        except (OSError, KeyError, ValueError, json.JSONDecodeError) as exc:
            errors.append(f"{cell_id}:analysis failed: {exc}")

    pairs = []
    verdict = "SMOKE_ONLY"
    if formal or campaign_kind == "confirmation-400":
        required_cells = 10 if formal else 2
        if len(cells) != required_cells:
            verdict = "INCONCLUSIVE"
            errors.append("campaign does not contain the required analyzable cells")
        if not errors:
            by_condition = {
                (row["mode"], row["ratePerPeer"]): row for row in cells
            }
            for rate in sorted({row["ratePerPeer"] for row in cells}):
                try:
                    pairs.append(
                        classify_rate(
                            by_condition[(MODES[0], rate)],
                            by_condition[(MODES[1], rate)],
                        )
                    )
                except KeyError:
                    errors.append(f"rate {rate}:missing paired mode")
            if campaign_kind == "confirmation-400":
                descriptive_extension = (
                    len(pairs) == 1
                    and pairs[0]["capacityExtension"]
                    and pairs[0]["verdict"] == "USEFUL_AT_RATE"
                )
                verdict = (
                    "DESCRIPTIVE_400_CAPACITY_EXTENSION"
                    if descriptive_extension
                    else "NO_DESCRIPTIVE_400_PPS_BENEFIT"
                )
                for pair in pairs:
                    pair["formalVerdictEligible"] = False
                    pair["verdict"] = (
                        "DESCRIPTIVE_CAPACITY_EXTENSION"
                        if descriptive_extension
                        else "NO_DESCRIPTIVE_BENEFIT"
                    )
            else:
                for pair in pairs:
                    pair["formalVerdictEligible"] = True
                verdict = (
                    overall_verdict(pairs)
                    if len(pairs) == 5 and not errors
                    else "INCONCLUSIVE"
                )

    result = {
        "schema": "spec136.analysis.v2",
        "formal": formal,
        "evidenceClass": "FORMAL" if formal else "NON_FORMAL_DESCRIPTIVE",
        "verdict": verdict,
        "errors": errors,
        "loadOutcomes": sorted(set(load_outcomes)),
        "cells": cells,
        "pairedContrasts": pairs,
    }
    (campaign / "analysis.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if cells:
        write_csv(campaign / "comparison.csv", cells)
    write_markdown(
        campaign / "comparison.md",
        campaign_kind,
        formal,
        verdict,
        cells,
        errors,
        sorted(set(load_outcomes)),
        pairs,
    )
    print(campaign / "comparison.md")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
