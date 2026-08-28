# Public Planning API Contract

## Contract boundary

The canonical public extension is one joint strategy. Existing low-level
partition and provider-assignment policy ports may be adapted for compatibility
but are not the authoritative application path.

The strategy is invoked inside one existing generic NDNSF distributed
collaboration invocation. It does not call a raw Request API and it does not
create a second DI collaboration protocol. The carrier contract is
[ndnsf-collaboration-carrier.md](ndnsf-collaboration-carrier.md).

```python
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Mapping


class ModelPlacementStrategy(ABC):
    """Operator-trusted code returning a data-only placement proposal."""

    name: str
    version: str
    state_digest: str
    deterministic: bool = True

    @abstractmethod
    def plan(self, request: "PlacementRequest") -> "PlacementDecision":
        ...


@dataclass(frozen=True)
class PlacementRequest:
    request_id: str
    attempt: int
    deadline_ms: int
    model: "ModelDescriptor"
    # Immutable dependency graph produced by the selected model adapter.
    graph: "ModelGraphSnapshot"
    candidates: tuple["SplitCandidate", ...]
    # One sanitized, validated planning view per ACK in the closed ACK set.
    # Each view exposes capacity, queue, RTT/bandwidth, and exact cache state.
    providers: tuple["ProviderPlanningView", ...]
    network: "NetworkSnapshot"
    catalog: "PreSplitCatalogSnapshot"
    split_constraints: "SplitConstraints"
    runtime_estimates: "RuntimeEstimateSnapshot"
    objective: "OptimizationObjective"
    budget: "CandidateBudget"
    task: "InferenceTaskDescriptor"
    state_contracts: tuple["InferenceStateContract", ...]


@dataclass(frozen=True)
class PlacementDecision:
    split_id: str
    split_digest: str
    assignments: tuple["ProviderAssignment", ...]
    fallback_order: Mapping[str, tuple[str, ...]]
    input_digest: str
    evidence_digest: str
    artifact_preparation: "ArtifactPreparationMode"
    evidence: Mapping[str, object]
```

`ArtifactPreparationMode.PRE_SPLIT` asks the trusted coordinator to resolve and
revalidate an existing publication. `ArtifactPreparationMode.GENERATED` asks
it to materialize and publish the selected candidate. The strategy never
receives either side-effecting object.

For ONNX, the registered adapter builds `ModelGraphSnapshot` from the actual
ONNX graph before strategy invocation:

```python
@dataclass(frozen=True)
class ModelGraphSnapshot:
    graph_digest: str
    adapter: "AdapterDescriptor"
    nodes: tuple["GraphNodeView", ...]
    edges: tuple["TensorEdgeView", ...]
    topological_order: tuple[str, ...]
    legal_cut_edges: tuple[str, ...]
    model_inputs: tuple["TensorContract", ...]
    model_outputs: tuple["TensorContract", ...]


@dataclass(frozen=True)
class TensorEdgeView:
    edge_id: str
    producer: str
    consumers: tuple[str, ...]
    dtype: str
    shape: tuple[int | str, ...]
    estimated_bytes: int | None
```

The snapshot contains dependency structure and bounded metadata, not model
weights. Its digest is bound to `ModelDescriptor.graph_digest`. A generated
`SplitSpecification` must partition graph nodes exactly once, cut only declared
legal edges, preserve topological dependencies and model I/O semantics, and
name every cross-partition tensor edge. The strategy therefore decides the
split from both the dependency graph and the closed ACK-derived Provider views.

`ProviderPlanningView` is derived only after ACK closure. It contains no token
or mutable handle, but gives the strategy the information needed to make the
joint decision:

```python
@dataclass(frozen=True)
class ProviderPlanningView:
    provider: str
    boot_epoch: str
    offer_digest: str
    expires_at_ms: int
    backends: tuple[str, ...]
    usable_gpu_memory_mb: int
    queue_depth: int | None
    estimated_wait_ms: float | None
    rtt_ms: float | None
    bandwidth_mbps: float | None
    cached_shards: tuple["ShardResidencyView", ...]  # GPU/RAM/disk/repository
    reusable_state: tuple["ReusableStateView", ...]  # opaque, bounded evidence
```

