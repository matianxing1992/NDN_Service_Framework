# Contract: Deployment, Discovery, and Request Handles

## Primary Request-Time Lifecycle

```python
request = application.request(
    definition,
    input=value,
    timeout=timedelta(minutes=6),
)
```

The facade executes this existing authority chain:

```text
validate definition
  -> verify Application creator signature and authorization
  -> run bounded decision graph
  -> resolve immutable plan revision (model/graph/roles/bounded candidates)
  -> publish normal NDNSF request
  -> collect typed ACK deployment offers
  -> Selection assigns Provider responsibilities and carries the existing
     responsibility/membership/lease execution certificate
  -> validate that certificate, then prepare selected Providers
  -> expose per-role progress and authoritative status snapshots
  -> verify exact signed readiness receipts and all-role readiness barrier
  -> execute inference
  -> publish final inference Response
```

Any failure enters the existing abort/recovery path. The facade cannot fabricate
receipts, treat ACK or Selection as readiness, or mark a revision active
directly. The request deadline includes both preparation and execution.

`DeploymentDefinition` is created and signed only by
`InferenceApplication.define(...)`. APPDeployment receives it as immutable
intent. It cannot select new allowed models, change hard constraints, replace
the OptimizationSuite, or author concrete graph/role/Provider/execution-target facts;
those facts are validated policy outputs bound into the revision.
The canonical signed definition Data name is:

```text
/<application>/NDNSF/DI/DEFINITION/<deployment-id>/<definition-digest>
```

The Application publishes those exact signed bytes through existing NDN Data;
the later activation record references the name and digest.

## ACK, Selection, Preparation, and Response Semantics

The normal NDNSF message types retain one meaning:

| Existing message | NDNSF-DI meaning |
|---|---|
| Request | Inference input plus exact signed requestable deployment and bounded preparation intent |
| ACK | Willingness/capability plus typed multi-role `ProviderDeploymentOffers`; not readiness authority |
| Selection | Responsibility/role assignment plus request-attempt/lease/fence binding |
| Response | Final inference result only |

Each role offer reports `READY`, `NEEDS_PREPARATION`, or `UNAVAILABLE`; one ACK
may carry multiple role offers. A READY offer is useful to a selection policy
but remains advisory. After Selection the
coordinator revalidates `ProviderReadiness` for the exact revision, artifacts,
adapter, role, and Provider boot epoch. A NEEDS_PREPARATION Provider progresses
through ACCEPTED, FETCHING, VERIFYING, LOADING, WARMING, and READY. Selection
carries the existing execution certificate that commits membership,
responsibilities, leases, and fences before preparation; it is not a readiness
assertion. Only a complete set of valid role readiness receipts and the
all-role barrier permit model-runner invocation.

NDNSF owns generic Collaboration operation-status inspection, not DI model
readiness. The existing signed `SELECTION-STATUS` reply is extended additively
with latest per-member `ServiceOperationStatus` entries; Provider reporting is
available before `CollaborationContext` construction and through the context
afterward. Requesters use one validated query/watch/wait binding, while optional
collaboration events only reduce observation latency. NDNSF-DI projects these
entries into `DeploymentProgress`/`DeploymentStatus` and retains all model,
artifact, adapter/GPU, exact `ProviderReadiness`, and all-role barrier
semantics. See [collaboration-operation-status.md](collaboration-operation-status.md).
Progress never substitutes for the final readiness receipt.

## Optional Explicit Prewarming

```python
deployment = application.deploy(definition)
deployment.wait_until_active(timeout=timedelta(minutes=5))
request = application.request(
    deployment,
    input=value,
    timeout=timedelta(seconds=30),
)
```

`deploy` is an optional latency/operations tool. It invokes the identical
ensure-deployment coordinator, policy evidence, status snapshots, receipts,
commit/abort, journal, and recovery logic used by cold `request`. Once complete,
it may publish the signed `DeploymentActivationRecord` below. `request` against
that ACTIVE handle revalidates exact readiness and may reuse it. No caller must
predeploy for correctness, and no second state machine is allowed.

## Bound Deployment Handle

```python
class DeploymentHandle:
    @property
    def handle_ref(self) -> DeploymentHandleRef: ...
    @property
    def ref(self) -> DeploymentRef: ...  # ACTIVE only; otherwise raises
    def status(self) -> DeploymentStatus: ...
    def refresh(self) -> DeploymentHandle: ...
    def wait_until_active(self, *, timeout: timedelta) -> DeploymentHandle: ...
```

`refresh` refreshes status/evidence for the same immutable revision; it never
silently follows a newer active revision. An application-authorized handle may
additionally reach rollback/drain/delete
through `application.advanced.deployments`; a requester-bound handle cannot.
`handle_ref` is durable in every lifecycle state. `.ref` is created only from a
validated ACTIVE activation record; PREPARING, FAILED, DRAINING, INACTIVE, and
DELETED handles cannot produce an invocable reference.

## Deployment Catalog

Applications may publish `definitionRecordName`, `definitionRecordDigest`,
service, and deployment ID for an ON_DEMAND deployment. Providers may additionally
place `activationRecordName`, `activationRecordDigest`, deployment ID, and
revision for ACTIVE deployments in existing NDNSD service metadata. These fields
are untrusted lookup hints. Canonical records are versioned APP data at:

