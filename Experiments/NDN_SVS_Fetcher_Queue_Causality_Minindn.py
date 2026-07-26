#!/usr/bin/env python3
"""Once-only MiniNDN runner for the Spec 135 RSA diagnostic."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
from types import MethodType
from typing import Any


REPO = Path(__file__).resolve().parents[1]
SUBJECT = REPO / "build/spec135/subject-manifest.json"
BASE_RUNNER = REPO / "Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py"
DRIVER_TRANSFORM = REPO / "Experiments/build_svs_fetcher_queue_causality.py"
RSA_HELPER = REPO / "Experiments/ndn-svs-pubsub-benchmark/spec135-rsa-security.hpp"
PEERS = ("peer-a", "peer-b")
RATES = (200, 400, 600, 800, 1000)
FIXED = {
    "peers": list(PEERS),
    "profileMode": "enabled",
    "warmupSeconds": 10,
    "measureSeconds": 60,
    "drainSeconds": 10,
    "payloadBytes": 256,
    "linkDelayMs": 10,
    "bandwidthMbps": 100,
    "configuredLossPercent": 0,
    "attempt": 1,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_base_runner() -> Any:
    spec = importlib.util.spec_from_file_location("spec133_runner_for_spec135", BASE_RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot import Spec 133 MiniNDN runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_subject(path: Path) -> dict[str, Any]:
    subject = load_json(path)
    if subject.get("schemaVersion") != "spec135-rsa-subject-v1":
        raise RuntimeError("Spec 135 subject schema mismatch")
    for key in ("profiledBinary", "profiledLibrary", "generatedDriver",
                "diagnosticPatch", "driverTransform", "rsaHelper"):
        artifact = Path(subject[key])
        if not artifact.is_file() or sha256(artifact) != subject[f"{key}Sha256"]:
            raise RuntimeError(f"subject artifact changed: {key}")
    if subject.get("securityProfile", {}).get("data") != \
            "RSA-2048/SignatureSha256WithRsa":
        raise RuntimeError("subject is not RSA-2048")
    if subject.get("executionModel") != "single-face-io-thread" or \
       subject.get("publishApi") != "publish" or \
       subject.get("parallelWorkers") is not None:
        raise RuntimeError("subject execution model mismatch")
    return subject


def cell(cell_id: str, ordinal: int, rate: int, window: int,
         max_params: int, stage: str, subject: dict[str, Any]) -> dict[str, Any]:
    return {
        "cellId": cell_id,
        "ordinal": ordinal,
        "stage": stage,
        "ratePpsPerPeer": rate,
        "fetcherWindow": window,
        "maxApplicationParametersSize": max_params,
        "binary": subject["profiledBinary"],
        "binarySha256": subject["profiledBinarySha256"],
        "library": subject["profiledLibrary"],
        "librarySha256": subject["profiledLibrarySha256"],
        "profileStageCount": subject["profileConfig"]["stageCount"],
        "profileSampleModulus": subject["profileConfig"]["sampleModulus"],
        **FIXED,
    }


def make_contract(campaign_id: str, subject_path: Path) -> dict[str, Any]:
    subject = load_subject(subject_path)
    stage_a = [
        cell(f"{ordinal:02d}-rsa-sweep-{rate}", ordinal, rate, 10, 4096,
             "rsa-boundary-sweep", subject)
        for ordinal, rate in enumerate(RATES, 1)
    ]
    contract = {
        "schemaVersion": "spec135-campaign-contract-v1",
        "campaignId": campaign_id,
        "automaticRetry": False,
        "subjectManifest": str(subject_path.resolve()),
        "subjectManifestSha256": sha256(subject_path),
        "runner": str(Path(__file__).resolve()),
        "runnerSha256": sha256(Path(__file__)),
        "baseRunner": str(BASE_RUNNER.resolve()),
        "baseRunnerSha256": sha256(BASE_RUNNER),
        "driverTransform": str(DRIVER_TRANSFORM.resolve()),
        "driverTransformSha256": sha256(DRIVER_TRANSFORM),
        "rsaHelper": str(RSA_HELPER.resolve()),
        "rsaHelperSha256": sha256(RSA_HELPER),
        "boundaryRule": {
            "attemptedScheduledMinimum": 0.98,
            "aggregateDeliveredAttemptedMinimum": 0.98,
            "fallbackStressRate": 1000,
        },
        "stageA": stage_a,
        "stageBTemplates": [
            {"label": "W40-P4096", "fetcherWindow": 40,
             "maxApplicationParametersSize": 4096},
            {"label": "W10-P7168", "fetcherWindow": 10,
             "maxApplicationParametersSize": 7168},
            {"label": "W40-P7168", "fetcherWindow": 40,
             "maxApplicationParametersSize": 7168},
        ],
    }
    validate_contract(contract)
    return contract


def validate_contract(contract: dict[str, Any]) -> None:
    cells = contract.get("stageA", [])
    if contract.get("automaticRetry") is not False or len(cells) != 5:
        raise RuntimeError("stage A must contain five once-only cells")
    if [item["ratePpsPerPeer"] for item in cells] != list(RATES):
        raise RuntimeError("stage-A rate order changed")
    if any(item["attempt"] != 1 or item["fetcherWindow"] != 10 or
           item["maxApplicationParametersSize"] != 4096 for item in cells):
        raise RuntimeError("stage-A controls changed")
    templates = contract.get("stageBTemplates", [])
    expected = [(40, 4096), (10, 7168), (40, 7168)]
    if [(item["fetcherWindow"], item["maxApplicationParametersSize"])
            for item in templates] != expected:
        raise RuntimeError("stage-B template order changed")


def verify_contract(campaign: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    contract_path = campaign / "campaign-contract.json"
    contract = load_json(contract_path)
    validate_contract(contract)
    seal = campaign / ".contract-sealed"
    if not seal.is_file() or seal.read_text(encoding="utf-8").strip() != \
            sha256(contract_path):
        raise RuntimeError("campaign contract seal mismatch")
    for key in ("runner", "baseRunner", "driverTransform", "rsaHelper",
                "subjectManifest"):
        if sha256(Path(contract[key])) != contract[f"{key}Sha256"]:
            raise RuntimeError(f"frozen campaign authority changed: {key}")
    return contract, load_subject(Path(contract["subjectManifest"]))


def diagnostic_process_sample(processes: dict[str, Any]) -> dict[str, Any]:
    sample = {"monotonicNs": time.monotonic_ns(), "processes": {}}
    for role, process in processes.items():
        candidates = [process.pid]
        cursor = 0
        while cursor < len(candidates):
            children = Path(
                f"/proc/{candidates[cursor]}/task/{candidates[cursor]}/children")
            if children.is_file():
                candidates.extend(
                    int(value) for value in children.read_text().split()
                    if int(value) not in candidates)
            cursor += 1
        resolved = []
        for candidate in candidates:
            cmdline = Path(f"/proc/{candidate}/cmdline")
            if cmdline.is_file() and "svs-fetcher-queue-causality" in \
                    cmdline.read_bytes().replace(b"\0", b" ").decode(
                        errors="replace"):
                resolved.append(candidate)
        pid = resolved[-1] if resolved else process.pid
        stat = Path(f"/proc/{pid}/stat")
        status = Path(f"/proc/{pid}/status")
        entry: dict[str, Any] = {
            "pid": pid, "wrapperPid": process.pid,
            "peerResolved": bool(resolved),
        }
        if stat.is_file():
            fields = stat.read_text().split()
            entry["cpuTicks"] = int(fields[13]) + int(fields[14])
        if status.is_file():
            for line in status.read_text(
                    encoding="utf-8", errors="replace").splitlines():
                if line.startswith(("VmRSS:", "Threads:")):
                    key, value = line.split(":", 1)
                    entry[key] = value.strip()
        sample["processes"][role] = entry
    return sample


def install_minindn_node_app_environment() -> tuple[Any, Any]:
    """Make inherited Spec 133 raw host.popen calls honor MiniNDN node homes."""
    from minindn.minindn import Minindn
    from minindn.util import popenGetEnv

    original_start = Minindn.start

    def start_with_node_environment(instance: Any, *args: Any, **kwargs: Any) -> Any:
        result = original_start(instance, *args, **kwargs)
        for host in instance.net.hosts:
            home = host.params["params"]["homeDir"]
            environment = popenGetEnv(host)
            raw_popen = host.popen

            def isolated_popen(
                    _host: Any, *command: Any, _raw: Any = raw_popen,
                    _home: str = home, _environment: dict[str, str] = environment,
                    **parameters: Any) -> Any:
                parameters.setdefault("cwd", _home)
                parameters.setdefault("env", dict(_environment))
                return _raw(*command, **parameters)

            host.popen = MethodType(isolated_popen, host)
        return result

    Minindn.start = start_with_node_environment
    return Minindn, original_start


def run_one(base: Any, campaign: Path, config: dict[str, Any],
            subject: dict[str, Any], *, formal: bool = True) -> dict[str, Any]:
    original_environment = base.profile_environment
    original_process_sample = base.process_sample
    minindn_class, original_minindn_start = install_minindn_node_app_environment()

    def diagnostic_environment(mode: str, cell_id: str, peer_id: str,
                               loaded_subject: dict[str, Any]) -> dict[str, str]:
        values = original_environment(mode, cell_id, peer_id, loaded_subject)
        values["NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW"] = str(config["fetcherWindow"])
        values["NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS"] = \
            str(config["maxApplicationParametersSize"])
        return values

    base.profile_environment = diagnostic_environment
    base.process_sample = diagnostic_process_sample
    try:
        receipt = base.run_cell(campaign, config, subject, formal=formal)
    finally:
        base.profile_environment = original_environment
        base.process_sample = original_process_sample
        minindn_class.start = original_minindn_start
    environment_path = campaign / "cells" / config["cellId"] / "environment.json"
    if environment_path.is_file():
        environment = load_json(environment_path)
        environment.update({
            "dataSigner": "RSA-2048/SignatureSha256WithRsa",
            "applicationLaunch": "MiniNDN per-node HOME/cwd environment",
            "fetcherWindow": config["fetcherWindow"],
            "maxApplicationParametersSize": config["maxApplicationParametersSize"],
        })
        atomic_json(environment_path, environment)
    print(json.dumps({"cellId": config["cellId"], "status": receipt["status"]}),
          flush=True)
    return receipt


def measured_metrics(campaign: Path, config: dict[str, Any]) -> dict[str, Any]:
    scheduled: dict[str, int] = {}
    attempted: dict[str, int] = {}
    delivered: dict[str, int] = {}
    rsa_proof: dict[str, bool] = {}
    for peer in PEERS:
        events = [
            json.loads(line) for line in
            (campaign / "cells" / config["cellId"] /
             f"{peer}-events.jsonl").read_text(encoding="utf-8").splitlines()
        ]
        stop = [event for event in events if event["event"] == "process-stop"]
        start = [event for event in events if event["event"] == "process-start"]
        if len(stop) != 1 or len(start) != 1:
            raise RuntimeError(f"missing process boundary events: {config['cellId']}/{peer}")
        scheduled[peer] = int(stop[0]["details"]["scheduledMeasured"])
        attempted[peer] = sum(event["event"] == "api-return" and
                              event["phase"] == "measured" for event in events)
        delivered[peer] = sum(event["event"] == "delivery" and
                              event["phase"] == "measured" for event in events)
        details = start[0]["details"]
        rsa_proof[peer] = details.get("dataSigner") == "RSA-2048" and \
            int(details.get("rsaSignatureType", -1)) == 1
    total_attempted = sum(attempted.values())
    total_delivered = sum(delivered.values())
    return {
        "scheduled": scheduled,
        "attempted": attempted,
        "delivered": delivered,
        "attemptedScheduledRatio": {
            peer: attempted[peer] / scheduled[peer] if scheduled[peer] else 0
            for peer in PEERS
        },
        "aggregateDeliveredAttemptedRatio":
            total_delivered / total_attempted if total_attempted else 0,
        "rsaProof": rsa_proof,
    }


def select_boundary(campaign: Path, contract: dict[str, Any]) -> dict[str, Any]:
    rows = []
    threshold_a = contract["boundaryRule"]["attemptedScheduledMinimum"]
    threshold_d = contract["boundaryRule"]["aggregateDeliveredAttemptedMinimum"]
    selected = None
    for config in contract["stageA"]:
        receipt = load_json(campaign / "receipts" / f"{config['cellId']}.json")
        if receipt["status"] != "COMPLETE":
            rows.append({"cellId": config["cellId"], "ratePpsPerPeer":
                         config["ratePpsPerPeer"], "valid": False})
            continue
        metrics = measured_metrics(campaign, config)
        if not all(metrics["rsaProof"].values()):
            raise RuntimeError(f"RSA proof failed in {config['cellId']}")
        unstable = any(value < threshold_a for value in
                       metrics["attemptedScheduledRatio"].values()) or \
            metrics["aggregateDeliveredAttemptedRatio"] < threshold_d
        rows.append({"cellId": config["cellId"],
                     "ratePpsPerPeer": config["ratePpsPerPeer"],
                     "valid": True, "unstable": unstable, "metrics": metrics})
        if unstable and selected is None:
            selected = config
    if selected is None:
        valid = [config for config in contract["stageA"]
                 if any(row.get("cellId") == config["cellId"] and row.get("valid")
                        for row in rows)]
        if not valid:
            raise RuntimeError("no infrastructure-valid RSA sweep cell")
        selected = valid[-1]
        kind = "highest-tested-stress-point-no-boundary"
    else:
        kind = "first-rsa-instability-boundary"
    return {
        "schemaVersion": "spec135-boundary-selection-v1",
        "campaignId": contract["campaignId"],
        "selectionKind": kind,
        "selectedBaselineCellId": selected["cellId"],
        "selectedRatePpsPerPeer": selected["ratePpsPerPeer"],
        "rule": contract["boundaryRule"],
        "sweep": rows,
    }


def make_stage_b(contract: dict[str, Any], subject: dict[str, Any],
                 selection: dict[str, Any]) -> dict[str, Any]:
    rate = selection["selectedRatePpsPerPeer"]
    cells = []
    for ordinal, template in enumerate(contract["stageBTemplates"], 6):
        cells.append(cell(
            f"{ordinal:02d}-rsa-{rate}-{template['label']}",
            ordinal, rate, template["fetcherWindow"],
            template["maxApplicationParametersSize"], "factor-treatment", subject,
        ))
    return {
        "schemaVersion": "spec135-stage-b-manifest-v1",
        "campaignId": contract["campaignId"],
        "automaticRetry": False,
        "selectedRatePpsPerPeer": rate,
        "baselineCellId": selection["selectedBaselineCellId"],
        "cells": cells,
    }


def plan(campaign: Path, campaign_id: str, subject_path: Path) -> None:
    if campaign.exists():
        raise RuntimeError(f"campaign path already exists: {campaign}")
    campaign.mkdir(parents=True)
    contract = make_contract(campaign_id, subject_path)
    path = campaign / "campaign-contract.json"
    atomic_json(path, contract)
    (campaign / ".contract-sealed").write_text(sha256(path) + "\n",
                                                encoding="utf-8")
    print(path)


def preflight(output: Path, subject_path: Path) -> None:
    if output.exists():
        raise RuntimeError(f"preflight output already exists: {output}")
    subject = load_subject(subject_path)
    config = cell("preflight-rsa200", 0, 200, 10, 4096,
                  "preflight", subject)
    config.update({"warmupSeconds": 1, "measureSeconds": 2,
                   "drainSeconds": 1})
    base = load_base_runner()
    receipt = run_one(base, output, config, subject, formal=False)
    if receipt["status"] != "COMPLETE":
        raise RuntimeError(f"preflight failed: {receipt}")
    metrics = measured_metrics(output, config)
    if not all(metrics["rsaProof"].values()):
        raise RuntimeError("preflight RSA proof failed")
    parsed = base.parse_cell_metrics(output / "cells" / config["cellId"], config)
    if not parsed["profileComplete"]:
        raise RuntimeError("preflight profiler output incomplete")
    samples = [
        json.loads(line) for line in
        (output / "cells" / config["cellId"] /
         "resource-samples.jsonl").read_text(encoding="utf-8").splitlines()
    ]
    if not samples or not all(
            sample["processes"][peer].get("peerResolved")
            for sample in samples for peer in PEERS):
        raise RuntimeError("preflight process sampling did not resolve peer binaries")
    atomic_json(output / "preflight-summary.json", {
        "schemaVersion": "spec135-preflight-summary-v1",
        "status": "PASS",
        "rsaProof": metrics["rsaProof"],
        "attemptedScheduledRatio": metrics["attemptedScheduledRatio"],
        "aggregateDeliveredAttemptedRatio":
            metrics["aggregateDeliveredAttemptedRatio"],
        "profileComplete": parsed["profileComplete"],
        "aggregateCpuPercent": parsed["aggregateCpuPercent"],
    })


def execute(campaign: Path) -> None:
    contract, subject = verify_contract(campaign)
    if (campaign / "receipts").exists() or (campaign / "cells").exists():
        raise RuntimeError("campaign already consumed; no resume or retry is allowed")
    base = load_base_runner()
    for config in contract["stageA"]:
        run_one(base, campaign, config, subject)
    selection = select_boundary(campaign, contract)
    selection_path = campaign / "boundary-selection.json"
    atomic_json(selection_path, selection)
    stage_b = make_stage_b(contract, subject, selection)
    stage_b["boundarySelectionSha256"] = sha256(selection_path)
    stage_b_path = campaign / "stage-b-manifest.json"
    atomic_json(stage_b_path, stage_b)
    (campaign / ".stage-b-sealed").write_text(sha256(stage_b_path) + "\n",
                                               encoding="utf-8")
    for config in stage_b["cells"]:
        run_one(base, campaign, config, subject)
    receipts = sorted((campaign / "receipts").glob("*.json"))
    if len(receipts) != 8:
        raise RuntimeError(f"expected 8 terminal receipts, got {len(receipts)}")
    atomic_json(campaign / "execution-summary.json", {
        "schemaVersion": "spec135-execution-summary-v1",
        "campaignId": contract["campaignId"],
        "receiptCount": len(receipts),
        "selectedRatePpsPerPeer": selection["selectedRatePpsPerPeer"],
        "selectionKind": selection["selectionKind"],
        "statuses": {
            path.stem: load_json(path)["status"] for path in receipts
        },
    })


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("plan")
    create.add_argument("--campaign", type=Path, required=True)
    create.add_argument("--campaign-id", required=True)
    create.add_argument("--subject-manifest", type=Path, default=SUBJECT)
    check = sub.add_parser("preflight")
    check.add_argument("--output", type=Path, required=True)
    check.add_argument("--subject-manifest", type=Path, default=SUBJECT)
    run_parser = sub.add_parser("run")
    run_parser.add_argument("--campaign", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "plan":
        plan(args.campaign.resolve(), args.campaign_id,
             args.subject_manifest.resolve())
    elif args.command == "preflight":
        preflight(args.output.resolve(), args.subject_manifest.resolve())
    else:
        execute(args.campaign.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
