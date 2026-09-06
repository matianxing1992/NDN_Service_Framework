#!/usr/bin/env python3
"""Host-orchestrated Spec175 exact-SIF replay driver.

MiniNDN, Mininet, Open vSwitch, and host routing helpers stay on the host.
This driver never imports them into the SIF and never runs a host NDNSF
application as a substitute. The production runner must consume the
``SPEC175_RUNTIME_SIF``/``SPEC175_APPTAINER`` command-provider contract before
this driver can report a replay result.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[4]
RUNNER = ROOT / "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py"
PRODUCTION_RUNNER = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
TOPOLOGY = ROOT / "Experiments/Topology/spec175-host-gate.conf"
HOST_PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight"
WORKLOAD_SEED = 1750001
FAULT_SEED = 1750002
FAULT_CASES = frozenset({"M05", "M06", "M07", "M08", "M09"})
# These are part of the replay subject.  They are deliberately constants, not
# ambient environment overrides, so a timeout change creates a new candidate.
CASE_WATCHDOG_SECONDS = 900
CAMPAIGN_WATCHDOG_SECONDS = 14400
PROCESS_TERM_GRACE_SECONDS = 30


class CampaignInterrupted(RuntimeError):
    """Raised after the driver has started a bounded terminal shutdown."""

    def __init__(self, signum: int):
        super().__init__(f"signal:{signum}")
        self.signum = signum


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def without_sha256_prefix(value: str) -> str:
    """Keep the host replay driver compatible with Python 3.8 MiniNDN."""
    return value[7:] if value.startswith("sha256:") else value


def case_seed(case: str, workload_seed: int) -> int:
    """Apply the frozen G3/G4 seed contract to every case.

    ``--seed`` names the healthy workload seed.  Fault cases deliberately use
    their separate registered seed so an exact-SIF replay cannot silently
    turn M05--M09 into healthy runs.
    """
    return FAULT_SEED if case in FAULT_CASES else workload_seed


def fail(message: str) -> None:
    raise SystemExit("SPEC175_G4_REPLAY_" + message)


def ensure_fresh_run_output(path: Path, run_id: str) -> None:
    """Never allow a replay to reuse evidence from an earlier run."""
    if path.exists():
        if not path.is_dir():
            fail(f"RUN_OUTPUT_NOT_DIRECTORY:{run_id}")
        try:
            has_entries = any(path.iterdir())
        except OSError as exc:
            fail(f"RUN_OUTPUT_UNREADABLE:{run_id}:{type(exc).__name__}")
        if has_entries:
            fail(f"RUN_OUTPUT_NOT_EMPTY:{run_id}")
        return
    path.mkdir(parents=True, exist_ok=False)


def resolve_replay_output(path: Path) -> Path:
    """Resolve the aggregate manifest before deriving per-case output paths.

    The production runner starts inside the SIF at ``/opt/ndnsf-di/replay/repo``.
    A relative ``--output-dir`` therefore points into the read-only image rather
    than at the host directory bind-mounted for the case.  G4 must pass only
    absolute host paths to the SIF-owned processes so journals and evidence are
    written to the bound run directory.
    """
    return path.expanduser().resolve()


def load_host_gate():
    path = ROOT / "packaging/ndnsf-di-container/lib/spec175_host_gate.py"
    spec = importlib.util.spec_from_file_location("spec175_host_gate", path)
    if spec is None or spec.loader is None:
        fail("HOST_GATE_VALIDATOR_UNAVAILABLE")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_manifest(path: Path, sif: Path) -> dict:
    if os.geteuid() != 0:
        fail("REQUIRES_ROOT_FOR_MININDN")
    if not RUNNER.is_file() or not TOPOLOGY.is_file():
        fail("HOST_INPUT_MISSING")
    if not sif.is_file() or sif.stat().st_size == 0:
        fail("SIF_MISSING")
    try:
        result = load_host_gate().validate_host_gate(path, ROOT)
    except Exception as exc:
        fail("HOST_GATE_NOT_PASS:" + str(exc))
    expected_entries = result.get("total")
    if (not isinstance(expected_entries, int) or expected_entries < 1 or
            result.get("passed") != expected_entries or
            not isinstance(result.get("entries"), list) or
            len(result["entries"]) != expected_entries):
        fail("HOST_GATE_ENTRY_SET_INVALID")
    return result


def inspect_sif(apptainer: str, sif: Path, expected: str) -> dict:
    actual = digest(sif)
    if expected and without_sha256_prefix(actual) != without_sha256_prefix(expected):
        fail("SIF_DIGEST_MISMATCH")
    check = subprocess.run([apptainer, "inspect", "--json", str(sif)],
                           text=True, capture_output=True, check=False)
    if check.returncode != 0:
        fail("SIF_INSPECT_FAILED")
    try:
        labels = json.loads(check.stdout)["data"]["attributes"]["labels"]
    except (KeyError, json.JSONDecodeError) as exc:
        fail("SIF_LABELS_INVALID:" + type(exc).__name__)
    if labels.get("org.ndnsf.di.build-boundary") != "container-runtime-in-sif":
        fail("SIF_BUILD_BOUNDARY_INVALID")
    return {"sha256": actual, "bytes": sif.stat().st_size, "labels": labels}


def verify_source_identity(host: dict, sif_info: dict, source_path: Path) -> None:
    """Verify the candidate SIF's complete seal after host-G3 overlap checks."""
    try:
        source = json.loads(source_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail("SOURCE_SEAL_INVALID:" + type(exc).__name__)
    seal_digest = source.get("sealDigest")
    image_digest = sif_info.get("labels", {}).get("org.ndnsf.di.source-seal")
    if not seal_digest or image_digest != seal_digest:
        fail("SOURCE_SEAL_SIF_MISMATCH")


def valid_terminal_case_result(path: Path, case: str) -> tuple[bool, str]:
    """Require fresh, reaped child evidence before accepting one replay."""
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        return False, "CASE_RESULT_UNREADABLE:" + type(exc).__name__
    if (result.get("schema") != "ndnsf-di-spec175-minindn-case-result-v1" or
            result.get("status") != "PASS" or result.get("case") != case):
        return False, "CASE_RESULT_INVALID"
    terminal = result.get("terminalEvidence")
    if not isinstance(terminal, dict):
        return False, "TERMINAL_EVIDENCE_MISSING"
    if terminal.get("schema") != "ndnsf-di-spec175-terminal-evidence-v1":
        return False, "TERMINAL_EVIDENCE_SCHEMA_INVALID"
    if (terminal.get("status") != "PASS" or
            terminal.get("resultWrittenAfterProcessExit") is not True or
            terminal.get("abortObserved") is not False or
            terminal.get("unexpectedSignalExits") != {} or
            terminal.get("survivingOwnedProcesses") != []):
        return False, "TERMINAL_EVIDENCE_NOT_CLOSED"
    exits = terminal.get("childExitCodes")
    if (not isinstance(exits, dict) or not exits or
            any(not isinstance(value, int) for value in exits.values())):
        return False, "TERMINAL_EXIT_CODES_INCOMPLETE"
    return True, ""


def _stream_text(value) -> str:
    """Normalize ``Popen`` output for both text and Python 3.8 paths."""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def terminate_process_group(process: subprocess.Popen) -> dict:
    """Reap one case and all of its descendants within a bounded grace period."""
    signals_sent = []
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            signals_sent.append("SIGTERM")
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=PROCESS_TERM_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                signals_sent.append("SIGKILL")
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
    return {"signals": signals_sent, "returncode": process.returncode}


def provider_readiness(output: Path) -> dict:
    """Summarize machine-readable Provider readiness without trusting log order."""
    ready = []
    logs = []
    for path in sorted(output.glob("stage*-provider.log")):
        logs.append(str(path))
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "LLM_PIPELINE_PROVIDER_READY" in text:
            ready.append(path.name)
    return {
        "readyCount": len(ready),
        "readyProviderLogs": ready,
        "providerLogPaths": logs,
        "allFourReady": len(ready) == 4,
    }


def run_case(command: list[str], output: Path, run_id: str, env: dict,
             campaign_deadline: float, case: str) -> dict:
    """Run one isolated case and always produce a bounded driver-side record."""
    ensure_fresh_run_output(output, run_id)
    started = time.monotonic()
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    stdout = ""
    stderr = ""
    returncode = None
    failure_reason = ""
    shutdown = {"signals": [], "returncode": None}
    try:
        remaining = max(1.0, campaign_deadline - time.monotonic())
        timeout = min(float(CASE_WATCHDOG_SECONDS), remaining)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
            returncode = process.returncode
        except subprocess.TimeoutExpired as exc:
            shutdown = terminate_process_group(process)
            tail_stdout, tail_stderr = process.communicate()
            stdout = _stream_text(tail_stdout) or _stream_text(exc.stdout)
            stderr = _stream_text(tail_stderr) or _stream_text(exc.stderr)
            returncode = 124
            failure_reason = (
                "CAMPAIGN_WATCHDOG_TIMEOUT" if remaining <= timeout
                else "CASE_WATCHDOG_TIMEOUT"
            )
    except CampaignInterrupted:
        shutdown = terminate_process_group(process)
        try:
            tail_stdout, tail_stderr = process.communicate()
        except (OSError, subprocess.SubprocessError):
            tail_stdout, tail_stderr = "", ""
        (output / "g4.stdout").write_text(_stream_text(tail_stdout), encoding="utf-8")
        (output / "g4.stderr").write_text(_stream_text(tail_stderr), encoding="utf-8")
        raise
    finally:
        if process.poll() is None:
            shutdown = terminate_process_group(process)
    (output / "g4.stdout").write_text(_stream_text(stdout), encoding="utf-8")
    (output / "g4.stderr").write_text(_stream_text(stderr), encoding="utf-8")
    result_path = output / "spec175-case-result.json"
    passed = returncode == 0 and result_path.is_file()
    if passed:
        passed, failure_reason = valid_terminal_case_result(result_path, case)
    readiness = provider_readiness(output)
    if passed and not readiness["allFourReady"]:
        passed = False
        failure_reason = "ALL_FOUR_PROVIDER_READINESS_MISSING"
    if not passed and not failure_reason:
        failure_reason = "CASE_RESULT_MISSING" if returncode == 0 else "CASE_PROCESS_FAILED"
    return {
        "returncode": returncode,
        "status": "PASS" if passed else "FAIL",
        "failureReason": failure_reason,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "resultPath": str(result_path),
        "providerReadiness": readiness,
        "driverShutdown": shutdown,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-gate-manifest", required=True, type=Path)
    parser.add_argument("--source-seal", required=True, type=Path,
                        help="complete candidate source seal embedded in the SIF")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sif", type=Path,
                        default=Path(os.environ.get("SPEC175_RUNTIME_SIF", "")))
    parser.add_argument("--apptainer",
                        default=os.environ.get("SPEC175_APPTAINER", ""))
    parser.add_argument("--sif-sha256", default=os.environ.get("SPEC175_SIF_SHA256", ""))
    parser.add_argument("--seed", type=int, default=WORKLOAD_SEED,
                        help="healthy workload seed; M05-M09 use the fixed fault seed 1750002")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if not args.apptainer:
        fail("APPTAINER_REQUIRED")
    if not args.sif:
        fail("SIF_REQUIRED")
    output_manifest = resolve_replay_output(args.output)
    if output_manifest.exists():
        fail("OUTPUT_MANIFEST_EXISTS")

    host = load_manifest(args.host_gate_manifest.resolve(), args.sif.resolve())
    sif = inspect_sif(args.apptainer, args.sif.resolve(), args.sif_sha256)
    verify_source_identity(host, sif, args.source_seal.resolve())
    host_preflight = output_manifest.parent / "host-substrate-preflight.json"
    preflight = subprocess.run([
        str(HOST_PREFLIGHT),
        "--host-gate-manifest", str(args.host_gate_manifest.resolve()),
        "--repository-root", str(ROOT),
        "--topology-file", str(TOPOLOGY),
        "--replay-driver", str(Path(__file__).resolve()),
        "--apptainer", args.apptainer,
        "--expected-apptainer", "1.5.3",
        "--output", str(host_preflight),
    ], text=True, capture_output=True, check=False)
    if preflight.returncode != 0:
        fail("HOST_SUBSTRATE_PREFLIGHT_FAILED")

    source = PRODUCTION_RUNNER.read_text(encoding="utf-8")
    required_runtime_contract = (
        "SPEC175_RUNTIME_SIF", "sif_exec_prefix", "--cleanenv",
        "SPEC175_SIF_HOST_PROCESS_FALLBACK",
    )
    if not PRODUCTION_RUNNER.is_file() or any(
            marker not in source for marker in required_runtime_contract):
        fail("PRODUCTION_RUNNER_HAS_NO_SIF_COMMAND_PROVIDER")

    entries = []
    host_entries = host["entries"]
    for index, registered in enumerate(host_entries):
        case = registered.get("case") if isinstance(registered, dict) else None
        repetition = registered.get("repetition", index + 1) \
            if isinstance(registered, dict) else index + 1
        if not isinstance(case, str) or not isinstance(repetition, int):
            fail("HOST_GATE_ENTRY_INVALID")
        run_id = registered.get("runId", f"{case}-r{repetition}") \
            if isinstance(registered, dict) else f"{case}-r{repetition}"
        if (not isinstance(run_id, str) or not run_id or
                Path(run_id).name != run_id):
            fail("HOST_GATE_RUN_ID_INVALID")
        output = output_manifest.parent / run_id
        registered_seed = registered.get("seed") if isinstance(registered, dict) else None
        seed = registered_seed if isinstance(registered_seed, int) else case_seed(case, args.seed)
        command = [
            sys.executable, str(RUNNER),
            "--case", case,
            "--seed", str(seed),
            "--output-dir", str(output),
            "--tiny-fixture-root", str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"),
            "--topology-file", str(TOPOLOGY),
            "--runtime-sif", str(args.sif.resolve()),
            "--runtime-apptainer", args.apptainer,
        ]
        entries.append({"case": case, "repetition": repetition,
                        "runId": run_id, "command": command,
                        "seed": seed,
                        "outputDir": str(output),
                        "runtimeSif": str(args.sif.resolve()),
                        "apptainer": args.apptainer,
                        "status": "NOT_RUN",
                        "failureReason": "PENDING"})

    campaign_started = time.monotonic()
    campaign_deadline = campaign_started + CAMPAIGN_WATCHDOG_SECONDS
    campaign_termination = "completed"
    previous_handlers = {}

    def interrupt_handler(signum, _frame):
        raise CampaignInterrupted(signum)

    if not args.dry_run:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[signum] = signal.signal(signum, interrupt_handler)
    try:
        if not args.dry_run:
            env = dict(os.environ)
            env.update({
                "SPEC175_RUN_REAL_MININDN": "1",
                "SPEC175_RUNTIME_SIF": str(args.sif.resolve()),
                "SPEC175_APPTAINER": args.apptainer,
                "SPEC175_SIF_SHA256": sif["sha256"],
            })
            for entry in entries:
                if time.monotonic() >= campaign_deadline:
                    campaign_termination = "campaign-watchdog"
                    break
                report = run_case(
                    entry["command"], Path(entry["outputDir"]), entry["runId"],
                    env, campaign_deadline, entry["case"])
                entry.update(report)
    except CampaignInterrupted as exc:
        campaign_termination = f"signal:{exc.signum}"
        first_pending = next(
            (entry for entry in entries if entry.get("status") == "NOT_RUN"),
            None,
        )
        if first_pending is not None:
            first_pending["failureReason"] = "CAMPAIGN_INTERRUPTED"
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)

    if campaign_termination != "completed":
        pending_seen = False
        for entry in entries:
            if entry.get("status") == "NOT_RUN":
                if not pending_seen:
                    pending_seen = True
                    if entry.get("failureReason") == "PENDING":
                        entry["failureReason"] = campaign_termination
                else:
                    entry["failureReason"] = "NOT_RUN_AFTER_FIRST_INCOMPLETE"

    passed = sum(entry.get("status") == "PASS" for entry in entries)
    completed = sum(entry.get("status") in {"PASS", "FAIL"}
                    for entry in entries)
    not_run = [entry["runId"] for entry in entries
               if entry.get("status") == "NOT_RUN"]
    first_incomplete = next(
        (entry["runId"] for entry in entries
         if entry.get("status") != "PASS"),
        None,
    )
    result = {
        "schema": "spec175-g4-host-orchestrated-replay-v1",
        "status": "PASS" if not args.dry_run and entries and
        passed == len(entries) else "FAIL",
        "layer": "host-substrate-plus-exact-sif-runtime",
        "repetitionsPerCase": host.get("repetitionsPerCase"),
        "workloadSeed": host.get("subject", {}).get("workloadSeed", args.seed),
        "faultSeed": host.get("faultSeed", FAULT_SEED),
        "faultCases": host.get("faultCases", sorted(FAULT_CASES)),
        "caseWatchdogSeconds": CASE_WATCHDOG_SECONDS,
        "campaignWatchdogSeconds": CAMPAIGN_WATCHDOG_SECONDS,
        "campaignTermination": campaign_termination,
        "completedEntries": completed,
        "passedEntries": passed,
        "notRunEntries": not_run,
        "firstIncomplete": first_incomplete,
        "hostGate": host,
        "sourceSeal": {"path": str(args.source_seal.resolve()),
                        "sha256": digest(args.source_seal.resolve())},
        "hostPreflight": str(host_preflight),
        "sif": sif,
        "entries": entries,
    }
    output_manifest.parent.mkdir(parents=True, exist_ok=True)
    output_manifest.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
    print(json.dumps({"status": result["status"], "output": str(output_manifest)}, sort_keys=True))
    return 0 if result["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
