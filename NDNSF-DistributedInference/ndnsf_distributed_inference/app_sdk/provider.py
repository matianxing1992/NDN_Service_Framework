"""Revision-scoped provider-agent registration, readiness, and reporting."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import base64
import hashlib
import json
from pathlib import Path
import time
from typing import Callable, Mapping

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey,
)

from ..core.ports import CheckpointRecord, ProgressRecord


class ProviderEvidenceSigner:
    """Ed25519 signer for revision- and boot-bound Provider evidence."""

    def __init__(self, private_key: Ed25519PrivateKey):
        self._private_key = private_key
        public_raw = private_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.key_id = "sha256:" + hashlib.sha256(public_raw).hexdigest()

    @classmethod
    def generate(cls):
        return cls(Ed25519PrivateKey.generate())

    @classmethod
    def from_private_pem(cls, pem: bytes):
        key = serialization.load_pem_private_key(
            pem, password=None, backend=default_backend())
        if not isinstance(key, Ed25519PrivateKey):
            raise ValueError("Provider evidence key must be Ed25519")
        return cls(key)

    def __call__(self, digest: str) -> str:
        return base64.b64encode(
            self._private_key.sign(digest.encode("utf-8"))).decode("ascii")

    def private_pem(self) -> bytes:
        return self._private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )

    def public_pem(self) -> bytes:
        return self._private_key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo,
        )


class ProviderEvidenceVerifier:
    """Explicit trust set for Provider evidence; unknown key IDs fail closed."""

    def __init__(self, trusted_public_keys: Mapping[str, bytes]):
        self._keys: dict[str, Ed25519PublicKey] = {}
        for key_id, pem in trusted_public_keys.items():
            key = serialization.load_pem_public_key(
                bytes(pem), backend=default_backend())
            if not isinstance(key, Ed25519PublicKey):
                raise ValueError("Provider evidence key must be Ed25519")
            raw = key.public_bytes(
                serialization.Encoding.Raw, serialization.PublicFormat.Raw)
            expected = "sha256:" + hashlib.sha256(raw).hexdigest()
            if key_id != expected:
                raise ValueError("Provider evidence key ID does not match public key")
            self._keys[key_id] = key

    def __call__(self, receipt) -> bool:
        key = self._keys.get(receipt.signer_key_id)
        if key is None:
            return False
        try:
            signature = base64.b64decode(receipt.signature, validate=True)
            key.verify(signature, receipt.evidence_digest.encode("utf-8"))
            return True
        except (InvalidSignature, ValueError):
            return False


@dataclass(frozen=True)
class ProviderRegistration:
    provider: str
    boot_epoch: str
    capabilities: tuple[str, ...]
    capacity_by_role: Mapping[str, int]
    permission_ready: bool


@dataclass(frozen=True)
class ProviderReadiness:
    provider: str
    role: str
    revision: str
    boot_epoch: str
    artifact_digests: tuple[str, ...]
    adapter_name: str
    adapter_version: str
    capacity: int
    permission_ready: bool
    observed_at_ms: int
    expires_at_ms: int
    ready: bool
    evidence_digest: str
    signer_key_id: str
    signature: str
    reason: str = ""

    def evidence_payload(self) -> bytes:
        return json.dumps({
            "provider": self.provider,
            "role": self.role,
            "revision": self.revision,
            "bootEpoch": self.boot_epoch,
            "artifactDigests": list(self.artifact_digests),
            "adapterName": self.adapter_name,
            "adapterVersion": self.adapter_version,
            "capacity": self.capacity,
            "permissionReady": self.permission_ready,
            "observedAtMs": self.observed_at_ms,
            "expiresAtMs": self.expires_at_ms,
            "ready": self.ready,
            "signerKeyId": self.signer_key_id,
            "reason": self.reason,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    def expected_digest(self) -> str:
        return "sha256:" + hashlib.sha256(self.evidence_payload()).hexdigest()

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, payload: Mapping):
        values = dict(payload)
        values["artifact_digests"] = tuple(values.get("artifact_digests", ()))
        return cls(**values)

    @classmethod
    def issue(cls, *, signer: Callable[[str], str] | None, **values):
        unsigned = cls(evidence_digest="", signature="", **values)
        digest = unsigned.expected_digest()
        signature = signer(digest) if signer is not None else ""
        return cls(evidence_digest=digest, signature=signature, **values)


@dataclass(frozen=True)
class ProviderActionReceipt:
    provider: str
    role: str
    revision: str
    boot_epoch: str
    action: str
    state: str
    observed_at_ms: int
    expires_at_ms: int
    evidence_digest: str
    signer_key_id: str
    signature: str
    reason: str = ""

    def evidence_payload(self) -> bytes:
        return json.dumps({
            "provider": self.provider,
            "role": self.role,
            "revision": self.revision,
            "bootEpoch": self.boot_epoch,
            "action": self.action,
            "state": self.state,
            "observedAtMs": self.observed_at_ms,
            "expiresAtMs": self.expires_at_ms,
            "signerKeyId": self.signer_key_id,
            "reason": self.reason,
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")

    def expected_digest(self) -> str:
        return "sha256:" + hashlib.sha256(self.evidence_payload()).hexdigest()

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, payload: Mapping):
        return cls(**dict(payload))

    @classmethod
    def issue(cls, *, signer: Callable[[str], str] | None, **values):
        unsigned = cls(evidence_digest="", signature="", **values)
        digest = unsigned.expected_digest()
        signature = signer(digest) if signer is not None else ""
        return cls(evidence_digest=digest, signature=signature, **values)


class APPProvider:
    """APP-owned façade over an already launched generic Provider agent.

    Registration is not process provisioning. The external operator starts the
    agent; this object binds its boot epoch and capabilities to one immutable
    deployment revision before creating the selected Runner.
    """

    def __init__(self, provider: str, adapter_registry=None,
                 *, signer: Callable[[str], str] | None = None,
                 signer_key_id: str = "",
                 readiness_ttl_ms: int = 30_000,
                 network_provider=None):
        self.provider = provider
        self.adapters = adapter_registry
        self._network_provider = network_provider
        self.signer = signer
        self.signer_key_id = signer_key_id
        self.readiness_ttl_ms = int(readiness_ttl_ms)
        self.registration: ProviderRegistration | None = None
        self.active_revision = ""
        self.runner = None
        self.events: list[object] = []
        self._active_readiness: dict[str, ProviderReadiness] = {}
        self._drained_readiness: dict[str, ProviderReadiness] = {}

    @property
    def provider_boot_epoch(self) -> str:
        """Return the network Provider's Core-authenticated boot epoch."""

        if self._network_provider is None:
            raise AttributeError("provider_boot_epoch")
        return self._network_provider.provider_boot_epoch

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        provider_id: str = "",
        provider_identity: str = "",
        adapter_registry=None,
        signer: Callable[[str], str] | None = None,
        signer_key_id: str = "",
        readiness_ttl_ms: int = 30_000,
        **network_options,
    ) -> "APPProvider":
        """Construct the canonical Provider over the existing NDNSF agent."""
        from .facades import APPProvider as NetworkAPPProvider

        network_provider = NetworkAPPProvider.from_config(
            config, provider_id=provider_id, **network_options)
        return cls(
            provider_identity or provider_id, adapter_registry,
            network_provider=network_provider, signer=signer,
            signer_key_id=signer_key_id,
            readiness_ttl_ms=readiness_ttl_ms,
        )

    @property
    def deployment(self):
        if self._network_provider is None:
            raise AttributeError("deployment")
        return self._network_provider.deployment

    def roles_for_service(self, service: str):
        if self._network_provider is None:
            raise RuntimeError("network Provider is not configured")
        return self._network_provider.roles_for_service(service)

    def serve_service(self, **kwargs):
        if self._network_provider is None:
            raise RuntimeError("network Provider is not configured")
        return self._network_provider.serve_service(**kwargs)

    serve = serve_service

    def run(self):
        if self._network_provider is None:
            raise RuntimeError("network Provider is not configured")
        return self._network_provider.run()

    def stop(self):
        if self._network_provider is not None:
            return self._network_provider.stop()
        self.drain()
        return 0

    def register_agent(self, *, boot_epoch: str, capabilities=(),
                       capacity_by_role=None, permission_ready: bool = True):
        if not boot_epoch or self.registration is not None:
            raise ValueError("provider agent requires one boot-epoch registration")
        capacity = dict(capacity_by_role or {})
        if any(int(value) <= 0 for value in capacity.values()):
            raise ValueError("provider capacity must be positive")
        self.registration = ProviderRegistration(
            self.provider, boot_epoch, tuple(capabilities), capacity,
            bool(permission_ready))
        return self.registration

    def stage(self, revision, target, artifacts):
        registration = self.registration
        artifact_tuple = tuple(artifacts)
        observed_at_ms = int(time.time() * 1000)
        try:
            if registration is None:
                raise ValueError("provider agent is not registered")
            if not registration.permission_ready:
                raise PermissionError("provider permission is not ready")
            if target.provider != self.provider:
                raise ValueError("execution target provider mismatch")
            if (registration.capacity_by_role
                    and registration.capacity_by_role.get(target.role, 0) <= 0):
                raise ValueError("provider role capacity unavailable")
            if not artifact_tuple or any(not item.startswith("sha256:")
                                         for item in artifact_tuple):
                raise ValueError("provider artifacts require immutable digests")
            if self.adapters is None:
                raise ValueError("provider Runner adapter registry is missing")
            if self.signer is None or not self.signer_key_id:
                raise ValueError("provider readiness signer is not configured")
            adapter = self.adapters.resolve(target.adapter_name, target)
            self.runner = adapter.create_runner(target, artifact_tuple)
            identity = ":".join((self.provider, registration.boot_epoch,
                                 revision, target.adapter_name, *artifact_tuple))
            del identity
            return ProviderReadiness.issue(
                signer=self.signer,
                provider=self.provider,
                role=target.role,
                revision=revision,
                boot_epoch=registration.boot_epoch,
                artifact_digests=artifact_tuple,
                adapter_name=target.adapter_name,
                adapter_version=str(adapter.version),
                capacity=int(registration.capacity_by_role.get(target.role, 1)),
                permission_ready=registration.permission_ready,
                observed_at_ms=observed_at_ms,
                expires_at_ms=observed_at_ms + self.readiness_ttl_ms,
                ready=True,
                signer_key_id=self.signer_key_id,
                reason="",
            )
        except Exception as exc:
            return ProviderReadiness.issue(
                signer=self.signer,
                provider=self.provider,
                role=getattr(target, "role", ""),
                revision=revision,
                boot_epoch=registration.boot_epoch if registration else "",
                artifact_digests=artifact_tuple,
                adapter_name=getattr(target, "adapter_name", ""),
                adapter_version="",
                capacity=0,
                permission_ready=bool(registration and registration.permission_ready),
                observed_at_ms=observed_at_ms,
                expires_at_ms=observed_at_ms + max(1, self.readiness_ttl_ms),
                ready=False,
                signer_key_id=self.signer_key_id,
                reason=type(exc).__name__,
            )

    def activate(self, readiness):
        if (not readiness.ready or not readiness.signature
                or self.registration is None
                or readiness.boot_epoch != self.registration.boot_epoch
                or readiness.provider != self.provider
                or readiness.evidence_digest != readiness.expected_digest()
                or self.signer is None
                or readiness.signature != self.signer(readiness.evidence_digest)):
            raise ValueError("provider is not revision-bound READY")
        if self.active_revision and self.active_revision != readiness.revision:
            raise ValueError("provider cannot activate mixed revisions")
        self.active_revision = readiness.revision
        self._active_readiness[readiness.role] = readiness
        now_ms = int(time.time() * 1000)
        return ProviderActionReceipt.issue(
            signer=self.signer,
            provider=readiness.provider,
            role=readiness.role,
            revision=readiness.revision,
            boot_epoch=readiness.boot_epoch,
            action="ACTIVATE",
            state="ACTIVE",
            observed_at_ms=now_ms,
            expires_at_ms=now_ms + max(1, self.readiness_ttl_ms),
            signer_key_id=self.signer_key_id,
            reason="",
        )

    def report_progress(self, record: ProgressRecord):
        if not self.active_revision or record.attempt_epoch <= 0:
            raise ValueError("progress requires active revision and attempt")
        self.events.append(record)

    def report_checkpoint(self, record: CheckpointRecord):
        if not self.active_revision or not record.checkpoint_digest.startswith("sha256:"):
            raise ValueError("checkpoint requires active revision and digest")
        self.events.append(record)

    def report_output(self, *, request_id: str, attempt_epoch: int,
                      output_epoch: int, result_digest: str):
        if (not self.active_revision or not request_id or attempt_epoch <= 0
                or output_epoch < 0 or not result_digest.startswith("sha256:")):
            raise ValueError("invalid provider output evidence")
        event = {"requestId": request_id, "attemptEpoch": attempt_epoch,
                 "outputEpoch": output_epoch, "resultDigest": result_digest}
        self.events.append(event)
        return event

    def drain(self):
        now_ms = int(time.time() * 1000)
        receipts = tuple(ProviderActionReceipt.issue(
            signer=self.signer,
            provider=item.provider,
            role=item.role,
            revision=item.revision,
            boot_epoch=item.boot_epoch,
            action="DRAIN",
            state="INACTIVE",
            observed_at_ms=now_ms,
            expires_at_ms=now_ms + max(1, self.readiness_ttl_ms),
            signer_key_id=self.signer_key_id,
            reason="",
        ) for item in self._active_readiness.values())
        self._drained_readiness = dict(self._active_readiness)
        self._active_readiness.clear()
        self.active_revision = ""
        self.runner = None
        return receipts

    def delete(self, revision: str):
        now_ms = int(time.time() * 1000)
        selected = tuple(item for item in self._drained_readiness.values()
                         if item.revision == revision)
        receipts = tuple(ProviderActionReceipt.issue(
            signer=self.signer,
            provider=item.provider,
            role=item.role,
            revision=item.revision,
            boot_epoch=item.boot_epoch,
            action="DELETE",
            state="DELETED",
            observed_at_ms=now_ms,
            expires_at_ms=now_ms + max(1, self.readiness_ttl_ms),
            signer_key_id=self.signer_key_id,
            reason="",
        ) for item in selected)
        for item in selected:
            self._drained_readiness.pop(item.role, None)
        return receipts

    shutdown = drain