Every value records its source, capture time, units, and confidence/freshness
horizon. ACK-carried facts and coordinator-measured network observations remain
distinguishable; neither is silently treated as a timeless truth. A strategy
must jointly choose the split and Provider roles from these views, rather than
selecting a split independently of the Providers that answered.

An in-process strategy implementation is a digest-pinned, allowlisted local
extension and part of the host TCB. The narrow API minimizes authority and its
output is treated as untrusted, but the Python ABC is not a sandbox. The
canonical path does not accept a remotely selected implementation. Hostile code
would require a separate process/OS isolation contract outside this feature.

## Splitter contract

Model-specific splitters normalize graph information and candidates; they do
not control NDNSF.

They are resolved through the composed, versioned `ModelFamilyAdapter` contract
in [model-adapter-state-cache.md](model-adapter-state-cache.md). The graph,
split, task-I/O, state, and runner ports are independent. A model family that
cannot expose safe internal dependencies returns one opaque graph node with no
legal cut edge; it remains runnable but is not falsely declared splittable.

```python
class ModelSplitter(ABC):
    name: str
    version: str
    state_digest: str

    @abstractmethod
    def analyze(self, model: "ModelDescriptor") -> "ModelGraphSnapshot":
        ...

    @abstractmethod
    def candidates(
        self,
        graph: "ModelGraphSnapshot",
        constraints: "SplitConstraints",
        budget: "CandidateBudget",
    ) -> tuple["SplitCandidate", ...]:
        ...


class SplitMaterializer(Protocol):
    """Trusted side-effecting port, never passed to placement strategies."""

    def materialize(
        self,
        candidate: "SplitCandidate",
        *,
        deadline_ms: int,
    ) -> "MaterializedSplit":
        ...


class DistributedArtifactPublisher(Protocol):
    """Trusted NDNSF-DistributedRepo publication port."""

    def publish(
        self,
        candidate: "SplitCandidate",
        materialized: "MaterializedSplit",
        *,
        deadline_ms: int,
    ) -> "PublishedSplit":
        ...

    def resolve_existing(
        self,
        candidate: "SplitCandidate",
        *,
        deadline_ms: int,
    ) -> "PublishedSplit":
        ...
```

`SplitCandidate` uses one common representation for ONNX, PyTorch, container,
LLM, and future adapters. ONNX adapters derive tensor dependencies from the
graph. Other adapters must provide equivalent graph and artifact evidence.

## Default strategy

```python
class PreSplitFirstStrategy(ModelPlacementStrategy):
    def __init__(
        self,
        *,
        allow_dynamic_split: bool = True,
        splitter_id: str | None = None,
        cost_model: "PlacementCostModel | None" = None,
    ) -> None:
        ...
```

Default behavior:

1. Reject candidates with model, semantics, graph, backend, artifact, resource,
   freshness, or provider-role mismatches.
2. Prefer feasible exact pre-split candidates. For equivalent assignments,
   rank only fresh, exact-compatible residency as:
3. Rank feasible bindings by:

   ```text
   GPU-resident with a valid reuse pin/lease
     > unpinned GPU-resident with a separately feasible reload path
     > host-RAM-resident
     > local-disk-resident
     > repository-only
     > new materialization
     > estimated preparation + execution + critical-path transfer cost
     > canonical candidate/assignment digest tie-break
   ```

4. If no feasible pre-split candidate exists:
   - by default, use the configured named model-specific splitter and the
     closed ACK snapshot to derive a data-only `SplitSpecification`;
   - balance graph-valid contiguous units by estimated runtime peak, including
     weights, activations, KV/workspace, transient overhead, and safety margin;
   - for homogeneous units and equal usable GPU envelopes, use a deterministic
     near-equal partition;
   - return `NO_FEASIBLE_SPLIT` if the adapter cannot provide a safe bound or
     no assignment fits.

