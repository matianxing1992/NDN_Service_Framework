"""Content-addressed canonical layer and assembled ONNX identities.

The module is application/DI-owned; it does not replace DistributedRepo's
placement or replication policy.  It only builds deterministic names,
manifests, and Provider-local immutable bundles that a Repo client may publish
or fetch.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Any, Callable, Iterable, Mapping


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


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


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


def _digest_or_empty(value: str, field: str) -> str:
    if value:
        _require_digest(value, field)
    return value


def _ndn_identity(value: str, field: str) -> str:
    parts = tuple(part for part in str(value).strip("/").split("/") if part)
    if not parts:
        raise ValueError(f"{field} is required")
    return "/" + "/".join(_token(part, field) for part in parts)


@dataclass(frozen=True)
class ModelIdentity:
    """Placement-independent identity of one normalized source model.

    Human aliases and source filenames are deliberately confined to
    ``metadata`` and do not participate in ``digest``.  Repacking an equivalent
    checkpoint therefore preserves identity, while changing any normalized
    tensor, graph, configuration, execution, or preprocessing fact does not.
    """

    publisher: str
    origin_signature_identity: str
    provenance_digest: str
    source_content_digest: str
    normalized_tensor_map_digest: str
    parameter_config_digest: str
    execution_semantics_digest: str
    graph_digest: str
    normalized_initializer_content_digest: str = ""
    tokenizer_digest: str = ""
    preprocessing_digest: str = ""
    metadata: Mapping[str, Any] = field(default_factory=dict, compare=False)
    schema: str = "ndnsf-di-model-identity-v1"

    def __post_init__(self) -> None:
        if self.schema != "ndnsf-di-model-identity-v1":
            raise ValueError("unsupported model identity schema")
        object.__setattr__(self, "publisher", _ndn_identity(
            self.publisher, "model publisher"))
        object.__setattr__(self, "origin_signature_identity", _ndn_identity(
            self.origin_signature_identity, "origin signature identity"))
        for field_name in (
            "provenance_digest", "source_content_digest",
            "normalized_tensor_map_digest", "parameter_config_digest",
            "execution_semantics_digest", "graph_digest",
        ):
            _require_digest(getattr(self, field_name), field_name)
        _digest_or_empty(self.tokenizer_digest, "tokenizer_digest")
        _digest_or_empty(self.preprocessing_digest, "preprocessing_digest")
        _digest_or_empty(
            self.normalized_initializer_content_digest,
            "normalized_initializer_content_digest")
        metadata = dict(self.metadata)
        # Validate deterministic JSON compatibility at the trust boundary.
        _canonical_bytes(metadata)
        object.__setattr__(self, "metadata", metadata)

    def identity_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "publisher": self.publisher,
            "originSignatureIdentity": self.origin_signature_identity,
            "provenanceDigest": self.provenance_digest,
            "sourceContentDigest": self.source_content_digest,
            "normalizedTensorMapDigest": self.normalized_tensor_map_digest,
            "parameterConfigDigest": self.parameter_config_digest,
            "executionSemanticsDigest": self.execution_semantics_digest,
            "graphDigest": self.graph_digest,
            "normalizedInitializerContentDigest":
                self.normalized_initializer_content_digest,
            "tokenizerDigest": self.tokenizer_digest,
            "preprocessingDigest": self.preprocessing_digest,
        }

    def to_dict(self) -> dict[str, Any]:
        return {**self.identity_dict(), "metadata": dict(self.metadata)}

    @property
    def digest(self) -> str:
        return _digest(_canonical_bytes(self.identity_dict()))


@dataclass(frozen=True)
class CanonicalArtifactProfile:
    """Exact reusable ONNX/layerizer representation of a model identity."""

    layerizer_descriptor_digest: str
    adapter_descriptor_digest: str
    serialization_schema_digest: str
    chunking_layout_digest: str
    precision_format_digest: str
    protection_transform_digest: str
    protection_epoch: str
    tool_runtime_digest: str
    schema: str = "ndnsf-di-canonical-profile-v1"

    def __post_init__(self) -> None:
        if self.schema != "ndnsf-di-canonical-profile-v1":
            raise ValueError("unsupported canonical artifact profile schema")
        for field_name in (
            "layerizer_descriptor_digest", "adapter_descriptor_digest",
            "serialization_schema_digest", "chunking_layout_digest",
            "precision_format_digest", "protection_transform_digest",
            "tool_runtime_digest",
        ):
            _require_digest(getattr(self, field_name), field_name)
        _token(self.protection_epoch, "protection epoch")

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "layerizerDescriptorDigest": self.layerizer_descriptor_digest,
            "adapterDescriptorDigest": self.adapter_descriptor_digest,
            "serializationSchemaDigest": self.serialization_schema_digest,
            "chunkingLayoutDigest": self.chunking_layout_digest,
            "precisionFormatDigest": self.precision_format_digest,
            "protectionTransformDigest": self.protection_transform_digest,
            "protectionEpoch": self.protection_epoch,
            "toolRuntimeDigest": self.tool_runtime_digest,
        }

    @property
    def digest(self) -> str:
        return _digest(_canonical_bytes(self.to_dict()))


@dataclass(frozen=True)
class CanonicalObjectSlice:
    """Independently verifiable range selected from one canonical object."""

    object_name: str
    tensor_name: str
    dtype: str
    shape: tuple[int | str, ...]
    byte_order: str
    offset: int
    length: int
    chunk_digest: str
    payload: bytes = field(repr=False, compare=False)

    def verify(self) -> None:
        if len(self.payload) != self.length:
            raise ValueError("canonical object slice length mismatch")
        if _digest(bytes(self.payload)) != self.chunk_digest:
            raise ValueError("canonical object slice digest mismatch")


@dataclass(frozen=True)
class CanonicalArtifactBinding:
    """Read-only root facts needed to seal Provider assembly recipes."""

    model_manifest_digest: str
    artifact_profile_digest: str
    graph_digest: str
    canonical_initializer_digest: str
    adapter_descriptor_digest: str
    assembler_descriptor_digest: str
    backend_abi: str
    canonical_source_bytes: int

    def __post_init__(self) -> None:
        for field_name in (
            "model_manifest_digest", "artifact_profile_digest", "graph_digest",
            "canonical_initializer_digest", "adapter_descriptor_digest",
            "assembler_descriptor_digest",
        ):
            _require_digest(getattr(self, field_name), field_name)
        if not self.backend_abi:
            raise ValueError("canonical artifact binding requires backend ABI")
        if self.canonical_source_bytes <= 0:
            raise ValueError("canonical artifact binding requires source bytes")


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


def _normalize_tensor_index(
    values: Iterable[Mapping[str, Any]], *, object_bytes: int,
) -> tuple[dict[str, Any], ...]:
    normalized: list[dict[str, Any]] = []
    names: set[str] = set()
    ranges: list[tuple[int, int]] = []
    for raw in values:
        item = dict(raw)
        name = str(item.get("tensorName", item.get("name", "")))
        if not name or name in names:
            raise ValueError("canonical tensor index requires unique tensor names")
        dtype = str(item.get("dtype", ""))
        shape_raw = item.get("shape", ())
        if not dtype or not isinstance(shape_raw, (list, tuple)):
            raise ValueError("canonical tensor index requires dtype and shape")
        shape: list[int | str] = []
        for dimension in shape_raw:
            if isinstance(dimension, bool) or not isinstance(dimension, (int, str)):
                raise ValueError("canonical tensor shape is invalid")
            if isinstance(dimension, int) and dimension < 0:
                raise ValueError("canonical tensor shape is invalid")
            if isinstance(dimension, str) and not dimension:
                raise ValueError("canonical tensor shape is invalid")
            shape.append(dimension)
        byte_order = str(item.get("byteOrder", "little")).lower()
        if byte_order not in {"little", "big", "na"}:
            raise ValueError("canonical tensor byte order is invalid")
        offset = int(item.get("offset", -1))
        length = int(item.get("length", -1))
        chunk_digest = str(item.get("chunkDigest", ""))
        _require_digest(chunk_digest, "tensor chunk digest")
        if offset < 0 or length <= 0 or offset + length > object_bytes:
            raise ValueError("canonical tensor range escapes its object")
        if any(offset < end and start < offset + length for start, end in ranges):
            raise ValueError("canonical tensor ranges overlap")
        shared_reference = str(item.get("sharedReference", ""))
        normalized.append({
            "tensorName": name,
            "dtype": dtype,
            "shape": shape,
            "byteOrder": byte_order,
            "offset": offset,
            "length": length,
            "chunkDigest": chunk_digest,
            "sharedReference": shared_reference,
        })
        names.add(name)
        ranges.append((offset, offset + length))
    if not normalized:
        raise ValueError("canonical layer requires a tensor/chunk index")
    return tuple(sorted(normalized, key=lambda item: (
        item["tensorName"], item["offset"], item["length"])))


@dataclass(frozen=True)
class CanonicalLayerManifest:
    model_name: str
    model_digest: str
    profile: str
    graph_digest: str
    role_kind: str
    layer_begin: int
    layer_end: int
    rank: int = field(compare=False)
    recipe_digest: str
    object_digest: str
    object_bytes: int
    publisher: str
    origin_attestation: str
    transformation_attestation: str
    tensor_index: tuple[Mapping[str, Any], ...] = ()
    graph_nodes: tuple[str, ...] = ()
    input_contracts: tuple[Mapping[str, Any], ...] = ()
    output_contracts: tuple[Mapping[str, Any], ...] = ()
    model_manifest_name_digest: str = field(default="", compare=False)
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
        _digest_or_empty(
            self.model_manifest_name_digest, "model_manifest_name_digest")
        if not self.publisher or not self.origin_attestation or not self.transformation_attestation:
            raise ValueError("canonical layer trust attestations are required")
        object.__setattr__(self, "publisher", _ndn_identity(
            self.publisher, "canonical publisher"))
        object.__setattr__(self, "tensor_index", _normalize_tensor_index(
            self.tensor_index, object_bytes=self.object_bytes))
        graph_nodes = tuple(sorted(str(item) for item in self.graph_nodes))
        if not graph_nodes or any(not item for item in graph_nodes):
            raise ValueError("canonical layer requires graph-node membership")
        object.__setattr__(self, "graph_nodes", graph_nodes)
        for field_name in ("input_contracts", "output_contracts"):
            contracts = tuple(dict(item) for item in getattr(self, field_name))
            if not contracts:
                raise ValueError(
                    "canonical layer requires input and output tensor contracts")
            _canonical_bytes(contracts)
            object.__setattr__(self, field_name, contracts)
        # Validate the complete semantic NDN identity at construction time;
        # callers must not be able to carry an invalid alias until publish.
        self.name

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "modelName": self.model_name, "modelDigest": self.model_digest,
            "profile": self.profile, "graphDigest": self.graph_digest,
            "roleKind": self.role_kind, "layerBegin": self.layer_begin,
            "layerEnd": self.layer_end,
            "recipeDigest": self.recipe_digest, "objectDigest": self.object_digest,
            "objectBytes": self.object_bytes, "publisher": self.publisher,
            "originAttestation": self.origin_attestation,
            "transformationAttestation": self.transformation_attestation,
            "tensorIndex": list(self.tensor_index),
            "graphNodes": list(self.graph_nodes),
            "inputContracts": list(self.input_contracts),
            "outputContracts": list(self.output_contracts),
        }

    @property
    def profile_digest(self) -> str:
        return _identity_digest(self.profile, "profile")

    @property
    def model_manifest_digest(self) -> str:
        if self.model_manifest_name_digest:
            return self.model_manifest_name_digest
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
            "graphNodes": list(self.graph_nodes),
            "inputContracts": list(self.input_contracts),
            "outputContracts": list(self.output_contracts),
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
    def manifest_name(self) -> str:
        return self.name.rsplit("/OBJECT/", 1)[0]

    @property
    def manifest_digest(self) -> str:
        return _digest(json.dumps(self.to_dict(), sort_keys=True,
                                  separators=(",", ":")).encode())


@dataclass(frozen=True)
class CanonicalModelManifest:
    """Signed ACTIVE root for one complete model/profile publication."""

    model_name: str
    model_identity: ModelIdentity
    artifact_profile: CanonicalArtifactProfile
    layer_manifest_digests: tuple[str, ...]
    origin_attestation: str
    transformation_attestation: str
    activation_epoch: str
    signer: str
    signature: str
    metadata: Mapping[str, Any]
    state: str = "ACTIVE"
    schema: str = "ndnsf-di-canonical-model-manifest-v1"

    _REQUIRED_METADATA = frozenset({
        "architecture", "parameterCount", "layerCount", "hiddenSize",
        "attentionHeads", "experts", "precision", "sourceRevision",
    })

    def __post_init__(self) -> None:
        if self.schema != "ndnsf-di-canonical-model-manifest-v1":
            raise ValueError("unsupported canonical model manifest schema")
        if self.state != "ACTIVE":
            raise ValueError("only a completely verified ACTIVE root is publishable")
        name = "/".join(_token(part, "model name component")
                        for part in str(self.model_name).strip("/").split("/"))
        if not name:
            raise ValueError("model name is required")
        object.__setattr__(self, "model_name", name)
        digests = tuple(sorted(set(self.layer_manifest_digests)))
        if not digests or len(digests) != len(self.layer_manifest_digests):
            raise ValueError("model manifest layer cover is empty or duplicated")
        for digest in digests:
            _require_digest(digest, "layer_manifest_digest")
        object.__setattr__(self, "layer_manifest_digests", digests)
        if not self.origin_attestation or not self.transformation_attestation:
            raise ValueError("model manifest attestations are required")
        _token(self.activation_epoch, "activation epoch")
        object.__setattr__(self, "signer", _ndn_identity(
            self.signer, "model manifest signer"))
        if not self.signature:
            raise ValueError("model manifest signature is required")
        metadata = dict(self.metadata)
        missing = self._REQUIRED_METADATA - set(metadata)
        if missing:
            raise ValueError(
                "model manifest metadata is incomplete: " + ",".join(sorted(missing)))
        _canonical_bytes(metadata)
        object.__setattr__(self, "metadata", metadata)

    def unsigned_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "state": self.state,
            "modelName": self.model_name,
            "modelIdentity": self.model_identity.to_dict(),
            "modelIdentityDigest": self.model_identity.digest,
            "artifactProfile": self.artifact_profile.to_dict(),
            "artifactProfileDigest": self.artifact_profile.digest,
            "layerManifestDigests": list(self.layer_manifest_digests),
            "originAttestation": self.origin_attestation,
            "transformationAttestation": self.transformation_attestation,
            "activationEpoch": self.activation_epoch,
            "signer": self.signer,
            "metadata": dict(self.metadata),
        }

    def to_dict(self) -> dict[str, Any]:
        return {**self.unsigned_dict(), "signature": self.signature}

    @property
    def digest(self) -> str:
        return _digest(_canonical_bytes(self.to_dict()))

    @property
    def name(self) -> str:
        return (f"{self.model_identity.publisher}/NDNSF-DI/MODEL/v1/NAME/"
                f"{self.model_name}/MID/{self.model_identity.digest}/PROFILE/"
                f"{self.artifact_profile.digest}/MANIFEST/{self.digest}")

    def verify(
        self,
        *,
        verify_origin: Callable[[ModelIdentity, str], bool],
        verify_transformation: Callable[[ModelIdentity, CanonicalArtifactProfile,
                                         str], bool],
        verify_signature: Callable[[bytes, str, str], bool],
    ) -> None:
        if not bool(verify_origin(self.model_identity, self.origin_attestation)):
            raise ValueError("canonical origin attestation is invalid")
        if not bool(verify_transformation(
                self.model_identity, self.artifact_profile,
                self.transformation_attestation)):
            raise ValueError("canonical transformation attestation is invalid")
        if not bool(verify_signature(
                _canonical_bytes(self.unsigned_dict()), self.signature, self.signer)):
            raise ValueError("canonical model manifest signature is invalid")


class CanonicalLayerCatalog:
    """Idempotent object/layer publication with a verified root-last barrier."""

    def __init__(self) -> None:
        self._layers: dict[str, CanonicalLayerManifest] = {}
        self._bytes: dict[str, bytes] = {}
        self._root_published = False
        self._active_manifest: CanonicalModelManifest | None = None

    @property
    def total_object_bytes(self) -> int:
        """Return the exact immutable byte cover represented by this catalog."""

        return sum(item.object_bytes for item in self._layers.values())

    def publish_layer(self, manifest: CanonicalLayerManifest, payload: bytes) -> str:
        payload = bytes(payload)
        if _digest(payload) != manifest.object_digest:
            raise ValueError("canonical layer object digest mismatch")
        if len(payload) != manifest.object_bytes:
            raise ValueError("canonical layer object size mismatch")
        if manifest.model_digest not in manifest.origin_attestation:
            raise ValueError("canonical origin attestation does not bind the model")
        if manifest.recipe_digest not in manifest.transformation_attestation:
            raise ValueError(
                "canonical transformation attestation does not bind the recipe")
        for item in manifest.tensor_index:
            start = int(item["offset"])
            end = start + int(item["length"])
            if _digest(payload[start:end]) != item["chunkDigest"]:
                raise ValueError(
                    f"canonical tensor chunk digest mismatch for {item['tensorName']}")
        key = manifest.layer_manifest_digest
        existing = self._layers.get(key)
        if existing is not None:
            if existing != manifest or self._bytes[key] != payload:
                raise ValueError("canonical layer name/content conflict")
            return existing.name
        self._layers[key] = manifest
        self._bytes[key] = payload
        self._root_published = False
        self._active_manifest = None
        return manifest.name

    def activate_model(
        self,
        *,
        model_name: str,
        model_identity: ModelIdentity,
        artifact_profile: CanonicalArtifactProfile,
        origin_attestation: str,
        transformation_attestation: str,
        activation_epoch: str,
        signer: str,
        signature: str,
        metadata: Mapping[str, Any],
        verify_origin: Callable[[ModelIdentity, str], bool],
        verify_transformation: Callable[[ModelIdentity, CanonicalArtifactProfile,
                                         str], bool],
        verify_signature: Callable[[bytes, str, str], bool],
    ) -> CanonicalModelManifest:
        if not self._layers:
            raise ValueError("cannot activate an empty canonical root")
        for layer in self._layers.values():
            if (layer.model_digest != model_identity.digest
                    or layer.profile_digest != artifact_profile.digest
                    or layer.graph_digest != model_identity.graph_digest):
                raise ValueError("canonical root/layer identity cover mismatch")
        root = CanonicalModelManifest(
            model_name=model_name,
            model_identity=model_identity,
            artifact_profile=artifact_profile,
            layer_manifest_digests=tuple(self._layers),
            origin_attestation=origin_attestation,
            transformation_attestation=transformation_attestation,
            activation_epoch=activation_epoch,
            signer=signer,
            signature=signature,
            metadata=metadata,
        )
        root.verify(
            verify_origin=verify_origin,
            verify_transformation=verify_transformation,
            verify_signature=verify_signature,
        )
        self._layers = {
            digest: replace(layer, model_manifest_name_digest=root.digest)
            for digest, layer in self._layers.items()
        }
        self._active_manifest = root
        self._root_published = True
        return root

    def publish_root(self) -> str:
        """Compatibility root for callers without a signed model manifest.

        New V3 code should call :meth:`activate_model`; this method preserves
        the older local test/helper surface but does not create an ACTIVE model
        manifest suitable for Provider selection.
        """
        if not self._layers:
            raise ValueError("cannot publish empty canonical root")
        self._root_published = True
        return _digest(_canonical_bytes(
            [self._layers[key].to_dict() for key in sorted(self._layers)]))

    @property
    def active_manifest(self) -> CanonicalModelManifest:
        if self._active_manifest is None:
            raise RuntimeError("canonical model manifest is not ACTIVE")
        return self._active_manifest

    def root(self) -> tuple[CanonicalLayerManifest, ...]:
        if not self._root_published:
            raise RuntimeError("canonical root is not published")
        return tuple(self._layers[key] for key in sorted(self._layers))

    def select_tensors(self, tensor_names: Iterable[str]) -> tuple[CanonicalObjectSlice, ...]:
        """Return only independently verified ranges required by a role."""
        if self._active_manifest is None:
            raise RuntimeError("selective retrieval requires an ACTIVE model root")
        required = {str(name) for name in tensor_names}
        if not required or "" in required:
            raise ValueError("selective retrieval requires tensor names")
        selected: list[CanonicalObjectSlice] = []
        found: set[str] = set()
        for digest in sorted(self._layers):
            layer = self._layers[digest]
            payload = self._bytes[digest]
            for item in layer.tensor_index:
                name = str(item["tensorName"])
                if name not in required:
                    continue
                start = int(item["offset"])
                length = int(item["length"])
                value = CanonicalObjectSlice(
                    object_name=layer.name,
                    tensor_name=name,
                    dtype=str(item["dtype"]),
                    shape=tuple(item["shape"]),
                    byte_order=str(item["byteOrder"]),
                    offset=start,
                    length=length,
                    chunk_digest=str(item["chunkDigest"]),
                    payload=payload[start:start + length],
                )
                value.verify()
                selected.append(value)
                found.add(name)
        missing = required - found
        if missing:
            raise KeyError("canonical tensors are absent: " + ",".join(sorted(missing)))
        return tuple(sorted(selected, key=lambda item: (
            item.object_name, item.offset, item.tensor_name)))

    def select_components(
        self, *, role_kind: str, layer_begin: int, layer_end: int,
    ) -> tuple[CanonicalLayerManifest, ...]:
        """Resolve the complete canonical component cover for one role range."""
        if self._active_manifest is None:
            raise RuntimeError("component retrieval requires an ACTIVE model root")
        kind = _component_kind(role_kind)
        if layer_begin < 0 or layer_end <= layer_begin:
            raise ValueError("invalid canonical component selector")
        selected = tuple(sorted(
            (item for item in self._layers.values()
             if item.role_kind == kind
             and item.layer_begin >= layer_begin
             and item.layer_end <= layer_end),
            key=lambda item: (item.layer_begin, item.layer_end,
                              item.layer_manifest_digest),
        ))
        cursor = layer_begin
        for item in selected:
            if item.layer_begin != cursor:
                raise ValueError("canonical component cover has a gap or overlap")
            cursor = item.layer_end
        if cursor != layer_end:
            raise ValueError("canonical component cover is incomplete")
        return selected

    def publish_via(self, publish_object, *, publisher: str,
                    deadline_ms: int | None = None) -> str:
        """Publish objects, then layer manifests, then the root manifest."""
        if not callable(publish_object):
            raise TypeError("canonical catalog publisher is not callable")
        import time
        if not self._layers:
            raise ValueError("cannot publish empty canonical root")
        if self._active_manifest is None:
            raise RuntimeError("Repo publication requires a verified ACTIVE root")

        def check_deadline() -> None:
            if deadline_ms is not None and int(time.time() * 1000) >= int(deadline_ms):
                raise TimeoutError("canonical catalog publication deadline expired")

        for digest in sorted(self._layers):
            check_deadline()
            manifest = self._layers[digest]
            object_name = manifest.name
            result = publish_object(
                name=object_name, payload=self._bytes[digest],
                manifest={"schema": "ndnsf-di-canonical-object-v1",
                          "objectDigest": manifest.object_digest,
                          "layerManifestDigest": manifest.layer_manifest_digest},
                idempotency_key=manifest.object_digest)
            if str(result) != object_name:
                raise ValueError("Repo publisher returned a different canonical name")
            check_deadline()
            layer_payload = _canonical_bytes(manifest.to_dict())
            result = publish_object(
                name=manifest.manifest_name, payload=layer_payload,
                manifest=manifest.to_dict(),
                idempotency_key=manifest.layer_manifest_digest)
            if str(result) != manifest.manifest_name:
                raise ValueError("Repo publisher returned a different layer manifest name")
        check_deadline()
        root = self._active_manifest
        result = publish_object(
            name=root.name, payload=_canonical_bytes(root.to_dict()),
            manifest=root.to_dict(), idempotency_key=root.digest)
        if str(result) != root.name:
            raise ValueError("Repo publisher returned a different canonical root name")
        return root.name


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
    "ALLOWED_ENTRIES", "AssembledOnnxArtifactV1", "CanonicalArtifactBinding",
    "CanonicalArtifactProfile", "CanonicalLayerCatalog", "CanonicalLayerManifest",
    "CanonicalModelManifest", "CanonicalObjectSlice", "ModelIdentity",
    "canonical_layer_name",
]
