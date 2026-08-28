# Data Model: Spec 130 Boundary Repair

## AttemptKey

Fields: requester identity, service name, request ID, positive attempt number,
request digest, absolute deadline, requester boot/session epoch. Equality is
exact; the same request ID under another identity is a different key.

## AckLiability

Fields: AttemptKey, Provider identity, Provider boot epoch, ACK digest,
application decision key, optional opaque application context digest, liability
horizon, state, last delivery sequence, decision digest and receipt digest.

States:

```text
COLLECTING -> SELECTED | NOT_SELECTED
COLLECTING -> LATE_PENDING -> NOT_SELECTED
SELECTED/NOT_SELECTED -> RECEIPT_CONFIRMED
LATE_PENDING -> LIABILITY_EXPIRED
```

The generic framework does not know whether the application context denotes a
reservation. It only guarantees at-most-one logical terminal decision per
liability and idempotent delivery/receipt tracking.

## AttemptTombstone

Fields: AttemptKey, closed-at time, bounded expires-at time, selected Provider
set, known AckLiabilities, accepted response state, diagnostic counters.

The tombstone cannot add candidates, invoke the application response callback,
or change an existing winner. It can authenticate a previously unknown late ACK
for the same attempt, create one liability through the registered application
hook, deliver its terminal decision, and garbage-collect after the maximum
liability horizon.

## ReservationRecord

NDNSF-DI-owned fields: reservation ID, AttemptKey, Provider identity and boot
epoch, resource binding, units, tentative/committed deadline, decision digest,
state and release reason.

States:

```text
TENTATIVE -> COMMITTED -> EXECUTING -> COMPLETING -> RELEASED
TENTATIVE -> NOT_SELECTED | EXPIRED -> RELEASED
COMMITTED -> ABORTED | EXPIRED -> RELEASED
EXECUTING -> STOPPING -> RELEASED
```

`EXPIRED` may directly release only a non-executing reservation. An executing
record must pass through `STOPPING` or `COMPLETING` and include local stop or
completion evidence.

## ExecutionPin

Fields: reservation ID, AttemptKey, exact resource/model binding digest,
Provider boot epoch, role, worker/executor identity, start time, renewal
deadline, execution deadline, state, terminal proof and release time.

Validation rules:

- one live pin per exclusive resource;
- stale attempt/boot completion cannot mutate a newer pin;
- timer/renewal expiry changes `EXECUTING` to `STOPPING`, never directly to
  reusable capacity;
- release requires completion proof, local stop proof, or restart fencing that
  proves the old executor cannot run.

## RetryAttempt

Fields: AttemptKey, ordinal, prior reservations, release barrier status,
collision/rejection reasons, entropy-source class, jitter upper bound, sampled
delay, scheduled-at time, remaining deadline and terminal outcome.

A successor attempt may be created only after the prior release barrier is
satisfied. Production entropy source is recorded by class, not its secret
state. Deterministic seeds are test-only metadata.

## DependencyPlan

NDNSF-DI-owned fields: plan digest, AttemptKey, exact roles, selected Provider
per role, direct predecessor edges, terminal role, per-recipient assignment
digests and input-binding digest. The graph is acyclic and every non-source role
has at least one direct predecessor.

## StageDataEvidence

Fields: AttemptKey, plan digest, producer Provider/role, consumer Provider/role,
chunk and sequence, payload digest, authorization digest, observed delivery
state and replay key.

Same replay key/digest is idempotent. Same key with a conflicting digest,
wrong predecessor, wrong consumer, stale attempt or stale Provider boot epoch is
rejected before eligibility.

## StageExecution

Fields: AttemptKey, role, pin ID, local preparation state, required predecessor
set, received predecessor evidence, start/end times, output digest and terminal
reason.

States:

```text
SELECTED -> PREPARING -> WAITING_DEPENDENCIES -> RUNNING -> COMPLETED
SELECTED/PREPARING/WAITING_DEPENDENCIES/RUNNING -> STOPPING -> ABORTED
```

A source role may move from preparation to running with an empty predecessor
set. A downstream role may run only when received evidence exactly covers its
declared direct predecessors.

## FormalCellEvidence

Fields: immutable cell ID, family, manifest/source/Spec129 hashes, topology,
host-to-identity/NFD/PID map, command and environment, fault plan and observed
fault events, message ledger, AckLiabilities, reservations, pins, retry attempts,
stage evidence/executions, safety/availability/terminal verdicts, invocation
count and exit status.

A formal cell is harness-invalid if required actors share a host/NFD, a named
fault has no observed real effect, or an outcome metric was assigned without an
underlying event. Harness-invalid remains in the campaign and blocks closure.
