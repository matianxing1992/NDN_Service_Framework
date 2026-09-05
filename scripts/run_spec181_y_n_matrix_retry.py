#!/usr/bin/env python3
"""Retired compatibility entry point for the unsound per-subcase retry driver.

Use the maintained Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-N
entry after the active design audit passes, inside the documented isolated
runtime and with a fresh output directory. Diagnose and retain each failure
before starting another run. This shim intentionally imports no runtime,
starts no processes, changes no files, and never issues a matrix verdict.
"""


def main() -> int:
    print("SPEC181_MATRIX_DRIVER_RESULT status=UNQUALIFIED "
          "reason=LEGACY_RETRY_DRIVER_DISABLED", flush=True)
    print("Use Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-N "
          "after T007 PASS, with the documented isolation and a fresh run-id. "
          "Preserve and diagnose failures before any new run.", flush=True)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
