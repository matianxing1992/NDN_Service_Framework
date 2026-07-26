#!/usr/bin/env python3
"""Run the frozen Spec 111 10-pair MiniNDN campaign exactly once per cell.

The journal is deliberately resumable without retry: a cell with a start
record but no terminal record is classified INTERRUPTED_NO_RERUN on resume.
Terminal cells are skipped, never replaced.

The default mode is the single-candidate formal campaign and stops at the first
failed cell. ``--continue-diagnostic-after-terminal-failure`` is only for bug
convergence: it preserves every terminal record, skips it, and advances to the
next unstarted cell. Such a mixed-candidate sweep is never formal evidence.
"""

from __future__ import annotations

import argparse
import csv
import fcntl
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BASELINE_ROOT = Path("/tmp/spec111-baseline-4d695ce8")
CANDIDATE_PATH = ROOT / (
    "specs/111-ndnsf-di-core-app-separation/evidence/"
    "post-separation-candidate.json"
)
BASE_COMMIT = "4d695ce8b7ffe2c79465dc1f3db649a5a65806a6"
PAIR_SEEDS = tuple(range(11101, 11111))
DEFERRED = "DEFERRED_TO_SPEC110"
BASELINE_MEASUREMENT_PATCH_PATHS = (
    "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py",
    "pythonWrapper/src/ndnsf/_ndnsf.cpp",
    "ndn-service-framework/NDNSFMessages.cpp",
    "ndn-service-framework/NDNSFMessages.hpp",
    "ndn-service-framework/ServiceUser.cpp",
    "ndn-service-framework/ServiceUser.hpp",
    "ndn-service-framework/ServiceProvider.cpp",
    "ndn-service-framework/ServiceProvider.hpp",
)
READINESS_FATAL_LOG_MARKERS = (
    "Validator/policy did not invoke success or failure callback",
    "No sub-element of type 602",
    "Failed to fetch collaboration scope key",
    "Collaboration handler failed",
    "hybrid MessageKey is not cached and not attached",
    "hybrid AES-GCM authentication failed",
    "Fatal Python error",
    "Segmentation fault",
)


def sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def canonical_digest(value: object) -> str:
    return sha256_bytes(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    )


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * q
    lower = int(index)
    upper = min(len(ordered) - 1, lower + 1)
    fraction = index - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def process_snapshot() -> list[dict[str, object]]:
    result = subprocess.run(
        ["ps", "-eo", "pid=,ppid=,rss=,comm=,args="],
        text=True,
        capture_output=True,
        check=True,
    )
    rows: list[dict[str, object]] = []
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 4)
        if len(fields) != 5:
            continue
        rows.append({
            "pid": int(fields[0]),
            "ppid": int(fields[1]),
            "rssKiB": int(fields[2]),
            "comm": fields[3],
            "args": fields[4],
        })
    return rows


def active_minindn_processes() -> list[dict[str, object]]:
    active = []
    for row in process_snapshot():
        comm = str(row["comm"])
        args = str(row["args"])
        if comm in {"nfd", "nlsr", "mnexec", "mininet"}:
            active.append(row)
        elif comm.startswith("python") and "NDNSF_DI_LlmPipeline_Minindn.py" in args:
            active.append(row)
    return active


def descendant_rss_kib(root_pid: int) -> int:
    rows = process_snapshot()
    children: dict[int, list[int]] = {}
    rss = {int(row["pid"]): int(row["rssKiB"]) for row in rows}
    for row in rows:
        children.setdefault(int(row["ppid"]), []).append(int(row["pid"]))
    pending = [root_pid]
    seen: set[int] = set()
    while pending:
        pid = pending.pop()
        if pid in seen:
            continue
        seen.add(pid)
        pending.extend(children.get(pid, ()))
    return sum(rss.get(pid, 0) for pid in seen)


