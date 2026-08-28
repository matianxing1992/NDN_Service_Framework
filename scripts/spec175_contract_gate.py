#!/usr/bin/env python3
"""Mechanical qualification gate for Spec175 streamed invocation.

The first repository run is intentionally expected-negative.  The same gate is
rerun without ``--expected-negative`` at T020 and must then report ``PASS``.
It never imports the native extension, so an ABI-stale host build cannot hide
source-contract drift.
"""

from __future__ import annotations

import argparse
import ast
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import textwrap
from typing import Iterable


SCHEMA = "spec175-qualification-manifest-v1"
FEATURE_BASENAME = "175-ndnsf-di-streamed-invocation"
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".c", ".cc", ".cpp", ".cxx"}
SKIP_DIRS = {
    ".git", ".codegraph", ".pytest_cache", "build", "results",
    "docs", "specs", "third_party", "node_modules", "__pycache__",
}
TLV_DECLARATION = re.compile(
    r"\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?P<value>0x[0-9A-Fa-f]+)\b"
)
CONTRACT_TLV = re.compile(
    r"^\|\s*`(?P<name>[A-Za-z_][A-Za-z0-9_]*)`\s*\|\s*`(?P<value>0x[0-9A-Fa-f]+)`\s*\|\s*$",
    re.MULTILINE,
)

# G0 seals the source that can affect the Spec175 binaries, Python package,
# formal tests, SIF, and experiment launchers.  The shared research worktree
# may legitimately contain unrelated papers or historical Spec evidence; those
# paths are recorded for audit but cannot block this feature's promotion.
IN_SCOPE_DIRS = (
    "ndn-service-framework/",
    "NDNSF-DistributedInference/cpp/",
    "NDNSF-DistributedInference/ndnsf_distributed_inference/",
    "NDNSF-DistributedRepo/",
    "pythonWrapper/",
    "examples/python/NDNSF-DistributedInference/",
    "tests/unit-tests/",
    "tests/integration-tests/",
    "tests/fixtures/spec175/",
    "packaging/ndnsf-di-container/",
    f"specs/{FEATURE_BASENAME}/",
    "tools/ndnsf-di/",
)
IN_SCOPE_FILES = {
    "wscript",
    "scripts/spec175_contract_gate.py",
    "scripts/run_spec175_integration_gate.py",
    "scripts/analyze_spec175_performance.py",
    "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py",
    "tests/wscript",
    "tests/python/test_streamed_invocation_api.py",
    "tests/python/test_ndnsf_di_core_contracts.py",
}


def _issue(code: str, detail: str, path: str = "") -> dict[str, str]:
    value = {"code": code, "detail": detail}
    if path:
        value["path"] = path
    return value


def _iter_source_files(root: Path) -> Iterable[Path]:
    for current, directory_names, file_names in os.walk(root):
        directory_names[:] = sorted(
            name for name in directory_names
            if name not in SKIP_DIRS
            and not (name.startswith(".") and name.endswith("-tmp"))
        )
        base = Path(current)
        for file_name in sorted(file_names):
            path = base / file_name
            if path.suffix.lower() in SOURCE_SUFFIXES:
                yield path


