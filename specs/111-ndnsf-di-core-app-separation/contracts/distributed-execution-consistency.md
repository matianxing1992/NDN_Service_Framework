# Contract: Distributed Execution and Deployment Consistency

## Purpose and system model

This contract defines how one process-local `DistributedInferenceEngine`
coordinates side effects across independently failing Providers without becoming
a cluster-global service. It closes the gap between a locally coherent
`ValidatedExecutionIntent` and distributed Provider/deployment state.

NDNSF-DI uses the following explicit authority model:

- the initiating `APPClient` is the **request coordinator** for one
  `(requester identity, request ID, attempt epoch)`;
- one operator-authorized `APPDeployment` identity is the **single writer** for
  one deployment ID and lifecycle epoch stream;
- every Provider is authoritative for its own boot epoch, capacity,
  reservations, local admission, execution and cleanup;
- `ServiceController` remains the NDNSF permission/policy authority but is not
  an inference coordinator or data-plane scheduler;
- Provider-to-Provider dependency Data and final results remain normal secured
  NDNSF/NDN data-plane traffic.

The design is requester-coordinated distributed execution. It does not claim a
leaderless consensus system, a globally serializable database or simultaneous
failure-atomic execution at every Provider.

## Reuse and protocol boundary

The consistency mechanism MUST reuse the existing generic
`ProviderExecutionLeaseTable`, `DistributedLeaseTransaction`, authenticated
Targeted lease service, V2 request/ACK/selection/response path,
`ExecutionAttemptAuthority`, Provider boot identity and output/checkpoint epoch
mechanisms. It MUST NOT add a second lease table, a planner service, a cluster
coordinator or new NDN top-level wire names.

Versioned lease-response, selection-assignment and result-rendezvous payloads
MAY carry the additional identities and authenticated receipts required below.
Legacy payloads remain readable during the declared compatibility window, but
they cannot be promoted to the new distributed-consistency evidence level.

## Identity and fencing tuple

Every distributed action is bound to a complete fencing tuple:

```text
request execution:
  requester identity
  request ID
  positive attempt epoch
  intent digest
  Provider identity + Provider boot epoch
  lease ID + lease expiry/deadline

deployment lifecycle:
  deployment ID
  authorized deployment-owner identity
  positive lifecycle epoch
  expected previous state digest
  action digest + idempotency key
  target Provider identity + Provider boot epoch
```

`attempt epoch` is also the request-coordinator term. A new/replanned attempt
advances it; a same-identity process restart may resume the current attempt from
authenticated durable evidence. A second independent coordinator epoch is not
introduced. Only the same requester identity may resume a request by default. A different
identity requires an explicit operator/controller-authorized delegation that is
outside policy authority and is not inferred from possession of a request ID.

Providers MUST remember or derive the highest accepted attempt/lifecycle epoch
and reject stale epochs. Releasing the owned resource does not immediately erase
that fence: a compact high-watermark tombstone survives for at least the maximum
operation-retry, certificate, result-rendezvous and replay-protection window.
It is garbage-collected only after no delayed operation/output in that domain
can be accepted. Provider restart creates a new Provider boot epoch and
invalidates every receipt, certificate and lease bound to the old epoch; every
new `PREPARE` also binds the coordinator's expected Provider boot epoch, so an
old-attempt prepare cannot silently rebind to the restarted Provider.
Idempotency keys deduplicate the same operation; they do not resolve different
operations and therefore never replace epoch fencing.

## Request execution state machine

The request coordinator drives the existing lease operations in the following
order:

```text
PROPOSED
  -> PREPARING
       -> PREPARED_ALL
       -> ABORTING -> ABORTED_OR_EXPIRING
  -> REVALIDATING
  -> COMMITTING_LEASES
       -> COMMITTED_ALL
       -> RELEASING -> RELEASED_OR_EXPIRING
  -> CERTIFIED
  -> DISPATCHED
  -> EXECUTING
  -> COMPLETED | FAILED | EXPIRED
```

Normative rules:

