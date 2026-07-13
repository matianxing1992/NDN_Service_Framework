#!/usr/bin/env python3
"""Content-addressed, read-only Spec 107 ONNX artifact store."""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
from typing import Any, Mapping, Sequence

from spec107_identity import digest_object
from spec107_lineage import LineageError, assert_mutation_allowed, sha256_file


SCHEMA = "ndnsf-di-spec107-artifact-set-v1"
CANDIDATE_ID_RE = re.compile(r"^spec107-c1(?:-[0-9a-f]{12}){6}$")
EXPECTED_ROLES = frozenset({
    "/LLM/Pipeline/Stage/0",
    "/LLM/Pipeline/Stage/1",
    "/LLM/Pipeline/Stage/2",
})
FICLONE = 0x40049409


class ArtifactError(ValueError):
    """Stable fail-closed artifact-store error."""


def _fail(code: str, detail: str = "") -> None:
    suffix = f":{detail}" if detail else ""
    raise ArtifactError(code + suffix)


def _canonical_source_name(value: object) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        _fail("ARTIFACT_SOURCE_INVALID", repr(value))
    path = PurePosixPath(value)
    if path.is_absolute() or len(path.parts) != 1 or path.suffix != ".onnx":
        _fail("ARTIFACT_SOURCE_INVALID", value)
    return path.name


