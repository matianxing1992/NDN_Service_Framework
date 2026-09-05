#!/usr/bin/env python3
"""Run the spec181 Y-N matrix subcase-by-subcase with bounded retries.

The MiniNDN Controller startup has a known transient race (the spec180
r39-era face-transport race, reduced but not eliminated by the startup
drain).  This driver runs each subcase in an isolated fresh output
directory and retries up to N times so a transient startup failure does
not lose a whole matrix run.  Each attempt is a real, barriered MiniNDN
run through the maintained runner; the matrix-level verdict remains the
runner's own y-n-matrix-result.json.

Usage (inside the unshare boundary):
    unshare -Urnm python3 scripts/run_spec181_y_n_matrix_retry.py \
        --output-root /tmp/spec181-y-n-matrix \
        [--max-attempts 3]
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Experiments"))

import NDNSF_DI_YoloAckDriven_Minindn as runner  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", required=True)
    parser.add_argument("--max-attempts", type=int, default=3)
    args = parser.parse_args()

    root = Path(args.output_root)
    results: dict[str, dict] = {}
    for subcase in runner.YN_SUBCASES:
        passed = False
        last: dict = {}
        for attempt in range(1, args.max_attempts + 1):
            # Stale /run/nfd sockets from an earlier MiniNDN instance make
            # NFD fail with "Address already in use" before any child starts;
            # clear them per attempt (inside the user-namespace boundary).
            shutil.rmtree("/run/nfd", ignore_errors=True)
            target = root / subcase / f"attempt-{attempt}"
            if target.exists():
                shutil.rmtree(target)
            target.mkdir(parents=True)
            env = dict(os.environ)
            env["SPEC180_CASE_OUTPUT_DIR"] = str(target)
            try:
                output, inputs = runner.validate_inputs("Y-N", env)
                runner._run_live_case_once(
                    "Y-N", output, inputs, subcase=subcase)
                result_path = target / "subcase-result.json"
                if result_path.is_file():
                    result = json.loads(result_path.read_text())
                    if (result.get("status") == "PASS"
                            and result.get("outcome") in
                            ("CONTROL", "FAIL_CLOSED")):
                        passed = True
                        last = result
                        break
                    last = result
                else:
                    last = {"status": "NO_RESULT"}
            except Exception as exc:
                last = {"status": "ATTEMPT_FAILED", "error": str(exc)}
                print(f"SPEC181_MATRIX_ATTEMPT subcase={subcase} "
                      f"attempt={attempt} error={exc}", flush=True)
        results[subcase] = {
            "passed": passed,
            "attempts": attempt,
            "last": last,
        }
        print(f"SPEC181_MATRIX_SUBCASE subcase={subcase} "
              f"passed={passed} attempts={attempt}", flush=True)

    matrix = {
        "schema": "spec181-yn-matrix-retry-driver-v1",
        "subcases": results,
    }
    (root / "retry-driver-result.json").write_text(
        json.dumps(matrix, sort_keys=True, indent=2) + "\n",
        encoding="utf-8")
    all_passed = all(item["passed"] for item in results.values())
    print(f"SPEC181_MATRIX_DRIVER_RESULT all_passed={all_passed}",
          flush=True)
    return 0 if all_passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