1. `PREPARE` reserves only Provider-local resources. It never authorizes model
   execution or visible output.
2. The coordinator MUST prepare every selected Provider, retain authenticated
   receipts and revalidate the complete intent against current Provider,
   snapshot, deadline and policy-state lineage.
3. Any failed prepare/revalidation causes best-effort `ABORT` for every prepared
   lease. Unreachable leases remain inert and expire at their Provider-owned
   reservation TTL.
4. `COMMIT` changes a lease from prepared to committed but remains non-
   executable. A partial committed set is safe because no execution certificate
   exists and every member has a finite TTL/deadline.
5. Only after all required commit receipts are authenticated may the coordinator
   create one immutable `ExecutionCommitCertificate`.
6. Existing V2 Selection/assignment dispatch MUST bind or reference that
   certificate. A Provider activates its local committed lease only after it
   validates the certificate, its membership, the complete assignment/intent
   digest, Provider boot epoch, request/attempt fencing and execution deadline.
7. A Provider that lacks a valid certificate MUST reject activation even when a
   local lease is `COMMITTED`.
8. No Provider or policy may locally reconstruct, shorten or extend the
   certificate membership set.

This is **atomic execution visibility**, not a claim that every distributed
machine changes durable state at the same instant. Network partitions may let
only a subset receive the certificate and perform bounded work. Such work
cannot produce an accepted terminal result unless the result satisfies the
same certificate, attempt and output-commit contract.

## Authenticated Provider receipts

A prepared or committed Provider response used as evidence MUST retain:

- operation and schema version;
- Provider identity and Provider boot epoch;
- requester identity, request ID and attempt epoch;
- service, plan and intent digests;
- role/target/resource-binding digest and conflict keys;
- lease ID, state, expiry and execution deadline;
- idempotency key/fingerprint;
- NDNSF Data name, signer/certificate identity and wire/content digest, or an
  equivalent Core-verifiable authenticated receipt.

The Python wrapper MUST NOT reduce an authenticated response to unproven payload
bytes when it is later used to certify a multi-Provider commit. Core validates
signer, digest, freshness, requester/service binding and exact receipt
membership before accepting it.

## ExecutionCommitCertificate

The certificate is an immutable content-addressed record containing:

- schema/version, certificate ID and certificate digest;
- requester identity, request ID, attempt epoch and intent digest;
- objective, snapshot, policy-state, deployment/session and plan digests;
- the exact sorted role/Provider/target assignment;
- the exact sorted committed receipt identities and digests;
- certificate creation time, not-after deadline and result-rendezvous prefix;
- coordinator signature/identity and required Core validation evidence.

The certificate MAY be embedded when bounded or published as authenticated
named Data and referenced by exact name/digest from Selection. Providers fail
closed when it is missing, stale, malformed, signed by the wrong requester,
contains an unknown receipt, omits a required role, binds a different lease or
uses a different Provider boot epoch.

## Request-coordinator crash and restart

Failure behavior is phase-specific:

| Crash point | Required behavior |
| --- | --- |
| Before any prepare | No distributed side effect exists. |
| During prepare/revalidation | Prepared leases remain non-executable and are aborted on recovery or expire by reservation TTL. |
| During lease commit, before certificate | Committed leases remain non-executable; recovery releases them or Provider TTL expires them. |
| After certificate publication, before all dispatches | Reachable Providers may execute only under the certificate; missing roles time out and no incomplete terminal result is accepted. |
| During execution | Providers continue only until the committed execution deadline; checkpoint/progress and output remain attempt-scoped. |
| After terminal result publication | The same requester identity can retrieve the content-addressed result/evidence by request ID, attempt epoch and certificate digest without re-executing. |

The restarted coordinator either:

1. resumes observation/retrieval for the same certified attempt using the same
   requester identity and immutable certificate; or
2. starts a higher attempt epoch after Core recovery validation. The higher
   epoch fences stale work and output; it does not make old resource cleanup
   optional.

