#!/usr/bin/env python3
"""Immutable admission/campaign identity for Spec 168.

This tool freezes metadata only. It never builds a SIF, prepares a model,
copies payloads, starts MiniNDN, or submits Slurm work.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Any, Dict, Iterable, List


SCHEMA = "ndnsf-di.spec168-campaign.v3"
EXPERIMENT_SCHEMA = "ndnsf-di.spec168-experiment.v3"
SCHEDULE_SCHEMA = "ndnsf-di.spec168-schedule.v3"
DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")

REQUIRED_BINDINGS = (
    "baselineDigest",
    "sourceDigest",
    "runtimeSifDigest",
    "localFixtureManifestDigest",
    "remoteSmallStageManifestDigest",
    "remoteLargeStageManifestDigest",
    "sourceBundleDigest",
    "localGateManifestDigest",
    "exactSifPreflightDigest",
    "strategyDigest",
    "promptSetDigest",
    "routeDigest",
    "analyzerDigest",
    "scheduleDigest",
)

CANDIDATE_BINDINGS = (
    "baselineDigest",
    "sourceDigest",
    "runtimeSifDigest",
    "localFixtureManifestDigest",
    "remoteSmallStageManifestDigest",
    "remoteLargeStageManifestDigest",
    "sourceBundleDigest",
    "localGateManifestDigest",
    "exactSifPreflightDigest",
    "strategyDigest",
)

PHASES = (
    ("focused", False, "T003"),
    ("real-minindn", False, "T004"),
    ("exact-container-overlay", False, "T004"),
    ("exact-sif-cuda-preflight", True, "T004"),
    ("candidate-audit", False, "T004"),
    ("remote-small-single", True, "T008"),
    ("remote-small-repeated", True, "T011"),
    ("remote-large-single", True, "T015"),
    ("clean-reproduction", True, "T016"),
)


class CampaignError(ValueError):
    pass


def canonical(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_object(value: Any) -> str:
    return sha256_bytes(canonical(value))


def load_json(path: Path) -> Dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as error:
        raise CampaignError(f"JSON_INVALID:{path}:{error}") from error
    if not isinstance(value, dict):
        raise CampaignError(f"JSON_OBJECT_REQUIRED:{path}")
    return value


def normalize_digest(value: str) -> str:
    text = str(value)
    return text if text.startswith("sha256:") else "sha256:" + text


def validate_bindings(value: Dict[str, Any]) -> Dict[str, str]:
    if set(value) != set(REQUIRED_BINDINGS):
        missing = sorted(set(REQUIRED_BINDINGS) - set(value))
        extra = sorted(set(value) - set(REQUIRED_BINDINGS))
        raise CampaignError(
            f"BINDING_FIELDS_INVALID:missing={missing}:extra={extra}"
        )
    bindings = {key: str(value[key]) for key in REQUIRED_BINDINGS}
    for key, item in bindings.items():
        if DIGEST_RE.fullmatch(item) is None:
            raise CampaignError(f"BINDING_DIGEST_INVALID:{key}")
    return bindings


def validate_baseline(
    baseline_path: Path, baseline: Dict[str, Any], bindings: Dict[str, str]
) -> None:
    if baseline.get("schema") != "ndnsf-di.spec168-baseline.v1":
        raise CampaignError("BASELINE_SCHEMA_INVALID")
    if sha256_bytes(baseline_path.read_bytes()) != bindings["baselineDigest"]:
        raise CampaignError("BASELINE_DIGEST_MISMATCH")
    runtime = normalize_digest(baseline.get("runtime", {}).get("sha256", ""))
    if runtime != bindings["runtimeSifDigest"]:
        raise CampaignError("BASELINE_RUNTIME_MISMATCH")
    small = normalize_digest(
        baseline.get("smallModelControl", {}).get("stageManifestSha256", "")
    )
    if small != bindings["remoteSmallStageManifestDigest"]:
        raise CampaignError("BASELINE_SMALL_STAGE_MISMATCH")
    large = normalize_digest(
        baseline.get("largeModelNegativeControl", {}).get(
            "stageManifestSha256", ""
        )
    )
    if large != bindings["remoteLargeStageManifestDigest"]:
        raise CampaignError("BASELINE_LARGE_STAGE_MISMATCH")


def asset_references(
    baseline: Dict[str, Any], bindings: Dict[str, str],
) -> Dict[str, str]:
    return {
        "runtimeSif": str(baseline["runtime"]["path"]),
        "localFixtureStageManifest": (
            "content-addressed:" + bindings["localFixtureManifestDigest"]
        ),
        "remoteSmallStageManifest": str(
            baseline["smallModelControl"]["stageManifestPath"]
        ),
        "remoteLargeStageManifest": str(
            baseline["largeModelNegativeControl"]["stageManifestPath"]
        ),
        "sourceBundle": "content-addressed:" + bindings["sourceBundleDigest"],
        "localGateManifest": (
            "content-addressed:" + bindings["localGateManifestDigest"]
        ),
        "exactSifPreflight": (
            "content-addressed:" + bindings["exactSifPreflightDigest"]
        ),
    }


def build_documents(
    baseline_path: Path, bindings_path: Path
) -> Dict[str, Dict[str, Any]]:
    baseline = load_json(baseline_path)
    bindings = validate_bindings(load_json(bindings_path))
    validate_baseline(baseline_path, baseline, bindings)

    candidate_payload = {
        "schema": EXPERIMENT_SCHEMA,
        "bindingDigests": {key: bindings[key] for key in CANDIDATE_BINDINGS},
        "assetReferences": asset_references(baseline, bindings),
        "payloadPolicy": "content-addressed-references-only",
    }
    candidate_digest = digest_object(candidate_payload)
    candidate_id = "spec168-candidate-v3-" + candidate_digest[-20:]
    experiment = dict(candidate_payload)
    experiment.update({
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "state": "FROZEN",
    })

    phases: List[Dict[str, Any]] = []
    previous = None
    for phase, remote, owner in PHASES:
        phases.append({
            "phase": phase,
            "ownerTask": owner,
            "remote": remote,
            "requires": [] if previous is None else [previous],
            "onFailure": "CLOSE_IDENTITY_AND_RETURN_TO_LOCAL_REPAIR",
        })
        previous = phase
    schedule_payload = {
        "schema": SCHEDULE_SCHEMA,
        "candidateId": candidate_id,
        "scheduleInputDigest": bindings["scheduleDigest"],
        "promptSetDigest": bindings["promptSetDigest"],
        "routeDigest": bindings["routeDigest"],
        "analyzerDigest": bindings["analyzerDigest"],
        "phases": phases,
        "manualRemoteSubmissionOnly": True,
        "automaticRetry": False,
        "replacementPolicy": "new-linked-source-and-campaign-identity",
    }
    schedule_digest = digest_object(schedule_payload)
    schedule = dict(schedule_payload)
    schedule["scheduleManifestDigest"] = schedule_digest

    campaign_payload = {
        "schema": SCHEMA,
        "state": "FROZEN",
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "scheduleManifestDigest": schedule_digest,
        "bindingDigests": bindings,
        "assetReferences": asset_references(baseline, bindings),
        "experimentManifest": "experiment-manifest.json",
        "scheduleManifest": "schedule-manifest.json",
        "submissionPolicy": "at-most-once-no-auto-resubmit",
        "automaticRemoteSubmission": False,
    }
    campaign_digest = digest_object(campaign_payload)
    campaign_id = "spec168-campaign-v3-" + campaign_digest[-20:]
    campaign = dict(campaign_payload)
    campaign.update({
        "campaignId": campaign_id,
        "campaignDigest": campaign_digest,
    })
    return {"experiment": experiment, "schedule": schedule, "campaign": campaign}


def write_json(path: Path, value: Dict[str, Any]) -> None:
    payload = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    with path.open("x", encoding="utf-8") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())


def freeze(baseline: Path, bindings: Path, output_root: Path) -> str:
    documents = build_documents(baseline, bindings)
    campaign_id = str(documents["campaign"]["campaignId"])
    output_root.mkdir(parents=True, exist_ok=True)
    target = output_root / campaign_id
    if target.exists():
        raise CampaignError(f"campaign already exists:{target}")
    temporary = Path(tempfile.mkdtemp(prefix=f".{campaign_id}.", dir=str(output_root)))
    try:
        write_json(temporary / "experiment-manifest.json", documents["experiment"])
        write_json(temporary / "schedule-manifest.json", documents["schedule"])
        write_json(temporary / "campaign-manifest.json", documents["campaign"])
        try:
            temporary.rename(target)
        except FileExistsError as error:
            raise CampaignError(f"campaign already exists:{target}") from error
    except Exception:
        if temporary.exists():
            shutil.rmtree(temporary)
        raise
    return campaign_id


def campaign_payload(document: Dict[str, Any]) -> Dict[str, Any]:
    value = dict(document)
    value.pop("campaignId", None)
    value.pop("campaignDigest", None)
    return value


def validate_campaign(path: Path) -> Dict[str, Any]:
    document = load_json(path)
    expected_digest = digest_object(campaign_payload(document))
    expected_id = "spec168-campaign-v3-" + expected_digest[-20:]
    if (
        document.get("schema") != SCHEMA
        or document.get("state") != "FROZEN"
        or document.get("campaignDigest") != expected_digest
        or document.get("campaignId") != expected_id
    ):
        raise CampaignError("FROZEN_CAMPAIGN_MUTATED")
    validate_bindings(document.get("bindingDigests", {}))
    parent = path.parent
    experiment_path = parent / str(document.get("experimentManifest", ""))
    schedule_path = parent / str(document.get("scheduleManifest", ""))
    experiment = load_json(experiment_path)
    schedule = load_json(schedule_path)
    if digest_object({key: experiment[key] for key in (
            "schema", "bindingDigests", "assetReferences", "payloadPolicy")}) \
            != document["candidateDigest"]:
        raise CampaignError("EXPERIMENT_MANIFEST_MUTATED")
    schedule_without_digest = dict(schedule)
    schedule_without_digest.pop("scheduleManifestDigest", None)
    if digest_object(schedule_without_digest) != document["scheduleManifestDigest"]:
        raise CampaignError("SCHEDULE_MANIFEST_MUTATED")
    return document


def claim(manifest: Path, result_root: Path) -> Path:
    campaign = validate_campaign(manifest)
    result_root.mkdir(parents=True, exist_ok=True)
    target = result_root / str(campaign["campaignId"])
    if target.exists():
        raise CampaignError(f"campaign already claimed:{target}")
    temporary = Path(tempfile.mkdtemp(
        prefix=f".{campaign['campaignId']}.claim.", dir=str(result_root)
    ))
    try:
        write_json(temporary / "run-claim.json", {
            "schema": "ndnsf-di.spec168-run-claim.v3",
            "campaignId": campaign["campaignId"],
            "campaignDigest": campaign["campaignDigest"],
            "claimedAtUtc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "state": "CLAIMED",
            "payloadBytesCopied": 0,
            "automaticSubmission": False,
        })
        try:
            temporary.rename(target)
        except FileExistsError as error:
            raise CampaignError(f"campaign already claimed:{target}") from error
    except Exception:
        if temporary.exists():
            shutil.rmtree(temporary)
        raise
    return target


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    for command in ("derive", "freeze"):
        item = commands.add_parser(command)
        item.add_argument("--baseline", type=Path, required=True)
        item.add_argument("--bindings", type=Path, required=True)
        if command == "freeze":
            item.add_argument("--output-root", type=Path, required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("--manifest", type=Path, required=True)
    claim_parser = commands.add_parser("claim")
    claim_parser.add_argument("--manifest", type=Path, required=True)
    claim_parser.add_argument("--result-root", type=Path, required=True)
    return root


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "derive":
            print(build_documents(args.baseline, args.bindings)["campaign"]["campaignId"])
        elif args.command == "freeze":
            print(freeze(args.baseline, args.bindings, args.output_root))
        elif args.command == "validate":
            print(validate_campaign(args.manifest)["campaignId"])
        elif args.command == "claim":
            print(claim(args.manifest, args.result_root))
        else:
            raise CampaignError("COMMAND_INVALID")
    except (CampaignError, FileExistsError, KeyError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
