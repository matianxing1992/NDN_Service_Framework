#!/usr/bin/env python3
"""Fail-closed guardrails for NDNSF Context Mode retrieval and host capture.

The guard has four independent entry points:

* ``query`` builds a safe ``ctx_search`` request and can validate saved output.
* ``health`` verifies the active-feature pointer, indexed file hashes, host
  restart ordering, and current-project session capture.
* ``marker`` proves that a unique real user-prompt marker reached the exact
  project's SessionDB after the relevant timestamps.
* ``hook`` enforces the query policy for Codex ``PreToolUse`` events.

Only Python's standard library is required.  The guard never writes to a
Context Mode database.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import shlex
import sqlite3
import stat
import subprocess
import sys
import time
from typing import Any, Iterable, Sequence


EXIT_OK = 0
EXIT_INVALID_QUERY = 2
EXIT_CONTAMINATED_RESULT = 3
EXIT_SOURCE_MISMATCH = 4
EXIT_PROJECT_BINDING_UNPROVEN = 5

AUTHORITY_SORT = "relevance"
SESSION_SORT = "timeline"
PLATFORMS = ("codex", "claude-code")
SESSION_CATEGORIES = {
    "blocked-on",
    "compaction",
    "decision",
    "error",
    "error-resolution",
    "plan",
    "rejected-approach",
    "user-prompt",
}
PROJECT_CONTEXT_ANCHOR_LABEL = "NDNSF project context anchor"
PROJECT_CONTEXT_ANCHOR_RELATIVE_PATH = Path(
    ".specify/memory/context-mode-project.md"
)
PROJECT_ROOT = Path(__file__).resolve().parents[1]
GENERIC_AUTHORITY_PHRASES = {
    "active feature",
    "current feature",
    "latest checkpoint",
    "latest verified checkpoint",
    "open blocker",
    "open blockers",
    "unresolved blocker",
    "unresolved blockers",
    "rejected approach",
    "rejected approaches",
}
REAL_MARKER_RE = re.compile(r"^CTX_HOST_ACCEPT_[A-Za-z0-9][A-Za-z0-9_.:-]*$")
FIXTURE_MARKER_RE = re.compile(r"^CTX_FIXTURE_[A-Za-z0-9][A-Za-z0-9_.:-]*$")
AUTO_MEMORY_HEADER_RE = re.compile(
    r"(?im)^\s*(?:---\s*)?(?:#{1,6}\s*)?\[\s*auto[- ]memory(?:\s*[|\]])"
)
MCP_RESULT_HEADER_RE = re.compile(
    r"(?m)^--- \[(?P<meta>[^\]\r\n]+)\] ---[ \t]*$"
)
CLI_RESULT_HEADER_RE = re.compile(r"(?m)^##[ \t]+\d+\.[ \t]+.+$")
CLI_SOURCE_RE = re.compile(r"(?m)^Source:[ \t]*(?P<source>[^\r\n]+)[ \t]*$")


class GuardError(RuntimeError):
    """A guard failure with a stable process exit code."""

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class FeatureContext:
    root: Path
    feature_directory: str
    feature_path: Path
    feature_name: str


@dataclass(frozen=True)
class SearchResultBlock:
    source: str
    origin: str
    text: str


def _resolved(path: Path) -> Path:
    return path.expanduser().resolve(strict=False)


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _connect_read_only(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(f"{path.resolve().as_uri()}?mode=ro", uri=True)


def load_feature_context(project_root: Path) -> FeatureContext:
    root = _resolved(project_root)
    pointer = root / ".specify" / "feature.json"
    try:
        payload = json.loads(pointer.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise GuardError(
            EXIT_SOURCE_MISMATCH, f"active feature pointer is missing: {pointer}"
        ) from exc
    except (json.JSONDecodeError, OSError) as exc:
        raise GuardError(
            EXIT_SOURCE_MISMATCH, f"active feature pointer is unreadable: {exc}"
        ) from exc

    feature_directory = payload.get("feature_directory")
    if not isinstance(feature_directory, str) or not feature_directory.strip():
        raise GuardError(
            EXIT_SOURCE_MISMATCH,
            "feature.json must contain a non-empty string feature_directory",
        )
    feature_path = _resolved(root / feature_directory)
    if not _is_within(feature_path, root):
        raise GuardError(
            EXIT_SOURCE_MISMATCH,
            f"feature_directory escapes the project root: {feature_directory}",
        )
    return FeatureContext(
        root=root,
        feature_directory=feature_directory,
        feature_path=feature_path,
        feature_name=feature_path.name,
    )


def _is_high_entropy(identifier: str) -> bool:
    value = identifier.strip()
    if REAL_MARKER_RE.fullmatch(value) or FIXTURE_MARKER_RE.fullmatch(value):
        return True
    if len(value) < 8 or any(ch.isspace() for ch in value):
        return False
    return (
        any(ch.isdigit() for ch in value)
        or any(ch in "-_/.:#" for ch in value)
        or sum(ch.isupper() for ch in value) >= 2
    )


def _normalize_queries(queries: Sequence[str]) -> list[str]:
    normalized = [query.strip() for query in queries if query.strip()]
    if not normalized:
        raise GuardError(EXIT_INVALID_QUERY, "at least one non-empty query is required")
    return normalized


def _validate_required_identifiers(
    queries: Sequence[str], required: Sequence[str], lane: str
) -> list[str]:
    identifiers = list(dict.fromkeys(item.strip() for item in required if item.strip()))
    if not identifiers:
        raise GuardError(
            EXIT_INVALID_QUERY,
            f"{lane} queries require explicit high-entropy --require identifiers",
        )
    invalid = [item for item in identifiers if not _is_high_entropy(item)]
    if invalid:
        raise GuardError(
            EXIT_INVALID_QUERY,
            "low-entropy required identifier(s): " + ", ".join(invalid),
        )
    corpus = "\n".join(queries)
    missing = [item for item in identifiers if item not in corpus]
    if missing:
        raise GuardError(
            EXIT_INVALID_QUERY,
            "required identifier(s) absent from the query: " + ", ".join(missing),
        )
    return identifiers


def _query_identifiers(queries: Sequence[str]) -> list[str]:
    candidates: list[str] = []
    for query in queries:
        candidates.extend(
            re.findall(r"[A-Za-z0-9][A-Za-z0-9_./:#-]{7,}", query)
        )
    return list(dict.fromkeys(item for item in candidates if _is_high_entropy(item)))


def _generic_authority_hits(queries: Sequence[str]) -> list[str]:
    normalized = re.sub(r"[_-]+", " ", "\n".join(queries).lower())
    normalized = re.sub(r"\s+", " ", normalized).strip()
    return sorted(
        phrase for phrase in GENERIC_AUTHORITY_PHRASES if phrase in normalized
    )


def build_search_request(
    *,
    lane: str,
    queries: Sequence[str],
    source: str,
    required: Sequence[str],
    known_sources: set[str] | None = None,
) -> dict[str, Any]:
    """Build a safe ctx_search payload or fail before retrieval."""

    normalized = _normalize_queries(queries)
    identifiers = _validate_required_identifiers(normalized, required, lane)
    source = source.strip()
    if not source:
        raise GuardError(EXIT_INVALID_QUERY, "--source must be exact and non-empty")
    if any(token in source for token in ("*", "?", "[", "]")):
        raise GuardError(EXIT_INVALID_QUERY, "--source cannot contain wildcard syntax")

    generic_hits = _generic_authority_hits(normalized)

    if lane in {"authority", "project"}:
        if known_sources is not None and source not in known_sources:
            raise GuardError(
                EXIT_SOURCE_MISMATCH,
                f"{lane} source is not an exact indexed label: {source}",
            )
        return {
            "lane": lane,
            "ctx_search": {
                "queries": normalized,
                "source": source,
                "sort": AUTHORITY_SORT,
            },
            "required_identifiers": identifiers,
            "postconditions": [
                "reject auto-memory origins",
                "reject results missing the exact source label",
                "reject results missing any required identifier",
            ],
        }

    if lane == "session":
        if source not in SESSION_CATEGORIES:
            raise GuardError(
                EXIT_INVALID_QUERY,
                "session --source must be an explicit event category: "
                + ", ".join(sorted(SESSION_CATEGORIES)),
            )
        if generic_hits:
            raise GuardError(
                EXIT_INVALID_QUERY,
                "timeline queries cannot ask document-authority questions: "
                + ", ".join(generic_hits),
            )
        has_marker = any(
            REAL_MARKER_RE.fullmatch(item) or FIXTURE_MARKER_RE.fullmatch(item)
            for item in identifiers
        )
        if not has_marker and len(identifiers) < 2:
            raise GuardError(
                EXIT_INVALID_QUERY,
                "session recall requires one unique marker or two high-entropy identifiers",
            )
        return {
            "lane": lane,
            "ctx_search": {
                "queries": normalized,
                "source": source,
                "sort": SESSION_SORT,
            },
            "required_identifiers": identifiers,
            "postconditions": [
                "reject auto-memory origins",
                "verify category and project binding in SessionDB",
                "reject results missing any required identifier",
            ],
        }

    raise GuardError(EXIT_INVALID_QUERY, f"unsupported retrieval lane: {lane}")


def _is_context_search_tool(tool_name: object) -> bool:
    if not isinstance(tool_name, str):
        return False
    if tool_name == "ctx_search":
        return True
    normalized = tool_name.lower().replace("-", "_")
    return normalized.endswith("ctx_search") and "context_mode" in normalized


def _hook_tool_input(payload: dict[str, Any]) -> dict[str, Any]:
    for key in ("tool_input", "arguments", "input"):
        value = payload.get(key)
        if isinstance(value, dict):
            return value
        if isinstance(value, str):
            try:
                decoded = json.loads(value)
            except json.JSONDecodeError:
                continue
            if isinstance(decoded, dict):
                return decoded
    return {}


def _deny_hook(reason: str) -> dict[str, Any]:
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": reason,
        }
    }


def evaluate_pretool_hook(
    payload: dict[str, Any], *, project_root: Path, context_home: Path
) -> dict[str, Any]:
    """Return an empty allow object or a Codex PreToolUse denial.

    The policy is intentionally scoped to this repository.  Hook events for
    another repository, or for a tool other than Context Mode ``ctx_search``,
    are left untouched.
    """

    root = _resolved(project_root)
    raw_cwd = payload.get("cwd") or payload.get("project_dir")
    if not isinstance(raw_cwd, str) or not raw_cwd.strip():
        return {}
    cwd = _resolved(Path(raw_cwd))
    if not _is_within(cwd, root):
        return {}
    tool_name = payload.get("tool_name") or payload.get("toolName")
    if not _is_context_search_tool(tool_name):
        return {}

    tool_input = _hook_tool_input(payload)
    raw_queries = tool_input.get("queries")
    if isinstance(raw_queries, str):
        raw_queries = [raw_queries]
    if not isinstance(raw_queries, list) or not all(
        isinstance(item, str) for item in raw_queries
    ):
        return _deny_hook("Context Mode guard: ctx_search requires string queries")
    try:
        queries = _normalize_queries(raw_queries)
    except GuardError as exc:
        return _deny_hook(f"Context Mode guard: {exc}")

    sort = tool_input.get("sort")
    source = tool_input.get("source")
    source = source.strip() if isinstance(source, str) else ""
    generic_hits = _generic_authority_hits(queries)

    if sort == SESSION_SORT:
        exact_real_marker = (
            len(queries) == 1 and REAL_MARKER_RE.fullmatch(queries[0]) is not None
        )
        if exact_real_marker:
            if source and source != "user-prompt":
                return _deny_hook(
                    "Context Mode guard: exact host markers may only use "
                    "source=user-prompt"
                )
            return {}
        if generic_hits:
            return _deny_hook(
                "Context Mode guard: timeline cannot retrieve document authority "
                "or checkpoints; use relevance plus an exact file-backed source"
            )
        if source not in SESSION_CATEGORIES:
            return _deny_hook(
                "Context Mode guard: timeline requires an explicit session-event "
                "source/category"
            )
        identifiers = _query_identifiers(queries)
        if len(identifiers) < 2:
            return _deny_hook(
                "Context Mode guard: timeline requires an exact CTX_HOST_ACCEPT_* "
                "marker or at least two high-entropy identifiers"
            )
        return {}

    if sort != AUTHORITY_SORT:
        return _deny_hook(
            "Context Mode guard: maintained-document searches require sort=relevance"
        )
    if not source:
        return _deny_hook(
            "Context Mode guard: maintained-document searches require an exact "
            "file-backed source label"
        )
    try:
        content_db = find_project_content_db(root, context_home)
        known_sources = exact_source_selectors(indexed_source_labels(content_db))
        active_context = None
        if source == "NDNSF active feature pointer" or source.startswith(
            "NDNSF active feature "
        ):
            active_context = load_feature_context(root)
        if active_context is not None and (
            source == "NDNSF active feature pointer"
            or source.startswith(
                f"NDNSF active feature {active_context.feature_name}:"
            )
        ):
            verify_file_backed_sources(active_context, content_db)
        else:
            verify_project_file_backed_sources(root, content_db)
    except GuardError as exc:
        return _deny_hook(f"Context Mode guard: {exc}")
    if source not in known_sources:
        return _deny_hook(
            "Context Mode guard: source is not an exact current-project indexed label"
        )
    if generic_hits and not _query_identifiers(queries):
        # Exact source already prevents cross-document pollution, but a generic
        # authority prompt still needs one feature-specific discriminator.
        return _deny_hook(
            "Context Mode guard: generic authority phrases require a "
            "feature-specific high-entropy identifier"
        )
    return {}


def _json_origin_is_contaminated(value: Any) -> bool:
    if isinstance(value, list):
        return any(_json_origin_is_contaminated(item) for item in value)
    if not isinstance(value, dict):
        return False
    for key, item in value.items():
        if key.lower() in {"origin", "source", "source_path", "file", "file_path"}:
            if isinstance(item, str) and (
                "auto-memory" in item.lower()
                or Path(item).name.lower() == "agents.md"
            ):
                return True
        if _json_origin_is_contaminated(item):
            return True
    return False


def validate_search_results(
    text: str, *, lane: str, source: str, required: Sequence[str]
) -> None:
    """Validate serialized ctx_search output without trusting its ranking."""

    if AUTO_MEMORY_HEADER_RE.search(text):
        raise GuardError(
            EXIT_CONTAMINATED_RESULT, "search output contains an auto-memory result"
        )
    try:
        decoded = json.loads(text)
    except json.JSONDecodeError:
        decoded = None
    if decoded is not None and _json_origin_is_contaminated(decoded):
        raise GuardError(
            EXIT_CONTAMINATED_RESULT,
            "search output identifies AGENTS.md or auto-memory as an origin",
        )
    if source not in text:
        raise GuardError(
            EXIT_SOURCE_MISMATCH,
            f"search output does not contain the exact source/category: {source}",
        )
    missing = [identifier for identifier in required if identifier not in text]
    if missing:
        raise GuardError(
            EXIT_SOURCE_MISMATCH,
            "search output is missing required identifier(s): " + ", ".join(missing),
        )
    if lane not in {"authority", "project", "session"}:
        raise GuardError(EXIT_INVALID_QUERY, f"unsupported retrieval lane: {lane}")


def find_project_content_db(project_root: Path, context_home: Path) -> Path:
    root = _resolved(project_root)
    anchor = str(root / PROJECT_CONTEXT_ANCHOR_RELATIVE_PATH)
    pointer = str(root / ".specify" / "feature.json")

    def matching_databases(file_path: str) -> list[Path]:
        matches: list[Path] = []
        for database in sorted(_resolved(context_home).glob("content/*.db")):
            try:
                with _connect_read_only(database) as connection:
                    row = connection.execute(
                        "SELECT 1 FROM sources WHERE file_path = ? LIMIT 1",
                        (file_path,),
                    ).fetchone()
            except sqlite3.Error:
                continue
            if row:
                matches.append(database)
        return matches

    # The stable anchor identifies the project ContentDB independently of the
    # mutable active Spec pointer.  Pointer fallback keeps older stores
    # diagnosable until they run the authority index helper once.
    matches = matching_databases(anchor)
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            f"expected one project ContentDB for {anchor}, found {len(matches)}",
        )

    matches = matching_databases(pointer)
    if len(matches) != 1:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "expected one project ContentDB for "
            f"{anchor} (or legacy pointer {pointer}), found {len(matches)}; "
            "run scripts/context_mode_index_authority.sh, then rerun health",
        )
    return matches[0]


def indexed_source_labels(content_db: Path) -> set[str]:
    try:
        with _connect_read_only(content_db) as connection:
            return {
                row[0]
                for row in connection.execute("SELECT label FROM sources")
                if isinstance(row[0], str)
            }
    except sqlite3.Error as exc:
        raise GuardError(
            EXIT_SOURCE_MISMATCH, f"cannot read indexed source labels: {exc}"
        ) from exc


def exact_source_selectors(labels: Iterable[str]) -> set[str]:
    """Return full labels plus unambiguous directory-index base labels.

    Context Mode stores a directory source as ``BASE:/absolute/file`` for every
    indexed file, while callers address the directory by ``BASE``.  A base is
    accepted only when every label containing that selector belongs to the
    exact ``BASE:/...`` family; arbitrary substrings remain invalid.
    """

    full_labels = {label for label in labels if isinstance(label, str) and label}
    selectors = set(full_labels)
    bases = {
        label.split(":/", 1)[0]
        for label in full_labels
        if ":/" in label and label.split(":/", 1)[0]
    }
    for base in bases:
        matches = {label for label in full_labels if base in label}
        if matches and all(label.startswith(base + ":/") for label in matches):
            selectors.add(base)
    return selectors


def verify_project_file_backed_sources(
    project_root: Path, content_db: Path
) -> list[dict[str, Any]]:
    """Verify the stable project-layer sources.

    This layer deliberately does not inspect ``.specify/feature.json`` or any
    active Spec document.  It is the safe retrieval surface for cross-Spec
    architecture and historical design context.
    """

    root = _resolved(project_root)
    required = {
        PROJECT_CONTEXT_ANCHOR_LABEL: root / PROJECT_CONTEXT_ANCHOR_RELATIVE_PATH,
        "NDNSF Context Mode operating guide": root / ".specify" / "memory" / "context-mode.md",
    }
    try:
        with _connect_read_only(content_db) as connection:
            rows = connection.execute(
                "SELECT label, file_path, content_hash FROM sources"
            ).fetchall()
    except sqlite3.Error as exc:
        raise GuardError(
            EXIT_SOURCE_MISMATCH, f"cannot inspect ContentDB sources: {exc}"
        ) from exc

    by_label = {row[0]: row for row in rows}
    failures: list[str] = []
    checks: list[dict[str, Any]] = []
    for label, expected_path in required.items():
        expected_path = _resolved(expected_path)
        row = by_label.get(label)
        if row is None:
            failures.append(f"missing indexed project source: {label}")
            continue
        _, stored_path, stored_hash = row
        if _resolved(Path(stored_path)) != expected_path:
            failures.append(f"project source path mismatch for {label}")
            continue
        if not expected_path.is_file():
            failures.append(f"indexed project source file is missing: {expected_path}")
            continue
        actual_hash = _sha256(expected_path)
        if stored_hash != actual_hash:
            failures.append(f"stale indexed project source hash: {label}")
            continue
        checks.append(
            {
                "label": label,
                "path": str(expected_path),
                "fresh": True,
            }
        )
    if failures:
        raise GuardError(EXIT_SOURCE_MISMATCH, "; ".join(failures))
    return checks


def verify_file_backed_sources(
    context: FeatureContext, content_db: Path
) -> list[dict[str, Any]]:
    """Verify current-project indexed files and required active-feature sources."""

    pointer = context.root / ".specify" / "feature.json"
    guide = context.root / ".specify" / "memory" / "context-mode.md"
    required = {
        "NDNSF active feature pointer": pointer,
        "NDNSF Context Mode operating guide": guide,
    }
    feature_prefix = f"NDNSF active feature {context.feature_name}:"
    for filename in ("spec.md", "plan.md", "tasks.md"):
        path = context.feature_path / filename
        required[feature_prefix + str(path)] = path

    try:
        with _connect_read_only(content_db) as connection:
            rows = connection.execute(
                "SELECT label, file_path, content_hash FROM sources"
            ).fetchall()
    except sqlite3.Error as exc:
        raise GuardError(
            EXIT_SOURCE_MISMATCH, f"cannot inspect ContentDB sources: {exc}"
        ) from exc

    by_label = {row[0]: row for row in rows}
    failures: list[str] = []
    checks: list[dict[str, Any]] = []
    for label, expected_path in required.items():
        expected_path = _resolved(expected_path)
        row = by_label.get(label)
        if row is None:
            failures.append(f"missing indexed source: {label}")
            continue
        _, stored_path, stored_hash = row
        if _resolved(Path(stored_path)) != expected_path:
            failures.append(f"source path mismatch for {label}")
            continue
        if not expected_path.is_file():
            failures.append(f"indexed source file is missing: {expected_path}")
            continue
        actual_hash = _sha256(expected_path)
        if stored_hash != actual_hash:
            failures.append(f"stale indexed source hash: {label}")
            continue
        checks.append({"label": label, "path": str(expected_path), "fresh": True})

    for label, stored_path, stored_hash in rows:
        if not stored_path:
            continue
        path = _resolved(Path(stored_path))
        if not _is_within(path, context.root):
            continue
        if path.name.lower() == "agents.md":
            failures.append(f"AGENTS.md must not be a file-backed source: {label}")
            continue
        if not path.is_file():
            failures.append(f"indexed project file is missing: {path}")
            continue
        if stored_hash != _sha256(path):
            failures.append(f"stale indexed source hash: {label}")

    if failures:
        raise GuardError(EXIT_SOURCE_MISMATCH, "; ".join(dict.fromkeys(failures)))
    return checks


def _process_start_epoch(pid: int, proc_root: Path = Path("/proc")) -> float:
    stat = (proc_root / str(pid) / "stat").read_text(encoding="utf-8")
    closing = stat.rfind(")")
    if closing < 0:
        raise ValueError(f"malformed process stat for PID {pid}")
    fields_after_name = stat[closing + 2 :].split()
    start_ticks = int(fields_after_name[19])
    uptime = float((proc_root / "uptime").read_text().split()[0])
    boot_epoch = time.time() - uptime
    return boot_epoch + start_ticks / os.sysconf("SC_CLK_TCK")


def _is_client_process(tokens: Sequence[str], platform: str) -> bool:
    if not tokens:
        return False
    executable = Path(tokens[0]).name.lower()
    if platform == "codex":
        return executable == "codex" and "app-server" in tokens[1:]
    if platform == "claude-code":
        if executable in {"claude", "claude-code"}:
            return True
        if executable in {"node", "nodejs", "bun"}:
            return any(
                "@anthropic-ai/claude-code" in token.lower()
                or "/claude-code/" in token.lower()
                for token in tokens[1:]
            )
        return False
    raise GuardError(EXIT_INVALID_QUERY, f"unsupported platform: {platform}")


def newest_client_start(
    platform: str, proc_root: Path = Path("/proc")
) -> float:
    if platform not in PLATFORMS:
        raise GuardError(EXIT_INVALID_QUERY, f"unsupported platform: {platform}")
    starts: list[float] = []
    for cmdline in _resolved(proc_root).glob("[0-9]*/cmdline"):
        try:
            tokens = [
                token.decode("utf-8", errors="replace")
                for token in cmdline.read_bytes().split(b"\0")
                if token
            ]
            if _is_client_process(tokens, platform):
                starts.append(_process_start_epoch(int(cmdline.parent.name), proc_root))
        except (OSError, ValueError):
            continue
    if not starts:
        client_name = "Codex app-server" if platform == "codex" else "Claude Code"
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            f"no running {client_name} process was found",
        )
    return max(starts)


def newest_codex_app_server_start(proc_root: Path = Path("/proc")) -> float:
    """Backward-compatible wrapper for callers predating --platform."""

    return newest_client_start("codex", proc_root)


def _hook_commands(value: Any) -> Iterable[str]:
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "command" and isinstance(child, str):
                yield child
            else:
                yield from _hook_commands(child)
    elif isinstance(value, list):
        for child in value:
            yield from _hook_commands(child)


def _is_guard_hook_command(
    command: str, guard_script: Path, platform: str = "codex"
) -> bool:
    try:
        tokens = shlex.split(command)
    except ValueError:
        return False
    expected = _resolved(guard_script)
    for index, token in enumerate(tokens[:-1]):
        if token.endswith("context_mode_guard.py"):
            candidate = _resolved(Path(token))
            if candidate == expected and tokens[index + 1] == "hook":
                platform_values = [
                    tokens[position + 1]
                    for position, value in enumerate(tokens[:-1])
                    if value == "--platform"
                ]
                if platform == "claude-code":
                    return platform_values == ["claude-code"]
                return not platform_values or platform_values == ["codex"]
    return False


def _has_enforced_guard_hook(
    payload: Any, guard_script: Path, platform: str = "codex"
) -> bool:
    if not isinstance(payload, dict):
        return False
    hook_groups = payload.get("hooks")
    if not isinstance(hook_groups, dict):
        return False
    rules = hook_groups.get("PreToolUse")
    if not isinstance(rules, list):
        return False
    for rule in rules:
        if not isinstance(rule, dict):
            continue
        matcher = rule.get("matcher", "")
        if not isinstance(matcher, str):
            continue
        covers_search = (
            not matcher.strip()
            or "ctx_search" in matcher
            or "mcp__" in matcher
        )
        if not covers_search:
            continue
        if any(
            _is_guard_hook_command(command, guard_script, platform)
            for command in _hook_commands(rule.get("hooks"))
        ):
            return True
    return False


CODEX_CONTEXT_HOOKS = {
    ("preToolUse", "pretooluse"),
    ("postToolUse", "posttooluse"),
    ("preCompact", "precompact"),
    ("sessionStart", "sessionstart"),
    ("userPromptSubmit", "userpromptsubmit"),
    ("stop", "stop"),
}


def _codex_rpc_response(
    process: subprocess.Popen[str], request_id: int, timeout: float
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    assert process.stdout is not None
    while time.monotonic() < deadline:
        ready, _, _ = select.select([process.stdout], [], [], 0.2)
        if not ready:
            if process.poll() is not None:
                break
            continue
        line = process.stdout.readline()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        if payload.get("id") == request_id:
            return payload
    raise GuardError(
        EXIT_PROJECT_BINDING_UNPROVEN,
        "cannot inspect Codex hook trust: app-server RPC timed out",
    )


def _load_codex_hook_entries(project_root: Path) -> list[dict[str, Any]]:
    executable = shutil.which("codex")
    if not executable:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "cannot inspect Codex hook trust: codex executable is unavailable",
        )
    process = subprocess.Popen(
        [executable, "app-server", "--stdio"],
        cwd=_resolved(project_root),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )

    def send(request_id: int, method: str, params: dict[str, Any]) -> None:
        assert process.stdin is not None
        process.stdin.write(
            json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "method": method,
                    "params": params,
                }
            )
            + "\n"
        )
        process.stdin.flush()

    try:
        send(
            1,
            "initialize",
            {
                "clientInfo": {
                    "name": "ndnsf-context-mode-guard",
                    "version": "1",
                },
                "capabilities": {"experimentalApi": True},
            },
        )
        initialized = _codex_rpc_response(process, 1, 8.0)
        if "error" in initialized:
            raise GuardError(
                EXIT_PROJECT_BINDING_UNPROVEN,
                "cannot inspect Codex hook trust: initialize failed",
            )
        assert process.stdin is not None
        process.stdin.write(
            json.dumps({"jsonrpc": "2.0", "method": "initialized"}) + "\n"
        )
        process.stdin.flush()
        send(2, "hooks/list", {"cwds": [str(_resolved(project_root))]})
        response = _codex_rpc_response(process, 2, 8.0)
        if "error" in response:
            raise GuardError(
                EXIT_PROJECT_BINDING_UNPROVEN,
                "cannot inspect Codex hook trust: hooks/list failed",
            )
        groups = response.get("result", {}).get("data", [])
        return [
            hook
            for group in groups
            if isinstance(group, dict)
            for hook in group.get("hooks", [])
            if isinstance(hook, dict)
        ]
    except (OSError, BrokenPipeError) as exc:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            f"cannot inspect Codex hook trust: {exc}",
        ) from exc
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def verify_codex_hook_trust(
    entries: Sequence[dict[str, Any]], guard_script: Path
) -> dict[str, Any]:
    expected = {
        f"context-mode:{event}": False for event, _command in CODEX_CONTEXT_HOOKS
    }
    expected["ndnsf-query-guard:preToolUse"] = False
    failures: list[str] = []
    checks: list[dict[str, Any]] = []

    for entry in entries:
        event = entry.get("eventName")
        command = entry.get("command")
        if not isinstance(event, str) or not isinstance(command, str):
            continue
        role = None
        for expected_event, command_name in CODEX_CONTEXT_HOOKS:
            if (
                event == expected_event
                and command.strip() == f"context-mode hook codex {command_name}"
            ):
                role = f"context-mode:{event}"
                break
        if (
            role is None
            and event == "preToolUse"
            and _is_guard_hook_command(command, guard_script, "codex")
        ):
            role = "ndnsf-query-guard:preToolUse"
        if role is None:
            continue
        expected[role] = True
        trust = entry.get("trustStatus")
        enabled = entry.get("enabled")
        if enabled is not True:
            failures.append(f"disabled Codex hook: {role}")
        if trust not in {"trusted", "managed"}:
            failures.append(f"untrusted Codex hook: {role} ({trust})")
        checks.append(
            {
                "role": role,
                "key": entry.get("key"),
                "trust_status": trust,
                "enabled": enabled,
            }
        )

    failures.extend(
        f"missing Codex hook: {role}" for role, found in expected.items() if not found
    )
    if failures:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "HOOK_TRUST_REQUIRED: " + "; ".join(failures),
        )
    return {"all_required_hooks_trusted": True, "hooks": checks}


def _verify_codex_configuration(
    client_home: Path,
    app_start: float,
    guard_script: Path,
    project_root: Path = PROJECT_ROOT,
) -> dict[str, Any]:
    home = _resolved(client_home)
    config = home / "config.toml"
    hooks = home / "hooks.json"
    for path in (config, hooks):
        if not path.is_file():
            raise GuardError(
                EXIT_PROJECT_BINDING_UNPROVEN, f"Context Mode host file is missing: {path}"
            )
    config_text = config.read_text(encoding="utf-8")
    required_config = {
        "plugin_hooks=true": r"(?m)^\s*plugin_hooks\s*=\s*true\s*$",
        "hooks=true": r"(?m)^\s*hooks\s*=\s*true\s*$",
        "[mcp_servers.context-mode]": (
            r"(?m)^\s*\[mcp_servers\.context-mode\]\s*$"
        ),
        "CONTEXT_MODE_PLATFORM=codex": (
            r"(?m)^\s*CONTEXT_MODE_PLATFORM\s*=\s*[\"']codex[\"']\s*$"
        ),
    }
    missing = [
        label
        for label, pattern in required_config.items()
        if re.search(pattern, config_text) is None
    ]
    if missing:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "Codex configuration is missing: " + ", ".join(missing),
        )
    try:
        hooks_payload = json.loads(hooks.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, f"hooks.json is invalid: {exc}"
        ) from exc
    hooks_text = json.dumps(hooks_payload)
    missing_hooks = [
        name
        for name in ("SessionStart", "UserPromptSubmit", "PreCompact")
        if name not in hooks_text
    ]
    if missing_hooks:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "hooks.json is missing hook(s): " + ", ".join(missing_hooks),
        )
    if not _has_enforced_guard_hook(hooks_payload, guard_script, "codex"):
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "hooks.json does not enforce scripts/context_mode_guard.py hook",
        )
    hook_trust = verify_codex_hook_trust(
        _load_codex_hook_entries(project_root), guard_script
    )
    config_mtime = config.stat().st_mtime
    hooks_mtime = hooks.stat().st_mtime
    # Codex rewrites config.toml while the app-server is running when it stores
    # hook trust hashes and unrelated UI settings.  Its whole-file mtime is
    # therefore not evidence that the host missed hook registration.  The
    # host-owned hook definitions live in hooks.json; trust/enabled state is
    # checked above through hooks/list, and real dispatch is checked separately
    # through the SessionDB prompt acceptance gate.
    if app_start <= hooks_mtime:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "HOST_RESTART_REQUIRED: app-server predates hooks.json",
        )
    return {
        "platform": "codex",
        "app_server_started_at": app_start,
        "newest_configuration_mtime": hooks_mtime,
        "config_toml_mtime": config_mtime,
        "hooks_json_mtime": hooks_mtime,
        "config_toml_mtime_requires_restart": False,
        "loaded_after_configuration": True,
        "hook_trust": hook_trust,
    }


CLAUDE_CONTEXT_HOOKS = {
    "PreToolUse": "pretooluse",
    "PostToolUse": "posttooluse",
    "SessionStart": "sessionstart",
    "PreCompact": "precompact",
    "UserPromptSubmit": "userpromptsubmit",
    "Stop": "stop",
}


def _has_claude_context_hook(
    hook_groups: dict[str, Any], group: str, command_name: str
) -> bool:
    rules = hook_groups.get(group)
    if not isinstance(rules, list):
        return False
    for command in _hook_commands(rules):
        lowered = command.lower()
        if f"/{command_name}.mjs" in lowered:
            return True
        if (
            "context-mode" in lowered
            and "hook" in lowered
            and "claude" in lowered
            and command_name in lowered
        ):
            return True
    return False


def _verify_claude_configuration(
    client_home: Path,
    client_start: float,
    guard_script: Path,
) -> dict[str, Any]:
    home = _resolved(client_home)
    settings = home / "settings.json"
    if not settings.is_file():
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            f"Claude Code settings file is missing: {settings}",
        )
    try:
        payload = json.loads(settings.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, f"settings.json is invalid: {exc}"
        ) from exc
    if not isinstance(payload, dict):
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, "settings.json must contain an object"
        )
    env = payload.get("env")
    if not isinstance(env, dict) or env.get("CONTEXT_MODE_PLATFORM") not in {
        "claude",
        "claude-code",
    }:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "Claude settings must set CONTEXT_MODE_PLATFORM to claude",
        )
    hook_groups = payload.get("hooks")
    if not isinstance(hook_groups, dict):
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "Claude settings.json does not contain a hooks object",
        )
    missing = [
        group
        for group, command_name in CLAUDE_CONTEXT_HOOKS.items()
        if not _has_claude_context_hook(hook_groups, group, command_name)
    ]
    if missing:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "Claude settings are missing Context Mode hook(s): "
            + ", ".join(missing),
        )
    if not _has_enforced_guard_hook(payload, guard_script, "claude-code"):
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "Claude PreToolUse does not enforce context_mode_guard.py "
            "hook --platform claude-code",
        )
    settings_mtime = settings.stat().st_mtime
    if client_start <= settings_mtime:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "HOST_RESTART_REQUIRED: Claude Code predates settings.json",
        )
    return {
        "platform": "claude-code",
        "client_started_at": client_start,
        "newest_configuration_mtime": settings_mtime,
        "loaded_after_configuration": True,
    }


def verify_host_configuration(
    client_home: Path,
    app_start: float,
    guard_script: Path = PROJECT_ROOT / "scripts" / "context_mode_guard.py",
    platform: str = "codex",
) -> dict[str, Any]:
    if platform == "codex":
        return _verify_codex_configuration(
            client_home,
            app_start,
            guard_script,
            project_root=guard_script.resolve(strict=False).parents[1],
        )
    if platform == "claude-code":
        return _verify_claude_configuration(client_home, app_start, guard_script)
    raise GuardError(EXIT_INVALID_QUERY, f"unsupported platform: {platform}")


def _parse_timestamp(value: str | float | int) -> float:
    if isinstance(value, (float, int)):
        numeric = float(value)
        while abs(numeric) >= 100_000_000_000:
            numeric /= 1000.0
        return numeric
    stripped = value.strip()
    try:
        numeric = float(stripped)
        while abs(numeric) >= 100_000_000_000:
            numeric /= 1000.0
        return numeric
    except ValueError:
        pass
    if stripped.endswith("Z"):
        stripped = stripped[:-1] + "+00:00"
    parsed = datetime.fromisoformat(stripped)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.timestamp()


def _marker_occurs(data: str, marker: str) -> bool:
    boundary_chars = r"A-Za-z0-9_.:-"
    return (
        re.search(
            rf"(?<![{boundary_chars}]){re.escape(marker)}(?![{boundary_chars}])", data
        )
        is not None
    )


def _database_latest_mtime(database: Path) -> float:
    candidates = [database, Path(str(database) + "-wal"), Path(str(database) + "-shm")]
    return max(path.stat().st_mtime for path in candidates if path.exists())


def verify_session_marker(
    *,
    session_db: Path,
    project_root: Path,
    marker: str,
    expect: str,
    after: Sequence[float] = (),
    allow_fixture_marker: bool = False,
) -> dict[str, Any]:
    marker_pattern = FIXTURE_MARKER_RE if allow_fixture_marker else REAL_MARKER_RE
    if not marker_pattern.fullmatch(marker):
        expected_prefix = "CTX_FIXTURE_*" if allow_fixture_marker else "CTX_HOST_ACCEPT_*"
        raise GuardError(
            EXIT_INVALID_QUERY, f"marker must be a never-reused {expected_prefix} value"
        )
    database = _resolved(session_db)
    root = _resolved(project_root)
    if not database.is_file():
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, f"SessionDB is missing: {database}"
        )
    try:
        with _connect_read_only(database) as connection:
            rows = connection.execute(
                """
                SELECT e.category, e.project_dir, e.created_at, e.data,
                       m.project_dir
                  FROM session_events AS e
             LEFT JOIN session_meta AS m ON m.session_id = e.session_id
                 WHERE instr(e.data, ?) > 0
                """,
                (marker,),
            ).fetchall()
    except sqlite3.Error as exc:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, f"cannot inspect SessionDB: {exc}"
        ) from exc
    rows = [row for row in rows if _marker_occurs(str(row[3]), marker)]

    if expect == "absent":
        if rows:
            raise GuardError(
                EXIT_CONTAMINATED_RESULT,
                f"marker is not unique; {len(rows)} pre-existing event(s) found",
            )
        return {"marker": marker, "expect": expect, "events": 0, "ok": True}
    if expect != "present":
        raise GuardError(EXIT_INVALID_QUERY, f"unsupported marker expectation: {expect}")
    if not after:
        raise GuardError(
            EXIT_INVALID_QUERY,
            "present-marker verification requires --after prompt-submission time",
        )
    threshold = max(float(value) for value in after)
    if not rows:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "real user-prompt marker was not captured in the project SessionDB",
        )

    user_prompt_rows = [row for row in rows if row[0] == "user-prompt"]
    other_event_count = len(rows) - len(user_prompt_rows)
    if not user_prompt_rows:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "marker exists only in non-user-prompt events; real prompt capture "
            "is not proven",
        )

    invalid: list[str] = []
    valid = 0
    for _category, event_project, created_at, _data, session_project in user_prompt_rows:
        if not event_project or _resolved(Path(event_project)) != root:
            invalid.append(f"wrong event project {event_project!r}")
            continue
        if not session_project or _resolved(Path(session_project)) != root:
            invalid.append(f"wrong session project {session_project!r}")
            continue
        try:
            event_time = _parse_timestamp(created_at)
        except (TypeError, ValueError):
            invalid.append(f"invalid event timestamp {created_at!r}")
            continue
        if event_time <= threshold:
            invalid.append(f"event timestamp {created_at!r} is not after {threshold}")
            continue
        valid += 1
    if invalid:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "marker capture failed binding checks: " + "; ".join(invalid),
        )
    if valid == 0:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "no valid current-project user-prompt marker event was found",
        )
    if _database_latest_mtime(database) <= threshold:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "SessionDB mtime did not advance after prompt submission",
        )
    return {
        "marker": marker,
        "expect": expect,
        "events": valid,
        "project_root": str(root),
        "after": threshold,
        "ignored_non_user_prompt_events": other_event_count,
        "ok": True,
    }


def verify_current_project_session(
    session_db: Path, project_root: Path, client_start: float
) -> dict[str, Any]:
    database = _resolved(session_db)
    root = str(_resolved(project_root))
    if not database.is_file():
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, f"SessionDB is missing: {database}"
        )
    try:
        with _connect_read_only(database) as connection:
            sessions = connection.execute(
                "SELECT COUNT(*), COALESCE(SUM(event_count), 0) "
                "FROM session_meta WHERE project_dir = ?",
                (root,),
            ).fetchone()
            prompts = connection.execute(
                "SELECT created_at FROM session_events "
                "WHERE project_dir = ? AND category = 'user-prompt' "
                "AND instr(data, 'CTX_FIXTURE_') = 0",
                (root,),
            ).fetchall()
    except sqlite3.Error as exc:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN, f"cannot inspect project sessions: {exc}"
        ) from exc
    recent_prompts = [
        created_at
        for (created_at,) in prompts
        if _parse_timestamp(created_at) > client_start
    ]
    if not sessions or sessions[0] == 0:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "NO_REAL_SESSION_EVENTS: no current-project session_meta row exists",
        )
    if sessions[1] == 0 or not recent_prompts:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "NO_REAL_SESSION_EVENTS: no post-restart current-project user-prompt exists",
        )
    if _database_latest_mtime(database) <= client_start:
        raise GuardError(
            EXIT_PROJECT_BINDING_UNPROVEN,
            "SessionDB did not advance after the current client process started",
        )
    return {
        "session_count": sessions[0],
        "recorded_event_count": sessions[1],
        "post_restart_user_prompts": len(recent_prompts),
        "ok": True,
    }


def run_health(
    *,
    project_root: Path,
    context_home: Path,
    client_home: Path | None = None,
    codex_home: Path | None = None,
    proc_root: Path = Path("/proc"),
    platform: str = "codex",
    scope: str = "project",
) -> dict[str, Any]:
    if platform not in PLATFORMS:
        raise GuardError(EXIT_INVALID_QUERY, f"unsupported platform: {platform}")
    if scope not in {"project", "active"}:
        raise GuardError(EXIT_INVALID_QUERY, f"unsupported health scope: {scope}")
    if client_home is not None and codex_home is not None:
        raise GuardError(
            EXIT_INVALID_QUERY, "use --client-home or --codex-home, not both"
        )
    if codex_home is not None:
        if platform != "codex":
            raise GuardError(
                EXIT_INVALID_QUERY,
                "--codex-home is only valid with --platform codex; "
                "use --client-home for Claude Code",
            )
        client_home = codex_home
    if client_home is None:
        client_home = default_client_home(platform)
    root = _resolved(project_root)
    context = None
    if scope == "active":
        context = load_feature_context(root)
        for filename in ("spec.md", "plan.md", "tasks.md"):
            path = context.feature_path / filename
            if not path.is_file():
                raise GuardError(
                    EXIT_SOURCE_MISMATCH,
                    f"active feature document is missing: {path}",
                )
        agents = context.root / "AGENTS.md"
        expected_pointer = f"at {context.feature_directory}/plan.md"
        if not agents.is_file() or expected_pointer not in agents.read_text(
            encoding="utf-8"
        ):
            raise GuardError(
                EXIT_SOURCE_MISMATCH,
                f"AGENTS.md does not point to {context.feature_directory}/plan.md",
            )

    content_db = find_project_content_db(root, context_home)
    source_checks = (
        verify_file_backed_sources(context, content_db)
        if context is not None
        else verify_project_file_backed_sources(root, content_db)
    )
    client_start = newest_client_start(platform, proc_root)
    host_check = verify_host_configuration(
        client_home,
        client_start,
        guard_script=root / "scripts" / "context_mode_guard.py",
        platform=platform,
    )
    session_db = _resolved(context_home) / "sessions" / content_db.name
    session_check = verify_current_project_session(
        session_db, root, client_start
    )
    return {
        "ok": True,
        "platform": platform,
        "scope": scope,
        "project_root": str(root),
        "active_feature": context.feature_directory if context else None,
        "content_db": str(content_db),
        "session_db": str(session_db),
        "file_backed_sources": source_checks,
        "host": host_check,
        "session": session_check,
    }


def _read_result_file(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8")


def default_context_home(platform: str) -> Path:
    if platform == "codex":
        return Path.home() / ".codex" / "context-mode"
    if platform == "claude-code":
        return Path.home() / ".claude" / "context-mode"
    raise GuardError(EXIT_INVALID_QUERY, f"unsupported platform: {platform}")


def default_client_home(platform: str) -> Path:
    if platform == "codex":
        return Path.home() / ".codex"
    if platform == "claude-code":
        return Path.home() / ".claude"
    raise GuardError(EXIT_INVALID_QUERY, f"unsupported platform: {platform}")


def _context_home_from_args(args: argparse.Namespace) -> Path:
    if args.context_home:
        return Path(args.context_home)
    return default_context_home(args.platform)


def _command_query(args: argparse.Namespace) -> dict[str, Any]:
    root = _resolved(Path(args.project_root))
    content_db = find_project_content_db(
        root, _context_home_from_args(args)
    )
    known_sources = (
        exact_source_selectors(indexed_source_labels(content_db))
        if args.lane in {"authority", "project"}
        else None
    )
    if args.lane == "authority":
        context = load_feature_context(root)
        verify_file_backed_sources(context, content_db)
    elif args.lane == "project":
        verify_project_file_backed_sources(root, content_db)
    request = build_search_request(
        lane=args.lane,
        queries=args.query,
        source=args.source,
        required=args.require,
        known_sources=known_sources,
    )
    if args.result_file:
        validate_search_results(
            _read_result_file(args.result_file),
            lane=args.lane,
            source=args.source,
            required=request["required_identifiers"],
        )
        request["result_validation"] = "passed"
    request["platform"] = args.platform
    return request


def _command_marker(args: argparse.Namespace) -> dict[str, Any]:
    root = _resolved(Path(args.project_root))
    context_home = _context_home_from_args(args)
    if args.session_db:
        session_db = Path(args.session_db)
    else:
        content_db = find_project_content_db(root, context_home)
        session_db = _resolved(context_home) / "sessions" / content_db.name
    thresholds = [_parse_timestamp(value) for value in args.after]
    if args.expect == "present":
        thresholds.append(
            newest_client_start(args.platform, Path(args.proc_root))
        )
    result = verify_session_marker(
        session_db=session_db,
        project_root=root,
        marker=args.value,
        expect=args.expect,
        after=thresholds,
    )
    result["platform"] = args.platform
    return result


def _command_hook(args: argparse.Namespace) -> dict[str, Any]:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        # Without a valid host event the repository cannot be attributed.
        # Passing through avoids breaking unrelated clients or repositories.
        return {}
    if not isinstance(payload, dict):
        return {}
    return evaluate_pretool_hook(
        payload,
        project_root=Path(args.project_root),
        context_home=_context_home_from_args(args),
    )


def _command_health(args: argparse.Namespace) -> dict[str, Any]:
    client_home = Path(args.client_home) if args.client_home else None
    legacy_codex_home = Path(args.codex_home) if args.codex_home else None
    return run_health(
        project_root=Path(args.project_root),
        context_home=_context_home_from_args(args),
        client_home=client_home,
        codex_home=legacy_codex_home,
        proc_root=Path(args.proc_root),
        platform=args.platform,
        scope=args.scope,
    )


def _add_platform_arguments(
    parser: argparse.ArgumentParser, *, include_client_home: bool = False
) -> None:
    parser.add_argument("--platform", choices=PLATFORMS, default="codex")
    parser.add_argument(
        "--context-home",
        help="override the platform default Context Mode store",
    )
    if include_client_home:
        parser.add_argument(
            "--client-home",
            help="override ~/.codex or ~/.claude for the selected platform",
        )
        parser.add_argument(
            "--codex-home",
            help="deprecated alias for --client-home with --platform codex",
        )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    query = subparsers.add_parser("query", help="build and validate a safe search")
    query.add_argument(
        "--lane", choices=("project", "authority", "session"), required=True
    )
    query.add_argument("--query", action="append", required=True)
    query.add_argument("--source", required=True)
    query.add_argument("--require", action="append", default=[], metavar="IDENTIFIER")
    query.add_argument("--result-file", help="saved ctx_search output, or - for stdin")
    query.add_argument("--project-root", default=".")
    _add_platform_arguments(query)
    query.set_defaults(handler=_command_query)

    health = subparsers.add_parser("health", help="verify Context Mode end to end")
    health.add_argument("--project-root", default=".")
    health.add_argument(
        "--scope",
        choices=("project", "active"),
        default="project",
        help="project cross-Spec layer (default) or strict active-Spec layer",
    )
    health.add_argument("--proc-root", default="/proc")
    _add_platform_arguments(health, include_client_home=True)
    health.set_defaults(handler=_command_health)

    marker = subparsers.add_parser("marker", help="verify real prompt marker capture")
    marker.add_argument("--value", required=True)
    marker.add_argument("--expect", choices=("absent", "present"), required=True)
    marker.add_argument(
        "--after",
        action="append",
        default=[],
        metavar="TIME",
        help="prompt-submission epoch or ISO time; required for present",
    )
    marker.add_argument("--project-root", default=".")
    marker.add_argument("--session-db")
    marker.add_argument("--proc-root", default="/proc")
    _add_platform_arguments(marker)
    marker.set_defaults(handler=_command_marker)

    hook = subparsers.add_parser(
        "hook", help="enforce query policy for a client PreToolUse event on stdin"
    )
    hook.add_argument("--project-root", default=str(PROJECT_ROOT))
    _add_platform_arguments(hook)
    hook.set_defaults(handler=_command_hook)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = args.handler(args)
    except GuardError as exc:
        print(
            json.dumps(
                {"ok": False, "exit_code": exc.code, "error": str(exc)},
                ensure_ascii=False,
                indent=2,
            )
        )
        return exc.code
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
