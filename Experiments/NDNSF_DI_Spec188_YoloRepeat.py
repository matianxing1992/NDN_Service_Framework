#!/usr/bin/env python3
"""Run two candidate-bound Spec188 YOLO MiniNDN requests.

This is orchestration only.  The maintained Spec187 runner owns MiniNDN/NFD,
process barriers and input validation; the ``spec188-yolo-repeat`` executable
owns the native request/ACK/Selection/Provider/response assertions.  The
wrapper changes only the run output and state roots between repetitions, so
the model, profile, reference and selector identity stay fixed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "Experiments" / "NDNSF_DI_YoloAckDriven_Minindn.py"


def _file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _run_identity(run_root: Path) -> dict[str, object]:
    descriptor = json.loads((run_root / "case-input.json").read_text(
        encoding="utf-8"))
    # These are the candidate/model/profile/reference bindings emitted by the
    # maintained runner.  Exclude output and state paths, which intentionally
    # differ between repetitions, while retaining selector inode/digest and
    # every native request/reference digest.
    fields = (
        "case", "packageManifestSha256", "canonicalModelSha256",
        "catalogueRegistrySha256", "catalogueDataName", "catalogueSigner",
        "topologySha256", "configSha256", "casePolicySha256",
        "nativeAuthorityConfigSha256", "nativeAuthorityReferencedFiles",
        "nativeSelectorBinding", "nativeRequestFileBindings",
        "nativeRequesterReferencedFiles", "candidateIds",
    )
    identity = {field: descriptor.get(field) for field in fields}
    identity["casePlanSha256"] = _file_digest(run_root / "case-plan.json")
    return identity


def _write_summary(root: Path, status: str, runs: list[dict[str, object]],
                   *, identity: dict[str, object] | None = None) -> None:
    payload: dict[str, object] = {
        "schema": "spec188-yolo-repeat-v1",
        "status": status,
        "runs": runs,
    }
    if identity is not None:
        payload["candidateIdentity"] = identity
    (root / "repeat-summary.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")


def _run_bounded(command: list[str], env: dict[str, str]) -> tuple[int | None, str, str, bool]:
    """Run one case with a process-group bound and deterministic teardown."""
    try:
        child = subprocess.Popen(command, cwd=ROOT, env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, start_new_session=True)
    except OSError as exc:
        return None, "", str(exc), False
    try:
        stdout, stderr = child.communicate(timeout=300)
        return child.returncode, stdout, stderr, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = child.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = child.communicate()
        return child.returncode, stdout, stderr, True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selector", required=True,
                        help="absolute executable path for spec188-yolo-repeat")
    parser.add_argument("--repeat-root", required=True,
                        help="empty output parent under results/")
    args = parser.parse_args(argv)

    selector = Path(args.selector).expanduser()
    repeat_root = Path(args.repeat_root).expanduser()
    if not selector.is_absolute() or selector.is_symlink() \
            or not selector.is_file() or not os.access(selector, os.X_OK):
        print("SPEC188_REPEAT_RESULT status=WAITING_EXTERNAL_INPUT "
              "error=SELECTOR_INVALID", flush=True)
        return 78
    if not repeat_root.is_absolute() or repeat_root.is_symlink():
        print("SPEC188_REPEAT_RESULT status=WAITING_EXTERNAL_INPUT "
              "error=REPEAT_ROOT_INVALID", flush=True)
        return 78
    repeat_root = repeat_root.resolve()
    results_root = (ROOT / "results").resolve()
    try:
        repeat_root.relative_to(results_root)
    except ValueError:
        print("SPEC188_REPEAT_RESULT status=WAITING_EXTERNAL_INPUT "
              "error=REPEAT_ROOT_OUTSIDE_RESULTS", flush=True)
        return 78
    if repeat_root.exists():
        if repeat_root.stat().st_uid != os.geteuid():
            print("SPEC188_REPEAT_RESULT status=WAITING_EXTERNAL_INPUT "
                  "error=REPEAT_ROOT_OWNER_MISMATCH", flush=True)
            return 78
        try:
            if any(repeat_root.iterdir()):
                print("SPEC188_REPEAT_RESULT status=WAITING_EXTERNAL_INPUT "
                      "error=REPEAT_ROOT_NOT_EMPTY", flush=True)
                return 78
        except OSError:
            print("SPEC188_REPEAT_RESULT status=WAITING_EXTERNAL_INPUT "
                  "error=REPEAT_ROOT_UNREADABLE", flush=True)
            return 78
    else:
        repeat_root.mkdir(parents=True, exist_ok=False)

    base = os.environ.copy()
    base["SPEC187_NATIVE_MODE"] = "1"
    base["SPEC187_NATIVE_SELECTOR"] = str(selector)
    base["SPEC187_NATIVE_SUITE"] = "Spec188YoloRepeat"
    base["SPEC187_NATIVE_CASE"] = "NativeRequesterThroughMiniNdn"
    records: list[dict[str, object]] = []
    for index in (1, 2):
        run_id = f"run-{index}"
        run_root = repeat_root / run_id
        run_root.mkdir(exist_ok=False)
        env = base.copy()
        env["SPEC180_CASE_OUTPUT_DIR"] = str(run_root)
        env["SPEC187_NATIVE_REQUEST_OUTPUT"] = str(run_root / "native-result.bin")
        raw_state_root = env.get("SPEC188_REPEAT_STATE_ROOT", "").strip()
        if raw_state_root:
            state_root = Path(raw_state_root).expanduser()
        else:
            state_root = Path.home() / ".local" / "state" / "ndnsf" / "spec188" / repeat_root.name
        env["NDNSF_DI_STATE_ROOT"] = str(state_root / run_id)
        # ControllerGenerationStore uses an exclusive lock beside its state
        # file.  A failed child may leave that lease as a durable failure
        # boundary, so repetitions must not share it.  Keep the generation
        # state under the already-created run root, alongside the run's other
        # identity-bound artifacts.
        env["NDNSF_CONTROLLER_GENERATION_STATE"] = str(
            run_root / "controller-generation.state")
        command = [sys.executable, str(RUNNER), "--case", "Y-A"]
        started = time.monotonic()
        returncode, stdout, stderr, timed_out = _run_bounded(command, env)
        elapsed = time.monotonic() - started
        (run_root / "launcher.stdout.log").write_text(
            stdout, encoding="utf-8")
        (run_root / "launcher.stderr.log").write_text(
            stderr, encoding="utf-8")
        terminal = run_root / "terminal-result.txt"
        child_exits = run_root / "child-exits.json"
        record = {
            "runId": run_id,
            "exit": returncode,
            "elapsedSeconds": round(elapsed, 3),
            "timedOut": timed_out,
            "terminalResult": terminal.is_file(),
            "childExits": child_exits.is_file(),
            "nativeResult": (run_root / "native-result.bin").is_file(),
        }
        child_status = "PASS"
        for line in stdout.splitlines():
            if line.startswith("SPEC180_CASE_RESULT status="):
                child_status = line.split("status=", 1)[1].split()[0]
        record["childStatus"] = child_status
        records.append(record)
        if returncode != 0 or returncode is None or timed_out or not all(
                record[key] for key in ("terminalResult", "childExits", "nativeResult")):
            status = ("WAITING_EXTERNAL_INPUT"
                      if child_status == "WAITING_EXTERNAL_INPUT"
                      or returncode == 78 else "UNQUALIFIED")
            _write_summary(repeat_root, status, records)
            print("SPEC188_REPEAT_RESULT status=" + status + " run=" + run_id,
                  flush=True)
            return 78 if status == "WAITING_EXTERNAL_INPUT" else returncode or 2
        try:
            identity = _run_identity(run_root)
        except (OSError, ValueError, KeyError, json.JSONDecodeError):
            _write_summary(repeat_root, "UNQUALIFIED", records)
            print("SPEC188_REPEAT_RESULT status=UNQUALIFIED run=" + run_id,
                  flush=True)
            return 2
        if index == 1:
            first_identity = identity
        elif identity != first_identity:
            record["identityMismatch"] = True
            _write_summary(repeat_root, "UNQUALIFIED", records,
                           identity=first_identity)
            print("SPEC188_REPEAT_RESULT status=UNQUALIFIED "
                  "error=CANDIDATE_IDENTITY_CHANGED", flush=True)
            return 2

    _write_summary(repeat_root, "PASS", records, identity=first_identity)
    print("SPEC188_REPEAT_RESULT status=PASS runs=2", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