The strategy chooses the split but performs no side effect. It never invokes an
implicit universal splitter and never equates serialized file bytes with GPU
runtime peak. The trusted coordinator materializes and publishes the returned
specification only after independent validation.

## Application integration

The existing high-level application API remains authoritative. Planning is
configured at the Application-authorized coordinator, not by ordinary remote
requesters.

```python
@dataclass(frozen=True)
class ModelRef:
    model_name: str
    content_digest: str
    semantics_digest: str
    source_revision: str | None = None


class InferenceApplication:
    @classmethod
    def from_config(
        cls,
        config,
        *,
        state_root=None,
        placement_strategy: ModelPlacementStrategy | None = None,
        # existing security, model-adapter, and envelope configuration remains
    ) -> "InferenceApplication":
        ...

    def request(
        self,
        *,
        model: "ModelRef",
        task: "InferenceTaskRef",
        input: "ApplicationInput",
        options: "TaskOptions | None" = None,
        timeout_ms: int,
        objective: "OptimizationObjective | None" = None,
        constraints: "InferenceConstraints | None" = None,
    ) -> "InvocationHandle":
        ...

    @property
    def advanced(self) -> "ApplicationAdmin":
        ...


class ApplicationAdmin:
    @property
    def pre_splits(self) -> "PreSplitOps":
        ...
```

Internally, application-facing `APPClient.request()` forwards to the implemented
`AutomaticPlanningCoordinator.request()`, which SHALL use the generic NDNSF
deferred-collaboration carrier. The deployment-taking
`InferenceApplication.request(...)` is compatibility-only:

```python
invocation = service_user.begin_collaboration(
    service_name,
    encode_di_request(model, input, objective, constraints),
    mode="DEFERRED",
    ack_timeout_ms=ack_timeout_ms,
    timeout_ms=timeout_ms,
    request_id=request_id,
)

ack_closed = await invocation.acks_closed()
placement_request = coordinator.build_placement_request(ack_closed)
decision = strategy.plan(placement_request)
sealed_plan = await coordinator.validate_materialize_and_seal(decision)

await invocation.commit_plan(
    ack_closed_digest=ack_closed.digest,
    roles=sealed_plan.ndnsf_roles(),
    dependencies=sealed_plan.ndnsf_dependencies(),
    key_scopes=sealed_plan.ndnsf_key_scopes(),
    provider_assignments=sealed_plan.providers_by_role(),
    artifact_data_names=sealed_plan.artifact_data_names(),
    scope_key_data_names=sealed_plan.scope_key_data_names(),
    assignment_payloads=sealed_plan.opaque_assignments_by_provider(),
)
```

This is internal framework composition, not a second application call.
`begin_collaboration`, `acks_closed`, and `commit_plan` are generic NDNSF
operations and interpret no model/GPU fields. Planning and materialization run
outside the Face event-loop callback while the original deadline continues.

The existing:

```python
service_user.request_collaboration(
    service,
    payload,
    roles=roles,
    dependencies=dependencies,
    ...
)
```

remains the `PREPLANNED` compatibility form for fixed/deployed plans. The
default Spec 163 path SHALL NOT call it with precomputed or placeholder roles;
it uses `DEFERRED` because roles and dependencies are graph/ACK-derived.

The ordering is strict. Local task/input/schema and digest-pin validation may
run before publication so malformed application calls fail without a wire
side-effect. Model-family graph inspection, dependency analysis, candidate
enumeration, split selection, materialization, and Repository publication MUST
not run before the generic Request has been published and its immutable
`ACK_CLOSED` snapshot has been obtained. This is what lets the strategy use the
actual Provider offer (capacity, cache residency, RTT, bandwidth, and queue
state) rather than a deployment-time guess. A request with no valid ACKs fails
without performing model splitting or publication.

