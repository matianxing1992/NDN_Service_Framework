#!/usr/bin/env python3
"""Run the bounded Spec175 Python compatibility gate.

The repository contains historical Spec127--Spec173 result/source checks that
are useful diagnostics but are not a promotion gate for Spec175.  This runner
keeps the G1 subject explicit: Spec175 Python contracts, the streamed facade,
and the shared DI contract tests.  The native unit/integration binaries and
the real MiniNDN matrix are separate gates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
SCOPED_TESTS = (
    ROOT / "tests/python/test_streamed_invocation_api.py",
    ROOT / "tests/python/test_ndnsf_di_core_contracts.py",
)


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _test_paths() -> list[Path]:
    return sorted(ROOT.glob("tests/python/test_spec175_*.py")) + list(SCOPED_TESTS)


def _counts(output: str) -> dict[str, int]:
    counts = {key: 0 for key in ("passed", "failed", "skipped", "xfailed", "xpassed")}
    for key in counts:
        match = re.search(rf"(\d+) {key}", output)
        if match:
            counts[key] = int(match.group(1))
    return counts


def _run(command: list[str], *, env: dict[str, str] | None = None) -> dict[str, object]:
    process = subprocess.run(
        command, cwd=ROOT, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    return {
        "command": command,
        "returnCode": process.returncode,
        "output": process.stdout,
    }


def _artifact_record(path: Path) -> dict[str, object]:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        return {
            "path": str(resolved),
            "available": False,
            "ready": False,
            "sha256": None,
            "lddReturnCode": None,
            "lddOutput": "",
            "unresolvedLibraries": [],
        }
    ldd = subprocess.run(
        ["ldd", str(resolved)], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    unresolved = [
        line.strip() for line in ldd.stdout.splitlines() if "not found" in line
    ]
    return {
        "path": str(resolved),
        "available": True,
        "ready": ldd.returncode == 0 and not unresolved,
        "sha256": _sha256(resolved),
        "lddReturnCode": ldd.returncode,
        "lddOutput": ldd.stdout,
        "unresolvedLibraries": unresolved,
    }


def _default_python_extension() -> Path:
    candidates = sorted((ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))
    if len(candidates) != 1:
        names = ",".join(str(path) for path in candidates) or "none"
        raise SystemExit(
            "SPEC175_G1_PYTHON_EXTENSION_CANDIDATES=" + names)
    return candidates[0]


def _tool_version(command: list[str]) -> dict[str, object]:
    record = _run(command)
    record["output"] = str(record["output"]).splitlines()[0] if record["output"] else ""
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-seal", type=Path, required=True)
    parser.add_argument("--pytest", default=sys.executable)
    parser.add_argument("--unit-binary", type=Path, default=ROOT / "build/unit-tests")
    parser.add_argument("--python-extension", type=Path)
    args = parser.parse_args()
    tests = _test_paths()
    missing = [str(path) for path in tests if not path.is_file()]
    if missing:
        raise SystemExit("SPEC175_PYTHON_GATE_MISSING_TEST=" + ",".join(missing))
    env = dict(os.environ)
    entries = [
        str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
        str(ROOT / "NDNSF-DistributedInference"),
        str(ROOT / "pythonWrapper"),
    ]
    existing = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = ":".join(entries + ([existing] if existing else []))
    python_command = [
        args.pytest, "-m", "pytest", "-q", "--tb=short",
        *(str(path.relative_to(ROOT)) for path in tests),
    ]
    unit_binary = args.unit_binary.expanduser().resolve()
    python_extension = (args.python_extension.expanduser().resolve()
                        if args.python_extension else _default_python_extension())
    native_unit = _artifact_record(unit_binary)
    extension = _artifact_record(python_extension)
    unit_command = [str(unit_binary), "--log_level=message"]
    import_command = [
        sys.executable,
        "-c",
        "import ndnsf._ndnsf as module; print(module.__file__)",
    ]
    started = time.monotonic()
    unit_result = (_run(unit_command, env=env) if native_unit["available"] else {
        "command": unit_command, "returnCode": 127, "output": "unit binary unavailable",
    })
    import_result = _run(import_command, env=env)
    python_result = _run(python_command, env=env)
    output = str(python_result["output"])
    counts = _counts(output)
    seal = args.source_seal.expanduser().resolve()
    if not seal.is_file():
        raise SystemExit(f"SPEC175_PYTHON_GATE_SOURCE_SEAL_MISSING={seal}")
    source_seal = {"path": str(seal), "sha256": _sha256(seal)}
    blocking_issues = []
    if not native_unit["ready"]:
        blocking_issues.append("native-unit-artifact-not-ready")
    if not extension["ready"]:
        blocking_issues.append("python-extension-artifact-not-ready")
    if int(unit_result["returnCode"]) != 0:
        blocking_issues.append("native-unit-suite-failed")
    if int(import_result["returnCode"]) != 0:
        blocking_issues.append("python-extension-import-failed")
    if int(python_result["returnCode"]) != 0:
        blocking_issues.append("python-subject-failed")
    record = {
        "schemaVersion": "spec175-g1-host-python-gate-v2",
        "status": "PASS" if not blocking_issues else "BLOCKED",
        "returnCode": int(python_result["returnCode"]),
        "durationSeconds": round(time.monotonic() - started, 3),
        "tests": [str(path.relative_to(ROOT)) for path in tests],
        "counts": counts,
        "sourceSeal": source_seal,
        "command": python_command,
        "output": output,
        "blockingIssues": blocking_issues,
        "nativeUnit": {**native_unit, "execution": unit_result},
        "pythonExtension": extension,
        "pythonImport": import_result,
        "toolchain": {
            "python": platform.python_version(),
            "compiler": _tool_version(["c++", "--version"]),
            "linker": _tool_version(["ld", "--version"]),
        },
        "fullRepositoryPytestIsDiagnosticOnly": True,
    }
    output_path = args.output.expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps({
        "output": str(output_path), "status": record["status"],
        "returnCode": 0 if record["status"] == "PASS" else 1,
        "counts": counts, "blockingIssues": blocking_issues,
        "testCount": len(tests),
    }, sort_keys=True))
    return 0 if record["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