No hidden in-memory APP state is the sole source of truth for a certified
attempt. Result rendezvous, checkpoint and terminal evidence have bounded
freshness/retention sufficient for the declared restart window. If that window
expires, the result is unavailable and recovery starts a new attempt rather
than accepting unverified output.

APP persists coordinator bindings, authenticated receipt identities,
certificates and rendezvous pointers through the fixed `RuntimeJournal` defined
by [`deployment-and-invocation-workflow.md`](deployment-and-invocation-workflow.md).
The journal does not replace Provider authority or authenticated NDN evidence;
it makes exact references reopenable after APP process failure.

## Output visibility and recovery

Intermediate Data MAY be produced independently by certified Providers, but
every object is named/bound by request ID, attempt epoch, producer role,
Provider boot epoch and certificate/intent digest. Consumers and the requester
reject cross-attempt or cross-certificate Data.

A terminal output becomes visible only when Core validates:

- the currently authoritative request attempt;
- the required completion/role evidence under the same certificate;
- monotonic progress/output-commit epoch;
- exact requester/service/plan/model identity;
- absence of an already accepted terminal result.

Late, duplicate, reordered or stale outputs may remain retrievable as diagnostic
Data but MUST NOT become application-visible results or optimization success
evidence.

## Network-partition semantics

The consistency model is fail-closed for new authority and bounded for already
committed work:

- stale, missing or mutually inconsistent telemetry cannot authorize a new
  deployment action, lease, assignment, renewal or recovery decision;
- a Provider may continue an already certified attempt only while its local
  lease, dependency/security state and execution deadline remain valid;
- a Provider cannot extend a lease because the coordinator is unreachable;
- unreachable prepared/committed-but-uncertified resources expire locally;
- a partitioned coordinator cannot synthesize Provider receipts or downgrade to
  a legacy unleased execution path;
- after healing, higher attempt/lifecycle and current Provider boot epochs win;
  stale messages are rejected rather than merged;
- Snapshot freshness supports planning only and never proves ownership. Lease,
  certificate and epoch validation prove authority to act.

The contract guarantees safety and bounded cleanup under partition; it does not
guarantee availability when required Providers, dependencies or authority facts
are unreachable.

## Deployment lifecycle consistency

`DeploymentPolicy` remains advisory. `APPDeployment` applies one lifecycle
proposal using a mechanism-owned `DeploymentLifecycleRecord` and the following
state machine:

```text
PROPOSED
  -> PREPARED_TARGETS
  -> REVALIDATED
  -> CERTIFIED
  -> APPLYING
  -> APPLIED | ABORTED | EXPIRED
```

Rules:

1. Operator configuration assigns exactly one deployment-owner identity per
   deployment ID. Competing identities are rejected. This is an explicit
   single-writer control-plane boundary, not an elected cluster leader.
2. Every action carries a positive lifecycle epoch, expected previous state
   digest, action digest, idempotency key, target set and rollback/release plan.
3. Target Providers prepare the action and return authenticated receipts without
   changing externally visible residency/capacity state.
4. The owner issues a `DeploymentActionCertificate` only after all target
   receipts and active lease/session, cooldown, minimum-residency, capacity and
   drain preconditions revalidate.
5. Providers compare-and-apply the certificate only when the expected previous
   state and lifecycle epoch match. Duplicate application is idempotent;
   different action digests at the same epoch are conflicts.
6. Destructive scale-in, drain, unload or cache/model eviction never becomes
   visible from a partial prepare. A partial apply is reconciled by the same
   certificate/action digest; it must not be counteracted by an independently
   generated lower/equal epoch.
7. Owner restart reuses durable lifecycle/action evidence and continues the same
   idempotent action or advances the lifecycle epoch after reconciliation. A
   second active process using the same owner identity must still win the
   Provider compare-and-apply race; identity equality alone is not fencing.
8. Transfer of deployment ownership requires an explicit operator-authorized
   owner-generation change and reconciliation. Automatic leader election and
   consensus are outside Spec 111.

The APPDeployment owner records revision/action/certificate/event identities in
the same fixed RuntimeJournal. A local journal entry without matching Provider
evidence cannot promote observed state to READY, ACTIVE or APPLIED.

