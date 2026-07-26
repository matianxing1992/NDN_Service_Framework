"""Application-owned definition authority and thin composition root."""

from __future__ import annotations

import base64
from dataclasses import replace
from datetime import datetime, timedelta, timezone
import hashlib
from pathlib import Path
from typing import Mapping

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey,
)

from .contracts import (
    ArtifactReference, DeploymentConstraints, DeploymentDefinition,
    InferenceOptions, ModelIntent, OptimizationObjective, RequestContract,
    RequestableDeployment,
)


class ApplicationDefinitionSigner:
    """Signs immutable deployment intent; never signs policy-resolved output."""

    def __init__(self, application_identity: str, key: Ed25519PrivateKey):
        if not application_identity.startswith("/"):
            raise ValueError("Application identity must be an NDN name")
        self.application_identity = application_identity
        self._key = key
        raw = key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.public_key = base64.b64encode(raw).decode("ascii")
        self.key_id = "sha256:" + hashlib.sha256(raw).hexdigest()

    @classmethod
    def generate(cls, application_identity: str) -> "ApplicationDefinitionSigner":
        return cls(application_identity, Ed25519PrivateKey.generate())

    @classmethod
    def load_or_create(cls, application_identity: str, state_root: str | Path):
        root = Path(state_root)
        root.mkdir(parents=True, exist_ok=True)
        path = root / "application-definition-signing-key.pem"
        if path.exists():
            key = serialization.load_pem_private_key(path.read_bytes(), password=None)
            if not isinstance(key, Ed25519PrivateKey):
                raise ValueError("Application definition key must be Ed25519")
            return cls(application_identity, key)
        key = Ed25519PrivateKey.generate()
        path.write_bytes(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        path.chmod(0o600)
        return cls(application_identity, key)

    def define(
        self, *, deployment_id: str, deployment_owner: str, service: str,
        model_intent: ModelIntent, artifacts: tuple[ArtifactReference, ...],
        request_contract: RequestContract, objective: OptimizationObjective,
        constraints: DeploymentConstraints, optimization_profile: str,
        metadata: Mapping | None = None, coordinator_service: str = "",
        previous_revision: str = "",
        validity: timedelta = timedelta(days=1),
    ) -> DeploymentDefinition:
        if not deployment_owner.startswith("/") or not service.startswith("/"):
            raise ValueError("deployment owner and service must be NDN names")
        if validity.total_seconds() <= 0:
            raise ValueError("definition validity must be positive")
        now = datetime.now(timezone.utc)
        coordinator = coordinator_service or (
            deployment_owner.rstrip("/") + "/NDNSF/DI/COORDINATE")
        unsigned = DeploymentDefinition(
            deployment_id=deployment_id,
            model_id=model_intent.allowed[0],
            artifacts=tuple(artifacts),
            roles=(),
            configuration={},
            application_identity=self.application_identity,
            deployment_owner=deployment_owner,
            coordinator_service=coordinator,
            service=service,
            model_intent=model_intent,
            request_contract=request_contract,
            objective=objective,
            constraints=constraints,
            optimization_profile=optimization_profile,
            metadata=dict(metadata or {}),
            created_at=now.isoformat(),
            expires_at=(now + validity).isoformat(),
            previous_revision=previous_revision,
            signer_key_id=self.key_id,
            signer_public_key=self.public_key,
        )
        signature = base64.b64encode(
            self._key.sign(unsigned.digest().encode("ascii"))).decode("ascii")
        return replace(unsigned, signature=signature)

    @staticmethod
    def verify(definition: DeploymentDefinition) -> DeploymentDefinition:
        if not definition.signed:
            raise ValueError("deployment definition is unsigned")
        try:
            raw = base64.b64decode(definition.signer_public_key, validate=True)
            if ("sha256:" + hashlib.sha256(raw).hexdigest() !=
                    definition.signer_key_id):
                raise ValueError("definition signer key ID mismatch")
            Ed25519PublicKey.from_public_bytes(raw).verify(
                base64.b64decode(definition.signature, validate=True),
                definition.digest().encode("ascii"))
            expires = datetime.fromisoformat(definition.expires_at)
            if expires.tzinfo is None or expires <= datetime.now(timezone.utc):
                raise ValueError("deployment definition expired")
        except (InvalidSignature, ValueError) as exc:
            raise ValueError("deployment definition signature is invalid") from exc
        return definition


class InferenceApplication:
    """Definition authority composed with the requester-only client."""

    def __init__(self, signer: ApplicationDefinitionSigner, client,
                 *, deployment_manager=None):
        self._signer = signer
        self._client = client
        self._deployment_manager = deployment_manager
        if client is not None:
            client.deployments.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            if deployment_manager is not None:
                ensure = getattr(
                    deployment_manager, "ensure_deployment", deployment_manager)
                client.deployments.bind_ensure_deployment(ensure)

    @classmethod
    def from_config(cls, config, *, state_root, envelope_key_file=None,
                    envelope_key_provider=None, optimization=None,
                    deployment_manager=None):
        from .client import InferenceClient
        root = Path(state_root)
        client = InferenceClient.from_config(
            config, state_root=root,
            envelope_key_file=envelope_key_file,
            envelope_key_provider=envelope_key_provider,
            optimization=optimization)
        # Bind definition authorship to the configured NDNSF identity rather
        # than deriving an unrelated name from the configuration filename.
        identity = str(client._core.requester_identity)
        if not identity.startswith("/"):
            raise ValueError("configured Application identity must be an NDN name")
        signer = ApplicationDefinitionSigner.load_or_create(identity, root)
        return cls(signer, client, deployment_manager=deployment_manager)

    def define(self, **intent) -> DeploymentDefinition:
        return self._signer.define(**intent)

    def publish_definition(self, definition: DeploymentDefinition):
        self._signer.verify(definition)
        return self._client.deployments.publish_definition(definition)

    def deploy(self, definition: DeploymentDefinition):
        self._signer.verify(definition)
        return self._client.deploy(definition)

    def request(
        self, deployment: RequestableDeployment, *, input,
        timeout=None, deadline=None, options: InferenceOptions | None = None,
    ) -> InferenceRequestHandle:
        return self._client.request(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options)

    @property
    def deployments(self):
        return self._client.deployments

    @property
    def requests(self):
        return self._client.requests


__all__ = ["ApplicationDefinitionSigner", "InferenceApplication"]
