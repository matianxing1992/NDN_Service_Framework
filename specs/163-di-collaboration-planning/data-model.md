# Data Model: Pluggable DI Collaboration Planning

## ModelDescriptor

Immutable identity and semantics of the requested model.

| Field | Meaning | Validation |
|---|---|---|
| `model_name` | Stable human-readable model name | Non-empty, canonical |
| `content_digest` | Authoritative model identity | SHA-256 of the canonical signed artifact manifest |
| `semantics_digest` | Tokenizer/preprocessing/output semantics | Required for exact compatibility |
| `source_revision` | Optional upstream provenance | Never substitutes for a digest |
| `adapter` | Model-family adapter identity and version | Registered and allowlisted |
| `graph_digest` | Canonical model/tensor graph summary | Must match splitter input |
| `inputs`, `outputs` | Public tensor or value contracts | Complete and unambiguous |

The name makes the model understandable; the digests make it reusable and
unambiguous. Two references with the same name but different content or
semantics digests are different models. Cache/pre-split identity never relies
on a moving repository tag or name alone.

## ModelAdapterDescriptor and InferenceTaskDescriptor

`ModelAdapterDescriptor` identifies the composed model-family adapter and binds
its stable name, semantic version, implementation/state digest, ABI, supported
formats/backends/tasks/state profiles, and graph, split, input, options, result,
and state schema digests. The descriptor is allowlisted and digest-pinned.

`InferenceTaskDescriptor` identifies one adapter-owned task such as
`text-generation`, `object-detection`, `speech-recognition`, `diffusion`, or an
opaque application task.

| Field | Meaning | Validation |
|---|---|---|
| `task_id` | Stable task name | Supported by selected adapter |
| `input_schema_digest` | Canonical application-input schema | Size-bounded and exact |
| `options_schema_digest` | Adapter-owned task options | No framework-only LLM fields |
| `result_schema_digest` | Complete output interpretation | Bound to `ResultContract` |
| `adapter` | Model adapter descriptor digest | Exact and allowlisted |

The composed `ModelFamilyAdapter` has graph/split, task-I/O, state, and runner
ports. Only the runner is side-effecting and executable; pure adapter values do
not grant network, repository, Selection, or device authority.

## ModelGraphSnapshot

Immutable common dependency graph produced by the model-specific adapter. For
ONNX, it is derived from the actual ONNX graph before the placement strategy is
invoked.

| Field | Meaning | Validation |
|---|---|---|
| `graph_digest` | Canonical graph identity | Equals `ModelDescriptor.graph_digest` |
| `adapter` | Adapter name, version, state digest | Registered and allowlisted |
| `nodes` | Operators/layers and bounded runtime metadata | Unique IDs; complete coverage |
| `edges` | Producer-to-consumer tensor dependencies | Producer and every consumer exist |
| `topological_order` | Canonical execution order | Covers all nodes; graph is acyclic |
| `legal_cut_edges` | Adapter-approved partition boundaries | Subset of graph edges |
| `model_inputs`, `model_outputs` | End-to-end semantic boundary | Matches `ModelDescriptor` |

Each tensor edge records its ID, producer, consumers, dtype, symbolic or fixed
shape, and estimated transfer bytes when safely known. Unknown dynamic sizes
remain explicit. The snapshot contains no model weights or tensor payload.
An adapter that cannot safely expose internal dependencies emits one opaque
atomic node and no legal cut edge. This is a valid unsplittable graph, not an
excuse to invent dependencies.

## InferenceStateContract

Adapter-owned declaration of mutable inference state.

| Field | Meaning | Validation |
|---|---|---|
| `profile` | `STATELESS`, `REQUEST_SCOPED`, `SESSION_SCOPED`, `EXACT_PREFIX_REUSABLE`, or `CUSTOM_ADAPTER_MANAGED` | Known version |
| `identity_schema_digest` | Canonical exact-match inputs | Complete and adapter-bound |
| `owner_scope` | Invocation, session, role/layer range, Provider | No ambiguous ownership |
| `resource_estimator` | GPU/RAM/disk/transfer bytes | Bound or explicit unknown |
| `access_domain` | Tenant/session/security-domain rule | Cross-tenant deny by default |
| `retention` | TTL, tier, budget and terminal action | Destroy when absent |
| `epochs` | Boot/cache/entry generation rules | Fresh and monotonic |
| `pin_policy` | Reservation and revalidation | No ACK-only authority |
| `migration` | Disabled or explicit encrypted contract | Disabled by baseline |
| `cleanup` | Release, eviction and restart behavior | Bounded convergence |

