#!/usr/bin/env python3
"""Audit Spec 115 commit ownership, ref containment, and push fencing."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable, Sequence


OID_RE = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_ROW_KEYS = {
    "changeGroup",
    "classification",
    "sourceCommits",
    "targetCommit",
    "paths",
    "acceptanceGate",
}


class RewriteAuditError(RuntimeError):
    pass


def _run(repo: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", *args], cwd=repo, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode != 0:
        raise RewriteAuditError(
            f"git {' '.join(args)} failed: {completed.stderr.strip()}"
        )
    return completed.stdout.strip()


def _require_oid(value: str, label: str) -> str:
    if not OID_RE.fullmatch(value):
        raise RewriteAuditError(f"{label} is not a full 40-character OID")
    return value


def audit_manifest(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if "owners" in document:
        return _audit_nine_owner_manifest(document)

    rows = document.get("changeGroups")
    if not isinstance(rows, list) or not rows:
        raise RewriteAuditError("manifest requires a non-empty changeGroups list")

    groups: set[str] = set()
    official_targets: set[str] = set()
    classifications: dict[str, set[str]] = {}
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or not REQUIRED_ROW_KEYS.issubset(row):
            raise RewriteAuditError(f"manifest row {index} is incomplete")
        group = row["changeGroup"]
        classification = row["classification"]
        target = row["targetCommit"]
        if not all(isinstance(value, str) and value for value in (group, classification, target)):
            raise RewriteAuditError(f"manifest row {index} has an empty identity")
        if group in groups:
            raise RewriteAuditError(f"duplicate change group: {group}")
        groups.add(group)
        classifications.setdefault(target, set()).add(classification)
        if classification == "official-v3":
            official_targets.add(target)
        if classification == "official-v3-repair":
            raise RewriteAuditError("official V3 repair-only descendant is prohibited")
        if not row["sourceCommits"] or not row["paths"] or not row["acceptanceGate"]:
            raise RewriteAuditError(f"manifest row {group} lacks sources, paths, or a gate")

    if len(official_targets) != 1:
        raise RewriteAuditError("official V3 groups must have exactly one target")
    owner = next(iter(official_targets))
    forbidden = classifications[owner] - {"official-v3"}
    if forbidden:
        raise RewriteAuditError(
            "fork extensions or reliability work must not share the official V3 target"
        )
    return {
        "valid": True,
        "officialV3Owner": owner,
        "changeGroupCount": len(rows),
        "classifications": sorted({row["classification"] for row in rows}),
    }


def _audit_nine_owner_manifest(document: dict[str, Any]) -> dict[str, Any]:
    for key in ("base", "sourceHead", "finalHead", "finalTree"):
        _require_oid(document.get(key, ""), key)

    owners = document.get("owners")
    if not isinstance(owners, list) or len(owners) != 9:
        raise RewriteAuditError("nine-owner manifest requires exactly 9 owners")

    orders: set[int] = set()
    commits: set[str] = set()
    groups: dict[str, dict[str, Any]] = {}
    for index, owner in enumerate(owners):
        if not isinstance(owner, dict):
            raise RewriteAuditError(f"owner {index} is not an object")
        required = {"order", "commit", "changeGroup", "sourceCommits", "boundary"}
        if not required.issubset(owner):
            raise RewriteAuditError(f"owner {index} is incomplete")
        order = owner["order"]
        commit = _require_oid(owner["commit"], f"owner {index} commit")
        group = owner["changeGroup"]
        if not isinstance(order, int) or not isinstance(group, str) or not group:
            raise RewriteAuditError(f"owner {index} has an invalid identity")
        if order in orders or commit in commits or group in groups:
            raise RewriteAuditError(f"owner {index} duplicates order, commit, or group")
        if not isinstance(owner["sourceCommits"], list) or not owner["sourceCommits"]:
            raise RewriteAuditError(f"owner {index} lacks source ownership")
        if not isinstance(owner["boundary"], str) or not owner["boundary"]:
            raise RewriteAuditError(f"owner {index} lacks a boundary")
        orders.add(order)
        commits.add(commit)
        groups[group] = owner

    if orders != set(range(1, 10)):
        raise RewriteAuditError("owner order must be exactly 1 through 9")

    required_groups = {
        "official-v3-protocol",
        "failure-atomic-segmented-publication",
        "bounded-segmented-fetch-repair",
    }
    if not required_groups.issubset(groups):
        raise RewriteAuditError("manifest lacks V3, producer, or receiver owner")
    if (groups["failure-atomic-segmented-publication"]["commit"] ==
            groups["bounded-segmented-fetch-repair"]["commit"]):
        raise RewriteAuditError("producer publication and receiver recovery must be separate")

    harness = document.get("externalHarness")
    if not isinstance(harness, dict) or harness.get("owner") != "NDNSF":
        raise RewriteAuditError("external interoperability harness must remain NDNSF-owned")
    if not isinstance(harness.get("paths"), list) or not harness["paths"]:
        raise RewriteAuditError("external interoperability harness paths are missing")

    return {
        "valid": True,
        "officialV3Owner": groups["official-v3-protocol"]["commit"],
        "producerOwner": groups["failure-atomic-segmented-publication"]["commit"],
        "receiverOwner": groups["bounded-segmented-fetch-repair"]["commit"],
        "ownerCount": len(owners),
        "finalHead": document["finalHead"],
        "finalTree": document["finalTree"],
    }


def audit_force_command(expected_old: str, command: Sequence[str]) -> dict[str, Any]:
    expected_old = _require_oid(expected_old, "expected old remote")
    normalized = [item for item in command if item != "--"]
    if normalized[:2] != ["git", "push"]:
        raise RewriteAuditError("publication command must start with git push")
    if "--force" in normalized or "--force-with-lease" in normalized:
        raise RewriteAuditError("unqualified force or force-with-lease is prohibited")
    required = f"--force-with-lease=refs/heads/master:{expected_old}"
    if normalized.count(required) != 1:
        raise RewriteAuditError("publication command lacks the exact expected-old-OID lease")
    if "origin" not in normalized:
        raise RewriteAuditError("publication command must target origin")
    destinations = [item for item in normalized if item.endswith(":refs/heads/master")]
    if len(destinations) != 1:
        raise RewriteAuditError("publication command must update only refs/heads/master")
    return {"valid": True, "expectedOld": expected_old, "destination": destinations[0]}


def audit_refs(repo: Path, v3: str, master_ref: str,
               origin_master_ref: str, experimental_ref: str) -> dict[str, Any]:
    repo = repo.resolve()
    if not (repo / ".git").exists():
        raise RewriteAuditError(f"not a normal Git worktree: {repo}")
    v3_oid = _require_oid(_run(repo, "rev-parse", v3), "replacement V3")
    refs = {
        "master": _require_oid(_run(repo, "rev-parse", master_ref), "master"),
        "originMaster": _require_oid(
            _run(repo, "rev-parse", origin_master_ref), "origin/master"
        ),
        "experimental": _require_oid(
            _run(repo, "rev-parse", experimental_ref), "Experimental"
        ),
    }
    if refs["master"] != refs["originMaster"]:
        raise RewriteAuditError("local master and origin/master differ")
    for label, target in refs.items():
        completed = subprocess.run(
            ["git", "merge-base", "--is-ancestor", v3_oid, target], cwd=repo,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        if completed.returncode != 0:
            raise RewriteAuditError(f"replacement V3 is not an ancestor of {label}")
    return {"valid": True, "replacementV3": v3_oid, **refs}


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    manifest = subparsers.add_parser("manifest")
    manifest.add_argument("path", type=Path)

    force = subparsers.add_parser("force-command")
    force.add_argument("--expected-old", required=True)
    force.add_argument("push_command", nargs=argparse.REMAINDER)

    refs = subparsers.add_parser("refs")
    refs.add_argument("--repo", type=Path, required=True)
    refs.add_argument("--v3", required=True)
    refs.add_argument("--master", default="master")
    refs.add_argument("--origin-master", default="origin/master")
    refs.add_argument("--experimental", default="Experimental")

    args = parser.parse_args(argv)
    try:
        if args.command == "manifest":
            receipt = audit_manifest(args.path)
        elif args.command == "force-command":
            receipt = audit_force_command(args.expected_old, args.push_command)
        else:
            receipt = audit_refs(
                args.repo, args.v3, args.master,
                args.origin_master, args.experimental,
            )
        print(json.dumps(receipt, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError, RewriteAuditError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
