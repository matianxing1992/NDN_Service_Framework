#!/usr/bin/env python3
"""Verify that Spec Kit entrypoints and installed shared skills use one contract.

This is a synchronization check only.  It does not inspect product code or
turn a skill/template check into implementation or qualification evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path


ENTRYPOINTS = (
    "speckit-specify",
    "speckit-clarify",
    "speckit-plan",
    "speckit-tasks",
    "speckit-analyze",
    "speckit-audit",
    "speckit-implement",
    "speckit-converge",
    "speckit-checklist",
    "speckit-constitution",
    "speckit-taskstoissues",
)

TEMPLATES = (
    ".specify/templates/spec-template.md",
    ".specify/templates/plan-template.md",
    ".specify/templates/tasks-template.md",
)

SHARED_FILES = (
    "SKILL.md",
    "agents/openai.yaml",
    "references/batch-quality-gates.md",
    "references/bounded-executor.md",
    "references/design-template.md",
    "references/experiment-static-review-loop.md",
    "references/pre-test-static-review.md",
    "references/review-agent.md",
    "references/review-gate.md",
    "references/symbol-contract.md",
    "references/task-progress.md",
    "references/work-unit-contract.md",
)

ENTRYPOINT_MARKER_GROUPS = (
    ("skills/speckit-code-design/references/batch-quality-gates.md",),
    ("Command Output Contract",),
    ("review-agent",),
    ("Coverage matrix", "five-lane coverage"),
    ("project-symbol definition map",),
    ("verify-spec-kit-sync.py",),
    ("--require-entrypoints",),
)

TEMPLATE_MARKER_GROUPS = {
    ".specify/templates/spec-template.md": (
        ("batch-quality-gates.md",),
        ("pre-test-static-review.md",),
    ),
    ".specify/templates/plan-template.md": (
        ("Design binding",),
        ("batch-quality-gates.md",),
        ("review-agent",),
    ),
    ".specify/templates/tasks-template.md": (
        ("Progress Timestamp",),
        ("YYYY-MM-DD HH:mm ±HH:MM",),
        ("Design binding",),
        ("batch-quality-gates.md",),
        ("review-agent",),
    ),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def repo_root(path: Path) -> Path:
    if path != Path.cwd():
        return path.resolve()
    try:
        return Path(
            subprocess.check_output(
                ["git", "rev-parse", "--show-toplevel"],
                cwd=path,
                text=True,
            ).strip()
        ).resolve()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"cannot resolve repository root: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path.cwd(),
        help="repository root (default: current directory)",
    )
    parser.add_argument(
        "--entrypoint-root",
        type=Path,
        default=None,
        help="local entrypoint installation (default: <repo>/.agents/skills)",
    )
    parser.add_argument(
        "--codex-home",
        type=Path,
        default=None,
        help="personal skill installation (default: CODEX_HOME or ~/.codex)",
    )
    parser.add_argument(
        "--require-entrypoints",
        action="store_true",
        help="fail when a local entrypoint installation is absent",
    )
    parser.add_argument(
        "--require-personal",
        action="store_true",
        help="fail when the personal shared skill installation is absent",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON")
    args = parser.parse_args()

    root = repo_root(args.repo_root)
    entrypoint_root = (args.entrypoint_root or root / ".agents" / "skills").resolve()
    codex_home = (
        args.codex_home
        or Path(os.environ.get("CODEX_HOME", Path.home() / ".codex"))
    ).expanduser().resolve()

    errors: list[str] = []
    warnings: list[str] = []
    checks: list[dict[str, str]] = []

    shared_reference = root / "skills/speckit-code-design/references/batch-quality-gates.md"
    if not shared_reference.is_file():
        errors.append(f"missing shared reference: {shared_reference}")
    else:
        checks.append({"kind": "shared-reference", "status": "present", "path": str(shared_reference)})

    for relative in TEMPLATES:
        path = root / relative
        if not path.is_file():
            errors.append(f"missing template: {path}")
            continue
        text = path.read_text(encoding="utf-8")
        missing = [
            " or ".join(group)
            for group in TEMPLATE_MARKER_GROUPS[relative]
            if not any(marker in text for marker in group)
        ]
        if missing:
            errors.append(f"template missing {', '.join(missing)}: {path}")
        else:
            checks.append({"kind": "template", "status": "ok", "path": relative})

    installed_count = 0
    for name in ENTRYPOINTS:
        path = entrypoint_root / name / "SKILL.md"
        if not path.is_file():
            message = f"entrypoint not installed: {path}"
            (errors if args.require_entrypoints else warnings).append(message)
            checks.append({"kind": "entrypoint", "status": "missing", "path": str(path)})
            continue
        installed_count += 1
        text = path.read_text(encoding="utf-8")
        missing = [
            " or ".join(group)
            for group in ENTRYPOINT_MARKER_GROUPS + (
                (("Design binding",),) if name in (
                    "speckit-plan", "speckit-tasks", "speckit-implement", "speckit-audit"
                ) else ()
            ) + (
                (("Progress Timestamp",),) if name in (
                    "speckit-plan", "speckit-tasks", "speckit-implement", "speckit-converge", "speckit-audit"
                ) else ()
            )
            if not any(marker in text for marker in group)
        ]
        if missing:
            errors.append(f"entrypoint missing {', '.join(missing)}: {path}")
            checks.append({"kind": "entrypoint", "status": "stale", "path": str(path)})
        else:
            checks.append({"kind": "entrypoint", "status": "ok", "path": str(path)})

    personal_root = codex_home / "skills" / "speckit-code-design"
    if not personal_root.is_dir():
        message = f"personal shared skill not installed: {personal_root}"
        (errors if args.require_personal else warnings).append(message)
        checks.append({"kind": "personal", "status": "missing", "path": str(personal_root)})
    else:
        for relative in SHARED_FILES:
            source = root / "skills/speckit-code-design" / relative
            target = personal_root / relative
            if not source.is_file():
                errors.append(f"versioned shared file missing: {source}")
                checks.append({"kind": "source", "status": "missing", "path": str(source)})
                continue
            if not target.is_file():
                errors.append(f"personal shared file missing: {target}")
                checks.append({"kind": "personal", "status": "missing", "path": str(target)})
                continue
            source_digest = sha256(source)
            target_digest = sha256(target)
            if source_digest != target_digest:
                errors.append(
                    f"personal shared file differs: {relative} "
                    f"source={source_digest} installed={target_digest}"
                )
                checks.append({"kind": "personal", "status": "stale", "path": str(target)})
            else:
                checks.append({"kind": "personal", "status": "ok", "path": str(target)})

    result = {
        "status": "PASS" if not errors else "FAIL",
        "repoRoot": str(root),
        "entrypointRoot": str(entrypoint_root),
        "installedEntrypoints": installed_count,
        "expectedEntrypoints": len(ENTRYPOINTS),
        "personalRoot": str(personal_root),
        "warnings": warnings,
        "errors": errors,
        "checks": checks,
    }
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(
            f"{result['status']}: {installed_count}/{len(ENTRYPOINTS)} local entrypoints; "
            f"personal shared skill={'present' if personal_root.is_dir() else 'absent'}"
        )
        for message in warnings:
            print(f"WARNING: {message}")
        for message in errors:
            print(f"ERROR: {message}")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
