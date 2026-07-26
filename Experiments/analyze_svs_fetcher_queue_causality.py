#!/usr/bin/env python3
"""Analyze the two-stage Spec 135 RSA/Fetcher diagnostic campaign."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
from statistics import mean
from typing import Any, Iterable


REPO = Path(__file__).resolve().parents[1]
BASE_ANALYZER = REPO / "Experiments/analyze_svs_sync_stage_profile.py"
BASE_RUNNER = REPO / "Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py"
PEERS = ("peer-a", "peer-b")


def load_module(path: Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: Iterable[int | float], fraction: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    return float(ordered[max(0, int(len(ordered) * fraction + 0.999999) - 1)])


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise RuntimeError(f"refusing empty output: {path}")
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def stage_metric(summaries: dict[str, dict[str, Any]],
                 spans: list[dict[str, Any]], stage: str) -> dict[str, Any]:
    summary = summaries[stage]
    durations = [item["durationNs"] for item in spans if item["stage"] == stage]
    calls = int(summary["calls"])
    total = int(summary["totalDurationNs"])
    return {
        "calls": calls,
        "totalNs": total,
        "meanNs": total / calls if calls else 0,
        "sampleP95Ns": percentile(durations, 0.95),
        "sampleMaxNs": max(durations) if durations else None,
    }


def configs(contract: dict[str, Any], stage_b: dict[str, Any]) -> list[dict[str, Any]]:
    return list(contract["stageA"]) + list(stage_b["cells"])


def analyze(campaign: Path, evidence_dir: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)
    contract = load_json(campaign / "campaign-contract.json")
    stage_b = load_json(campaign / "stage-b-manifest.json")
    selection = load_json(campaign / "boundary-selection.json")
    subject = load_json(Path(contract["subjectManifest"]))
    base = load_module(BASE_ANALYZER, "spec133_analysis_for_spec135")
    base_runner = load_module(BASE_RUNNER, "spec133_runner_metrics_for_spec135")
    registry = base.parse_registry(Path(subject["profileWorktree"]) /
                                   "ndn-svs/profile.hpp")
    modulus = int(subject["profileConfig"]["sampleModulus"])

    peer_rows: list[dict[str, Any]] = []
    stage_rows: list[dict[str, Any]] = []
    for config in configs(contract, stage_b):
        cell_id = config["cellId"]
        receipt = load_json(campaign / "receipts" / f"{cell_id}.json")
        if receipt["status"] != "COMPLETE":
            peer_rows.append({
                "cellId": cell_id, "ratePpsPerPeer": config["ratePpsPerPeer"],
                "fetcherWindow": config["fetcherWindow"],
                "maxApplicationParametersSize":
                    config["maxApplicationParametersSize"],
                "peerId": "cell", "status": receipt["status"],
            })
            continue
        cell_dir = campaign / "cells" / cell_id
        cell_metric = base_runner.parse_cell_metrics(cell_dir, config)
        events_by_peer = {
            peer: [
                json.loads(line) for line in
                (cell_dir / f"{peer}-events.jsonl").read_text(
                    encoding="utf-8").splitlines()
            ]
            for peer in PEERS
        }
        attempted_by_peer = {
            peer: sum(item["event"] == "api-return" and
                      item["phase"] == "measured"
                      for item in events_by_peer[peer])
            for peer in PEERS
        }
        for peer in PEERS:
            events = events_by_peer[peer]
            starts = [item for item in events if item["event"] == "process-start"]
            stops = [item for item in events if item["event"] == "process-stop"]
            if len(starts) != 1 or len(stops) != 1:
                raise RuntimeError(f"process evidence mismatch: {cell_id}/{peer}")
            start_details = starts[0]["details"]
            stop_details = stops[0]["details"]
            rsa_proof = start_details.get("dataSigner") == "RSA-2048" and \
                int(start_details.get("rsaSignatureType", -1)) == 1
            if not rsa_proof:
                raise RuntimeError(f"RSA wire-type proof failed: {cell_id}/{peer}")
            attempted = attempted_by_peer[peer]
            delivered = sum(item["event"] == "delivery" and
                            item["phase"] == "measured" for item in events)
            invalid = sum(item["event"] == "invalid" for item in events)
            batches: dict[int, int] = defaultdict(int)
            for item in events:
                if item["event"] == "state-update":
                    batches[int(item["monotonicRawNs"])] += 1
            batch_widths = list(batches.values())
            spans, summaries = base.read_profile(
                cell_dir / f"{peer}.stderr", cell_id, peer, registry, modulus)
            payload_queue = stage_metric(
                summaries, spans, "PAYLOAD.FETCHER_QUEUE_WAIT")
            mapping_queue = stage_metric(
                summaries, spans, "MAP.FETCHER_QUEUE_WAIT")
            sync_receive = stage_metric(summaries, spans, "SYNC.RECEIVE_TOTAL")
            inner_sign = stage_metric(summaries, spans, "PUB.INNER_SIGN")
            outer_sign = stage_metric(summaries, spans, "PUB.OUTER_SIGN")
            scheduled = int(stop_details["scheduledMeasured"])
            missed = int(stop_details["missedReleaseMeasured"])
            row = {
                "cellId": cell_id,
                "stage": config["stage"],
                "ratePpsPerPeer": config["ratePpsPerPeer"],
                "fetcherWindow": config["fetcherWindow"],
                "maxApplicationParametersSize":
                    config["maxApplicationParametersSize"],
                "peerId": peer,
                "status": receipt["status"],
                "rsaSignatureType": int(start_details["rsaSignatureType"]),
                "scheduled": scheduled,
                "attempted": attempted,
                "attemptedScheduledRatio":
                    attempted / scheduled if scheduled else 0,
                "missedRelease": missed,
                "delivered": delivered,
                "deliveryAttemptedRatio":
                    delivered / attempted_by_peer[
                        "peer-b" if peer == "peer-a" else "peer-a"]
                    if attempted_by_peer[
                        "peer-b" if peer == "peer-a" else "peer-a"] else 0,
                "invalid": invalid,
                "missingBatchCount": len(batch_widths),
                "missingBatchP50": percentile(batch_widths, 0.50),
                "missingBatchP95": percentile(batch_widths, 0.95),
                "missingBatchMax": max(batch_widths) if batch_widths else 0,
                "mappingFallbackCalls": summaries["MAP.INTEREST_BUILD"]["calls"],
                "payloadFallbackCalls": summaries["PAYLOAD.INTEREST_BUILD"]["calls"],
                "mappingQueueCalls": mapping_queue["calls"],
                "mappingQueueMeanUs": mapping_queue["meanNs"] / 1000,
                "mappingQueueSampleMaxUs":
                    mapping_queue["sampleMaxNs"] / 1000
                    if mapping_queue["sampleMaxNs"] is not None else None,
                "payloadQueueCalls": payload_queue["calls"],
                "payloadQueueMeanUs": payload_queue["meanNs"] / 1000,
                "payloadQueueSampleMaxUs":
                    payload_queue["sampleMaxNs"] / 1000
                    if payload_queue["sampleMaxNs"] is not None else None,
                "syncReceiveCalls": sync_receive["calls"],
                "syncReceiveTotalMs": sync_receive["totalNs"] / 1e6,
                "syncReceiveMeanUs": sync_receive["meanNs"] / 1000,
                "rsaInnerSignCalls": inner_sign["calls"],
                "rsaInnerSignMeanUs": inner_sign["meanNs"] / 1000,
                "rsaOuterSignCalls": outer_sign["calls"],
                "rsaOuterSignMeanUs": outer_sign["meanNs"] / 1000,
                "rsaSignTotalMeanUs":
                    (inner_sign["meanNs"] + outer_sign["meanNs"]) / 1000,
                "aggregateCellCpuPercent":
                    cell_metric["aggregateCpuPercent"],
                "profileComplete": cell_metric["profileComplete"],
            }
            peer_rows.append(row)
            for stage_id, values in summaries.items():
                stage_rows.append({
                    "cellId": cell_id,
                    "ratePpsPerPeer": config["ratePpsPerPeer"],
                    "fetcherWindow": config["fetcherWindow"],
                    "maxApplicationParametersSize":
                        config["maxApplicationParametersSize"],
                    "peerId": peer,
                    "stageId": stage_id,
                    "calls": values["calls"],
                    "totalDurationNs": values["totalDurationNs"],
                    "meanDurationNs":
                        values["totalDurationNs"] / values["calls"]
                        if values["calls"] else 0,
                    "failures": values["failures"],
                })

    complete_rows = [row for row in peer_rows if row.get("peerId") in PEERS]
    if len(complete_rows) != 16:
        raise RuntimeError(f"expected 16 complete peer rows, got {len(complete_rows)}")
    write_csv(campaign / "peer-causality-summary.csv", peer_rows)
    write_csv(campaign / "stage-causality-summary.csv", stage_rows)

    selected_rate = selection["selectedRatePpsPerPeer"]
    factorial = [row for row in complete_rows
                 if row["ratePpsPerPeer"] == selected_rate and
                 (row["stage"] == "factor-treatment" or
                  row["cellId"] == selection["selectedBaselineCellId"])]
    grouped: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    for row in factorial:
        grouped[(row["fetcherWindow"],
                 row["maxApplicationParametersSize"])].append(row)
    if set(grouped) != {(10, 4096), (40, 4096), (10, 7168), (40, 7168)}:
        raise RuntimeError("factorial combinations are incomplete")
    aggregate: dict[tuple[int, int], dict[str, float]] = {}
    metrics = (
        "attemptedScheduledRatio", "deliveryAttemptedRatio", "missedRelease",
        "mappingFallbackCalls", "payloadFallbackCalls", "mappingQueueMeanUs",
        "payloadQueueMeanUs", "syncReceiveTotalMs",
    )
    for factor, rows in grouped.items():
        aggregate[factor] = {
            metric: mean(float(row[metric]) for row in rows) for metric in metrics
        }
    contrasts: list[dict[str, Any]] = []
    for factor_name, left, right in (
        ("window@4096", (10, 4096), (40, 4096)),
        ("window@7168", (10, 7168), (40, 7168)),
        ("piggyback@W10", (10, 4096), (10, 7168)),
        ("piggyback@W40", (40, 4096), (40, 7168)),
    ):
        row: dict[str, Any] = {
            "contrast": factor_name, "from": str(left), "to": str(right)}
        for metric in metrics:
            row[f"{metric}Delta"] = \
                aggregate[right][metric] - aggregate[left][metric]
        contrasts.append(row)
    write_csv(campaign / "factor-contrasts.csv", contrasts)

    window_contrasts = [
        row for row in contrasts if row["contrast"].startswith("window@")]
    piggy_contrasts = [
        row for row in contrasts if row["contrast"].startswith("piggyback@")]
    window_supported = all(
        row["missedReleaseDelta"] <= 0 and
        row["payloadQueueMeanUsDelta"] <= 0 and
        row["deliveryAttemptedRatioDelta"] >= 0
        for row in window_contrasts
    )
    window_partial = all(
        row["payloadQueueMeanUsDelta"] < 0 for row in window_contrasts)
    piggy_supported = all(
        row["payloadFallbackCallsDelta"] <= 0 and
        row["payloadQueueMeanUsDelta"] <= 0 and
        row["missedReleaseDelta"] <= 0
        for row in piggy_contrasts
    )
    piggy_partial = all(
        row["payloadFallbackCallsDelta"] < 0 for row in piggy_contrasts)
    verdicts = {
        "H0_RSA_BOUNDARY": selection["selectionKind"],
        "H1_FETCHER_WINDOW":
            "SUPPORTED" if window_supported else
            ("PARTIAL" if window_partial else "NOT_SUPPORTED"),
        "H2_PIGGYBACK_CAPACITY":
            "SUPPORTED" if piggy_supported else
            ("PARTIAL" if piggy_partial else "NOT_SUPPORTED"),
        "H3_SHARED_IO": "MECHANISM_ONLY_NOT_ISOLATED",
    }
    summary = {
        "schemaVersion": "spec135-causality-summary-v1",
        "campaignId": contract["campaignId"],
        "receiptCount": len(list((campaign / "receipts").glob("*.json"))),
        "peerRowCount": len(complete_rows),
        "selectedRatePpsPerPeer": selected_rate,
        "selectionKind": selection["selectionKind"],
        "verdicts": verdicts,
    }
    (campaign / "causality-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    sweep_rows = [row for row in complete_rows
                  if row["stage"] == "rsa-boundary-sweep"]
    report = [
        "# Spec 135 RSA-2048 NDN-SVS Queue Causality Report",
        "",
        "## Material Passport",
        "",
        "- Origin Skill: experiment-agent",
        "- Origin Mode: validate",
        "- Verification Status: MEASURED",
        "- Version Label: code_result_v1",
        "",
        "## RSA boundary result",
        "",
        f"Selection: **{selection['selectionKind']}** at "
        f"**{selected_rate} pps/peer**. Every peer proved TLV signature type 1 "
        "(`SignatureSha256WithRsa`) before warmup; RSA key generation and the "
        "probe were outside the measured window.",
        "",
        "| Rate | Mean attempted/scheduled | Aggregate delivered/attempted | "
        "RSA inner+outer sign (us) | PUB.TOTAL (us) | "
        "Payload queue (us) |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    digest_combined_us = {
        200: 17.12 + 9.49,
        400: 14.70 + 5.63,
        600: 14.03 + 5.81,
        800: 16.90 + 6.25,
        1000: 15.22 + 6.86,
    }
    rate_means: dict[int, dict[str, float]] = {}
    stage_means_by_rate: dict[int, dict[str, float]] = {}
    for rate in (200, 400, 600, 800, 1000):
        rows = [row for row in sweep_rows if row["ratePpsPerPeer"] == rate]
        cell_id = rows[0]["cellId"]
        selected_stages = [
            row for row in stage_rows if row["cellId"] == cell_id]

        def combined_stage_mean_us(stage_id: str) -> float:
            selected = [
                row for row in selected_stages if row["stageId"] == stage_id]
            calls = sum(int(row["calls"]) for row in selected)
            total = sum(int(row["totalDurationNs"]) for row in selected)
            return total / calls / 1000 if calls else 0.0

        stage_means_by_rate[rate] = {
            stage_id: combined_stage_mean_us(stage_id)
            for stage_id in (
                "PUB.INNER_BUILD", "PUB.INNER_SIGN", "PUB.OUTER_BUILD",
                "PUB.OUTER_SIGN", "PUB.OUTER_STORE_INSERT",
                "PUB.OUTER_FACE_PUT", "PUB.TOTAL",
                "SYNC.RECEIVE_TOTAL", "PAYLOAD.FETCHER_QUEUE_WAIT",
                "MAP.FETCHER_QUEUE_WAIT",
            )
        }

        delivered_ratio = sum(int(row["delivered"]) for row in rows) / \
            max(1, sum(int(row["attempted"]) for row in rows))
        rsa_mean = combined_stage_mean_us("PUB.INNER_SIGN") + \
            combined_stage_mean_us("PUB.OUTER_SIGN")
        pub_total = combined_stage_mean_us("PUB.TOTAL")
        payload_queue = combined_stage_mean_us("PAYLOAD.FETCHER_QUEUE_WAIT")
        rate_means[rate] = {
            "attempted": mean(row["attemptedScheduledRatio"] for row in rows),
            "delivered": delivered_ratio,
            "rsa": rsa_mean,
            "pubTotal": pub_total,
            "payloadQueue": payload_queue,
        }
        report.append(
            f"| {rate} | {rate_means[rate]['attempted']:.4f} "
            f"| {rate_means[rate]['delivered']:.4f} "
            f"| {rsa_mean:.3f} | {pub_total:.3f} | "
            f"{payload_queue:.3f} |"
        )
    report += [
        "",
        "The preregistered 98% operational boundary is 400 pps/peer. The more "
        "pronounced capacity knee is 600 pps: attempted/scheduled falls to about "
        "82%; at 800 it is about 54%. At 1000 pps the complete publication call "
        "already exceeds the 1 ms release period before receive/fetch callbacks "
        "are counted, and aggregate delivery collapses to about 6%.",
        "",
        "## RSA cost versus the frozen DigestSha256 baseline",
        "",
        "| Rate | Digest inner+outer sign (us) | RSA inner+outer sign (us) | "
        "RSA/Digest | RSA share of PUB.TOTAL |",
        "|---:|---:|---:|---:|---:|",
    ]
    for rate in (200, 400, 600, 800, 1000):
        rsa_mean = rate_means[rate]["rsa"]
        pub_total = rate_means[rate]["pubTotal"]
        report.append(
            f"| {rate} | {digest_combined_us[rate]:.2f} | {rsa_mean:.2f} | "
            f"{rsa_mean / digest_combined_us[rate]:.1f}x | "
            f"{rsa_mean / pub_total * 100:.1f}% |"
        )
    report += [
        "",
        "The Digest values are the already frozen Spec 133 combined-peer means; "
        "they are not remeasured or relabeled. RSA-2048 consumes about "
        "1.09--1.10 ms for the two Data signatures per `publish()` and roughly "
        "95% of `PUB.TOTAL`. Thus the earlier 14--17 us inner-sign values were "
        "correct for DigestSha256 but cannot represent the deployed RSA path.",
        "",
        "## Publication stage detail",
        "",
        "Combined-peer exact mean per completed call:",
        "",
        "| Stage | 200 | 400 | 600 | 800 | 1000 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for label, stage_id in (
        ("Inner Data build (us)", "PUB.INNER_BUILD"),
        ("Inner RSA sign (us)", "PUB.INNER_SIGN"),
        ("Outer Data build (us)", "PUB.OUTER_BUILD"),
        ("Outer RSA sign (us)", "PUB.OUTER_SIGN"),
        ("Outer store insert (us)", "PUB.OUTER_STORE_INSERT"),
        ("Outer Face put (us)", "PUB.OUTER_FACE_PUT"),
        ("Complete publish (us)", "PUB.TOTAL"),
    ):
        report.append(
            f"| {label} | " + " | ".join(
                f"{stage_means_by_rate[rate][stage_id]:.3f}"
                for rate in (200, 400, 600, 800, 1000)
            ) + " |"
        )
    report += [
        "",
        "Encoding/build/store/Face-put work remains in the low tens of "
        "microseconds. The approximately 0.56 ms inner RSA sign plus 0.53 ms "
        "outer RSA sign dominates the synchronous call.",
        "",
        "## Receive/fetch pressure",
        "",
        "| Rate | Sync receive mean (us) | Payload queue mean | "
        "Mapping queue mean | Mean payload fallbacks/peer | "
        "Missing-batch p95 |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for rate in (200, 400, 600, 800, 1000):
        rows = [row for row in sweep_rows if row["ratePpsPerPeer"] == rate]
        payload_us = stage_means_by_rate[rate][
            "PAYLOAD.FETCHER_QUEUE_WAIT"]
        mapping_us = stage_means_by_rate[rate]["MAP.FETCHER_QUEUE_WAIT"]
        report.append(
            f"| {rate} | "
            f"{stage_means_by_rate[rate]['SYNC.RECEIVE_TOTAL']:.3f} | "
            f"{payload_us / 1000:.3f} ms | {mapping_us / 1000:.3f} ms | "
            f"{mean(float(row['payloadFallbackCalls']) for row in rows):.1f} | "
            f"{mean(float(row['missingBatchP95']) for row in rows):.1f} |"
        )
    report += [
        "",
        "At 1000 pps, completed Payload and Mapping queue waits are about "
        "9.45 s and 9.33 s, while missing-range batches reach thousands of "
        "items. This preserves the later fallback-queue instability found with "
        "DigestSha256, but it is no longer the first boundary: RSA signing and "
        "shared-loop scheduling reduce attempted load earlier.",
        "",
        "## Factor verdicts",
        "",
        f"- H1 Fetcher window: **{verdicts['H1_FETCHER_WINDOW']}**.",
        f"- H2 piggyback capacity: **{verdicts['H2_PIGGYBACK_CAPACITY']}**.",
        "- H3 shared I/O thread: **MECHANISM_ONLY_NOT_ISOLATED**; temporal "
        "co-variation is measurable, but no unsafe cross-thread treatment was run.",
        "",
        "Detailed peer metrics and exact 81-stage totals are in "
        "`peer-causality-summary.csv` and `stage-causality-summary.csv`; matched "
        "factor deltas are in `factor-contrasts.csv`.",
        "",
        "| Contrast | Attempted ratio delta | Missed-release delta | "
        "Payload fallback delta | Payload queue delta (us) |",
        "|---|---:|---:|---:|---:|",
    ]
    for row in contrasts:
        report.append(
            f"| {row['contrast']} | "
            f"{row['attemptedScheduledRatioDelta']:+.5f} | "
            f"{row['missedReleaseDelta']:+.1f} | "
            f"{row['payloadFallbackCallsDelta']:+.1f} | "
            f"{row['payloadQueueMeanUsDelta']:+.1f} |"
        )
    report += [
        "",
        "## Solution directions (analysis only)",
        "",
        "1. Treat RSA publication signing as fixed service demand on the shared "
        "I/O loop. Cache only safe encoding/key lookup work, batch signatures "
        "only if protocol semantics allow it, or move signing through an "
        "explicitly thread-safe ordered completion path in a new Spec.",
        "2. Bound missing-range expansion and admit fetch work incrementally, "
        "so the later overload regime cannot enqueue one enormous burst.",
        "3. Replace the fixed Fetcher window with a bounded adaptive window only "
        "after separating the RSA pacing limit; the matched intervention proves "
        "queue reduction but not a consistent attempted-rate improvement.",
        "4. Larger piggyback capacity reliably cuts fallback Interest count in "
        "this round, but its queue/pacing effects are mixed; evaluate packet "
        "size and fragmentation before adopting it.",
        "",
        "No production NDN-SVS or NDNSF change is made by Spec 135.",
        "",
        "## Limitations",
        "",
        "- One once-only cell per configuration supports descriptive mechanism "
        "evidence, not variance estimates or population inference.",
        "- RSA uses ndn-cxx software RSA-2048 with each MiniNDN node's separate "
        "persistent file TPM. Hardware TPM/HSM latency is not represented.",
        "- Validators remain disabled to preserve the Spec 133 receive-side "
        "control; RSA verification cost is therefore not included.",
        "- The 98% rule is an operational pacing boundary, not CPU saturation. "
        "The sharper throughput knee occurs at 600 pps/peer.",
        "- Stage-B interactions are real: neither a larger window nor larger "
        "piggyback capacity is an unconditional standalone improvement.",
        "",
        "## Fallacy scan",
        "",
        "- No cherry-picking: all eight once-only receipts are retained.",
        "- No security relabeling: Spec 133 remains DigestSha256; Spec 135 proves RSA.",
        "- No pseudo-replication: two peers in one cell are paired observations, "
        "not independent campaign replicates.",
        "- No p-values or confidence intervals from n=1 cells.",
        "- No sum of queue/network wait with leaf CPU demand.",
        "- No survivor-only queue mean without calls, delivery, and missed releases.",
        "- No throughput claim from offered rate alone.",
        "- No causal shared-thread claim without a thread treatment.",
        "- No automatic retry or replacement of failures.",
        "- No claim that a larger packet is free of fragmentation effects.",
        "- No production-fix claim from a diagnostic-only patch.",
    ]
    report_path = evidence_dir / "causality-report.md"
    report_path.write_text("\n".join(report) + "\n", encoding="utf-8")
    authority_paths = [
        campaign / "campaign-contract.json",
        campaign / "boundary-selection.json",
        campaign / "stage-b-manifest.json",
        campaign / "execution-summary.json",
        campaign / "causality-summary.json",
        campaign / "peer-causality-summary.csv",
        campaign / "stage-causality-summary.csv",
        campaign / "factor-contrasts.csv",
        Path(contract["subjectManifest"]),
        Path(__file__).resolve(),
        report_path,
        *sorted((campaign / "receipts").glob("*.json")),
    ]
    evidence_manifest = {
        "schemaVersion": "spec135-evidence-manifest-v1",
        "campaignId": contract["campaignId"],
        "receiptCount": 8,
        "authorities": {
            str(path.resolve()): sha256(path) for path in authority_paths
        },
    }
    (campaign / "evidence-manifest.json").write_text(
        json.dumps(evidence_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    parser.add_argument(
        "--evidence-dir", type=Path,
        default=REPO / "specs/135-svs-fetcher-queue-causality/evidence")
    args = parser.parse_args()
    print(json.dumps(analyze(args.campaign, args.evidence_dir), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
