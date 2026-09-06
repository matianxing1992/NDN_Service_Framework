#!/usr/bin/env python3
"""Create the current one-case Spec175 host-gate manifest.

This is deliberately not a matrix runner.  It validates one completed M01
host/CPU production-path directory and emits the v2 manifest consumed by the
exact-SIF builder and replay driver.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
from typing import Any


SCHEMA = "spec175-host-minindn-manifest-v2"
CASE = "M01"
WORKLOAD_SEED = 1750001
TOPOLOGY = {
    "accessLinkMbit": 100,
    "oneWayDelayMs": 10,
    "queuePackets": 1000,
    "providerCount": 4,
    "admissionControl": False,
    "targetedPrefetch": False,
}


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def json_digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def relative_path(root: Path, path: Path, label: str) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as exc:
        raise SystemExit(f"{label} must be inside repository root: {path}") from exc


def load_result(run_dir: Path) -> tuple[dict[str, Any], Path]:
    result_path = run_dir / "spec175-case-result.json"
    if not result_path.is_file():
        raise SystemExit(f"M01_RESULT_MISSING:{result_path}")
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"M01_RESULT_INVALID:{type(exc).__name__}") from exc
    if not isinstance(result, dict):
        raise SystemExit("M01_RESULT_NOT_OBJECT")
    expected = {
        "schema": "ndnsf-di-spec175-minindn-case-result-v1",
        "status": "PASS",
        "case": CASE,
        "seed": WORKLOAD_SEED,
        "campaignId": f"spec175-{CASE}-{WORKLOAD_SEED}",
        "runtime": "tiny-onnx",
        "providerCount": 4,
        "admissionControl": False,
        "userReturnCode": 0,
        "expectedTerminal": False,
    }
    for key, value in expected.items():
        if result.get(key) != value:
            raise SystemExit(f"M01_RESULT_MISMATCH:{key}")
    terminal = result.get("terminalEvidence")
    if not isinstance(terminal, dict):
        raise SystemExit("M01_TERMINAL_EVIDENCE_MISSING")
    for key, value in {
        "schema": "ndnsf-di-spec175-terminal-evidence-v1",
        "status": "PASS",
        "resultWrittenAfterProcessExit": True,
        "abortObserved": False,
        "unexpectedSignalExits": {},
        "survivingOwnedProcesses": [],
    }.items():
        if terminal.get(key) != value:
            raise SystemExit(f"M01_TERMINAL_EVIDENCE_MISMATCH:{key}")
    exits = terminal.get("childExitCodes")
    if not isinstance(exits, dict) or not exits or any(
            not isinstance(item, int) for item in exits.values()):
        raise SystemExit("M01_CHILD_EXIT_CODES_INCOMPLETE")
    fanout = result.get("svsGroupFanout")
    if not isinstance(fanout, dict) or fanout.get("status") != "VERIFIED":
        raise SystemExit("M01_SVS_FANOUT_NOT_VERIFIED")
    route = result.get("nfdRouteSnapshot")
    if not isinstance(route, dict) or route.get("status") != "PASS":
        raise SystemExit("M01_NFD_ROUTE_NOT_PASS")
    for index in range(4):
        if not (run_dir / f"stage{index}-provider.log").is_file():
            raise SystemExit(f"M01_PROVIDER_LOG_MISSING:stage{index}")
    return result, result_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", default=".", type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--source-seal", required=True, type=Path)
    parser.add_argument("--fixture-manifest", required=True, type=Path)
    parser.add_argument("--topology", default="Experiments/Topology/spec175-host-gate.conf",
                        type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    root = args.project_root.expanduser().resolve()
    run_dir = args.run_dir.expanduser().resolve()
    source_seal = args.source_seal.expanduser().resolve()
    fixture = args.fixture_manifest.expanduser().resolve()
    topology = args.topology.expanduser().resolve()
    output = args.output.expanduser().resolve()
    for path, label in ((source_seal, "source seal"),
                        (fixture, "fixture manifest"),
                        (topology, "topology")):
        if not path.is_file():
            raise SystemExit(f"{label} missing: {path}")
    result, result_path = load_result(run_dir)
    topology_digest = sha256(topology)
    recorded_topology = result.get("topology", {})
    if (not isinstance(recorded_topology, dict) or
            recorded_topology.get("sha256") != topology_digest):
        raise SystemExit("M01_TOPOLOGY_DIGEST_MISMATCH")

    entry = {
        "case": CASE,
        "fault": "healthy",
        "repetition": 1,
        "status": "PASS",
        "directory": relative_path(root, run_dir, "run directory"),
        "caseResultPath": relative_path(root, result_path, "case result"),
        "caseResultSha256": sha256(result_path),
        "requestId": result["requestId"],
        "campaignId": result["campaignId"],
        "workloadSeed": WORKLOAD_SEED,
        "providerRoleIndices": result.get("providerRoleIndices", []),
        "expectedTerminal": False,
    }
    subject = {
        "runtime": "tiny-onnx",
        **TOPOLOGY,
        "topologyPath": relative_path(root, topology, "topology"),
        "topologySha256": topology_digest,
        "sourceSealPath": relative_path(root, source_seal, "source seal"),
        "sourceSealSha256": sha256(source_seal),
        "fixtureManifestPath": relative_path(root, fixture, "fixture manifest"),
        "fixtureManifestSha256": sha256(fixture),
        "workloadSeed": WORKLOAD_SEED,
    }
    payload: dict[str, Any] = {
        "schema": SCHEMA,
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "status": "PASS",
        "subject": subject,
        "matrix": {
            "cases": [CASE],
            "repetitionsPerCase": 1,
            "total": 1,
            "passed": 1,
            "entries": [entry],
        },
        "lineageAndBounds": {
            "registeredRoute": "one M01 host/CPU production-path smoke",
            "manualRepeats": False,
            "legacyMatrix": "historical-only",
        },
        "manifestDigest": None,
    }
    payload["manifestDigest"] = json_digest(payload)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(json.dumps({"status": "PASS", "schema": SCHEMA,
                      "entries": 1, "output": str(output)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