Immutable model/runtime artifacts and semantic response caching are separate
classes and do not use this entity.

## DerivedStateCacheEntry and StateReuseBinding

`DerivedStateCacheEntry` is Provider-owned mutable cached inference state.
The LLM `EXACT_PREFIX_KV_V1` profile stores only an opaque
`state_key_digest` outside the Provider's protected cache and binds exact model
content/semantics, adapter/runner ABI and digests, split/manifest, role/layer
range, prefix-token digest, position/context parameters, precision/KV layout,
security domain, Provider boot/cache epoch, entry generation, bytes, tier, and
expiry.

`StateReuseBinding` is immutable plan and Selection data:

| Field | Meaning | Validation |
|---|---|---|
| `provider`, `role`, `layer_range` | Intended state consumer | Exact assignment |
| `state_key_digest`, `profile` | Exact opaque entry | Recomputed by trusted DI code |
| `entry_generation`, `epochs`, `expiry` | Freshness fence | Current immediately before use |
| `required_pin` | Reservation before execution | Atomically installed with Selection |
| `expected_bytes`, `avoided_work` | Cost evidence | Bounded and unit-tagged |
| `fallback` | `CLEAN_COMPUTE` or `REPLAN` | Sealed into plan |

ACK evidence is only an offer. `commit_plan` and final Selection bind the
accepted reuse decision; the Provider must revalidate before use.

## SplitCandidate and SplitSpecification

A side-effect-free possible model decomposition.

| Field | Meaning | Validation |
|---|---|---|
| `candidate_digest` | Content identity | Derived from all canonical fields |
| `source` | `PRE_SPLIT` or `GENERATED` | Exactly one |
| `splitter` | Name, version, state digest | Bound to candidate identity |
| `model` | Exact `ModelDescriptor` reference | No semantic substitution |
| `execution_plan` | Roles and dependency DAG | Acyclic and complete |
| `fragments_by_role` | Exact fragment keys or recipes | One entry per role |
| `artifacts_by_role` | Model/runtime references or recipes | Digest-bound |
| `requirements_by_role` | Backend and resource constraints | Typed hard constraints |
| `estimated_costs` | Transfer, preparation, execution estimates | Units and unknowns explicit |

A generated candidate is not proof that its artifacts exist. It becomes
eligible for preparation only after trusted materialization creates and
validates a `PreSplitManifest`.

`SplitSpecification` is the strategy-selected, data-only generated form. It
binds the same model and `ModelGraphSnapshot` digest, splitter, graph-valid
contiguous units, legal cut edges, every cross-partition tensor, role DAG,
provider assignments, artifact recipes, and runtime requirements, plus the
closed ACK/input digest. Its capacity evidence includes weights, activations,
KV/workspace, transient overhead, and safety margin per segment. It grants no
materialization or publication authority.

## PreSplitManifest

Immutable materialized split registered by an operator or trusted
materializer.

| Field | Meaning | Validation |
|---|---|---|
| `manifest_digest` | Content-addressed identity | Idempotent registration key |
| `candidate_digest` | Source `SplitCandidate` | Exact match |
| `model_digest` | Model content identity | Exact match |
| `graph_digest` | Model/tensor DAG identity | Exact match |
| `splitter_descriptor` | Splitter implementation evidence | Name, version, state digest |
| `execution_plan` | Canonical role DAG | Same as candidate |
| `fragments` | Role-to-fragment mapping | Exact role coverage |
| `artifacts` | Signed/content-addressed object references | Digest and size required |
| `requirements` | Backend/runtime/resources | One set per role |
| `lifecycle` | `ACTIVE`, `RETIRED`, or `REVOKED` | Only `ACTIVE` is selectable |

Provider residency is deliberately absent. It is dynamic provider state.

## ProviderPlanningView

