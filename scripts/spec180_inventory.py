#!/usr/bin/env python3
"""Build and validate the source-bound local-suite inventory owned by Spec181.

The inventory is deliberately a planning artifact.  It discovers the exact
native/Python selectors and declares the three YOLO MiniNDN cases, but it
never executes a test.  Execution belongs to the supervised local-gate runner
owned by Spec181 T008.  A missing binary, selector, or case entrypoint is an
error; silently omitting an item would make a later ``PASS`` meaningless.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Iterable, Mapping, Sequence


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "spec180-local-suite-inventory-v1"
BACKEND = "cpu-onnxruntime"
CLEANUP_POLICY = "bounded-child-cleanup-v1"
DEFAULT_TIMEOUT_SECONDS = 120
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_SOURCE_RE = re.compile(r"^[0-9a-f]{40}(?:-dirty)?$")
_ENTRY_KINDS = {"cpp-suite", "cpp-selector", "python-selector", "minindn-case"}
_INVENTORY_KEYS = {
    "schema", "candidateId", "candidateDigest", "sourceRevision",
    "effectiveConfigDigest", "backend", "timeoutSeconds", "cleanupPolicy",
    "entries", "inventoryDigest", "inputIdentity", "inputDigest",
}
_ENTRY_KEYS = {
    "id", "kind", "path", "selector", "case", "command", "commandDigest",
    "workingDirectory", "artifactSha256", "sourceRevision", "candidateId",
    "candidateDigest", "effectiveConfigDigest", "backend", "timeoutSeconds",
    "cleanupPolicy", "evidencePath",
}


class InventoryError(ValueError):
    """Raised when an inventory cannot represent the complete local gate."""


DEFAULT_CASES = (
    # Spec181 retains the maintained runner and qualifies only YOLO network cases.
    ("Y-A", "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py", ("--case", "Y-A")),
    ("Y-B", "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py", ("--case", "Y-B")),
    ("Y-N", "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py", ("--case", "Y-N")),
)

# The supervisor selects an explicitly supplied policy; it never derives roles.
CASE_CONFIG_ENV = {case: "SPEC181_LOCAL_CONFIG_" + case.replace("-", "_")
                   for case, _path, _args in DEFAULT_CASES}


def validate_case_contract(case: str, path: str, args: Sequence[str]) -> None:
    """Bind the case name to its maintained source and exact workload arguments."""
    registered = {name: (source, tuple(arguments))
                  for name, source, arguments in DEFAULT_CASES}
    if case not in registered:
        raise InventoryError("UNREGISTERED_CASE:" + case)
    if (path, tuple(args)) != registered[case]:
        raise InventoryError("CASE_CONTRACT_MISMATCH:" + case)


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def canonical_digest(value: Any) -> str:
    return digest_bytes(canonical_bytes(value))


def local_launch_configuration(root: Path | str, environment: Mapping[str, str],
                               timeout_seconds: int) -> dict[str, Any]:
    """Describe actual supervisor launch inputs without recording env values.

    Native dependencies and external input-file contents retain their own
    identity checks; this record binds the configuration passed to children.
    """
    if not isinstance(environment, Mapping) or any(
            not isinstance(key, str) or not isinstance(value, str)
            for key, value in environment.items()):
        raise InventoryError("ENVIRONMENT_MUST_BE_STRING_MAP")
    if any(not key or "=" in key or "\0" in key or "\0" in value
           for key, value in environment.items()):
        raise InventoryError("CONFIG_INVALID_ENVIRONMENT")
    if "SPEC180_CASE_OUTPUT_DIR" in environment:
        raise InventoryError("CONFIG_RESERVED_ENVIRONMENT:SPEC180_CASE_OUTPUT_DIR")
    if environment.get("SPEC180_RUNTIME_SIF"):
        raise InventoryError("LOCAL_GATE_SIF_RUNTIME_UNSUPPORTED")
    if any(name in environment for name in CASE_CONFIG_ENV.values()):
        missing = [name for name in CASE_CONFIG_ENV.values() if not environment.get(name)]
        if missing:
            raise InventoryError("LOCAL_CASE_CONFIG_INCOMPLETE:" + ",".join(missing))
        if "SPEC180_YOLO_CONFIG" in environment:
            raise InventoryError("LOCAL_CASE_CONFIG_AMBIGUOUS")
        for name in CASE_CONFIG_ENV.values():
            if not Path(environment[name]).is_absolute():
                raise InventoryError("LOCAL_CASE_CONFIG_NOT_ABSOLUTE:" + name)
    if not isinstance(timeout_seconds, int) or timeout_seconds <= 0:
        raise InventoryError("INVALID_TIMEOUT_SECONDS")
    interpreter = Path(sys.executable)
    try:
        resolved = interpreter.resolve(strict=True)
        digest = hashlib.sha256()
        with resolved.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except (OSError, RuntimeError) as exc:
        raise InventoryError("CONFIG_INTERPRETER_UNAVAILABLE") from exc
    return {
        "schema": "spec181-local-launch-configuration-v1",
        "workingDirectory": str(Path(root).resolve()),
        "environmentDigest": canonical_digest(dict(environment)),
        "interpreter": {"path": str(interpreter), "resolvedPath": str(resolved),
                        "sha256": "sha256:" + digest.hexdigest()},
        "backend": BACKEND,
        "timeoutSeconds": timeout_seconds,
        "cleanupPolicy": CLEANUP_POLICY,
        "caseOutputLayout": "{outputRoot}/{entryId}/case-output",
        "caseOutputVariable": "SPEC180_CASE_OUTPUT_DIR",
    }


def local_input_identity(root: Path | str, environment: Mapping[str, str]) -> dict[str, Any]:
    """Snapshot configured external inputs; private file contents never escape.

    Missing environment inputs stay explicit. The maintained case validator
    owns required inputs and cryptographic validity; this owner detects drift
    across inventory discovery and the complete local qualification run.
    """
    root = Path(root).resolve()
    home = Path(environment.get("HOME") or Path.home())

    def path_for(value: str) -> Path:
        if value == "~" or value.startswith("~/"):
            return root / home / value[2:]
        return root / Path(value).expanduser()

    def file_record(path: Path) -> dict[str, Any]:
        before = path.stat()
        if not path.is_file():
            raise InventoryError("INPUT_NOT_FILE:" + str(path))
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        after = path.stat()
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
                before.st_ctime_ns) != (after.st_dev, after.st_ino, after.st_size,
                                       after.st_mtime_ns, after.st_ctime_ns):
            raise InventoryError("INPUT_CHANGED_DURING_HASH:" + str(path))
        return {"path": str(path), "resolvedPath": str(path.resolve(strict=True)),
                "sha256": "sha256:" + digest.hexdigest(), "size": after.st_size,
                "mode": after.st_mode & 0o777}

    result: dict[str, Any] = {"schema": "spec181-local-input-identity-v1", "inputs": {}}
    inputs = result["inputs"]
    file_names = (
        "NDNSF_DI_ENVELOPE_KEY_FILE", "SPEC180_YOLO_CATALOGUE_REGISTRY",
        "SPEC180_YOLO_OFFER_TRUST_ROOT", "SPEC180_YOLO_TOPOLOGY", "SPEC180_YOLO_CONFIG",
    ) + tuple(CASE_CONFIG_ENV.values())
    map_names = ("SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP", "SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP",
                 "SPEC181_PROVIDER_RECIPIENT_KEY_MAP")
    try:
        for name in file_names + map_names:
            value = environment.get(name)
            if not value:
                inputs[name] = {"status": "UNCONFIGURED"}
                continue
            path = path_for(value)
            record = file_record(path)
            if name in map_names:
                document = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(document, dict) or any(
                        not isinstance(key, str) or not isinstance(value, str) or not value
                        for key, value in document.items()):
                    raise InventoryError("INPUT_KEY_MAP_INVALID:" + name)
                record["referencedFiles"] = {key: file_record(path_for(value))
                                             for key, value in sorted(document.items())}
                if file_record(path) != {k: v for k, v in record.items() if k != "referencedFiles"}:
                    raise InventoryError("INPUT_CHANGED_DURING_HASH:" + name)
            inputs[name] = record
        package_value = environment.get("SPEC180_YOLO_CANONICAL_PACKAGE")
        if package_value:
            package = path_for(package_value)
            if not package.is_dir():
                raise InventoryError("INPUT_NOT_DIRECTORY:" + str(package))
            files = {}
            for path in sorted(package.rglob("*")):
                if path.is_symlink() and path.is_dir():
                    raise InventoryError("INPUT_DIRECTORY_SYMLINK:" + str(path))
                if path.is_file() or path.is_symlink():
                    files[str(path.relative_to(package))] = file_record(path)
            inputs["SPEC180_YOLO_CANONICAL_PACKAGE"] = {
                "path": str(package), "resolvedPath": str(package.resolve(strict=True)), "files": files}
        else:
            inputs["SPEC180_YOLO_CANONICAL_PACKAGE"] = {"status": "UNCONFIGURED"}
        epoch = environment.get("SPEC181_PROTECTION_EPOCH", "")
        if epoch and epoch != "plaintext-v1":
            config_root = path_for(environment.get("NDNSF_SPEC180_CONFIG_ROOT") or
                                   str(home / ".config/ndnsf/spec180"))
            inputs["protectedAuthorityPrivateKey"] = file_record(config_root / "artifact-policy-authority.key")
        else:
            inputs["protectedAuthorityPrivateKey"] = {"status": "UNCONFIGURED"}
        manifest = path_for(environment.get("SPEC180_NATIVE_BUILD_MANIFEST") or
                            "build-system-j2/spec180-native-build.json")
        inputs["nativeBuildManifest"] = (file_record(manifest) if manifest.is_file()
                                         else {"status": "UNCONFIGURED"})
    except (OSError, RuntimeError, UnicodeError, json.JSONDecodeError) as exc:
        raise InventoryError("INPUT_IDENTITY_UNREADABLE") from exc
    return result


def _require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise InventoryError("INVALID_DIGEST:" + label)
    return value


def _require_identity(candidate_id: Any, candidate_digest: Any,
                     source_revision: Any, config_digest: Any) -> None:
    if not isinstance(candidate_id, str) or not candidate_id:
        raise InventoryError("INVALID_STRING:candidateId")
    _require_digest(candidate_digest, "candidateDigest")
    if not isinstance(source_revision, str) or not _SOURCE_RE.fullmatch(
            source_revision):
        raise InventoryError("INVALID_SOURCE_REVISION")
    _require_digest(config_digest, "effectiveConfigDigest")


def _relative_file(root: Path, value: Path | str, label: str,
                   *, executable: bool = False) -> tuple[str, Path]:
    path = Path(value)
    if path.is_absolute():
        resolved = path.resolve()
    else:
        resolved = (root / path).resolve()
    try:
        relative = resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise InventoryError("PATH_ESCAPES_ROOT:" + label) from exc
    if not resolved.is_file():
        raise InventoryError("FILE_MISSING:" + label)
    if executable and not os.access(resolved, os.X_OK):
        raise InventoryError("FILE_NOT_EXECUTABLE:" + label)
    return relative.as_posix(), resolved


def _file_digest(path: Path) -> str:
    try:
        return digest_bytes(path.read_bytes())
    except OSError as exc:
        raise InventoryError("FILE_READ_FAILED:" + str(path)) from exc


def parse_boost_list(text: str) -> tuple[str, ...]:
    """Return all leaf selectors from Boost.Test ``--list_content`` output.

    Boost prints suites and test cases as an indentation tree.  Top-level
    tests are valid selectors by themselves; nested leaves are returned as
    ``suite/test``.  A trailing ``*`` is registration decoration, not part of
    the selector.
    """
    stack: list[dict[str, Any]] = []
    leaves: list[str] = []
    for raw in text.splitlines():
        if not raw.strip():
            continue
        expanded = raw.expandtabs(4)
        indent = len(expanded) - len(expanded.lstrip(" "))
        name = expanded.strip().rstrip("*").strip()
        if not name:
            continue
        while stack and indent <= int(stack[-1]["indent"]):
            node = stack.pop()
            if not node["has_child"]:
                leaves.append(str(node["path"]))
        parent = stack[-1] if stack else None
        if parent is not None:
            parent["has_child"] = True
            path = str(parent["path"]) + "/" + name
        else:
            path = name
        stack.append({"indent": indent, "path": path, "has_child": False})
    while stack:
        node = stack.pop()
        if not node["has_child"]:
            leaves.append(str(node["path"]))
    return tuple(sorted(set(leaves)))


def parse_pytest_collect_output(text: str, root: Path) -> tuple[str, ...]:
    """Extract collected pytest node IDs and bind them to ``root``."""
    selectors: set[str] = set()
    for raw in text.splitlines():
        line = raw.strip()
        # Node IDs may end in parameter brackets; only the ``.py::`` marker is
        # needed to distinguish them from pytest's summary/warning lines.
        if "::" not in line or ".py::" not in line:
            continue
        # pytest -q emits one node ID per line; ignore summary/warning text.
        candidate = line.split(" ", 1)[0]
        if "::" not in candidate:
            continue
        path_part = candidate.split("::", 1)[0]
        try:
            relative, _ = _relative_file(root, path_part, "pytest-selector")
        except InventoryError:
            continue
        selectors.add(relative + candidate[len(path_part):])
    if not selectors:
        raise InventoryError("PYTEST_COLLECTION_EMPTY")
    return tuple(sorted(selectors))


def _run_listing(command: Sequence[str], *, cwd: Path, label: str, environment: Mapping[str, str],
                 timeout_seconds: float = 30.0) -> str:
    try:
        completed = subprocess.run(
            list(command), cwd=str(cwd), env=dict(environment), text=True, capture_output=True,
            check=False, timeout=timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise InventoryError("COMMAND_PROBE_FAILED:" + label) from exc
    output = (completed.stdout or "") + (completed.stderr or "")
    if completed.returncode != 0:
        raise InventoryError("COMMAND_PROBE_NONZERO:" + label)
    return output


def discover_integration_selectors(root: Path, binary: Path | str,
                                   listing: str | None = None, *, environment: Mapping[str, str] | None = None
                                   ) -> tuple[str, ...]:
    relative, resolved = _relative_file(
        root, binary, "integration-binary", executable=True)
    del relative
    text = listing if listing is not None else _run_listing(
        (str(resolved), "--list_content", "--log_level=nothing"),
        cwd=root, label="integration-list", environment=environment or {})
    selectors = parse_boost_list(text)
    if not selectors:
        raise InventoryError("INTEGRATION_SELECTOR_EMPTY")
    return selectors


def discover_python_selectors(root: Path,
                              selectors: Iterable[str] | None = None, *,
                              environment: Mapping[str, str] | None = None
                              ) -> tuple[str, ...]:
    if selectors is not None:
        result = tuple(sorted(set(str(item) for item in selectors)))
        if not result:
            raise InventoryError("PYTEST_SELECTOR_EMPTY")
        for item in result:
            path = item.split("::", 1)[0]
            _relative_file(root, path, "pytest-selector")
        return result
    files = sorted({path for pattern in ("test_spec180_*.py", "test_spec181_*.py")
                    for path in root.glob("tests/python/" + pattern)})
    if not files:
        raise InventoryError("PYTEST_SOURCE_FILES_EMPTY")
    command = [sys.executable, "-m", "pytest", "--collect-only", "-q"]
    command.extend(str(path.relative_to(root)) for path in files)
    env = dict(environment or {})
    try:
        completed = subprocess.run(
            command, cwd=str(root), env=env, text=True, capture_output=True,
            check=False, timeout=120.0,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise InventoryError("PYTEST_COLLECTION_FAILED") from exc
    output = (completed.stdout or "") + (completed.stderr or "")
    if completed.returncode != 0:
        raise InventoryError("PYTEST_COLLECTION_NONZERO")
    return parse_pytest_collect_output(output, root)


def _entry(entry_id: str, kind: str, *, root: Path, path: str,
           artifact: Path, command: Sequence[str], candidate_id: str,
           candidate_digest: str, source_revision: str,
           config_digest: str, timeout_seconds: int,
           selector: str | None = None, case: str | None = None
           ) -> dict[str, Any]:
    if kind not in _ENTRY_KINDS:
        raise InventoryError("UNKNOWN_ENTRY_KIND:" + kind)
    record: dict[str, Any] = {
        "id": entry_id,
        "kind": kind,
        "path": path,
        "selector": selector,
        "case": case,
        "command": list(command),
        "commandDigest": canonical_digest(list(command)),
        "workingDirectory": ".",
        "artifactSha256": _file_digest(artifact),
        "sourceRevision": source_revision,
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "effectiveConfigDigest": config_digest,
        "backend": BACKEND,
        "timeoutSeconds": timeout_seconds,
        "cleanupPolicy": CLEANUP_POLICY,
        "evidencePath": "evidence/local/" + entry_id + ".json",
    }
    return record


def _case_registry(root: Path, cases: Sequence[tuple[str, str, Sequence[str]]]
                   ) -> list[tuple[str, str, Path, Sequence[str]]]:
    result = []
    seen: set[str] = set()
    for case, path, args in cases:
        if case in seen:
            raise InventoryError("DUPLICATE_CASE:" + case)
        seen.add(case)
        if case not in {item[0] for item in DEFAULT_CASES}:
            raise InventoryError("UNREGISTERED_CASE:" + case)
        relative, resolved = _relative_file(root, path, "case-" + case)
        validate_case_contract(case, relative, args)
        result.append((case, relative, resolved, tuple(args)))
    expected = [item[0] for item in DEFAULT_CASES]
    if [item[0] for item in result] != expected:
        raise InventoryError("CASE_REGISTRY_INCOMPLETE")
    return result


def validate_inventory(inventory: Mapping[str, Any]) -> dict[str, Any]:
    if not isinstance(inventory, Mapping):
        raise InventoryError("INVENTORY_NOT_OBJECT")
    unknown = sorted(set(inventory) - _INVENTORY_KEYS)
    if unknown:
        raise InventoryError("UNKNOWN_INVENTORY_FIELD:" + ",".join(unknown))
    missing = sorted(_INVENTORY_KEYS - set(inventory))
    if missing:
        raise InventoryError("MISSING_INVENTORY_FIELD:" + ",".join(missing))
    if inventory.get("schema") != SCHEMA:
        raise InventoryError("INVENTORY_SCHEMA_UNSUPPORTED")
    identity = inventory.get("inputIdentity")
    if (not isinstance(identity, dict) or identity.get("schema") != "spec181-local-input-identity-v1"
            or not isinstance(identity.get("inputs"), dict)
            or inventory.get("inputDigest") != canonical_digest(identity)):
        raise InventoryError("INVENTORY_INPUT_IDENTITY_INVALID")
    _require_identity(
        inventory.get("candidateId"), inventory.get("candidateDigest"),
        inventory.get("sourceRevision"), inventory.get("effectiveConfigDigest"),
    )
    if inventory.get("backend") != BACKEND:
        raise InventoryError("INVENTORY_BACKEND_MISMATCH")
    timeout = inventory.get("timeoutSeconds")
    if not isinstance(timeout, int) or timeout <= 0:
        raise InventoryError("INVENTORY_TIMEOUT_INVALID")
    if inventory.get("cleanupPolicy") != CLEANUP_POLICY:
        raise InventoryError("INVENTORY_CLEANUP_POLICY_MISMATCH")
    entries = inventory.get("entries")
    if not isinstance(entries, list) or not entries:
        raise InventoryError("INVENTORY_ENTRIES_EMPTY")
    ids: set[str] = set()
    for entry in entries:
        if not isinstance(entry, Mapping):
            raise InventoryError("ENTRY_NOT_OBJECT")
        unknown_entry = sorted(set(entry) - _ENTRY_KEYS)
        if unknown_entry:
            raise InventoryError("UNKNOWN_ENTRY_FIELD:" + ",".join(unknown_entry))
        missing_entry = sorted(_ENTRY_KEYS - set(entry))
        if missing_entry:
            raise InventoryError("MISSING_ENTRY_FIELD:" + ",".join(missing_entry))
        entry_id = entry.get("id")
        if (not isinstance(entry_id, str)
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", entry_id)
                or entry_id in ids):
            raise InventoryError("DUPLICATE_OR_INVALID_ENTRY_ID")
        ids.add(entry_id)
        if entry.get("kind") not in _ENTRY_KINDS:
            raise InventoryError("ENTRY_KIND_INVALID:" + str(entry.get("kind")))
        if not isinstance(entry.get("path"), str) or not entry["path"] \
                or Path(entry["path"]).is_absolute() \
                or ".." in Path(entry["path"]).parts:
            raise InventoryError("ENTRY_PATH_INVALID:" + entry_id)
        command = entry.get("command")
        if (not isinstance(command, list) or not command
                or not all(isinstance(item, str) and item for item in command)):
            raise InventoryError("ENTRY_COMMAND_INVALID:" + entry_id)
        if entry.get("commandDigest") != canonical_digest(command):
            raise InventoryError("ENTRY_COMMAND_DIGEST_MISMATCH:" + entry_id)
        if entry.get("workingDirectory") != ".":
            raise InventoryError("ENTRY_CWD_MISMATCH:" + entry_id)
        _require_digest(entry.get("artifactSha256"), "artifactSha256")
        if entry.get("sourceRevision") != inventory.get("sourceRevision"):
            raise InventoryError("ENTRY_SOURCE_REVISION_MISMATCH:" + entry_id)
        if entry.get("candidateId") != inventory.get("candidateId") \
                or entry.get("candidateDigest") != inventory.get("candidateDigest"):
            raise InventoryError("ENTRY_CANDIDATE_MISMATCH:" + entry_id)
        if entry.get("effectiveConfigDigest") != inventory.get(
                "effectiveConfigDigest"):
            raise InventoryError("ENTRY_CONFIG_DIGEST_MISMATCH:" + entry_id)
        if entry.get("backend") != BACKEND:
            raise InventoryError("ENTRY_BACKEND_MISMATCH:" + entry_id)
        if entry.get("timeoutSeconds") != timeout:
            raise InventoryError("ENTRY_TIMEOUT_MISMATCH:" + entry_id)
        if entry.get("cleanupPolicy") != CLEANUP_POLICY:
            raise InventoryError("ENTRY_CLEANUP_POLICY_MISMATCH:" + entry_id)
        evidence_path = entry.get("evidencePath")
        if (not isinstance(evidence_path, str) or not evidence_path
                or Path(evidence_path).is_absolute()
                or ".." in Path(evidence_path).parts):
            raise InventoryError("ENTRY_EVIDENCE_PATH_INVALID:" + entry_id)
        if entry.get("kind") == "minindn-case":
            if not isinstance(entry.get("case"), str) or entry.get("selector") is not None:
                raise InventoryError("MININDN_CASE_FIELDS_INVALID:" + entry_id)
            validate_case_contract(entry["case"], entry["path"], command[2:])
        else:
            if entry.get("case") is not None:
                raise InventoryError("NON_CASE_HAS_CASE:" + entry_id)
    without_digest = dict(inventory)
    without_digest.pop("inventoryDigest", None)
    if inventory.get("inventoryDigest") != canonical_digest(without_digest):
        raise InventoryError("INVENTORY_DIGEST_MISMATCH")
    return dict(inventory)


def build_inventory(
    root: Path | str,
    *,
    candidate_id: str,
    candidate_digest: str,
    source_revision: str,
    environment: Mapping[str, str],
    effective_config_digest: str | None = None,
    unit_binary: Path | str = "build/unit-tests",
    integration_binary: Path | str = "build/integration-tests",
    integration_listing: str | None = None,
    python_selectors: Iterable[str] | None = None,
    cases: Sequence[tuple[str, str, Sequence[str]]] = DEFAULT_CASES,
    timeout_seconds: int = DEFAULT_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    """Build a complete inventory or fail before writing any output."""
    root_path = Path(root).resolve()
    actual_config_digest = canonical_digest(local_launch_configuration(
        root_path, environment, timeout_seconds))
    if effective_config_digest is not None and effective_config_digest != actual_config_digest:
        raise InventoryError("EFFECTIVE_CONFIG_DIGEST_MISMATCH")
    effective_config_digest = actual_config_digest
    environment = dict(environment)
    inputs = local_input_identity(root_path, environment)
    _require_identity(
        candidate_id, candidate_digest, source_revision,
        effective_config_digest,
    )
    if not isinstance(timeout_seconds, int) or timeout_seconds <= 0:
        raise InventoryError("INVALID_TIMEOUT_SECONDS")
    unit_rel, unit_path = _relative_file(
        root_path, unit_binary, "unit-binary", executable=True)
    int_rel, int_path = _relative_file(
        root_path, integration_binary, "integration-binary", executable=True)
    int_selectors = discover_integration_selectors(
        root_path, int_path, listing=integration_listing, environment=environment)
    py_selectors = discover_python_selectors(root_path, python_selectors, environment=environment)
    case_records = _case_registry(root_path, cases)
    entries: list[dict[str, Any]] = []
    entries.append(_entry(
        "cpp-unit-suite", "cpp-suite", root=root_path, path=unit_rel,
        artifact=unit_path,
        command=(unit_rel, "--log_level=nothing"), candidate_id=candidate_id,
        candidate_digest=candidate_digest, source_revision=source_revision,
        config_digest=effective_config_digest, timeout_seconds=timeout_seconds,
    ))
    for selector in int_selectors:
        entries.append(_entry(
            "cpp-" + re.sub(r"[^A-Za-z0-9_.-]+", "-", selector),
            "cpp-selector", root=root_path, path=int_rel, artifact=int_path,
            command=(int_rel, "--run_test=" + selector, "--log_level=nothing"),
            candidate_id=candidate_id, candidate_digest=candidate_digest,
            source_revision=source_revision,
            config_digest=effective_config_digest,
            timeout_seconds=timeout_seconds, selector=selector,
        ))
    for selector in py_selectors:
        path_part = selector.split("::", 1)[0]
        py_rel, py_path = _relative_file(
            root_path, path_part, "pytest-selector")
        entry_id = "python-" + re.sub(r"[^A-Za-z0-9_.-]+", "-", selector)
        entries.append(_entry(
            entry_id, "python-selector", root=root_path, path=py_rel,
            artifact=py_path,
            command=(sys.executable, "-m", "pytest", "-q", selector),
            candidate_id=candidate_id, candidate_digest=candidate_digest,
            source_revision=source_revision,
            config_digest=effective_config_digest,
            timeout_seconds=timeout_seconds, selector=selector,
        ))
    for case, rel, case_path, args in case_records:
        entries.append(_entry(
            "minindn-" + case.lower(), "minindn-case", root=root_path,
            path=rel, artifact=case_path,
            command=(sys.executable, rel, *args), candidate_id=candidate_id,
            candidate_digest=candidate_digest, source_revision=source_revision,
            config_digest=effective_config_digest,
            timeout_seconds=timeout_seconds, case=case,
        ))
    inventory: dict[str, Any] = {
        "schema": SCHEMA,
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "sourceRevision": source_revision,
        "effectiveConfigDigest": effective_config_digest,
        "backend": BACKEND,
        "timeoutSeconds": timeout_seconds,
        "cleanupPolicy": CLEANUP_POLICY,
        "entries": entries,
        "inputIdentity": inputs,
        "inputDigest": canonical_digest(inputs),
    }
    if local_input_identity(root_path, environment) != inputs:
        raise InventoryError("INPUTS_CHANGED_DURING_DISCOVERY")
    inventory["inventoryDigest"] = canonical_digest(inventory)
    return validate_inventory(inventory)


def _write_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--candidate-id", required=True)
    parser.add_argument("--candidate-digest", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--environment-json", type=Path, required=True,
                        help="explicit child environment; ambient values are never substituted")
    parser.add_argument("--effective-config-digest",
                        help="optional expected digest; checked against actual launch inputs")
    parser.add_argument("--unit-binary", default="build/unit-tests")
    parser.add_argument("--integration-binary", default="build/integration-tests")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int,
                        default=DEFAULT_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    try:
        environment = json.loads(args.environment_json.read_text(encoding="utf-8"))
        inventory = build_inventory(
            args.root, candidate_id=args.candidate_id,
            candidate_digest=args.candidate_digest,
            source_revision=args.source_revision,
            environment=environment,
            effective_config_digest=args.effective_config_digest,
            unit_binary=args.unit_binary, integration_binary=args.integration_binary,
            timeout_seconds=args.timeout_seconds,
        )
        output = args.output if args.output.is_absolute() else args.root / args.output
        _write_json(output, inventory)
    except (InventoryError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 78
    print(json.dumps(inventory, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
