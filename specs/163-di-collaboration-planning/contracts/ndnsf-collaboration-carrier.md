# Generic NDNSF Deferred-Collaboration Carrier Contract

## 1. Purpose

This contract makes NDNSF's existing distributed-collaboration API the
canonical carrier for Spec 163. It adds one generic, application-independent
way to choose roles and dependencies after ACK discovery without adding a
parallel NDNSF-DI Request/ACK/Selection protocol.

The existing collaboration facilities remain authoritative for:

- Request publication and ACK collection;
- pending-request identity, deadline, UserToken, ProviderToken, permissions,
  signatures, NAC-ABE, replay, and optional opaque lease handling;
- final Selection delivery and retry;
- Provider `CollaborationContext`;
- collaboration Data and exact-name segmented large objects;
- operation status and final Response.

NDNSF-DI supplies only opaque request/ACK/assignment/result payloads and the
generic plan projection described below.

## 2. Modes

Every generic collaboration invocation has one immutable mode.

| Mode | Roles/dependencies become known | Intended use |
|---|---|---|
| `PREPLANNED` | Before Request | Existing fixed/deployed collaboration callers |
| `DEFERRED` | After immutable ACK closure | Spec 163 dynamic split and placement |

Both modes use one implementation of pending-request, ACK, plan-commit,
Selection, token, collaboration-data/status, timeout, cancellation, and
Response state. `PREPLANNED` is not a second protocol; it stages a generic plan
before Request and applies it through the same commit transition after ACK
closure.

## 3. Public layering

The application-facing API remains one call:

```python
handle = app.request(
    model=exact_model_ref,
    input=input_value,
    objective=objective,
    constraints=constraints,
    timeout_ms=timeout_ms,
)
```

The following carrier API is internal to NDNSF-DI and other framework
integrations:

```python
invocation = service_user.begin_collaboration(
    service,
    opaque_request_payload,
    mode="DEFERRED",
    ack_timeout_ms=ack_timeout_ms,
    timeout_ms=timeout_ms,
    request_id=request_id,
    ack_coverage_predicate=coverage_policy,
)

ack_closed = await invocation.acks_closed()

# Caller-owned work, outside the Core Face/event-loop callback:
decision = di_coordinator.plan(ack_closed)
await di_coordinator.ensure_materialized(decision)

commit = await invocation.commit_plan(
    ack_closed_digest=ack_closed.digest,
    roles=decision.generic_roles,
    dependencies=decision.generic_dependencies,
    key_scopes=decision.generic_key_scopes,
    provider_assignments=decision.providers_by_role,
    artifact_data_names=decision.artifact_data_names,
    scope_key_data_names=decision.scope_key_data_names,
    assignment_payloads=decision.opaque_assignments_by_provider,
)
```

Names above define the design contract. Language bindings may use idiomatic
spelling, but SHALL preserve one begin operation, one immutable ACK-closure
result, and one one-shot plan commit on the same invocation handle.

`ack_coverage_predicate` is optional and application-owned. It receives the
authenticated candidates observed so far and returns a boolean early-close
hint. The Core invokes it only after normal ACK authentication, token, policy,
and duplicate checks. A true result closes the same immutable `ACK_CLOSED`
snapshot; it does not select a Provider, infer a split, publish artifacts, or
commit a plan. A DI implementation may provide a bounded
`AckRoleCoveragePolicy` that validates `DIProviderOfferV2` candidates and
checks coverage of a declared role hint. The hint is not the role plan: the
adapter graph, splitter, strategy, and final role assignments remain
post-`ACK_CLOSED`.

## 4. Begin contract

`begin_collaboration` SHALL:

1. create one generic pending collaboration identity;
2. bind service, request payload digest, request ID, mode, ACK deadline, total
   deadline, security profile, and fallback policy;
3. publish at most one normal NDNSF Request;
4. collect existing authenticated ACK messages and ProviderTokens;
5. close the candidate set exactly once;
6. resolve `acks_closed()` with an immutable canonical snapshot and digest.

It SHALL NOT:

- accept DI model, GPU, shard, tensor, preparation, or result fields as typed
  Core arguments;
- split or load a model;
- invoke an application planner on the Face event loop;
- send Selection before a valid plan commit;
- allow a late ACK to mutate the closed snapshot.

## 5. ACK-closure result

The generic `AckClosedSnapshot` contains:

```text
request ID and attempt
service
mode
closed-at and total deadline
ordered tuple of existing generic AckSelectionCandidate values
per-candidate authenticated envelope identity
snapshot digest
```

Core treats every ACK payload as opaque. NDNSF-DI validates
`DIProviderOfferV2` and derives `ProviderPlanningView` after receiving this
snapshot. Raw ProviderTokens, lease proofs, decryption keys, and mutable Core
handles are not passed to the placement strategy.

`acks_closed()` is a future/event result, not an ACK callback that must finish
planning synchronously. Repeated reads return the same snapshot. Cancel,
deadline, or Request failure resolves it to a typed terminal error.

## 6. Plan-commit input

Core accepts only generic collaboration fields:

