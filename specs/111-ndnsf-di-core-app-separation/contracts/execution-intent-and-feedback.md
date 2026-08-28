# Contract: Atomic Execution Intent and Optimization Feedback

## Why this is not another policy

Policies propose alternatives; mechanisms validate and apply them. Atomic
commit/abort is therefore a Core/provider correctness mechanism, and outcome
observation is a one-way optional SPI. Neither owns an optimization choice and
neither belongs in the ten-policy `OptimizationSuite`.

## Prepared intent

Before side-effecting execution, APP constructs `ValidatedExecutionIntent` from:

- request/attempt, objective, engine-snapshot and policy-state identities;
- selected model variant, variant-bound plan and Provider assignment;
- cache affinity, role-scoped execution targets and declared tuning values;
- deployment/session, lease, reservation and adapter-capability preconditions;
- deterministic intent/idempotency identity and release actions.

The process-local intent state is:

```text
PROPOSED
  -> PREPARED
  -> REVALIDATING
  -> COMMITTED_LOCAL
  -> CERTIFIED -> DISPATCHED -> EXECUTING
  -> ABORTED
```

`COMMITTED_LOCAL` is a reservation state, not permission to execute. The
distributed phase expands it into prepare-all/revalidate/commit-all and requires
the complete authenticated receipt set described by
[`distributed-execution-consistency.md`](distributed-execution-consistency.md).
At most one terminal state is valid. Every transition is evidenced.

## Revalidation and atomicity

Core/provider mechanisms revalidate variant/plan binding, Provider eligibility,
lease/reservation, deployment lifecycle, cache epoch, adapter/device capability,
deadline and policy/snapshot lineage as one unit. They reuse the existing
authenticated Targeted lease transaction. Changed input aborts the whole intent.
Abort releases every reachable resource acquired during preparation; bounded
lease expiry and periodic Provider cleanup reclaim unreachable orphans. The
engine may re-enter only a graph-declared bounded decision edge; it never applies
a mixture of old and new decisions.

Fault injection covers before/after every prepare, reserve, revalidate, commit
certificate, activation, result publication and release boundary. Acceptance
requires either one complete certificate and one visible attempt, or no
executable attempt plus bounded cleanup. A partition may leave a reservation
temporarily in doubt; it may not create new execution authority. This contract
therefore promises atomic execution visibility, not simultaneous global commit.

## Streaming progress and recovery

Core records output epochs, visible/acknowledged commit offsets and compatible
checkpoint identities. `RecoveryPolicy` sees only advertised restart/resume
boundaries and cannot manufacture a checkpoint. Mechanism validation rejects
duplicate, reordered or stale output after retry, resume, reassign or restart.

## Optimization outcome

After intent rejection/commit and execution completion/failure, APP emits one
bounded `OptimizationOutcome` containing:

- unique outcome ID plus decision/intent/execution/candidate lineage;
- committed/rejected/failed state and standardized metric/estimate envelopes;
- progress/checkpoint/failure summary without prompt, tensor or secret payload;
- policy state epoch/digest consumed by the corresponding decision.

Delivery may be at-least-once, but the observer MUST implement logical
exactly-once processing by outcome ID.

## Observer SPI

```text
OptimizationObserver.observe(outcome, observer_budget) -> ObserverEvidence
```

The observer is instance-scoped, optional, allowlisted like other external code
and executed after the current decision on a separate APP executor. It may
update extension-owned learning/evaluation state. It cannot modify the current
request, block result publication, weaken Core checks or require a Core state
store. Timeout/exception is separately evidenced; inference remains unaffected.

Stateful policies record `policy_state_epoch` and `policy_state_digest` on every
decision. Extension configuration declares snapshot/version or serialized
update semantics, and concurrent suite instances do not share mutable state
unless APP explicitly supplies that state owner.

## Privacy and tenancy

Outcome and policy projections contain workload shape and consented metadata
only. Prompt text, raw tensors, credentials, tokens, decrypted policy material
and unrelated tenant/cache contents are excluded. Cache outcome/affinity facts
are tenant/security-scope filtered before observation.