Sanitized, validated planning view derived from exactly one Provider ACK in the
closed ACK set. It combines the signed DI offer with clearly sourced
ACK-exchange/network observations needed by the placement strategy.

| Field | Meaning | Validation |
|---|---|---|
| `profile` | NDNSF-DI payload contract | Exact signed `SELECTION_DATAFLOW_V2`; opaque to Core |
| `requester` | Requesting identity | Authorized and exact |
| `attempt` | Planning/execution generation | Exact live attempt |
| `provider` | Provider identity | Authorized for the service |
| `boot_epoch` | Current provider process epoch | Non-empty and fresh |
| `request_id` | Outer inference request | Exact match |
| `service` | Requested service | Exact match |
| `model_intent_digest` | Immutable model intent accepted by the Provider | Exact request binding |
| `captured_at_ms`, `expires_at_ms` | Freshness interval | Planning occurs inside interval |
| `accepted_deadline_ms` | Latest accepted end-to-end deadline | No later than request deadline |
| `resource_sequence` | Monotonic capacity snapshot generation | Fresh for this Provider epoch |
| `acceptance_predicate` | Version and digest of typed compatibility rules | Exact schema, units, role-count and safety-margin rules |
| `role_scope` | Assignment scope | `ANY_COMPATIBLE_ROLE` for this request |
| `backends` | Supported execution backends | Typed identifiers |
| `offered_gpu_memory_mb` | GPU RAM promised to this request | Positive, units explicit, and exclusive until terminal offer outcome |
| `resources` | Other capacity/resource envelopes | Units explicit |
| `queue` | Queue/readiness estimate | Optional but typed |
| `residency` | Fragment/artifact availability | Bound to boot epoch |
| `reusable_state` | Bounded opaque derived-state offers | Exact digest, access domain, epochs, expiry; no prompts/tokens |
| `rtt_ms` | Request/ACK path RTT | Measured source and horizon explicit |
| `bandwidth_mbps` | Advertised or separately measured usable bandwidth | Source, confidence, units, and horizon explicit |
| `network` | Other measured or estimated link properties | Source and horizon explicit |
| `reservation_digest` | Optional bounded capacity hold | Private proof excluded |
| `evidence_digest` | Validated ACK evidence | No token or secret fields exposed |

A generic `ACK.status=true` means that the registered service accepted the
request as a candidate and supplied an authenticated opaque payload. When that
payload validates as `DIProviderOfferV2`, NDNSF-DI gives it the stronger
meaning that, until expiry, the Provider is willing to accept any compatible
role tuple whose aggregate verified requirements fit this envelope. For the
baseline strategy, the aggregate estimated peak GPU RAM of roles assigned to a
Provider cannot exceed `offered_gpu_memory_mb`. The DI Provider must back the
promise with its exclusive local admission ledger, optionally bound to an
opaque Core lease; a volatile free-memory sample is insufficient. The snapshot
is not a list of preaccepted stage identities, and Core never interprets it.

The trusted coordinator retains the ACK's private control material separately
from this sanitized strategy view:

Each `residency` entry contains:

```text
model name + content/semantics digest + manifest/artifact digest
+ graph/layer range
backend + precision/runtime ABI + trust-policy digest + byte count
tier = GPU | HOST_RAM | LOCAL_DISK | REPOSITORY_ONLY
device (when applicable) + Provider boot epoch + cache epoch
observed-at + expiry + pin/lease expiry + evictable/reloadable state
```

GPU/RAM state is invalid after a Provider restart. Disk state is usable only
after renewed verification. A hard plan may depend on a resident entry only
while a pin protects it through Selection expiry or while a separately
feasible reload path exists.

## ProviderAckOffer

Provider-authenticated, request-scoped positive-ACK authority retained by the
trusted coordinator.

