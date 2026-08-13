#!/usr/bin/env python3
"""Registration-driven matched MiniNDN campaign for Spec 173.

The dry-run interface is intentionally usable without root or MiniNDN. Actual
execution preserves immutable attempt directories and validates every retained
cell against the frozen registration and toolchain hashes.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
from typing import Any, Optional

import yaml


REPO = Path(__file__).resolve().parents[1]
DEFAULT_REGISTRATION = (
    REPO / "specs/173-paper-submission-evidence/contracts/experiment-registration.yaml"
)
DEFAULT_TOOLCHAIN = (
    REPO / "specs/173-paper-submission-evidence/evidence/toolchain-manifest.json"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def next_attempt_directory(block_root: Path) -> Path:
    """Return the next immutable attempt path without creating it."""
    numbers = []
    if block_root.exists():
        for path in block_root.glob("attempt-[0-9][0-9][0-9][0-9]"):
            try:
                numbers.append(int(path.name.split("-", 1)[1]))
            except ValueError:
                continue
    return block_root / f"attempt-{max(numbers, default=0) + 1:04d}"


def cell_manifest(
    cell: dict[str, Any], registration_sha256: str, toolchain_sha256: str,
    toolchain: dict[str, Any],
) -> dict[str, Any]:
    """Build the immutable identity checked before a cell may be reused."""
    return {
        "schemaVersion": 1,
        "cellId": cell["cellId"],
        "comparison": cell["comparison"],
        "repetition": cell["repetition"],
        "seed": cell["seed"],
        "rateRps": cell["rateRps"],
        "system": cell["system"],
        "outputDirectory": cell["outputDirectory"],
        "exactCommand": cell["command"],
        "registrationSha256": registration_sha256,
        "toolchainManifestSha256": toolchain_sha256,
        "campaignRunnerSha256": sha256_file(Path(__file__).resolve()),
        "sourceRevisions": toolchain.get("source", {}),
        "runtimeArtifacts": toolchain.get("artifacts", {}),
    }


def verify_cell_attempt(
    cell_dir: Path, expected_cell: dict[str, Any], registration_sha256: str,
    toolchain_sha256: str,
) -> tuple[bool, str]:
    """Accept only a terminal valid cell with exact identity and retained hashes."""
    manifest_path = cell_dir / "cell-manifest.json"
    result_path = cell_dir / "cell-result.json"
    if not manifest_path.is_file() or not result_path.is_file():
        return False, "cell manifest or result is missing"
    try:
        manifest = json.loads(manifest_path.read_text())
        result = json.loads(result_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return False, f"cell metadata is not parseable: {error}"

    expected_identity = {
        "cellId": expected_cell["cellId"],
        "comparison": expected_cell["comparison"],
        "repetition": expected_cell["repetition"],
        "seed": expected_cell["seed"],
        "rateRps": expected_cell["rateRps"],
        "system": expected_cell["system"],
        "outputDirectory": expected_cell["outputDirectory"],
        "exactCommand": expected_cell["command"],
        "registrationSha256": registration_sha256,
        "toolchainManifestSha256": toolchain_sha256,
        "campaignRunnerSha256": sha256_file(Path(__file__).resolve()),
    }
    for key, expected in expected_identity.items():
        if manifest.get(key) != expected:
            return False, f"cell manifest {key} mismatch"
    if result.get("status") != "valid" or result.get("exitCode") != 0:
        return False, "cell result is not terminal valid"

    required = result.get("requiredSummaries")
    hashes = result.get("artifactHashes")
    if not isinstance(required, list) or not required:
        return False, "required summaries are missing"
    if not isinstance(hashes, dict):
        return False, "artifact hashes are missing"
    for relative in required:
        artifact = cell_dir / relative
        if not artifact.is_file():
            return False, f"required summary is missing: {relative}"
        try:
            json.loads(artifact.read_text())
        except (OSError, json.JSONDecodeError) as error:
            return False, f"required summary is not parseable: {relative}: {error}"
        expected_hash = hashes.get(relative)
        if expected_hash != sha256_file(artifact):
            return False, f"artifact hash mismatch: {relative}"
    for relative, expected_hash in hashes.items():
        artifact = cell_dir / relative
        if not artifact.is_file() or sha256_file(artifact) != expected_hash:
            return False, f"artifact hash mismatch: {relative}"
    return True, "verified"


def write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    with path.open("x") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def run_cell(
    cell: dict[str, Any], registration_sha256: str, toolchain_sha256: str,
    toolchain: dict[str, Any], command_runner: Any = subprocess.run,
) -> tuple[bool, str]:
    """Execute one cell exactly once and seal its terminal result."""
    cell_dir = Path(cell["outputDirectory"])
    cell_dir.mkdir(parents=True, exist_ok=False)
    write_json_exclusive(
        cell_dir / "cell-manifest.json",
        cell_manifest(cell, registration_sha256, toolchain_sha256, toolchain),
    )
    log_path = cell_dir / "stdout-stderr.log"
    exit_code = -1
    execution_error = ""
    try:
        with log_path.open("x") as log:
            completed = command_runner(
                cell["command"],
                cwd=REPO,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            exit_code = int(completed.returncode)
    except Exception as error:  # preserve a terminal artifact for infrastructure failures
        execution_error = f"{type(error).__name__}: {error}"

    summary_path = cell_dir / "summary.json"
    if cell["system"] == "regression" and exit_code == 0 and not summary_path.exists():
        summary_path.write_text(json.dumps({"regressionPass": True}, sort_keys=True) + "\n")

    summary_valid = False
    if summary_path.is_file():
        try:
            json.loads(summary_path.read_text())
            summary_valid = True
        except (OSError, json.JSONDecodeError):
            summary_valid = False

    hashes = {}
    for artifact in sorted(path for path in cell_dir.rglob("*") if path.is_file()):
        if artifact.name == "cell-result.json":
            continue
        relative = str(artifact.relative_to(cell_dir))
        hashes[relative] = sha256_file(artifact)
    valid = exit_code == 0 and summary_valid
    result = {
        "schemaVersion": 1,
        "status": "valid" if valid else "infrastructure-invalid",
        "exitCode": exit_code,
        "requiredSummaries": ["summary.json"],
        "artifactHashes": hashes,
        "executionError": execution_error,
        "reason": "verified terminal cell" if valid else (
            execution_error or f"exit={exit_code}; parseable summary={summary_valid}"
        ),
    }
    write_json_exclusive(cell_dir / "cell-result.json", result)
    return verify_cell_attempt(
        cell_dir, cell, registration_sha256, toolchain_sha256
    ) if valid else (False, result["reason"])


def execute_block(
    block_id: str, cells: list[dict[str, Any]], attempt_dir: Path,
    registration_sha256: str, toolchain_sha256: str, toolchain: dict[str, Any],
    command_runner: Any = subprocess.run,
) -> tuple[bool, str]:
    """Run a matched block once; any failed member invalidates the whole attempt."""
    attempt_dir.mkdir(parents=True, exist_ok=False)
    completed_cells: list[str] = []
    for cell in cells:
        valid, reason = run_cell(
            cell,
            registration_sha256,
            toolchain_sha256,
            toolchain,
            command_runner=command_runner,
        )
        completed_cells.append(cell["cellId"])
        if not valid:
            block_result = {
                "schemaVersion": 1,
                "blockId": block_id,
                "status": "infrastructure-invalid",
                "failedCell": cell["cellId"],
                "completedCells": completed_cells,
                "reason": reason,
                "rerunScope": "whole-matched-block",
                "rerunInstruction": (
                    "Correct the infrastructure defect, then rerun the campaign with "
                    "--resume; a new immutable attempt directory will contain every "
                    "member of this matched block."
                ),
            }
            write_json_exclusive(attempt_dir / "block-result.json", block_result)
            return False, reason
    write_json_exclusive(attempt_dir / "block-result.json", {
        "schemaVersion": 1,
        "blockId": block_id,
        "status": "valid",
        "completedCells": completed_cells,
        "rerunScope": "none",
        "rerunInstruction": "none",
    })
    return True, "verified matched block"


def verify_block_attempt(
    block_id: str, cells: list[dict[str, Any]], attempt_dir: Path,
    registration_sha256: str, toolchain_sha256: str,
) -> tuple[bool, str]:
    result_path = attempt_dir / "block-result.json"
    if not result_path.is_file():
        return False, "block result is missing"
    try:
        result = json.loads(result_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return False, f"block result is not parseable: {error}"
    if result.get("blockId") != block_id or result.get("status") != "valid":
        return False, "block result is not terminal valid"
    for cell in cells:
        valid, reason = verify_cell_attempt(
            Path(cell["outputDirectory"]),
            cell,
            registration_sha256,
            toolchain_sha256,
        )
        if not valid:
            return False, f"{cell['system']}: {reason}"
    return True, "verified matched block"


def preflight_report(
    registration_path: Path, toolchain_path: Path, require_execution: bool = False,
) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []

    def add(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "pass": bool(passed), "detail": detail})

    try:
        registration = load_registration(registration_path)
        add(
            "frozen-registration",
            registration.get("status") == "refrozen-after-preimplementation-audit",
            str(registration.get("status")),
        )
    except Exception as error:
        registration = {}
        add("frozen-registration", False, str(error))

    try:
        toolchain = json.loads(toolchain_path.read_text())
        add(
            "terminal-toolchain",
            toolchain.get("terminalStatus") == "pass",
            str(toolchain.get("terminalStatus")),
        )
    except Exception as error:
        toolchain = {}
        add("terminal-toolchain", False, str(error))

    artifacts = toolchain.get("artifacts", {})
    mismatches = []
    for name, metadata in artifacts.items():
        path = REPO / metadata.get("path", "")
        expected = metadata.get("sha256")
        if not path.is_file() or expected != sha256_file(path):
            mismatches.append(name)
    add(
        "runtime-artifact-hashes",
        bool(artifacts) and not mismatches,
        "verified" if artifacts and not mismatches else f"mismatch={mismatches}",
    )

    registered_paths = []
    if registration:
        registered_paths.append(REPO / registration["commonConditions"]["topologyFile"])
        for comparison in registration["comparisons"]:
            if comparison.get("command"):
                registered_paths.append(REPO / comparison["command"])
        registered_paths.extend([
            REPO / "Experiments/NDNSF_NewAPI_Minindn_Perf.py",
            REPO / "Experiments/gRPC_memphis_ucla_latency.py",
            REPO / "Experiments/NSC_memphis_ucla_latency.py",
        ])
    missing_paths = [str(path.relative_to(REPO)) for path in registered_paths if not path.is_file()]
    add("registered-inputs", bool(registered_paths) and not missing_paths,
        "verified" if registered_paths and not missing_paths else f"missing={missing_paths}")

    mini_ndn_available = importlib.util.find_spec("minindn") is not None
    commands = {name: shutil.which(name) for name in ("mnexec", "nfd", "nlsr")}
    add(
        "minindn-runtime",
        mini_ndn_available and all(commands.values()),
        json.dumps({"pythonModule": mini_ndn_available, "commands": commands}, sort_keys=True),
    )
    if require_execution:
        add("root-execution", os.geteuid() == 0, f"euid={os.geteuid()}")

    return {
        "schemaVersion": 1,
        "status": "pass" if all(item["pass"] for item in checks) else "fail",
        "miniNdnStarted": False,
        "registrationSha256": sha256_file(registration_path),
        "toolchainManifestSha256": sha256_file(toolchain_path),
        "checks": checks,
    }


def load_registration(path: Path) -> dict[str, Any]:
    value = yaml.safe_load(path.read_text())
    if not isinstance(value, dict) or value.get("schemaVersion") != 1:
        raise ValueError("unsupported Spec 173 experiment registration")
    return value


def comparison_by_id(registration: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["id"]: item for item in registration["comparisons"]}


def deterministic_system_order(
    systems: list[str], comparison_id: str, repetition: dict[str, Any],
    rate: float | int | None,
) -> list[str]:
    ordered = list(systems)
    if len(ordered) < 2:
        return ordered
    base_material = f"{comparison_id}|{rate}".encode()
    base = int.from_bytes(hashlib.sha256(base_material).digest()[:8], "big")
    offset = (base + int(repetition["seed"])) % len(ordered)
    return ordered[offset:] + ordered[:offset]


def make_cell(
    registration: dict[str, Any], comparison: dict[str, Any], repetition: dict[str, Any],
    rate: float | int | None, system: str, output_dir: Path,
) -> dict[str, Any]:
    common = registration["commonConditions"]
    comparison_id = comparison["id"]
    rate_token = "none" if rate is None else str(rate).replace(".", "p")
    cell_id = f"{comparison_id}--{repetition['id']}--rps-{rate_token}--{system}"
    cell = {
        "cellId": cell_id,
        "comparison": comparison_id,
        "repetition": repetition["id"],
        "seed": int(repetition["seed"]),
        "rateRps": rate,
        "system": system,
        "outputDirectory": str(output_dir),
    }
    cell["command"] = build_cell_command(registration, comparison, cell)
    return cell


def build_cell_command(
    registration: dict[str, Any], comparison: dict[str, Any], cell: dict[str, Any]
) -> list[str]:
    common = registration["commonConditions"]
    system = cell["system"]
    output = cell["outputDirectory"]
    if comparison["id"] == "selective-ack-correctness":
        return ["bash", str(REPO / comparison["command"])]

    rate = float(cell["rateRps"])
    duration = int(common["measuredSeconds"])
    warmup = int(common["warmupSeconds"])
    deadline = int(common["requestTimeoutMs"])
    topology = str(REPO / common["topologyFile"])
    seed = str(cell["seed"])

    if system.startswith("ndnsf"):
        providers = list(comparison.get("providerNodes") or [comparison["providerNode"]])
        strategy = "custom-selection" if system == "ndnsf-custom-selection" else "first-responding"
        ack_timeout = int(comparison.get("customAckCollectionMs") or
                          comparison.get("ndnsf", {}).get("ackTimeoutMs", 1000))
        delay_values = comparison.get("providerRequestDelayMs") or [common["serviceDelayMs"]]
        command = [
            sys.executable,
            str(REPO / "Experiments/NDNSF_NewAPI_Minindn_Perf.py"),
            "--topology-file", topology,
            "--user-node", common["userNode"],
            "--controller-node", common["controllerNode"],
            "--provider-nodes", ",".join(providers),
            "--providers", str(len(providers)),
            "--workload-mode", "open-loop",
            "--rate-rps", str(rate),
            "--duration", str(duration),
            "--warmup", str(warmup),
            "--request-timeout-ms", str(deadline),
            "--timeout-ms", str(int(common["globalDeadlineMs"])),
            "--ack-timeout-ms", str(ack_timeout),
            "--drain-seconds", str(int(common["drainSeconds"])),
            "--strategy", strategy,
            "--provider-request-delay-ms-series",
            ",".join(str(value) for value in delay_values),
            "--max-outstanding", str(int(common["maxOutstanding"])),
            "--max-requests", str(int(round(rate * duration))),
            "--handler-threads", "0",
            "--performance-mode",
            "--nfd-log-level", "WARN",
            "--skip-post-run-diagnostics",
            "--output-dir", output,
        ]
        admission_enabled = system == "ndnsf-admission-enabled"
        command.append("--adaptive-admission-control" if admission_enabled
                       else "--disable-adaptive-admission-control")
        return command

    if system == "grpc":
        return [
            sys.executable, str(REPO / "Experiments/gRPC_memphis_ucla_latency.py"),
            "--topology-file", topology,
            "--client-node", common["userNode"],
            "--server-node", comparison["providerNode"],
            "--delay-ms", str(common["serviceDelayMs"]),
            "--count", str(int(round(rate * duration))),
            "--rate-rps", str(rate),
            "--duration-s", str(duration),
            "--timeout-s", str(deadline / 1000.0),
            "--warmup-s", str(warmup),
            "--server-workers", "32",
            "--failure-probability", str(common["failureProbability"]),
            "--epoch-ms", "10000",
            "--seed", seed,
            "--output-dir", output,
        ]

    if system == "nsc":
        return [
            sys.executable, str(REPO / "Experiments/NSC_memphis_ucla_latency.py"),
            "--topology-file", topology,
            "--client-node", common["userNode"],
            "--server-node", comparison["providerNode"],
            "--service-delay-ms", str(common["serviceDelayMs"]),
            "--rate-series", str(rate),
            "--duration-s", str(duration),
            "--warmup-s", str(warmup),
            "--request-deadline-ms", str(deadline),
            "--failure-probability", str(common["failureProbability"]),
            "--epoch-ms", "10000",
            "--seed", seed,
            "--output-dir", output,
        ]
    raise ValueError(f"unsupported registered system: {system}")


def expand_plan(
    registration: dict[str, Any], mode: str, output_root: Path,
    registration_sha256: str, toolchain_sha256: str,
) -> dict[str, Any]:
    comparisons = comparison_by_id(registration)
    blocks: list[dict[str, Any]] = []
    if mode == "pilot":
        for item in registration["pilot"]["cells"]:
            comparison = comparisons[item["comparison"]]
            repetition = next(
                value for value in registration["commonConditions"]["repetitions"]
                if value["id"] == item["repetition"]
            )
            rate = item.get("rateRps")
            block_id = f"{comparison['id']}--{repetition['id']}--rps-{rate if rate is not None else 'none'}"
            block_dir = output_root / "dry-run" / block_id
            cells = [
                make_cell(registration, comparison, repetition, rate, system,
                          block_dir / system)
                for system in deterministic_system_order(
                    item["systems"], comparison["id"], repetition, rate
                )
            ]
            blocks.append({"blockId": block_id, "cells": cells})
    elif mode == "confirmatory":
        repetitions = registration["commonConditions"]["repetitions"]
        for repetition in repetitions:
            repetition_blocks: list[tuple[dict[str, Any], float | int]] = []
            for comparison in registration["comparisons"]:
                if comparison["id"] == "selective-ack-correctness":
                    continue
                for rate in comparison["ratesRps"]:
                    repetition_blocks.append((comparison, rate))
            random.Random(int(repetition["seed"])).shuffle(repetition_blocks)
            for comparison, rate in repetition_blocks:
                rate_token = str(rate).replace(".", "p")
                block_id = f"{comparison['id']}--{repetition['id']}--rps-{rate_token}"
                block_dir = output_root / "runs" / block_id / "attempt-0001"
                cells = [
                    make_cell(
                        registration,
                        comparison,
                        repetition,
                        rate,
                        system,
                        block_dir / system,
                    )
                    for system in deterministic_system_order(
                        comparison["systems"], comparison["id"], repetition, rate
                    )
                ]
                blocks.append({"blockId": block_id, "cells": cells})

        correctness = comparisons["selective-ack-correctness"]
        repetition = repetitions[0]
        block_id = f"{correctness['id']}--{repetition['id']}--rps-none"
        block_dir = output_root / "runs" / block_id / "attempt-0001"
        blocks.append({
            "blockId": block_id,
            "cells": [
                make_cell(
                    registration,
                    correctness,
                    repetition,
                    None,
                    "regression",
                    block_dir / "regression",
                )
            ],
        })
    else:
        raise ValueError(f"unsupported campaign mode: {mode}")

    manifest = {
        "schemaVersion": 1,
        "status": "dry-run",
        "mode": mode,
        "registrationSha256": registration_sha256,
        "toolchainManifestSha256": toolchain_sha256,
        "campaignRunnerSha256": sha256_file(Path(__file__).resolve()),
        "overwrite": "forbidden",
    }
    return {
        "schemaVersion": 1,
        "mode": mode,
        "manuscriptEligible": bool(mode == "confirmatory"),
        "registrationSha256": registration_sha256,
        "toolchainManifestSha256": toolchain_sha256,
        "campaignRunnerSha256": sha256_file(Path(__file__).resolve()),
        "campaignManifest": manifest,
        "blocks": blocks,
    }


def retarget_block(
    registration: dict[str, Any], block: dict[str, Any], attempt_dir: Path,
) -> list[dict[str, Any]]:
    comparisons = comparison_by_id(registration)
    repetitions = {
        item["id"]: item for item in registration["commonConditions"]["repetitions"]
    }
    cells = []
    for planned in block["cells"]:
        cells.append(make_cell(
            registration,
            comparisons[planned["comparison"]],
            repetitions[planned["repetition"]],
            planned["rateRps"],
            planned["system"],
            attempt_dir / planned["system"],
        ))
    return cells


def latest_attempt_directory(block_root: Path) -> Optional[Path]:
    attempts = sorted(block_root.glob("attempt-[0-9][0-9][0-9][0-9]"))
    return attempts[-1] if attempts else None


def write_json_replace(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(str(temporary), str(path))


def execute_campaign(
    registration: dict[str, Any], toolchain: dict[str, Any], mode: str,
    output_root: Path, registration_sha256: str, toolchain_sha256: str,
    resume: bool,
) -> int:
    if output_root.exists() and not resume:
        raise FileExistsError(
            f"output root already exists; refuse overwrite: {output_root}"
        )
    output_root.mkdir(parents=True, exist_ok=True)
    plan = expand_plan(
        registration, mode, output_root, registration_sha256, toolchain_sha256
    )
    plan_path = output_root / "campaign-plan.json"
    manifest_path = output_root / "campaign-manifest.json"
    if plan_path.exists():
        retained_plan = json.loads(plan_path.read_text())
        for key in (
            "mode", "registrationSha256", "toolchainManifestSha256",
            "campaignRunnerSha256",
        ):
            if retained_plan.get(key) != plan.get(key):
                raise ValueError(f"retained campaign plan {key} mismatch")
    else:
        write_json_exclusive(plan_path, plan)

    if manifest_path.exists():
        campaign_manifest = json.loads(manifest_path.read_text())
        for key, expected in (
            ("mode", mode),
            ("registrationSha256", registration_sha256),
            ("toolchainManifestSha256", toolchain_sha256),
            ("campaignRunnerSha256", sha256_file(Path(__file__).resolve())),
        ):
            if campaign_manifest.get(key) != expected:
                raise ValueError(f"retained campaign manifest {key} mismatch")
    else:
        campaign_manifest = {
            "schemaVersion": 1,
            "mode": mode,
            "manuscriptEligible": mode == "confirmatory",
            "registrationSha256": registration_sha256,
            "toolchainManifestSha256": toolchain_sha256,
            "campaignRunnerSha256": sha256_file(Path(__file__).resolve()),
            "startedAt": datetime.now(timezone.utc).isoformat(),
            "status": "running",
            "completedBlocks": [],
            "skippedVerifiedBlocks": [],
            "outcomeBasedRetries": 0,
        }
        write_json_exclusive(manifest_path, campaign_manifest)

    for block in plan["blocks"]:
        block_id = block["blockId"]
        block_root = output_root / "runs" / block_id
        block_root.mkdir(parents=True, exist_ok=True)
        latest = latest_attempt_directory(block_root)
        if resume and latest is not None:
            expected_cells = retarget_block(registration, block, latest)
            reusable, reason = verify_block_attempt(
                block_id,
                expected_cells,
                latest,
                registration_sha256,
                toolchain_sha256,
            )
            if reusable:
                print(f"SKIP verified block {block_id} ({latest.name})", flush=True)
                if block_id not in campaign_manifest["skippedVerifiedBlocks"]:
                    campaign_manifest["skippedVerifiedBlocks"].append(block_id)
                write_json_replace(manifest_path, campaign_manifest)
                continue
            print(f"RERUN whole block {block_id}: {reason}", flush=True)

        attempt = next_attempt_directory(block_root)
        cells = retarget_block(registration, block, attempt)
        print(f"RUN block {block_id} -> {attempt.name}", flush=True)
        valid, reason = execute_block(
            block_id,
            cells,
            attempt,
            registration_sha256,
            toolchain_sha256,
            toolchain,
        )
        if not valid:
            campaign_manifest["status"] = "infrastructure-invalid"
            campaign_manifest["failedBlock"] = block_id
            campaign_manifest["failureReason"] = reason
            campaign_manifest["stoppedAt"] = datetime.now(timezone.utc).isoformat()
            write_json_replace(manifest_path, campaign_manifest)
            write_json_replace(output_root / "campaign-summary.json", {
                "schemaVersion": 1,
                "status": "infrastructure-invalid",
                "failedBlock": block_id,
                "reason": reason,
                "rerunScope": "whole-matched-block",
                "resumeCommandRequired": True,
            })
            return 2
        if block_id not in campaign_manifest["completedBlocks"]:
            campaign_manifest["completedBlocks"].append(block_id)
        write_json_replace(manifest_path, campaign_manifest)

    campaign_manifest["status"] = "pass"
    campaign_manifest.pop("failedBlock", None)
    campaign_manifest.pop("failureReason", None)
    campaign_manifest.pop("stoppedAt", None)
    campaign_manifest["completedAt"] = datetime.now(timezone.utc).isoformat()
    write_json_replace(manifest_path, campaign_manifest)
    write_json_replace(output_root / "campaign-summary.json", {
        "schemaVersion": 1,
        "status": "pass",
        "mode": mode,
        "manuscriptEligible": mode == "confirmatory",
        "blocks": len(plan["blocks"]),
        "cells": sum(len(block["cells"]) for block in plan["blocks"]),
        "outcomeBasedRetries": 0,
    })
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registration", type=Path, default=DEFAULT_REGISTRATION)
    parser.add_argument("--toolchain-manifest", type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument("--output-root", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--mode", choices=("pilot", "confirmatory"))
    mode.add_argument("--pilot", dest="mode", action="store_const", const="pilot")
    mode.add_argument(
        "--confirmatory", dest="mode", action="store_const", const="confirmatory"
    )
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    registration_path = args.registration.resolve()
    toolchain_path = args.toolchain_manifest.resolve()
    if args.preflight:
        report = preflight_report(registration_path, toolchain_path)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if report["status"] == "pass" else 2
    if args.mode is None:
        raise SystemExit("select --pilot, --confirmatory, or --mode")
    if args.output_root is None:
        raise SystemExit("--output-root is required for dry-run or execution")

    registration = load_registration(registration_path)
    toolchain = json.loads(toolchain_path.read_text())
    if toolchain.get("terminalStatus") != "pass":
        raise SystemExit("toolchain manifest is not terminal pass")
    registration_sha = sha256_file(registration_path)
    toolchain_sha = sha256_file(toolchain_path)
    preflight = preflight_report(
        registration_path, toolchain_path, require_execution=not args.dry_run
    )
    if preflight["status"] != "pass":
        print(json.dumps(preflight, indent=2, sort_keys=True))
        return 2
    output_root = args.output_root.resolve()
    plan = expand_plan(
        registration, args.mode, output_root,
        registration_sha, toolchain_sha,
    )
    if args.dry_run:
        output_root.mkdir(parents=True, exist_ok=False)
        (output_root / "dry-run-plan.json").write_text(
            json.dumps(plan, indent=2, sort_keys=True) + "\n"
        )
        print(json.dumps({
            "status": "dry-run",
            "blocks": len(plan["blocks"]),
            "cells": sum(len(block["cells"]) for block in plan["blocks"]),
            "output": str(output_root / "dry-run-plan.json"),
        }, sort_keys=True))
        return 0
    return execute_campaign(
        registration,
        toolchain,
        args.mode,
        output_root,
        registration_sha,
        toolchain_sha,
        args.resume,
    )


if __name__ == "__main__":
    raise SystemExit(main())
