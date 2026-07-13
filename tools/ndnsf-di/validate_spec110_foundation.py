#!/usr/bin/env python3
"""Cross-contract foundation gate for the frozen Spec 110 campaign."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import spec110_candidate as candidate
import spec110_cluster as cluster
import spec110_evidence as evidence
import spec110_state as state
import spec110_storage as storage
import spec110_workload as workload


class FoundationError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise FoundationError(code + (f":{detail}" if detail else ""))


def _file_digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def validate() -> dict[str, object]:
    spec = ROOT / "specs/110-itiger-qwen-live-inference"
    fixtures = ROOT / "tests/container/itiger-qwen-live/fixtures"
    results = ROOT / "results/spec110-itiger-qwen-live/offline-foundation"
    campaign_doc = _json(spec / "experiment/campaign-v1.json")
    bindings = campaign_doc["bindingDigests"]
    checks = []

    baseline = (spec / "evidence/source-baseline.md").read_text(encoding="utf-8")
    match = re.search(r"\| Source snapshot \| `(sha256:[0-9a-f]{64})` \|", baseline)
    if match is None or bindings["sourceBaselineDigest"] != match.group(1):
        _fail("FOUNDATION_SOURCE_BASELINE_MISMATCH")
    checks.append("source-baseline")

    model_path = fixtures / "model-ladder.json"
    models = _json(model_path)
    if candidate.digest_object(models) != bindings["modelLadderDigest"]:
        _fail("FOUNDATION_MODEL_LADDER_DIGEST_MISMATCH")
    if len(models.get("models", [])) != 7 or any(
        re.fullmatch(r"[0-9a-f]{40}", row.get("revision", "")) is None or not row.get("license")
        for row in models.get("models", [])
    ):
        _fail("FOUNDATION_MODEL_LADDER_INVALID")
    checks.append("model-ladder")

    workload_path = fixtures / "workload.json"
    workload_doc = _json(workload_path)
    workload.validate_workload(workload_doc)
    if candidate.digest_object(workload_doc) != bindings["workloadDigest"]:
        _fail("FOUNDATION_WORKLOAD_DIGEST_MISMATCH")
    checks.append("workload")

    expected_files = {
        "identityContractDigest": HERE / "spec110_candidate.py",
        "clusterContractDigest": HERE / "spec110_cluster.py",
        "evidenceContractDigest": spec / "contracts/run-evidence.schema.json",
    }
    for field, path in expected_files.items():
        if _file_digest(path) != bindings[field]:
            _fail("FOUNDATION_CONTRACT_DIGEST_MISMATCH", field)
    if candidate.freeze_campaign({"bindingDigests": bindings}) != campaign_doc:
        _fail("FOUNDATION_CAMPAIGN_FREEZE_MISMATCH")
    if tuple(cluster.MUTABLE_FACTS) != ("partition", "gres", "quota", "versions", "addresses"):
        _fail("FOUNDATION_CLUSTER_FACT_CONTRACT_MISMATCH")
    checks.extend(["identity-contract", "cluster-contract", "campaign-freeze"])

    lifecycle = state.new_execution_state("spec110-cell-" + "a" * 20)
    lifecycle = state.transition(lifecycle, "PREFLIGHT_BLOCKED", {"reasonCode": "FOUNDATION_PROBE"})
    if state.can_close_live_task(lifecycle):
        _fail("FOUNDATION_PRESTART_CLOSE_VIOLATION")
    checks.append("execution-state")

    storage_cases = _json(fixtures / "storage/cases.json")
    if storage.evaluate_admission(storage_cases["admissionPass"])["status"] != "PASS":
        _fail("FOUNDATION_STORAGE_PASS_INVALID")
    if storage.evaluate_admission(storage_cases["quotaFull"])["status"] != "BLOCKED":
        _fail("FOUNDATION_STORAGE_BLOCK_INVALID")
    checks.append("storage")

    expected_sif = "a" * 64
    evidence.validate_evidence(_json(fixtures / "evidence/single-node-pass.json"), expected_sif_sha256=expected_sif)
    evidence.validate_evidence(_json(fixtures / "evidence/multi-node-pass.json"), expected_sif_sha256=expected_sif)
    checks.append("evidence")

    summary = _json(results / "summary.json")
    junit = results / "junit.xml"
    if _file_digest(junit) != "sha256:" + summary["junitSha256"]:
        _fail("FOUNDATION_JUNIT_DIGEST_MISMATCH")
    suite = ET.parse(junit).getroot()
    if (
        suite.attrib.get("tests") != "49"
        or suite.attrib.get("failures") != "0"
        or suite.attrib.get("errors") != "0"
        or suite.attrib.get("skipped") != "0"
    ):
        _fail("FOUNDATION_JUNIT_RESULT_INVALID")
    checks.append("offline-junit")

    return {
        "schemaVersion": "spec110-foundation-gate-v1",
        "status": "PASS",
        "campaignId": campaign_doc["campaignId"],
        "campaignDigest": campaign_doc["campaignDigest"],
        "checks": checks,
        "checkCount": len(checks),
        "offlineTests": 49,
        "liveSubmissionCount": 0,
        "foundationGateSatisfied": True,
        "liveSubmissionEligible": False,
        "nextRequiredGate": "T031-T049_RUNTIME_RELEASE_AND_PROBE",
        "physicalProduction": "DEFERRED",
        "physicalProductionOwner": "Spec 106",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        report = validate()
    except Exception as error:
        report = {"schemaVersion": "spec110-foundation-gate-v1", "status": "FAIL", "reasonCode": str(error)}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        target = Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