```text
/<application>/NDNSF/DI/DEFINITION/<deployment-id>/<definition-digest>
/<deployment-owner>/NDNSF/DI/DEPLOYMENT/<deployment-id>/<revision>/ACTIVATION
```

The two producer prefixes intentionally differ: the Application signs and
serves immutable intent, while the independently credentialed deployment owner
signs and serves lifecycle activation/revocation evidence. Requiring an owner
to publish below the Application prefix would be unroutable without an unsafe
cross-identity signing/serving proxy.

The catalog fetches the exact digest-bound signed Data and verifies the exact
referenced Application-signed definition Data, deployment-owner
authorization/signature,
activation certificate, lifecycle epoch, ACTIVE state, freshness/expiry, and
supersedes/revocation chain. A raw NDNSD dictionary never becomes a
`DeploymentRef` or `DeploymentDefinitionRef` directly. These are APP-level
records over existing NDN Data, not a new NDNSF Request/Response mode or TLV
protocol.

```python
class DeploymentCatalog:
    def discover(
        self,
        *,
        service: str,
        model: ModelSelector | None = None,
        constraints: DiscoveryConstraints | None = None,
    ) -> tuple[DeploymentSummary, ...]: ...

    def get(
        self,
        ref: DeploymentDefinitionRef | DeploymentRef | DeploymentHandleRef,
    ) -> DeploymentHandle: ...
```

Discovery validates Application signature, authorization, digest, and freshness
for every ON_DEMAND definition. ACTIVE results additionally require lifecycle
owner authority, active state, revision, activation certificate, and rollover
fence. Ordering is deterministic for equal evidence. A summary's `deployment` is a
`DeploymentDefinitionRef` or `DeploymentRef`; `get` performs final binding.
An ON_DEMAND reference also binds the authorized deployment coordinator. The
remote client sends the frozen reference/input there and never resolves or
executes the Application's optimization profile in requester space.

## Request Creation

```python
request = client.request(
    deployment,
    input=value,
    timeout=timedelta(seconds=30),
    options=InferenceOptions(...),
)
```

Before publication the client freezes the signed definition or ACTIVE
deployment/revision evidence, requester identity, input digest/reference,
timing, and options into the durable request envelope. An ON_DEMAND deployment
resolves and freezes one immutable plan revision during PLANNING. The existing
execution-intent Prepare/Commit path creates the request-specific
responsibility/membership/lease certificate before Provider preparation. Exact
readiness is established afterward and separately gates model execution. The
client never silently follows a later definition or active revision.

## Bound Request Handle

```python
class InferenceRequestHandle:
    @property
    def ref(self) -> RequestRef: ...
    def status(self) -> RequestStatus: ...
    def deployment_status(self) -> DeploymentStatus: ...
    def wait(self, *, wait_timeout: timedelta | None = None) -> RequestStatus: ...
    def result(self, *, wait_timeout: timedelta | None = None) -> InferenceResult: ...
    async def result_async(
        self, *, wait_timeout: timedelta | None = None
    ) -> InferenceResult: ...
    def cancel(self, reason: str = "user-requested") -> CancellationResult: ...
    def events(self, *, after: EventCursor | None = None) -> Iterator[RequestEvent]: ...
```

`events()` is lifecycle observation. Future token/tensor streaming must use
`output_stream()` and its own ordering/backpressure contract.

`result()` returns `InferenceResult` only for successful completion. Failed,
cancelled, or timed-out requests raise a typed `InferenceError` carrying the
terminal status and structured evidence; callers can inspect `status()` without
raising.

`wait_timeout` bounds only the caller's local observation wait and never changes
the durable request deadline. `result_async` awaits the same journal/rendezvous
state machine and returns the same `InferenceResult`; it is not a second
request path.

During PREPARING, `deployment_status()` returns the authenticated aggregate and
per-role snapshot. It does not depend on having consumed every event. Its
monotonic coordinator epoch and per-role sequence numbers reject stale,
duplicated, or reordered updates. A completed request retains the final
preparation/readiness evidence for audit.

Cancellation is idempotent and request/attempt/revision fenced. Before
certification it aborts preparation, releases reservations, and schedules
bounded orphan cleanup. After certification it sends
bounded Provider cancellation, prevents new dependent work, and transitions to
`CANCELLED` only under the existing Core visibility rules. An already accepted
terminal result is not retroactively revoked, and a local waiting timeout is
not implicit cancellation.

## Recovery

```python
saved = request.ref.to_json()
# process restart
request = client.requests.get(RequestRef.from_json(saved))
result = request.result(wait_timeout=timedelta(seconds=10))
```

Deserialization validates schema/version/digests. Rebinding consults the
canonical journal and remote evidence; it does not recreate or resubmit a
request.

## Timing Validation

- `timeout` is a positive `timedelta` measured from request creation.
- `deadline` is a timezone-aware `datetime`.
- Both together, neither, numeric values, naive datetime, and past deadline are
  errors.
- Compatibility adapters may parse old numeric forms only while emitting a
  replacement warning; canonical code never uses magnitude heuristics.