class InferenceProvider:
    """Least-authority model-serving surface.

    Lifecycle mutation deliberately lives on ``ProviderAdminPort`` and is not
    reachable through this facade.
    """

    def __init__(self, network_provider):
        self._network_provider = network_provider
        self._registrations = {}

    @classmethod
    def from_config(cls, config, *, state_root):
        root = Path(state_root)
        root.mkdir(parents=True, exist_ok=True)
        if not root.is_dir():
            raise ValueError("provider state_root must be a directory")
        from .facades import APPProvider as NetworkAPPProvider
        return cls(NetworkAPPProvider.from_config(config))

    def serve(self, service: str, runner, *, capabilities):
        if not service.startswith("/"):
            raise ValueError("service must be an NDN name")
        if service in self._registrations:
            raise ValueError("service is already registered")
        def capability(name, default=None):
            if isinstance(capabilities, Mapping):
                return capabilities.get(name, default)
            return getattr(capabilities, name, default)

        handler = runner if callable(runner) else getattr(runner, "handle", None)
        if handler is None:
            handler = getattr(runner, "run", None)
        if not callable(handler):
            raise TypeError("runner must be callable or expose handle()/run()")
        registration = self._network_provider.serve_service(
            service=service,
            roles=capability("roles", "all"),
            handler=handler,
            backends=tuple(capability("backends", ())),
            queue_depth=int(capability("queue_depth", 0)),
            has_model=bool(capability("has_model", False)),
            can_provision=bool(capability("can_provision", True)),
            readiness_probe=capability("readiness_probe"),
            ready_without_model=bool(capability("ready_without_model", False)),
        )
        self._registrations[service] = registration
        return registration

    def serve_service(self, *args, **kwargs):
        import warnings
        warnings.warn(
            "serve_service() is deprecated; use serve()",
            DeprecationWarning, stacklevel=2)
        return self.serve(*args, **kwargs)

    def run(self):
        return self._network_provider.run()

    def stop(self):
        return self._network_provider.stop()


class ProviderAdminPort:
    """Separately credentialed advanced Provider lifecycle authority."""

    def __init__(self, provider: APPProvider, *, credential_id: str):
        if not credential_id:
            raise PermissionError("ProviderAdminPort requires coordinator credentials")
        self._provider = provider
        self.credential_id = credential_id

    def stage(self, revision, target, artifacts):
        return self._provider.stage(revision, target, artifacts)

    def activate(self, readiness):
        return self._provider.activate(readiness)

    def drain(self):
        return self._provider.drain()

    def delete(self, revision):
        return self._provider.delete(revision)

    def report_progress(self, record):
        return self._provider.report_progress(record)

    def report_checkpoint(self, record):
        return self._provider.report_checkpoint(record)


__all__ = [
    "APPProvider", "InferenceProvider", "ProviderAdminPort",
    "ProviderActionReceipt", "ProviderEvidenceSigner",
    "ProviderEvidenceVerifier", "ProviderReadiness", "ProviderRegistration",
]
