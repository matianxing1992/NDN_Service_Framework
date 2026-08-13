"""Content-addressed canonical layer and assembled ONNX identities.

The module is application/DI-owned; it does not replace DistributedRepo's
placement or replication policy.  It only builds deterministic names,
manifests, and Provider-local immutable bundles that a Repo client may publish
or fetch.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Any, Mapping


_TOKEN = re.compile(r"^[A-Za-z0-9._~-]+$")
_DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
ALLOWED_ENTRIES = ("model.onnx", "model.onnx.data")
_ROLE_KIND_ALIASES = {
    "pipeline": "PIPELINE_RANGE",
    "stage": "PIPELINE_RANGE",
    "stages": "PIPELINE_RANGE",
    "tensor": "TENSOR_RANK",
    "shard": "TENSOR_RANK",
    "hybrid": "HYBRID_RANK",
    "component": "COMPONENT_SET",
}
_ROLE_KINDS = frozenset({
    "PIPELINE_RANGE", "TENSOR_RANK", "HYBRID_RANK", "COMPONENT_SET",
})


def _digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _require_digest(value: str, field: str) -> str:
    if not isinstance(value, str) or not _DIGEST.fullmatch(value):
        raise ValueError(f"{field} must be a canonical sha256 digest")
    return value


def _token(value: str, field: str) -> str:
    value = str(value)
    if not value or not _TOKEN.fullmatch(value) or value in {".", ".."}:
        raise ValueError(f"unsafe {field}")
    return value


def _component_kind(value: str) -> str:
    raw = str(value).strip()
    normalized = _ROLE_KIND_ALIASES.get(raw.lower(), raw.upper())
    if normalized not in _ROLE_KINDS:
        raise ValueError(f"unsupported canonical component kind: {value}")
    return normalized


def _identity_digest(value: str, field: str) -> str:
    """Normalize a human profile into a content-addressed identity.

    The public V3 grammar carries a digest, not an adapter/profile label.  The
    compatibility helpers historically accepted labels such as ``fp16``;
    hashing those labels here keeps old callers deterministic while ensuring
    that a readable alias can never become the equality key on the wire.
    """

    if isinstance(value, str) and _DIGEST.fullmatch(value):
        return value
    if not value:
        raise ValueError(f"{field} is required")
    return _digest(str(value).encode("utf-8"))


def canonical_layer_name(*, publisher: str, model_name: str, model_digest: str,
                         profile: str, graph_digest: str, role_kind: str,
                         layer_begin: int, layer_end: int, rank: int = 0,
                         recipe_digest: str = "", object_digest: str,
                         model_manifest_digest: str = "",
                         layer_manifest_digest: str = "",
                         segment_number: int = 0) -> str:
    """Build the only Spec 170 V3 public layer/object namespace.

    Request/attempt/provider/role/rank/placement values are deliberately not
    path components.  ``role_kind`` is the adapter-defined component kind and
    the stable coordinate is the graph layer interval; rank remains a field in
    the signed manifest for execution, never an artifact equality key.
    """

    model_name = "/".join(_token(part, "model name component")
                           for part in str(model_name).strip("/").split("/"))
    if not model_name:
        raise ValueError("model name is required")
    if layer_begin < 0 or layer_end <= layer_begin or rank < 0:
        raise ValueError("invalid layer coordinate")
    for field, value in (("model_digest", model_digest),
                         ("graph_digest", graph_digest),
                         ("object_digest", object_digest)):
        _require_digest(value, field)
    role_kind = _component_kind(role_kind)
    recipe_digest = recipe_digest or _digest(
        f"{role_kind}:{layer_begin}:{layer_end}".encode("utf-8"))
    _require_digest(recipe_digest, "recipe_digest")
    publisher = "/" + "/".join(_token(part, "publisher component")
                                for part in str(publisher).strip("/").split("/"))
    if str(profile) in {"REV", "CFG", "CONTENT"}:
        raise ValueError("legacy REV/CFG/CONTENT alias is not a V3 profile")
    profile_digest = _identity_digest(str(profile), "profile")
    if model_manifest_digest:
        _require_digest(model_manifest_digest, "model_manifest_digest")
    else:
        model_manifest_digest = _digest(json.dumps({
            "modelName": model_name,
            "modelDigest": model_digest,
            "profileDigest": profile_digest,
            "graphDigest": graph_digest,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    if layer_manifest_digest:
        _require_digest(layer_manifest_digest, "layer_manifest_digest")
    else:
        layer_manifest_digest = _digest(json.dumps({
            "componentKind": role_kind,
            "stableCoordinate": f"layers-{layer_begin}-{layer_end}",
            "recipeDigest": recipe_digest,
            "graphDigest": graph_digest,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    if segment_number < 0:
        raise ValueError("segment_number must be non-negative")
    return (f"{publisher}/NDNSF-DI/MODEL/v1/NAME/{model_name}/MID/{model_digest}"
            f"/PROFILE/{profile_digest}/MANIFEST/{model_manifest_digest}"
            f"/LAYER/{role_kind}"
            f"/layers-{layer_begin}-{layer_end}/MANIFEST/{layer_manifest_digest}"
            f"/OBJECT/{object_digest}/{segment_number}")


@dataclass(frozen=True)
class CanonicalLayerManifest:
    model_name: str
    model_digest: str
    profile: str
    graph_digest: str
    role_kind: str
    layer_begin: int
    layer_end: int
    rank: int
    recipe_digest: str
    object_digest: str
    object_bytes: int
    publisher: str
    origin_attestation: str
    transformation_attestation: str
    tensor_index: tuple[Mapping[str, Any], ...] = ()
    schema: str = "ndnsf-di-canonical-layer-v1"

    def __post_init__(self) -> None:
        if self.schema != "ndnsf-di-canonical-layer-v1":
            raise ValueError("unsupported canonical layer schema")
        if self.object_bytes < 0 or self.layer_end <= self.layer_begin:
            raise ValueError("invalid canonical layer bounds")
        object.__setattr__(self, "role_kind", _component_kind(self.role_kind))
        _require_digest(self.model_digest, "model_digest")
        _require_digest(self.graph_digest, "graph_digest")
        _require_digest(self.recipe_digest, "recipe_digest")
        _require_digest(self.object_digest, "object_digest")
        if not self.publisher or not self.origin_attestation or not self.transformation_attestation:
            raise ValueError("canonical layer trust attestations are required")
        object.__setattr__(self, "tensor_index", tuple(dict(item) for item in self.tensor_index))
        # Validate the complete semantic NDN identity at construction time;
        # callers must not be able to carry an invalid alias until publish.
        self.name

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "modelName": self.model_name, "modelDigest": self.model_digest,
            "profile": self.profile, "graphDigest": self.graph_digest,
            "roleKind": self.role_kind, "layerBegin": self.layer_begin,
            "layerEnd": self.layer_end, "rank": self.rank,
            "recipeDigest": self.recipe_digest, "objectDigest": self.object_digest,
            "objectBytes": self.object_bytes, "publisher": self.publisher,
            "originAttestation": self.origin_attestation,
            "transformationAttestation": self.transformation_attestation,
            "tensorIndex": list(self.tensor_index),
        }

    @property
    def profile_digest(self) -> str:
        return _identity_digest(self.profile, "profile")

    @property
    def model_manifest_digest(self) -> str:
        return _digest(json.dumps({
            "modelName": self.model_name,
            "modelDigest": self.model_digest,
            "profileDigest": self.profile_digest,
            "graphDigest": self.graph_digest,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8"))

    @property
    def layer_manifest_digest(self) -> str:
        return _digest(json.dumps({
            "componentKind": self.role_kind,
            "stableCoordinate": f"layers-{self.layer_begin}-{self.layer_end}",
            "recipeDigest": self.recipe_digest,
            "graphDigest": self.graph_digest,
            "tensorIndex": list(self.tensor_index),
        }, sort_keys=True, separators=(",", ":")).encode("utf-8"))

    @property
    def name(self) -> str:
        return canonical_layer_name(
            publisher=self.publisher, model_name=self.model_name,
            model_digest=self.model_digest, profile=self.profile,
            graph_digest=self.graph_digest, role_kind=self.role_kind,
            layer_begin=self.layer_begin, layer_end=self.layer_end, rank=self.rank,
            recipe_digest=self.recipe_digest, object_digest=self.object_digest,
            model_manifest_digest=self.model_manifest_digest,
            layer_manifest_digest=self.layer_manifest_digest)

    @property
    def manifest_digest(self) -> str:
        return _digest(json.dumps(self.to_dict(), sort_keys=True,
                                  separators=(",", ":")).encode())


class CanonicalLayerCatalog:
    """Idempotent root-last catalog without duplicate semantic objects."""

    def __init__(self) -> None:
        self._layers: dict[str, CanonicalLayerManifest] = {}
        self._bytes: dict[str, bytes] = {}
        self._root_published = False

    def publish_layer(self, manifest: CanonicalLayerManifest, payload: bytes) -> str:
        if _digest(bytes(payload)) != manifest.object_digest:
            raise ValueError("canonical layer object digest mismatch")
        if len(payload) != manifest.object_bytes:
            raise ValueError("canonical layer object size mismatch")
        existing = self._layers.get(manifest.name)
        if existing is not None:
            if existing != manifest or self._bytes[manifest.name] != bytes(payload):
                raise ValueError("canonical layer name/content conflict")
            return manifest.name
        self._layers[manifest.name] = manifest
        self._bytes[manifest.name] = bytes(payload)
        self._root_published = False
        return manifest.name

    def publish_root(self) -> str:
        if not self._layers:
            raise ValueError("cannot publish empty canonical root")
        self._root_published = True
        return _digest(json.dumps(
            [self._layers[name].to_dict() for name in sorted(self._layers)],
            sort_keys=True, separators=(",", ":")).encode())

    def root(self) -> tuple[CanonicalLayerManifest, ...]:
        if not self._root_published:
            raise RuntimeError("canonical root is not published")
        return tuple(self._layers[name] for name in sorted(self._layers))

    def publish_via(self, publish_object, *, publisher: str,
                    deadline_ms: int | None = None) -> str:
        """Publish immutable layer Data then the root through a Repo adapter.

        The callback owns NDNSF-DistributedRepo transport and must be
        idempotent for an already committed content-addressed name.  Root-last
        ordering prevents a consumer from observing a partial catalog.
        """
        if not callable(publish_object):
            raise TypeError("canonical catalog publisher is not callable")
        import time
        if not self._layers:
            raise ValueError("cannot publish empty canonical root")
        for name in sorted(self._layers):
            if deadline_ms is not None and int(time.time() * 1000) >= int(deadline_ms):
                raise TimeoutError("canonical catalog publication deadline expired")
            manifest = self._layers[name]
            result = publish_object(
                name=name, payload=self._bytes[name],
                manifest=manifest.to_dict(), idempotency_key=manifest.object_digest)
            if str(result) != name:
                raise ValueError("Repo publisher returned a different canonical name")
        root_digest = self.publish_root()
        root_name = (str(publisher).rstrip("/")
                     + "/NDNSF-DI/MODEL/v1/ROOT/" + root_digest)
        if deadline_ms is not None and int(time.time() * 1000) >= int(deadline_ms):
            raise TimeoutError("canonical root publication deadline expired")
        result = publish_object(
            name=root_name,
            payload=json.dumps([item.to_dict() for item in self.root()],
                               sort_keys=True, separators=(",", ":")).encode(),
            manifest={"schema": "ndnsf-di-canonical-root-v1",
                      "rootDigest": root_digest,
                      "layers": [item.name for item in self.root()]},
            idempotency_key=root_digest)
        if str(result) != root_name:
            raise ValueError("Repo publisher returned a different canonical root name")
        return root_name


@dataclass(frozen=True)
class AssembledOnnxArtifactV1:
    manifest: Mapping[str, Any]
    entries: Mapping[str, bytes]
    signer: str
    signature: str

    def __post_init__(self) -> None:
        names = tuple(sorted(self.entries))
        if any(name not in ALLOWED_ENTRIES for name in names):
            raise ValueError("unsupported ONNX bundle entry")
        if names not in (("model.onnx",), ("model.onnx", "model.onnx.data")):
            raise ValueError("ONNX bundle must contain model.onnx and optional external data")
        if not self.signer or not self.signature:
            raise ValueError("assembled artifact signer and signature are required")
        for name, payload in self.entries.items():
            if not isinstance(payload, (bytes, bytearray)) or len(payload) > 8 * 1024**3:
                raise ValueError("invalid ONNX bundle payload")

    def _manifest_bytes(self) -> bytes:
        return json.dumps(dict(self.manifest), sort_keys=True,
                          separators=(",", ":"), ensure_ascii=False).encode()

    def to_bytes(self) -> bytes:
        manifest = self._manifest_bytes()
        signature = self.signature.encode()
        out = bytearray(b"NDNSF-ONNX-ARTIFACT-V1\0")
        out.extend(struct.pack(">III", len(manifest), len(signature), len(self.entries)))
        out.extend(manifest)
        out.extend(signature)
        for name in sorted(self.entries):
            encoded = name.encode()
            payload = bytes(self.entries[name])
            out.extend(struct.pack(">IQ", len(encoded), len(payload)))
            out.extend(encoded)
            out.extend(payload)
        return bytes(out)

    @property
    def object_digest(self) -> str:
        return _digest(self.to_bytes())

    def write_atomic(self, target: str | Path) -> str:
        path = Path(target)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_bytes(self.to_bytes())
        temporary.replace(path)
        return self.object_digest

    @classmethod
    def from_bytes(cls, wire: bytes, *, max_size: int = 8 * 1024**3
                   ) -> "AssembledOnnxArtifactV1":
        raw = bytes(wire)
        if len(raw) > max_size or not raw.startswith(b"NDNSF-ONNX-ARTIFACT-V1\0"):
            raise ValueError("malformed ONNX artifact framing")
        offset = len(b"NDNSF-ONNX-ARTIFACT-V1\0")
        if len(raw) < offset + 12:
            raise ValueError("truncated ONNX artifact header")
        manifest_len, signature_len, entry_count = struct.unpack(
            ">III", raw[offset:offset + 12])
        offset += 12
        if entry_count not in (1, 2) or manifest_len > 1 * 1024 * 1024:
            raise ValueError("invalid ONNX artifact bounds")
        end = offset + manifest_len + signature_len
        if end > len(raw) or signature_len > 16 * 1024:
            raise ValueError("invalid ONNX artifact signature bounds")
        try:
            manifest = json.loads(raw[offset:offset + manifest_len].decode())
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("invalid embedded ONNX manifest") from exc
        signature = raw[offset + manifest_len:end].decode("utf-8")
        offset = end
        entries: dict[str, bytes] = {}
        for _ in range(entry_count):
            if offset + 12 > len(raw):
                raise ValueError("truncated ONNX artifact entry")
            name_len, payload_len = struct.unpack(">IQ", raw[offset:offset + 12])
            offset += 12
            if name_len <= 0 or name_len > 128 or payload_len > max_size:
                raise ValueError("invalid ONNX artifact entry bounds")
            if offset + name_len + payload_len > len(raw):
                raise ValueError("truncated ONNX artifact payload")
            name = raw[offset:offset + name_len].decode("utf-8")
            offset += name_len
            if name in entries or name not in ALLOWED_ENTRIES:
                raise ValueError("duplicate or unsafe ONNX artifact entry")
            entries[name] = raw[offset:offset + payload_len]
            offset += payload_len
        if offset != len(raw):
            raise ValueError("unexpected trailing ONNX artifact bytes")
        value = cls(manifest=manifest, entries=entries,
                    signer=str(manifest.get("signer", "")), signature=signature)
        return value

    def verify_provider(
        self,
        provider: str,
        *,
        allow_cross_provider: bool = False,
        verify_signature=None,
    ) -> None:
        """Validate Provider binding and the embedded signed manifest.

        Signature verification is injected because the identity/certificate
        backend belongs to NDNSF, not this model-neutral framing module.  A
        caller that supplies a verifier must return true for the exact
        canonical manifest bytes and signature; a missing verifier remains a
        deliberate compatibility mode for local unprotected artifacts.
        """
        if not provider or (not allow_cross_provider
                            and not self.signer.startswith(provider)):
            raise ValueError("assembled artifact signer is not the Provider identity")
        if self.manifest.get("objectDigest") == self.object_digest:
            raise ValueError("manifest cannot self-reference whole-file object digest")
        if self.manifest.get("signer") != self.signer:
            raise ValueError("assembled artifact signer/manifest mismatch")
        declared = dict(self.manifest.get("entryDigests", {}))
        if set(declared) != set(self.entries):
            raise ValueError("assembled artifact entry digest cover is incomplete")
        for name, payload in self.entries.items():
            if declared.get(name) != _digest(bytes(payload)):
                raise ValueError("assembled artifact entry digest mismatch")
        if verify_signature is not None:
            if not callable(verify_signature) or not bool(
                    verify_signature(self._manifest_bytes(), self.signature, self.signer)):
                raise ValueError("assembled artifact manifest signature is invalid")


__all__ = [
    "ALLOWED_ENTRIES", "AssembledOnnxArtifactV1", "CanonicalLayerCatalog",
    "CanonicalLayerManifest", "canonical_layer_name",
]