def scan_tlv_collisions(root: Path, start: int, end: int) -> list[dict[str, object]]:
    """Return every C/C++ enum-style TLV owner in the inclusive range."""
    root = root.resolve()
    rows: list[dict[str, object]] = []
    for path in _iter_source_files(root):
        try:
            source = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line_number, line in enumerate(source.splitlines(), 1):
            for match in TLV_DECLARATION.finditer(line):
                value = int(match.group("value"), 16)
                if start <= value <= end:
                    rows.append({
                        "name": match.group("name"),
                        "value": f"0x{value:04X}",
                        "path": str(path.relative_to(root)),
                        "line": line_number,
                    })
    return sorted(rows, key=lambda row: (int(str(row["value"]), 16), str(row["path"]), int(row["line"])))


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _stream_tlv_contract(feature_dir: Path) -> tuple[dict[str, int], list[dict[str, str]]]:
    path = feature_dir / "contracts/wire-protocol-v1.md"
    if not path.is_file():
        return {}, [_issue("MISSING_CONTRACT", "wire protocol contract is missing", str(path))]
    mapping = {
        match.group("name"): int(match.group("value"), 16)
        for match in CONTRACT_TLV.finditer(_read(path))
    }
    issues: list[dict[str, str]] = []
    # The conversation continuation contract extends the original 36 streamed
    # invocation assignments with the contiguous 0xF685..0xF698 block.
    if len(mapping) != 56:
        issues.append(_issue(
            "TLV_ASSIGNMENT_COUNT",
            f"expected 56 Spec175 TLVs, found {len(mapping)}",
            str(path),
        ))
        return mapping, issues
    values = sorted(mapping.values())
    if values != list(range(values[0], values[0] + len(values))):
        issues.append(_issue(
            "TLV_RANGE_NOT_CONTIGUOUS",
            "Spec175 TLV assignments are not one contiguous range",
            str(path),
        ))
    return mapping, issues


def check_tlv_contract(project_root: Path, feature_dir: Path) -> tuple[dict[str, object], list[dict[str, str]]]:
    mapping, issues = _stream_tlv_contract(feature_dir)
    if not mapping:
        return {"assignments": {}}, issues
    start, end = min(mapping.values()), max(mapping.values())
    owners = scan_tlv_collisions(project_root, start, end)
    collisions = [
        owner for owner in owners
        if mapping.get(str(owner["name"])) != int(str(owner["value"]), 16)
    ]
    mismatches = [
        owner for owner in owners
        if str(owner["name"]) in mapping
        and mapping[str(owner["name"])] != int(str(owner["value"]), 16)
    ]
    if collisions or mismatches:
        compact = ", ".join(
            f"{item['name']}={item['value']}@{item['path']}:{item['line']}"
            for item in (collisions + mismatches)[:12]
        )
        issues.append(_issue(
            "TLV_RANGE_COLLISION",
            f"0x{start:04X}..0x{end:04X} is already owned: {compact}",
            str(feature_dir / "contracts/wire-protocol-v1.md"),
        ))
    return {
        "range": f"0x{start:04X}..0x{end:04X}",
        "assignmentCount": len(mapping),
        "assignments": {name: f"0x{value:04X}" for name, value in mapping.items()},
        "existingOwners": owners,
        "collisionCount": len(collisions) + len(mismatches),
    }, issues


def _contains_per_token_distributed_call(path: Path) -> bool:
    if not path.is_file():
        return False
    source = _read(path)
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return bool(
            re.search(r"for\s+token_index\s+in\s+range", source)
            and "distributed_inference(" in source
        )
    for node in ast.walk(tree):
        if not isinstance(node, (ast.For, ast.AsyncFor)):
            continue
        target_names = {
            child.id for child in ast.walk(node.target) if isinstance(child, ast.Name)
        }
        if not ({"token_index", "tokenIndex", "token_epoch"} & target_names):
            continue
        for child in ast.walk(node):
            if not isinstance(child, ast.Call):
                continue
            function = child.func
            if isinstance(function, ast.Attribute) and function.attr in {
                "distributed_inference", "async_distributed_inference"
            }:
                return True
    return False


