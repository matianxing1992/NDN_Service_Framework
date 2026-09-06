#!/usr/bin/env python3
"""Host-orchestrated Spec180 exact-SIF replay driver.

MiniNDN, Mininet, Open vSwitch, and host routing helpers stay on the host.
This driver never imports them into the SIF and never runs a host NDNSF
application as a substitute.  The production runner must consume the
``SPEC180_RUNTIME_SIF`` / ``SPEC180_RUNTIME_APPTAINER`` command-provider
contract before this driver can report a replay result.

The driver runs exactly one Y-B request: the sealed candidate inputs are
bound into the SIF (data trees only), every NFD and application child runs
inside the image, and the result oracle is the runner's
``SPEC180_CASE_RESULT status=PASS case=Y-B`` terminal marker plus the
lifecycle journal.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[4]
RUNNER = ROOT / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
REQUIRED_RUNNER_CONTRACT = (
    "SPEC180_RUNTIME_SIF", "sif_exec_prefix", "--cleanenv",
    "SPEC180_SIF_HOST_PROCESS_FALLBACK", "Spec180SifNfd",
)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def fail(message: str) -> None:
    raise SystemExit("SPEC180_EXACT_SIF_REPLAY_" + message)


def verify_sif(apptainer: str, sif: Path, expected_sha256: str) -> None:
    if not Path(apptainer).is_file():
        fail("APPTAINER_MISSING")
    version = subprocess.run(
        [apptainer, "version"], text=True, capture_output=True,
        check=False).stdout.strip()
    if not version.startswith("1.5.3"):
        fail(f"APPTAINER_VERSION_MISMATCH:{version}")
    if not sif.is_file():
        fail("SIF_MISSING")
    observed = digest(sif)
    expected = expected_sha256
    if expected.startswith("sha256:"):
        expected = expected[7:]
    if observed[7:] != expected:
        fail(f"SIF_DIGEST_MISMATCH:{observed}")


def verify_runner_contract() -> None:
    if not RUNNER.is_file():
        fail("RUNNER_MISSING")
    source = RUNNER.read_text(encoding="utf-8")
    missing = [marker for marker in REQUIRED_RUNNER_CONTRACT
               if marker not in source]
    if missing:
        fail("RUNNER_CONTRACT_MISSING:" + ",".join(missing))


def render_environment(case_environment: Path) -> dict[str, str]:
    """Load the case environment allowlist from a JSON file."""
    value = json.loads(case_environment.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or not value:
        fail("CASE_ENVIRONMENT_INVALID")
    return {str(key): str(item) for key, item in value.items()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apptainer", required=True)
    parser.add_argument("--sif", required=True, type=Path)
    parser.add_argument("--sif-sha256", required=True)
    parser.add_argument("--case-environment", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args(argv)

    verify_sif(args.apptainer, args.sif, args.sif_sha256)
    verify_runner_contract()

    output = args.output_root.resolve()
    if output.exists():
        try:
            if any(output.iterdir()):
                fail("OUTPUT_ROOT_NOT_EMPTY")
        except OSError as exc:
            fail("OUTPUT_ROOT_UNREADABLE:" + str(exc))
    else:
        output.mkdir(parents=True)

    environment = render_environment(args.case_environment)
    environment["SPEC180_RUNTIME_SIF"] = str(args.sif.resolve())
    environment["SPEC180_RUNTIME_APPTAINER"] = str(args.apptainer)

    record = {
        "schema": "spec180-exact-sif-replay-v1",
        "runner": str(RUNNER),
        "runnerSha256": digest(RUNNER),
        "sif": str(args.sif.resolve()),
        "sifSha256": args.sif_sha256,
        "apptainer": args.apptainer,
        "case": "Y-B",
        "outputRoot": str(output),
    }
    (output / "replay-record.json").write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")

    command = [
        sys.executable, str(RUNNER), "--case", "Y-B",
    ]
    completed = subprocess.run(
        command, cwd=str(ROOT), env={**os.environ, **environment},
        text=True, capture_output=True, check=False)
    stdout = completed.stdout or ""
    (output / "runner-stdout.log").write_text(stdout, encoding="utf-8")
    (output / "runner-stderr.log").write_text(
        completed.stderr or "", encoding="utf-8")
    marker = "SPEC180_CASE_RESULT status=PASS case=Y-B"
    if completed.returncode != 0 or marker not in stdout:
        fail(
            f"CASE_ORACLE_MISSING:exit={completed.returncode}")
    print("SPEC180_EXACT_SIF_REPLAY status=PASS case=Y-B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
