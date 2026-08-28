# Lifecycle, Failure, and Security Contract

## 1. Scope and layer boundary

This contract defines the NDNSF-DI protocol profile carried by the existing
NDNSF Request/ACK/Selection/Response lifecycle. It does not add a fifth generic
NDNSF message kind.

| Owner | Authoritative responsibilities |
|---|---|
| NDNSF Core | Existing generic Collaboration API; invocation identity; `PREPLANNED` and `DEFERRED` carrier modes; Request publication; immutable ACK closure; exactly-one opaque collaboration-plan commit; generic service names and payload transport; identity, signature, NAC-ABE, permission, UserToken and ProviderToken checks; exact-target delivery; opaque payload-digest binding; generic lease identifier/proof/expiry/consume/release; deadline, replay, idempotency, and signed status transport |
| NDNSF-DI | DI request/offer/selection/result codecs and canonical digests; model, split, role, backend, artifact, tensor, GPU-memory, preparation, dependency, result, compensation, and evidence semantics |
| Model adapter | Model-family graph analysis, candidate construction, materialization recipe, and runner implementation behind NDNSF-DI ports |
| Placement strategy | A bounded, data-only split/provider proposal; never execution or network authority |

`begin_collaboration`, `ACK_CLOSED`, and `commit_plan` are local generic API and
state boundaries inside the existing collaboration lifecycle; they add no
fifth wire message. `PREPLANNED` stages roles/dependencies before Request for
source compatibility. Spec 163 uses `DEFERRED`, so NDNSF-DI derives them from
the closed ACK snapshot and model dependency graph before committing them.

The dependency direction is `NDNSF-DI -> NDNSF Core`. Core MUST treat every DI
payload as opaque bytes and MUST NOT branch on a DI profile name or parse a
model, GPU, role, artifact, tensor, DAG, preparation, or recovery field.

The current Core-owned deployment messages and READY/activation path are legacy
architectural debt. They may remain frozen behind an explicit V1 compatibility
adapter during migration, but the V2 path MUST NOT extend them. The canonical
V2 codecs, validators, admission ledger, preparation state, and executor live
under `NDNSF-DistributedInference/`.

## 2. Trust and threat model

| Principal or component | Assumption and boundary |
|---|---|
| Trust anchor and ServiceController | Trusted to issue identities and service policy correctly. Compromise is outside this protocol's protection boundary. |
| Application identity and NDNSF-DI coordinator | Trusted to preserve request constraints, validate strategy output, seal plans, protect ephemeral control state, and apply terminal decisions. |
| Placement strategy implementation | An operator-installed, digest-pinned, allowlisted local extension and therefore part of the host TCB. Its output is still untrusted and independently validated. NDNSF-DI does not claim to execute hostile Python safely in-process. |
| Model adapter, RunnerAdapter, and executable artifact publisher | Operator-authorized and trusted for executable content; they are part of the Provider host TCB. A valid signature/digest authenticates bytes but does not sandbox a container, pickle, custom operator, native library, or runner. Host compromise by authorized malicious executable content is outside this baseline; accepting untrusted executable producers requires a separate process/VM sandbox and restricted artifact-format policy. |
| Provider | Authorized and authenticated. The protocol tolerates crash, omission, delay, duplication, restart, stale state, and equivocation detectable from conflicting signed evidence. Correct computation by a malicious authorized Provider is not proven without optional redundancy, attestation, or application-level result verification. |
| Non-executable data artifact publisher | Authorized signer for the declared namespace and schema. An artifact store, cache, or network replica is not trusted for integrity, confidentiality, or availability. |
| NDN network, cache, and repository | May drop, delay, duplicate, reorder, replay, partition, truncate, or substitute packets. Cryptographic verification and bounds protect safety; availability remains deadline-bounded, not guaranteed. |
| Local clocks | No perfect clock synchronization is assumed. Each participant enforces a local monotonic deadline derived from the signed remaining lifetime and a configured maximum clock-skew allowance. |

The strategy API does not pass tokens, keys, raw input, writable stores, network
clients, or device handles. This is an authority-minimization API, not a Python
sandbox. Hostile third-party code requires a separately specified process/OS
sandbox and is not accepted by this feature's in-process extension path.

## 3. Authenticated DI envelopes

The generic NDNSF messages carry the following opaque application payloads:

```text
Request.payload   = DIRequestEnvelopeV2
ACK.payload       = DIProviderOfferV2
Selection.payload = DISelectionAssignmentV2
Status.payload    = DISelectionAcceptanceV2 | DIRoleStatusV2
Response.payload  = DIResultEnvelopeV2
```

The signed `DIRequestEnvelopeV2` binds the exact required profile and minimum
version. A Provider MUST echo that version in its signed offer. Capability
stripping or substituting V1 for a V2-required request fails closed; fallback
requires a new attempt explicitly authorized by the Application.

Every DI digest uses one canonical encoding and a distinct domain-separation
label. Unknown critical fields, duplicate fields, non-canonical encodings, or
semantically equal fields with a different canonical byte string are rejected.
Core binds the generic Request/ACK/Selection identity, token, lease, deadline,
and signature to the exact opaque payload bytes/digest; NDNSF-DI validates the
payload semantics.

Generic `ACK.status=true` means that the service handler accepted the request as
a candidate and supplied an authenticated opaque payload. Only
`DIProviderOfferV2` gives that payload the stronger NDNSF-DI meaning:

> Until its local monotonic expiry, the Provider offers one compatible,
> non-empty role tuple within its DI-owned exclusive resource envelope.

The offer carries an `acceptance_predicate_version` and digest covering typed
backend, precision, role-count, model-intent, resource-unit, GPU-memory safety
margin, deadline, and compatibility rules. NDNSF-DI owns the live GPU probe,
peak-memory calculation, safety margin, and exclusive admission ledger. A Core
lease is an opaque proof only; Core never interprets GPU MiB.

`DISelectionAcceptanceV2` and `DIRoleStatusV2` are disclosed only to the
requester identity bound to the attempt or to an explicitly controller-
authorized auditor. The generic status transport authenticates the query and
signer and protects recipient-confidential payloads; NDNSF-DI applies
field-level redaction. Tokens, lease proofs, input names, key grants, plaintext
sizes not required by the requester, and local filesystem/device details never
enter public status or ordinary evidence logs.

## 4. Invocation and attempt hierarchy

One public model/task-first `APPClient.request()` call, implemented by
`AutomaticPlanningCoordinator.request()`, creates one durable
`InferenceInvocation`. The invocation may contain a bounded sequence of wire
attempts:

```text
InferenceInvocation
  CREATED -> RUNNING -> COMPLETED | FAILED | CANCELLED | EXPIRED

Attempt[n]
  CREATED -> REQUEST_PUBLISHED -> ACK_COLLECTING -> ACK_CLOSED
  -> PLANNING -> MATERIALIZING? -> PLAN_COMMITTED -> SELECTING -> ACTIVE
  -> SUCCEEDED | FAILED | CANCELLED | EXPIRED | SUPERSEDED
```

Each attempt publishes at most one Request, closes one ACK set, commits at most
one plan on the same collaboration invocation, and creates at most one logical
final Selection per selected
Provider. Here “one Request” means one inference Request; bounded status,
cancel, and release operations are auxiliary generic exact-target control
invocations, not new inference attempts, and cannot assign roles, extend a
deadline, or produce the inference Response. An attempt never returns from a
terminal state to `ACTIVE`.
Compensation that needs a new Provider, role assignment, token, or lease creates
`Attempt[n+1]`; the public invocation and deadline do not change.

Only one attempt may win the invocation's terminal compare-and-set. A verified
successful Response that is durably accepted first makes the invocation
`COMPLETED`; a durable cancellation, failure, or expiry recorded first prevents
all later Responses from completing it.

The requester-side collaboration-plan commit and the Provider-side Selection
transaction are separate linearization points:

```text
commit_plan:
  requester-side, same invocation, seals generic roles/dependencies/opaque
  assignments, consumes no ProviderToken, proves no Provider acceptance

GenericSelectionTxnStore COMMITTED:
  Provider-side, one selected Provider, consumes its ProviderToken/opaque lease
  and durably accepts that Provider's complete tuple
```

Early, expired, cross-invocation, or conflicting plan commits fail before any
Selection. A byte-identical retry returns the recorded commit result. No
planning, model materialization, artifact I/O, or strategy execution may block
the Face event loop.

## 5. Provider-side Selection transaction and delivery evidence

Core first authenticates the generic Selection envelope, exact target,
request/attempt identity, opaque payload digest, token, lease proof, deadline,
and replay state without irreversibly consuming application state. NDNSF-DI
then validates the complete DI assignment against its offer and plan through
the registered pure opaque participant seam.