def _streaming_qwen_option_drift(path: Path) -> list[str]:
    """Return unsafe option markers from functions that submit request_streaming.

    The legacy diagnostic token loop may intentionally use ``useCache=false``.
    It is not a Spec175 subject.  Scope this check to the nested production
    function that actually calls ``request_streaming`` so the gate catches a
    full-context implementation without banning the labelled diagnostic.
    """
    if not path.is_file():
        return ["missing-user-source"]
    try:
        tree = ast.parse(textwrap.dedent(_read(path)))
    except SyntaxError:
        return ["unparseable-user-source"]
    drift: list[str] = []
    for function in (
            node for node in ast.walk(tree)
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))):
        has_stream_submit = any(
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "request_streaming"
            for node in ast.walk(function)
        )
        if not has_stream_submit:
            continue
        options: dict[str, object] = {}
        for node in ast.walk(function):
            if not isinstance(node, ast.Dict):
                continue
            for key, value in zip(node.keys, node.values):
                if not isinstance(key, ast.Constant) or key.value not in {
                        "useCache", "outputMode"}:
                    continue
                if isinstance(value, ast.Constant):
                    options[str(key.value)] = value.value
        if options.get("useCache") is not True:
            drift.append("request_streaming does not require useCache=true")
        if options.get("outputMode") != "TOKEN_STREAMING":
            drift.append(
                "request_streaming does not seal outputMode=TOKEN_STREAMING")
    if not any(
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "request_streaming"
            for node in ast.walk(tree)):
        drift.append("request_streaming call is absent")
    return drift


