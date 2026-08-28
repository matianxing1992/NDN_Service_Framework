#!/usr/bin/env python3
"""Spec175 four-Provider MiniNDN qualification entry point.

This wrapper freezes the M01--M14 input contract and delegates the real host
CPU network launch to the existing production runner.  It uses the checked-in
tiny ONNX fixture; Qwen/SIF/Tiger inputs are intentionally not required here.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
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


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=tuple(CASES), required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--output-dir", required=True)
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
    return command


def main() -> int:
    args = build_parser().parse_args()
    # Admission control is intentionally not an argument.  This campaign's
    # contract is disabled admission; adding a flag here would permit silently
    # changing the subject between M-cases.
    if args.seed <= 0:
        raise SystemExit("seed must be positive")
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
    print(
        "NDNSF_DI_SPEC175_MININDN_WRAPPER_PASS "
        f"case={args.case} seed={args.seed} result={result_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