Scale-out/prewarm may leave safe extra capacity after partial application and
reconcile forward or release it. Scale-in/unload is fail-closed: inability to
prove all destructive preconditions leaves the existing deployment active.

## Orphan resource recovery

Every prepared, committed, executing or lifecycle-prepared resource has:

- owner/requester identity and action/request identity;
- Provider boot epoch and attempt/lifecycle epoch;
- conflict keys or resource-binding proof;
- explicit reservation TTL or execution deadline;
- terminal release/abort/expire reason and evidence identity.

Providers run cleanup on operation entry and a bounded periodic timer so an
idle Provider does not retain orphan resources indefinitely. Cleanup is
idempotent and releases compute slots, memory reservations, model residency
pins, cache/KV references, subscriptions, temporary artifacts and waiter-queue
entries owned only by the expired record. Cleanup MUST NOT evict resources still
referenced by another active certified attempt or deployment action.

Cleanup also MUST NOT delete attempt/lifecycle high-watermark tombstones before
their declared replay window. Tombstones are bounded metadata, not active
resource reservations, and have an independently tested garbage-collection
deadline.

The implementation declares and tests upper bounds for prepared-reservation,
committed-uncertified, executing and lifecycle-action orphan retention. A
best-effort remote `ABORT`/`RELEASE` is an acceleration, not the safety basis.

## Evidence and reason codes

Every phase records coordinator/deployment owner, operation, request/deployment
identity, attempt/lifecycle/provider epochs, intent/action/certificate and
receipt digests, lease/deadline, transition, result and reason code. At minimum,
typed rejection covers:

```text
CONSISTENCY_RECEIPT_MISSING
CONSISTENCY_RECEIPT_INVALID
CONSISTENCY_CERTIFICATE_INCOMPLETE
CONSISTENCY_CERTIFICATE_MISMATCH
CONSISTENCY_STALE_ATTEMPT
CONSISTENCY_STALE_LIFECYCLE_EPOCH
CONSISTENCY_PROVIDER_EPOCH_MISMATCH
CONSISTENCY_OWNER_MISMATCH
CONSISTENCY_PREVIOUS_STATE_MISMATCH
CONSISTENCY_PARTIAL_COMMIT_INERT
CONSISTENCY_ORPHAN_EXPIRED
CONSISTENCY_RESULT_NOT_AUTHORITATIVE
```

Existing more-specific lease/security/attempt reason codes remain authoritative
and are not collapsed into generic failures.

## Required fault matrix

Before large-scale Engine implementation, deterministic contract tests and a
MiniNDN fault smoke MUST cover:

- failure before/after every Provider prepare, revalidate, commit, certificate,
  selection/activation, output commit and release boundary;
- duplicate, reordered, delayed, lost and conflicting operations;
- requester crash/restart before certificate, after certificate and during
  streaming execution;
- Provider restart between receipt and activation;
- two coordinators using lower/higher attempt epochs;
- two APPDeployment processes proposing same-action replay and conflicting
  actions at the same/adjacent lifecycle epochs;
- network partition before commit, after certificate and during dependency
  transfer;
- periodic orphan cleanup with no subsequent incoming operation;
- stale operation replay after resource cleanup but before/after the fencing-
  tombstone retention boundary;
- result retrieval after requester restart and rejection after retention expiry;
- legacy payload compatibility without promotion to certified evidence.

Acceptance requires zero uncertified execution, zero duplicate/stale visible
terminal output, exactly one authoritative lifecycle action per epoch, and
eventual release or expiry of every orphan within its declared upper bound.

## Implementation gate

This contract, its entities, requirements, tasks, traceability and fault matrix
are blocking prerequisites for `DistributedInferenceEngine` execution-intent
implementation. A structure-only PASS does not satisfy the gate. Code-aware
pre-implementation audit must confirm reuse of the existing lease/attempt
mechanisms, no second authority, executable failure tests and no claim of
leaderless/global atomicity.
