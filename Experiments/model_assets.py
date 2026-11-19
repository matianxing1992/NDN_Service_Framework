"""Offline model-asset admission shared by preparation and experiment callers.

This module validates files; it does not grant inference permission or construct
native model identities. Recipes are maintained inputs, never manifest-selected code.
"""
import hashlib
import json
import re
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
