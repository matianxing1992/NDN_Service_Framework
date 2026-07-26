#!/usr/bin/env python3
"""Validity-first analyzer for a frozen Spec 142 MiniNDN campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))

from Experiments import NDN_SVS_NDNSF_Profile_Worker_Minindn as runner


ANALYSIS_SCHEMA = "spec142.analysis.v1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: list[int], pct: int) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    rank = (len(ordered) * pct + 99) // 100
    return ordered[max(1, rank) - 1]


def read_samples(path: Path) -> list[int]:
    with path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows or set(rows[0]) != {"latencyNs"}:
        raise RuntimeError(f"{path}: expected one latencyNs column")
    try:
        values = [int(row["latencyNs"]) for row in rows]
    except (TypeError, ValueError) as error:
        raise RuntimeError(f"{path}: invalid latency sample") from error
    if any(value < 0 for value in values):
        raise RuntimeError(f"{path}: negative latency sample")
    return values


def recovery_delta(summary: dict[str, Any], prefix: str, metric: str) -> int:
    start = int(summary[f"{prefix}{metric}AtMeasureStart"])
    end = int(summary[f"{prefix}{metric}AtMeasureEnd"])
    if end < start:
        raise RuntimeError(f"{prefix}{metric}: counter decreased")
    return end - start


def analyze_peer(cell: Path, peer: str, terminal: dict[str, Any]) -> dict[str, Any]:
    summary_path = cell / f"{peer}-summary.json"
    sample_path = cell / f"{peer}-delivery-latency.csv"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    samples = read_samples(sample_path)
    if len(samples) != int(summary["deliverySamples"]):
        raise RuntimeError(
            f"{cell.name}:{peer}: sample count {len(samples)} != "
            f"summary {summary['deliverySamples']}"
        )
    calculated = {
        "meanNs": sum(samples) // len(samples) if samples else 0,
        "p50Ns": percentile(samples, 50),
        "p95Ns": percentile(samples, 95),
        "p99Ns": percentile(samples, 99),
    }
    expected = {
        "meanNs": int(summary["deliveryMeanNs"]),
        "p50Ns": int(summary["deliveryP50Ns"]),
        "p95Ns": int(summary["deliveryP95Ns"]),
        "p99Ns": int(summary["deliveryP99Ns"]),
    }
    if calculated != expected:
        raise RuntimeError(
            f"{cell.name}:{peer}: raw latency statistics mismatch: "
            f"{calculated!r} != {expected!r}"
        )

    attempted = int(summary["attemptedMeasured"])
    delivered = int(summary["deliveredMeasured"])
    return {
        "peer": peer,
        "summaryPath": str(summary_path),
        "summarySha256": sha256(summary_path),
        "samplePath": str(sample_path),
        "sampleSha256": sha256(sample_path),
        "sampleCount": len(samples),
        "attempted": attempted,
        "attemptedPps": attempted / int(summary["measureSeconds"]),
        "delivered": delivered,
        "deliveredPps": delivered / int(summary["measureSeconds"]),
        "deliveryRatio": delivered / attempted if attempted else 0.0,
        "latency": calculated,
        "survivorDistribution": delivered < attempted,
        "signedPublicationWireBytesMean": (
            int(summary["signedPublicationWireBytesTotal"])
            / int(summary["signedPublicationWireBytesCount"])
        ),
        "signedPublicationWireBytesMax": int(summary["signedPublicationWireBytesMax"]),
        "piggyback": {
            "eligible": int(summary["piggybackEligibleCount"]),
            "ineligible": int(summary["piggybackIneligibleCount"]),
            "sent": int(summary["piggybackSentCount"]),
            "received": int(summary["piggybackReceivedCount"]),
            "delivered": int(summary["piggybackDeliveredCount"]),
            "fallback": int(summary["publicationFetchFallbackCount"]),
        },
        "publicationFetch": {
            "dispatchedAtDrainEnd": int(summary["publicationFetchDispatchedAtDrainEnd"]),
            "dataAtDrainEnd": int(summary["publicationFetchDataAtDrainEnd"]),
            "retriesDuringMeasure": recovery_delta(summary, "publicationFetch", "Retries"),
            "timeoutsDuringMeasure": recovery_delta(summary, "publicationFetch", "Timeouts"),
            "nacksDuringMeasure": recovery_delta(summary, "publicationFetch", "Nacks"),
        },
        "mappingFetch": {
            "dispatchedAtDrainEnd": int(summary["mappingFetchDispatchedAtDrainEnd"]),
            "dataAtDrainEnd": int(summary["mappingFetchDataAtDrainEnd"]),
            "retriesDuringMeasure": recovery_delta(summary, "mappingFetch", "Retries"),
            "timeoutsDuringMeasure": recovery_delta(summary, "mappingFetch", "Timeouts"),
            "nacksDuringMeasure": recovery_delta(summary, "mappingFetch", "Nacks"),
        },
        "rsa": {
            "dataSignCalls": int(summary["dataSignCalls"]),
            "dataValid": int(summary["dataValid"]),
            "dataInvalid": int(summary["dataInvalid"]),
            "syncEnvelopeSignatureType": int(summary["syncEnvelopeSignatureType"]),
        },
        "resources": {
            "maxRssKiB": int(summary["maxRssKiB"]),
            "workerMaxPending": int(summary["workerMaxPending"]),
            "faceDispatchPendingAtMeasureEnd": int(
                summary["faceDispatchPendingAtMeasureEnd"]
            ),
        },
        "validity": terminal["validity"],
        "outcome": terminal["outcome"],
    }


def analyze_campaign(campaign: Path) -> dict[str, Any]:
    manifest_path = campaign / "runtime-profile-manifest.json"
    verdict_path = campaign / "qualification-verdict.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "spec142.runtime-profile-manifest.v1":
        raise RuntimeError("unexpected runtime profile manifest schema")
    if verdict.get("schema") != runner.QUALIFICATION_VERDICT_SCHEMA:
        raise RuntimeError("unexpected qualification verdict schema")
    if verdict.get("runtimeProfileManifestSha256") != sha256(manifest_path):
        raise RuntimeError("qualification verdict does not bind the runtime manifest")
    terminals_path = Path(verdict["terminalPath"])
    if verdict.get("terminalSha256") != sha256(terminals_path):
        raise RuntimeError("qualification terminal hash mismatch")

    terminals = json.loads(terminals_path.read_text(encoding="utf-8"))
    cells = []
    for terminal in terminals:
        cell = campaign / terminal["cellId"]
        terminal_path = cell / "terminal.json"
        frozen_terminal = json.loads(terminal_path.read_text(encoding="utf-8"))
        if frozen_terminal != terminal:
            raise RuntimeError(f"{cell.name}: terminal aggregate mismatch")
        peers = [analyze_peer(cell, peer, terminal) for peer in ("peer-a", "peer-b")]
        cells.append(
            {
                "cellId": terminal["cellId"],
                "mode": terminal["mode"],
                "ratePerPeer": int(terminal["ratePerPeer"]),
                "validity": terminal["validity"],
                "outcome": terminal["outcome"],
                "profileErrors": terminal["profileErrors"],
                "loadErrors": terminal["loadErrors"],
                "terminalPath": str(terminal_path),
                "terminalSha256": sha256(terminal_path),
                "peers": peers,
            }
        )

    valid_pairs = []
    by_rate: dict[int, list[dict[str, Any]]] = {}
    for cell in cells:
        by_rate.setdefault(cell["ratePerPeer"], []).append(cell)
    for rate, candidates in sorted(by_rate.items()):
        if (
            len(candidates) == 2
            and {cell["mode"] for cell in candidates}
            == {"face-inline-rsa", "worker-rsa"}
            and all(cell["validity"] == "PROFILE_VALID" for cell in candidates)
        ):
            valid_pairs.append(rate)

    return {
        "schema": ANALYSIS_SCHEMA,
        "campaign": str(campaign),
        "manifestPath": str(manifest_path),
        "manifestSha256": sha256(manifest_path),
        "qualificationStatus": verdict["status"],
        "validPairs": valid_pairs,
        "formalComparisonAuthorized": bool(valid_pairs),
        "cells": cells,
    }


def fmt_ms(value_ns: int) -> str:
    return f"{value_ns / 1_000_000:.3f}"


def render_report(analysis: dict[str, Any]) -> str:
    lines = [
        "# Spec 142 Final Report",
        "",
        "## Validity first",
        "",
        "| Cell | Validity | Outcome | Formal comparison eligible |",
        "|---|---|---|---:|",
    ]
    for cell in analysis["cells"]:
        lines.append(
            f"| {cell['cellId']} | {cell['validity']} | {cell['outcome']} | "
            f"{'yes' if cell['validity'] == 'PROFILE_VALID' else 'no'} |"
        )
    lines.extend(
        [
            "",
            f"Qualification verdict: **{analysis['qualificationStatus']}**. "
            f"Paired valid rates: {analysis['validPairs'] or 'none'}.",
            "",
            "The 600/800 pps stage was not authorized and was not started.",
            "",
            "## Diagnostic metrics from invalid 400 pps receipts",
            "",
            "These rows are boundary diagnostics only. They cannot support a causal "
            "worker-performance claim because recovery activated during measurement.",
            "",
            "| Mode | Peer | Delivered | Ratio | Mean ms | p50 ms | p95 ms | p99 ms | Pub retry/timeout/Nack | Map retry/timeout/Nack |",
            "|---|---|---:|---:|---:|---:|---:|---:|---|---|",
        ]
    )
    for cell in analysis["cells"]:
        for peer in cell["peers"]:
            pub = peer["publicationFetch"]
            mapping = peer["mappingFetch"]
            latency = peer["latency"]
            lines.append(
                f"| {cell['mode']} | {peer['peer']} | {peer['delivered']}/{peer['attempted']} | "
                f"{peer['deliveryRatio'] * 100:.3f}% | {fmt_ms(latency['meanNs'])} | "
                f"{fmt_ms(latency['p50Ns'])} | {fmt_ms(latency['p95Ns'])} | "
                f"{fmt_ms(latency['p99Ns'])} | "
                f"{pub['retriesDuringMeasure']}/{pub['timeoutsDuringMeasure']}/{pub['nacksDuringMeasure']} | "
                f"{mapping['retriesDuringMeasure']}/{mapping['timeoutsDuringMeasure']}/{mapping['nacksDuringMeasure']} |"
            )
    lines.extend(
        [
            "",
            "## Piggyback, Fetch, RSA, and resource evidence",
            "",
            "| Mode | Peer | Piggy eligible/sent/received/delivered | Fallback | Signed Data bytes mean/max | RSA sign/valid/invalid | Max RSS KiB | Samples SHA-256 |",
            "|---|---|---|---:|---|---|---:|---|",
        ]
    )
    for cell in analysis["cells"]:
        for peer in cell["peers"]:
            piggy = peer["piggyback"]
            rsa = peer["rsa"]
            lines.append(
                f"| {cell['mode']} | {peer['peer']} | "
                f"{piggy['eligible']}/{piggy['sent']}/{piggy['received']}/{piggy['delivered']} | "
                f"{piggy['fallback']} | "
                f"{peer['signedPublicationWireBytesMean']:.1f}/{peer['signedPublicationWireBytesMax']} | "
                f"{rsa['dataSignCalls']}/{rsa['dataValid']}/{rsa['dataInvalid']} | "
                f"{peer['resources']['maxRssKiB']} | `{peer['sampleSha256']}` |"
            )
    lines.extend(
        [
            "",
            "CPU utilization was not recorded by the frozen r4 peer schema; it is "
            "reported as unavailable rather than reconstructed after the fact.",
            "",
            "## Root-cause evidence preserved before r4",
            "",
            "The first corrected V3 campaign exposed a production-path defect: with "
            "parallel Sync production enabled, extra blocks built by a worker were "
            "discarded when V3 envelope signing returned to the Face thread. A focused "
            "unit test now covers that path, and the full NDN-SVS suite passes 75/75. "
            "After the fix, piggyback receive recovered from zero, but nonzero fallback "
            "Fetch timeout/retry events remained during the formal 60-second window.",
            "",
            "## Claim boundary",
            "",
            "This is a two-node, bidirectional NDN-SVS microbenchmark using the "
            "current NDNSF-effective V3 profile on a configured zero-loss MiniNDN "
            "link. It is not a full NDNSF workflow. The result proves that the prior "
            "forced-V2 benchmark was not representative and identifies a remaining "
            "recovery boundary; it does not prove a clean causal worker advantage.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--report-output", type=Path)
    args = parser.parse_args()
    campaign = args.campaign.resolve()
    analysis = analyze_campaign(campaign)
    json_output = args.json_output or campaign / "analysis.json"
    report_output = args.report_output or campaign / "analysis.md"
    json_output.parent.mkdir(parents=True, exist_ok=True)
    report_output.parent.mkdir(parents=True, exist_ok=True)
    json_output.write_text(
        json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    report_output.write_text(render_report(analysis), encoding="utf-8")
    print(report_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
