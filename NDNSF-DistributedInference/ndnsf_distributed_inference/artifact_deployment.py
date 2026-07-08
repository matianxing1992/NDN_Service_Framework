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
from pathlib import Path
import stat
import threading
import time
from typing import Any

from ndnsf import (
    AckDecision,
    NEGATIVE_ACK_REASON_INTERNAL_ERROR,
    NEGATIVE_ACK_REASON_MODEL_UNAVAILABLE,
    ServiceOperationState,
    ServiceOperationStatus,
    encode_ack_metadata,
)

from .repo import NetworkDistributedRepoClient, RepoObjectManifest, repo_manifest_from_large_data_reference


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