| Field | Meaning | Validation |
|---|---|---|
| `profile`, `requester`, `request_id`, `attempt` | V2 DI payload identity | Exact live request and signed `SELECTION_DATAFLOW_V2` |
| `service`, `model_intent_digest` | Accepted operation/model | Exact request binding |
| `provider`, `boot_epoch` | Issuing Provider process | Same as validated ACK |
| `captured_at_ms`, `expires_at_ms`, `accepted_deadline_ms` | Bounded authority interval | Fresh and inside request deadline |
| `resource_sequence` | Capacity generation | Same as sanitized snapshot |
| `capability_resource_digest`, `acceptance_predicate_digest` | Canonical DI compatibility and GPU-RAM envelope | Same fields exposed in sanitized form |
| `status_handle` | Opaque signed status query handle | Fresh and requester-bound |
| `provider_token` | Existing one-time final Selection authorization | Private; never enters strategy/evidence |
| `admission_lease` | Optional resource hold and binding proof | Same GPU budget as sanitized offer |
| `ack_digest` | Signature-covered canonical ACK-envelope identity | Covers every field above and exact snapshot binding |

Ordinary evidence logs never persist raw tokens, lease proofs, or plaintext
keys. The durable coordinator may retain the exact signed Selection wire object
and necessary token/lease/key-grant ciphertext only as encrypted-at-rest,
identity-scoped ephemeral control state until expiry, so a crash can retransmit
identical bytes. Evidence retains only hashes or identifiers, issuer, binding
digest, and terminal consumption state. If protected control state is
unrecoverable, the same attempt cannot synthesize a replacement Selection.

## PlacementRequest

Canonical, immutable input to one strategy invocation.

```text
request/attempt/deadline
  + ModelDescriptor
  + ModelGraphSnapshot
  + tuple<SplitCandidate>
  + tuple<ProviderPlanningView>
  + PreSplitCatalogSnapshot
  + SplitConstraints
  + RuntimeEstimateSnapshot
  + NetworkSnapshot
  + OptimizationObjective
  + CandidateBudget
  + strategy state digest
```

The request contains no raw prompt, tensor content, token, private key,
writable path, network client, artifact-store writer, or device handle.

## PlacementDecision

The external strategy's data-only result.

| Field | Meaning | Validation |
|---|---|---|
| `split_id`, `split_digest` | Selected candidate and its canonical identity | Must name one candidate in the request snapshot |
| `assignments` | One provider assignment per role | Complete, unique, feasible |
| `fallback_order` | Optional declared alternatives | Providers must be in snapshot |
| `artifact_preparation` | `PRE_SPLIT` or `GENERATED` trusted-coordinator action | Enum only; exact catalog reuse must be revalidated |
| `evidence`, `evidence_digest` | Objective values and explanation | Canonical, bounded, digest-consistent |
| `input_digest` | Exact `PlacementRequest` identity | Must match |

The decision does not contain invocation tokens, arbitrary NDN names,
executable callbacks, artifact bytes, or Selection messages.

## DeferredCollaborationInvocation

Application-independent NDNSF carrier for one distributed collaboration. Core
persists and transports these fields without parsing DI payloads.

| Field | Meaning | Validation |
|---|---|---|
| `request_id`, `attempt`, `service` | Generic invocation identity | Unique live pending request |
| `mode` | `PREPLANNED` or `DEFERRED` | Immutable after begin |
| `request_payload_digest` | Opaque application request identity | Exact published Request |
| `ack_deadline_ms`, `total_deadline_ms` | Bounded closure and lifetime | ACK deadline no later than total |
| `ack_closed_digest` | Canonical immutable ACK set | Set once at `ACK_CLOSED` |
| `plan_commit_digest` | Generic roles/dependencies/assignments/artifact-name/opaque-payload identity | Empty before commit; one value afterward |
| `commit_state` | `NOT_ALLOWED`, `OPEN`, `COMMITTING`, `COMMITTED`, or terminal rejection | Monotonic |
| `selection_delivery` | Existing per-Provider generic Selection state | Driven only after commit |
| `terminal_state` | Existing response/timeout/cancel outcome | First terminal wins |

`PREPLANNED` stores generic roles and dependencies before Request, then projects
them through the same commit transition after ACK closure. `DEFERRED` exposes
the immutable ACK snapshot to NDNSF-DI and accepts its generic plan projection
after DI planning and any trusted publication complete. A byte-identical
commit retry returns the same result; a second or conflicting commit fails.

## DICollaborationPlan

Trusted NDNSF-DI output assembled after validating the decision and, when
necessary, materializing and publishing the selected split through
NDNSF-DistributedRepo.