`model_name` is human-readable discovery, logging, and policy metadata.
`content_digest` is the authoritative SHA-256 digest of the canonical signed
model artifact manifest, which binds every weight/graph/config object and its
digest. `semantics_digest` binds tokenizer, preprocessing, generation defaults,
and output interpretation. `source_revision` is optional provenance and is not
accepted as a substitute for either digest. Exact cache/pre-split reuse requires
the name, content digest, semantics digest, graph digest, backend, precision,
and runtime compatibility to match.

The canonical configuration file is explicit and contains local identity,
security, repository, planning-adapter, and cache policy—not a precomputed
deployment:

```yaml
# app.yaml -- proposed NDNSF-DI application configuration V2
schema: ndnsf-di-application/v2
identity:
  requester: /ndnsf/di/user
  controller: /ndnsf/controller
  trust_schema: config/ndnsf-di-trust.conf
state:
  root: state
repository:
  prefix: /ndnsf/di/models
planning:
  strategy:
    name: pre-split-first
    version: "2"
    digest: sha256:<strategy-code-and-state>
  adapters:
    allow:
      - name: onnx
        digest: sha256:<adapter-code-and-state>
      - name: qwen-text-generation
        digest: sha256:<adapter-code-and-state>
  default_adapter: onnx
  gpu_safety_margin: 0.15
cache:
  immutable_model_shards:
    retain_verified: true
    selected_and_inflight_evictable: false
  derived_state:
    default: destroy
    exact_prefix_local_reuse: true
    cross_tenant_reuse: false
    migration: disabled
```

The machine-readable schema and complete configuration example are
`contracts/app-config.schema.json` and `contracts/app.example.yaml`.

Secrets, private keys, mutable device handles, model identity, Provider lists,
and role assignments are not embedded in `app.yaml`. Identity keys remain in
the configured NDNSF keychain. A locally installed external research strategy
may replace `planning.strategy`, subject to the digest-pin/allowlist rules.
Such a strategy is operator-trusted local code. The bounded in-process
executor stops waiting for a late result and prevents that result from
authorizing the current request; it is neither a security sandbox nor a
hard-preemption boundary. Every returned field is therefore revalidated as
untrusted data before trusted NDNSF-DI code creates names, artifacts, security
bindings, or a collaboration plan. The old partition/provider ports and
`APPClient.decide(requests)` are available only through
`LegacyPlacementCompatibilityAdapter` as non-authoritative migration hints.

`PreSplitOps` is an optional operator optimization API. A normal inference call
does not register or select a pre-split manifest.

The canonical V2 application call accepts the model itself by immutable
reference, plus input and optional objectives/constraints:

```python
handle = application.request(
    model=ModelRef(
        model_name="Qwen/Qwen3-0.6B",
        content_digest="sha256:<canonical-model-manifest>",
        semantics_digest="sha256:<tokenizer-config-preprocessing>",
    ),
    task=InferenceTaskRef("text-generation"),
    input=ApplicationInput.text("Explain named-data networking."),
    options=GenerationOptions(max_new_tokens=64, temperature=0.0),
    objective=OptimizationObjective.LOWEST_END_TO_END_LATENCY,
    timeout_ms=120_000,
)
```

For a complete generation, the runtime exposes the same surface as an
explicit durable value contract:

```python
generation = GenerationRequest(
    model=model_ref,
    task=task_ref,
    input=validated_full_prompt_input,
    options=generation_options,
    timeout_ms=120_000,
)
handle = app_client.generate(generation)
answer = handle.result()
```

`generate()` performs exactly one `begin_collaboration`, one immutable
`ACK_CLOSED`, one post-closure plan commit, and one final complete Response.
The full input token sequence is sent once. Autoregressive token/KV exchange
is internal data-plane work and must not reopen ACK collection, run placement,
or run Selection per output token. This whole-generation granularity is
intentional: it amortizes discovery, authentication, planning, artifact
preparation, and distributed invocation overhead over the complete answer. A
future presentation layer may render the finished answer incrementally, but
that is not a second NDNSF request or a per-token collaboration.
If the complete payload exceeds one NDN Data packet, the transport may use
ordinary bounded NDN segmentation/reassembly under the same response lineage;
segments are transport fragments, not token-level Responses or new
collaborations.