def _validate_sources(
    source_root: Path,
    artifacts: Sequence[Mapping[str, object]],
) -> list[dict[str, Any]]:
    if len(artifacts) != 3:
        _fail("ARTIFACT_ROLE_SET_INVALID", str(len(artifacts)))
    root = source_root.resolve()
    roles: set[str] = set()
    names: set[str] = set()
    rows = []
    for row in artifacts:
        role = row.get("role")
        if not isinstance(role, str):
            _fail("ARTIFACT_ROLE_INVALID", repr(role))
        roles.add(role)
        name = _canonical_source_name(row.get("source"))
        if name in names:
            _fail("ARTIFACT_SOURCE_DUPLICATE", name)
        names.add(name)
        lexical = root / name
        if lexical.is_symlink():
            _fail("ARTIFACT_SOURCE_ESCAPE", name)
        try:
            path = lexical.resolve(strict=True)
            path.relative_to(root)
        except (FileNotFoundError, ValueError):
            _fail("ARTIFACT_SOURCE_ESCAPE", name)
        if not path.is_file():
            _fail("ARTIFACT_SOURCE_INVALID", name)
        rows.append({
            "role": role,
            "source": path,
            "path": name,
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    if roles != EXPECTED_ROLES:
        _fail("ARTIFACT_ROLE_SET_INVALID", ",".join(sorted(roles)))
    return sorted(rows, key=lambda row: row["role"])


def _descriptor(model_revision: str, rows: Sequence[Mapping[str, object]]) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "modelRevision": model_revision,
        "artifacts": [{
            "role": row["role"],
            "path": row["path"],
            "bytes": row["bytes"],
            "sha256": row["sha256"],
        } for row in rows],
    }


def _try_reflink(source: Path, destination: Path) -> bool:
    try:
        with source.open("rb") as src, destination.open("xb") as dst:
            fcntl.ioctl(dst.fileno(), FICLONE, src.fileno())
        return True
    except (OSError, ValueError):
        try:
            destination.unlink()
        except OSError:
            pass
        return False


def _copy_one(source: Path, destination: Path) -> str:
    source_readonly = stat.S_IMODE(source.stat().st_mode) & 0o222 == 0
    if source_readonly:
        try:
            os.link(source, destination)
            return "hardlink"
        except OSError:
            pass
    if _try_reflink(source, destination):
        return "reflink"
    shutil.copy2(source, destination)
    return "copy"


def _make_tree_writable(path: Path) -> None:
    if not path.exists():
        return
    for child in path.rglob("*"):
        try:
            if child.is_dir():
                child.chmod(0o755)
            else:
                child.chmod(0o644)
        except OSError:
            pass
    try:
        path.chmod(0o755)
    except OSError:
        pass


def _remove_intermediates(source_root: Path, repo_root: Path) -> None:
    for path in sorted(source_root.rglob("*.pt")):
        resolved = path.resolve(strict=False)
        try:
            resolved.relative_to(repo_root)
        except ValueError:
            pass
        else:
            try:
                assert_mutation_allowed(resolved, repo_root=repo_root)
            except LineageError as exc:
                _fail("SPEC105_INTERMEDIATE_MUTATION_DENIED", str(exc))
        try:
            path.unlink()
        except OSError as exc:
            _fail("ARTIFACT_INTERMEDIATE_REMOVE_FAILED", f"{path}:{exc}")


def _read_manifest(store: Path) -> dict[str, Any]:
    path = store / "artifact-set.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail("ARTIFACT_SET_MANIFEST_INVALID", str(exc))
    if not isinstance(value, dict):
        _fail("ARTIFACT_SET_MANIFEST_INVALID", "root-not-object")
    return value


def verify_artifact_set(store: Path | str) -> dict[str, Any]:
    root = Path(store).resolve()
    manifest = _read_manifest(root)
    if manifest.get("schema") != SCHEMA:
        _fail("ARTIFACT_SET_SCHEMA_INVALID")
    rows = manifest.get("artifacts")
    model_revision = manifest.get("modelRevision")
    if not isinstance(rows, list) or len(rows) != 3 or not isinstance(model_revision, str):
        _fail("ARTIFACT_SET_MANIFEST_INVALID")
    verified_rows = []
    content_mismatch = False
    readonly_mismatch: str | None = None
    for row in rows:
        if not isinstance(row, dict):
            _fail("ARTIFACT_SET_MANIFEST_INVALID")
        name = _canonical_source_name(row.get("path"))
        path = root / name
        if path.is_symlink() or not path.is_file():
            content_mismatch = True
            continue
        actual_size = path.stat().st_size
        actual_digest = sha256_file(path)
        if actual_size != row.get("bytes") or actual_digest != row.get("sha256"):
            content_mismatch = True
        if stat.S_IMODE(path.stat().st_mode) & 0o222:
            readonly_mismatch = readonly_mismatch or name
        verified_rows.append({
            "role": row.get("role"),
            "path": name,
            "bytes": actual_size,
            "sha256": actual_digest,
        })
    if content_mismatch:
        _fail("ARTIFACT_SET_DIGEST_MISMATCH")
    if readonly_mismatch is not None:
        _fail("ARTIFACT_SET_NOT_READONLY", readonly_mismatch)
    descriptor = _descriptor(model_revision, verified_rows)
    actual_set_digest = digest_object(descriptor)
    if manifest.get("artifactSetDigest") != actual_set_digest:
        _fail("ARTIFACT_SET_DIGEST_MISMATCH")
    if root.name != actual_set_digest.split(":", 1)[1]:
        _fail("ARTIFACT_SET_PATH_MISMATCH")
    if stat.S_IMODE(root.stat().st_mode) & 0o222:
        _fail("ARTIFACT_SET_NOT_READONLY", str(root))
    if any(root.rglob("*.pt")):
        _fail("ARTIFACT_SET_INTERMEDIATE_PRESENT")
    manifest_path = root / "artifact-set.json"
    if stat.S_IMODE(manifest_path.stat().st_mode) & 0o222:
        _fail("ARTIFACT_SET_NOT_READONLY", "artifact-set.json")
    return manifest


def materialize_artifact_set(
    *,
    source_root: Path | str,
    output_root: Path | str,
    artifacts: Sequence[Mapping[str, object]],
    candidate_id: str | None = None,
    model_revision: str,
    repo_root: Path | str,
    reserve_bytes: int = 1024 * 1024 * 1024,
    free_bytes: int | None = None,
) -> dict[str, Any]:
    """Materialize exactly one immutable three-stage artifact set."""

    if candidate_id is not None and (
        not isinstance(candidate_id, str)
        or CANDIDATE_ID_RE.fullmatch(candidate_id) is None
    ):
        _fail("ARTIFACT_CANDIDATE_ID_INVALID")
    if not isinstance(model_revision, str) or not model_revision:
        _fail("ARTIFACT_MODEL_REVISION_INVALID")
    if isinstance(reserve_bytes, bool) or not isinstance(reserve_bytes, int) or reserve_bytes < 0:
        _fail("ARTIFACT_RESERVE_INVALID")
    repo = Path(repo_root).resolve()
    source = Path(source_root).resolve()
    if not source.is_dir():
        _fail("ARTIFACT_SOURCE_ROOT_INVALID", str(source))
    output = assert_mutation_allowed(output_root, repo_root=repo)
    try:
        relative_output = output.relative_to(repo)
    except ValueError:
        _fail("ARTIFACT_OUTPUT_INVALID", str(output))
    if relative_output.as_posix() != "results/spec107-artifacts":
        _fail("ARTIFACT_OUTPUT_INVALID", relative_output.as_posix())

    rows = _validate_sources(source, artifacts)
    descriptor = _descriptor(model_revision, rows)
    set_digest = digest_object(descriptor)
    final_store = output / set_digest.split(":", 1)[1]
    if final_store.exists():
        manifest = verify_artifact_set(final_store)
        return {
            "candidateId": candidate_id,
            "materialization": "REUSED",
            "storePath": str(final_store),
            "manifest": manifest,
        }

    total_bytes = sum(int(row["bytes"]) for row in rows)
    ancestor = output.parent
    while not ancestor.exists() and ancestor != ancestor.parent:
        ancestor = ancestor.parent
    available = shutil.disk_usage(ancestor).free if free_bytes is None else free_bytes
    if (
        isinstance(available, bool)
        or not isinstance(available, int)
        or available <= total_bytes + reserve_bytes
    ):
        _fail(
            "ARTIFACT_SPACE_INSUFFICIENT",
            f"free={available}:required>{total_bytes + reserve_bytes}",
        )

    _remove_intermediates(source, repo)
    output.mkdir(parents=True, exist_ok=True)
    temporary = output / f".{set_digest.split(':', 1)[1]}.tmp-{os.getpid()}"
    if temporary.exists():
        _fail("ARTIFACT_TEMP_EXISTS", str(temporary))
    temporary.mkdir(mode=0o755)
    materialized_rows = []
    try:
        for row in rows:
            source_path = Path(row["source"])
            destination = temporary / str(row["path"])
            mode = _copy_one(source_path, destination)
            if (
                destination.stat().st_size != row["bytes"]
                or sha256_file(destination) != row["sha256"]
            ):
                _fail("ARTIFACT_COPY_VERIFY_FAILED", str(row["path"]))
            materialized_rows.append({
                "role": row["role"],
                "path": row["path"],
                "bytes": row["bytes"],
                "sha256": row["sha256"],
                "storageMode": mode,
            })
        manifest = {
            "schema": SCHEMA,
            "artifactSetDigest": set_digest,
            "modelRevision": model_revision,
            "artifacts": materialized_rows,
            "retention": "content-addressed-read-only",
        }
        manifest_path = temporary / "artifact-set.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        for path in temporary.iterdir():
            if path.is_file():
                path.chmod(0o444)
        temporary.chmod(0o555)
        try:
            temporary.rename(final_store)
        except FileExistsError:
            _make_tree_writable(temporary)
            shutil.rmtree(temporary)
            manifest = verify_artifact_set(final_store)
            return {
                "candidateId": candidate_id,
                "materialization": "REUSED",
                "storePath": str(final_store),
                "manifest": manifest,
            }
    except Exception:
        _make_tree_writable(temporary)
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    verified = verify_artifact_set(final_store)
    return {
        "candidateId": candidate_id,
        "materialization": "CREATED",
        "storePath": str(final_store),
        "manifest": verified,
    }


__all__ = [
    "ArtifactError",
    "materialize_artifact_set",
    "verify_artifact_set",
]