```text
model and semantics
split manifest
provider assignments and boot epochs
role DAG and tensor schemas
model/runtime artifact references
canonical intermediate-object naming template
predictable prefetch references
per-role Selection and preparation deadlines
execution deadline and order
fallback/compensation policy
exactly one ResultContract and complete required sink set
plan digest and signer
```

This entity is not the same type as Core's existing generic
`CollaborationPlan`. At `commit_plan`, NDNSF-DI projects only generic role
specifications, dependency edges, key scopes, Provider assignments, artifact
Data names, and opaque recipient assignment bytes into that Core type. Model,
GPU, shard, tensor, preparation, result, and compensation semantics remain in
the opaque NDNSF-DI payload and DI-owned journal.

## ProviderSelectionAssignment

The recipient-specific projection of the sealed plan carried directly by one
final Selection to one Provider. It atomically assigns the complete non-empty
tuple of roles allocated to that Provider and starts bounded, request-scoped,
role-local asynchronous preparation while allowing eligible prewarm/cache
reuse. A one-role assignment is a tuple of length one.

| Field | Meaning | Validation |
|---|---|---|
| `profile`, `requester`, `service` | V2 DI operation identity | Exact signed offer/request binding; opaque to Core |
| `request_id`, `attempt` | Original inference identity | Exact live pending request |
| `model_intent_digest`, `resource_sequence` | Accepted model and capacity generation | Exact ACK binding |
| `plan_digest`, `input_snapshot_digest` | Sealed decision and source state | Exact trusted values |
| `ack_digest` | Positive ACK and offered envelope | Fresh and exact |
| `provider`, `boot_epoch`, `role_assignments` | Concrete target and complete role tuple | Every role unique; exact ACK and plan binding |
| `artifacts`, `backend`, `dependencies` | Provider-local plan projection | Complete for every assigned role |
| `aggregate_required_gpu_memory_mb` | Estimated peak GPU RAM for the tuple | Within ACK offer; no hidden role omitted |
| `prepare_by_ms`, `deadline_ms` | Bounded work and execution | Inside total request deadline |
| `provider_token_proof` | Existing one-time Selection authorization | Fresh and exact purpose |
| `admission_lease_proof` | Optional GPU-capacity reservation | Same offer and assignment |
| `input_key_grants` | Optional per-role/per-input recipient-encrypted access grants | Bound to mode, request, attempt, plan, Provider, boot epoch, complete role tuple, input edge/object, and expiry |
| `input_key_grant_digest` | Canonical identity of all grants | Covered by Selection digest/signature; tamper or cross-binding replay rejected |
| `selection_digest` | Duplicate/conflict identity | Same digest idempotent; conflict rejected |
| `status_handle` | Signed progress return binding | Exact request/provider/Selection binding |

Core authenticates the generic Selection target, identity, exact opaque payload
digest, ProviderToken, optional lease proof, deadline, and replay state without
parsing the fields above. Through the generic participant seam, a pure
NDNSF-DI validator returns one canonical opaque commit blob and acceptance
payload. The Core-owned `GenericSelectionTxnStore` atomically consumes the
ProviderToken, commits the optional opaque-lease disposition, and persists the
encrypted blob/payload in one WAL `COMMITTED` record. That record is the
authoritative logical DI admission/tuple/grant/generation state; NDNSF-DI
projects it idempotently into runtime state and only then starts asynchronous
preparation. No key grant or role authority exists before this record commits.
Incremental same-attempt role additions are forbidden; they require a new
attempt and fresh ACK/ProviderToken. There is no `PreparationCommit`,
`PreparationToken`, or second role-acceptance decision.

## GenericSelectionTxnRecord

Application-independent Core WAL record defined by
`contracts/core-opaque-selection-transaction.md`.

| Field | Meaning | Owner/validation |
|---|---|---|
| `transaction_id`, `state`, `sequence` | Generic local transaction identity | Core uniqueness and replay |
| request/attempt/Selection/provider/boot/deadline | Authenticated generic context | Core |
| token and optional opaque-lease record references/dispositions | One-time generic authority | Core; replayed from same WAL |
| participant ID/version | Registered application callback | Core treats as opaque registration identity |
| encrypted opaque commit blob and digest | Authoritative application logical state | Core bounds/digests only; NDNSF-DI defines semantics |
| opaque acceptance payload and digest | Replayable application evidence | Core carries/signs; NDNSF-DI defines semantics |

