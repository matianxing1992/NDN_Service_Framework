# Data Model and State Invariants

## 1. InvocationIdentity

| Field | Meaning |
|---|---|
| `request_id` | Created once by the User before the first Request |
| `attempt_epoch` | New only for an explicitly linked replacement attempt |
| `requester` / `service_name` | Security and routing binding |
| `source_digest` / `runtime_digest` | Immutable candidate identity |
| `model_identity_digest` | Digest of the complete `ModelIdentity` |
| `strategy_digest` | Exact planning implementation/configuration |
| `prompt_digest` / `generation_digest` | Input and decoding policy |
| `created_at` / `hard_deadline` | Monotonic lifecycle bounds |

**Invariant**: `request_id` never changes from Request through terminal Response.
An automatic transport retransmission is not a new invocation. A formal rerun
after a repair uses a new experiment/source identity and is linked to, not
substituted for, the original.

## 2. ModelIdentity

```text
model_name
revision
content_digest
tokenizer_digest
semantics_digest
adapter_digest
dependency_graph_digest
precision
runtime_backend_digest
partition_scheme_digest
```

**Compatibility rule**: Reuse is safe only when every field that affects stored
bytes or execution semantics matches. A display name alone is never sufficient.

## 3. ProviderCapabilitySnapshot

| Group | Fields |
|---|---|
| Binding | request, attempt, Provider identity, boot epoch, signed-at, expiry |
| Reachability | route/face identity, measured RTT, usable bandwidth estimate |
| Capacity | disk, host RAM, total/usable GPU memory, device identity |
| Load | queue depth, active roles, GPU utilization, memory pressure |
| Residency | artifact/model/partition digest, tier, bytes, verified-at, last-used, pin/lease |

ACK collection is side-effect free: the snapshot advertises what the Provider is
willing and able to do but does not fetch, load, reserve, or mutate model state.
Late or wrong-request snapshots are retained as rejected evidence and cannot
reopen a closed set.

## 4. CollaborationPlan

| Field | Meaning |
|---|---|
| `request_id`, `attempt_epoch` | Invocation binding |
| `ack_set_digest` | Exact inputs used by the strategy |
| `graph_digest` | Adapter-produced dependency graph |
| `partition_digest` | Ordered graph cut and shard definitions |
| `assignments` | Role, Provider, boot epoch, device, artifact digests |
| `dependencies` | Direct predecessor role and named output |
| `strategy_digest` | Algorithm and configuration identity |
| `execution_policy` | `DATA_DRIVEN_V2` or explicit `LEGACY_READY_SET_V1` |
| `deadline`, `signature` | Bounded authenticated authorization |

**State**: `DRAFT -> VALIDATED -> COMMITTED -> TERMINAL` or `DRAFT/VALIDATED -> REJECTED`.

**Invariant**: A committed plan is immutable. Capacity, graph, artifact, and
dependency validation completes before final Selection/commit.

All selected Providers must have advertised the committed execution policy.
Mixed policy is invalid. `DATA_DRIVEN_V2` is the default; the V1 ReadySet policy
is explicit compatibility only and cannot be selected as an automatic fallback.

## 5. ArtifactResidencyRecord

```text
ABSENT
  -> FETCHING
  -> VERIFIED_DISK
  -> HOST_LOADING
  -> HOST_RESIDENT
  -> DEVICE_LOADING
  -> GPU_RESIDENT
  -> EVICTING
  -> ABSENT
```

Each transition contains request/attempt/plan (when request-scoped), Provider
boot epoch, artifact digest, byte length, storage/device locator, adapter/runtime
identity, monotonic sequence, timestamps, and verification evidence.

**Invariants**:

- `VERIFIED_DISK` requires exact content digest and size.
- `HOST_RESIDENT` requires a live owner/lease for the verified bytes.
- `GPU_RESIDENT` requires adapter confirmation on the assigned device and enough
  usable device memory; a log message or file copy is insufficient.
- Reusable content survives request cleanup only in the content-addressed cache
  or managed resident object. Per-request work paths are links/views where
  possible, not duplicate payload owners.
- A Provider restart changes `boot_epoch` and invalidates RAM/GPU claims.

## 6. StageOperation

| Field | Meaning |
|---|---|
| Binding | request, attempt, plan, assignment, role, Provider boot epoch |
| Preparation | required artifacts, current residency, adapter session |
| Dependencies | direct predecessor role/output identities |
| Input | prompt input for Stage 0 or predecessor outputs for later stages |
| Output | content identity, sequence/token range, destination roles |
| Progress | epoch, monotonic sequence, state, value/checkpoint, expiry |
| Terminal | completed, failed, canceled, expired; reason and evidence |

**Eligibility predicate**:

```text
plan_committed
AND assignment_binding_valid
AND local_preparation == GPU_RESIDENT
AND every direct predecessor input is authenticated and present
AND request is non-terminal and within hard deadline
```

The predicate is evaluated on every relevant local state/data event. It does not
include `all_selected_roles_ready` or an operator/settle timer.

**State**:

```text
ASSIGNED -> VERIFYING -> FETCHING? -> LOADING -> WARMING -> LOCAL_READY
LOCAL_READY + dependencies -> RUNNABLE -> EXECUTING -> OUTPUT_PUBLISHED -> COMPLETED
any nonterminal -> FAILED | CANCELED | EXPIRED
```

## 7. ProgressCheckpoint

Fields: invocation binding, component, operation ID, Provider/role, artifact and
range when relevant, attempt, epoch, sequence, state, progress-known, progress,
message, authenticated timestamp, expiry, and details schema/payload.

**Invariant**: Only a valid strictly newer checkpoint for the same operation
refreshes its no-progress deadline. Duplicate, stale, unauthenticated, wrong-plan,
or wrong-attempt events are evidence but not liveness.

## 8. LifecycleTrace

An append-only ordered record of Request, ACK outcomes, ACK closure, plan,
Selection, artifact events, residency transitions, stage inputs/outputs, token
records, progress, terminal Response/failure, and cleanup.

**Trace acceptance**:

- exactly one request ID and one committed plan per admitted invocation;
- at most one accepted terminal outcome;
- causal timestamps: stage start follows local readiness and every direct input;
- every model byte and load event is attributable to a residency transition;
- every generated token belongs to the same invocation;
- security verdict and CPU/GPU backend are explicit.

## 9. FailureRecord

Fields: experiment identity, invocation identity, primary boundary, component,
Provider/role, operation/artifact/range, last progress checkpoint, observed
symptom, root cause status, terminal reason, evidence paths, owner, repair commit,
local regression, replacement candidate, and remote requalification.

**State**: `OBSERVED -> CLASSIFIED -> REPRODUCED -> ROOT_CAUSED -> REPAIRED -> LOCAL_QUALIFIED -> REMOTE_REQUALIFIED`, with `ENVIRONMENTAL` and `UNRESOLVED` terminal audit states allowed.

## 10. ExperimentIdentity

The formal run identity binds source/SIF, model/tokenizer/graph/shards, planning
strategy, prompts/generation, topology/routes, security configuration, Slurm
schedule/allocation, analyzer version, and admission evidence.

**Invariant**: Once submitted, the experiment row and raw artifacts are
append-only. A repair creates a new linked identity; it cannot overwrite the
original failure or reuse its success slot.
