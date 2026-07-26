#!/usr/bin/env python3
"""Spec 138 same-binary Face-vs-one-worker MiniNDN authority."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any


REPO = Path(__file__).resolve().parents[1]
EXPERIMENTS = Path(__file__).resolve().parent
if str(EXPERIMENTS) not in sys.path:
    sys.path.insert(0, str(EXPERIMENTS))
import NDN_SVS_Serial_Production_Offload_Minindn as base  # noqa: E402
import analyze_svs_worker_necessity as analysis  # noqa: E402


SUBJECT = REPO / "build/spec137-four-core/source-manifest.json"
EXPECTED_COMMIT = "6bb34545b4f89f1f6c265a68c18f1a40ade413eb"
EXPECTED_BINARY = (
    "c4f3b296137033eb82d0e888bd2cacdf492a0e06609de12c3e1e301547e435ac"
)
SVS_REPO = Path("/home/tianxing/NDN/ndn-svs")
RATES = analysis.RATES
CALIBRATION_TIMING = (5, 15, 5)
FORMAL_TIMING = (10, 60, 10)
SMOKE_TIMING = (1, 3, 2)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def tree_digest(roots: list[Path]) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    for root in roots:
        if not root.exists():
            raise RuntimeError(f"protected Spec 137 root is missing: {root}")
        for path in sorted(item for item in root.rglob("*") if item.is_file()):
            rows.append(
                {
                    "path": str(path.resolve()),
                    "bytes": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
            )
    encoded = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
    return {
        "schema": "spec138.protected-tree.v1",
        "roots": [str(root.resolve()) for root in roots],
        "records": rows,
        "digest": hashlib.sha256(encoded).hexdigest(),
    }


def spec137_protected_digest() -> dict[str, Any]:
    return tree_digest(
        [
            REPO / "specs/137-svs-serial-production-offload",
            REPO
            / "results/spec137-svs-serial-production-offload/"
            "t003-four-core-20260723-03",
        ]
    )


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
        if receipt.get("schema") != "spec138.receipt.v1":
            raise RuntimeError("receipt schema mismatch")
        ordinal = receipt.get("ordinal")
        if not isinstance(ordinal, int) or ordinal not in range(1, 7):
            raise RuntimeError(f"invalid receipt ordinal: {ordinal}")
        if receipt.get("retryCount") != 0:
            raise RuntimeError("Spec 138 forbids formal retries")
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


def read_cpu_counters() -> dict[int, tuple[int, int]]:
    values: dict[int, tuple[int, int]] = {}
    for line in Path("/proc/stat").read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or not fields[0].startswith("cpu") or fields[0] == "cpu":
            continue
        if not fields[0][3:].isdigit():
            continue
        numbers = [int(value) for value in fields[1:]]
        total = sum(numbers)
        idle = numbers[3] + (numbers[4] if len(numbers) > 4 else 0)
        values[int(fields[0][3:])] = (total, idle)
    return values


def sample_cpu_busy(seconds: float = 1.0) -> dict[int, float]:
    before = read_cpu_counters()
    time.sleep(seconds)
    after = read_cpu_counters()
    ratios: dict[int, float] = {}
    for cpu in sorted(set(before) & set(after)):
        total = after[cpu][0] - before[cpu][0]
        idle = after[cpu][1] - before[cpu][1]
        ratios[cpu] = 1.0 - idle / total if total > 0 else 1.0
    return ratios


def choose_cpu_map() -> tuple[dict[str, Any], dict[int, float]]:
    allowed = sorted(os.sched_getaffinity(0))
    if len(allowed) != 4:
        raise RuntimeError(f"Spec 138 requires exactly four allowed CPUs: {allowed}")
    busy = sample_cpu_busy(2.0)
    ordered = sorted(allowed, key=lambda cpu: (busy.get(cpu, 1.0), cpu))
    publisher, receiver, worker, nfd = ordered
    cpu_map = {
        "schema": "spec138.cpu-map.v1",
        "allowed": allowed,
        "peer-a": {"main": publisher, "face": publisher, "worker": worker},
        "peer-b": {"main": receiver, "face": receiver, "worker": worker},
        "nfd": {"peer-a": nfd, "peer-b": nfd},
        "roles": {
            "publisherPacerAndFace": publisher,
            "receiver": receiver,
            "singleWorker": worker,
            "bothNfds": nfd,
        },
    }
    return cpu_map, busy


def wait_quiescent(
    root: Path,
    label: str,
    cpu_map: dict[str, Any],
    *,
    timeout_seconds: int = 60,
) -> Path:
    path = root / "host-quiescence" / f"{label}.json"
    if path.exists():
        raise RuntimeError(f"quiescence authority already exists: {path}")
    started = time.monotonic()
    attempts: list[dict[str, Any]] = []
    roles = cpu_map["roles"]
    while time.monotonic() - started < timeout_seconds:
        busy = sample_cpu_busy(1.0)
        selected = {
            role: busy[int(cpu)] for role, cpu in roles.items()
        }
        passed = (
            selected["publisherPacerAndFace"] <= 0.30
            and selected["receiver"] <= 0.30
            and selected["singleWorker"] <= 0.30
            and selected["bothNfds"] <= 0.60
            and os.getloadavg()[0] <= 2.0
        )
        attempt = {
            "monotonicNs": time.monotonic_ns(),
            "busyRatios": selected,
            "loadAverage": list(os.getloadavg()),
            "passed": passed,
        }
        attempts.append(attempt)
        if passed:
            value = {
                "schema": "spec138.host-quiescence.v1",
                "label": label,
                "cpuMap": cpu_map,
                "thresholds": {
                    "applicationCpuBusyMax": 0.30,
                    "nfdCpuBusyMax": 0.60,
                    "load1Max": 2.0,
                    "timeoutSeconds": timeout_seconds,
                },
                "attempts": attempts,
                "final": attempt,
                "passed": True,
            }
            atomic_json(path, value)
            return path.resolve()
        time.sleep(1.0)
    value = {
        "schema": "spec138.host-quiescence.v1",
        "label": label,
        "cpuMap": cpu_map,
        "attempts": attempts,
        "final": attempts[-1] if attempts else {},
        "passed": False,
    }
    atomic_json(path, value)
    raise RuntimeError(f"host quiescence timeout: {path}")


def formal_cells(rate: int) -> list[dict[str, Any]]:
    if rate not in RATES:
        raise RuntimeError(f"rate is outside the registered ladder: {rate}")
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
            "rate": rate,
            "warmup": warmup,
            "measure": measure,
            "drain": drain,
            "retryCount": 0,
            "cellId": f"{ordinal:02d}-pair-{pair}-{mode}",
        }
        for ordinal, pair, mode in order
    ]


def verify_subject() -> dict[str, Any]:
    subject = base.subject_builder.load_subject(SUBJECT)
    if subject["baseCommit"] != EXPECTED_COMMIT:
        raise RuntimeError("subject commit changed")
    if subject["binarySha256"] != EXPECTED_BINARY:
        raise RuntimeError("subject binary authority changed")
    if sha256_file(Path(subject["binary"])) != EXPECTED_BINARY:
        raise RuntimeError("subject binary bytes changed")
    head = subprocess.run(
        ["git", "-C", str(SVS_REPO), "rev-parse", "HEAD"],
        check=True, text=True, stdout=subprocess.PIPE
    ).stdout.strip()
    status = subprocess.run(
        ["git", "-C", str(SVS_REPO), "status", "--porcelain"],
        check=True, text=True, stdout=subprocess.PIPE
    ).stdout
    if head != EXPECTED_COMMIT or status:
        raise RuntimeError("active NDN-SVS checkout is not clean at subject commit")
    return subject


def run_one(
    campaign: Path,
    subject: dict[str, Any],
    cpu_map: dict[str, Any],
    config: dict[str, Any],
    namespace: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    terminal = base.run_cell(
        campaign, subject, cpu_map, config, namespace=namespace
    )
    if terminal["status"] != "complete":
        return terminal, {}
    cell = campaign / namespace / config["cellId"]
    metrics = analysis.cell_metrics(cell, campaign.name, config)
    return terminal, metrics


def run_preflight(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    if campaign.exists():
        raise RuntimeError(f"fresh campaign path required: {campaign}")
    campaign.mkdir(parents=True)
    with CampaignLock(campaign):
        subject = verify_subject()
        protected = spec137_protected_digest()
        cpu_map, initial_busy = choose_cpu_map()
        configs = base.self_test_configs(subject)
        smokes: list[dict[str, Any]] = []
        for mode in analysis.MODES:
            qpath = wait_quiescent(campaign, f"preflight-{mode}", cpu_map)
            warmup, measure, drain = SMOKE_TIMING
            config = {
                "campaignId": campaign.name,
                "cellId": f"smoke-{mode}",
                "mode": mode,
                "rate": 60,
                "warmup": warmup,
                "measure": measure,
                "drain": drain,
                "formal": False,
                "diagnostics": "enabled",
            }
            terminal, metrics = run_one(
                campaign, subject, cpu_map, config, "preflight/smoke"
            )
            smokes.append(
                {
                    "mode": mode,
                    "quiescenceRecord": str(qpath),
                    "terminal": terminal,
                    "metrics": metrics,
                }
            )
        checks = {
            "subject_commit_current": subject["baseCommit"] == EXPECTED_COMMIT,
            "same_binary": subject["binarySha256"] == EXPECTED_BINARY,
            "boost_1_71_only": subject["boost"]["versionNumber"] == 107100,
            "runtime_delta_exact":
                base.analysis.runtime_config_delta(
                    configs["face-serial"], configs["worker-serial"]
                )
                == base.analysis.ALLOWED_TREATMENT_FIELDS,
            "two_smokes_complete": all(
                row["terminal"]["status"] == "complete" for row in smokes
            ),
            "one_signer": all(
                row["metrics"].get("maxActiveSigners") == 1 for row in smokes
            ),
            "zero_fallback": all(
                row["metrics"].get("fallbacks") == 0 for row in smokes
            ),
            "accounting_complete": all(
                row["metrics"].get("productionAccountingRemainder") == 0
                and row["metrics"].get("publicationAccountingRemainder") == 0
                for row in smokes
            ),
            "shutdown_drained": all(
                row["metrics"].get("shutdownDrained") is True for row in smokes
            ),
            "no_formal_receipts": not (campaign / "receipts").exists(),
        }
        summary = {
            "schema": "spec138.preflight.v1",
            "campaignId": campaign.name,
            "subjectManifest": str(SUBJECT.resolve()),
            "subjectManifestSha256": sha256_file(SUBJECT),
            "subject": subject,
            "protectedSpec137": protected,
            "cpuMap": cpu_map,
            "initialBusyRatios": initial_busy,
            "runtimeConfigs": configs,
            "smokes": smokes,
            "checks": checks,
            "admitted": all(checks.values()),
            "runnerSha256": sha256_file(Path(__file__)),
            "analyzerSha256": sha256_file(Path(analysis.__file__)),
        }
        atomic_json(campaign / "preflight/preflight-summary.json", summary)
        return summary


def verify_preflight(campaign: Path) -> dict[str, Any]:
    summary = load_json(campaign / "preflight/preflight-summary.json")
    if summary.get("schema") != "spec138.preflight.v1":
        raise RuntimeError("preflight schema mismatch")
    if not summary.get("admitted") or not all(summary.get("checks", {}).values()):
        raise RuntimeError("preflight is not admitted")
    verify_subject()
    current = spec137_protected_digest()
    if current["digest"] != summary["protectedSpec137"]["digest"]:
        raise RuntimeError("protected Spec 137 evidence changed")
    if sha256_file(SUBJECT) != summary["subjectManifestSha256"]:
        raise RuntimeError("subject manifest changed")
    return summary


def calibrate_and_qualify(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    if (campaign / "calibration").exists():
        raise RuntimeError("calibration already exists")
    preflight = verify_preflight(campaign)
    subject = preflight["subject"]
    cpu_map = preflight["cpuMap"]
    rows: list[dict[str, Any]] = []
    with CampaignLock(campaign):
        for rate in RATES:
            cell_id = f"control-{rate}"
            qpath = wait_quiescent(campaign, cell_id, cpu_map)
            warmup, measure, drain = CALIBRATION_TIMING
            config = {
                "campaignId": campaign.name,
                "cellId": cell_id,
                "mode": "face-serial",
                "rate": rate,
                "warmup": warmup,
                "measure": measure,
                "drain": drain,
                "formal": False,
                "diagnostics": "enabled",
            }
            terminal, metrics = run_one(
                campaign, subject, cpu_map, config, "calibration/cells"
            )
            row = {
                "rate": rate,
                "mode": "face-serial",
                "quiescenceRecord": str(qpath),
                "terminal": terminal,
                **metrics,
            }
            rows.append(row)
            selection = analysis.select_control_rate(rows)
            if selection["selectedRate"] is not None:
                break
        selection = analysis.select_control_rate(rows)
        selected = selection["selectedRate"]
        qualification: dict[str, Any] = {}
        if selected is not None:
            qpath = wait_quiescent(campaign, f"worker-{selected}", cpu_map)
            warmup, measure, drain = CALIBRATION_TIMING
            config = {
                "campaignId": campaign.name,
                "cellId": f"worker-{selected}",
                "mode": "worker-serial",
                "rate": selected,
                "warmup": warmup,
                "measure": measure,
                "drain": drain,
                "formal": False,
                "diagnostics": "enabled",
            }
            terminal, metrics = run_one(
                campaign, subject, cpu_map, config, "qualification/cells"
            )
            qualification = {
                "quiescenceRecord": str(qpath),
                "terminal": terminal,
                **metrics,
            }
            if terminal["status"] != "complete" or not metrics.get(
                "admissible", False
            ):
                selection["selectedRate"] = None
                selection["reason"] = "WORKER_QUALIFICATION_FAILED"
        selection["qualification"] = qualification
        selection["subjectManifestSha256"] = preflight["subjectManifestSha256"]
        selection["binarySha256"] = subject["binarySha256"]
        atomic_json(campaign / "calibration/rate-selection.json", selection)
        return selection


def seal_campaign(campaign: Path) -> dict[str, Any]:
    campaign = campaign.resolve()
    preflight = verify_preflight(campaign)
    selection_path = campaign / "calibration/rate-selection.json"
    selection = load_json(selection_path)
    rate = selection.get("selectedRate")
    if rate is None:
        raise RuntimeError(f"no qualified pressure rate: {selection['reason']}")
    if (campaign / "campaign-manifest.json").exists():
        raise RuntimeError("campaign already sealed")
    if (campaign / "receipts").exists():
        raise RuntimeError("formal receipts exist before seal")
    manifest = {
        "schema": "spec138.campaign.v1",
        "campaignId": campaign.name,
        "state": "sealed",
        "createdUnixNs": time.time_ns(),
        "subjectManifest": preflight["subjectManifest"],
        "subjectManifestSha256": preflight["subjectManifestSha256"],
        "binarySha256": preflight["subject"]["binarySha256"],
        "protectedSpec137Digest": preflight["protectedSpec137"]["digest"],
        "preflightSha256": sha256_file(
            campaign / "preflight/preflight-summary.json"
        ),
        "rateSelection": str(selection_path.resolve()),
        "rateSelectionSha256": sha256_file(selection_path),
        "cpuMap": preflight["cpuMap"],
        "frozenRate": int(rate),
        "automaticRetry": False,
        "cells": formal_cells(int(rate)),
    }
    with CampaignLock(campaign):
        atomic_json(campaign / "campaign-manifest.json", manifest)
        (campaign / ".sealed").write_text(
            sha256_file(campaign / "campaign-manifest.json") + "\n",
            encoding="utf-8",
        )
    return manifest


def verify_seal(campaign: Path) -> dict[str, Any]:
    manifest_path = campaign / "campaign-manifest.json"
    manifest = load_json(manifest_path)
    if (
        manifest.get("schema") != "spec138.campaign.v1"
        or manifest.get("automaticRetry") is not False
        or manifest.get("cells") != formal_cells(int(manifest["frozenRate"]))
    ):
        raise RuntimeError("sealed manifest contract mismatch")
    if (campaign / ".sealed").read_text().strip() != sha256_file(manifest_path):
        raise RuntimeError("campaign seal mismatch")
    preflight = verify_preflight(campaign)
    if (
        manifest["protectedSpec137Digest"]
        != preflight["protectedSpec137"]["digest"]
    ):
        raise RuntimeError("protected Spec 137 digest mismatch")
    return manifest


def run_formal(campaign: Path) -> list[dict[str, Any]]:
    campaign = campaign.resolve()
    outcomes: list[dict[str, Any]] = []
    with CampaignLock(campaign):
        manifest = verify_seal(campaign)
        subject = verify_subject()
        ledger = ReceiptLedger(campaign / "receipts")
        for config in manifest["cells"]:
            verify_seal(campaign)
            qpath = wait_quiescent(
                campaign, f"formal-{config['ordinal']:02d}", manifest["cpuMap"]
            )
            terminal, metrics = run_one(
                campaign,
                subject,
                manifest["cpuMap"],
                {**config, "campaignId": campaign.name, "formal": True},
                "formal/cells",
            )
            receipt = {
                "schema": "spec138.receipt.v1",
                "ordinal": config["ordinal"],
                "pair": config["pair"],
                "mode": config["mode"],
                "rate": config["rate"],
                "retryCount": 0,
                "quiescenceRecord": str(qpath),
                "terminal": terminal,
                "admissionChecks": metrics.get("admissionChecks", {}),
                "admissible": (
                    terminal["status"] == "complete"
                    and bool(metrics.get("admissible", False))
                ),
                "metrics": metrics,
            }
            ledger.append(receipt)
            outcomes.append(receipt)
            if terminal["status"] == "infrastructure-invalid":
                break
        atomic_json(
            campaign / "formal/campaign-terminal.json",
            {
                "schema": "spec138.campaign-terminal.v1",
                "receiptCount": len(outcomes),
                "outcomes": outcomes,
            },
        )
    return outcomes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign", type=Path, required=True)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--preflight", action="store_true")
    actions.add_argument("--calibrate-and-qualify", action="store_true")
    actions.add_argument("--seal", action="store_true")
    actions.add_argument("--run-formal", action="store_true")
    args = parser.parse_args(argv)
    if args.preflight:
        result = run_preflight(args.campaign)
    elif args.calibrate_and_qualify:
        result = calibrate_and_qualify(args.campaign)
    elif args.seal:
        result = seal_campaign(args.campaign)
    else:
        result = {"outcomes": run_formal(args.campaign)}
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