def preflight(output: Path) -> dict[str, object]:
    if output.exists():
        raise RuntimeError(f"SPEC111_OUTPUT_ALREADY_EXISTS:{output}")
    sudo = subprocess.run(["sudo", "-n", "true"], check=False)
    if sudo.returncode != 0:
        raise RuntimeError("SPEC111_NONINTERACTIVE_SUDO_REQUIRED")
    active = active_minindn_processes()
    if active:
        raise RuntimeError("SPEC111_LIVE_MININDN_OWNER:" + json.dumps(active))
    free = shutil.disk_usage(ROOT).free
    if free < 5 * 1024**3:
        raise RuntimeError(f"SPEC111_DISK_HEADROOM_LOW:{free}")
    return {
        "sudo": "PASS",
        "liveOwners": [],
        "outputUnused": True,
        "freeBytes": free,
    }


def source_pythonpath(source_root: Path) -> str:
    """Return a source-root-isolated path for role startup imports."""
    return ":".join(str(path) for path in (
        source_root / "NDNSF-DistributedInference",
        source_root / "NDNSF-DistributedRepo/pythonWrapper",
        source_root / "pythonWrapper",
        source_root / "Experiments",
    ))


def ephemeral_app_state_for(output: Path) -> Path:
    digest = hashlib.sha256(str(output.resolve()).encode()).hexdigest()[:24]
    return Path("/tmp") / f"spec111-app-state-{digest}"


def cleanup_ephemeral_app_state(output: Path) -> dict[str, object]:
    state_root = ephemeral_app_state_for(output)
    if (state_root.parent != Path("/tmp") or
            not state_root.name.startswith("spec111-app-state-")):
        raise RuntimeError("SPEC111_UNSAFE_APP_STATE_CLEANUP_PATH")
    completed = subprocess.run(
        ["sudo", "-n", "rm", "-rf", "--", str(state_root)],
        text=True, capture_output=True, check=False)
    return {
        "path": str(state_root),
        "returncode": int(completed.returncode),
        "removed": not state_root.exists(),
    }


def role_import_commands(source_root: Path) -> list[tuple[str, list[str]]]:
    """Build zero-network startup-import commands for the three real roles."""
    prefix = [
        "sudo", "-n", "-E", "env",
        f"PYTHONPATH={source_pythonpath(source_root)}",
        "PYTHONNOUSERSITE=1",
        "PYTHONDONTWRITEBYTECODE=1",
        "timeout", "15s", "python3",
    ]
    example = source_root / (
        "examples/python/NDNSF-DistributedInference/llm_pipeline")
    canonical_controller = source_root / (
        "NDNSF-DistributedInference/ndnsf_distributed_inference/"
        "app_sdk/controller.py")
    controller_import = (
        "from ndnsf_distributed_inference.app_sdk.controller "
        "import APPController, DistributedInferenceController; "
        "assert callable(APPController.from_config); "
        "assert callable(DistributedInferenceController.create)"
        if canonical_controller.is_file()
        else
        "from ndnsf_distributed_inference import APPController; "
        "assert callable(APPController.from_config)"
    )
    user_import = (
        "import os, runpy, sys; "
        "sys.path.insert(0, os.path.dirname(sys.argv[1])); "
        "runpy.run_path(sys.argv[1], run_name='spec111_role_import'); "
        "from ndnsf_distributed_inference.app_sdk.facades import APPClient; "
        "APPClient(object(), object())"
        if canonical_controller.is_file()
        else
        "import os, runpy, sys; "
        "sys.path.insert(0, os.path.dirname(sys.argv[1])); "
        "runpy.run_path(sys.argv[1], run_name='spec111_role_import'); "
        "from ndnsf_distributed_inference import APPClient; "
        "APPClient(object(), object())"
    )
    return [
        ("controller", [*prefix, "-c", controller_import]),
        ("provider", [*prefix, str(example / "provider.py"), "--help"]),
        ("user", [*prefix, "-c", user_import, str(example / "user.py")]),
    ]