The normative transaction mechanism is
[core-opaque-selection-transaction.md](core-opaque-selection-transaction.md):
one Core-owned `GenericSelectionTxnStore` WAL records generic token/opaque-lease
disposition plus an encrypted application commit blob/digest. Core never parses
that blob. The NDNSF-DI blob is the durable logical authority for its GPU
admission transition, complete role tuple, grants, generation fence, and
acceptance preimage; in-memory DI state is an idempotent projection.

```text
generic authentication
  -> NDNSF-DI prepare(context, opaque payload)
       validates DI semantics and returns canonical blob + acceptance payload
       with no external side effect
  -> one fsynced Core WAL COMMITTED record
       atomically consumes ProviderToken, commits opaque lease disposition,
       stores authoritative encrypted blob and acceptance payload
  -> NDNSF-DI onCommitted(record)
       idempotently projects GPU ledger, role tuple, grants, and latches
  -> expose DISelectionAcceptanceV2 through signed generic status
  -> enqueue asynchronous preparation
```

The WAL `COMMITTED` fsync is the sole Provider-local Selection linearization
point. No externally visible DI projection, role preparation, or execution
occurs before it. Token and lease decisions are replayed from this same WAL,
not committed earlier in a separate store. Recovery replays the same
transaction and `onCommitted()` callback idempotently. A
same-identity/same-digest Selection returns the existing acceptance record; a
same-identity/different-digest Selection, `SELECTED` versus `NOT_SELECTED`
conflict, expired offer, or consumed token fails closed.

`DISelectionAcceptanceV2` is mandatory, authenticated, per Provider, and
queryable through the access-controlled generic signed status transport. It is delivery evidence, not a
cross-Provider readiness cover and never delays another Provider's eligible
role. Missing evidence produces `UNKNOWN`, not `NOT_SELECTED`. The coordinator
retries the byte-identical Selection and reconciles the acceptance record until
the attempt cutoff. `UNKNOWN` becomes `EXPIRED` or `FAILED` only after the
Provider's offer/Selection deadline; the Provider can no longer start new work
after that local deadline.

## 6. Role, object, output, and Response product state

For every selected role:

```text
Preparation: NOT_STARTED -> PREPARING -> LOCAL_READY
             | FAILED | CANCELLED | EXPIRED
Each input:  MISSING -> FETCHING -> VERIFIED
             | INVALID | CANCELLED | EXPIRED
Execution:   WAITING -> RUNNING -> COMPLETED
             | FAILED | CANCELLED | EXPIRED
Producer publication:
             NOT_PUBLISHED -> PUBLISHING -> PUBLISHED
             | FAILED | CANCELLED | EXPIRED
Per consumer edge:
             MISSING -> FETCHING -> VERIFIED
             | INVALID | CANCELLED | EXPIRED
```

The only execution admission transition is an atomic
`WAITING -> RUNNING` compare-and-set under
`request/attempt/plan/role/provider-boot/execution-generation`. It requires:

```text
valid DI Selection
AND fresh local preparation
AND every declared direct input object fully verified
AND live attempt generation
```

This is at-most-once admission per role generation. NDNSF-DI does not claim
physical exactly-once computation across process crashes. A new attempt may
recompute a pure role, but only one result lineage can be accepted. Runners MUST
be inference-pure or make any external side effect separately idempotent.

Every `InputOutputObjectManifestV2` binds:

```text
canonical opaque object name; request/attempt/plan/edge; producer and allowed consumers;
provider identities and boot epochs; model semantics; dtype/shape/layout;
segment count, final block, total bytes, maximum expanded bytes;
public ciphertext segment/whole-object digests; recipient-protected plaintext digest;
encryption/key identifier, AEAD suite, per-segment nonce, and associated-data digest;
expiry, signer certificate, policy epoch, and signature
```

The consumer verifies bounded reassembly, every ciphertext segment and public
ciphertext digest, schema, producer, authorization, expiry, AEAD associated
data, and the recipient-protected plaintext digest after decryption before
setting an input latch. Protected request inputs and intermediate tensors
require recipient-scoped grants; grants, plaintext buffers, and ephemeral keys
are erased or made unreachable on terminal cleanup. Artifact ingestion uses
authorized signer/revocation checks, bounded staging, digest verification, and
atomic promotion so corrupt or oversized content cannot poison a reusable
cache.

