#!/usr/bin/env python3
"""Run the registered Spec 175 native integration cases without skipping.

This runner is deliberately conservative during development.  It maps only
cases that are actually registered in the current binary; an absent I-case is
reported as a blocker instead of being silently treated as a skip/pass.  A
mixed request still executes its registered cases so their evidence is not
hidden by unrelated missing cases.  The same interface is used by the later
G2 process matrix.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "spec175-g2-integration-manifest-v1"
ALL_CASES = tuple(f"I{index:02d}" for index in range(1, 21))
HEALTHY_CASES = frozenset({
    "I01", "I02", "I03", "I12", "I14", "I15", "I16", "I17", "I18",
})
# Formal G2 IDs may be registered only when the test matches the Provider/role,
# tiny-ONNX, lineage, and fault oracle in validation-contract.md.  I01 is the
# one-Provider native tiny-ONNX baseline; I02 and I03 are the two-/four-
# Provider epoch-coordinator subjects; I15 reuses the four-Provider production
# path with a noncanonical role-to-Provider permutation. I04-I07 exercise the
# same production harness with distinct fault oracles and run once each.
REGISTERED: dict[str, str] = {
    "I01": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI01OneProvider",
    "I02": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI02TwoProviderEpochCoordinator",
    "I03": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI03FourProviderEpochCoordinator",
    "I04": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI04ReordersEventThreeAfterFour",
    "I05": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI05SuppressesDuplicateEventFour",
    "I06": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI06RetriesFirstLostEventFive",
    "I07": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI07FailsEveryUnavailableEventFiveMode",
    "I08": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI08CancelsAfterThirdEvent",
    "I09": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI09RejectsTamperAndWithholdsUnselectedGrant",
    "I10": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI10ContainsThirdCallbackFailure",
    "I11": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI11BoundsCapacityOneSlowConsumer",
    "I12": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI12ProviderUnavailableAfterEvent3WithReplacement",
    "I13": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI13ProviderUnavailableAfterEvent3NoReplacement",
    "I14": "Spec170NdnsfDiCoreFlow/Spec175UnaryYoloI14CompletesWithoutStreamState",
    "I15": "Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI15PermutedRoleProviderMap",
}

# Conversation continuation is implemented at the Python application/runtime
# boundary.  Keep these process-level cases in the same G2 manifest while
# recording their runner explicitly; they are not disguised as C++ network
# tests.  The tests exercise fresh request/generation authority, checkpoint
# lineage, role receipts, tier transitions, fallback mutations, CAS conflict,
# and cancellation during prefetch.
PYTHON_REGISTERED: dict[str, str] = {
    "I16": "tests/python/test_spec175_conversation.py::test_i16_one_provider_two_turns_finalizes_without_event",
    "I17": "tests/python/test_spec175_conversation.py::test_i17_four_role_two_turns_commit_one_successor_checkpoint",
    "I18": "tests/python/test_spec175_conversation.py::test_i18_three_conversations_are_isolated_across_host_prefetch",
    "I19": "tests/python/test_spec175_conversation.py::test_i19_invalid_state_mutations_fallback_without_runner_use",
    "I20": "tests/python/test_spec175_conversation.py::test_i20_same_parent_has_one_winner_and_cancelled_prefetch_leaks_nothing",
}


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def parse_cases(value: str) -> tuple[str, ...]:
    selected: set[str] = set()
    for part in value.split(","):
        part = part.strip().upper()
        if not part:
            continue
        match = re.fullmatch(r"I(\d{2})(?:-I(\d{2}))?", part)
        if not match:
            raise ValueError(f"invalid Spec175 case selector: {part}")
        first = int(match.group(1))
        last = int(match.group(2) or match.group(1))
        if first < 1 or last > 20 or first > last:
            raise ValueError(f"Spec175 case selector out of range: {part}")
        selected.update(f"I{index:02d}" for index in range(first, last + 1))
    if not selected:
        raise ValueError("at least one Spec175 case is required")
    return tuple(case for case in ALL_CASES if case in selected)


def _run_case(binary: Path, case_id: str, test_name: str, *, seed: int,
              repetition: int, log_dir: Path,
              timeout_seconds: float = 120.0) -> dict[str, object]:
    command = [str(binary), f"--run_test={test_name}", "--log_level=message"]
    started = time.monotonic()
    timed_out = False
    try:
        completed = subprocess.run(
            command,
            cwd=binary.parent.parent,
            env={**os.environ, "SPEC175_SEED": str(seed)},
            text=True,
            capture_output=True,
            check=False,
            timeout=timeout_seconds,
        )
        stdout = completed.stdout
        stderr = completed.stderr
        exit_code = completed.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        stderr += ("\nSPEC175_CASE_TIMEOUT "
                   f"after {timeout_seconds:.3f} seconds\n")
        exit_code = 124
    log_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{case_id.lower()}-r{repetition}"
    stdout_path = log_dir / f"{stem}.stdout.log"
    stderr_path = log_dir / f"{stem}.stderr.log"
    stdout_path.write_text(stdout, encoding="utf-8")
    stderr_path.write_text(stderr, encoding="utf-8")
    return {
        "caseId": case_id,
        "testName": test_name,
        "seed": seed,
        "repetition": repetition,
        "command": command,
        "exitCode": exit_code,
        "status": "PASS" if exit_code == 0 else ("TIMEOUT" if timed_out else "FAIL"),
        "timedOut": timed_out,
        "timeoutSeconds": timeout_seconds,
        "durationSeconds": round(time.monotonic() - started, 3),
        "stdoutBytes": len(stdout.encode()),
        "stderrBytes": len(stderr.encode()),
        "stdoutPath": str(stdout_path),
        "stderrPath": str(stderr_path),
        "stdoutSha256": _sha256(stdout_path),
        "stderrSha256": _sha256(stderr_path),
    }


def _probe_native_binary(binary: Path, cases: Iterable[str]) -> dict[str, object]:
    """Verify the G2 executable before starting any registered case.

    ``unit-tests`` may expose similarly named registration text but reject
    integration filters at runtime. Probe the exact integration executable and
    its registered test inventory first so an operator mistake becomes a
    deterministic, side-effect-free blocker instead of a misleading matrix of
    exit-code-200 failures.
    """
    selected = tuple(cases)
    expected = tuple(REGISTERED[case] for case in selected if case in REGISTERED)
    if not expected:
        return {"status": "NOT_REQUIRED", "missingTests": [], "command": []}
    if binary.name != "integration-tests":
        return {
            "status": "BLOCKED_BINARY_KIND",
            "expectedName": "integration-tests",
            "actualName": binary.name,
            "missingTests": list(expected),
            "command": [],
        }
    command = [str(binary), "--list_content"]
    try:
        completed = subprocess.run(
            command,
            cwd=binary.parent.parent,
            env=dict(os.environ),
            text=True,
            capture_output=True,
            check=False,
            timeout=30.0,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "status": "BLOCKED_BINARY_PROBE",
            "missingTests": list(expected),
            "command": command,
            "error": str(error),
        }
    listing = completed.stdout + completed.stderr
    listed_tests: set[str] = set()
    suite = ""
    for line in listing.splitlines():
        stripped = line.strip().rstrip("*")
        if not stripped:
            continue
        if line[0].isspace():
            if suite:
                listed_tests.add(f"{suite}/{stripped}")
        else:
            suite = stripped
    missing = [test for test in expected if test not in listed_tests]
    listing_hash = "sha256:" + hashlib.sha256(
        listing.encode("utf-8", errors="replace")).hexdigest()
    if completed.returncode != 0 or missing:
        return {
            "status": "BLOCKED_BINARY_REGISTRY",
            "returnCode": completed.returncode,
            "missingTests": missing,
            "command": command,
            "listingSha256": listing_hash,
        }
    return {
        "status": "PASS",
        "returnCode": completed.returncode,
        "missingTests": [],
        "command": command,
        "listingSha256": listing_hash,
    }


def _run_python_case(case_id: str, test_name: str, *, seed: int,
                     repetition: int, log_dir: Path,
                     timeout_seconds: float = 120.0) -> dict[str, object]:
    command = [sys.executable, "-m", "pytest", "-q", "-s", test_name]
    python_path = [
        str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
        str(ROOT / "NDNSF-DistributedInference"),
        str(ROOT / "pythonWrapper"),
    ]
    existing = os.environ.get("PYTHONPATH", "")
    if existing:
        python_path.append(existing)
    started = time.monotonic()
    timed_out = False
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            env={**os.environ, "PYTHONPATH": os.pathsep.join(python_path),
                 "SPEC175_SEED": str(seed)},
            text=True,
            capture_output=True,
            check=False,
            timeout=timeout_seconds,
        )
        stdout = completed.stdout
        stderr = completed.stderr
        exit_code = completed.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        stderr += ("\nSPEC175_PYTHON_CASE_TIMEOUT "
                   f"after {timeout_seconds:.3f} seconds\n")
        exit_code = 124
    log_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{case_id.lower()}-r{repetition}"
    stdout_path = log_dir / f"{stem}.stdout.log"
    stderr_path = log_dir / f"{stem}.stderr.log"
    stdout_path.write_text(stdout, encoding="utf-8")
    stderr_path.write_text(stderr, encoding="utf-8")
    metrics = None
    for line in stdout.splitlines():
        if not line.startswith("SPEC175_CONVERSATION_METRICS "):
            continue
        try:
            candidate = json.loads(line.split(" ", 1)[1])
        except json.JSONDecodeError:
            continue
        if isinstance(candidate, dict):
            metrics = candidate
            break
    return {
        "caseId": case_id,
        "runner": "python",
        "testName": test_name,
        "seed": seed,
        "repetition": repetition,
        "command": command,
        "exitCode": exit_code,
        "status": "PASS" if exit_code == 0 else ("TIMEOUT" if timed_out else "FAIL"),
        "timedOut": timed_out,
        "timeoutSeconds": timeout_seconds,
        "durationSeconds": round(time.monotonic() - started, 3),
        "stdoutBytes": len(stdout.encode()),
        "stderrBytes": len(stderr.encode()),
        "stdoutPath": str(stdout_path),
        "stderrPath": str(stderr_path),
        "stdoutSha256": _sha256(stdout_path),
        "stderrSha256": _sha256(stderr_path),
        "metrics": metrics,
        "metricsMissing": metrics is None,
    }


def run_gate(*, binary: Path, cases: Iterable[str], healthy_repeats: int,
             seed: int, output: Path, source_seal: Path,
             case_timeout_seconds: float = 120.0) -> dict[str, object]:
    selected = tuple(cases)
    if healthy_repeats < 1:
        raise ValueError("healthy-repeats must be positive")
    if case_timeout_seconds <= 0:
        raise ValueError("case-timeout-seconds must be positive")
    source_seal = source_seal.expanduser().resolve()
    if not source_seal.is_file():
        raise ValueError(f"source seal is missing: {source_seal}")
    missing = [case for case in selected
               if case not in REGISTERED and case not in PYTHON_REGISTERED]
    native_selected = tuple(case for case in selected if case in REGISTERED)
    binary_available = binary.is_file() and os.access(binary, os.X_OK)
    binary_probe = (_probe_native_binary(binary, native_selected)
                    if binary_available else {
                        "status": "BLOCKED_BINARY_UNAVAILABLE",
                        "missingTests": [REGISTERED[case]
                                         for case in native_selected],
                        "command": [],
                    })
    generated_at = dt.datetime.now(dt.timezone.utc).isoformat()
    run_id = generated_at.replace(":", "").replace("+", "-")
    log_dir = output.expanduser().resolve().parent / "logs" / run_id
    results: list[dict[str, object]] = []
    # Run every registered subject even when another requested subject is
    # missing.  A missing case still blocks the gate, but suppressing healthy
    # registered evidence would make a mixed audit request unable to show
    # which part is actually passing.
    if not native_selected or binary_probe["status"] == "PASS":
        for case in selected:
            if case in REGISTERED and not binary_available:
                continue
            if case not in REGISTERED and case not in PYTHON_REGISTERED:
                continue
            repetitions = healthy_repeats if case in HEALTHY_CASES else 1
            for repetition in range(repetitions):
                if case in REGISTERED:
                    result = _run_case(
                        binary, case, REGISTERED[case], seed=seed,
                        repetition=repetition + 1, log_dir=log_dir,
                        timeout_seconds=case_timeout_seconds)
                else:
                    result = _run_python_case(
                        case, PYTHON_REGISTERED[case], seed=seed,
                        repetition=repetition + 1, log_dir=log_dir,
                        timeout_seconds=case_timeout_seconds)
                results.append(result)
    if missing:
        status = "BLOCKED_MISSING_CASES"
    elif native_selected and not binary_available:
        status = "BLOCKED_BINARY_UNAVAILABLE"
    elif native_selected and binary_probe["status"] != "PASS":
        status = str(binary_probe["status"])
    elif any(item.get("runner") == "python" and item.get("metricsMissing")
             for item in results):
        status = "FAIL_MISSING_METRICS"
    else:
        status = "PASS" if all(
            item["status"] == "PASS" for item in results) else "FAIL"
    manifest = {
        "schema": SCHEMA,
        "gate": "G2",
        "status": status,
        "generatedAt": generated_at,
        "logDirectory": str(log_dir),
        "binary": str(binary),
        "binaryAvailable": binary_available,
        "binarySha256": _sha256(binary) if binary_available else None,
        "binaryProbe": binary_probe,
        "pythonExecutable": sys.executable,
        "sourceSeal": {
            "path": str(source_seal),
            "sha256": _sha256(source_seal),
        },
        "cases": list(selected),
        "healthyRepeats": healthy_repeats,
        "faultRepeats": 1,
        "caseTimeoutSeconds": case_timeout_seconds,
        "healthyCases": sorted(HEALTHY_CASES),
        "seed": seed,
        "registeredCases": {
            case: REGISTERED.get(case, PYTHON_REGISTERED.get(case))
            for case in selected
        },
        "runnerByCase": {
            case: ("cpp-integration" if case in REGISTERED else "python")
            for case in selected if case in REGISTERED or case in PYTHON_REGISTERED
        },
        "missingCases": missing,
        "metricsRequired": bool(set(selected) & set(PYTHON_REGISTERED)),
        "results": results,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/integration-tests")
    parser.add_argument("--cases", default="I01-I20")
    parser.add_argument("--healthy-repeats", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1750001)
    parser.add_argument("--source-seal", type=Path, required=True)
    parser.add_argument("--case-timeout-seconds", type=float, default=120.0)
    parser.add_argument("--output", default="results/spec175/g2/qualification-manifest-v1.json")
    args = parser.parse_args(argv)
    try:
        cases = parse_cases(args.cases)
        manifest = run_gate(
            binary=Path(args.binary).resolve(), cases=cases,
            healthy_repeats=args.healthy_repeats, seed=args.seed,
            output=Path(args.output).resolve(), source_seal=args.source_seal,
            case_timeout_seconds=args.case_timeout_seconds,
        )
    except (OSError, ValueError) as exc:
        print(json.dumps({"status": "BLOCKED", "error": str(exc)}, sort_keys=True))
        return 2
    print(json.dumps({
        "status": manifest["status"],
        "missingCases": manifest["missingCases"],
        "resultCount": len(manifest["results"]),
        "output": str(Path(args.output).resolve()),
    }, sort_keys=True))
    return 0 if manifest["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