def run_role_import_preflight(
    source_root: Path,
    *,
    runner=subprocess.run,
) -> dict[str, object]:
    """Validate real role imports without constructing an NDN runtime."""
    roles: list[dict[str, object]] = []
    for role, command in role_import_commands(source_root):
        completed = runner(
            command,
            cwd=str(source_root),
            text=True,
            capture_output=True,
            check=False,
        )
        record = {
            "role": role,
            "returncode": int(completed.returncode),
            "commandDigest": canonical_digest(command),
            "stdoutSha256": sha256_bytes((completed.stdout or "").encode()),
            "stderrSha256": sha256_bytes((completed.stderr or "").encode()),
        }
        roles.append(record)
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout or "no output").strip()
            detail = detail.splitlines()[-1][:240] if detail else "no output"
            raise RuntimeError(
                f"SPEC111_ROLE_IMPORT_PREFLIGHT_FAILED:{role}:{detail}")
    return {
        "status": "PASS",
        "sourceRoot": str(source_root),
        "roles": roles,
        "networkRuntimeStarts": 0,
    }


def cell_order() -> list[tuple[int, int, str]]:
    cells = []
    for pair, seed in enumerate(PAIR_SEEDS, 1):
        variants = ("baseline", "treatment") if pair % 2 else ("treatment", "baseline")
        for variant in variants:
            cells.append((pair, seed, variant))
    return cells


def command_for(source_root: Path, cell_id: str, seed: int, output: Path) -> list[str]:
    command = [
        "sudo", "-n", "-E", "env",
        f"PYTHONPATH={source_pythonpath(source_root)}",
        "PYTHONNOUSERSITE=1",
        "PYTHONDONTWRITEBYTECODE=1",
        f"PYTHONHASHSEED={seed}",
        "NDNSF_TIMELINE_TRACE=1",
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE=100",
        "timeout", "240s", "python3",
        "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
        "--topology-file", "Experiments/Topology/AI_Lab.conf",
        "--output-dir", str(output),
        "--campaign-id", cell_id,
        "--runtime", "fake", "--stages", "3", "--layers", "24",
        "--compute-delay-ms", "1",
        "--warmup-requests", "10",
        "--measured-requests", "60",
        "--measured-duration-s", "60",
        "--request-interval-ms", "1000",
        "--max-new-tokens", "1",
        "--ack-timeout-ms", "1500",
        "--timeout-ms", "60000",
        "--ndn-log", "ndn_service_framework.*=WARN",
    ]
    if source_root.resolve() == ROOT.resolve():
        command.extend([
            "--app-state-root", str(ephemeral_app_state_for(output)),
            "--test-only-allow-ephemeral-app-state",
        ])
    return command


def readiness_command_for(
    source_root: Path,
    variant: str,
    output: Path,
) -> list[str]:
    """Build the fail-fast real-runtime gate that precedes the frozen matrix."""
    command = command_for(
        source_root,
        f"spec111-readiness-{variant}",
        11100,
        output,
    )
    return command


def parse_runtime_readiness(output: Path, returncode: int) -> dict[str, object]:
    metrics = output / "llm-pipeline-user-measured.csv"
    phases: list[dict[str, str]] = []
    if metrics.is_file():
        with metrics.open(newline="", encoding="utf-8") as stream:
            phases = list(csv.DictReader(stream))
    phase_status = {
        phase: {
            "count": sum(row.get("phase") == phase for row in phases),
            "ok": sum(
                row.get("phase") == phase and row.get("status") == "ok"
                for row in phases
            ),
        }
        for phase in ("warmup", "measured")
    }
    passed = (
        returncode == 0 and len(phases) == 70 and
        phase_status == {
            "warmup": {"count": 10, "ok": 10},
            "measured": {"count": 60, "ok": 60},
        }
    )
    return {
        "status": "PASS" if passed else "FAIL",
        "returncode": returncode,
        "requestCount": len(phases),
        "phaseStatus": phase_status,
        "metricsPath": str(metrics),
        "metricsSha256": sha256_file(metrics) if metrics.is_file() else None,
    }


def readiness_fatal_log_findings(output: Path) -> list[dict[str, object]]:
    findings: list[dict[str, object]] = []
    for log_path in sorted(output.rglob("*.log")):
        text = log_path.read_text(encoding="utf-8", errors="replace")
        for marker in READINESS_FATAL_LOG_MARKERS:
            if marker not in text:
                continue
            findings.append({
                "path": str(log_path),
                "marker": marker,
                "count": text.count(marker),
            })
    return findings