| Field | Core validation |
|---|---|
| `ack_closed_digest` | Exact live invocation snapshot |
| `roles` | Bounded, unique generic role specs |
| `dependencies` | Bounded references to declared roles; acyclic where required by the generic API |
| `key_scopes` / `role_scopes` | Refer only to declared roles |
| `provider_assignments` | Providers exist in the closed successful ACK set |
| `artifact_data_names` | Canonical bounded NDN names |
| `scope_key_data_names` | Canonical bounded NDN names |
| `assignment_payloads` | Bounded opaque bytes addressed to selected Providers |
| `commit_digest` | Canonical digest of every field above |

NDNSF-DI validates all model, graph, GPU, cache, artifact-manifest, tensor,
result, and compensation semantics before calling `commit_plan`. Core does not
repeat or replace that semantic validation.

## 7. Commit state and idempotency

```text
NOT_ALLOWED -> OPEN -> COMMITTING -> COMMITTED
OPEN | COMMITTING -> FAILED | CANCELLED | EXPIRED
```

- Core enters `OPEN` exactly when ACK closure is durable.
- Commit before `OPEN` fails with `PLAN_COMMIT_NOT_OPEN`.
- The first valid commit digest is immutable.
- Byte-identical retry returns the original commit/Selection-delivery identity.
- A different digest returns `PLAN_COMMIT_CONFLICT`.
- Commit after cancel, expiry, supersession, or terminal Response fails.
- A commit that fails validation emits zero Selection.
- `COMMITTED` authorizes Core to execute the existing participant selector
  result/explicit assignment validation and Selection delivery exactly once.

Unknown delivery after `COMMITTED` does not reopen plan choice. Core retries
the same Selection bytes or uses existing status reconciliation.

## 8. Selection and Provider execution

The commit SHALL use the existing Selection message and ProviderToken path.
For each selected Provider, Core carries one complete generic role projection
and one opaque NDNSF-DI assignment payload. The Provider's existing
`CollaborationContext` remains the application entrypoint.

NDNSF-DI uses:

- `assignment()` for the Selection-bound opaque DI projection;
- `fetchLarge()` or DistributedRepo references for exact-name model/runtime
  artifacts;
- `publishLargeNamed()` and collaboration publish/subscribe/wait primitives
  for planned objects where appropriate;
- `reportOperationStatus()` for authenticated progress;
- `publishFinalResponse()` for the result admitted by `ResultContract`.

No `PreparationCommit`, second Request, second ACK round, global ReadySet, or
DI-specific activation wire message is introduced.

## 9. Threading and deadline rules

- Request/ACK transport remains on the normal NDNSF event path.
- ACK closure only snapshots generic state and schedules/resolves continuation.
- DI planning, model graph analysis, materialization, repository publication,
  and GPU work execute outside the Face event-loop callback.
- The total invocation deadline continues while deferred work runs.
- Planning/materialization cannot extend the signed total deadline.
- Cancel and supersession fence an in-progress planner; a late commit is
  rejected even if its DI decision is otherwise valid.
- The implementation must bound ACK count, role count, dependency count,
  assignment bytes, commit bytes, and retained invocation state.

## 10. Security and persistence

The carrier SHALL durably bind:

```text
request/attempt/service/mode
request payload digest
ACK-closed digest
plan-commit digest and state
selected Provider identities and ProviderToken hashes
exact Selection byte/digest identities
deadline/cancel/supersession/terminal state
```

Protected exact Selection bytes and necessary token/lease ciphertext may be
stored encrypted at rest until bounded expiry for byte-identical retry.
Ordinary evidence stores hashes, not raw tokens or keys.

The generic deferred handle grants no Selection authority to a placement
strategy. NDNSF-DI's trusted coordinator calls commit only after independent
decision validation and trusted publication.

## 11. Compatibility and migration

Current callers of:

```python
request_collaboration(
    service,
    payload,
    roles=...,
    dependencies=...,
    ...
)
```

remain source-compatible. Internally they create a `PREPLANNED` invocation,
stage the generic plan, collect ACKs, and apply it through the same commit
state. Existing selector and role-coverage behavior remains for this mode.

The Spec 163 application path SHALL use `DEFERRED`. It must not:

- call the old preplanned entrypoint with placeholder roles;
- use the collaboration API only to select one coordinator and then create a
  private second collaboration protocol;
- publish a DI-owned Selection equivalent;
- duplicate Core timeout, token, status, or Response state.

Rollback may disable `DEFERRED` for new calls while draining or expiring
existing deferred invocations. It never converts an in-flight deferred
invocation to preplanned mode.

## 12. Verification obligations

Required tests include:

1. non-DI deferred collaboration proves the seam is application-independent;
2. preplanned source-compatibility and identical Selection behavior;
3. immutable ACK snapshot and rejection of late candidate mutation;
4. early, duplicate-identical, duplicate-conflicting, late, cancelled, expired,
   and superseded commit histories;
5. crash before/after ACK closure and before/after commit persistence;
6. one Request, one ACK closure, one accepted commit, and one logical
   Selection per selected Provider;
7. a deferred role-coverage predicate can close ACK collection early while
   preserving the immutable closure digest and post-closure planning order;
8. existing `CollaborationContext` object/status/final-Response path;
9. static check for zero parallel DI Request/ACK/Selection wire names;
10. default NDNSF-DI call-path trace proves `DEFERRED`, while the fixed-plan
   compatibility trace proves `PREPLANNED`;
11. event-loop watchdog proves planning/materialization does not block Face
    progress.