For each encryption key, every object/segment nonce MUST be unique, or the
profile MUST use an approved nonce-misuse-resistant AEAD construction. AEAD
associated data binds the canonical object identity, request/attempt/plan/edge,
producer/consumer set, segment index/count, schema digest, policy epoch, and
expiry. Public manifests expose only ciphertext digests; a plaintext digest is
keyed or recipient-encrypted and is checked only after decryption. Public NDN
names use opaque randomized object components, but the baseline still leaks
namespace routing prefixes, ciphertext size/segment count, request timing, and
traffic relationships to an observer. Padding, batching, and anonymity against
that metadata leakage are optional extensions, not baseline claims.

Every sealed plan defines exactly one `ResultContract`: either one response
producer role or one explicit aggregation role whose declared input edges cover
the complete required sink set. `DIResultEnvelopeV2` binds:

```text
profile/service/requester/requestId/attempt and UserToken echo;
model semantics, plan and winning Selection lineage;
complete required sink-output manifest set and aggregate digest;
required Provider/Selection acceptance-set digest;
response producer identity/boot epoch, deadline, status, and signature
```

Finalization is explicitly split by principal:

```text
ResultContract at coordinator:
  INCOMPLETE -> COMPLETE | INVALID | CANCELLED | EXPIRED
Provider Response publication:
  NOT_PUBLISHED -> PUBLISHING -> PUBLISHED | FAILED | CANCELLED | EXPIRED
Requester Response validation:
  NOT_RECEIVED -> RECEIVED -> VERIFIED -> ACCEPTED
                              | REJECTED | LATE
Invocation terminal CAS:
  RUNNING -> COMPLETED | FAILED | CANCELLED | EXPIRED
```

The requester accepts a Response only after the ResultContract is complete,
every Provider in the plan's required result dependency closure has a durable
authenticated Selection acceptance, the recomputed required-acceptance-set
digest matches the `DIResultEnvelopeV2`, no such delivery remains
`UNKNOWN`/`FAILED`/`CANCELLED`/`EXPIRED`/`NOT_SELECTED`, every output manifest
verifies, the signer is authorized, and the invocation terminal compare-and-set
succeeds. Same-digest Response retransmission is idempotent;
conflicting, partial, late, wrong-attempt, wrong-plan, wrong-signer, or
wrong-UserToken Responses are rejected and retained as evidence.

## 7. Failure and exception taxonomy

| Phase | Stable error class | Required action |
|---|---|---|
| Discovery | `ACK_TIMEOUT`, `NO_AUTHORIZED_PROVIDER` | Close ACK set once; release/expire offers; fail or start a new bounded attempt |
| Offer validation | `OFFER_INVALID`, `OFFER_EXPIRED`, `CAPACITY_CONFLICT` | No Selection; release or expire DI admission record |
| Planning | `NO_FEASIBLE_PLAN`, `STRATEGY_TIMEOUT`, `STRATEGY_INVALID` | No materialization, Selection, or execution side effect |
| Materialization | `ARTIFACT_MISSING`, `ARTIFACT_INVALID`, `MATERIALIZATION_FAILED` | Quarantine/cleanup staging; no Selection using the failed manifest |
| Selection | `SELECTION_INVALID`, `SELECTION_DELIVERY_UNKNOWN`, `SELECTION_DELIVERY_FAILED` | Reconcile identical bytes until cutoff; never reuse the token in the same attempt |
| Preparation | `ARTIFACT_FETCH_FAILED`, `ARTIFACT_CORRUPT`, `BACKEND_UNAVAILABLE`, `GPU_OOM` | Fence role and unfinished dependent closure; release or compensate |
| Input | `INPUT_TIMEOUT`, `INPUT_INVALID`, `INPUT_UNAUTHORIZED` | Do not set latch; fence dependent closure |
| Execution/output | `EXECUTION_FAILED`, `OUTPUT_INVALID`, `OUTPUT_PUBLISH_FAILED` | Reject partial result; compensate or fail |
| Finalization | `RESPONSE_INVALID`, `RESULT_INCOMPLETE`, `RESULT_CONFLICT` | Do not complete invocation; compensate within budget or fail |
| Any nonterminal phase | `CANCELLED`, `DEADLINE_EXCEEDED`, `PROVIDER_RESTARTED` | Persist tombstone, reject late authority/results, converge release |

