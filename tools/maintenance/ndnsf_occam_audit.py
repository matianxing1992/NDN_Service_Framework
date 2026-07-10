#!/usr/bin/env python3
"""Read-only source inventory for the NDNSF Occam simplification program."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


RULES: dict[str, tuple[str, ...]] = {
    "v1-invocation": (
        r"\bPublishRequest\b",
        r"\bBloomFilter\b",
        r"\bsearchByFunctionName\b",
        r"\bparsePermissionTokenName\b",
    ),
    "core-application-leakage": (
        r"\bExecutionArtifactSpec\b",
        r"\bExecutionArtifact\b",
        r"\bRepoDataPlaneProducer\b",
        r"\bCoordinationIntent\b",
    ),
    "handler-less-planner": (
        r"NotImplementedError",
        r"handler\s*=\s*None",
    ),
    "legacy-contract-field": (
        r"legacy[_-](?:field|status|capability|reason)",
        r"operationStatus.*status|status.*operationStatus",
    ),
}

TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc",
    ".py", ".sh", ".md", ".rst", ".txt", ".json", ".yaml", ".yml",
}


@dataclass(frozen=True)
class Finding:
    rule: str
    pattern: str
    classification: str
    path: str
    line: int
    text: str


def classify_path(relative: Path) -> str:
    parts = relative.parts
    if not parts:
        return "other"
    if parts[0] in {"build", "_build"} or any(
        part in {"generated", "_generated", "gen"} for part in parts
    ):
        return "generated"
    if parts[0] == "tests" or any(part in {"test", "tests"} for part in parts):
        return "test"
    if parts[0] == "specs":
        return "historical-spec"
    if parts[0] == "docs" or relative.name.startswith("README"):
        return "docs"
    if parts[0] == "examples":
        return "example"
    if relative.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".py"}:
        return "active"
    return "other"


def iter_files(root: Path) -> Iterable[Path]:
    ignored = {
        ".git", ".codegraph", ".venv", "__pycache__", "node_modules",
        "third_party", ".planning", ".agents",
    }
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if (
            not path.is_file()
            or any(part in ignored for part in relative.parts)
            or relative.parts[:2] == ("Experiments", "gRPC")
            or relative.as_posix() == "tools/maintenance/ndnsf_occam_audit.py"
        ):
            continue
        if path.suffix.lower() in TEXT_SUFFIXES or path.name in {"wscript", "README", "AGENTS.md", "CLAUDE.md"}:
            yield path


def scan(root: Path, selected_rules: Iterable[str] | None = None) -> list[Finding]:
    names = list(selected_rules or RULES)
    unknown = sorted(set(names) - set(RULES))
    if unknown:
        raise ValueError(f"unknown rules: {', '.join(unknown)}")
    compiled = {
        name: [(pattern, re.compile(pattern)) for pattern in RULES[name]]
        for name in names
    }
    findings: list[Finding] = []
    for path in iter_files(root):
        relative = path.relative_to(root)
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        classification = classify_path(relative)
        for line_number, line in enumerate(lines, 1):
            for rule, patterns in compiled.items():
                for pattern, regex in patterns:
                    if regex.search(line):
                        findings.append(Finding(
                            rule=rule,
                            pattern=pattern,
                            classification=classification,
                            path=relative.as_posix(),
                            line=line_number,
                            text=line.strip()[:240],
                        ))
    return findings


def summary(findings: Iterable[Finding]) -> dict[str, object]:
    rows = list(findings)
    by_classification: dict[str, int] = {}
    by_rule: dict[str, int] = {}
    for row in rows:
        by_classification[row.classification] = by_classification.get(row.classification, 0) + 1
        by_rule[row.rule] = by_rule.get(row.rule, 0) + 1
    return {
        "total": len(rows),
        "active": by_classification.get("active", 0),
        "byClassification": dict(sorted(by_classification.items())),
        "byRule": dict(sorted(by_rule.items())),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".")
    parser.add_argument("--rule", action="append", choices=sorted(RULES))
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--fail-on-active", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    findings = scan(root, args.rule)
    result = {
        "root": str(root),
        "rules": list(args.rule or RULES),
        "summary": summary(findings),
        "findings": [asdict(item) for item in findings],
    }
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(json.dumps(result["summary"], sort_keys=True))
        for item in findings:
            print(f"{item.classification}\t{item.rule}\t{item.path}:{item.line}\t{item.text}")
    return 1 if args.fail_on_active and result["summary"]["active"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