def run_runtime_readiness_preflight(
    source_root: Path,
    variant: str,
    output: Path,
    *,
    runner=None,
    cleanup_runner=None,
    preflight_fn=preflight,
    active_fn=active_minindn_processes,
) -> dict[str, object]:
    """Run the complete formal workload before creating immutable cell journals."""
    runner = subprocess.run if runner is None else runner
    cleanup_runner = subprocess.run if cleanup_runner is None else cleanup_runner
    facts = preflight_fn(output)
    state_root = ephemeral_app_state_for(output)
    if source_root.resolve() == ROOT.resolve() and state_root.exists():
        raise RuntimeError(f"SPEC111_APP_STATE_ALREADY_EXISTS:{state_root}")
    output.mkdir(parents=True, exist_ok=False)
    command = readiness_command_for(source_root, variant, output)
    log_path = output / "readiness-run.log"
    with log_path.open("wb") as log:
        completed = runner(
            command,
            cwd=str(source_root),
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        )
    cleanup = cleanup_runner(
        ["sudo", "-n", "mn", "-c"],
        text=True,
        capture_output=True,
        check=False,
    )
    app_state_cleanup = (
        cleanup_ephemeral_app_state(output)
        if source_root.resolve() == ROOT.resolve()
        else {"path": "", "returncode": 0, "removed": True}
    )
    fatal_log_findings = readiness_fatal_log_findings(output)
    parsed = parse_runtime_readiness(output, int(completed.returncode))
    if fatal_log_findings:
        parsed["status"] = "FAIL"
    result = {
        "schema": "ndnsf-di-spec111-runtime-readiness-v1",
        "variant": variant,
        "sourceRoot": str(source_root),
        "command": command,
        "commandDigest": canonical_digest(command),
        "commandLog": str(log_path),
        "commandLogSha256": sha256_file(log_path),
        "preflight": facts,
        "cleanup": {
            "returncode": int(cleanup.returncode),
            "survivors": active_fn(),
            "appState": app_state_cleanup,
        },
        "fatalLogFindings": fatal_log_findings,
        **parsed,
    }
    write_json(output / "readiness-result.json", result)
    if fatal_log_findings:
        raise RuntimeError(
            "SPEC111_RUNTIME_READINESS_FATAL_LOG:"
            f"{variant}:findings={len(fatal_log_findings)}"
        )
    if (result["status"] != "PASS" or result["cleanup"]["survivors"] or
            not result["cleanup"]["appState"]["removed"]):
        raise RuntimeError(
            "SPEC111_RUNTIME_READINESS_FAILED:"
            f"{variant}:returncode={result['returncode']}:"
            f"phases={result['phaseStatus']}:"
            f"survivors={len(result['cleanup']['survivors'])}"
        )
    return result


def ensure_runtime_readiness_preflight(
    source_root: Path,
    variant: str,
    output: Path,
) -> dict[str, object]:
    """Reuse an immutable passing readiness record or run it once.

    Resume must not spend another 70-request workload merely to reach the next
    unstarted matrix cell. A recorded failure is never silently replaced.
    """
    result_path = output / "readiness-result.json"
    if not result_path.is_file():
        return run_runtime_readiness_preflight(source_root, variant, output)
    result = json.loads(result_path.read_text(encoding="utf-8"))
    survivors = result.get("cleanup", {}).get("survivors", [])
    if (
        result.get("variant") != variant or
        result.get("status") != "PASS" or
        survivors
    ):
        raise RuntimeError(
            "SPEC111_EXISTING_READINESS_NOT_PASSING:"
            f"{variant}:status={result.get('status', 'unknown')}:"
            f"survivors={len(survivors)}"
        )
    return result