Every error record includes request, attempt, plan/Selection when available,
owner, phase, stable reason code, retryability, affected dependency closure,
resource disposition, and evidence digest. Free-form text is diagnostic only.

## 8. Compensation and old-output adoption

The safe default is a complete new attempt. A smaller dependency-closure retry
is allowed only when the failed closure is the transitive set of unfinished
roles reachable from the failed role, plus any result-contract role whose
required input is invalidated.

An old-attempt output is never consumed directly as a live event. Before
superseding the old attempt, it must have reached `PUBLISHED` with a complete
verified manifest. Trusted NDNSF-DI code may create
`AdoptedInputEvidenceV2`, which binds the old object digest and lineage to one
declared input edge of the new sealed plan, rechecks model semantics and policy,
and issues fresh recipient encryption/grants. After the supersession cutoff,
all newly arriving old-attempt events remain rejected. Without valid adoption
evidence, the role is recomputed.

Attempt count, compensation count, materialization work, and elapsed time share
one configured budget and the original invocation deadline; compensation cannot
loop indefinitely.

## 9. Cancellation, partition, and release

Cancellation has one requester/coordinator-local linearization point, not a
distributed atomic instant. The coordinator durably records the terminal
generation tombstone, rejects later results, and sends authenticated idempotent
cancel/release commands to exact Providers. Each Provider independently records
the cancellation or reaches its local hard deadline.

`DICancelAttemptV2`, `DIReleaseOfferV2`, and `DIStatusQueryV2` are
NDNSF-DI-owned opaque payloads carried by the existing generic exact-target
service/control path; they do not introduce a base-NDNSF message kind. Each
operation has its own generic control request ID and one-time authorization,
but binds the target invocation, attempt, plan/Selection when present, Provider
and boot epoch, generation, idempotency key, reason, and a deadline no later
than the inference attempt. These operations cannot change a role tuple or
extend any lease. Loss is safe because local terminal fencing and hard expiry
remain authoritative; successful delivery only accelerates convergence and
resource release.

A partitioned or non-preemptable Provider may continue consuming resources or
finish a GPU kernel until cancellation arrives or its local deadline expires.
Its output cannot revive the tombstoned invocation. Release state is explicit:

```text
IN_USE | HELD -> RELEASE_PENDING -> RELEASED | LEASE_EXPIRED
```

Verified immutable model/runtime shards may remain as non-authoritative
disk/RAM/GPU cache entries under an explicit bounded operator policy. Cache
identity includes the exact model name, content/semantics digest,
manifest/artifact digest,
graph range, backend, precision/runtime ABI, trust-policy digest, Provider boot
epoch for volatile tiers, cache epoch, and device. A selected, in-flight, or
reuse-pinned shard is not evictable before its bound expires; other eviction
advances the cache epoch. Provider restart invalidates GPU/RAM residency, while
disk residency must be reverified before advertisement.

Request input/output buffers, activations, grants, plaintext, temporary
decryption state, and every mutable state class lacking an explicit reusable
`InferenceStateContract` are request-scoped and are never converted into
cross-request model-cache entries. Exact prefix KV is a separate mutable
derived-state cache, not a model artifact: Provider-local retention requires
the exact opaque identity, same authorized security domain, bounded TTL/budget,
fresh boot/cache/entry epochs, Selection-bound pin, and immediate
revalidation defined in
`contracts/model-adapter-state-cache.md`. Cross-tenant reuse and public
repository publication are denied by default. Offer holds, prepared instances,
derived-state pins, input buffers, output staging, and keys each have bounded
ownership, expiry, and idempotent cleanup. Per-identity offer and pin quotas
plus rate limits prevent an authorized requester from exhausting capacity
through many positive ACK holds or reuse pins.

## 10. Crash recovery and protected control state

The coordinator journals before publishing a plan, each exact Selection, an
attempt supersession, cancellation, and terminal Response acceptance. To allow
byte-identical retransmission after a coordinator crash, the exact signed
Selection wire object and necessary token/lease/key-grant ciphertext are stored
only as encrypted-at-rest ephemeral control state with identity-scoped access,
expiry, audit metadata, and key erasure. Plain tokens and plaintext input keys
are never stored in ordinary evidence logs.

If protected control state cannot be recovered, the coordinator MUST NOT
construct a replacement Selection under the same attempt. It reconciles signed
Provider acceptance/status where possible, then expires or supersedes the
attempt and obtains fresh ACK/token/lease evidence.