`VALIDATING` is in-memory and has no authority. The single fsynced
`COMMITTED` record is the linearization point. Core then invokes
`onCommitted(record)` idempotently. WAL failure aborts before token/lease
disposition; callback failure after commit is an accepted-then-failed
preparation outcome, never `NOT_SELECTED`.

## DISelectionAcceptance

Mandatory authenticated evidence of the one Provider-local Selection
linearization point. It is queryable through generic signed status transport
and is never aggregated into a cross-Provider readiness cover.

| Field | Meaning | Validation |
|---|---|---|
| `request_id`, `attempt`, `provider`, `boot_epoch` | Exact live Provider attempt | Same as offer and Selection |
| `offer_digest`, `selection_digest`, `role_tuple_digest` | Installed authority | Exact committed values |
| `token_disposition`, `lease_disposition`, `gpu_admission_digest` | Local transaction result | Consumed/committed once |
| `install_sequence`, `installed_at`, `local_not_after` | Monotonic acceptance record | Same-sequence conflicts rejected |
| `signer`, `signature` | Provider authenticity | Required and verified |

Until this record is observed, delivery state is `UNKNOWN`, not
`NOT_SELECTED`. The coordinator retransmits only the byte-identical Selection
and reconciles status until the local cutoff. Missing acceptance evidence says
nothing about whether the Provider installed the tuple.

## RoleLocalReadyReceipt

Authenticated evidence that one assigned model/runtime instance is locally
executable under a bounded lease. It satisfies only that role's local
preparation latch and has no authority to permit or prevent final Selection.

| Field | Meaning | Validation |
|---|---|---|
| `request_id`, `attempt`, `plan_digest`, `selection_digest` | Assigned dataflow identity | Exact match |
| `provider`, `boot_epoch`, `role` | Local role | Same as final Selection |
| `instance_id` | Prepared local instance | Stable and unique |
| `artifact_digests` | Verified loaded model/runtime objects | Same as plan |
| `backend`, `device`, `resources` | Actual local binding | Satisfies assignment |
| `ready_at_ms`, `expires_at_ms` | Local-ready lease | Fresh at role start |
| `signer`, `signature` | Provider authenticity | Required and verified |

## RoleExecutionGate

For role `r`:

```text
eligible(r) =
    validFinalSelection(r, request, attempt, plan, bootEpoch, generation)
    AND freshLocalPreparation(r)
    AND every required direct input is complete and authenticated
    AND r has not started
    AND the attempt generation is not cancelled, failed, expired, or superseded
```

Direct inputs are the request input for source roles and verified output
objects from declared direct predecessors for downstream roles. A verified
direct predecessor output already implies completion of that predecessor's
transitive dependencies; the role does not wait for a global or transitive
ReadySet.

## InputOutputObjectManifest

Canonical security and completeness contract for a request input, intermediate
tensor bundle, or terminal output.

| Field group | Required binding |
|---|---|
| Identity | Canonical opaque/randomized object name, request, attempt, plan, edge/scope |
| Lineage | Producer role/Provider/boot epoch and allowed consumer roles/Providers |
| Semantics | Model-semantics digest, dtype, shape, layout, and output schema |
| Bounds | Segment count, final block, total bytes, maximum expanded bytes |
| Integrity | Public per-segment/ciphertext-object digests plus recipient-protected plaintext digest |
| Confidentiality | Encryption/key identifier, approved AEAD suite, per-key unique object/segment nonce or misuse-resistant construction, recipient grant, and canonical identity/schema/segment fields as associated data |
| Authority | Expiry, signer certificate, policy epoch, and signature |

The consumer sets an input latch only after bounded whole-object reassembly and
all identity, lineage, schema, authorization, public ciphertext-digest, AEAD,
and protected plaintext-digest checks pass. Mixed segments, nonce reuse under a
non-misuse-resistant suite, wrong producer, stale/revoked authority,
decompression overflow, and path-like artifact names fail closed. Routing
prefixes, ciphertext size/segment count, timing, and traffic relationships
remain observable baseline metadata; padding/anonymity is not claimed.

