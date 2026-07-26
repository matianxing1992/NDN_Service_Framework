"""DI-owned conversion between artifact metadata and public Repo references.

This module contains no Repo server, placement, persistence, catalog, or repair
policy. Operational Repo behavior belongs to ``py_repoclient.orchestration``.
"""

from __future__ import annotations

from typing import Any


def _boolish(value: object, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "on", "enabled"}:
        return True
    if text in {"0", "false", "no", "off", "disabled"}:
        return False
    return default


def _manifest_dict(manifest: Any) -> dict:
    if isinstance(manifest, dict):
        return dict(manifest)
    if hasattr(manifest, "to_dict"):
        return dict(manifest.to_dict())
    if hasattr(manifest, "to_json"):
        import json
        return dict(json.loads(manifest.to_json()))
    raise TypeError("repo manifest must be a mapping or expose to_dict/to_json")


def large_data_reference_from_repo_manifest(
        manifest: Any, *, object_type: str = "", object_id: str = "") -> dict:
    manifest_dict = _manifest_dict(manifest)
    return {
        "source": "repo-manifest",
        "dataName": str(manifest_dict.get("objectName", "")),
        "objectType": object_type or str(manifest_dict.get("objectType", "")),
        "objectId": object_id or str(manifest_dict.get("objectName", "")),
        "plaintextSize": int(manifest_dict.get("size", 0)),
        "encrypted": _boolish(manifest_dict.get("encrypted", False), False),
        "digest": "sha256:" + str(manifest_dict.get("sha256", "")),
    }


def repo_artifact_reference(
        manifest: Any, *, object_type: str = "", object_id: str = "") -> dict:
    manifest_dict = _manifest_dict(manifest)
    return {
        "repoManifest": manifest_dict,
        "largeDataReference": large_data_reference_from_repo_manifest(
            manifest_dict, object_type=object_type, object_id=object_id),
    }


def repo_manifest_from_artifact_reference(entry: dict) -> dict:
    if not isinstance(entry, dict):
        raise ValueError("repo artifact entry must be a mapping")
    if "largeDataReference" in entry:
        reference = entry.get("largeDataReference", {})
        if not isinstance(reference, dict):
            raise ValueError("largeDataReference must be a mapping")
        source = str(reference.get("source", ""))
        if source and source != "repo-manifest":
            raise ValueError(f"unsupported artifact largeDataReference source: {source}")
        manifest = dict(entry.get("repoManifest", entry.get("repo_manifest", {})))
        if not manifest:
            raise ValueError("repo-backed artifact largeDataReference missing repoManifest")
        digest = str(reference.get("digest", ""))
        if digest.startswith("sha256:"):
            expected = digest.split(":", 1)[1]
            actual = str(manifest.get("sha256", ""))
            if expected and actual and expected != actual:
                raise ValueError(
                    "largeDataReference digest does not match repoManifest: "
                    f"reference={expected} manifest={actual}")
        return manifest
    if "repoManifest" in entry:
        return dict(entry["repoManifest"])
    if "repo_manifest" in entry:
        return dict(entry["repo_manifest"])
    return dict(entry)


def repo_manifest_from_large_data_reference(entry: dict) -> dict:
    return repo_manifest_from_artifact_reference(entry)


__all__ = [
    "large_data_reference_from_repo_manifest",
    "repo_artifact_reference",
    "repo_manifest_from_artifact_reference",
    "repo_manifest_from_large_data_reference",
]