def parse_cell(output: Path, returncode: int, peak_rss_kib: int,
               duration_s: float) -> dict[str, object]:
    metrics = output / "llm-pipeline-user-measured.csv"
    latencies: list[float] = []
    failures = 0
    rows = 0
    if metrics.is_file():
        with metrics.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                if row.get("phase") != "measured":
                    continue
                rows += 1
                if row.get("status") == "ok":
                    latencies.append(float(row["distributed_ms"]))
                else:
                    failures += 1
    queue_values: list[int] = []
    for log_path in output.rglob("*.log") if output.exists() else ():
        text = log_path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"(?:queue|queueDepth)=([0-9]+)", text):
            queue_values.append(int(match.group(1)))
    fatal_log_findings = readiness_fatal_log_findings(output)
    expected = (
        rows == 60 and len(latencies) == 60 and failures == 0 and
        not fatal_log_findings
    )
    try:
        metrics_name = str(metrics.relative_to(ROOT))
    except ValueError:
        metrics_name = str(metrics)
    return {
        "returncode": returncode,
        "status": "PASS" if returncode == 0 and expected else "FAILED_OBSERVED",
        "measuredRows": rows,
        "completedRequests": len(latencies),
        "failedRequests": failures,
        "completionRatio": len(latencies) / 60.0,
        "p50Ms": statistics.median(latencies) if latencies else None,
        "p95Ms": percentile(latencies, 0.95),
        "throughputRps": len(latencies) / 60.0,
        "peakProcessTreeRssKiB": peak_rss_kib,
        "queueSamples": len(queue_values),
        "maxQueueObserved": max(queue_values) if queue_values else None,
        "fatalLogFindings": fatal_log_findings,
        "wallDurationSeconds": duration_s,
        "metricsPath": metrics_name,
        "metricsSha256": sha256_file(metrics) if metrics.is_file() else None,
    }


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_cell(campaign_root: Path, pair: int, seed: int, variant: str) -> dict[str, object]:
    cell_id = f"spec111-pair-{pair:02d}-{variant}"
    output = campaign_root / "cells" / cell_id
    start_path = campaign_root / "journal" / f"{cell_id}.started.json"
    result_path = campaign_root / "journal" / f"{cell_id}.result.json"
    if result_path.is_file():
        return json.loads(result_path.read_text(encoding="utf-8"))
    if start_path.is_file():
        result = {
            "cellId": cell_id,
            "pair": pair,
            "seed": seed,
            "variant": variant,
            "status": "INTERRUPTED_NO_RERUN",
            "started": json.loads(start_path.read_text(encoding="utf-8")),
        }
        write_json(result_path, result)
        return result
    facts = preflight(output)
    source_root = BASELINE_ROOT if variant == "baseline" else ROOT
    state_root = ephemeral_app_state_for(output)
    if variant == "treatment" and state_root.exists():
        raise RuntimeError(f"SPEC111_APP_STATE_ALREADY_EXISTS:{state_root}")
    command = command_for(source_root, cell_id, seed, output)
    command_digest = canonical_digest(command)
    started = {
        "schema": "ndnsf-di-spec111-cell-start-v1",
        "cellId": cell_id,
        "pair": pair,
        "seed": seed,
        "variant": variant,
        "sourceRoot": str(source_root),
        "sourceCommit": BASE_COMMIT,
        "command": command,
        "commandDigest": command_digest,
        "preflight": facts,
        "startedAtUnixNs": time.time_ns(),
        "noAutomaticRerun": True,
    }
    write_json(start_path, started)
    command_log = campaign_root / "logs" / f"{cell_id}.log"
    started_clock = time.monotonic()
    peak_rss_kib = 0
    with command_log.open("wb") as log:
        proc = subprocess.Popen(command, cwd=source_root, stdout=log, stderr=subprocess.STDOUT)
        while proc.poll() is None:
            try:
                peak_rss_kib = max(peak_rss_kib, descendant_rss_kib(proc.pid))
            except (OSError, subprocess.SubprocessError):
                pass
            time.sleep(1.0)
        returncode = int(proc.returncode or 0)
    duration_s = time.monotonic() - started_clock
    cleanup = subprocess.run(
        ["sudo", "-n", "mn", "-c"], text=True, capture_output=True, check=False
    )
    app_state_cleanup = (
        cleanup_ephemeral_app_state(output)
        if variant == "treatment"
        else {"path": "", "returncode": 0, "removed": True}
    )
    parsed = parse_cell(output, returncode, peak_rss_kib, duration_s)
    result = {
        "schema": "ndnsf-di-spec111-cell-result-v1",
        "cellId": cell_id,
        "pair": pair,
        "seed": seed,
        "variant": variant,
        "candidateId": (
            f"baseline-{BASE_COMMIT[:12]}" if variant == "baseline" else
            json.loads(CANDIDATE_PATH.read_text(encoding="utf-8"))["candidateId"]
        ),
        "commandDigest": command_digest,
        "commandLog": str(command_log.relative_to(ROOT)),
        "commandLogSha256": sha256_file(command_log),
        "finishedAtUnixNs": time.time_ns(),
        "cleanup": {
            "command": ["sudo", "-n", "mn", "-c"],
            "returncode": cleanup.returncode,
            "survivors": active_minindn_processes(),
            "appState": app_state_cleanup,
        },
        **parsed,
    }
    write_json(result_path, result)
    return result