There is no required `deployment`, precomputed role graph, split manifest, or
provider list in the application request.
`AutomaticPlanningCoordinator.request()` is the implemented model/task-first
surface and returns one durable invocation handle. The older
`InferenceApplication.request(deployment, ...)` remains only as an explicit
preplanned compatibility surface. The
coordinator discovers Providers, closes the ACK set, constructs
`ProviderPlanningView` values, obtains the active immutable pre-split tuple
from its read-only `catalog_snapshot_provider`, and only then calls the
configured strategy.
No public `prepare_then_request()` or second inference call is added. The
trusted NDNSF-DI coordinator automatically performs each bounded wire attempt
as:

```text
publishes one inference Request and collects one ACK set
-> constructs validated snapshots
-> resolves the digest-pinned model-family adapter and validates task, input,
   and options contracts (the only pre-publication local validation)
-> after ACK_CLOSED, validates model, result, graph/split, and inference-state
   contracts and enumerates graph-valid candidates
-> invokes the configured placement strategy
-> validates the decision's selected candidate and ArtifactPreparationMode
-> for PRE_SPLIT, resolves and revalidates an existing publication; for
   GENERATED, invokes the trusted SplitMaterializer and
   publishes one signed, immutable, content-addressed manifest through
   NDNSF-DistributedRepo; partial staging is not selectable
-> revalidates the published manifest and seals the plan
-> creates at most one logical exact-target final-Selection identity per
   selected Provider under the same request/attempt, containing that Provider's
   complete role tuple inside the capability/aggregate-GPU-RAM envelope of its
   validated DI offer carried by the positive generic ACK
-> NDNSF Core authenticates the generic target, token, lease, deadline, replay,
   and exact opaque payload digest without parsing DI fields
-> registered NDNSF-DI participant purely validates the offer/tuple and returns
   one canonical opaque commit blob plus acceptance payload
-> Core-owned GenericSelectionTxnStore fsyncs one COMMITTED WAL record that
   atomically consumes the token, commits opaque-lease disposition, and stores
   those encrypted opaque bytes/digests without parsing them
-> NDNSF-DI idempotently projects the GPU ledger, complete tuple, grants, and
   latches from that record and exposes the mandatory acceptance evidence
-> each selected Provider revalidates an exact GPU-resident shard or fetches
   its immutable shard by NDN, verifies and persists it, then loads RAM/GPU
   asynchronously while retaining authenticated inputs; when the plan contains
   an exact authorized derived-state binding, the Provider also revalidates and
   pins that local state immediately before use
-> starts each role when Selection + local readiness + inputs are all true
-> validates the complete ResultContract and uses first-terminal-wins to
   complete the same invocation handle with one accepted inference Response
```

Replanning that changes an assignment creates a new attempt with fresh ACKs,
ProviderTokens, leases, plan, and Selection identities. The invocation retains
its original deadline and bounded attempt/compensation budget. A terminal
attempt never returns to active.

The total request deadline bounds planning, materialization/publication when
needed,
Selection, role-local preparation, and execution. ACK timeout only closes discovery. An
optional operator/application prewarm operation remains an optimization; the
normal `request()` path still revalidates the exact request-scoped DI offer
carried by the positive generic ACK and performs role-local preparation
internally after Selection when needed. Prewarm or cache residency may make
post-Selection verification and
preparation immediate, but it does not satisfy request-specific readiness or
grant assignment authority before Selection. Neither prewarm nor a complete
local-ready set is required before final Selection.
Successfully verified immutable model/runtime shards may remain in bounded
Provider disk/RAM/GPU caches after terminal convergence. Request inputs,
outputs, activations, plaintext grants, temporary decrypted buffers, and all
mutable state lacking an explicit reusable `InferenceStateContract` may not.
Exact prefix KV is an adapter-specific `EXACT_PREFIX_REUSABLE` profile: it may
remain Provider-local only under exact identity, authorization, security-domain,
TTL, budget, epoch, eviction, and revalidation rules. It is never entered into
the immutable pre-split catalog. A later ACK advertises only bounded opaque
state evidence as a fresh signed offer; neither the catalog nor strategy treats
it as timeless proof. Optional encrypted migration is disabled in the baseline.
There is no `PreparationCommit`, preparation token, commit receipt, second
Provider role-negotiation decision, or second ACK round. The mandatory
Selection-acceptance record is evidence of the first Selection transaction.

