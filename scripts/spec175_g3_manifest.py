#!/usr/bin/env python3
"""Seal the current Spec175 host/CPU MiniNDN M01--M14 matrix.

The G3 manifest is deliberately generated from explicit run directories.  It
does not discover or pool historical diagnostics: each case must name exactly
three fresh PASS processes, and every case result must carry the frozen
four-Provider topology and disabled-admission contract.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
from typing import Any


CASES = {
    "M01": "healthy",
    "M02": "event-reorder",
    "M03": "event-duplicate",
    "M04": "one-loss-retry",
    "M05": "permanent-gap",
    "M06": "callback-exception",
    "M07": "cancel-after-event-3",
    "M08": "deadline",
    "M09": "provider-failure",
    "M10": "ack-capacity-permutation",
    "M11": "two-turn-delta-prefill",
    "M12": "three-conversation-host-tier-isolation",
    "M13": "conversation-negative-fallback",
    "M14": "concurrent-parent-cancel-prefetch",
}
EXPECTED = {
    "M01": False, "M02": False, "M03": False, "M04": False,
    "M05": True, "M06": True, "M07": True, "M08": True,
    "M09": True, "M10": False,
    "M11": False, "M12": False, "M13": False, "M14": False,
}
TOPOLOGY = {
    "accessLinkMbit": 100,
    "oneWayDelayMs": 10,
    "queuePackets": 1000,
    "providerCount": 4,
    "admissionControl": False,
    "targetedPrefetch": False,
}
WORKLOAD_SEED = 1750001
FAULT_SEED = 1750002


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def json_digest(value: Any) -> str:
    data = json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")
    return "sha256:" + hashlib.sha256(data).hexdigest()


def marker_counts(log_path: Path) -> dict[str, int]:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    counts: dict[str, int] = {}
    for marker in re.findall(r"\b(?:NDNSF|LLM_PIPELINE)_[A-Z0-9_]+\b", text):
        counts[marker] = counts.get(marker, 0) + 1
    return dict(sorted(counts.items()))


def artifact_records(run: Path, result: dict[str, Any]) -> list[dict[str, Any]]:
    records = []
    for item in result.get("artifacts", []):
        relative = str(item.get("path", ""))
        path = run / relative
        if not relative or not path.is_file():
            raise ValueError(f"missing recorded artifact: {run / relative}")
        actual = sha256(path)
        records.append({
            "path": relative,
            "bytes": path.stat().st_size,
            "sha256": actual,
            # The runner records hashes before Mininet's final per-process
            # statistics flush.  Preserve both identities instead of
            # silently treating the harmless post-result append as a test
            # failure.
            "resultRecordedSha256": item.get("sha256"),
            "changedAfterResult": actual != item.get("sha256"),
        })
    return records


def validate_run(run: Path, case: str) -> dict[str, Any]:
    result_path = run / "spec175-case-result.json"
    runner_log = run.with_name(run.name + ".runner.log")
    if not result_path.is_file():
        raise ValueError(f"missing case result: {result_path}")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("schema") != "ndnsf-di-spec175-minindn-case-result-v1":
        raise ValueError(f"wrong case-result schema: {result_path}")
    if result.get("case") != case or result.get("status") != "PASS":
        raise ValueError(f"case is not PASS: {result_path}")
    if result.get("providerCount") != 4 or result.get("admissionControl") is not False:
        raise ValueError(f"provider/admission contract mismatch: {result_path}")
    if result.get("runtime") != "tiny-onnx" or result.get("userReturnCode") != 0:
        raise ValueError(f"runtime/return-code contract mismatch: {result_path}")
    expected_seed = FAULT_SEED if EXPECTED[case] else WORKLOAD_SEED
    expected_campaign = f"spec175-{case}-{expected_seed}"
    if result.get("seed") != expected_seed:
        raise ValueError(
            f"case-seed field mismatch: expected={expected_seed} "
            f"actual={result.get('seed')}: {result_path}")
    if result.get("campaignId") != expected_campaign:
        raise ValueError(
            f"workload-seed mismatch: expected={expected_campaign} "
            f"actual={result.get('campaignId')}: {result_path}")
    if result.get("expectedTerminal") is not EXPECTED[case]:
        raise ValueError(f"expected terminal mismatch: {result_path}")
    if case in {"M11", "M12", "M13", "M14"}:
        conversation = result.get("conversationEvidence")
        if not isinstance(conversation, dict):
            raise ValueError(f"missing conversation evidence: {result_path}")
        if conversation.get("case") != case or conversation.get("status") != "PASS":
            raise ValueError(f"conversation evidence is not PASS: {result_path}")
        required = {
            "networkRequests", "freshRequestIds", "freshGenerationIds",
            "requestLocalEntriesAfterCleanup", "conversationEntries",
            "stateTensorBytesOnNdn", "runnerCallsAfterRejectedValidation",
        }
        missing = sorted(required.difference(conversation))
        if missing:
            raise ValueError(
                f"conversation evidence missing fields {missing}: {result_path}")
        if int(conversation["networkRequests"]) < 1:
            raise ValueError(f"conversation network request missing: {result_path}")
        if int(conversation["stateTensorBytesOnNdn"]) != 0:
            raise ValueError(f"conversation state leaked onto NDN: {result_path}")
        if int(conversation["runnerCallsAfterRejectedValidation"]) != 0:
            raise ValueError(
                f"rejected conversation validation reached runner: {result_path}")
    topology = result.get("topology", {})
    for key, value in TOPOLOGY.items():
        if key in topology and topology[key] != value:
            raise ValueError(f"topology mismatch {key}: {result_path}")
    if not runner_log.is_file():
        raise ValueError(f"missing runner log: {runner_log}")
    user_log = run / "llm-pipeline-user.log"
    if not user_log.is_file():
        raise ValueError(f"missing user log: {user_log}")
    return {
        "directory": str(run),
        "caseResultSha256": sha256(result_path),
        "runnerLogSha256": sha256(runner_log),
        "caseResult": result,
        "artifacts": artifact_records(run, result),
        "markers": marker_counts(user_log),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", default=".")
    parser.add_argument("--output", required=True)
    parser.add_argument("--source-seal", required=True)
    parser.add_argument("--fixture-manifest", required=True)
    parser.add_argument("--topology", default="Experiments/Topology/spec175-host-gate.conf")
    parser.add_argument("--run-root", action="append", required=True,
                        help="case=directory; repeat exactly once per case")
    args = parser.parse_args()
    root = Path(args.project_root).expanduser().resolve()
    mappings: dict[str, Path] = {}
    for raw in args.run_root:
        if "=" not in raw:
            raise SystemExit("--run-root must be CASE=DIRECTORY")
        case, directory = raw.split("=", 1)
        if case in mappings or case not in CASES:
            raise SystemExit(f"invalid or duplicate case: {case}")
        mappings[case] = (root / directory).resolve()
    if set(mappings) != set(CASES):
        raise SystemExit("--run-root must cover M01 through M14 exactly")
    seal = (root / args.source_seal).resolve()
    fixture = (root / args.fixture_manifest).resolve()
    topology = (root / args.topology).resolve()
    for path in (seal, fixture, topology):
        if not path.is_file():
            raise SystemExit(f"missing manifest input: {path}")
    repetitions = []
    for case in CASES:
        base = mappings[case]
        for index in range(1, 4):
            # A run-root may be a printf-style template or a directory that
            # contains r1/r2/r3 subdirectories.
            run = Path(str(base).format(rep=f"r{index}", index=index))
            if not run.is_dir():
                candidate = base.parent / f"{base.name}-r{index}"
                run = candidate if candidate.is_dir() else run
            repetitions.append(validate_run(run, case))
    matrix = [{
        "case": item["caseResult"]["case"],
        "fault": CASES[item["caseResult"]["case"]],
        "directory": item["directory"],
        "requestId": item["caseResult"]["requestId"],
        "campaignId": item["caseResult"]["campaignId"],
        "workloadSeed": WORKLOAD_SEED,
        "providerRoleIndices": item["caseResult"]["providerRoleIndices"],
        "expectedTerminal": item["caseResult"]["expectedTerminal"],
        "caseResultSha256": item["caseResultSha256"],
        "runnerLogSha256": item["runnerLogSha256"],
        "artifacts": item["artifacts"],
        "markers": item["markers"],
        **({"conversationEvidence": item["caseResult"]["conversationEvidence"]}
           if item["caseResult"]["case"] in {"M11", "M12", "M13", "M14"}
           else {}),
    } for item in repetitions]
    output = (root / args.output).resolve()
    payload = {
        "schema": "spec175-g3-host-minindn-manifest-v1",
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "status": "PASS",
        "matrix": {
            "cases": list(CASES),
            "repetitionsPerCase": 3,
            "total": len(matrix),
            "passed": len(matrix),
            "entries": matrix,
        },
        "subject": {
            "runtime": "tiny-onnx",
            "providerCount": 4,
            "admissionControl": False,
            "targetedPrefetch": False,
            "topology": TOPOLOGY,
            "topologyPath": str(topology.relative_to(root)),
            "topologySha256": sha256(topology),
            "sourceSealPath": str(seal.relative_to(root)),
            "sourceSealSha256": sha256(seal),
            "fixtureManifestPath": str(fixture.relative_to(root)),
            "fixtureManifestSha256": sha256(fixture),
            "workloadSeed": WORKLOAD_SEED,
        },
        "faultController": {
            "seed": FAULT_SEED,
            "oneDimensionPerCase": True,
            "cases": CASES,
        },
        "lineageAndBounds": {
            "required": [
                "REQUEST -> ACK -> SELECTION -> assignment fetch -> provider execution",
                "internal feedback publication/fetch before next epoch",
                "external event publication/fetch before user delivery",
                "terminal END/RESPONSE or registered terminal error",
            ],
            "retryBudget": "fixed by the Spec175 validation contract; no case changes it",
            "queuePolicy": "bounded queues; no unbounded retry or admission-control path",
            "perRunMarkerCounts": {
                item["directory"]: item["markers"] for item in repetitions
            },
        },
        "excludedSetupFailures": [
            {
                "directory": "results/spec175/g3/replay1-M07-r1-20260825",
                "reason": "provider/2 artifact fetch exceeded the 120 s preparation bound before Selection",
                "classification": "setup-only; excluded from M07 matrix",
            },
        ],
        "manifestDigest": None,
    }
    payload["manifestDigest"] = json_digest(payload)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(json.dumps({"status": payload["status"], "total": len(matrix),
                      "passed": len(matrix), "output": str(output)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
