"""Application-owned definition authority and thin composition root."""

from __future__ import annotations

import base64
from dataclasses import replace
from datetime import datetime, timedelta, timezone
import hashlib
from pathlib import Path
from typing import Mapping
import warnings

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey,
)

from .contracts import (
    ApplicationRuntimeConfig, ArtifactReference, DeploymentConstraints,
    DeploymentDefinition, GenerationConfig, GenerationInput, InferenceOptions, ModelIntent,
    OptimizationObjective, RequestContract, RequestableDeployment,
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
        self._preplanned_compatibility_uses = 0
        if client is not None:
            client.deployments.authorize_application(
                signer.application_identity, signer.key_id, signer.public_key)
            if deployment_manager is not None:
                ensure = getattr(
                    deployment_manager, "ensure_deployment", deployment_manager)
                client.deployments.bind_ensure_deployment(ensure)

    @classmethod
    def from_config(
        cls, config, *, state_root, envelope_key_file=None,
        envelope_key_provider=None, optimization=None,
        deployment_manager=None, adapters=(), strategy=None,
        catalog_snapshot_provider=None, verify_offer_signature=None,
        split_materializer=None, artifact_publisher=None, budget=None,
        **network_options,
    ):
        from .client import InferenceClient
        from ..policy import load_config
        root = Path(state_root)
        runtime = ApplicationRuntimeConfig.from_mapping(load_config(config))
        client = InferenceClient.from_application_config(
            runtime, state_root=root,
            envelope_key_file=envelope_key_file,
            envelope_key_provider=envelope_key_provider,
            optimization=optimization,
            **network_options)
        # Bind definition authorship to the configured NDNSF identity rather
        # than deriving an unrelated name from the configuration filename.
        identity = str(client._core.requester_identity)
        if not identity.startswith("/"):
            raise ValueError("configured Application identity must be an NDN name")
        signer = ApplicationDefinitionSigner.load_or_create(identity, root)
        application = cls(
            signer, client, deployment_manager=deployment_manager)
        adapter_values = tuple(adapters)
        if adapter_values:
            if verify_offer_signature is None:
                raise ValueError(
                    "automatic planning requires Provider offer verification")
            if strategy is None:
                import time
                from ..planner.presplit_first import PreSplitFirstStrategy
                strategy = PreSplitFirstStrategy(
                    at_ms=int(time.time() * 1000))
            client._core.configure_automatic_planning(
                service_name=runtime.service,
                adapters=adapter_values,
                strategy=strategy,
                catalog_snapshot_provider=(
                    catalog_snapshot_provider or (lambda: ())),
                verify_offer_signature=verify_offer_signature,
                split_materializer=split_materializer,
                artifact_publisher=artifact_publisher,
                budget=budget,
                ack_timeout_ms=runtime.ack_timeout_ms,
            )
        return application

    def define(self, **intent) -> DeploymentDefinition:
        return self._signer.define(**intent)

    def publish_definition(self, definition: DeploymentDefinition):
        self._signer.verify(definition)
        return self._client.deployments.publish_definition(definition)

    def deploy(self, definition: DeploymentDefinition):
        self._signer.verify(definition)
        return self._client.deploy(definition)

    def request(
        self, *legacy_deployment,
        model=None, input=None, generation: GenerationConfig | None = None,
        strategy=None, request_id: str = "",
        timeout=None, deadline=None, options: InferenceOptions | None = None,
    ):
        """Submit the request-first, post-ACK-planned public invocation.

        A single positional deployment remains a counted migration shim.  It
        never becomes the default path and keyword ``deployment=`` is rejected
        by the Python signature.
        """
        if legacy_deployment:
            if (len(legacy_deployment) != 1 or model is not None
                    or generation is not None or strategy is not None):
                raise TypeError("legacy request accepts one deployment only")
            warnings.warn(
                "positional InferenceApplication.request(deployment, ...) is "
                "deprecated; use request_preplanned()",
                DeprecationWarning, stacklevel=2)
            self._preplanned_compatibility_uses += 1
            return self.request_preplanned(
                legacy_deployment[0], input=input, timeout=timeout,
                deadline=deadline, options=options)
        if model is None or not isinstance(input, GenerationInput):
            raise TypeError("request requires model and GenerationInput")
        if not str(getattr(model, "source_revision", "") or ""):
            raise ValueError(
                "public model requests require an immutable model revision")
        effective_generation = generation or GenerationConfig()
        if not isinstance(effective_generation, GenerationConfig):
            raise TypeError("generation must be GenerationConfig")
        if any(value is not None for value in (timeout, deadline, options)):
            raise TypeError(
                "model-first deadlines/options belong in GenerationConfig/app.yaml")
        return self._client.request_model(
            model=model, input=input, generation=effective_generation,
            strategy=strategy, request_id=request_id)

    def request_preplanned(
        self, deployment: RequestableDeployment, *, input,
        timeout=None, deadline=None, options: InferenceOptions | None = None,
    ):
        requester = getattr(self._client, "request_preplanned", None)
        if requester is None:
            requester = self._client.request
        return requester(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options)

    @property
    def preplanned_compatibility_uses(self) -> int:
        return self._preplanned_compatibility_uses

    @property
    def deployments(self):
        return self._client.deployments

    @property
    def requests(self):
        return self._client.requests


__all__ = ["ApplicationDefinitionSigner", "InferenceApplication"]
