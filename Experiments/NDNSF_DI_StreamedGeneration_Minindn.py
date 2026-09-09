#!/usr/bin/env python3
"""Spec175 four-Provider MiniNDN qualification entry point.

This wrapper freezes the M01--M14 input contract and delegates the real host
CPU network launch to the existing production runner.  It uses the checked-in
tiny ONNX fixture; Qwen/SIF/Tiger inputs are intentionally not required here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
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

_TIMELINE_FIELD_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^ ]+)")
_SENSITIVE_TIMELINE_FIELD_RE = re.compile(
    r"(?:^|_)(?:payload|prompt(?:text|bytes)?|answer(?:text|bytes)?|"
    r"logits?|token(?:key|payload|bytes)?|secret|private|"
    r"key(?:bytes|material)?|content(?:bytes)?|"
    r"state(?:tensor|payload|bytes)?|tensor(?:data|payload|bytes)?)(?:$|_)",
    re.IGNORECASE)


def write_request_lifecycle_evidence(output_dir: Path) -> dict[str, object]:
    """Merge bounded ndn-cxx TimelineTrace records from every node log.

    The original per-process logs remain authoritative.  This derivative is a
    correlation aid: it contains names, state and timing only, never payload,
    token, key, certificate private material, or application input bytes.
    """
    events: list[dict[str, object]] = []
    event_counts: dict[str, int] = {}
    request_ids: set[str] = set()
    for log_path in sorted(output_dir.glob("*.log")):
        for line_number, line in enumerate(
                log_path.read_text(errors="replace").splitlines(), start=1):
            markers = (
                ("timeline", line.find("NDNSF_TIMELINE ")),
                ("control", line.find("NDNSF_CONTROL_TIMING ")),
            )
            channel, marker = next(
                ((kind, index) for kind, index in markers if index >= 0),
                ("", -1),
            )
            if marker < 0:
                continue
            fields = dict(_TIMELINE_FIELD_RE.findall(line[marker:]))
            required = {"role", "event", "timestamp_us", "steady_us", "requestId"}
            if not required.issubset(fields):
                continue
            event_name = fields.pop("event")
            request_id = fields.pop("requestId")
            role = fields.pop("role")
            timestamp_us = int(fields.pop("timestamp_us"))
            steady_us = int(fields.pop("steady_us"))
            fields = {
                key: value for key, value in fields.items()
                if not _SENSITIVE_TIMELINE_FIELD_RE.search(key)
            }
            event_counts[event_name] = event_counts.get(event_name, 0) + 1
            request_ids.add(request_id)
            events.append({
                "timestampUs": timestamp_us,
                "steadyUs": steady_us,
                "role": role,
                "event": event_name,
                "requestId": request_id,
                "sourceLog": log_path.name,
                "sourceLine": line_number,
                "channel": channel,
                "fields": fields,
            })
    events.sort(key=lambda row: (
        int(row["timestampUs"]), str(row["sourceLog"]), int(row["sourceLine"])))
    lifecycle_path = output_dir / "spec175-request-lifecycle.jsonl"
    lifecycle_path.write_text(
        "".join(json.dumps(event, sort_keys=True) + "\n" for event in events),
        encoding="utf-8",
    )
    digest = hashlib.sha256(lifecycle_path.read_bytes()).hexdigest()
    return {
        "schema": "ndnsf-di-spec175-request-lifecycle-summary-v1",
        "status": "PASS" if events else "EMPTY",
        "path": str(lifecycle_path.resolve()),
        "sha256": "sha256:" + digest,
        "eventCount": len(events),
        "requestCount": len(request_ids),
        "eventCounts": dict(sorted(event_counts.items())),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=tuple(CASES), required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument(
        "--output-dir",
        default=os.environ.get("SPEC180_CASE_OUTPUT_DIR", ""),
        help=(
            "Case evidence directory. Spec180's isolated runner supplies "
            "SPEC180_CASE_OUTPUT_DIR per child; direct runs must pass this "
            "option explicitly."),
    )
    parser.add_argument(
        "--tiny-fixture-root",
        default=str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"),
    )
    parser.add_argument("--qwen-content-store", default="")
    parser.add_argument("--qwen-model", default="Qwen/Qwen3.6-27B")
    parser.add_argument(
        "--qwen-revision",
        default="6a9e13bd6fc8f0983b9b99948120bc37f49c13e9",
    )
    parser.add_argument("--prompt", default="")
    parser.add_argument("--workload-digest", default="")
    parser.add_argument("--model-identity-digest", default="")
    parser.add_argument("--request-id", default="")
    parser.add_argument(
        "--native-requester-config", default="",
        help="Operator-pinned native requester config forwarded to the maintained runner.",
    )
    parser.add_argument("--runtime-sif", default=os.environ.get("SPEC175_RUNTIME_SIF", ""))
    parser.add_argument("--runtime-apptainer", default=os.environ.get("SPEC175_APPTAINER", ""))
    parser.add_argument("--topology-file", default=str(
        ROOT / "Experiments/Topology/spec175-host-gate.conf"))
    parser.add_argument("--dry-run", action="store_true")
    return parser


def qualification_command(args: argparse.Namespace) -> list[str]:
    """Return the only permitted production-runner command for a case."""
    timeout_ms = "2500" if args.case == "M08" else "60000"
    command = [
        sys.executable, str(RUNNER),
        "--topology-file", args.topology_file,
        # Preparation-only stabilization: let NLSR converge before starting
        # the repository/provider bootstrap.  Stream deadlines remain owned
        # by the fixed case contract below.
        "--nlsr-wait-s", "15",
        "--output-dir", args.output_dir,
        "--stages", "4",
        "--runtime", "tiny-onnx",
        "--tiny-onnx-fixture-root", args.tiny_fixture_root,
        "--max-new-tokens", "8",
        "--measured-requests", "1",
        "--seed", str(args.seed),
        "--warmup-requests", "0",
        "--request-id", args.request_id or f"spec175-{args.case}-{args.seed}",
        "--campaign-id", f"spec175-{args.case}-{args.seed}",
        "--spec175-case", args.case,
        "--ack-timeout-ms", "1500",
        "--timeout-ms", timeout_ms,
        "--selection-dataflow-v3",
        "--app-state-root", str(Path(args.output_dir) / "app-state"),
        "--provider-start-timeout-s", "120",
        # The route snapshot proves FIB/strategy installation. This fixed,
        # measurement-excluded window lets the last Provider exchange initial
        # SVS state before the one registered business Request is published.
        "--initial-sync-settle-s", "5",
    ]
    if args.runtime_sif:
        command += ["--runtime-sif", args.runtime_sif]
    if args.runtime_apptainer:
        command += ["--runtime-apptainer", args.runtime_apptainer]
    if args.prompt:
        command += ["--prompt", args.prompt]
    if args.workload_digest:
        command += ["--workload-digest", args.workload_digest]
    if args.model_identity_digest:
        command += ["--model-identity-digest", args.model_identity_digest]
    if args.native_requester_config:
        command += ["--native-requester-config", args.native_requester_config]
    return command


def main() -> int:
    args = build_parser().parse_args()
    # Admission control is intentionally not an argument.  This campaign's
    # contract is disabled admission; adding a flag here would permit silently
    # changing the subject between M-cases.
    if args.seed <= 0:
        raise SystemExit("seed must be positive")
    if not args.output_dir:
        raise SystemExit("--output-dir or SPEC180_CASE_OUTPUT_DIR is required")
    for label, path in (("tiny ONNX fixture", args.tiny_fixture_root),):
        if not Path(path).expanduser().exists():
            raise SystemExit(f"missing {label}: {path}")
    frozen_topology = (
        ROOT / "Experiments/Topology/spec175-host-gate.conf").resolve()
    if Path(args.topology_file).expanduser().resolve() != frozen_topology:
        raise SystemExit(
            "Spec175 G3 requires the frozen 100-Mbit/10-ms star topology")
    command = qualification_command(args)
    if args.dry_run:
        print(json.dumps({
            "case": args.case, "fault": CASES[args.case], "seed": args.seed,
            "admissionControl": False, "providerCount": 4,
            "command": command,
        }, indent=2, sort_keys=True))
        return 0
    if os.environ.get("SPEC175_RUN_REAL_MININDN") != "1":
        raise SystemExit(
            "set SPEC175_RUN_REAL_MININDN=1 for the exclusive real MiniNDN gate")
    completed = subprocess.run(command, cwd=ROOT, check=False)
    lifecycle = write_request_lifecycle_evidence(Path(args.output_dir))
    if completed.returncode != 0:
        return int(completed.returncode)
    result_path = Path(args.output_dir) / "spec175-case-result.json"
    if not result_path.is_file():
        raise SystemExit(
            f"production runner returned success without case evidence: {result_path}")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("status") != "PASS" or result.get("case") != args.case:
        raise SystemExit(
            "production runner emitted invalid case evidence: "
            f"status={result.get('status')} case={result.get('case')}")
    terminal = result.get("terminalEvidence")
    if (not isinstance(terminal, dict) or
            terminal.get("schema") != "ndnsf-di-spec175-terminal-evidence-v1" or
            terminal.get("status") != "PASS" or
            terminal.get("resultWrittenAfterProcessExit") is not True or
            terminal.get("abortObserved") is not False or
            terminal.get("unexpectedSignalExits") != {} or
            terminal.get("survivingOwnedProcesses") != []):
        raise SystemExit(
            "production runner emitted incomplete terminal process evidence")
    if lifecycle["status"] != "PASS":
        raise SystemExit(
            "production runner emitted no structured request lifecycle evidence")
    result["requestLifecycle"] = lifecycle
    lifecycle_artifact = {
        "path": Path(str(lifecycle["path"])).name,
        "sha256": lifecycle["sha256"],
        "bytes": Path(str(lifecycle["path"])).stat().st_size,
    }
    artifacts = result.setdefault("artifacts", [])
    if not any(row.get("path") == lifecycle_artifact["path"]
               for row in artifacts if isinstance(row, dict)):
        artifacts.append(lifecycle_artifact)
    result_path.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "NDNSF_DI_SPEC175_MININDN_WRAPPER_PASS "
        f"case={args.case} seed={args.seed} result={result_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