The existing `optimization=OptimizationSuite` argument and
`APPClient.decide(requests)` may remain behind a named compatibility adapter.
They are not used by the new canonical lifecycle.

## Strategy validation

Trusted NDNSF-DI validation MUST reject a decision when:

- `input_digest` does not equal the canonical request digest;
- the strategy identity/state differs from configuration;
- the selected existing manifest was not supplied, or the generated
  `SplitSpecification` is not derivable within the named splitter and budget;
- a role is missing, duplicated, or assigned more than once;
- an assigned Provider lacks a fresh validated DI offer carried by a positive
  generic ACK for this request and attempt;
- a selected Provider would receive zero, multiple, incomplete, or incremental
  positive Selection bundles rather than one complete role tuple;
- the Provider boot epoch, backend, artifact availability, or deadline is
  incompatible;
- a claimed cache hit mismatches model name, content/semantics digest, artifact or manifest
  digest, layer/graph range, backend, precision/runtime ABI, trust policy,
  device, cache epoch, freshness, or expiry;
- feasibility depends solely on an unpinned cache entry with no valid reload
  path, or resident capacity is double-counted;
- the aggregate estimated peak GPU RAM of roles assigned to a Provider exceeds
  the GPU RAM offered or reserved by that Provider's ACK;
- the selected graph is cyclic, incomplete, or changes model semantics;
- fallback providers are unknown or violate hard constraints;
- candidate, assignment, or policy budgets are exceeded;
- any returned field contains a secret, token, raw payload, writable path,
  callback, device handle, or network handle.

## External implementation example

```python
class LowestCriticalPathStrategy(ModelPlacementStrategy):
    name = "example.lowest-critical-path"
    version = "1"
    state_digest = "sha256:..."

    def plan(self, request: PlacementRequest) -> PlacementDecision:
        # request.providers comes from this attempt's closed ACK set.
        feasible = [
            p for p in request.providers
            if offer_is_fresh_and_compatible(p, request)
        ]
        # Jointly evaluate exact GPU/RAM/disk cache hits, RTT, bandwidth,
        # queue/wait estimates, runtime peak, transfer cost, and role mapping.
        split, assignments = choose_joint_split_and_roles(
            model=request.model,
            graph=request.graph,
            candidates=request.candidates,
            providers=feasible,
            network=request.network,
            runtime_estimates=request.runtime_estimates,
        )
        # Trusted NDNSF-DI validation remains authoritative.
        return PlacementDecision(
            split_id=split.candidate_digest,
            split_digest=split.candidate_digest,
            assignments=assignments,
            fallback_order={},
            input_digest=canonical_digest(request),
            evidence_digest=canonical_digest(
                explain_costs(split, assignments)),
            artifact_preparation=(
                ArtifactPreparationMode.PRE_SPLIT
                if split.is_active_catalog_entry
                else ArtifactPreparationMode.GENERATED),
            evidence=explain_costs(split, assignments),
        )


application = InferenceApplication.from_config(
    "app.yaml",
    state_root="state",
    placement_strategy=LowestCriticalPathStrategy(),
)
```

The strategy API never supplies service-message names, artifacts writers,
device/network handles, or tokens, and a conforming implementation performs
none of those actions. Because an in-process class is operator-trusted rather
than sandboxed, protocol safety relies on independent output validation and on
keeping all Selection authority in trusted NDNSF-DI code.
