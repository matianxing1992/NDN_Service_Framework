#!/usr/bin/env python3
"""Subprocess lifecycle campaign for the reported OpenABE/RELIC exit crash."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


REPO = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = (REPO.parent / "NAC-ABE/build-tests/tests/unit-tests").resolve()
ROLES = ("Controller", "Provider", "User")
SANITIZER_MARKERS = (
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "runtime error:",
    "LeakSanitizer",
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_probe(
    binary: Path,
    *,
    cycle: int,
    role: str,
    controlled: bool,
    ready_root: Path,
    timeout_seconds: float,
) -> dict[str, object]:
    ready_file = ready_root / f"cycle-{cycle:03d}-{role.lower()}.ready"
    env = dict(os.environ)
    env["SPEC112_LIFECYCLE_ROLE"] = role
    if controlled:
        env["SPEC112_LIFECYCLE_CONTROLLED"] = "1"
        env["SPEC112_LIFECYCLE_READY_FILE"] = str(ready_file)
    else:
        env.pop("SPEC112_LIFECYCLE_CONTROLLED", None)
        env.pop("SPEC112_LIFECYCLE_READY_FILE", None)

    command = [
        str(binary),
        "--run_test=TestAbeSupport/ProcessLifecycleProbe",
        "--log_level=message",
        "--report_level=no",
    ]
    started = time.monotonic()
    process = subprocess.Popen(
        command,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    timed_out = False
    control_signal_sent = False
    try:
        if controlled:
            ready_deadline = time.monotonic() + timeout_seconds / 2.0
            while process.poll() is None and not ready_file.exists():
                if time.monotonic() >= ready_deadline:
                    raise subprocess.TimeoutExpired(command, timeout_seconds / 2.0)
                time.sleep(0.002)
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                control_signal_sent = True
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        stdout, stderr = process.communicate(timeout=2)

    return_code = process.returncode
    combined = stdout + "\n" + stderr
    sanitizer_finding = any(marker in combined for marker in SANITIZER_MARKERS)
    expected_marker = f"NAC_ABE_LIFECYCLE_OK role={role}"
    nac_abe_used = expected_marker in combined
    signal_number = -return_code if return_code is not None and return_code < 0 else 0
    return {
        "cycle": cycle,
        "role": role,
        "shutdownCause": "controlled" if controlled else "normal",
        "command": command,
        "nacAbeUsed": nac_abe_used,
        "controlSignalSent": control_signal_sent,
        "exitCode": return_code if return_code is not None and return_code >= 0 else None,
        "signal": signal_number,
        "timedOut": timed_out,
        "sanitizerEnabled": bool(env.get("ASAN_OPTIONS") or env.get("UBSAN_OPTIONS")),
        "sanitizerFinding": sanitizer_finding,
        "elapsedMs": round((time.monotonic() - started) * 1000.0, 3),
        "stdoutSha256": hashlib.sha256(stdout.encode("utf-8")).hexdigest(),
        "stderrSha256": hashlib.sha256(stderr.encode("utf-8")).hexdigest(),
        "success": (
            return_code == 0
            and not timed_out
            and not sanitizer_finding
            and nac_abe_used
            and (not controlled or control_signal_sent)
        ),
    }


def run_campaign(
    binary: Path,
    *,
    cycles: int,
    output: Path,
    timeout_seconds: float,
) -> dict[str, object]:
    if cycles < 1:
        raise ValueError("cycles must be positive")
    binary = binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)
    output = output.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite lifecycle evidence: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    ready_root = output.parent / (output.stem + "-ready")
    ready_root.mkdir(parents=True, exist_ok=False)
    records: list[dict[str, object]] = []
    campaign_started = time.monotonic()
    for cycle in range(1, cycles + 1):
        for role_index, role in enumerate(ROLES):
            records.append(
                run_probe(
                    binary,
                    cycle=cycle,
                    role=role,
                    controlled=(cycle + role_index) % 2 == 0,
                    ready_root=ready_root,
                    timeout_seconds=timeout_seconds,
                )
            )

    failures = [record for record in records if not record["success"]]
    summary = {
        "schemaVersion": "spec112-nac-abe-lifecycle-v1",
        "binary": str(binary),
        "binarySha256": _sha256(binary),
        "cycles": cycles,
        "roleExitCount": len(records),
        "normalExitCount": sum(record["shutdownCause"] == "normal" for record in records),
        "controlledExitCount": sum(record["shutdownCause"] == "controlled" for record in records),
        "successfulExitCount": len(records) - len(failures),
        "failureCount": len(failures),
        "sigsegvCount": sum(record["signal"] == signal.SIGSEGV for record in records),
        "sigabrtCount": sum(record["signal"] == signal.SIGABRT for record in records),
        "timeoutCount": sum(bool(record["timedOut"]) for record in records),
        "sanitizerFindingCount": sum(bool(record["sanitizerFinding"]) for record in records),
        "elapsedSeconds": round(time.monotonic() - campaign_started, 3),
        "records": records,
    }
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, output)
    return summary


class Spec112NacAbeExitTest(unittest.TestCase):
    def test_one_initialized_cycle_records_all_three_roles(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "lifecycle.json"
            summary = run_campaign(
                DEFAULT_BINARY,
                cycles=1,
                output=output,
                timeout_seconds=5.0,
            )
            self.assertEqual(summary["schemaVersion"], "spec112-nac-abe-lifecycle-v1")
            self.assertEqual(summary["roleExitCount"], 3)
            self.assertEqual(summary["successfulExitCount"], 3)
            self.assertEqual(summary["failureCount"], 0)
            self.assertEqual({record["role"] for record in summary["records"]}, set(ROLES))
            self.assertEqual({record["shutdownCause"] for record in summary["records"]},
                             {"normal", "controlled"})
            with self.assertRaises(FileExistsError):
                run_campaign(
                    DEFAULT_BINARY,
                    cycles=1,
                    output=output,
                    timeout_seconds=5.0,
                )


def _campaign_main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-campaign", action="store_true", required=True)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-s", type=float, default=5.0)
    args = parser.parse_args(argv)
    summary = run_campaign(
        args.binary,
        cycles=args.cycles,
        output=args.output,
        timeout_seconds=args.timeout_s,
    )
    print(json.dumps({key: value for key, value in summary.items() if key != "records"},
                     sort_keys=True))
    return 0 if summary["failureCount"] == 0 else 1


if __name__ == "__main__":
    if "--run-campaign" in sys.argv:
        raise SystemExit(_campaign_main(sys.argv[1:]))
    unittest.main()
