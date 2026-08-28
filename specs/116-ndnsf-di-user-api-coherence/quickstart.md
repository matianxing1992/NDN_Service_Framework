# Quickstart: Coherent NDNSF-DI API

This is the implemented Spec 116 contract. Network examples still require the
normal NDNSF controller, identities, permissions, and running NFD/MiniNDN
environment; CPU-only contract tests do not manufacture those prerequisites.

## 1. Request Directly and Prepare on Demand

```python
from datetime import timedelta
from ndnsf_distributed_inference.api import (
    ArtifactReference,
    DeploymentConstraints,
    InferenceApplication,
    ModelIntent,
    OptimizationObjective,
    RequestContract,
)

application = InferenceApplication.from_config(
    "ndnsf-di.yaml",
    state_root="/var/lib/ndnsf-di/my-app",
    envelope_key_file="/run/secrets/ndnsf-di-envelope-key",
)

definition = application.define(
    deployment_id="qwen-demo",
    deployment_owner="/ndnsf/apps/qwen/deployment-owner",
    service="/LLM/Qwen/Generate",
    model_intent=ModelIntent(allowed=("Qwen/approved-v1",)),
    artifacts=(ArtifactReference(
        uri="repo:/models/qwen",
        digest="sha256:...",
        size_bytes=0,
    ),),
    request_contract=RequestContract(
        input_schema="ndnsf.qwen.prompt/v1",
        output_schema="ndnsf.qwen.generated-text/v1",
    ),
    objective=OptimizationObjective(primary="latency"),
    constraints=DeploymentConstraints(
        minimum_providers=2,
        allowed_partition_kinds=("layer-range",),
    ),
    optimization_profile="default",
)

request = application.request(
    definition,
    input={"prompt": "Explain NDN in one sentence."},
    timeout=timedelta(minutes=6),
)
result = request.result()
```

After definition construction, the primary workflow is two statements: request
and result. If no compatible Provider is ready, the request itself performs
bounded Provider selection, artifact fetch/verification, model load/warmup,
readiness certification, and inference. The timeout covers the entire
preparation-plus-execution lifecycle.

Preparation can be observed without changing the flow:

```python
status = request.deployment_status()
for role in status.roles:
    print(role.phase, role.role, role.progress)
for event in request.events():
    print(event.state, event.reason_code)
```

Events are convenient observations; `deployment_status()` is the authoritative
recoverable aggregate/per-role snapshot and does not require every event to
arrive. Internally a creator request reconciles selected Providers' validated
generic NDNSF Collaboration snapshots. A remote request receives a signed
aggregate projection from the definition-bound deployment-owner coordinator,
whose inner request still validates each selected Provider. DI-specific phase
and exact readiness checks remain inside that owner; generic progress alone
never marks the model executable.

`application.define(...)` canonicalizes and signs Application intent. It does
not choose the partition graph, roles, Providers, or RunnerAdapter. The selected
OptimizationSuite resolves those facts into the immutable revision, and
`APPDeployment` may execute that revision but cannot rewrite the definition.
An ACK reports willingness and `READY`/`NEEDS_PREPARATION`/`UNAVAILABLE` state;
Selection assigns responsibility. Neither proves readiness. The Selection-bound
execution certificate commits responsibility and leases, but not readiness.
Exact signed readiness and an all-role barrier are checked before any model
runner executes. READY-first selection is an optional optimization policy.

## 2. Discover and Invoke From Another Process

```python
from datetime import timedelta
from ndnsf_distributed_inference.api import InferenceClient

client = InferenceClient.from_config(
    "ndnsf-di.yaml",
    state_root="/var/lib/ndnsf-di/requester",
    envelope_key_file="/run/secrets/ndnsf-di-envelope-key",
)

available = client.deployments.discover(service="/LLM/Qwen/Generate")
deployment = available[0].deployment
result = client.run(
    deployment,
    input={"prompt": "Hello"},
    timeout=timedelta(seconds=30),
)
```

