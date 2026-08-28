# Data Model And State Machines: Spec 174

The entities below are contracts over existing NDNSF-DI owners. They do not require replacement classes with these exact names. The implementation task must map each contract to current native/Python fields and add only missing identity or validation behavior.

## Identity Vocabulary

All digests use an explicitly declared canonical serialization and algorithm. A string label is never accepted where a digest-bound identity is required.

| Identity | Purpose | Mutability |
|---|---|---|
| `invocationId` | Stable logical application invocation | stable across recovery attempts |
| `requestId` | NDNSF request identity for one attempt | new per attempt unless the existing protocol binds attempt separately |
| `attemptId` | Fences retries/replans/restarts | monotonic or unique per invocation |
| `ackClosureDigest` | Binds immutable ordered/normalized Provider offers and close metadata | immutable after closure |
| `policySnapshotDigest` | Binds placement policy/config used for the proposal | immutable for one plan |
| `planId` / `planDigest` | Binds roles, Providers, edges, groups, result/security contracts | immutable after commit |
| `generation` | Fences reassembly/re-execution within an attempt | advances on permitted replacement |
| `groupId` / `groupEpoch` | Binds tensor-group membership and collective authority | epoch advances on group replacement |
| `roleId` | Identifies one complete pipeline or tensor-rank role | unique in plan |
| `rank` / `worldSize` | Tensor-group position and size | sealed in plan |
| `operationId` / `round` | Identifies collective operation and round | sealed or derived from plan schedule |
| `tensorId` / `microbatchId` | Identifies semantic tensor and microbatch | sealed or deterministically derived |
| `artifactDigest` | Identifies canonical model/adapter/tokenizer object | content-addressed |
| `manifestDigest` | Identifies exact tensor manifest | content-addressed |
| `candidateDigest` | Identifies exact SIF bytes | immutable promotion identity |

## Core Entities

### 1. OperationRequest

Application-owned intent, not a placement plan.

Required fields:

- requester identity, `invocationId`, request/attempt identity;
- model/task and operation identity;
- input reference or payload digest;
- deadline and acceptance/result contract;
- placement/security/runtime policy references and their snapshots/digests;
- UserToken and normal NDNSF signature/authorization material.

Forbidden fields in the default path:

- request-specific Provider list;
- final role-to-Provider map;
- precomputed stage split or tensor membership;
- an assertion that artifacts are already resident;
- mutable execution or device authority.

### 2. ProviderOffer

Signed request-bound willingness and sanitized capability evidence. It is not a reservation and not execution authority.

Required fields:

- requester/request/attempt binding;
- Provider identity and boot/session epoch;
- supported runtime/artifact/model-family compatibility;
- complete resource peak vector relevant to admission;
- bounded residency/cache evidence;
- topology/network information allowed by policy;
- offer expiry/freshness and signature;
- normal ACK status/payload and one-time ProviderToken where applicable.

Validation:

- exactly matches request/attempt;
- signature, permission, token, freshness, and replay checks pass;
- duplicates from the same Provider are deterministically resolved or rejected before closure;
- offer data is sanitized before the pure planner sees it.

### 3. AckClosure

The linearization point between discovery and placement.

Fields:

- request and attempt identities;
- collection start, deadline, close time, and close reason;
- canonical set/order of accepted offer digests;
- rejected-offer summary without secrets;
- `ackClosureDigest` and core signature/commit record.

Invariant: after `ACK_CLOSED`, no offer can enter, leave, or mutate the planner input. A late ACK is recorded as late and cannot affect the committed plan.

### 4. SplitCandidate

A graph-valid role/dependency decomposition with no Provider assignment.

Fields:

- canonical model graph/artifact identities;
- complete role definitions and their graph boundaries;
- tensor-group/rank definitions where used;
- dependency edges, tensor contracts, and final result owner/contract;
- graph coverage proof: every required node/initializer/input/output covered once;
- compatibility and estimated peak resource vectors.

Invariant: an opaque node remains one atomic role. A candidate never fabricates a split unsupported by the canonical graph/adapter.

### 5. ExecutionRole

Smallest complete assignment unit.

Variants:

- `PipelineRole`: complete local graph segment.
- `TensorRankRole`: complete executable for one rank of one tensor group.
- `MergeRole`: explicit computation that combines upstream results.

Required fields:

- `roleId`, role kind, graph/artifact slice, expected inputs/outputs;
- local assembly and peak resource requirements;
- one selected Provider identity;
- incoming/outgoing dependency identifiers;
- optional `groupId`, rank, world size, operation schedule;
- result/security/recovery contracts.

Invariants:

- one role has exactly one Provider;
- one selected Provider has exactly one role in an attempt;
- a role cannot span Providers;
- rank roles in one group share group/epoch/schedule but do not share role ownership.

### 6. PlacementProposal

Pure strategy output over `AckClosure + graph/artifact snapshots + policy snapshot`.

Fields:

- input digests;
- selected split candidate;
- proposed bijective Provider/role mapping;
- proposed dataflow/group/result contracts;
- deterministic score/explanation and strategy version.

It contains no keys, leases, open connections, callbacks, executables, or mutable runtime handles.

### 7. SealedPlacementPlan

Trusted-core validated authority source.

Fields:

- request, attempt, ACK closure, policy, model/artifact, and graph digests;
- all roles and bijective Provider assignments;
- all dependency edges and exact name templates;
- tensor groups, ranks, membership, collective operations/rounds, deadlines;
- Provider-local resource/admission requirements;
- protection epoch and security/result/recovery contracts;
- `planId`, canonical `planDigest`, commit time, signer.

Validation includes graph completeness, no cycles unless explicitly legal, role/Provider uniqueness, tensor-group completeness, dependency producer/consumer existence, final result reachability, deadline consistency, and security binding completeness.

### 8. ProviderSelectionProjection

The least-authority Provider-specific projection of the sealed plan.

Fields:

- request/attempt/plan/policy/closure identities;
- selected Provider and exactly one local role/rank;
- canonical artifact/assembly requirements;
- exact incoming/outgoing dependency contracts;
- group/rank schedule if applicable;
- local peak resource/admission requirement;
- Provider-specific grant, tokens, expiry, and protection epoch;
- plan digest/signature sufficient for verification.

Invariant: REQUEST and ACK permit consideration; only a valid exact Selection permits preparation/execution.

### 9. RoleAssemblySpec

Provider-local recipe derived from Selection.

Fields:

- canonical graph and initializer references with digests;
- adapter/tokenizer/config references as required;
- selected nodes/layers and external inputs/outputs;
- shape/dtype/layout constraints;
- local materialization identity and cache compatibility key;
- ONNX Runtime session/provider settings permitted by policy;
- full runtime peak vector and admission lease requirements.

Validation: complete local graph/initializer coverage; no undeclared external input; exact digest/compatibility; local runtime/provider availability; atomic admission immediately before execution.

### 10. DataflowEdge

One sealed producer-to-consumer dependency.

Fields:

- plan/attempt/generation and edge identity;
- producer Provider/role/rank;
- consumer Provider/role/rank;
- tensor/microbatch semantics and manifest name template;
- expected dtype/shape/layout/size bounds;
- digest/signature/trust policy;
- retry, no-progress, hard deadline, cancellation, and evidence policy.

### 11. TensorObjectManifest

Signed description fetched before segments.

Fields:

- full object identity/name and all binding components;
- producer identity and signing key reference;
- request/attempt/plan/generation/group/epoch/operation/round/role/rank/tensor/microbatch;
- dtype, shape, layout, canonical byte order, total length;
- segment count, maximum segment size, ordered segment names and digests;
- complete content digest and manifest digest;
- freshness/expiry and signature.

Invariant: consumers compare full expected identity and layout. Matching payload length alone is never sufficient.

### 12. TensorSegment

Fields:

- exact full segment name including segment number;
- payload bytes;
- segment digest and signed Data metadata.

Consumer behavior:

- accept only after a valid expected manifest;
- deduplicate by full name and digest;
- accept valid out-of-order arrival;
- reject wrong/stale/corrupt/conflicting duplicates;
- publish one reconstructed object-ready event exactly once.

### 13. TensorGroupContract

Fields:

- `groupId`, `groupEpoch`, world size, exact rank-role/Provider membership;
- group capability digest;
- operation and round schedule;
- per-round expected producers/consumers, partial/result tensors, and result owner;
- no-progress/hard deadlines and failure atomicity;
- cancellation/replan behavior.

Invariant: no rank is added, removed, independently failed over, or rebound inside an epoch. A membership change creates a new plan/group epoch.

### 14. ProviderGrant And GroupCapability

`ProviderGrant` binds exactly one Provider role to request/attempt/plan/policy/closure/artifacts/dataflow/protection epoch and expiry. `GroupCapability` binds one local rank to exact group membership, schedule, input/output contracts, and local wrapped key commitment.

Both are signed/authenticated, Provider-local, non-transferable, and checked again at use. Neither grants another Provider's role or arbitrary model access.

### 15. ProtectedRuntimeLease

Ephemeral local authority allowing a verified role to unwrap required data and invoke ONNX Runtime.

Fields:

