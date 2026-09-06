"""DI-owned conversion between artifact metadata and public Repo references.

This module contains no Repo server, placement, persistence, catalog, or repair
policy. Operational Repo behavior belongs to ``py_repoclient.orchestration``.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from typing import Any


def _require_digest(value: str, label: str) -> None:
    if (not isinstance(value, str) or len(value) != 71
            or not value.startswith("sha256:")):
        raise ValueError(f"{label} must be sha256:<64 hex>")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(f"{label} must be sha256:<64 hex>") from exc


def _publication_manifest_digest(*, data_name: str, object_id: str,
                                 plaintext_size: int, content_digest: str,
                                 authorization_scope: str,
                                 protection_epoch: str) -> str:
    """Return the digest emitted by the native encrypted publisher.

    This is deliberately a small canonical manifest, not a second repository
    protocol.  The native publisher is the authority for these values; the DI
    binding recomputes the same digest before it lets a request carry the
    resulting reference.
    """
    canonical = (
        "version=1\n"
        f"name={data_name}\n"
        f"object_id={object_id}\n"
        f"plaintext_size={int(plaintext_size)}\n"
        f"content_digest={content_digest}\n"
        f"authorization_scope={authorization_scope}\n"
        f"protection_epoch={protection_epoch}\n"
        "encrypted=1\n"
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(canonical).hexdigest()


@dataclass(frozen=True)
class LargeDataReference:
    """Authenticated reference for input too large to carry inline.

    The reference is metadata only.  It never contains plaintext and is
    resolved/fetched by the selected ingress role after Selection.
    """

    data_name: str
    manifest_digest: str
    plaintext_size: int
    ciphertext_digest: str
    authorization_scope: str
    protection_epoch: str
    encrypted: bool = True
    object_type: str = ""
    object_id: str = ""

    def __post_init__(self) -> None:
        if not self.data_name.startswith("/"):
            raise ValueError("large-data reference data_name must be absolute")
        if not self.object_id:
            raise ValueError("large-data reference object_id is required")
        if not self.authorization_scope:
            raise ValueError("large-data reference authorization scope is required")
        if (not self.protection_epoch
                or self.protection_epoch == "plaintext-v1"
                or len(self.protection_epoch) > 128):
            raise ValueError("large-data reference protection epoch is required")
        if self.plaintext_size <= 0:
            raise ValueError("large-data reference plaintext_size must be positive")
        if self.encrypted is not True:
            raise ValueError("large-data reference must be encrypted")
        _require_digest(self.manifest_digest, "manifest_digest")
        _require_digest(self.ciphertext_digest, "ciphertext_digest")

    def to_dict(self) -> dict[str, Any]:
        return {
            "source": "repo-manifest",
            "dataName": self.data_name,
            "manifestDigest": self.manifest_digest,
            "plaintextSize": self.plaintext_size,
            "ciphertextDigest": self.ciphertext_digest,
            "authorizationScope": self.authorization_scope,
            "protectionEpoch": self.protection_epoch,
            "encrypted": self.encrypted,
            "objectType": self.object_type,
            "objectId": self.object_id,
        }

    @classmethod
    def from_mapping(cls, value: Any) -> "LargeDataReference":
        if not isinstance(value, dict):
            raise ValueError("large-data reference must be a mapping")
        manifest_digest = str(value.get("manifestDigest", value.get("manifest_digest", "")))
        ciphertext_digest = str(value.get(
            "ciphertextDigest", value.get("ciphertext_digest", value.get("digest", ""))))
        return cls(
            data_name=str(value.get("dataName", value.get("data_name", ""))),
            manifest_digest=manifest_digest,
            plaintext_size=int(value.get("plaintextSize", value.get("plaintext_size", 0))),
            ciphertext_digest=ciphertext_digest,
            authorization_scope=str(value.get(
                "authorizationScope", value.get("authorization_scope", ""))),
            protection_epoch=str(value.get(
                "protectionEpoch", value.get("protection_epoch", ""))),
            encrypted=_boolish(value.get("encrypted"), False),
            object_type=str(value.get("objectType", value.get("object_type", ""))),
            object_id=str(value.get("objectId", value.get("object_id", ""))),
        )

    def digest(self) -> str:
        encoded = json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"))
        return "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def bind_published_large_data_reference(
        publication: Any, *, service_name: str, payload: bytes,
        object_type: str = "") -> LargeDataReference:
    """Bind one native publication result to the DI reference contract.

    The caller cannot fill in security metadata after publication.  Every
    security-bearing field is taken from the native result and checked against
    the exact bytes that were handed to the publisher.  Older bindings that
    return only a Data name/object ID fail closed instead of manufacturing an
    epoch or manifest digest in Python.
    """
    if not bool(getattr(publication, "success", False)):
        reason = str(getattr(publication, "error", "") or
                     getattr(publication, "error_message", "") or
                     "large-data publication failed")
        raise RuntimeError(reason)
    if not isinstance(payload, (bytes, bytearray, memoryview)):
        raise TypeError("published input payload must be bytes-like")
    payload = bytes(payload)
    if not service_name.startswith("/"):
        raise ValueError("service_name must be an absolute NDN name")

    data_name = str(getattr(publication, "encrypted_data_name", ""))
    object_id = str(getattr(publication, "object_id", ""))
    plaintext_size = int(getattr(publication, "plaintext_size", 0) or 0)
    content_digest = str(
        getattr(publication, "content_digest", "") or
        getattr(publication, "plaintext_digest", "") or
        getattr(publication, "digest", ""))
    manifest_digest = str(getattr(publication, "manifest_digest", ""))
    authorization_scope = str(getattr(publication, "authorization_scope", ""))
    protection_epoch = str(getattr(publication, "protection_epoch", ""))
    encrypted = getattr(publication, "encrypted", True)

    expected_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    expected_scope = "/SERVICE" + service_name
    if not data_name.startswith("/") or not object_id:
        raise ValueError("native publication result lacks object identity")
    if plaintext_size != len(payload):
        raise ValueError("native publication plaintext size is not source-bound")
    if content_digest != expected_digest:
        raise ValueError(
            "native publication content digest is not source-bound: "
            f"expected={expected_digest} actual={content_digest or '<empty>'}")
    if encrypted is not True:
        raise ValueError("native publication is not encrypted")
    if authorization_scope != expected_scope:
        raise ValueError("native publication authorization scope is not bound")
    if (not protection_epoch or protection_epoch == "plaintext-v1"):
        raise ValueError("native publication protection epoch is missing")
    _require_digest(manifest_digest, "manifest_digest")
    expected_manifest = _publication_manifest_digest(
        data_name=data_name,
        object_id=object_id,
        plaintext_size=plaintext_size,
        content_digest=content_digest,
        authorization_scope=authorization_scope,
        protection_epoch=protection_epoch,
    )
    if manifest_digest != expected_manifest:
        raise ValueError("native publication manifest digest mismatch")
    return LargeDataReference(
        data_name=data_name,
        manifest_digest=manifest_digest,
        plaintext_size=plaintext_size,
        # The current NDNSF fetch primitive verifies the digest after
        # decryption.  Keep the historical field name on the wire while also
        # emitting contentDigest for DI-owned readers.
        ciphertext_digest=content_digest,
        authorization_scope=authorization_scope,
        protection_epoch=protection_epoch,
        encrypted=True,
        object_type=str(object_type),
        object_id=object_id,
    )


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
    digest = "sha256:" + str(manifest_dict.get("sha256", ""))
    # Preserve the historical dictionary keys used by artifact deployment while
    # adding the explicit T002 transport fields consumed by ApplicationInput.
    return {
        "source": "repo-manifest",
        "dataName": str(manifest_dict.get("objectName", "")),
        "objectType": object_type or str(manifest_dict.get("objectType", "")),
        "objectId": object_id or str(manifest_dict.get("objectName", "")),
        "plaintextSize": int(manifest_dict.get("size", 0)),
        "encrypted": _boolish(manifest_dict.get("encrypted", False), False),
        "digest": digest,
        "ciphertextDigest": digest,
        "manifestDigest": str(manifest_dict.get(
            "manifestDigest", manifest_dict.get("manifestSha256", digest))),
        "authorizationScope": str(manifest_dict.get(
            "authorizationScope", manifest_dict.get("accessScope", ""))),
        "protectionEpoch": str(manifest_dict.get(
            "protectionEpoch", manifest_dict.get(
                "protection_epoch", "repo-manifest-v1"))),
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
    "LargeDataReference",
    "bind_published_large_data_reference",
    "large_data_reference_from_repo_manifest",
    "repo_artifact_reference",
    "repo_manifest_from_artifact_reference",
    "repo_manifest_from_large_data_reference",
]
