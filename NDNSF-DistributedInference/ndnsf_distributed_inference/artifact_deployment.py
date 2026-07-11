"""Provider-side artifact materialization helpers.

These helpers are intentionally above NDNSF Core. They understand DI artifact
reference files and repo manifests, then write verified files into a local
provider cache. A caller may supply either a real ``NetworkDistributedRepoClient``
or a local repo-compatible object for smoke tests.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import tempfile
import stat
import threading
import time
from typing import Any, Optional

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
                    "legacyStatus": status,
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
) -> ExecutionContext:
    assignment = context.assignment
    if not assignment.assigned_artifact:
        raise RuntimeError("collaboration assignment has no artifact name")
    if not context.fetch_artifact(
        assignment.assigned_artifact, assignment.provisioning_timeout_ms or 10000
    ):
        raise RuntimeError(f"failed to fetch execution spec {assignment.assigned_artifact}")
    spec_payload = context.get_artifact(assignment.assigned_artifact)
    if spec_payload is None:
        raise RuntimeError("execution spec fetch returned no payload")
    spec = ExecutionArtifactSpec.from_bytes(spec_payload)
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
