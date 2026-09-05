#!/usr/bin/env python3
"""Isolate PIB/TPM before loading the readiness binary and its static KeyChains.

Usage: python3 tests/standalone/run-service-controller-readiness.py BINARY [CASE|--real-nfd]
Private run directories are retained; no existing key store is read or removed.
"""

import argparse
import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("test_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Linux is required for parent-death process cleanup")
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("binary must be an existing executable; this launcher never compiles")

    root = Path(tempfile.mkdtemp(prefix="ndnsf-controller-readiness-", dir="/tmp"))
    env = os.environ.copy()
    env.update({
        "NDN_CLIENT_PIB": "pib-sqlite3:" + str(root / "pib"),
        "NDN_CLIENT_TPM": "tpm-file:" + str(root / "tpm"),
        "NDN_CLIENT_TRANSPORT": "unix://" + str(root / "nfd.sock"),
        "NDNSF_CONTROLLER_READINESS_ROOT": str(root),
        "NDNSF_CONTROLLER_READINESS_LAUNCHER_PID": str(os.getpid()),
    })
    print("TEST_ARTIFACT_DIR=" + str(root), flush=True)
    parent_pid = os.getpid()
    libc = ctypes.CDLL(None, use_errno=True)

    def parent_death_guard():
        # Single-threaded launcher; before exec and before any NDNSF DSO loads.
        if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0 or os.getppid() != parent_pid:
            os._exit(126)

    def interrupted(signum, _frame):
        raise InterruptedError(signum)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    child = None
    try:
        with (root / "test.log").open("x") as log:
            child = subprocess.Popen(
                [str(binary), *args.test_args], env=env, stdout=log,
                stderr=subprocess.STDOUT, preexec_fn=parent_death_guard,
            )
            status = child.wait(timeout=40)
            return status if status >= 0 else 128 - status
    except subprocess.TimeoutExpired:
        print("FAIL launcher: test exceeded 40 seconds", file=sys.stderr)
        return 124
    except InterruptedError as error:
        return 128 + error.args[0]
    finally:
        # Popen owns this exact child. Never signal a group or search by name.
        # The real-NFD test also attaches its own child to its parent's lifetime.
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=2)
        log_path = root / "test.log"
        if log_path.exists():
            for line in log_path.read_text(errors="replace").splitlines():
                if line.startswith(("TEST_", "REAL_NFD_", "PASS ", "FAIL ")) or "tests passed" in line:
                    print(line)
        print("Full log: " + str(log_path), flush=True)


if __name__ == "__main__":
    sys.exit(main())