The Application makes on-demand use discoverable with
`application.publish_definition(definition)`; `request(definition, ...)` also
publishes the exact signed definition as part of its durable workflow. NDNSD
supplies only untrusted locator/digest hints. An ON_DEMAND summary contains a
validated `DeploymentDefinitionRef`; an ACTIVE summary contains a validated
`DeploymentRef` with activation evidence. The requester can invoke either but
cannot create or mutate Application intent. An ON_DEMAND reference binds the
Application-authorized deployment coordinator; the remote client sends the
request there and does not install or run the Application's optimization
policies itself.

The deployment-owner process is an explicit advanced role. It registers the
bound coordinator as a one-role NDNSF Collaboration service and advertises only a
locator/digest hint. In abbreviated form (the concrete `ServiceProvider`
construction still comes from the deployment policy):

```python
from ndnsf_distributed_inference.app_sdk.coordinator import InferenceCoordinator

definition_ref = application.publish_definition(definition)

# In the deployment-owner process:
owner_client.deployments.resolve_definition(definition_ref)  # exact signed Data
coordinator = InferenceCoordinator(
    owner_client, definition_ref.coordinator_service)
coordinator.register(service_provider)
owner_client.deployments.advertise(
    service_provider, definition_ref, lifetime_seconds=30)
service_provider.run()
```

`deployments.advertise` publishes no authority: it serializes only record
names, digests, identities, service, and expiry. Discovery becomes authoritative
only after exact-name Data fetch, NDN trust validation, embedded Application
signature validation, lifecycle-owner activation validation, and revocation
fencing. The definition publisher must remain running while that exact Data is
being served.

## 3. Optional Explicit Prewarming

```python
deployment = application.deploy(definition)
deployment.wait_until_active(timeout=timedelta(minutes=5))
request = application.request(
    deployment,
    input={"prompt": "Reuse the warm deployment."},
    timeout=timedelta(seconds=30),
)
result = request.result()
```

This is optional. `deploy` uses the same ensure-deployment coordinator, status,
receipts, fencing, abort, and recovery logic as request-triggered preparation.
It exists to reduce future latency or support operations, not because requests
require a separate deployment phase.

## 4. Replace One Optimization Policy

```python
from ndnsf_distributed_inference.sdk import OptimizationSuite

suite = (
    OptimizationSuite.defaults()
    .replace(provider_assignment=MyProviderAssignmentPolicy())
    .build(name="research-team", version="1")
)

# Configure this on the authorized deployment coordinator, not on a remote
# requester. The coordinator's sole InferenceClient owns policy execution.
owner_client = InferenceClient.from_config(
    "ndnsf-di.yaml",
    state_root="/var/lib/ndnsf-di/deployment-owner",
    envelope_key_file="/run/secrets/ndnsf-di-envelope-key",
    optimization=suite,
)
```

All other policies remain versioned defaults. Runner adapters are configured
separately on Providers because execution mechanics are not a selection policy
and a requester/Application composition root must not gain Runner authority.

## 5. Serve a Model

```python
from ndnsf_distributed_inference.api import InferenceProvider

provider = InferenceProvider.from_config(
    "ndnsf-di.yaml",
    state_root="/var/lib/ndnsf-di/provider",
)
provider.serve(
    "/LLM/Qwen/Generate",
    QwenRunnerAdapter(model_root="/project/models/qwen"),
    capabilities=my_capabilities,
)
provider.run()
```

Deployment lifecycle actions and signed receipts use a separately constructed
advanced `ProviderAdminPort`; model-serving code does not receive that object or
gain its authority by registering a runner.

## 6. Recover a Durable Request

```python
from ndnsf_distributed_inference.api import RequestRef

saved_ref = request.ref.to_json()
# after restart
request = client.requests.get(RequestRef.from_json(saved_ref))
result = request.result(wait_timeout=timedelta(seconds=10))
```

The reference restores identity, not authority; the client revalidates journal
and deployment evidence before returning the bound handle.
