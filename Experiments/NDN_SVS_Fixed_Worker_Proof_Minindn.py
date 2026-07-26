#!/usr/bin/env python3
"""Spec 139 fixed 600-pps same-binary MiniNDN authority."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[1]
EXPERIMENTS = Path(__file__).resolve().parent
if str(EXPERIMENTS) not in sys.path:
    sys.path.insert(0, str(EXPERIMENTS))
import NDN_SVS_Worker_Necessity_Minindn as prior  # noqa: E402
import analyze_svs_worker_necessity as metrics  # noqa: E402


RATE = 600
QUALIFICATION_TIMING = (5, 15, 5)
FORMAL_TIMING = (10, 60, 10)


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON authority must be an object: {path}")
    return value


class CampaignLock:
    def __init__(self, campaign: Path):
        self.path = campaign / ".campaign.lock"
        self.handle: Any = None

    def __enter__(self) -> "CampaignLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = self.path.open("a+", encoding="utf-8")
        try:
            fcntl.flock(self.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            self.handle.close()
            raise RuntimeError(f"campaign already has a writer: {self.path}") from error
        return self

    def __exit__(self, *_: Any) -> None:
        if self.handle is not None:
            fcntl.flock(self.handle, fcntl.LOCK_UN)
            self.handle.close()


class ReceiptLedger:
    def __init__(self, root: Path):
        self.root = root

    def append(self, receipt: dict[str, Any]) -> Path:
        if receipt.get("schema") != "spec139.receipt.v1":
            raise RuntimeError("receipt schema mismatch")
        ordinal = receipt.get("ordinal")
        if not isinstance(ordinal, int) or ordinal not in range(1, 7):
            raise RuntimeError(f"invalid formal ordinal: {ordinal}")
        if receipt.get("retryCount") != 0:
            raise RuntimeError("Spec 139 forbids formal retries")
        self.root.mkdir(parents=True, exist_ok=True)
        path = self.root / f"{ordinal:02d}.json"
        try:
            with path.open("x", encoding="utf-8") as output:
                output.write(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
                output.flush()
                os.fsync(output.fileno())
        except FileExistsError as error:
            raise RuntimeError(f"formal receipt already exists: {path}") from error
        return path


def protected_digest() -> dict[str, Any]:
    return prior.tree_digest(
        [
            REPO / "specs/137-svs-serial-production-offload",
            REPO
            / "results/spec137-svs-serial-production-offload/"
            "t003-four-core-20260723-03",
            REPO / "specs/138-svs-worker-necessity",
            REPO
            / "results/spec138-svs-worker-necessity/"
            "confirmation01-20260723",
        ]
    )


def formal_cells() -> list[dict[str, Any]]:
    order = (
        (1, 1, "face-serial"),
        (2, 1, "worker-serial"),
        (3, 2, "worker-serial"),
        (4, 2, "face-serial"),
        (5, 3, "face-serial"),
        (6, 3, "worker-serial"),
    )
    warmup, measure, drain = FORMAL_TIMING
    return [
        {
            "ordinal": ordinal,
            "pair": pair,
            "mode": mode,
            "rate": RATE,
            "warmup": warmup,
            "measure": measure,
            "drain": drain,
            "retryCount": 0,
            "cellId": f"{ordinal:02d}-pair-{pair}-{mode}",
        }
        for ordinal, pair, mode in order
    ]


def qualify(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    if campaign.exists():
        raise RuntimeError(f"fresh campaign path required: {campaign}")
    campaign.mkdir(parents=True)
    with CampaignLock(campaign):
        subject = prior.verify_subject()
        protected = protected_digest()
        cpu_map, initial_busy = prior.choose_cpu_map()
        configs = prior.base.self_test_configs(subject)
        results: list[dict[str, Any]] = []
        for mode in metrics.MODES:
            qpath = prior.wait_quiescent(
                campaign, f"qualification-{mode}", cpu_map
            )
            warmup, measure, drain = QUALIFICATION_TIMING
            config = {
                "campaignId": campaign.name,
                "cellId": f"qualification-{mode}",
                "mode": mode,
                "rate": RATE,
                "warmup": warmup,
                "measure": measure,
                "drain": drain,
                "formal": False,
                "diagnostics": "enabled",
            }
            terminal, row = prior.run_one(
                campaign, subject, cpu_map, config, "qualification/cells"
            )
            results.append(
                {
                    "mode": mode,
                    "quiescenceRecord": str(qpath),
                    "terminal": terminal,
                    "metrics": row,
                }
            )
        face = next(
            row["metrics"] for row in results if row["mode"] == "face-serial"
        )
        worker = next(
            row["metrics"] for row in results if row["mode"] == "worker-serial"
        )
        checks = {
            "subject_current": subject["baseCommit"] == prior.EXPECTED_COMMIT,
            "same_binary": subject["binarySha256"] == prior.EXPECTED_BINARY,
            "runtime_delta_exact":
                prior.base.analysis.runtime_config_delta(
                    configs["face-serial"], configs["worker-serial"]
                )
                == prior.base.analysis.ALLOWED_TREATMENT_FIELDS,
            "face_admissible": bool(face.get("admissible", False)),
            "worker_admissible": bool(worker.get("admissible", False)),
            "face_pressure": bool(face.get("pressureGate", False)),
            "one_worker": configs["worker-serial"]["production_workers"] == 1,
            "no_formal_receipts": not (campaign / "receipts").exists(),
        }
        summary = {
            "schema": "spec139.qualification.v1",
            "campaignId": campaign.name,
            "rate": RATE,
            "timing": list(QUALIFICATION_TIMING),
            "subjectManifest": str(prior.SUBJECT.resolve()),
            "subjectManifestSha256": prior.sha256_file(prior.SUBJECT),
            "subject": subject,
            "protectedPredecessors": protected,
            "cpuMap": cpu_map,
            "initialBusyRatios": initial_busy,
            "runtimeConfigs": configs,
            "results": results,
            "checks": checks,
            "admitted": all(checks.values()),
            "runnerSha256": prior.sha256_file(Path(__file__)),
        }
        atomic_json(campaign / "qualification/summary.json", summary)
        return summary


def verify_qualification(campaign: Path) -> dict[str, Any]:
    summary = load_json(campaign / "qualification/summary.json")
    if (
        summary.get("schema") != "spec139.qualification.v1"
        or not summary.get("admitted")
        or not all(summary.get("checks", {}).values())
    ):
        raise RuntimeError("qualification is not admitted")
    prior.verify_subject()
    if protected_digest()["digest"] != summary["protectedPredecessors"]["digest"]:
        raise RuntimeError("protected predecessor evidence changed")
    if prior.sha256_file(prior.SUBJECT) != summary["subjectManifestSha256"]:
        raise RuntimeError("subject manifest changed")
    if summary["runnerSha256"] != prior.sha256_file(Path(__file__)):
        raise RuntimeError("Spec 139 runner changed after qualification")
    return summary


def seal_campaign(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    summary = verify_qualification(campaign)
    if (campaign / "campaign-manifest.json").exists():
        raise RuntimeError("campaign is already sealed")
    if (campaign / "receipts").exists():
        raise RuntimeError("receipts exist before seal")
    manifest = {
        "schema": "spec139.campaign.v1",
        "campaignId": campaign.name,
        "state": "sealed",
        "createdUnixNs": time.time_ns(),
        "rate": RATE,
        "binarySha256": summary["subject"]["binarySha256"],
        "subjectManifest": summary["subjectManifest"],
        "subjectManifestSha256": summary["subjectManifestSha256"],
        "protectedPredecessorDigest":
            summary["protectedPredecessors"]["digest"],
        "qualificationSha256": prior.sha256_file(
            campaign / "qualification/summary.json"
        ),
        "cpuMap": summary["cpuMap"],
        "automaticRetry": False,
        "cells": formal_cells(),
    }
    with CampaignLock(campaign):
        atomic_json(campaign / "campaign-manifest.json", manifest)
        (campaign / ".sealed").write_text(
            prior.sha256_file(campaign / "campaign-manifest.json") + "\n",
            encoding="utf-8",
        )
    return manifest


def verify_seal(campaign: Path) -> dict[str, Any]:
    manifest_path = campaign / "campaign-manifest.json"
    manifest = load_json(manifest_path)
    if (
        manifest.get("schema") != "spec139.campaign.v1"
        or manifest.get("automaticRetry") is not False
        or manifest.get("cells") != formal_cells()
        or manifest.get("rate") != RATE
    ):
        raise RuntimeError("campaign manifest contract mismatch")
    if (campaign / ".sealed").read_text().strip() != prior.sha256_file(
        manifest_path
    ):
        raise RuntimeError("campaign seal changed")
    summary = verify_qualification(campaign)
    if (
        manifest["protectedPredecessorDigest"]
        != summary["protectedPredecessors"]["digest"]
    ):
        raise RuntimeError("protected predecessor digest mismatch")
    return manifest


def run_formal(campaign: Path) -> list[dict[str, Any]]:
    campaign = campaign.resolve()
    outcomes: list[dict[str, Any]] = []
    with CampaignLock(campaign):
        manifest = verify_seal(campaign)
        subject = prior.verify_subject()
        ledger = ReceiptLedger(campaign / "receipts")
        for config in manifest["cells"]:
            verify_seal(campaign)
            qpath = prior.wait_quiescent(
                campaign, f"formal-{config['ordinal']:02d}", manifest["cpuMap"]
            )
            terminal, row = prior.run_one(
                campaign,
                subject,
                manifest["cpuMap"],
                {**config, "campaignId": campaign.name, "formal": True},
                "formal/cells",
            )
            receipt = {
                "schema": "spec139.receipt.v1",
                "ordinal": config["ordinal"],
                "pair": config["pair"],
                "mode": config["mode"],
                "rate": RATE,
                "retryCount": 0,
                "quiescenceRecord": str(qpath),
                "terminal": terminal,
                "admissionChecks": row.get("admissionChecks", {}),
                "admissible": (
                    terminal["status"] == "complete"
                    and bool(row.get("admissible", False))
                ),
                "metrics": row,
            }
            ledger.append(receipt)
            outcomes.append(receipt)
            if terminal["status"] == "infrastructure-invalid":
                break
        atomic_json(
            campaign / "formal/campaign-terminal.json",
            {
                "schema": "spec139.campaign-terminal.v1",
                "receiptCount": len(outcomes),
                "outcomes": outcomes,
            },
        )
    return outcomes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--qualify", action="store_true")
    actions.add_argument("--seal", action="store_true")
    actions.add_argument("--run-formal", action="store_true")
    args = parser.parse_args(argv)
    if args.qualify:
        result: Any = qualify(args.campaign)
    elif args.seal:
        result = seal_campaign(args.campaign)
    else:
        result = {"outcomes": run_formal(args.campaign)}
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