A Provider restart creates a non-repeating random boot epoch and invalidates all
in-memory work from the old epoch. Durable token/Selection/tombstone records
prevent replay across restart. Tombstones remain at least through the maximum
message, token, key-grant, lease, and clock-skew freshness horizon.

At message receipt, a participant converts the signed absolute wire deadline
to a conservative local timer:

```text
remaining = max(0, signedDeadlineWall - receiveWall - maxClockSkew)
localExpiry = receiveMonotonic + min(remaining, configuredHardCap)
```

Token, lease, grant, artifact, and attempt expiries can only shorten this
timer. Wall time is not consulted again for the state transition. A deployment
whose observed clock uncertainty exceeds `maxClockSkew` fails the offer or
Selection closed.

## 11. Safety and liveness claims

The design claims the following safety properties:

1. no role assignment without a fresh authenticated DI offer and generic
   ProviderToken;
2. no per-Provider GPU double commitment within the DI admission ledger;
3. no role start without Selection, local readiness, verified direct inputs, and
   a live generation;
4. no same-generation duplicate execution admission;
5. no old-attempt event or partial sink set can complete the invocation;
6. no DI business semantic is interpreted by NDNSF Core;
7. no final result after a prior durable cancel/fail/expiry decision.

The design does not claim cross-Provider atomic Selection, simultaneous start,
distributed consensus, deadlock freedom, instantaneous cancellation, Byzantine
computation correctness, or physical exactly-once execution.

Successful-completion liveness holds only if authenticated messages are
eventually delivered before their deadlines, required Providers and
repositories remain available, runner/worker queues are fair, and the plan is
feasible. Under those assumptions, each eligible role eventually runs and a
complete ResultContract eventually produces a Response.

Bounded failure/release liveness has a smaller but still explicit assumption:
each surviving or recovered participant must eventually run its local monotonic
timer, journal recovery, and cleanup worker. Under that assumption, every
attempt and resource reaches terminal failure, cancellation, release, or local
expiry within the hard deadline plus a fixed cleanup bound. If local
timer/recovery fairness is also absent, only the safety properties above are
claimed; no termination bound is claimed.

## 12. Verification obligations

The reference state model and tests MUST cover:

- exhaustive valid transition coverage and rejection of every invalid
  transition;
- message duplication, loss, reorder, partition, and same-identity conflicts;
- crashes before and after DI plan validation, collaboration-plan commit,
  Selection journal/send/install/receipt,
  role-start journal, output publication, Response verification, and terminal
  journal;
- `SELECTED` versus `NOT_SELECTED`, cancel versus start/output/Response/deadline,
  expiry during install, and requester/provider restart;
- partial Selection with `UNKNOWN` reconciliation and no ProviderToken reuse;
- old-output adoption, ciphertext/key replay, semantic mismatch, and cutoff;
- multi-sink ResultContract completeness and Response idempotency/conflict;
- object segmentation, size/shape bounds, signer/revocation, AEAD, mixed
  segments, corrupt cache staging, and key cleanup;
- cache-claim mismatch, eviction before Selection, pin expiry, Provider reboot,
  disk re-verification, cache-epoch monotonicity, exact warm reuse, cold
  repository fetch, and selected/in-flight non-eviction;
- forced downgrade, non-canonical encoding, boot-epoch reuse, status
  same-sequence conflict, and replay after tombstone cleanup;
- unauthorized status/acceptance query, redaction, cancel/release control
  substitution, duplicate control requests, and loss until local expiry;
- a static owner-boundary check proving V2 DI identifiers and field parsing do
  not enter base `ndn-service-framework/` or `pythonWrapper/ndnsf/`, except for a
  finite, frozen legacy allowlist;
- a non-DI fixture proving Core's opaque payload/token/lease/status seam is
  application-independent.

Passing examples alone are insufficient. The implementation gate requires a
sequential reference model with declared state/history bounds; exhaustive
enumeration of finite transition and crash-cut cases where tractable; and
randomized/generated loss, reorder, duplicate, partition, and concurrency
schedules with retained seeds. Counterexamples must be shrunk when supported,
persisted, and replayable. The report may claim only “zero violations in the
declared bounded/model-generated history corpus,” not a general proof, unless a
separate exhaustive model-checking theorem and bounds are supplied. These
checks precede the local Docker and MiniNDN negative matrices.