def check_source_contract(project_root: Path) -> list[dict[str, str]]:
    """Check the production source seams frozen by Spec175."""
    required_symbols = {
        "ndn-service-framework/InvocationStream.hpp": (
            "StreamRequestOptions", "InvocationEventMessage", "StreamCompletion",
        ),
        "ndn-service-framework/ServiceUser.hpp": ("RequestServiceStreaming",),
        "ndn-service-framework/ServiceProvider.hpp": (
            "addStreamingHandler", "StreamedResponseWriter",
        ),
        "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py": (
            "publish_event", "finish_stream",
        ),
        "pythonWrapper/ndnsf/service.py": ("request_streaming",),
    }
    issues: list[dict[str, str]] = []
    for relative, symbols in required_symbols.items():
        path = project_root / relative
        source = _read(path) if path.is_file() else ""
        for symbol in symbols:
            if symbol not in source:
                issues.append(_issue(
                    "MISSING_STREAMED_SYMBOL", f"missing {symbol}", relative,
                ))

    qwen_relative = "NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp"
    qwen_path = project_root / qwen_relative
    qwen_source = _read(qwen_path) if qwen_path.is_file() else ""
    bounds = [
        int(value) for value in re.findall(
            r"maxGeneratedTokens\s*<=\s*(\d+)", qwen_source
        )
    ]
    if not bounds or max(bounds) < 64:
        issues.append(_issue(
            "QWEN_MAX_TOKEN_BOUND_DRIFT",
            f"validated maxGeneratedTokens must cover 64; observed {bounds or 'none'}",
            qwen_relative,
        ))
    if "/LLM/Stage/" in qwen_source or "/LLM/Pipeline/Stage/" not in qwen_source:
        issues.append(_issue(
            "LEGACY_QWEN_ROLE_NAME",
            "Qwen session must use canonical /LLM/Pipeline/Stage/{index}",
            qwen_relative,
        ))

    placement_relative = (
        "NDNSF-DistributedInference/ndnsf_distributed_inference/"
        "adapters/qwen/placement.py"
    )
    placement_path = project_root / placement_relative
    placement_source = _read(placement_path) if placement_path.is_file() else ""
    placement_markers = {
        "QWEN_FP16_PROFILE_DRIFT": 'QWEN36_27B_PRECISION = "float16"',
        "QWEN_DECODE_MODE_DRIFT":
            'QWEN36_27B_DECODE_MODE = "single-token-autoregressive"',
        "QWEN_MODALITY_DRIFT": 'QWEN36_27B_MODALITY = "text-only"',
        "QWEN_MTP_MODE_DRIFT": "QWEN36_27B_MTP_ENABLED = False",
        "QWEN_THINKING_MODE_DRIFT":
            'QWEN36_27B_THINKING_MODE = "disabled"',
    }
    for code, marker in placement_markers.items():
        if marker not in placement_source:
            issues.append(_issue(code, f"missing pinned Qwen marker: {marker}",
                                 placement_relative))

    exporter_relative = "tools/ndnsf-di/export_spec175_qwen36_stateful_onnx.py"
    exporter_path = project_root / exporter_relative
    exporter_source = _read(exporter_path) if exporter_path.is_file() else ""
    for code, marker in {
        "QWEN_EXPORT_DECODE_MODE_DRIFT":
            'DECODE_MODE = "single-token-autoregressive"',
        "QWEN_EXPORT_MODALITY_DRIFT": 'MODALITY = "text-only"',
        "QWEN_EXPORT_MTP_MODE_DRIFT": "MTP_ENABLED = False",
        "QWEN_EXPORT_THINKING_MODE_DRIFT": 'THINKING_MODE = "disabled"',
    }.items():
        if marker not in exporter_source:
            issues.append(_issue(code, f"missing exporter boundary: {marker}",
                                 exporter_relative))

    user_relative = "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py"
    user_path = project_root / user_relative
    if _contains_per_token_distributed_call(user_path):
        issues.append(_issue(
            "PER_TOKEN_DISTRIBUTED_INFERENCE",
            "generation loop issues one distributed_inference call per token",
            user_relative,
        ))
    option_drift = _streaming_qwen_option_drift(user_path)
    if option_drift:
        issues.append(_issue(
            "QWEN_STREAMING_FULL_CONTEXT_OPTIONS",
            "; ".join(option_drift),
            user_relative,
        ))

    coordinator_relative = (
        "NDNSF-DistributedInference/ndnsf_distributed_inference/"
        "app_sdk/placement.py"
    )
    coordinator_path = project_root / coordinator_relative
    coordinator_source = (
        _read(coordinator_path) if coordinator_path.is_file() else "")
    coordinator_markers = (
        "GenerationExecutionContractV1", "TOKEN_FEEDBACK",
        "streaming_operation_stride", "max_generated_tokens",
        "generation_contract=generation_contract",
    )
    missing_coordinator = tuple(
        marker for marker in coordinator_markers
        if marker not in coordinator_source)
    if missing_coordinator:
        issues.append(_issue(
            "AUTOMATIC_STREAM_EPOCH_PLAN_MISSING",
            "automatic V3 stream plan lacks " + ", ".join(missing_coordinator),
            coordinator_relative,
        ))

    handler_relative = (
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp")
    handler_path = project_root / handler_relative
    handler_source = _read(handler_path) if handler_path.is_file() else ""
    handler_markers = (
        "generationConfigFromAuthenticatedRequest",
        "projection->generationContract.enabled",
        "authenticatedGeneration.maxEpochs",
        "runNativeEpochCoordinator",
    )
    executable_relative = "examples/DI_NativeProviderExecutable.cpp"
    executable_path = project_root / executable_relative
    executable_source = (
        _read(executable_path) if executable_path.is_file() else "")
    executable_markers = (
        'fields.find("executionPlanDigest")',
        "capability.planDigest != authenticatedPlanDigest",
    )
    missing_native = tuple(
        marker for marker in handler_markers
        if marker not in handler_source) + tuple(
        marker for marker in executable_markers
        if marker not in executable_source)
    if missing_native:
        issues.append(_issue(
            "NATIVE_STREAM_EPOCH_RUNTIME_DISABLED",
            "deployed native Provider lacks " + ", ".join(missing_native),
            handler_relative,
        ))
    return issues


def _expand_ids(text: str, prefix: str) -> set[str]:
    result: set[str] = set()
    pattern = re.compile(
        rf"\b{re.escape(prefix)}-(\d{{3}})([a-z]?)(?:\.\.{re.escape(prefix)}-(\d{{3}}))?\b"
    )
    for match in pattern.finditer(text):
        first = int(match.group(1))
        suffix = match.group(2)
        if suffix:
            result.add(f"{prefix}-{first:03d}{suffix}")
            continue
        last = int(match.group(3)) if match.group(3) else first
        if last < first:
            first, last = last, first
        result.update(f"{prefix}-{value:03d}" for value in range(first, last + 1))
    return result


def validate_requirement_coverage(feature_dir: Path) -> dict[str, object]:
    spec = _read(feature_dir / "spec.md")
    tasks = _read(feature_dir / "tasks.md")
    traceability = _read(feature_dir / "traceability.md")
    required_fr = _expand_ids(spec, "FR")
    required_sc = _expand_ids(spec, "SC")
    task_ids = _expand_ids(tasks, "FR") | _expand_ids(tasks, "SC")
    trace_ids = _expand_ids(traceability, "FR") | _expand_ids(traceability, "SC")
    required = required_fr | required_sc
    return {
        "functionalRequirements": len(required_fr),
        "successCriteria": len(required_sc),
        "missingFromTasks": sorted(required - task_ids),
        "missingFromTraceability": sorted(required - trace_ids),
    }


def check_documents(feature_dir: Path) -> list[dict[str, str]]:
    issues: list[dict[str, str]] = []
    required = (
        "spec.md", "plan.md", "tasks.md", "traceability.md", "data-model.md",
        "research.md", "quickstart.md", "experiment-plan.md",
        "contracts/api-contract.md", "contracts/wire-protocol-v1.md",
        "contracts/generation-state-machine-v1.md", "contracts/validation-contract.md",
    )
    texts: dict[str, str] = {}
    for relative in required:
        path = feature_dir / relative
        if not path.is_file():
            issues.append(_issue("MISSING_CONTRACT", "required document is missing", relative))
            continue
        texts[relative] = _read(path)
        if re.search(r"\{\{[^}\n]+\}\}|\[(?:TODO|TBD|FIXME)\]|<(?:TODO|TBD|FIXME)>", texts[relative], re.I):
            issues.append(_issue("UNRESOLVED_PLACEHOLDER", "document contains a hard placeholder", relative))

    defaults = {
        "maxEvents": "512",
        "interestWindow": "16",
        "interestLifetimeMs": "500",
        "maxEventRetries": "3",
        "publisherQueueCapacity": "64",
        "callbackQueueCapacity": "64",
        "reorderCapacity": "64",
        "retentionMs": "30000",
        "completionGraceMs": "5000",
        "maxEventWireBytes": "16384",
        "maxReplacements": "0",
    }
    for relative in ("contracts/api-contract.md", "data-model.md", "plan.md"):
        source = texts.get(relative, "")
        for name, value in defaults.items():
            if not re.search(rf"(?m)^.*\b{re.escape(name)}\b.*\b{value}\b", source):
                issues.append(_issue(
                    "CROSS_DOCUMENT_DEFAULT_DRIFT",
                    f"{name}={value} is absent or inconsistent",
                    relative,
                ))

    all_text = "\n".join(texts.values())
    normative_text = "\n".join(
        source for relative, source in texts.items()
        if relative not in {"tasks.md", "traceability.md"}
    )
    required_markers = {
        "SERVICE_ONLY_NORMAL_CONTRACT": "does not require a Provider list",
        "SAME_COLLABORATION_OWNER_CONTRACT": "same deferred collaboration",
        "SELECTED_PROVIDER_GRANT_CONTRACT": "only the final-role",
        "CANONICAL_ROLE_CONTRACT": "/LLM/Pipeline/Stage/",
        "REGISTERED_GENERATION_BOUND": "maxGeneratedTokens=64",
        "REGISTERED_EVENT_BOUND": "maxEvents=65",
        "RUNTIME_IMPORT_BOUNDARY": "no runtime Transformers/PyTorch import",
        "QWEN_TEXT_ONLY_BOUNDARY": "modality=text-only",
        "QWEN_SINGLE_TOKEN_DECODE_BOUNDARY": "decodeMode=single-token-autoregressive",
        "QWEN_MTP_DISABLED_BOUNDARY": "mtpEnabled=false",
        "QWEN_THINKING_DISABLED_BOUNDARY": "thinkingMode=disabled",
    }
    for code, marker in required_markers.items():
        if marker not in all_text:
            issues.append(_issue(code, f"missing contract marker: {marker}"))
    if "/LLM/Stage/" in normative_text:
        issues.append(_issue(
            "LEGACY_ROLE_IN_SPEC", "feature documents still contain /LLM/Stage/*",
        ))
    return issues


def _git_state(project_root: Path) -> tuple[str, list[str]]:
    try:
        revision = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=project_root, text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        status = subprocess.check_output(
            ["git", "status", "--porcelain=v1", "--untracked-files=all"],
            cwd=project_root, text=True, stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return "unknown", []
    return revision, [line.rstrip() for line in status.splitlines() if line.strip()]


def _status_path(line: str) -> str:
    value = line[3:] if len(line) >= 4 else line
    if " -> " in value:
        value = value.rsplit(" -> ", 1)[1]
    return value.strip().strip('"')


def _is_in_scope_status(line: str) -> bool:
    path = _status_path(line)
    if path.startswith("tests/python/test_spec175_") and path.endswith(".py"):
        return True
    return path in IN_SCOPE_FILES or any(path.startswith(prefix)
                                         for prefix in IN_SCOPE_DIRS)


def _is_source_subject_path(path: str) -> bool:
    """Return whether a path is an input to the sealed build subject.

    Feature documents are intentionally excluded from the build source seal:
    G0 writes their individual digests into its own manifest, while task-state
    and evidence updates must not invalidate already built native artifacts.
    The candidate binds both the source seal and the G0 manifest.
    """
    excluded = (
        f"specs/{FEATURE_BASENAME}/",
        "packaging/ndnsf-di-container/docs/",
    )
    return _is_in_scope_status("?? " + path) and not path.startswith(excluded)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _verify_source_seal(
    project_root: Path,
    seal_path: Path,
    revision: str,
    all_dirty: list[str],
) -> tuple[dict[str, object], list[dict[str, str]]]:
    """Verify a content-bound seal for an intentionally dirty worktree.

    A dirty tree is not promotion evidence by itself.  A seal is accepted only
    when it binds the exact HEAD, exact in-scope status set, and current hash of
    every changed file.  This keeps local source work reproducible without
    weakening the default clean-tree requirement.
    """
    issues: list[dict[str, str]] = []
    try:
        payload = json.loads(seal_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return {}, [_issue("SOURCE_SEAL_INVALID", str(error), str(seal_path))]
    if payload.get("schemaVersion") != "spec175-source-seal-v1":
        issues.append(_issue("SOURCE_SEAL_SCHEMA", "unsupported source seal schema", str(seal_path)))
    if str(payload.get("sourceRevision", "")) != revision:
        issues.append(_issue(
            "SOURCE_SEAL_REVISION",
            "source seal HEAD differs from the current repository HEAD",
            str(seal_path),
        ))
    current_dirty = [
        line for line in all_dirty
        if _is_in_scope_status(line)
        and _is_source_subject_path(_status_path(line))
    ]
    entries = payload.get("dirtyFiles")
    if not isinstance(entries, dict):
        entries = {}
        issues.append(_issue("SOURCE_SEAL_FILES", "dirtyFiles must be an object", str(seal_path)))
    current_paths = {_status_path(line) for line in current_dirty}
    sealed_paths = {str(path) for path in entries}
    if current_paths != sealed_paths:
        issues.append(_issue(
            "SOURCE_SEAL_PATHS",
            f"sealed paths differ (current={len(current_paths)}, sealed={len(sealed_paths)})",
            str(seal_path),
        ))
    mismatches: list[str] = []
    for line in current_dirty:
        path_name = _status_path(line)
        entry = entries.get(path_name)
        if not isinstance(entry, dict):
            mismatches.append(path_name)
            continue
        path = project_root / path_name
        actual = _sha256(path) if path.is_file() else None
        if entry.get("status") != line[:2] or entry.get("sha256") != actual:
            mismatches.append(path_name)
    if mismatches:
        issues.append(_issue(
            "SOURCE_SEAL_CONTENT",
            "changed files differ from source seal: " + ", ".join(sorted(mismatches)[:12]),
            str(seal_path),
        ))
    return {
        "path": str(seal_path),
        "schemaVersion": payload.get("schemaVersion"),
        "sourceRevision": payload.get("sourceRevision"),
        "dirtyFileCount": len(sealed_paths),
        "verified": not issues,
    }, issues


def build_manifest(
    project_root: Path,
    feature_dir: Path,
    *,
    expected_negative: bool,
    source_seal: Path | None = None,
) -> dict[str, object]:
    coverage = validate_requirement_coverage(feature_dir)
    tlv, tlv_issues = check_tlv_contract(project_root, feature_dir)
    blockers = check_documents(feature_dir) + tlv_issues + check_source_contract(project_root)
    for field in ("missingFromTasks", "missingFromTraceability"):
        if coverage[field]:
            blockers.append(_issue(
                "REQUIREMENT_COVERAGE_GAP",
                f"{field}: {', '.join(coverage[field])}",
                str(feature_dir / "traceability.md"),
            ))
    revision, all_dirty = _git_state(project_root)
    dirty = [line for line in all_dirty if _is_in_scope_status(line)]
    unrelated_dirty = [line for line in all_dirty if not _is_in_scope_status(line)]
    source_seal_record: dict[str, object] = {}
    if source_seal is not None:
        source_seal_record, seal_issues = _verify_source_seal(
            project_root, source_seal, revision, all_dirty)
        blockers.extend(seal_issues)
    if dirty and not source_seal_record.get("verified", False):
        blockers.append(_issue(
            "DIRTY_INPUT_TREE",
            f"{len(dirty)} dirty in-scope paths; promotion evidence requires a sealed source subject",
        ))
    blockers = sorted(blockers, key=lambda item: (item["code"], item.get("path", ""), item["detail"]))
    status = "PASS" if not blockers else ("BLOCKED_EXPECTED" if expected_negative else "BLOCKED")
    documents = {}
    for path in sorted(feature_dir.rglob("*.md")):
        documents[str(path.relative_to(project_root))] = _sha256(path)
    return {
        "schemaVersion": SCHEMA,
        "gate": "G0",
        "feature": FEATURE_BASENAME,
        "status": status,
        "expectedNegative": expected_negative,
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "sourceRevision": revision,
        "dirtyPaths": dirty,
        "unrelatedDirtyPathCount": len(unrelated_dirty),
        "sourceSeal": source_seal_record,
        "requirementCoverage": coverage,
        "tlvContract": tlv,
        "documentDigests": documents,
        "blockers": blockers,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", default=".")
    parser.add_argument("--feature-dir", default=f"specs/{FEATURE_BASENAME}")
    parser.add_argument(
        "--output", default="results/spec175/g0/qualification-manifest-v1.json",
    )
    parser.add_argument("--expected-negative", action="store_true")
    parser.add_argument(
        "--source-seal", default="",
        help=(
            "Verify an exact source seal for the intentionally dirty worktree; "
            "without this option any in-scope dirty path blocks G0."),
    )
    args = parser.parse_args(argv)

    project_root = Path(args.project_root).resolve()
    feature_dir = Path(args.feature_dir)
    if not feature_dir.is_absolute():
        feature_dir = (project_root / feature_dir).resolve()
    output = Path(args.output)
    if not output.is_absolute():
        output = (project_root / output).resolve()
    manifest = build_manifest(
        project_root, feature_dir, expected_negative=args.expected_negative,
        source_seal=(Path(args.source_seal).expanduser().resolve()
                     if args.source_seal else None),
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8",
    )
    print(json.dumps({
        "status": manifest["status"],
        "blockerCount": len(manifest["blockers"]),
        "output": str(output),
    }, sort_keys=True))
    if args.expected_negative:
        return 0 if manifest["status"] == "BLOCKED_EXPECTED" else 3
    return 0 if manifest["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
