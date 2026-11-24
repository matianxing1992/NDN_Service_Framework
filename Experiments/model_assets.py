"""Offline model-asset admission shared by preparation and experiment callers.

This module validates files; it does not grant inference permission or construct
native model identities. Recipes are maintained inputs, never manifest-selected code.
"""
import errno
import hashlib
import json
import os
import re
import shutil
import tempfile
from pathlib import Path

SCHEMA = "ndnsf-model-assets-v1"
MAX_JSON_BYTES = 1024 * 1024


def read_json(path, max_bytes=MAX_JSON_BYTES):
    path = Path(path)
    with path.open("rb") as source:
        raw = source.read(max_bytes + 1)
    if len(raw) > max_bytes:
        raise ValueError("ASSET_JSON_TOO_LARGE: " + str(path))
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("ASSET_DUPLICATE_FIELD: " + key)
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique_pairs)


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def contained_file(root, name):
    child = Path(name)
    if not name or child.is_absolute() or ".." in child.parts:
        raise ValueError("ASSET_PATH_INVALID: " + str(name))
    root = Path(root).resolve(strict=True)
    candidate = root / child
    current = root
    for part in child.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError("ASSET_SYMLINK_REJECTED: " + name)
    candidate.resolve(strict=True).relative_to(root)
    if not candidate.is_file():
        raise ValueError("ASSET_FILE_MISSING: " + name)
    return candidate


def verify_file(root, row):
    if (set(row) != {"path", "bytes", "sha256"} or
            type(row["bytes"]) is not int or row["bytes"] < 0 or
            not isinstance(row["sha256"], str) or
            re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is None):
        raise ValueError("ASSET_FILE_COMMITMENT_INVALID")
    path = contained_file(root, row["path"])
    if path.stat().st_size != row["bytes"] or file_digest(path) != row["sha256"]:
        raise ValueError("ASSET_FILE_COMMITMENT_MISMATCH: " + row["path"])
    return path


def recipe_identity(recipe):
    return hashlib.sha256(json.dumps(recipe, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()


def validate_manifest(path, recipe):
    """Return validated role paths, binding every role to the maintained recipe."""
    path = Path(path)
    if path.is_symlink():
        raise ValueError("ASSET_MANIFEST_SYMLINK_REJECTED")
    value = read_json(path)
    expected = {
        "schema": SCHEMA,
        "state": "SEALED",
        "assetId": recipe["assetId"],
        "recipeVersion": recipe["recipeVersion"],
        "recipeDigest": recipe_identity(recipe),
        "repository": recipe["repository"],
        "revision": recipe["revision"],
        "license": recipe["license"],
        "toolVersions": recipe["toolVersions"],
        "transforms": recipe["transforms"],
        "modelContract": recipe["modelContract"],
        "files": recipe["files"],
    }
    # JSON booleans/floats are not interchangeable with integer schema fields,
    # although Python's ordinary container equality treats True == 1 == 1.0.
    canonical = lambda item: json.dumps(item, sort_keys=True, separators=(",", ":"),
                                        allow_nan=False)
    if canonical(value) != canonical(expected):
        raise ValueError("ASSET_MANIFEST_NOT_SEALED_OR_RECIPE_MISMATCH")
    return {role: verify_file(path.parent, row)
            for role, row in recipe["files"].items()}


def sealed_manifest(recipe):
    return dict(schema=SCHEMA, state="SEALED", assetId=recipe["assetId"],
                recipeVersion=recipe["recipeVersion"], recipeDigest=recipe_identity(recipe),
                repository=recipe["repository"], revision=recipe["revision"],
                license=recipe["license"], toolVersions=recipe["toolVersions"],
                transforms=recipe["transforms"], modelContract=recipe["modelContract"],
                files=recipe["files"])


def prepare_source_cache(root, asset_manifest, recipe, assets):
    """Publish the already admitted model bytes for the native filesystem source loader."""
    root = Path(root)
    if not root.is_absolute() or root.resolve(strict=True) != root or not root.is_dir():
        raise ValueError("ASSET_SOURCE_CACHE_ROOT_INVALID")
    identity = dict(schema="ndnsf-di-model-source-cache-v1",
                    assetManifestDigest="sha256:" + file_digest(asset_manifest),
                    sourceDigest="sha256:" + recipe["files"]["canonicalGraph"]["sha256"],
                    initializerDigest="sha256:" + recipe["files"]["canonicalWeights"]["sha256"])
    marker = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()
    digest = hashlib.sha256(marker).hexdigest()
    target = root / digest
    objects = {
        role: Path("payloads/sha256") / row["sha256"][:2] / row["sha256"]
        for role, row in recipe["files"].items() if role in ("canonicalGraph", "canonicalWeights")}
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix=".publish-", dir=str(root)) as pending:
            stage = Path(pending) / "cache"
            stage.mkdir(mode=0o700)
            for role, relative in objects.items():
                path = stage / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                if path.exists():
                    continue
                try:
                    os.link(assets[role], path)
                except OSError as error:
                    if error.errno != errno.EXDEV:
                        raise
                    shutil.copyfile(assets[role], path)
                    if file_digest(path) != recipe["files"][role]["sha256"]:
                        raise ValueError("ASSET_SOURCE_CACHE_COPY_MISMATCH: " + role)
            (stage / "cache-identity.json").write_bytes(marker)
            try:
                stage.rename(target)
            except OSError as error:
                if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                    raise
    if target.is_symlink() or read_json(contained_file(target, "cache-identity.json")) != identity:
        raise ValueError("ASSET_SOURCE_CACHE_IDENTITY_MISMATCH")
    for role, relative in objects.items():
        path = contained_file(target, str(relative))
        row = recipe["files"][role]
        if path.stat().st_size != row["bytes"] or (
                not path.samefile(assets[role]) and file_digest(path) != row["sha256"]):
            raise ValueError("ASSET_SOURCE_CACHE_OBJECT_MISMATCH: " + role)
    return dict(mode="source-cache", source_directory=str(target), source_namespace="sha256:" + digest)