## ResultContract and DIResultEnvelope

Every sealed plan contains exactly one `ResultContract`:

```text
one response-producer role
OR one explicit aggregation role whose input edges cover the complete sink set
+ required output schemas and manifest digests
+ accepted model semantics and deadline
```

The final `DIResultEnvelope` binds profile, service, requester, request,
attempt, UserToken echo, model/plan/Selection lineage, complete required
sink-output manifests and aggregate digest, the required
Provider/Selection-acceptance-set digest, response producer identity and boot
epoch, deadline, terminal status, signer, and signature. The requester accepts
it only when every Provider in the result dependency closure is durably
accepted and none is unresolved or failed. Same-digest
retransmission is idempotent. A missing sink, wrong lineage/signer/UserToken,
conflicting digest, old attempt, or Response after an earlier durable terminal
decision is rejected.

## AdoptedInputEvidence

Trusted evidence that a fully published old-attempt output is imported as an
input to a fresh attempt. It binds the old object digest and lineage, the
supersession cutoff, exact new plan/input edge, revalidated model semantics,
fresh recipient encryption/key grant, and coordinator signature. Late
old-attempt events are not equivalent to adoption and remain rejected.

## State transitions

### Inference invocation and attempt

```text
Invocation:
  CREATED -> RUNNING -> COMPLETED | FAILED | CANCELLED | EXPIRED

Attempt[n]:
  CREATED -> REQUEST_PUBLISHED -> ACK_COLLECTING -> ACK_CLOSED
  -> PLANNING -> MATERIALIZING? -> DI_PLAN_VALIDATED
  -> COLLAB_PLAN_COMMITTED -> SELECTING -> ACTIVE
  -> SUCCEEDED | FAILED | CANCELLED | EXPIRED | SUPERSEDED
```

An attempt may be cancelled before publication and therefore has at most one
inference Request. Any attempt reaching `REQUEST_PUBLISHED` publishes exactly
one, and any attempt reaching `ACK_CLOSED` closes exactly one ACK set. A
terminal attempt never returns to `ACTIVE`. Compensation that needs a new assignment creates
`Attempt[n+1]`; the invocation remains `RUNNING` and the original deadline and
attempt budget still apply. Only a successful attempt's verified Response may
win the invocation's terminal compare-and-set.

The nested generic NDNSF carrier state is:

```text
DeferredCollaborationInvocation:
  CREATED -> REQUEST_PUBLISHED -> ACK_COLLECTING -> ACK_CLOSED
  -> PLAN_COMMITTING -> PLAN_COMMITTED -> SELECTION_DELIVERING
  -> RESPONDED | FAILED | CANCELLED | EXPIRED

commit permission:
  NOT_ALLOWED -> OPEN -> COMMITTING -> COMMITTED
  OPEN | COMMITTING -> FAILED | CANCELLED | EXPIRED
```

Core sets `OPEN` exactly when the immutable ACK snapshot closes. In
`PREPLANNED` mode it applies the already supplied generic plan through this
same transition. In `DEFERRED` mode NDNSF-DI asynchronously returns the
validated projection. Commit before `OPEN`, after any terminal state, or with
conflicting bytes is rejected. The carrier never invokes a DI strategy or
materializer on the Face event loop.

After `COLLAB_PLAN_COMMITTED`, Selection delivery and dataflow progress are
orthogonal:

```text
Selected-target delivery:
  UNSENT -> SENT -> ACCEPTED | UNKNOWN | FAILED | CANCELLED | EXPIRED
  UNKNOWN -> ACCEPTED | FAILED | CANCELLED | EXPIRED

Unselected offer disposition:
  OFFERED -> NOT_SELECTED | FAILED | CANCELLED | EXPIRED

Request observations (non-exclusive):
  SELECTION_PARTIAL | SELECTION_COMPLETE
  PREPARING | WAITING_INPUTS | DATAFLOW_ACTIVE | REPLANNING | FINALIZING
```

