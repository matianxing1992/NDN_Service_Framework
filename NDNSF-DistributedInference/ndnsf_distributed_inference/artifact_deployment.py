"""Provider-side artifact materialization helpers.

These helpers are intentionally above NDNSF Core. They understand DI artifact
reference files and repo manifests, then write verified files into a local
provider cache. A caller may supply either a real ``NetworkDistributedRepoClient``
or a local repo-compatible object for smoke tests.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
import hashlib
import json
import os
from pathlib import Path
import tempfile
import stat
import threading
import time
from typing import Any, Callable, Mapping, Optional

from ndnsf import (
    AckDecision,
    NEGATIVE_ACK_REASON_INTERNAL_ERROR,
    NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
    ServiceOperationState,
    ServiceOperationStatus,
    encode_ack_metadata,
)
from ndnsf import (
    SegmentHintRange,
    fetch_known_segmented_object_with_segment_hints,
    fetch_segmented_object,
    fetch_segmented_object_with_segment_hints,
)


from py_repoclient.orchestration import NetworkDistributedRepoClient, RepoObjectManifest

from .core.contracts import ProviderResidencyIdentity
from .repo_reference import repo_manifest_from_large_data_reference


class _ManifestOnlyRepoClient:
    timeout_ms = 60000
    data_name = staticmethod(NetworkDistributedRepoClient.data_name)

    def fetch_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
        if manifest is None:
            raise ValueError("manifest-only artifact fetch requires repoManifest")
        return NetworkDistributedRepoClient.fetch_object(self, object_name, manifest)


@dataclass(frozen=True)
class MaterializedArtifact:
    role: str
    slot: str
    path: Path
    manifest: RepoObjectManifest
    executable: bool = False
    metadata: dict[str, Any] | None = None


@dataclass(frozen=True)
class ProviderAssemblyResult:
    """Immutable Provider-local activation record for one assembled role."""

    role: str
    path: Path
    object_digest: str
    artifact: Any


class CanonicalCatalogEnsurer:
    """Concrete V3 canonical-ensure port backed by one ACTIVE catalog.

    The first post-ACK plan publishes the immutable object/layer/root sequence.
    Later placements only verify their component cover and reuse that exact root;
    they never generate or republish role-specific model bytes.
    """

    def __init__(self, catalog, publish_object, *, publisher: str,
                 assembler_descriptor_digest: str = "",
                 backend_abi: str = "onnxruntime-runtime") -> None:
        from .app_sdk.canonical_artifacts import CanonicalLayerCatalog
        if not isinstance(catalog, CanonicalLayerCatalog):
            raise TypeError("canonical ensurer requires a CanonicalLayerCatalog")
        if not callable(publish_object) or not str(publisher).startswith("/"):
            raise ValueError("canonical ensurer requires a Repo publisher identity")
        self._catalog = catalog
        self._publish_object = publish_object
        self._publisher = str(publisher)
        self._assembler_descriptor_digest = (
            str(assembler_descriptor_digest)
            or "sha256:" + hashlib.sha256(
                b"ndnsf-di-certified-onnx-assembler-v1").hexdigest())
        if (not self._assembler_descriptor_digest.startswith("sha256:")
                or len(self._assembler_descriptor_digest) != 71
                or not backend_abi):
            raise ValueError("canonical ensurer assembler identity is invalid")
        self._backend_abi = str(backend_abi)
        self._lock = threading.Lock()
        self._published_root_name = ""
        self._published_root_digest = ""

    def describe(self, candidate=None):
        """Return immutable catalog facts without publishing or mutating it."""
        from .app_sdk.canonical_artifacts import CanonicalArtifactBinding
        root = self._catalog.active_manifest
        if candidate is not None and candidate.graph_digest != root.model_identity.graph_digest:
            raise ValueError("candidate graph does not match canonical model root")
        return CanonicalArtifactBinding(
            model_manifest_digest=root.digest,
            artifact_profile_digest=root.artifact_profile.digest,
            graph_digest=root.model_identity.graph_digest,
            canonical_initializer_digest=(
                root.model_identity.normalized_initializer_content_digest
                or root.model_identity.source_content_digest),
            adapter_descriptor_digest=root.artifact_profile.adapter_descriptor_digest,
            assembler_descriptor_digest=self._assembler_descriptor_digest,
            backend_abi=self._backend_abi,
            canonical_source_bytes=self._catalog.total_object_bytes,
        )

    def ensure(self, candidate, role_specs, *, deadline_ms: int):
        from .app_sdk.placement import PublishedSplit
        if int(time.time() * 1000) >= int(deadline_ms):
            raise TimeoutError("canonical ensure deadline expired")
        specs = tuple(role_specs)
        if not specs:
            raise ValueError("canonical ensure requires role assembly specs")
        role_counts = {
            spec.role: sum(other.role == spec.role for other in specs)
            for spec in specs
        }
        digests: dict[str, str] = {}
        for spec in specs:
            # Coverage validation is deliberately independent of Provider and
            # request/attempt. Those identities belong to Selection, not the
            # canonical model namespace.
            self._catalog.select_components(
                role_kind=spec.role_kind,
                layer_begin=spec.layer_begin,
                layer_end=spec.layer_end,
            )
            key = (spec.role if role_counts[spec.role] == 1
                   else f"{spec.role}#{spec.rank}")
            if key in digests:
                raise ValueError("canonical ensure role/rank key is duplicated")
            digests[key] = spec.artifact_digest

        root = self._catalog.active_manifest
        with self._lock:
            if self._published_root_name:
                if self._published_root_digest != root.digest:
                    raise ValueError("canonical ensurer root changed after publication")
                root_name = self._published_root_name
            else:
                root_name = self._catalog.publish_via(
                    self._publish_object, publisher=self._publisher,
                    deadline_ms=deadline_ms)
                self._published_root_name = root_name
                self._published_root_digest = root.digest
        return PublishedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role=digests,
            artifact_data_names_by_role={key: root_name for key in digests},
        )


def assemble_onnx_role_v3(
    *,
    role_spec,
    recipe,
    canonical_model: bytes,
    model_name: str,
    model_digest: str,
    profile_digest: str,
    provider: str,
    signature: str,
    cache_dir: str | Path,
    verify_signature=None,
) -> ProviderAssemblyResult:
    """Build and atomically activate one Provider-owned ONNX role bundle.

    The caller supplies one sealed ``RoleAssemblySpec`` and its matching
    digest-pinned adapter certificate.  The Provider performs extraction,
    checker validation, exact I/O validation, and ONNX Runtime loading locally;
    it never accepts a requester-selected filesystem path or executable.
    """
    from .adapters.onnx.executor import (
        CertifiedOnnxAssemblyRecipe, assemble_certified_onnx_model,
    )
    from .app_sdk.canonical_artifacts import AssembledOnnxArtifactV1
    if not isinstance(recipe, CertifiedOnnxAssemblyRecipe):
        raise TypeError("V3 ONNX assembly requires a certified recipe")
    if not provider or not str(provider).startswith("/") or not signature:
        raise ValueError("V3 ONNX assembly requires Provider signing identity")
    for name, value in (
        ("model_digest", model_digest), ("profile_digest", profile_digest),
    ):
        if (not isinstance(value, str) or not value.startswith("sha256:")
                or len(value) != 71):
            raise ValueError(f"{name} must be a canonical digest")
    if (getattr(role_spec, "model_manifest_digest", "")
            and role_spec.model_manifest_digest != recipe.model_manifest_digest):
        raise ValueError("RoleAssemblySpec model manifest mismatch")
    if (getattr(role_spec, "artifact_profile_digest", "")
            and role_spec.artifact_profile_digest != profile_digest):
        raise ValueError("RoleAssemblySpec artifact profile mismatch")
    if getattr(role_spec, "graph_digest", "") != recipe.graph_digest:
        raise ValueError("RoleAssemblySpec graph mismatch")

    assembled = assemble_certified_onnx_model(
        canonical_model, role_spec=role_spec, recipe=recipe)
    entries = {"model.onnx": assembled.model_bytes}
    manifest = {
        "schema": "ndnsf-di-assembled-onnx-v1",
        "modelName": str(model_name),
        "modelDigest": str(model_digest),
        "modelManifestDigest": recipe.model_manifest_digest,
        "artifactProfileDigest": str(profile_digest),
        "graphDigest": recipe.graph_digest,
        "role": str(role_spec.role),
        "roleKind": str(role_spec.role_kind),
        "rank": int(role_spec.rank),
        "layerBegin": int(role_spec.layer_begin),
        "layerEnd": int(role_spec.layer_end),
        "recipeDigest": recipe.digest,
        "adapterDescriptorDigest": recipe.adapter_descriptor_digest,
        "assemblerDescriptorDigest": recipe.assembler_descriptor_digest,
        "backendAbi": recipe.backend_abi,
        "precision": recipe.precision,
        "quantization": recipe.quantization,
        "layout": recipe.layout,
        "padding": recipe.padding,
        "inputNames": list(assembled.input_names),
        "outputNames": list(assembled.output_names),
        "nodeCount": assembled.node_count,
        "entryDigests": {"model.onnx": assembled.model_digest},
        "entryLengths": {"model.onnx": len(assembled.model_bytes)},
        "onnxChecker": "PASS",
        "onnxRuntimeLoad": "PASS",
        "signer": str(provider),
        "layoutMode": "INLINE_ONNX",
    }
    artifact = AssembledOnnxArtifactV1(
        manifest=manifest, entries=entries,
        signer=str(provider), signature=str(signature))
    artifact.verify_provider(str(provider), verify_signature=verify_signature)
    root = Path(cache_dir).expanduser().resolve()
    target_dir = root / "assembled"
    target_dir.mkdir(parents=True, exist_ok=True)
    digest_hex = (artifact.object_digest[7:]
                  if artifact.object_digest.startswith("sha256:")
                  else artifact.object_digest)
    target = target_dir / (digest_hex + ".ndnsf-onnx-artifact")
    if target.is_symlink():
        raise ValueError("assembled artifact target must not be a symlink")
    wire = artifact.to_bytes()
    if target.exists():
        if not target.is_file() or target.read_bytes() != wire:
            raise ValueError("assembled content-addressed path conflict")
    else:
        artifact.write_atomic(target)
    checked = AssembledOnnxArtifactV1.from_bytes(target.read_bytes())
    checked.verify_provider(str(provider), verify_signature=verify_signature)
    if checked.object_digest != artifact.object_digest:
        raise ValueError("activated assembled artifact identity changed")
    return ProviderAssemblyResult(
        role=str(role_spec.role), path=target,
        object_digest=artifact.object_digest, artifact=checked)


def assemble_onnx_role(
    *,
    role: str,
    model_name: str,
    model_digest: str,
    profile: str,
    graph_digest: str,
    layer_payloads: dict[str, bytes],
    layer_digests: dict[str, str],
    recipe_digest: str,
    provider: str,
    signature: str,
    output_path: str | Path,
    verify_signature=None,
) -> ProviderAssemblyResult:
    """Assemble verified canonical layers without request-directory copies.

    ``layer_payloads`` is ordered by its canonical NDN name.  The resulting
    file is written through a private sibling and atomically activated only
    after framing, digest, Provider identity, and signature checks complete.
    """
    from .app_sdk.canonical_artifacts import AssembledOnnxArtifactV1
    if not role or not provider or not layer_payloads:
        raise ValueError("Provider assembly requires role, Provider, and layers")
    if set(layer_payloads) != set(layer_digests):
        raise ValueError("layer digest cover is incomplete")
    ordered = tuple(sorted(layer_payloads.items()))
    for name, payload in ordered:
        digest = str(layer_digests[name])
        actual = "sha256:" + hashlib.sha256(bytes(payload)).hexdigest()
        if digest != actual:
            raise ValueError(f"canonical layer digest mismatch for {name}")
    model_bytes = b"".join(bytes(payload) for _, payload in ordered)
    entry_digests = {
        "model.onnx": "sha256:" + hashlib.sha256(model_bytes).hexdigest(),
    }
    manifest = {
        "schema": "ndnsf-di-assembled-onnx-v1",
        "modelName": str(model_name),
        "modelDigest": str(model_digest),
        "profile": str(profile),
        "graphDigest": str(graph_digest),
        "role": str(role),
        "recipeDigest": str(recipe_digest),
        "layerNames": [name for name, _ in ordered],
        "layerDigests": [layer_digests[name] for name, _ in ordered],
        "entryDigests": entry_digests,
        "signer": str(provider),
        "layout": "INLINE_ONNX",
    }
    artifact = AssembledOnnxArtifactV1(
        manifest=manifest, entries={"model.onnx": model_bytes},
        signer=str(provider), signature=str(signature))
    artifact.verify_provider(str(provider), verify_signature=verify_signature)
    target = Path(output_path).expanduser()
    if target.suffix != ".ndnsf-onnx-artifact":
        raise ValueError("assembled output must use .ndnsf-onnx-artifact suffix")
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".tmp")
    try:
        artifact.write_atomic(temporary)
        # Verify the exact bytes that will become visible before activation.
        checked = AssembledOnnxArtifactV1.from_bytes(temporary.read_bytes())
        checked.verify_provider(str(provider), verify_signature=verify_signature)
        temporary.replace(target)
    except Exception:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise
    return ProviderAssemblyResult(
        role=str(role), path=target, object_digest=artifact.object_digest,
        artifact=artifact)


class ArtifactProvisioningState:
    """Generic async provisioning/readiness state for model/runtime artifacts.

    Providers use this for any artifact-backed runtime, not just one model
    family: ONNX shards, llama.cpp GGUF runtimes, TensorRT engines, vLLM
    bundles, or future containerized runners can all install in the background
    and expose the same ACK readiness contract.
    """

    def __init__(
        self,
        *,
        component: str = "artifact runtime",
        initial_status: str = "predeployed",
        initial_message: str = "predeployed runtime endpoint",
    ):
        self.component = component
        self._lock = threading.RLock()
        self._status = initial_status
        self._message = initial_message
        self._resource: Any = None
        self._thread: threading.Thread | None = None

    def start_install(
        self,
        install,
        *,
        installing_message: str = "materializing artifacts",
        ready_message: str = "runtime ready",
        thread_name: str = "ndnsf-di-artifact-install",
        start_marker: str = "NDNSF_DI_ARTIFACT_INSTALL_STARTED",
        fail_marker: str = "NDNSF_DI_ARTIFACT_INSTALL_FAILED",
    ) -> None:
        """Run ``install`` in a background thread and expose readiness via ACK.

        ``install`` may return a managed runtime object. If that object has a
        ``stop`` method, ``stop()`` will call it during provider shutdown.
        """

        with self._lock:
            self._status = "installing"
            self._message = installing_message

        def run() -> None:
            try:
                resource = install()
                with self._lock:
                    self._resource = resource
                    self._status = "ready"
                    self._message = ready_message
            except Exception as exc:  # noqa: BLE001
                with self._lock:
                    self._status = "failed"
                    self._message = str(exc)
                print(
                    fail_marker,
                    f"component={self.component}",
                    f"reason={exc}",
                    flush=True,
                )

        self._thread = threading.Thread(
            target=run,
            name=thread_name,
            daemon=True,
        )
        print(start_marker, f"component={self.component}", flush=True)
        self._thread.start()

    def mark_ready(self, message: str = "runtime ready", *, resource: Any = None) -> None:
        with self._lock:
            self._resource = resource
            self._status = "ready"
            self._message = message

    def mark_failed(self, message: str) -> None:
        with self._lock:
            self._status = "failed"
            self._message = message

    def wait_ready(self, timeout_s: float) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.ready:
                return True
            if self.failed:
                return False
            time.sleep(0.1)
        return self.ready

    @property
    def ready(self) -> bool:
        with self._lock:
            return self._status == "ready"

    @property
    def failed(self) -> bool:
        with self._lock:
            return self._status == "failed"

    @property
    def status(self) -> str:
        with self._lock:
            return self._status

    @property
    def message(self) -> str:
        with self._lock:
            return self._message

    @property
    def resource(self) -> Any:
        with self._lock:
            return self._resource

    def ack(self) -> AckDecision:
        with self._lock:
            status = self._status
            message = self._message
        reason = (
            NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE
            if status == "installing"
            else NEGATIVE_ACK_REASON_INTERNAL_ERROR
            if status == "failed"
            else ""
        )
        state = (
            ServiceOperationState.DONE
            if status == "ready"
            else ServiceOperationState.RUNNING
            if status == "installing"
            else ServiceOperationState.FAILED
        )
        fields: dict[str, Any] = {
            "runtimeStatus": status,
            "component": self.component,
            "runtimeMessage": message,
            "operationStatus": ServiceOperationStatus(
                operation_id=f"artifact:{self.component}",
                operation="ARTIFACT_PROVISION",
                state=state,
                reason_code=reason,
                message=message,
                progress=1.0 if state == ServiceOperationState.DONE else 0.0,
                metadata={
                    "component": self.component,
                },
            ),
        }
        if reason:
            fields["negativeAckReason"] = reason
        return AckDecision(
            status=status == "ready",
            message=(f"{self.component} ready: {message}" if status == "ready" else reason),
            payload=encode_ack_metadata(fields),
        )

    def require_ready(self) -> None:
        with self._lock:
            status = self._status
            message = self._message
        if status != "ready":
            raise RuntimeError(f"{self.component} is {status}: {message}")

    def stop(self) -> None:
        with self._lock:
            resource = self._resource
            self._resource = None
        stop = getattr(resource, "stop", None)
        if callable(stop):
            stop()


def load_artifact_references(path_or_mapping: str | Path | dict | None) -> dict:
    if path_or_mapping is None:
        return {}
    if isinstance(path_or_mapping, dict):
        return dict(path_or_mapping)
    return json.loads(Path(path_or_mapping).read_text(encoding="utf-8"))


def role_artifact_entries(references: dict, role: str) -> dict[str, dict]:
    roles = references.get("roles", references)
    entry = roles.get(role, {})
    if not isinstance(entry, dict):
        raise ValueError(f"artifact references for role {role} must be a mapping")
    return {
        str(slot): dict(value)
        for slot, value in entry.items()
        if isinstance(value, dict)
    }


def artifact_references_need_repo_client(
    references: str | Path | dict,
    role: str,
) -> bool:
    """Return true when role artifacts need repo control-plane access.

    Local payloads and repo manifests that already contain segment Data names
    can be materialized directly. Entries without local bytes or concrete
    segment/replica locations need a live repo client for lookup/fetch.
    """

    entries = role_artifact_entries(load_artifact_references(references), role)
    if not entries:
        return False
    for entry in entries.values():
        manifest = entry.get("repoManifest", entry.get("manifest", {}))
        if not isinstance(manifest, dict):
            manifest = {}
        metadata = entry.get("metadata", {})
        if not isinstance(metadata, dict):
            metadata = {}
        if not (
            entry.get("localPayloadPath") or
            entry.get("payloadPath") or
            metadata.get("localPayloadPath") or
            manifest.get("segmentLocations") or
            manifest.get("replicaDataNames")
        ):
            return True
    return False


def materialize_role_artifacts(
    references: str | Path | dict,
    role: str,
    cache_dir: str | Path,
    *,
    repo_client=None,
) -> dict[str, MaterializedArtifact]:
    """Fetch and cache every artifact entry for ``role``.

    ``repo_client`` should implement ``fetch_object(object_name, manifest)``.
    For local smoke tests, an entry may also carry ``localPayloadPath`` or
    ``payloadPath``. The bytes are always verified against the repo manifest
    hash and size before being written to the cache.
    """

    loaded = load_artifact_references(references)
    entries = role_artifact_entries(loaded, role)
    root = Path(cache_dir)
    root.mkdir(parents=True, exist_ok=True)
    result: dict[str, MaterializedArtifact] = {}
    for slot, entry in entries.items():
        manifest_dict = repo_manifest_from_large_data_reference(entry)
        manifest = RepoObjectManifest.from_dict(manifest_dict)
        payload = _fetch_payload(entry, manifest, repo_client)
        _verify_payload(payload, manifest)
        target = _artifact_target(root, role, slot, entry, manifest)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes() != payload:
            target.write_bytes(payload)
        executable = _boolish(entry.get("executable", False), False)
        if executable:
            target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        result[slot] = MaterializedArtifact(
            role=role,
            slot=slot,
            path=target,
            manifest=manifest,
            executable=executable,
            metadata=dict(entry.get("metadata", {}))
            if isinstance(entry.get("metadata", {}), dict) else {},
        )
    return result


def materialized_path(artifacts: dict[str, MaterializedArtifact], *slots: str) -> Path:
    for slot in slots:
        artifact = artifacts.get(slot)
        if artifact is not None:
            return artifact.path
    raise KeyError(f"none of the artifact slots are available: {', '.join(slots)}")


def _fetch_payload(entry: dict, manifest: RepoObjectManifest, repo_client) -> bytes:
    local_path = str(
        entry.get("localPayloadPath") or
        entry.get("payloadPath") or
        manifest.metadata.get("localPayloadPath", "")
    )
    if local_path:
        return Path(local_path).read_bytes()
    if repo_client is None:
        if manifest.segment_locations or manifest.replica_data_names:
            return _ManifestOnlyRepoClient().fetch_object(
                manifest.object_name,
                manifest,
            )
        raise ValueError(
            f"artifact {manifest.object_name} requires a repo_client, "
            "repo segment locations, replica data names, or localPayloadPath"
        )
    return repo_client.fetch_object(manifest.object_name, manifest)


def _verify_payload(payload: bytes, manifest: RepoObjectManifest) -> None:
    if len(payload) != manifest.size:
        raise ValueError(
            f"artifact size mismatch for {manifest.object_name}: "
            f"expected {manifest.size}, got {len(payload)}"
        )
    actual = hashlib.sha256(payload).hexdigest()
    if actual != manifest.sha256:
        raise ValueError(
            f"artifact sha256 mismatch for {manifest.object_name}: "
            f"expected {manifest.sha256}, got {actual}"
        )


def _artifact_target(
    root: Path,
    role: str,
    slot: str,
    entry: dict,
    manifest: RepoObjectManifest,
) -> Path:
    filename = str(
        entry.get("filename") or
        manifest.metadata.get("filename", "") or
        Path(manifest.object_name).name or
        f"{slot}.bin"
    )
    digest = manifest.sha256[:16]
    safe_role = role.strip("/").replace("/", "-") or "role"
    safe_slot = slot.strip("/").replace("/", "-") or "artifact"
    return root / safe_role / safe_slot / digest / filename


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


def default_large_data_interest_lifetime_ms() -> int:
    return max(
        50,
        int(os.environ.get("NDNSF_LARGE_DATA_INTEREST_LIFETIME_MS", "10000")),
    )


@dataclass(frozen=True)
class ExecutionArtifact:
    name: str
    data_name: str
    filename: str
    sha256: str
    kind: str = "model"
    chunks: list[str] | None = None
    executable: bool = False
    cache_name: str = ""
    repo_manifest: dict | None = None
    large_data_reference: dict | None = None


@dataclass(frozen=True)
class ExecutionArtifactSpec:
    role: str
    backend: str
    entrypoint: str = ""
    artifacts: list[ExecutionArtifact] | None = None
    metadata: dict | None = None

    def to_bytes(self) -> bytes:
        return json.dumps(
            {
                "role": self.role,
                "backend": self.backend,
                "entrypoint": self.entrypoint,
                "artifacts": [
                    {
                        "name": artifact.name,
                        "dataName": artifact.data_name,
                        "filename": artifact.filename,
                        "sha256": artifact.sha256,
                        "kind": artifact.kind,
                        "chunks": list(artifact.chunks or []),
                        "executable": bool(artifact.executable),
                        "cacheName": artifact.cache_name,
                        "repoManifest": dict(artifact.repo_manifest or {}),
                        "largeDataReference": dict(artifact.large_data_reference or {}),
                    }
                    for artifact in (self.artifacts or [])
                ],
                "metadata": dict(self.metadata or {}),
            },
            sort_keys=True,
        ).encode()

    @staticmethod
    def from_bytes(payload: bytes) -> "ExecutionArtifactSpec":
        obj = json.loads(payload.decode())
        return ExecutionArtifactSpec(
            role=str(obj["role"]),
            backend=str(obj["backend"]),
            entrypoint=str(obj.get("entrypoint", "")),
            artifacts=[
                ExecutionArtifact(
                    name=str(item["name"]),
                    data_name=str(item["dataName"]),
                    filename=str(item["filename"]),
                    sha256=str(item["sha256"]),
                    kind=str(item.get("kind", "model")),
                    chunks=[str(value) for value in item.get("chunks", [])],
                    executable=bool(item.get("executable", False)),
                    cache_name=str(item.get("cacheName", "")),
                    repo_manifest=dict(item.get("repoManifest", {})),
                    large_data_reference=dict(item.get("largeDataReference", {})),
                )
                for item in obj.get("artifacts", [])
            ],
            metadata=dict(obj.get("metadata", {})),
        )


@dataclass(frozen=True)
class ExecutionContext:
    spec: ExecutionArtifactSpec
    artifact_paths: dict[str, Path]
    work_dir: Path
    runtime_evidence: "RuntimePreparationEvidence | None" = None

    def path(self, artifact_name: str) -> Path:
        return self.artifact_paths[artifact_name]

    def executable(self, artifact_name: str) -> Path:
        path = self.path(artifact_name)
        artifact = next(
            (item for item in (self.spec.artifacts or []) if item.name == artifact_name),
            None,
        )
        if artifact is None or not artifact.executable:
            raise KeyError(f"artifact {artifact_name!r} is not declared executable")
        return path


def _require_residency_digest(value: str, field_name: str) -> str:
    value = str(value)
    if len(value) != 71 or not value.startswith("sha256:"):
        raise ValueError(f"{field_name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(
            f"{field_name} must be a canonical sha256 digest") from exc
    return value


def _residency_identity_digest(values: Mapping[str, Any]) -> str:
    wire = json.dumps(dict(values), sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")
    return "sha256:" + hashlib.sha256(wire).hexdigest()


@dataclass(frozen=True)
class CanonicalResidencyIdentity:
    """Placement-independent identity for one verified canonical object."""

    model_manifest_digest: str
    artifact_profile_digest: str
    graph_digest: str
    object_digest: str
    protection_epoch: str = "plaintext-v1"

    def __post_init__(self) -> None:
        for name in (
                "model_manifest_digest", "artifact_profile_digest",
                "graph_digest", "object_digest"):
            _require_residency_digest(getattr(self, name), name)
        if not self.protection_epoch:
            raise ValueError("canonical residency protection epoch is required")

    @property
    def digest(self) -> str:
        return _residency_identity_digest({
            "class": "CANONICAL", **self.__dict__,
        })

    @property
    def content_digest(self) -> str:
        return self.object_digest


@dataclass(frozen=True)
class AssembledFragmentIdentity:
    """Exact portable identity for one Provider-assembled ONNX fragment."""

    model_manifest_digest: str
    artifact_profile_digest: str
    graph_digest: str
    assembly_spec_digest: str
    artifact_digest: str
    adapter_descriptor_digest: str
    assembler_descriptor_digest: str
    backend_abi: str
    precision: str
    quantization: str
    layout: str
    padding: str
    protection_epoch: str = "plaintext-v1"

    def __post_init__(self) -> None:
        for name in (
                "model_manifest_digest", "artifact_profile_digest",
                "graph_digest", "assembly_spec_digest", "artifact_digest",
                "adapter_descriptor_digest", "assembler_descriptor_digest"):
            _require_residency_digest(getattr(self, name), name)
        if not all((self.backend_abi, self.precision, self.quantization,
                    self.layout, self.padding, self.protection_epoch)):
            raise ValueError("assembled fragment residency identity is incomplete")

    @property
    def digest(self) -> str:
        return _residency_identity_digest({
            "class": "ASSEMBLED_FRAGMENT", **self.__dict__,
        })

    @property
    def content_digest(self) -> str:
        return self.artifact_digest


@dataclass(frozen=True)
class LoadedRuntimeIdentity:
    """Exact live-runtime identity; equality is required for zero reload."""

    assembled_identity_digest: str
    artifact_digest: str
    backend: str
    runtime_version: str
    driver_abi: str
    kernel_profile_digest: str
    device_set: tuple[str, ...]
    topology_digest: str
    provider_boot_epoch: str
    process_epoch: str
    runtime_generation: int
    fencing_token: str
    protection_epoch: str = "plaintext-v1"
    reusable_state_contract_digest: str = ""

    def __post_init__(self) -> None:
        for name in (
                "assembled_identity_digest", "artifact_digest",
                "kernel_profile_digest", "topology_digest"):
            _require_residency_digest(getattr(self, name), name)
        if self.reusable_state_contract_digest:
            _require_residency_digest(
                self.reusable_state_contract_digest,
                "reusable_state_contract_digest")
        devices = tuple(str(item) for item in self.device_set)
        object.__setattr__(self, "device_set", devices)
        if (not all((self.backend, self.runtime_version, self.driver_abi,
                     self.provider_boot_epoch, self.process_epoch,
                     self.fencing_token, self.protection_epoch))
                or self.runtime_generation <= 0
                or len(set(devices)) != len(devices)
                or any(not item.startswith("cuda:") for item in devices)
                or (devices and self.backend.endswith("-cpu"))):
            raise ValueError("loaded runtime residency identity is incomplete")

    @property
    def digest(self) -> str:
        return _residency_identity_digest({
            "class": "LOADED_RUNTIME",
            **{**self.__dict__, "device_set": list(self.device_set)},
        })


@dataclass(frozen=True)
class ExactResidencyHit:
    identity_digest: str
    residency_class: str
    path: Path | None
    size: int
    resource: Any = field(compare=False, repr=False, default=None)


@dataclass
class _ExactResidencyEntry:
    identity: Any
    residency_class: str
    size: int
    path: Path | None = None
    resource: Any = None
    owners: set[str] = field(default_factory=set)
    last_used_sequence: int = 0


@dataclass(frozen=True)
class ProviderResidencyHit:
    identity: ProviderResidencyIdentity
    tier: str
    path: Path | None
    resource: Any = field(compare=False, repr=False, default=None)


@dataclass
class _ProviderResidencyEntry:
    identity: ProviderResidencyIdentity
    size: int
    disk_path: Path | None = None
    ram_resource: Any = None
    ram_boot_epoch: str = ""
    gpu_resource: Any = None
    gpu_boot_epoch: str = ""
    gpu_device: str = ""
    owners: set[str] = field(default_factory=set)


class ProviderResidencyLedger:
    """Thread-safe, content-addressed disk/RAM/GPU residency authority.

    The ledger owns no NDNSF wire semantics.  It is an NDNSF-DI Provider
    facility that turns adapter and Repository facts into auditable reuse and
    invalidation decisions without copying payloads into request directories.
    """

    _COUNTERS = (
        "repoUniqueBytes", "repoWireBytes", "duplicatePayloadBytes",
        "ramLoadCount", "deviceLoadCount", "diskHitCount", "ramHitCount",
        "gpuHitCount", "evictionCount", "invalidationCount",
        "canonicalAdmitCount", "assembledBuildCount", "runtimeLoadCount",
        "canonicalHitCount", "assembledHitCount", "loadedHitCount",
        "assemblyBuiltBytes", "runtimeLoadedBytes",
    )

    def __init__(
        self, root: str | Path, *, provider_boot_epoch: str,
        max_canonical_entries: int = 128,
        max_assembled_entries: int = 64,
        max_loaded_entries: int = 16,
    ):
        if not provider_boot_epoch:
            raise ValueError("provider_boot_epoch is required")
        if min(max_canonical_entries, max_assembled_entries,
               max_loaded_entries) <= 0:
            raise ValueError("residency bounds must be positive")
        self.root = Path(root).expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.provider_boot_epoch = str(provider_boot_epoch)
        self._entries: dict[tuple[str, ...], _ProviderResidencyEntry] = {}
        self._exact_entries: dict[str, dict[str, _ExactResidencyEntry]] = {
            "CANONICAL": {}, "ASSEMBLED_FRAGMENT": {}, "LOADED_RUNTIME": {},
        }
        self._exact_bounds = {
            "CANONICAL": int(max_canonical_entries),
            "ASSEMBLED_FRAGMENT": int(max_assembled_entries),
            "LOADED_RUNTIME": int(max_loaded_entries),
        }
        self._sequence = 0
        self._counters = {name: 0 for name in self._COUNTERS}
        self._lock = threading.RLock()

    def admit_canonical(
        self, identity: CanonicalResidencyIdentity, path: str | Path, *,
        size: int, unique_bytes: int = 0, wire_bytes: int = 0,
    ) -> ExactResidencyHit:
        hit, created = self._admit_exact_file(
            "CANONICAL", identity, path, size=size)
        with self._lock:
            if created:
                self._counters["canonicalAdmitCount"] += 1
                self._counters["repoUniqueBytes"] += int(unique_bytes)
                self._counters["repoWireBytes"] += int(wire_bytes)
        return hit

    def admit_assembled(
        self, identity: AssembledFragmentIdentity, path: str | Path, *,
        size: int, built_bytes: int,
    ) -> ExactResidencyHit:
        hit, created = self._admit_exact_file(
            "ASSEMBLED_FRAGMENT", identity, path, size=size)
        with self._lock:
            if created:
                self._counters["assembledBuildCount"] += 1
                self._counters["assemblyBuiltBytes"] += int(built_bytes)
        return hit

    def admit_loaded(
        self, identity: LoadedRuntimeIdentity, resource: Any, *,
        bytes_loaded: int, load_completed: bool, warmup_completed: bool,
        cpu_fallback_count: int,
    ) -> ExactResidencyHit:
        if (resource is None or int(bytes_loaded) <= 0 or not load_completed
                or not warmup_completed or int(cpu_fallback_count) != 0):
            raise ValueError(
                "loaded residency requires verified load/warmup and no fallback")
        with self._lock:
            if identity.provider_boot_epoch != self.provider_boot_epoch:
                raise ValueError("loaded residency uses a stale Provider boot epoch")
            fragments = self._exact_entries["ASSEMBLED_FRAGMENT"]
            fragment = fragments.get(identity.assembled_identity_digest)
            if (fragment is None
                    or fragment.identity.artifact_digest
                    != identity.artifact_digest):
                raise RuntimeError(
                    "exact assembled residency is required before runtime load")
            self._sequence += 1
            entry = _ExactResidencyEntry(
                identity=identity, residency_class="LOADED_RUNTIME",
                size=int(bytes_loaded), path=fragment.path, resource=resource,
                last_used_sequence=self._sequence)
            actual, created = self._insert_exact(entry)
            if created:
                self._counters["runtimeLoadCount"] += 1
                self._counters["runtimeLoadedBytes"] += int(bytes_loaded)
            return self._exact_hit(actual)

    def lookup_canonical(
        self, identity: CanonicalResidencyIdentity,
    ) -> ExactResidencyHit | None:
        return self._lookup_exact("CANONICAL", identity.digest)

    def lookup_assembled(
        self, identity: AssembledFragmentIdentity,
    ) -> ExactResidencyHit | None:
        return self._lookup_exact("ASSEMBLED_FRAGMENT", identity.digest)

    def lookup_loaded(
        self, identity: LoadedRuntimeIdentity,
    ) -> ExactResidencyHit | None:
        if identity.provider_boot_epoch != self.provider_boot_epoch:
            return None
        return self._lookup_exact("LOADED_RUNTIME", identity.digest)

    def acquire_exact(
        self, identity: Any, *, owner: str,
    ) -> ExactResidencyHit:
        if not owner:
            raise ValueError("residency owner is required")
        if isinstance(identity, CanonicalResidencyIdentity):
            kind, digest = "CANONICAL", identity.digest
        elif isinstance(identity, AssembledFragmentIdentity):
            kind, digest = "ASSEMBLED_FRAGMENT", identity.digest
        elif isinstance(identity, LoadedRuntimeIdentity):
            kind, digest = "LOADED_RUNTIME", identity.digest
        else:
            raise TypeError("unsupported exact residency identity")
        with self._lock:
            hit = self._lookup_exact(kind, digest)
            if hit is None:
                raise KeyError("exact compatible residency is unavailable")
            self._exact_entries[kind][digest].owners.add(str(owner))
            self._counters[{
                "CANONICAL": "canonicalHitCount",
                "ASSEMBLED_FRAGMENT": "assembledHitCount",
                "LOADED_RUNTIME": "loadedHitCount",
            }[kind]] += 1
            return hit

    def make_residency_proof_v3(
        self, identity: Any, *, role: str, rank: int,
        process_epoch: str, topology_digest: str,
        captured_at_ms: int, expires_at_ms: int,
        backend: str = "", missing_verified_bytes: int = 0,
        estimated_assembly_ms: float = 0.0,
        estimated_load_ms: float = 0.0,
    ):
        """Project one exact ledger entry into a bounded signed-offer view."""

        from .sdk.placement import (
            ResidencyClassV3, ResidencyProofV3, ResidencyTierV3,
        )
        if not role or rank < 0 or not process_epoch or not topology_digest:
            raise ValueError("residency proof projection context is incomplete")
        if self.lookup_canonical(identity) is not None:
            return ResidencyProofV3(
                artifact_digest=identity.object_digest, role=role, rank=rank,
                tier=ResidencyTierV3.CANONICAL,
                boot_epoch=self.provider_boot_epoch,
                process_epoch=process_epoch, topology_digest=topology_digest,
                captured_at_ms=captured_at_ms, expires_at_ms=expires_at_ms,
                residency_class=ResidencyClassV3.CANONICAL,
                identity_digest=identity.digest,
                model_manifest_digest=identity.model_manifest_digest,
                artifact_profile_digest=identity.artifact_profile_digest,
                graph_digest=identity.graph_digest, backend=backend,
                protection_epoch=identity.protection_epoch,
                missing_verified_bytes=missing_verified_bytes,
                estimated_assembly_ms=estimated_assembly_ms,
                estimated_load_ms=estimated_load_ms)
        if self.lookup_assembled(identity) is not None:
            if not backend:
                raise ValueError("assembled residency proof requires backend")
            return ResidencyProofV3(
                artifact_digest=identity.artifact_digest, role=role, rank=rank,
                tier=ResidencyTierV3.DISK,
                boot_epoch=self.provider_boot_epoch,
                process_epoch=process_epoch, topology_digest=topology_digest,
                captured_at_ms=captured_at_ms, expires_at_ms=expires_at_ms,
                residency_class=ResidencyClassV3.ASSEMBLED_FRAGMENT,
                identity_digest=identity.digest,
                assembly_spec_digest=identity.assembly_spec_digest,
                model_manifest_digest=identity.model_manifest_digest,
                artifact_profile_digest=identity.artifact_profile_digest,
                graph_digest=identity.graph_digest, backend=backend,
                protection_epoch=identity.protection_epoch,
                estimated_load_ms=estimated_load_ms)
        if self.lookup_loaded(identity) is not None:
            with self._lock:
                fragment = self._exact_entries["ASSEMBLED_FRAGMENT"].get(
                    identity.assembled_identity_digest)
            if fragment is None:
                raise RuntimeError("loaded proof lost its assembled identity")
            assembled = fragment.identity
            return ResidencyProofV3(
                artifact_digest=identity.artifact_digest, role=role, rank=rank,
                tier=(ResidencyTierV3.GPU if identity.device_set
                      else ResidencyTierV3.RAM),
                device_set=identity.device_set,
                boot_epoch=identity.provider_boot_epoch,
                process_epoch=identity.process_epoch,
                topology_digest=identity.topology_digest,
                captured_at_ms=captured_at_ms, expires_at_ms=expires_at_ms,
                residency_class=ResidencyClassV3.LOADED_RUNTIME,
                identity_digest=identity.digest,
                assembly_spec_digest=assembled.assembly_spec_digest,
                model_manifest_digest=assembled.model_manifest_digest,
                artifact_profile_digest=assembled.artifact_profile_digest,
                graph_digest=assembled.graph_digest, backend=identity.backend,
                protection_epoch=identity.protection_epoch,
                runtime_generation=identity.runtime_generation,
                fencing_token=identity.fencing_token)
        raise KeyError("exact residency is unavailable for proof projection")

    def release_exact(self, identity: Any, *, owner: str) -> None:
        kind = ("CANONICAL" if isinstance(identity, CanonicalResidencyIdentity)
                else "ASSEMBLED_FRAGMENT"
                if isinstance(identity, AssembledFragmentIdentity)
                else "LOADED_RUNTIME"
                if isinstance(identity, LoadedRuntimeIdentity) else "")
        if not kind:
            raise TypeError("unsupported exact residency identity")
        with self._lock:
            entry = self._exact_entries[kind].get(identity.digest)
            if entry is not None:
                entry.owners.discard(str(owner))

    def invalidate_exact_context(
        self, *, provider_boot_epoch: str, process_epoch: str,
        topology_digest: str, fencing_token: str, protection_epoch: str,
        runtime_generation: int,
    ) -> tuple[str, ...]:
        """Invalidate identities that cannot survive the current runtime fence."""

        if (not all((provider_boot_epoch, process_epoch, topology_digest,
                     fencing_token, protection_epoch))
                or int(runtime_generation) <= 0):
            raise ValueError("exact residency invalidation context is incomplete")
        removed: list[str] = []
        with self._lock:
            invalid_entries: list[tuple[str, str, _ExactResidencyEntry]] = []
            for kind, entries in self._exact_entries.items():
                for digest, entry in tuple(entries.items()):
                    identity = entry.identity
                    invalid = identity.protection_epoch != protection_epoch
                    if kind == "LOADED_RUNTIME":
                        invalid = invalid or any((
                            identity.provider_boot_epoch != provider_boot_epoch,
                            identity.process_epoch != process_epoch,
                            identity.topology_digest != topology_digest,
                            identity.fencing_token != fencing_token,
                            identity.runtime_generation
                            != int(runtime_generation),
                        ))
                    if invalid:
                        invalid_entries.append((kind, digest, entry))
            if any(entry.owners for _, _, entry in invalid_entries):
                raise RuntimeError(
                    "cannot invalidate residency owned by an active request")
            self.provider_boot_epoch = str(provider_boot_epoch)
            for kind, digest, _entry in invalid_entries:
                self._exact_entries[kind].pop(digest)
                removed.append(digest)
            if removed:
                self._counters["invalidationCount"] += len(removed)
        return tuple(sorted(removed))

    def evict_exact(self, identity: Any) -> None:
        kind = ("CANONICAL" if isinstance(identity, CanonicalResidencyIdentity)
                else "ASSEMBLED_FRAGMENT"
                if isinstance(identity, AssembledFragmentIdentity)
                else "LOADED_RUNTIME"
                if isinstance(identity, LoadedRuntimeIdentity) else "")
        if not kind:
            raise TypeError("unsupported exact residency identity")
        with self._lock:
            self._evict_exact_digest(kind, identity.digest)

    def content_path(
        self, identity: ProviderResidencyIdentity, filename: str,
    ) -> Path:
        name = Path(str(filename)).name
        if not name or name in {".", ".."}:
            raise ValueError("content-addressed filename is invalid")
        return self.root / "sha256" / identity.artifact_digest[7:] / name

    @staticmethod
    def _digest_file(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as source:
            while chunk := source.read(4 << 20):
                digest.update(chunk)
        return "sha256:" + digest.hexdigest()

    def admit_disk(
        self, identity: ProviderResidencyIdentity, path: str | Path, *,
        size: int, unique_bytes: int = 0, wire_bytes: int = 0,
    ) -> ProviderResidencyHit:
        target = Path(path).expanduser().resolve()
        if not target.is_file() or target.stat().st_size != int(size):
            raise ValueError("disk residency size does not match verified artifact")
        if self._digest_file(target) != identity.artifact_digest:
            raise ValueError("disk residency digest does not match verified artifact")
        expected_parent = (self.root / "sha256" / identity.artifact_digest[7:]).resolve()
        if target.parent != expected_parent:
            raise ValueError("disk residency must use the content-addressed root")
        with self._lock:
            entry = self._entries.get(identity.durable_key)
            if entry is None:
                entry = _ProviderResidencyEntry(identity=identity, size=int(size))
                self._entries[identity.durable_key] = entry
            elif entry.size != int(size):
                raise ValueError("content-addressed residency size changed")
            entry.disk_path = target
            self._counters["repoUniqueBytes"] += int(unique_bytes)
            self._counters["repoWireBytes"] += int(wire_bytes)
            return ProviderResidencyHit(identity, "DISK", target)

    def create_view(
        self, identity: ProviderResidencyIdentity, view_path: str | Path,
    ) -> Path:
        with self._lock:
            entry = self._entries.get(identity.durable_key)
            if entry is None or entry.disk_path is None:
                raise KeyError("artifact has no verified disk residency")
            target = entry.disk_path
        view = Path(view_path).expanduser()
        view.parent.mkdir(parents=True, exist_ok=True)
        if view.is_symlink() and view.resolve() == target.resolve():
            return view
        if view.exists() or view.is_symlink():
            raise FileExistsError(f"residency view already exists: {view}")
        view.symlink_to(target)
        return view

    def promote_ram(
        self, identity: ProviderResidencyIdentity, resource: Any, *,
        bytes_loaded: int,
    ) -> ProviderResidencyHit:
        if resource is None or int(bytes_loaded) <= 0:
            raise ValueError("RAM promotion requires a loaded resource")
        with self._lock:
            self._require_current_boot(identity)
            entry = self._require_disk(identity)
            entry.ram_resource = resource
            entry.ram_boot_epoch = self.provider_boot_epoch
            self._counters["ramLoadCount"] += 1
            return ProviderResidencyHit(identity, "RAM", entry.disk_path, resource)

    def promote_gpu(
        self, identity: ProviderResidencyIdentity, resource: Any, *,
        bytes_loaded: int, load_completed: bool, warmup_completed: bool,
        cpu_fallback_count: int,
    ) -> ProviderResidencyHit:
        if (resource is None or int(bytes_loaded) <= 0 or not load_completed
                or not warmup_completed or int(cpu_fallback_count) != 0):
            raise ValueError(
                "GPU promotion requires load, warmup, bytes, and zero CPU fallback")
        if (not identity.device.startswith("cuda:")
                or not identity.device[5:].isdigit()):
            raise ValueError("GPU promotion requires one exact CUDA device")
        with self._lock:
            self._require_current_boot(identity)
            entry = self._require_disk(identity)
            entry.gpu_resource = resource
            entry.gpu_boot_epoch = self.provider_boot_epoch
            entry.gpu_device = identity.device
            self._counters["deviceLoadCount"] += 1
            return ProviderResidencyHit(identity, "GPU", entry.disk_path, resource)

    def lookup(
        self, identity: ProviderResidencyIdentity,
    ) -> ProviderResidencyHit | None:
        with self._lock:
            entry = self._entries.get(identity.durable_key)
            if entry is None or entry.disk_path is None or not entry.disk_path.is_file():
                return None
            if (identity.provider_boot_epoch == self.provider_boot_epoch
                    and entry.gpu_resource is not None
                    and entry.gpu_boot_epoch == self.provider_boot_epoch
                    and entry.gpu_device == identity.device):
                return ProviderResidencyHit(
                    identity, "GPU", entry.disk_path, entry.gpu_resource)
            if (identity.provider_boot_epoch == self.provider_boot_epoch
                    and entry.ram_resource is not None
                    and entry.ram_boot_epoch == self.provider_boot_epoch):
                return ProviderResidencyHit(
                    identity, "RAM", entry.disk_path, entry.ram_resource)
            return ProviderResidencyHit(identity, "DISK", entry.disk_path)

    def acquire(
        self, identity: ProviderResidencyIdentity, *, owner: str,
    ) -> ProviderResidencyHit:
        if not owner:
            raise ValueError("residency owner is required")
        with self._lock:
            hit = self.lookup(identity)
            if hit is None:
                raise KeyError("compatible residency is unavailable")
            self._entries[identity.durable_key].owners.add(str(owner))
            self._counters[hit.tier.lower() + "HitCount"] += 1
            return hit

    def release(self, identity: ProviderResidencyIdentity, *, owner: str) -> None:
        with self._lock:
            entry = self._entries.get(identity.durable_key)
            if entry is not None:
                entry.owners.discard(str(owner))

    def evict(
        self, identity: ProviderResidencyIdentity, *, tier: str,
    ) -> None:
        tier = str(tier).upper()
        with self._lock:
            entry = self._entries.get(identity.durable_key)
            if entry is None:
                return
            if entry.owners:
                raise RuntimeError("residency is owned by active requests")
            if tier == "GPU":
                entry.gpu_resource = None
                entry.gpu_boot_epoch = ""
                entry.gpu_device = ""
            elif tier == "RAM":
                entry.ram_resource = None
                entry.ram_boot_epoch = ""
            elif tier == "DISK":
                raise ValueError("durable disk eviction requires an explicit retention policy")
            else:
                raise ValueError("unknown residency tier")
            self._counters["evictionCount"] += 1

    def rebind_boot_epoch(self, provider_boot_epoch: str) -> None:
        if not provider_boot_epoch:
            raise ValueError("provider_boot_epoch is required")
        with self._lock:
            if provider_boot_epoch == self.provider_boot_epoch:
                return
            loaded = self._exact_entries["LOADED_RUNTIME"]
            if any(entry.owners for entry in loaded.values()):
                raise RuntimeError(
                    "cannot rebind boot epoch with active loaded residency")
            self.provider_boot_epoch = str(provider_boot_epoch)
            loaded.clear()
            for entry in self._entries.values():
                entry.ram_resource = None
                entry.ram_boot_epoch = ""
                entry.gpu_resource = None
                entry.gpu_boot_epoch = ""
                entry.gpu_device = ""
                entry.owners.clear()
            self._counters["invalidationCount"] += 1

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            records = []
            for entry in self._entries.values():
                records.append({
                    "modelContentDigest": entry.identity.model_content_digest,
                    "graphDigest": entry.identity.graph_digest,
                    "partitionDigest": entry.identity.partition_digest,
                    "artifactDigest": entry.identity.artifact_digest,
                    "adapterId": entry.identity.adapter_id,
                    "adapterVersion": entry.identity.adapter_version,
                    "backend": entry.identity.backend,
                    "diskPath": str(entry.disk_path or ""),
                    "ramBootEpoch": entry.ram_boot_epoch,
                    "gpuBootEpoch": entry.gpu_boot_epoch,
                    "gpuDevice": entry.gpu_device,
                    "owners": sorted(entry.owners),
                })
            return {
                "schema": "ndnsf-di.provider-residency.v1",
                "providerBootEpoch": self.provider_boot_epoch,
                "records": records,
                "exactResidency": {
                    kind: [self._exact_record(entries[digest])
                           for digest in sorted(entries)]
                    for kind, entries in self._exact_entries.items()
                },
                "counters": dict(self._counters),
            }

    def _admit_exact_file(
        self, kind: str, identity: Any, path: str | Path, *, size: int,
    ) -> tuple[ExactResidencyHit, bool]:
        target = Path(path).expanduser().resolve()
        if (int(size) <= 0 or not target.is_file()
                or target.stat().st_size != int(size)):
            raise ValueError("exact residency file size is invalid")
        try:
            target.relative_to(self.root)
        except ValueError as exc:
            raise ValueError("exact residency path is outside the cache root") from exc
        if self._digest_file(target) != identity.content_digest:
            raise ValueError("exact residency content digest mismatch")
        with self._lock:
            self._sequence += 1
            entry = _ExactResidencyEntry(
                identity=identity, residency_class=kind, size=int(size),
                path=target, last_used_sequence=self._sequence)
            actual, created = self._insert_exact(entry)
            return self._exact_hit(actual), created

    def _insert_exact(
        self, entry: _ExactResidencyEntry,
    ) -> tuple[_ExactResidencyEntry, bool]:
        entries = self._exact_entries[entry.residency_class]
        digest = entry.identity.digest
        existing = entries.get(digest)
        if existing is not None:
            if (existing.identity != entry.identity or existing.size != entry.size
                    or existing.path != entry.path):
                raise ValueError("exact residency identity conflict")
            if (entry.resource is not None and existing.resource is not None
                    and existing.resource is not entry.resource):
                raise ValueError("exact loaded runtime identity conflict")
            existing.last_used_sequence = self._sequence
            if entry.resource is not None:
                existing.resource = entry.resource
            return existing, False
        entries[digest] = entry
        try:
            while len(entries) > self._exact_bounds[entry.residency_class]:
                candidates = [item for item in entries.values()
                              if not item.owners and item is not entry]
                if not candidates:
                    raise RuntimeError(
                        "exact residency bound is full of active entries")
                victim = min(candidates,
                             key=lambda item: item.last_used_sequence)
                self._evict_exact_digest(
                    entry.residency_class, victim.identity.digest)
        except Exception:
            entries.pop(digest, None)
            raise
        return entry, True

    def _lookup_exact(
        self, kind: str, identity_digest: str,
    ) -> ExactResidencyHit | None:
        with self._lock:
            entry = self._exact_entries[kind].get(identity_digest)
            if entry is None:
                return None
            if entry.path is not None and not entry.path.is_file():
                self._exact_entries[kind].pop(identity_digest, None)
                self._counters["invalidationCount"] += 1
                return None
            self._sequence += 1
            entry.last_used_sequence = self._sequence
            return self._exact_hit(entry)

    @staticmethod
    def _exact_hit(entry: _ExactResidencyEntry) -> ExactResidencyHit:
        return ExactResidencyHit(
            identity_digest=entry.identity.digest,
            residency_class=entry.residency_class,
            path=entry.path, size=entry.size, resource=entry.resource)

    def _evict_exact_digest(self, kind: str, identity_digest: str) -> None:
        entry = self._exact_entries[kind].get(identity_digest)
        if entry is None:
            return
        if entry.owners:
            raise RuntimeError("residency is owned by active requests")
        self._exact_entries[kind].pop(identity_digest)
        self._counters["evictionCount"] += 1

    @staticmethod
    def _exact_record(entry: _ExactResidencyEntry) -> dict[str, Any]:
        identity = {
            str(name): (list(value) if isinstance(value, tuple) else value)
            for name, value in entry.identity.__dict__.items()
        }
        return {
            "identityDigest": entry.identity.digest,
            "residencyClass": entry.residency_class,
            "identity": identity,
            "size": entry.size,
            "path": str(entry.path or ""),
            "owners": sorted(entry.owners),
            "lastUsedSequence": entry.last_used_sequence,
        }

    def _require_current_boot(self, identity: ProviderResidencyIdentity) -> None:
        if identity.provider_boot_epoch != self.provider_boot_epoch:
            raise ValueError("residency identity uses a stale provider boot epoch")

    def _require_disk(
        self, identity: ProviderResidencyIdentity,
    ) -> _ProviderResidencyEntry:
        entry = self._entries.get(identity.durable_key)
        if entry is None or entry.disk_path is None or not entry.disk_path.is_file():
            raise RuntimeError("verified disk residency is required before promotion")
        return entry


@dataclass(frozen=True)
class RuntimePreparationEvidence:
    """Adapter-issued proof that one assigned shard is runtime-ready.

    File materialization is not runtime readiness.  This contract is created
    only after the model adapter has loaded the verified shard and completed a
    warmup on the exact assigned device. ``CPU_LOGIC`` is an explicit backend
    class and cannot satisfy a CUDA acceptance gate.
    """

    adapter_id: str
    adapter_version: str
    backend: str
    device: str
    artifact_digests: tuple[str, ...]
    load_completed: bool
    warmup_completed: bool
    cpu_fallback_count: int
    prepared_at_ms: int
    device_class: str = "CUDA"

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "artifact_digests", tuple(self.artifact_digests))
        if not self.adapter_id or not self.adapter_version or not self.backend:
            raise ValueError("runtime adapter identity is incomplete")
        device = str(self.device)
        if self.device_class not in {"CUDA", "CPU_LOGIC"}:
            raise ValueError("runtime preparation device class is invalid")
        if self.device_class == "CUDA" and (
                not device.startswith("cuda:") or not device[5:].isdigit()):
            raise ValueError("CUDA runtime preparation requires an exact device")
        if self.device_class == "CPU_LOGIC" and (
                device != "cpu" or not self.backend.endswith("-cpu")):
            raise ValueError(
                "CPU logic preparation requires cpu and an explicit -cpu backend")
        if not self.load_completed or not self.warmup_completed:
            raise ValueError("runtime preparation requires load and warmup")
        if self.cpu_fallback_count != 0:
            raise ValueError("runtime preparation rejects CPU fallback")
        if self.prepared_at_ms <= 0 or not self.artifact_digests:
            raise ValueError("runtime preparation evidence is incomplete")
        for digest in self.artifact_digests:
            if (not isinstance(digest, str) or len(digest) != 71
                    or not digest.startswith("sha256:")):
                raise ValueError("runtime artifact digest is not canonical")
            try:
                int(digest[7:], 16)
            except ValueError as exc:
                raise ValueError(
                    "runtime artifact digest is not canonical") from exc

    def validate_assignment(
        self, *, adapter_id: str, adapter_version: str, backend: str,
        device: str, artifact_digest: str,
    ) -> None:
        mismatches = []
        if self.adapter_id != adapter_id:
            mismatches.append("adapter_id")
        if self.adapter_version != adapter_version:
            mismatches.append("adapter_version")
        if self.backend != backend:
            mismatches.append("backend")
        if self.device != device:
            mismatches.append("device")
        if artifact_digest not in self.artifact_digests:
            mismatches.append("artifact_digest")
        if mismatches:
            raise ValueError(
                "runtime preparation/assignment mismatch: "
                + ",".join(mismatches))


def prepare_runtime(
    execution: ExecutionContext, *,
    preparer: Callable[
        [ExecutionContext, Callable[[str, float], None]],
        RuntimePreparationEvidence],
    expected_adapter_id: str,
    expected_adapter_version: str,
    expected_backend: str,
    expected_device: str,
    expected_artifact_digest: str,
    progress: Callable[[str, float], None] | None = None,
) -> ExecutionContext:
    """Run one adapter load/warmup and attach exact CUDA readiness proof."""
    if not callable(preparer):
        raise TypeError("runtime preparer must be callable")
    phases: list[str] = []
    last_progress = 0.0

    def report(phase: str, value: float) -> None:
        nonlocal last_progress
        phase = str(phase)
        value = float(value)
        if phase not in {"LOADING", "WARMING"}:
            raise ValueError("runtime preparer reported an invalid phase")
        if value < last_progress or not 0.0 <= value <= 1.0:
            raise ValueError("runtime preparation progress is not monotonic")
        last_progress = value
        phases.append(phase)
        if progress is not None:
            progress(phase, value)

    evidence = preparer(execution, report)
    if not isinstance(evidence, RuntimePreparationEvidence):
        raise TypeError("runtime preparer returned no readiness evidence")
    if "LOADING" not in phases or "WARMING" not in phases:
        raise ValueError("runtime preparer omitted load/warmup progress")
    if phases.index("LOADING") > phases.index("WARMING"):
        raise ValueError("runtime preparer reported warmup before load")
    evidence.validate_assignment(
        adapter_id=expected_adapter_id,
        adapter_version=expected_adapter_version,
        backend=expected_backend,
        device=expected_device,
        artifact_digest=expected_artifact_digest,
    )
    return replace(execution, runtime_evidence=evidence)


def build_output_object_manifest(
    *, payload: bytes, object_name: str, request_id: str, attempt: int,
    plan_digest: str, producer_role: str, producer_provider: str,
    producer_boot_epoch: str, generation: int,
    consumer_roles: tuple[str, ...], lineage_digests: tuple[str, ...],
    schema_digest: str, segment_count: int, aead_algorithm: str,
    key_grant_digest: str, signer_key_id: str, signature: str,
    captured_at_ms: int, expires_at_ms: int,
):
    """Build the complete authenticated identity carried with a DAG object."""

    from .core.execution import InputOutputObjectManifest

    wire = bytes(payload)
    return InputOutputObjectManifest(
        object_name=object_name,
        request_id=request_id,
        attempt=attempt,
        plan_digest=plan_digest,
        producer_role=producer_role,
        producer_provider=producer_provider,
        producer_boot_epoch=producer_boot_epoch,
        generation=generation,
        consumer_roles=tuple(consumer_roles),
        lineage_digests=tuple(lineage_digests),
        schema_digest=schema_digest,
        segment_count=segment_count,
        total_bytes=len(wire),
        payload_digest="sha256:" + hashlib.sha256(wire).hexdigest(),
        aead_algorithm=aead_algorithm,
        key_grant_digest=key_grant_digest,
        signer_key_id=signer_key_id,
        signature=signature,
        captured_at_ms=captured_at_ms,
        expires_at_ms=expires_at_ms,
    )


def publish_execution_artifact_spec(
    user: Any,
    service: str,
    *,
    role: str,
    backend: str,
    artifacts: dict,
    entrypoint: str = "",
    metadata: Optional[dict] = None,
    object_label_prefix: str = "execution",
    max_artifact_chunk_size: int = 512 * 1024,
    freshness_ms: int = 60000,
):
    refs: list[ExecutionArtifact] = []
    for name, value in artifacts.items():
        if isinstance(value, dict):
            repo_artifact = _artifact_from_repo_spec(name, value)
            if repo_artifact is not None:
                refs.append(repo_artifact)
                continue
        payload, filename, kind, executable, cache_name = _artifact_spec_parts(value)
        chunks: list[str] = []
        data_name = ""
        if max_artifact_chunk_size > 0 and len(payload) > max_artifact_chunk_size:
            for offset in range(0, len(payload), max_artifact_chunk_size):
                chunk_index = offset // max_artifact_chunk_size
                result = user.publish_encrypted_large_data(
                    service,
                    payload[offset : offset + max_artifact_chunk_size],
                    object_label=f"{object_label_prefix}-{role}-{name}-part{chunk_index:04d}",
                    freshness_ms=freshness_ms,
                )
                if not result.success:
                    return result
                chunks.append(result.encrypted_data_name)
        else:
            result = user.publish_encrypted_large_data(
                service,
                payload,
                object_label=f"{object_label_prefix}-{role}-{name}",
                freshness_ms=freshness_ms,
            )
            if not result.success:
                return result
            data_name = result.encrypted_data_name
        digest = hashlib.sha256(payload).hexdigest()
        refs.append(
            ExecutionArtifact(
                name=name,
                data_name=data_name,
                filename=filename,
                sha256=digest,
                kind=kind,
                chunks=chunks,
                executable=executable,
                cache_name=cache_name,
                large_data_reference=(
                    {
                        "source": "ndn-large-data",
                        "dataName": data_name,
                        "objectType": kind,
                        "objectId": cache_name or filename,
                        "plaintextSize": len(payload),
                        "encrypted": True,
                        "digest": "sha256:" + digest,
                    }
                    if data_name
                    else {}
                ),
            )
        )
    spec = ExecutionArtifactSpec(
        role=role,
        backend=backend,
        entrypoint=entrypoint,
        artifacts=refs,
        metadata=dict(metadata or {}),
    )
    return user.publish_encrypted_large_data(
        service,
        spec.to_bytes(),
        object_label=f"{object_label_prefix}-{role}-spec",
        freshness_ms=freshness_ms,
    )


def prepare_execution(
    context: Any,
    *,
    temp_root: Optional[str | Path] = None,
    allow_executables: bool = False,
    progress: Optional[Callable[[str, float], None]] = None,
) -> ExecutionContext:
    def report(phase: str, value: float) -> None:
        if progress is not None:
            progress(phase, value)

    assignment = context.assignment
    if not assignment.assigned_artifact:
        raise RuntimeError("collaboration assignment has no artifact name")
    report("FETCHING", 0.15)
    if not context.fetch_artifact(
        assignment.assigned_artifact, assignment.provisioning_timeout_ms or 10000
    ):
        raise RuntimeError(f"failed to fetch execution spec {assignment.assigned_artifact}")
    spec_payload = context.get_artifact(assignment.assigned_artifact)
    if spec_payload is None:
        raise RuntimeError("execution spec fetch returned no payload")
    spec = ExecutionArtifactSpec.from_bytes(spec_payload)
    report("VERIFYING", 0.35)
    root = Path(temp_root) if temp_root is not None else Path(tempfile.gettempdir())
    root.mkdir(parents=True, exist_ok=True)
    work_dir = Path(
        tempfile.mkdtemp(prefix=f"ndnsf-{_safe_file_token(spec.role)}-", dir=str(root))
    )
    artifact_paths: dict[str, Path] = {}
    for artifact in spec.artifacts or []:
        payload = _read_cached_artifact(artifact)
        if payload is None:
            payload = _fetch_artifact_payload(context, assignment.service, artifact)
        if hashlib.sha256(payload).hexdigest() != artifact.sha256:
            raise RuntimeError(f"artifact hash mismatch for {artifact.name}")
        artifact_path = Path(artifact.filename)
        if artifact_path.is_absolute() or ".." in artifact_path.parts:
            raise RuntimeError(f"unsafe artifact filename {artifact.filename!r}")
        _write_cached_artifact(artifact, payload)
        path = work_dir / artifact_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        if artifact.executable:
            if not allow_executables:
                raise RuntimeError(
                    f"artifact {artifact.name} is executable; explicit application policy is required"
                )
            path.chmod(0o700)
        artifact_paths[artifact.name] = path
    report("LOADING", 0.70)
    return ExecutionContext(spec=spec, artifact_paths=artifact_paths, work_dir=work_dir)


def _fetch_artifact_payload(context: Any, service: str, artifact: ExecutionArtifact) -> bytes:
    if artifact.large_data_reference:
        reference = dict(artifact.large_data_reference)
        source = str(reference.get("source", reference.get("sourceType", ""))).lower()
        if source in {"repo", "repo-manifest", "repo_manifest"} or (
            not source and artifact.repo_manifest
        ):
            if not artifact.repo_manifest:
                raise RuntimeError("repo artifact reference has no manifest")
            return _fetch_repo_manifest_payload(artifact.repo_manifest)
        data_name = str(reference.get("dataName", reference.get("data_name", "")))
        payload = context.fetch_encrypted_large_data(data_name or artifact.data_name, service)
    elif artifact.repo_manifest:
        return _fetch_repo_manifest_payload(artifact.repo_manifest)
    elif artifact.chunks:
        parts = [
            context.fetch_encrypted_large_data(name, service) for name in artifact.chunks
        ]
        if any(part is None for part in parts):
            raise RuntimeError(f"failed to fetch chunked artifact {artifact.name}")
        return b"".join(parts)
    else:
        payload = context.fetch_encrypted_large_data(artifact.data_name, service)
    if payload is None:
        raise RuntimeError(f"failed to fetch execution artifact {artifact.name}")
    return bytes(payload)


def _artifact_spec_parts(spec) -> tuple[bytes, str, str, bool, str]:
    if isinstance(spec, dict):
        return (
            bytes(spec["payload"]),
            str(spec["filename"]),
            str(spec.get("kind", "model")),
            bool(spec.get("executable", False)),
            str(spec.get("cache_name", spec.get("cacheName", ""))),
        )
    if len(spec) == 3:
        payload, filename, kind = spec
        executable, cache_name = False, ""
    elif len(spec) == 4:
        payload, filename, kind, executable = spec
        cache_name = ""
    elif len(spec) == 5:
        payload, filename, kind, executable, cache_name = spec
    else:
        raise ValueError("invalid artifact specification")
    return bytes(payload), str(filename), str(kind), bool(executable), str(cache_name)


def _artifact_from_repo_spec(name: str, spec: dict) -> ExecutionArtifact | None:
    manifest = spec.get("repo_manifest", spec.get("repoManifest"))
    if not manifest:
        return None
    return ExecutionArtifact(
        name=name,
        data_name="",
        filename=str(spec["filename"]),
        sha256=str(manifest["sha256"]),
        kind=str(spec.get("kind", "model")),
        chunks=[],
        executable=bool(spec.get("executable", False)),
        cache_name=str(spec.get("cache_name", spec.get("cacheName", ""))),
        repo_manifest=dict(manifest),
        large_data_reference={
            "source": "repo-manifest",
            "dataName": str(manifest.get("objectName", "")),
            "objectType": str(spec.get("kind", "model")),
            "objectId": str(spec.get("cache_name", spec.get("cacheName", spec["filename"]))),
            "plaintextSize": int(manifest.get("size", 0)),
            "encrypted": bool(manifest.get("encrypted", False)),
            "digest": "sha256:" + str(manifest.get("sha256", "")),
        },
    )


def _fetch_repo_manifest_payload(manifest: dict) -> bytes:
    segment_count = int(manifest.get("segmentCount", 1))
    size = int(manifest["size"])
    expected_hash = str(manifest["sha256"])
    segment_locations = list(manifest.get("segmentLocations", []))
    if segment_locations:
        by_data_name: dict[str, list[dict]] = {}
        for location in segment_locations:
            by_data_name.setdefault(str(location["dataName"]), []).append(dict(location))
        last_error = None
        for data_name, locations in by_data_name.items():
            covered_segments = set()
            hint_ranges: list[SegmentHintRange] = []
            route_strategy = "hint-first"
            for location in locations:
                start = int(location.get("start", 0))
                end = int(location.get("end", start))
                covered_segments.update(range(start, end + 1))
                if str(location.get("routeStrategy", "")) == "direct-first":
                    route_strategy = "direct-first"
                hint_ranges.append(SegmentHintRange(
                    start=start,
                    end=end,
                    forwarding_hints=tuple(str(hint) for hint in location.get("hints", [])),
                ))
            if len(covered_segments) < segment_count:
                continue
            try:
                versioned_names = {
                    str(location.get("versionedDataName", ""))
                    for location in locations
                    if location.get("versionedDataName")
                }
                first_ranges = [] if route_strategy == "direct-first" else hint_ranges
                second_ranges = hint_ranges if route_strategy == "direct-first" else []
                if len(versioned_names) == 1:
                    versioned_name = next(iter(versioned_names))
                    try:
                        payload = fetch_known_segmented_object_with_segment_hints(
                            versioned_name,
                            segment_count,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=first_ranges,
                        )
                    except Exception:
                        payload = fetch_known_segmented_object_with_segment_hints(
                            versioned_name,
                            segment_count,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=second_ranges,
                        )
                else:
                    try:
                        payload = fetch_segmented_object_with_segment_hints(
                            data_name,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=first_ranges,
                        )
                    except Exception:
                        payload = fetch_segmented_object_with_segment_hints(
                            data_name,
                            timeout_ms=30000,
                            interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                            hint_ranges=second_ranges,
                        )
                break
            except Exception as exc:
                last_error = exc
        else:
            raise RuntimeError(
                f"no repo segment location could serve {manifest.get('objectName')}: {last_error}"
            )
    else:
        replica_nodes = [str(value) for value in manifest.get("replicaNodes", [])]
        data_names = [str(value) for value in manifest.get("replicaDataNames", [])]
        if not data_names:
            data_names = [
                RepoDataName.data_name(repo_node, str(manifest["objectName"]))
                for repo_node in replica_nodes
            ]
        last_error = None
        for repo_node, data_name in zip(replica_nodes, data_names):
            try:
                hints = [] if data_name.startswith(repo_node.rstrip("/") + "/") else [repo_node]
                payload = fetch_segmented_object(
                    data_name,
                    timeout_ms=30000,
                    interest_lifetime_ms=default_large_data_interest_lifetime_ms(),
                    init_cwnd=8.0,
                    forwarding_hints=hints,
                )
                break
            except Exception as exc:
                last_error = exc
        else:
            raise RuntimeError(
                f"no repo replica could serve {manifest.get('objectName')}: {last_error}"
            )
    if len(payload) != size:
        raise RuntimeError(f"repo object size mismatch: {manifest.get('objectName')}")
    digest = hashlib.sha256(payload).hexdigest()
    if digest != expected_hash:
        raise RuntimeError(
            f"repo object hash mismatch: {manifest.get('objectName')}: "
            f"expected {expected_hash}, got {digest}"
        )
    return payload


class RepoDataName:
    @staticmethod
    def data_name(repo_node: str, object_name: str) -> str:
        digest = hashlib.sha256(object_name.encode()).hexdigest()
        return f"{repo_node.rstrip('/')}/NDNSF-DISTRIBUTED-REPO/DATA/{digest}"


def _safe_file_token(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "-_." else "-" for ch in value).strip("-") or "artifact"


def _artifact_cache_path(artifact: ExecutionArtifact) -> Path | None:
    if not artifact.cache_name:
        return None
    root = os.environ.get("NDNSF_ARTIFACT_CACHE_DIR")
    cache_root = Path(root) if root else Path.home() / ".cache" / "ndnsf" / "artifacts"
    return cache_root / f"{_safe_file_token(artifact.cache_name)}-{artifact.sha256[:16]}" / Path(artifact.filename).name


def _read_cached_artifact(artifact: ExecutionArtifact) -> bytes | None:
    path = _artifact_cache_path(artifact)
    if path is None or not path.exists():
        return None
    payload = path.read_bytes()
    return payload if hashlib.sha256(payload).hexdigest() == artifact.sha256 else None


def _write_cached_artifact(artifact: ExecutionArtifact, payload: bytes) -> None:
    path = _artifact_cache_path(artifact)
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    if artifact.executable:
        path.chmod(0o700)
