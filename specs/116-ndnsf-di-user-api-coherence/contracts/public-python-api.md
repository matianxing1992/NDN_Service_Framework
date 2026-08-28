# Contract: Preferred Python API

## Canonical Imports

```python
from ndnsf_distributed_inference.api import (
    DeploymentDefinition,
    DeploymentDefinitionRef,
    DeploymentActivationRecord,
    ArtifactReference,
    DeploymentConstraints,
    DeploymentHandle,
    DeploymentHandleRef,
    DeploymentProgress,
    DeploymentRef,
    DeploymentSummary,
    DeploymentStatus,
    ModelIntent,
    OptimizationObjective,
    RequestContract,
    InferenceApplication,
    InferenceClient,
    InferenceRequestHandle,
    InferenceProvider,
    InferenceResult,
    RequestableDeployment,
    ProviderDeploymentOffer,
    ProviderDeploymentOffers,
    RequestRef,
)
```

`ndnsf_distributed_inference.api.__all__` is an explicit, reviewed allowlist.
Canonical docs do not import public behavior from the package root.
The package contains explicit re-exports only; behavior remains owned once in
the corresponding `app_sdk` role modules.

## Application Creator

```python
class InferenceApplication:
    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        state_root: str | Path,
        envelope_key_file: str | Path | None = None,
        envelope_key_provider: EnvelopeKeyProvider | None = None,
        optimization: OptimizationSuite | None = None,
        deployment_manager: EnsureDeploymentCoordinator | None = None,
    ) -> InferenceApplication: ...

    def define(
        self,
        *,
        deployment_id: str,
        deployment_owner: str,
        service: str,
        model_intent: ModelIntent,
        artifacts: tuple[ArtifactReference, ...],
        request_contract: RequestContract,
        objective: OptimizationObjective,
        constraints: DeploymentConstraints,
        optimization_profile: str,
        metadata: Mapping[str, JSONValue] | None = None,
    ) -> DeploymentDefinition: ...

    def publish_definition(
        self, definition: DeploymentDefinition
    ) -> DeploymentDefinitionRef: ...

    def deploy(self, definition: DeploymentDefinition) -> DeploymentHandle: ...

    def request(
        self,
        deployment: RequestableDeployment,
        *,
        input: InputValue,
        timeout: timedelta | None = None,
        deadline: datetime | None = None,
        options: InferenceOptions | None = None,
    ) -> InferenceRequestHandle: ...

    @property
    def deployments(self) -> DeploymentCatalog: ...

    @property
    def requests(self) -> RequestCatalog: ...

    @property
    def advanced(self) -> ApplicationAdmin: ...
```

`InferenceApplication` is a composition root. It owns no new journal, policy
decision, provider action, or wire protocol. `from_config` must resolve exactly
one Application identity and NDNSF KeyChain signer authorized by policy; secret
key bytes are never accepted as definition fields or serialized references.

## Requester

```python
class InferenceClient:
    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        state_root: str | Path,
        envelope_key_file: str | Path | None = None,
        envelope_key_provider: EnvelopeKeyProvider | None = None,
        optimization: OptimizationSuite | None = None,
    ) -> InferenceClient: ...

    @property
    def deployments(self) -> DeploymentCatalog: ...

    @property
    def requests(self) -> RequestCatalog: ...

    def request(
        self,
        deployment: RequestableDeployment,
        *,
        input: InputValue,
        timeout: timedelta | None = None,
        deadline: datetime | None = None,
        options: InferenceOptions | None = None,
    ) -> InferenceRequestHandle: ...

    def run(self, deployment, **request_arguments) -> InferenceResult:
        return self.request(deployment, **request_arguments).result()

    async def run_async(self, deployment, **request_arguments) -> InferenceResult:
        return await self.request(deployment, **request_arguments).result_async()
```

`run` is convenience composition only. It has no separate execution logic.

`optimization` is used only when this client is the Application-authorized
deployment coordinator. Ordinary remote requesters omit it and never execute
the Application's policy profile locally.

`RequestableDeployment` is the same public union for both roles:

```python
RequestableDeployment = (
    DeploymentDefinition
    | DeploymentDefinitionRef
    | DeploymentHandle
    | DeploymentRef
)
```

`InferenceApplication.request` is a strict delegate to its configured
`InferenceClient.request`; it does not implement a creator-only network path.
The Application role differs only because it may create/sign definitions and
perform optional lifecycle administration. A requester may use an already
signed `DeploymentDefinition` value directly but cannot create, modify, or
re-sign it.

## Provider

```python
class InferenceProvider:
    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        state_root: str | Path,
    ) -> InferenceProvider: ...

    def serve(
        self,
        service: str,
        runner: RunnerAdapter,
        *,
        capabilities: ProviderCapabilities,
    ) -> ServiceRegistration: ...

    def run(self) -> None: ...
    def stop(self) -> None: ...

```

`serve_service` becomes a compatibility alias for `serve`; lifecycle mutation
does not appear as direct model-author methods. `ProviderAdminPort` is created
only from the advanced coordinator surface with independent credentials and is
not a property of `InferenceProvider`.

## Rules

1. No canonical signature includes underscore-prefixed dependency injection.
2. No method selects local/network behavior from argument shape or empty values.
3. No public dynamic `__getattr__` defines undocumented methods.
4. Production durable state and envelope-key source remain explicit.
5. Optional model backends are lazy and cannot break API-only imports.
6. `request` requires exactly one typed `timeout` or aware `deadline`; neither
   absence nor numeric shorthand is canonical.
7. `InferenceApplication.define` signs canonical intent with the configured
   Application identity; `request`, `publish_definition`, and optional `deploy`
   reject unsigned, modified, expired, or unauthorized definitions before
   policy evaluation or Provider mutation.
8. `request` is the primary path and automatically prepares a cold deployment before
   execution. `deploy` is optional prewarming and must use the identical
   ensure-deployment coordinator and status contracts.
9. ACK availability and generic Collaboration progress are observations, not
   model-execution authority. The request execution certificate first commits
   responsibility/membership/leases; exact signed all-role readiness then gates
   model-runner invocation.
10. `InferenceApplication.request` and `InferenceClient.request` have identical
    deployment, timing, option, handle, transport, security, and recovery semantics.