def require_formal_cell_passed(
    result: dict[str, object],
) -> dict[str, object]:
    """Stop the campaign at the first broken cell so diagnosis precedes more work."""
    if result.get("status") != "PASS":
        raise RuntimeError(
            "SPEC111_FORMAL_CELL_FAILED:"
            f"{result.get('cellId', 'unknown')}:"
            f"status={result.get('status', 'unknown')}:"
            f"completed={result.get('completedRequests', 0)}"
        )
    return result


def accept_terminal_cell(
    result: dict[str, object],
    *,
    continue_diagnostic_after_failure: bool,
) -> dict[str, object]:
    """Apply formal fail-stop or diagnostic skip-without-rerun semantics."""
    if result.get("status") == "PASS":
        return result
    if continue_diagnostic_after_failure:
        return result
    return require_formal_cell_passed(result)


def write_diagnostic_continuation_marker(
    campaign_root: Path,
    candidate: dict[str, object],
) -> dict[str, object]:
    """Permanently exclude a continued sweep from formal comparison."""
    marker_path = campaign_root / "diagnostic-continuation.json"
    marker = {
        "schema": "ndnsf-di-spec111-diagnostic-continuation-v1",
        "formalComparisonEligible": False,
        "reason": "continued-after-terminal-failure-without-cell-rerun",
        "candidateIds": [],
    }
    if marker_path.is_file():
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
    candidate_ids = list(marker.get("candidateIds", []))
    candidate_id = str(candidate["candidateId"])
    if candidate_id not in candidate_ids:
        candidate_ids.append(candidate_id)
    marker["candidateIds"] = candidate_ids
    marker["lastContinuedAtUnixNs"] = time.time_ns()
    write_json(marker_path, marker)
    return marker


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign-root", type=Path, required=True)
    parser.add_argument(
        "--continue-diagnostic-after-terminal-failure",
        action="store_true",
        help=(
            "preserve failed/interrupted terminal cells and continue at the "
            "next unstarted cell; permanently ineligible for formal comparison"
        ),
    )
    args = parser.parse_args()
    campaign_root = args.campaign_root.resolve()
    role_import_preflight = {
        "treatment": run_role_import_preflight(ROOT),
        "baseline": run_role_import_preflight(BASELINE_ROOT),
    }
    candidate = json.loads(CANDIDATE_PATH.read_text(encoding="utf-8"))
    for field in ("ociDigest", "sifDigest", "containerBuildDigest", "itigerExecutionDigest"):
        if candidate[field] != DEFERRED:
            raise SystemExit(f"SPEC111_REMOTE_IDENTITY_FORBIDDEN:{field}")
    readiness_root = campaign_root.with_name(campaign_root.name + "-readiness")
    treatment_readiness_root = readiness_root / "treatment"
    if args.continue_diagnostic_after_terminal_failure:
        treatment_readiness_root = (
            readiness_root / "continuations" /
            str(candidate["candidateId"]) / "treatment"
        )
    runtime_readiness = {
        "treatment": ensure_runtime_readiness_preflight(
            ROOT, "treatment", treatment_readiness_root),
        "baseline": ensure_runtime_readiness_preflight(
            BASELINE_ROOT, "baseline", readiness_root / "baseline"),
    }
    campaign_root.mkdir(parents=True, exist_ok=True)
    (campaign_root / "journal").mkdir(exist_ok=True)
    (campaign_root / "logs").mkdir(exist_ok=True)
    lock_path = campaign_root / ".campaign.lock"
    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest_path = campaign_root / "campaign-manifest.json"
        order = [
            {"pair": pair, "seed": seed, "variant": variant}
            for pair, seed, variant in cell_order()
        ]
        manifest = {
            "schema": "ndnsf-di-spec111-non-regression-campaign-v1",
            "candidateId": candidate["candidateId"],
            "candidateSha256": sha256_file(CANDIDATE_PATH),
            "baselineCommit": BASE_COMMIT,
            "baselineWorktree": str(BASELINE_ROOT),
            "baselineNativeExtensionSha256": sha256_file(next(
                (BASELINE_ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so")
            )),
            "treatmentNativeExtensionSha256": candidate["localBuild"][
                "nativeExtensionSha256"
            ],
            "baselinePythonPathShim": "Experiments/ndnsf -> ../pythonWrapper/ndnsf",
            "baselineMeasurementPatchPaths": list(
                BASELINE_MEASUREMENT_PATCH_PATHS),
            "baselineMeasurementPatchSha256": sha256_bytes(
                subprocess.check_output([
                    "git", "-C", str(BASELINE_ROOT), "diff", "--",
                    *BASELINE_MEASUREMENT_PATCH_PATHS,
                ])
            ),
            "recipeSha256": sha256_file(ROOT / (
                "specs/111-ndnsf-di-core-app-separation/evidence/"
                "performance-baseline-recipe.md"
            )),
            "orderedCells": order,
            "orderedCellsSha256": canonical_digest(order),
            "roleImportPreflight": role_import_preflight,
            "runtimeReadinessPreflight": runtime_readiness,
            "noAutomaticRerun": True,
            "containerRuntimeInvocations": 0,
            "slurmSubmissions": 0,
        }
        if manifest_path.exists():
            existing = json.loads(manifest_path.read_text(encoding="utf-8"))
            if existing != manifest:
                if not args.continue_diagnostic_after_terminal_failure:
                    raise SystemExit("SPEC111_CAMPAIGN_MANIFEST_CHANGED")
                if (
                    existing.get("orderedCellsSha256") !=
                    manifest.get("orderedCellsSha256") or
                    existing.get("baselineCommit") != manifest.get("baselineCommit")
                ):
                    raise SystemExit(
                        "SPEC111_DIAGNOSTIC_CONTINUATION_INVARIANTS_CHANGED")
        else:
            write_json(manifest_path, manifest)
        diagnostic_marker = None
        if args.continue_diagnostic_after_terminal_failure:
            diagnostic_marker = write_diagnostic_continuation_marker(
                campaign_root, candidate)
        results = []
        for pair, seed, variant in cell_order():
            result = run_cell(campaign_root, pair, seed, variant)
            results.append(result)
            print(
                "SPEC111_CELL_TERMINAL",
                result["cellId"], result["status"],
                f"completed={result.get('completedRequests', 0)}",
                flush=True,
            )
            accept_terminal_cell(
                result,
                continue_diagnostic_after_failure=(
                    args.continue_diagnostic_after_terminal_failure
                ),
            )
        summary = {
            "schema": "ndnsf-di-spec111-campaign-summary-v1",
            "candidateId": candidate["candidateId"],
            "formalComparisonEligible": (
                diagnostic_marker is None and
                all(item.get("status") == "PASS" for item in results)
            ),
            "diagnosticContinuation": diagnostic_marker,
            "cellCount": len(results),
            "passCount": sum(item.get("status") == "PASS" for item in results),
            "failedObservedCount": sum(item.get("status") != "PASS" for item in results),
            "results": results,
        }
        write_json(campaign_root / "campaign-summary.json", summary)
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
