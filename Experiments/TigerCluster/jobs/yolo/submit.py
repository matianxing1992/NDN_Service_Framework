#!/usr/bin/env python3
"""Spec183 operator entrypoint; no execution until genuine gates are wired.

`check` is read-only. Exit 78 means incomplete qualification, not a launchable
candidate. The remaining contract commands are deliberately unavailable until
their real application and receipt consumers exist; there is no force bypass.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
BUNDLE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BUNDLE))
from runtime.yolo_profile import ClosureError, check_operator_profile, resolve_run_plan


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    check = commands.add_parser("check", help="read-only content check; not runtime qualification")
    check.add_argument("--profile", type=Path, required=True)
    check.add_argument("--stage", choices=("inputs", "runtime", "dispatch"), default="dispatch")
    check.add_argument("--run-id", help="optional run preview; requires --output and --case")
    check.add_argument("--output", type=Path)
    check.add_argument("--case", choices=("local-cpu", "single-node-gpu", "two-node-gpu", "negative-dependency"))
    args = parser.parse_args(argv)
    try:
        preview = (args.run_id is not None, args.output is not None, args.case is not None)
        if any(preview) and not all(preview):
            raise ClosureError("RUN_PREVIEW_OPTIONS")
        plan = (resolve_run_plan(args.profile, stage=args.stage, case=args.case,
                                 run_id=args.run_id, output=args.output) if all(preview) else None)
        report = check_operator_profile(args.profile, stage=args.stage)
        if plan is not None:
            if plan["documentDigest"] != report["documentDigest"]:
                raise ClosureError("PROFILE_CHANGED_DURING_CHECK")
            report["runPlan"] = plan
    except ClosureError as exc:
        print(json.dumps({"status": "REJECTED", "qualification": "NOT_EVALUATED",
                          "reason": str(exc)}, sort_keys=True))
        return 2
    print(json.dumps(report, sort_keys=True))
    return 78


if __name__ == "__main__":
    sys.exit(main())
