#!/usr/bin/env python3
"""Execute the Spec181-owned local-suite inventory in isolated child processes.

This runner is intentionally separate from MiniNDN case semantics.  It
validates the complete candidate-bound inventory first, then supervises every
entry independently.  A missing/stale entry, child crash, timeout, cleanup
failure, or redaction finding makes the aggregate result ``UNQUALIFIED``.
No aggregate-process result can replace an entry record.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Any, Mapping, Sequence

from spec180_inventory import DEFAULT_CASES, InventoryError, validate_inventory


ROOT = Path(__file__).resolve().parents[1]
RESULT_SCHEMA = "spec180-local-qualification-v1"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")

# These are deliberately conservative evidence-leak markers.  Matching text
# is replaced before logs are persisted; the raw child output is never written
# to the evidence directory.
_REDACTION_PATTERNS = (
    re.compile(
        r"-----BEGIN [^-]+-----.*?-----END [^-]+-----",
        re.IGNORECASE | re.DOTALL),
    re.compile(
        r"\b(?:private[_ -]?key|secret|capability|password|plaintext|"
        r"input_bytes|result_bytes)\s*[:=]\s*[^\s,;]+", re.IGNORECASE),
    re.compile(r"\b(?:private[_ -]?key|secret|capability|password)\b",
               re.IGNORECASE),
    re.compile(r"\b(?:plaintext|input_bytes|result_bytes)\s*=", re.IGNORECASE),
)


class LocalGateError(ValueError):
    """Raised before execution when the local-gate contract is invalid."""


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")


def canonical_digest(value: Any) -> str:
    return digest_bytes(canonical_bytes(value))


def _load_json(path: Path) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LocalGateError("INVENTORY_READ_FAILED:" + str(path)) from exc
    if not isinstance(value, Mapping):
        raise LocalGateError("INVENTORY_NOT_OBJECT")
    return value


def _require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise LocalGateError("INVALID_DIGEST:" + label)
    return value


def _entry_source_path(root: Path, entry: Mapping[str, Any]) -> Path:
    path = str(entry["path"])
    resolved = (root / path).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise LocalGateError("ENTRY_PATH_ESCAPES_ROOT:" + str(entry["id"])) from exc
    if not resolved.is_file():
        raise LocalGateError("ENTRY_FILE_MISSING:" + str(entry["id"]))
    return resolved


def _entry_digest_matches(root: Path, entry: Mapping[str, Any]) -> None:
    path = _entry_source_path(root, entry)
    expected = _require_digest(entry.get("artifactSha256"),
                               "entry.artifactSha256")
    try:
        actual = digest_bytes(path.read_bytes())
    except OSError as exc:
        raise LocalGateError("ENTRY_FILE_READ_FAILED:" + str(entry["id"])) from exc
    if actual != expected:
        raise LocalGateError("ENTRY_FILE_DIGEST_MISMATCH:" + str(entry["id"]))


def _validate_entry_command(root: Path, entry: Mapping[str, Any]) -> None:
    """Ensure the command is the inventory-declared source-bound shape."""
    command = entry["command"]
    kind = entry["kind"]
    path = entry["path"]
    if kind in {"cpp-suite", "cpp-selector"}:
        if command[0] != path:
            raise LocalGateError("ENTRY_COMMAND_PATH_MISMATCH:" + str(entry["id"]))
        executable = (root / path).resolve()
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise LocalGateError("ENTRY_COMMAND_EXECUTABLE_MISSING:" + str(entry["id"]))
        if kind == "cpp-suite":
            expected = [path, "--log_level=nothing"]
        else:
            selector = entry.get("selector")
            if not isinstance(selector, str) or not selector:
                raise LocalGateError("CPP_SELECTOR_MISSING:" + str(entry["id"]))
            expected = [path, "--run_test=" + selector, "--log_level=nothing"]
        if command != expected:
            raise LocalGateError("ENTRY_COMMAND_SHAPE_MISMATCH:" + str(entry["id"]))
        return
    # Bind the exact interpreter spelling recorded by the inventory.  A
    # symlink (for example /usr/bin/python3 -> python3.8) is still part of the
    # candidate's toolchain contract; silently normalizing it would invalidate
    # an otherwise matching inventory.
    interpreter = sys.executable
    if kind == "python-selector":
        selector = entry.get("selector")
        expected = [interpreter, "-m", "pytest", "-q", str(selector)]
    else:
        case = entry.get("case")
        registered = {item[0]: tuple(item[2]) for item in DEFAULT_CASES}
        if case not in registered:
            raise LocalGateError("CASE_NOT_REGISTERED:" + str(entry["id"]))
        expected = [interpreter, path, *registered[case]]
    if command != expected:
        raise LocalGateError("ENTRY_COMMAND_SHAPE_MISMATCH:" + str(entry["id"]))
    if kind == "minindn-case" and command[1] != path:
        raise LocalGateError("CASE_COMMAND_PATH_MISMATCH:" + str(entry["id"]))


def _redact(text: str) -> tuple[str, bool]:
    redacted = text
    found = False
    for pattern in _REDACTION_PATTERNS:
        redacted, count = pattern.subn("<REDACTED>", redacted)
        found = found or count > 0
    return redacted, found


def _case_oracle_marker(case: str) -> str:
    """Return the exact success marker emitted by the registered entrypoint."""
    return "SPEC180_CASE_RESULT status=PASS case=" + case


def _write_text(path: Path, text: str) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")
    return digest_bytes(text.encode("utf-8"))


def _write_json(path: Path, value: Mapping[str, Any]) -> str:
    """Write a canonical evidence snapshot and return its digest."""
    encoded = json.dumps(
        value, ensure_ascii=False, sort_keys=True, indent=2,
    ) + "\n"
    _write_text(path, encoded)
    return digest_bytes(encoded.encode("utf-8"))


def _terminate(proc: subprocess.Popen[str], *, grace_seconds: float = 2.0
               ) -> tuple[bool, int | None, int | None]:
    """Stop a child process group and return (clean, signal, exit_code)."""
    timed_out = False
    if proc.poll() is None:
        timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=grace_seconds)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait(timeout=grace_seconds)
    code = proc.returncode
    return not timed_out, (-code if code is not None and code < 0 else None), code


def _group_alive(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _terminate_group(pgid: int, *, grace_seconds: float = 2.0) -> bool:
    """Terminate descendants left behind after a child leader exits."""
    if not _group_alive(pgid):
        return True
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return True
    deadline = time.monotonic() + grace_seconds
    while _group_alive(pgid) and time.monotonic() < deadline:
        time.sleep(0.02)
    if _group_alive(pgid):
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            return True
        deadline = time.monotonic() + grace_seconds
        while _group_alive(pgid) and time.monotonic() < deadline:
            time.sleep(0.02)
    return not _group_alive(pgid)


def _run_entry(root: Path, entry: Mapping[str, Any], output_root: Path,
               environment: Mapping[str, str]) -> dict[str, Any]:
    entry_id = str(entry["id"])
    command = [str(item) for item in entry["command"]]
    timeout = float(entry["timeoutSeconds"])
    evidence_dir = output_root / entry_id
    stdout_path = evidence_dir / "stdout.log"
    stderr_path = evidence_dir / "stderr.log"
    started = time.time()
    timed_out = False
    signal_number: int | None = None
    exit_code: int | None = None
    launch_error = ""
    stdout = ""
    stderr = ""
    proc: subprocess.Popen[str] | None = None
    pid: int | None = None
    child_environment = dict(environment)
    if entry.get("kind") == "minindn-case":
        case_output = evidence_dir / "case-output"
        # The supervisor owns creation of the case root.  The case runner must
        # reject a missing root, so a typo cannot silently create evidence in
        # an unintended path.  It is created once, before the child starts,
        # and remains empty at launch.
        evidence_dir.mkdir(parents=True, exist_ok=True)
        try:
            case_output.mkdir()
        except FileExistsError as exc:
            raise LocalGateError("CASE_OUTPUT_ROOT_NOT_FRESH:" + entry_id) from exc
        child_environment["SPEC180_CASE_OUTPUT_DIR"] = str(case_output)
    try:
        proc = subprocess.Popen(
            command,
            cwd=str(root),
            env=child_environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        pid = proc.pid
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
            exit_code = proc.returncode
            if exit_code is not None and exit_code < 0:
                signal_number = -exit_code
        except subprocess.TimeoutExpired as exc:
            timed_out = True
            stdout = exc.stdout or ""
            stderr = exc.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode("utf-8", errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode("utf-8", errors="replace")
            _terminate(proc)
            exit_code = proc.returncode
            if exit_code is not None and exit_code < 0:
                signal_number = -exit_code
            stderr += "\nSPEC180_CHILD_TIMEOUT\n"
    except OSError as exc:
        launch_error = str(exc)
        exit_code = 127
    finally:
        if proc is not None and proc.poll() is None:
            _terminate(proc)
            if proc.stdout is not None or proc.stderr is not None:
                try:
                    extra_out, extra_err = proc.communicate(timeout=2)
                except Exception:
                    extra_out, extra_err = "", ""
                stdout += extra_out or ""
                stderr += extra_err or ""

    group_cleanup_ok = True
    if proc is not None and pid is not None:
        group_cleanup_ok = _terminate_group(pid)

    stdout, stdout_redacted = _redact(str(stdout))
    stderr, stderr_redacted = _redact(str(stderr))
    if launch_error:
        stderr += "\nSPEC180_CHILD_LAUNCH_ERROR:" + launch_error + "\n"
    stdout_hash = _write_text(stdout_path, stdout)
    stderr_hash = _write_text(stderr_path, stderr)
    cleanup_ok = (proc is None or proc.poll() is not None) and group_cleanup_ok
    status = "PASS"
    if timed_out:
        status = "TIMEOUT"
    elif exit_code != 0:
        status = "FAIL"
    if stdout_redacted or stderr_redacted:
        status = "REDACTION_FAIL"
    if not cleanup_ok:
        status = "CLEANUP_FAIL"
    # Formal MiniNDN cases must provide an explicit case-level oracle.  A
    # process exit of zero alone is not enough to call a protocol case passed.
    if status == "PASS" and entry.get("kind") == "minindn-case":
        marker = _case_oracle_marker(str(entry["case"]))
        if marker not in stdout and marker not in stderr:
            status = "CASE_ORACLE_MISSING"
    ended = time.time()
    return {
        "id": entry_id,
        "kind": entry["kind"],
        "case": entry.get("case"),
        "command": command,
        "commandDigest": entry["commandDigest"],
        "pid": pid,
        "startedAtUnix": started,
        "endedAtUnix": ended,
        "durationSeconds": round(max(0.0, ended - started), 3),
        "exitCode": exit_code,
        "signal": signal_number,
        "timedOut": timed_out,
        "timeoutSeconds": timeout,
        "stdoutPath": str(stdout_path),
        "stderrPath": str(stderr_path),
        "stdoutSha256": stdout_hash,
        "stderrSha256": stderr_hash,
        "redaction": "PASS" if not (stdout_redacted or stderr_redacted) else "FAIL",
        "cleanup": "PASS" if cleanup_ok else "FAIL",
        "oracle": (_case_oracle_marker(str(entry["case"]))
                   if entry.get("kind") == "minindn-case" else "exit-code-zero"),
        "status": status,
    }


def _validate_complete_inventory(inventory: Mapping[str, Any]) -> None:
    try:
        validate_inventory(inventory)
    except InventoryError as exc:
        raise LocalGateError(str(exc)) from exc
    entries = inventory["entries"]
    cases = {entry.get("case") for entry in entries
             if entry.get("kind") == "minindn-case"}
    required = {case for case, _path, _args in DEFAULT_CASES}
    if cases != required:
        raise LocalGateError("MININDN_CASE_SET_INCOMPLETE")
    if not any(entry.get("kind") == "cpp-suite" for entry in entries):
        raise LocalGateError("CPP_UNIT_SUITE_MISSING")
    if not any(entry.get("kind") == "cpp-selector" for entry in entries):
        raise LocalGateError("CPP_SELECTOR_SET_EMPTY")
    if not any(entry.get("kind") == "python-selector" for entry in entries):
        raise LocalGateError("PYTEST_SELECTOR_SET_EMPTY")


def run_local_gate(inventory: Mapping[str, Any], *, root: Path | str,
                   output_root: Path | str,
                   environment: Mapping[str, str]) -> dict[str, Any]:
    """Validate and execute every inventory item in its own child."""
    _validate_complete_inventory(inventory)
    root_path = Path(root).resolve()
    output_path = Path(output_root).resolve()
    if not output_path.is_absolute():  # pragma: no cover - resolve is absolute
        raise LocalGateError("OUTPUT_ROOT_NOT_ABSOLUTE")
    if any(not isinstance(key, str) or not isinstance(value, str)
           for key, value in environment.items()):
        raise LocalGateError("ENVIRONMENT_MUST_BE_STRING_MAP")
    if output_path.exists():
        try:
            if any(output_path.iterdir()):
                raise LocalGateError("OUTPUT_ROOT_NOT_EMPTY")
        except OSError as exc:
            raise LocalGateError("OUTPUT_ROOT_UNREADABLE") from exc
    # Validate command shapes and all source hashes before starting the first child.  This keeps
    # a partially executed matrix from being mistaken for a complete one.
    for entry in inventory["entries"]:
        _validate_entry_command(root_path, entry)
        _entry_digest_matches(root_path, entry)
    output_path.mkdir(parents=True, exist_ok=True)
    inventory_path = output_path / "inventory.json"
    inventory_file_digest = _write_json(inventory_path, inventory)
    results = []
    for entry in inventory["entries"]:
        results.append(_run_entry(root_path, entry, output_path, environment))
    failed = [item for item in results if item["status"] != "PASS"]
    result: dict[str, Any] = {
        "schema": RESULT_SCHEMA,
        "status": "PASS" if not failed else "UNQUALIFIED",
        "candidateId": inventory["candidateId"],
        "candidateDigest": inventory["candidateDigest"],
        "sourceRevision": inventory["sourceRevision"],
        "effectiveConfigDigest": inventory["effectiveConfigDigest"],
        "inventoryDigest": inventory["inventoryDigest"],
        "inventoryPath": str(inventory_path),
        "inventoryFileSha256": inventory_file_digest,
        "backend": inventory["backend"],
        "entryCount": len(results),
        "failedEntryIds": [item["id"] for item in failed],
        "entries": results,
        "cleanup": "PASS" if all(item["cleanup"] == "PASS" for item in results) else "FAIL",
        "redaction": "PASS" if all(item["redaction"] == "PASS" for item in results) else "FAIL",
    }
    return result


def _write_result(path: Path, result: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--environment-json", type=Path, required=True,
                        help="explicit JSON string map; ambient environment is never inherited")
    args = parser.parse_args(argv)
    try:
        inventory = _load_json(args.inventory)
        environment_value = _load_json(args.environment_json)
        if not isinstance(environment_value, Mapping):
            raise LocalGateError("ENVIRONMENT_JSON_NOT_OBJECT")
        if any(not isinstance(key, str) or not isinstance(value, str)
               for key, value in environment_value.items()):
            raise LocalGateError("ENVIRONMENT_JSON_VALUES_MUST_BE_STRINGS")
        result = run_local_gate(
            inventory, root=args.root, output_root=args.output_root,
            environment=dict(environment_value),
        )
        output = args.output_root / "local-qualification.json"
        _write_result(output, result)
    except (InventoryError, LocalGateError, OSError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 78
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0 if result["status"] == "PASS" else 3


if __name__ == "__main__":
    raise SystemExit(main())
