#!/usr/bin/env python3
"""Spec175 profile-bound submission implementation."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any

LIB = Path(__file__).resolve().parents[2] / "lib"
if str(LIB) not in sys.path:
    sys.path.insert(0, str(LIB))

from spec175_tiger_profile import (  # noqa: E402
    DELTA_SCHEMA,
    GATES,
    ProfileError,
    canonical_digest,
    load_profile,
    load_run,
    render_sbatch_argv,
    write_report,
)


ROOT = Path(__file__).resolve().parents[4]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _run(command: list[str], *, output: Path | None = None) -> None:
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()
        raise ProfileError(f"LOCAL_GATE_FAILED:{command[0]}:{detail[-1] if detail else result.returncode}")


def _verify_delta_report(path: Path, expected: dict[str, Any]) -> None:
    try:
        actual = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProfileError(f"PROFILE_DELTA_REPORT_INVALID:{path}:{exc}") from exc
    required = {
        "schema": DELTA_SCHEMA,
        "status": "PASS",
        "profileId": expected["profileId"],
        "profileSha256": expected["profileSha256"],
        "runId": expected["runId"],
        "gate": expected["gate"],
        "effectiveConfigSha256": expected["effectiveConfigSha256"],
        "argvSha256": expected["argvSha256"],
        "provenBaselineExactDelta": "PASS",
    }
    for key, value in required.items():
        if actual.get(key) != value:
            raise ProfileError(f"PROFILE_DELTA_REPORT_MISMATCH:{key}")
    if actual.get("differences") != expected["effectiveConfig"].get("allowlistedDeltas", []):
        raise ProfileError("PROFILE_DELTA_REPORT_HAS_UNRESOLVED_DIFFERENCES")
    if actual.get("argv") != expected["argv"]:
        raise ProfileError("PROFILE_SUBMISSION_ARGV_MISMATCH")
    if actual.get("sealedEnvironment") != expected["sealedEnvironment"]:
        raise ProfileError("PROFILE_SEALED_ENVIRONMENT_MISMATCH")


def _check_local_inputs(run: dict[str, Any]) -> None:
    candidate = run["candidate"]
    sif = Path(candidate["sif"])
    if _sha256(sif) != candidate["sifSha256"]:
        raise ProfileError("SPEC175_LOCAL_SIF_DIGEST_MISMATCH")
    if not Path(run["workload"]).is_file():
        raise ProfileError("SPEC175_WORKLOAD_MISSING")
    if run["gate"] != "control" and not Path(run["model"]["manifest"]).is_file():
        raise ProfileError("SPEC175_MODEL_MANIFEST_MISSING")


def _validate_candidate_and_prerequisites(run: dict[str, Any], profile_delta: Path) -> None:
    candidate = run["candidate"]
    closure_out = profile_delta.parent / "candidate-closure-validation.json"
    closure_validator = ROOT / "packaging/ndnsf-di-container/bin/spec175-candidate-closure"
    _run([
        str(closure_validator), "--manifest", candidate["closureManifest"],
        "--gate", GATES[run["gate"]]["candidate"], "--expected-sif", candidate["sif"],
        "--expected-sif-sha256", candidate["sifSha256"], "--output", str(closure_out),
    ])
    prerequisite_args: list[str] = []
    for name, path in sorted(run.get("prerequisites", {}).items()):
        prerequisite_args.extend(["--prerequisite", f"{name}={path}"])
    prereq_validator = ROOT / "packaging/ndnsf-di-container/bin/spec175-gate-prerequisites"
    prereq_out = profile_delta.parent / "gate-prerequisite-validation.json"
    _run([
        str(prereq_validator), "--candidate-closure", candidate["closureManifest"],
        "--gate", GATES[run["gate"]]["candidate"], *prerequisite_args,
        "--output", str(prereq_out),
    ])


def _validate_model_and_functional(run: dict[str, Any], profile_delta: Path) -> None:
    if run["gate"] == "control":
        return
    model = run["model"]
    _run([
        str(ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-spec175-model-preflight"),
        "--manifest", model["manifest"],
    ], output=profile_delta.parent / "model-preflight.json")
    if run["gate"] in {"multi-provider", "conversation-residency", "performance"}:
        _run([
            str(ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-spec175-functional-preflight"),
            "--bundle", run["bundle"], "--gate", run["gate"],
        ], output=profile_delta.parent / "functional-bundle-preflight.json")


def _validate_checklist(run: dict[str, Any], profile_delta: Path) -> None:
    candidate = run["candidate"]
    _run([
        str(ROOT / "packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist"),
        "--manifest", run["checklist"], "--gate", GATES[run["gate"]]["checklist"],
        "--output", run["checklistValidation"], "--candidate-manifest", candidate["closureManifest"],
        "--expected-sif", candidate["sif"], "--expected-sif-sha256", candidate["sifSha256"],
    ])
    if not profile_delta.is_file():
        raise ProfileError("PROFILE_DELTA_REPORT_MISSING_AFTER_CHECKLIST")


def render(profile_path: Path, run_path: Path, repository_root: Path) -> dict[str, Any]:
    profile = load_profile(profile_path)
    raw = json.loads(run_path.read_text(encoding="utf-8"))
    run = load_run(run_path, raw.get("gate", ""))
    if run["profileSha256"] != canonical_digest(profile):
        raise ProfileError("PROFILE_DIGEST_MISMATCH")
    _check_local_inputs(run)
    argv, exports, effective = render_sbatch_argv(profile, run, repository_root, profile_path)
    report = {
        "schema": DELTA_SCHEMA,
        "status": "PASS",
        "profileId": profile["profileId"],
        "profileSha256": canonical_digest(profile),
        "runId": run["runId"],
        "gate": run["gate"],
        "allowedDeltas": profile["gates"][run["gate"]]["allowlist"],
        "differences": effective.get("allowlistedDeltas", []),
        "provenBaselineExactDelta": "PASS",
        "effectiveConfig": effective,
        "effectiveConfigSha256": canonical_digest(effective),
        "argv": argv,
        "argvSha256": canonical_digest(argv),
        "sealedEnvironment": exports,
    }
    return report


def submit(gate: str, profile_path: Path, run_path: Path, repository_root: Path) -> int:
    profile = load_profile(profile_path)
    run = load_run(run_path, gate)
    if run["profileSha256"] != canonical_digest(profile):
        raise ProfileError("PROFILE_DIGEST_MISMATCH")
    _check_local_inputs(run)
    argv, exports, effective = render_sbatch_argv(profile, run, repository_root, profile_path)
    expected = {
        "profileId": profile["profileId"],
        "profileSha256": canonical_digest(profile),
        "runId": run["runId"],
        "gate": gate,
        "effectiveConfig": effective,
        "effectiveConfigSha256": canonical_digest(effective),
        "argv": argv,
        "argvSha256": canonical_digest(argv),
        "sealedEnvironment": exports,
        "provenBaselineExactDelta": "PASS",
    }
    _verify_delta_report(Path(run["profileDelta"]), expected)
    _validate_candidate_and_prerequisites(run, Path(run["profileDelta"]))
    _validate_model_and_functional(run, Path(run["profileDelta"]))
    _validate_checklist(run, Path(run["profileDelta"]))
    sbatch = shutil.which("sbatch")
    if not sbatch:
        raise ProfileError("SPEC175_SBATCH_UNAVAILABLE")
    # The job receives only the explicit --export=NONE values rendered above.
    parent_env = {"PATH": str(Path(sbatch).parent) + ":/usr/bin:/bin", "LC_ALL": "C"}
    submitted_argv = list(argv)
    result = subprocess.run(submitted_argv, cwd=repository_root, env=parent_env,
                            text=True, capture_output=True, check=False)
    submission = dict(expected)
    submission.update({
        "status": "SUBMITTED" if result.returncode == 0 else "REJECTED_BY_SCHEDULER",
        "submittedArgv": submitted_argv,
        "submittedArgvSha256": canonical_digest(submitted_argv),
        "schedulerReturnCode": result.returncode,
        "schedulerStdout": result.stdout,
        "schedulerStderr": result.stderr,
        "submittedBytesEqualValidated": submitted_argv == argv,
    })
    write_report(Path(run["profileDelta"]).with_name("submission-record.json"), submission)
    if result.returncode != 0:
        raise ProfileError(f"SPEC175_SBATCH_FAILED:{result.returncode}")
    return 0


def main(argv: list[str]) -> int:
    try:
        if len(argv) == 4 and argv[0] == "render":
            profile_path, run_path, root = map(Path, argv[1:])
            report = render(profile_path, run_path, root)
            raw = json.loads(run_path.read_text(encoding="utf-8"))
            write_report(Path(raw["profileDelta"]), report)
            print(json.dumps(report, indent=2, sort_keys=True))
            return 0
        if len(argv) == 5 and argv[0] == "submit":
            return submit(argv[1], Path(argv[2]), Path(argv[3]), Path(argv[4]))
    except (OSError, ProfileError, KeyError, TypeError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print("usage: submit_profile.py render PROFILE RUN ROOT | submit GATE PROFILE RUN ROOT", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