- exact Provider grant and group capability digests;
- local device/runtime lease identity;
- wrapped/unwrapped key handles, never plaintext in evidence;
- creation/expiry/terminal state;
- zeroization acknowledgement.

Invariant: no plaintext reaches the model runner before `AUTHORIZED`; every terminal path ends in `ZEROIZED`.

### 16. FinalResult

Fields:

- request/invocation/attempt/plan/result-contract identities;
- final owner role and complete application payload/reference;
- content digest and signature;
- accepted terminal state and evidence reference.

Invariant: a stage tensor, rank partial, single generated token, or merely loaded model is not a complete result unless the sealed application contract says it is.

### 17. VerificationCandidate And GateManifest

`VerificationCandidate` binds source tree/allowed dirty inputs, dependencies, build record, SIF bytes, runtime entry points, and local gate manifests to `candidateDigest`.

`GateManifest` fields:

- gate/case/run identity and status;
- source/spec/design/dependency/config/input/oracle/candidate hashes;
- command, cwd, environment allowlist, seed, timeout, process identity;
- expected/actual result and lifecycle terminal state;
- hidden fallback and transport checks;
- first failure and bounded evidence references.

## State Machines

### Invocation And Attempt

```text
CREATED
  -> REQUEST_PUBLISHED
  -> ACK_COLLECTING
  -> ACK_CLOSED
  -> PLAN_PROPOSED
  -> PLAN_COMMITTED
  -> SELECTIONS_PUBLISHED
  -> EXECUTING
  -> RESULT_PUBLISHED
  -> ACCEPTED
```

Terminal alternatives: `REJECTED`, `CANCELLED`, `TIMED_OUT`, `FAILED`. A replan creates a new attempt from a permitted recovery state; it never reopens the old ACK closure or mutates the old plan.

### Plan

```text
DRAFT -> PROPOSED -> VALIDATING -> COMMITTED -> PROJECTED
```

Any failed validation goes to `REJECTED`. `COMMITTED`, `REJECTED`, and `SUPERSEDED` are immutable. Selection is illegal before `COMMITTED`.

### Provider Role

```text
UNSELECTED
  -> SELECTION_VALIDATING
  -> SELECTED
  -> ARTIFACT_FETCHING
  -> ASSEMBLING
  -> LOCALLY_VALIDATED
  -> ADMISSION_PENDING
  -> AUTHORIZED
  -> WAITING_DEPENDENCIES
  -> RUNNABLE
  -> EXECUTING
  -> OUTPUT_PUBLISHING
  -> COMPLETED
```

Every state can transition to bounded `CANCELLED`, `TIMED_OUT`, or `FAILED` where legal. No execution is allowed before both `AUTHORIZED` and all dependencies ready. Role readiness does not wait for unrelated Providers.

### Tensor Object Consumer

```text
EXPECTED
  -> MANIFEST_REQUESTED
  -> MANIFEST_VERIFIED
  -> SEGMENTS_REQUESTED
  -> RECONSTRUCTING
  -> OBJECT_VERIFIED
  -> READY
```

Invalid manifest/segment goes to `REJECTED`; no-progress/hard deadline to `TIMED_OUT`; plan/attempt cancellation to `CANCELLED`. Duplicate valid segments do not create new transitions after `READY`.

### Tensor Group

```text
SEALED -> CAPABILITIES_PROJECTED -> ACTIVE -> ROUND_n -> ... -> RESULT_READY -> COMPLETED
```

Wrong/missing rank, schedule violation, permanent dependency failure, or deadline leads to group `FAILED` and fences all remaining ranks for that epoch. It does not silently shrink world size.

### Protected Runtime

```text
UNBOUND
  -> GRANT_VERIFIED
  -> CAPABILITY_VERIFIED
  -> LEASE_ACQUIRED
  -> KEY_UNWRAPPED
  -> AUTHORIZED
  -> IN_USE
  -> RELEASED
  -> ZEROIZED
```

Any validation or execution failure transitions through `RELEASED -> ZEROIZED`. A crash/restart invalidates the boot/session epoch and old lease.

### Candidate Promotion

```text
SOURCE_FROZEN
  -> UNIT_PASS
  -> INTEGRATION_PASS
  -> MININDN_CPU_PASS
  -> SIF_BUILT
  -> SIF_LOCAL_PASS
  -> PROMOTABLE
  -> REMOTE_HASH_VERIFIED
  -> TIGER_REFERENCE_PASS
  -> TIGER_DISTRIBUTED_PASS
  -> QUALIFIED
```

Failure is terminal for that candidate identity. Repair creates a new source/build/candidate identity and restarts at the cheapest affected gate; no manifest status is overwritten.