`ACTIVE` begins when exact-target delivery starts; it does not wait for
`SELECTION_COMPLETE`. A Provider with a validated Selection may prepare and run
eligible roles while another Provider remains `SENT` or `UNKNOWN`. An accepted
Selection consumes its ProviderToken and commits its optional opaque lease; an
unselected offer tombstones its unused token and releases or expires its lease.
A terminal inference Response still
requires a complete `ResultContract` and no unresolved required Selection
delivery. `UNKNOWN` means that Selection may have committed but authenticated
acceptance evidence has not yet been observed; absence of a receipt is never
proof of non-acceptance. Pending request state, local preparation leases, and token
tombstones remain bounded through completion, explicit release/cancellation,
failure, or expiry.

Request cancellation atomically tombstones only the coordinator's local
invocation generation and participates in the same first-terminal-wins
compare-and-set as Response acceptance. Authenticated idempotent cancel/release
delivery then converges each remote Provider independently, or its local
deadline expires. A partitioned or non-preemptable Provider may continue
physical work temporarily, but its late output cannot revive the invocation.

### Provider request offer

The role-neutral ACK exists before any concrete role object:

```text
Offer: OFFERED -> SELECTED | NOT_SELECTED | FAILED | CANCELLED | EXPIRED
```

`SELECTED` means one valid final Selection transaction durably installed that
Provider's complete role tuple, consumed its one ProviderToken, committed its
DI admission record, and created `DISelectionAcceptance`. Only then are the
concrete Provider-role states created. Same-digest replay returns the same
record; conflicting terminal offer decisions fail closed.

### Selected Provider role

After Selection authorizes a role, its input latches are initialized from any
already verified buffered inputs. Local-preparation completion and every
remaining authenticated input arrival may then occur in either order. Each
selected role retains orthogonal state:

```text
Preparation: NOT_STARTED -> PREPARING -> LOCAL_READY | FAILED | CANCELLED | EXPIRED
Each input:  MISSING -> FETCHING -> VERIFIED | INVALID | CANCELLED | EXPIRED
Execution:   WAITING -> RUNNING -> COMPLETED | FAILED | CANCELLED | EXPIRED
Producer publication:
             NOT_PUBLISHED -> PUBLISHING -> PUBLISHED
             | FAILED | CANCELLED | EXPIRED
Per consumer edge:
             MISSING -> FETCHING -> VERIFIED
             | INVALID | CANCELLED | EXPIRED
Resources:   HELD | IN_USE -> RELEASE_PENDING -> RELEASED | LEASE_EXPIRED
```

Derived observations may include:

```text
PREPARING
WAITING_LOCAL_READY
WAITING_INPUTS
RUNNABLE
RUNNING
COMPLETED
FAILED
CANCELLED
EXPIRED
```

Final-Selection validation, local-preparation completion, and each authenticated
input arrival re-evaluate only the affected role. The first successful
eligibility transition atomically changes `Execution` from `WAITING` to
`RUNNING` under the request/attempt/plan/role/boot-epoch/execution-generation
binding; concurrent duplicate events cannot admit it twice in that generation.
Physical recomputation after a crash is allowed only in a fresh generation or
attempt and cannot create a second accepted result. Cancellation,
deadline, failure, or a superseding attempt tombstones the generation and
disables future eligibility. A duplicate operation with the same canonical
identity is idempotent, and a conflicting digest for an existing identity is
rejected.

### Final Response

```text
ResultContract at coordinator:
          INCOMPLETE -> COMPLETE | INVALID | CANCELLED | EXPIRED
Provider Response publication:
          NOT_PUBLISHED -> PUBLISHING -> PUBLISHED
          | FAILED | CANCELLED | EXPIRED
Requester Response validation:
          NOT_RECEIVED -> RECEIVED -> VERIFIED -> ACCEPTED
                                    | REJECTED | LATE
Invocation terminal CAS:
          RUNNING -> COMPLETED | FAILED | CANCELLED | EXPIRED
```

`Result=COMPLETE` requires every sink in the sealed `ResultContract`, or the
output of its explicit aggregation role. A Response publication failure does
not turn a partial result into success. The requester journals verified
Response acceptance before exposing `COMPLETED`; duplicate same-digest
Responses are idempotent and conflicting or late Responses are evidence only.
