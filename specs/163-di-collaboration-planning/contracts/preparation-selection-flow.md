# ACK Offer, Final-Selection, and Dataflow Contract

## Layering decision

The canonical model/task-first application operation is one
`APPClient.request()` call, forwarded to
`AutomaticPlanningCoordinator.request()`, returning one durable invocation
handle. The deployment-taking `InferenceApplication.request(...)` remains an
explicit preplanned compatibility surface. The public invocation may contain bounded,
non-overlapping wire attempts. An attempt may terminate before Request
publication; it publishes at most one inference Request. Any attempt reaching
Request publication publishes exactly one, and any attempt reaching ACK closure
closes exactly one candidate set. It then plans from those ACKs, sends final
Selection, prepares each selected role, executes the role DAG, and accepts at
most one terminal result under the attempt identity.

Base NDNSF owns only the generic Request/ACK/Selection/Response transport,
identity, signatures, NAC-ABE authorization, one-time tokens, opaque
lease/status/deadline/replay mechanics, and byte payload carriage. The
existing generic Collaboration API is the lifecycle carrier. Its
application-independent deferred extension owns
`begin_collaboration`, immutable `ACK_CLOSED`, and exactly one opaque
`commit_plan` for the same invocation. These are local API/state boundaries,
not new wire message kinds. Existing callers that supply roles/dependencies
before Request use the same state machine in `PREPLANNED` compatibility mode;
Spec 163 uses `DEFERRED`.

The
`DIRequestEnvelopeV2`, `DIProviderOfferV2`, `DISelectionAssignmentV2`,
`DISelectionAcceptanceV2`, role-preparation, tensor-DAG, result, and recovery
semantics in this contract are owned and validated by
`NDNSF-DistributedInference`. The normative ownership, trust, persistence, and
state-machine rules are in
[lifecycle-security-contract.md](lifecycle-security-contract.md).

```text
generic begin_collaboration(DEFERRED)
  -> generic NDNSF Request carrying DIRequestEnvelopeV2
  -> generic positive ACKs carrying signed DIProviderOfferV2
       offer means willingness to accept any DI-compatible role set
       whose aggregate resource requirement fits the advertised envelope
  -> immutable ACK_CLOSED snapshot
  -> automatic split and provider-role plan
       prefer exact reusable manifest/residency;
       otherwise derive a capacity-safe SplitSpecification
  -> trusted materialization + immutable NDNSF-DistributedRepo publication
       when the split does not yet exist
  -> generic commit_plan(same invocation, complete plan)
  -> one exact-target generic Selection carrying DISelectionAssignmentV2
       per selected Provider
       atomically assigns that Provider's complete role tuple
  -> mandatory per-Provider DISelectionAcceptanceV2 evidence
  -> selected Providers revalidate GPU reuse or fetch by NDN, verify, persist,
       and load locally and asynchronously
  -> Selection + LocalReady(role) + InputsReady(role)
  -> dependency-driven role execution/status
  -> DIResultEnvelopeV2 accepted under ResultContract
  -> generic NDNSF Response
```

There is no preparation Request, `PreparationCommit`, commit receipt,
commit-acceptance cover, second ACK round, preparation Selection, or separate
execution-activation barrier.

## Positive ACK is the role-neutral offer

Base NDNSF interprets `ACK.status=true` only as a generic positive ACK and
treats its payload as opaque. When trusted NDNSF-DI code successfully validates
a signed `DI_PROVIDER_OFFER_V2` payload, the DI profile gives that offer a
stronger meaning than an advisory telemetry sample:

> Until the ACK offer expires, the Provider is willing to accept one final
> Selection for any compatible non-empty set of roles in this request whose
> aggregate verified resource requirement fits the ACK's advertised envelope.

The roles do not need to be known when the ACK is produced. The validated
`DIProviderOfferV2` carried by the positive generic ACK binds:

```text
requester / service / invocationId / requestId / attempt
DI_PROVIDER_OFFER_V2 schema, capability version, and canonical-encoding version
immutable model-intent digest
Provider identity and boot epoch
supported runtime/backend/precision facts
offered or reserved GPU RAM
queue and estimated preparation facts
bounded disk/RAM/GPU/repository shard-residency entries, cache epoch,
reuse-pin/lease expiry, eviction and reload facts
bounded opaque reusable-state offers: state-key digest, profile, role/layer
range, bytes, tier, access-domain digest, boot/cache/entry epochs, expiry,
pin and optional migration capability
resource sequence, captured-at time, and expiry
accepted total deadline
optional admission/reservation lease
one-time ProviderToken
status handle and authenticated ACK digest
acceptance-predicate version and digest
```

The baseline feasibility rule is:

```text
aggregate estimated peak GPU RAM of roles assigned to Provider p
    <= GPU RAM reserved by p's validated DI offer
```

Peak GPU RAM includes weights, runtime workspace, configured KV-cache/batch
budget, and a declared safety margin. Other advertised compatibility facts
remain hard constraints, but ACK does not contain a fixed list of future stage
identities.

ACK evaluation has no model splitting, artifact fetch, model load, warmup, or
execution side effect. NDNSF-DI owns a GPU/resource admission ledger and must
make concurrent DI offers non-overlapping until Selection, explicit release, or
expiry. Base NDNSF may carry and enforce an opaque bounded lease identifier, but
it does not know GPU units, model fragments, or DI feasibility. A purely
observational memory sample is not sufficient for a binding DI offer.

Each residency entry binds exact model/tokenizer and manifest/artifact digests,
graph range, backend, precision/runtime ABI, trust policy, byte count, tier,
device, boot epoch, cache epoch, observation time, and expiry. Ranking may
prefer an exact GPU-resident shard, but hard feasibility may rely on it only
while a bounded reuse pin/lease protects it through Selection expiry or while
a separately feasible reload path exists. Reboot invalidates GPU/RAM claims;
disk entries require renewed verification.

The external placement strategy sees only a sanitized
`ProviderPlanningView`. It never receives the raw ProviderToken, admission
lease proof, input ciphertext/key material, mutable device handle, network
client, or status-control authority.
It also never receives prompt text, token IDs, plaintext KV, decryption keys,
or unrestricted cache-membership information. Reusable-state evidence and
lifecycle follow
[model-adapter-state-cache.md](model-adapter-state-cache.md).

## One-attempt sequence

```text
Requester                    Provider A/B/...              NDNSF-DI coordinator
    | begin_collaboration(DEFERRED)   |                              |
    | Inference Request              |                              |
    |------------------------------->|                              |
    | generic ACK + opaque signed    |                              |
    | DI offer + ProviderToken       |                              |
    |<-------------------------------|                              |
    | ACK_CLOSED immutable snapshot  | sanitized ACK snapshots      |
    |                                |----------------------------->|
    |                                | split + role plan            |
    |                                |<-----------------------------|
    |                                | materialize/publish if new   |
    |                                |----------------------------->|
    | commit_plan(same invocation)   |                              |
    | final Selection carrying       |                              |
    | complete Provider role tuple ->| Core validates generic auth  |
    |                                | DI validates offer/plan/role  |
    |                                | crash-atomic local commit     |
    | DI Selection acceptance       <| + async local preparation    |
    |                                |                              |
    | local-ready event OR input     | re-evaluate affected role    |
    | arrival ---------------------->|                              |
    | execute once when all local    |                              |
    | latches are satisfied          |                              |
    | operation status / outputs     | wake direct consumers        |
    |<------------------------------>|                              |
    | terminal inference Response    |                              |
    |<-------------------------------|                              |
```

Every validated DI offer reaches one bounded terminal outcome:

- `SELECTED`: the ProviderToken and any admission lease are consumed for an
  exact assignment inside the offer;
- `NOT_SELECTED`: unused tentative resources are released;
- `EXPIRED`, `FAILED`, or `CANCELLED`: pending authority and resources are
  tombstoned or released idempotently.

## Planning and Selection invariant

Before issuing positive final Selection, trusted NDNSF-DI code validates:

1. every role is assigned exactly once and each selected Provider has exactly
   one complete recipient projection;
2. every assigned Provider supplied a fresh, authenticated positive generic
   ACK carrying a validated DI offer for the same request, attempt, service,
   model intent, and boot epoch;
3. each assignment is compatible with that DI offer and the aggregate GPU-RAM
   requirement does not exceed its reserved budget;
4. any new split has been completely materialized and its signed immutable
   NDNSF-DistributedRepo manifest is retrievable; partial staging is absent;
5. the sealed plan, artifact digests, dependency graph, deadlines, and ACK
   snapshot digests are internally consistent;
6. each selected ProviderToken and optional admission lease is fresh and
   unused;
7. no terminal cancellation, failure, or superseding attempt invalidated the
   plan.

Final Selection is the first request-scoped message that assigns concrete
roles. Exactly one immutable logical `SELECTED` decision identity/digest exists
for each selected Provider in an attempt; its recipient-specific projection
atomically contains every role assigned to that Provider:

```text
schema and capability version
requester / service / requestId / attempt
request and validated ACK-snapshot digests
sealed collaboration-plan digest
Provider identity and boot epoch
complete non-empty role-assignment tuple and bundle digest
per-role model fragment and runtime artifact references/digests
per-role direct predecessor and successor bindings
per-role backend/device requirement and aggregate required peak GPU RAM
preparation and total deadlines
optional reservation identifier/resource-binding proof
one-time ProviderToken proof
opaque signed-status handle
per-role/per-input recipient-encrypted input-key grants and canonical grant
digest when applicable
requester signature
```

Provider processing has two non-interchangeable layers. Base NDNSF first
validates the live pending Request, policy epoch, requester signature, recipient
encryption, one-time ProviderToken, generic lease/deadline, and exact
Selection-message replay identity. It then passes only the authenticated opaque
payload and verified identity context to NDNSF-DI. NDNSF-DI validates the
offer/acceptance-predicate version and digest, boot epoch, canonical schema,
plan and assignment digests, resource bound, complete role tuple, artifact and
object manifests, and every input-key grant's
request/attempt/plan/Provider/boot/role/input/expiry binding.

Acceptance uses the generic mechanism in
[core-opaque-selection-transaction.md](core-opaque-selection-transaction.md).
NDNSF-DI's pure participant validation returns one canonical commit blob and
acceptance payload with no side effect. One Core-owned encrypted WAL
`COMMITTED` record atomically consumes the ProviderToken, records the opaque
lease disposition, and makes those opaque bytes authoritative without parsing
them. NDNSF-DI then projects the DI ledger, tuple, grants, and latches
idempotently from that record. A successful transaction:

```text
fsyncs one generic Core WAL record containing token/lease disposition,
encrypted opaque DI commit blob, and acceptance payload
projects the exact DI resources, complete role tuple, bound key grants,
and Selection latches by transaction ID
exposes the persisted DISelectionAcceptanceV2 payload
starts request-scoped asynchronous local preparation for those roles,
reusing eligible prewarm/cache work
```

Incremental same-attempt role additions and a second `SELECTED` Selection for
the same Provider are forbidden. Reassignment requires a new attempt, a fresh
ACK, and a fresh ProviderToken.

The same authenticated Selection wire object may be retransmitted under that
unchanged identity/digest. Retransmission is idempotent: it does not create a
second decision, reinstall the tuple, or consume the ProviderToken again.
Changing, omitting, adding, or replaying a key grant changes the canonical
Selection digest and is rejected.

There is no second Provider decision. Rejecting a fresh, authentic,
in-envelope Selection is a Provider failure or protocol violation, not a normal
role-negotiation outcome. A stale, replayed, wrong-boot, wrong-plan, or
out-of-envelope Selection fails closed.

`DISelectionAcceptanceV2` is mandatory linearization evidence for the local
transaction. It binds the request/attempt/plan/Provider/boot epoch, exact
Selection digest, installed role-bundle digest, committed-resource digest,
acceptance sequence, and Provider signature. A duplicate identical Selection
returns the same recorded acceptance without a second token consumption or
tuple install.

Until the requester has verified this evidence, that Provider's Selection
delivery state is `UNKNOWN`, not accepted or rejected. The requester may retry
the identical Selection or query the authenticated status handle. Acceptance
records are per Provider: they are never collected into a global acceptance
cover and never delay an already eligible role at another accepted Provider.

## Role-local preparation and status

Request-specific verification, fetch, load, and warmup begin asynchronously
after valid Selection. An exact GPU-resident shard may satisfy the model-ready
latch immediately only after Selection-time revalidation of its digest,
compatibility, boot/cache epoch, device binding, freshness, revocation, and
pin/reload safety. Otherwise the Provider fetches the immutable named shard
through NDN from NDNSF-DistributedRepo, verifies all segments and the signed
manifest, atomically promotes it to local disk, and loads it through host RAM
to the assigned GPU. Existing cache residency or provider prewarm does not
create pre-Selection assignment authority.

At terminal convergence, an operator policy may retain verified immutable
model/runtime shards in disk/RAM/GPU caches for later requests. Selected,
in-flight, or explicitly pinned shards are not evictable. Request inputs,
outputs, activations, temporary plaintext, grants, and all mutable state without
an explicit reusable `InferenceStateContract` are released and never
advertised as model residency. Exact prefix KV may be retained only as
Provider-local `EXACT_PREFIX_REUSABLE` derived state under exact model,
semantics, adapter/runner, split/layer, prefix-token, position, precision,
layout, security-domain, epoch, TTL, budget, pin, and revalidation rules. Its
`StateReuseBinding` is sealed into plan and final Selection; ACK evidence alone
grants no reuse authority. Cache eviction, restart, pin loss, or
incompatibility causes clean computation or bounded failure/replanning; it
must never execute with approximate or unauthorized state.

The signed operation-status path binds an opaque status handle to:

```text
request and attempt
plan and Selection digests
Provider identity and boot epoch
role and prepared instance
model/runtime artifact digests
actual backend/device/resource binding
monotonic status epoch/sequence
freshness and expiry
Provider signer and signature
```

Status strings such as `FETCHING`, `VERIFYING`, `LOADING`, `WARMING`,
`WAITING_INPUTS`, `RUNNABLE`, `EXECUTING`, `READY`, or `FAILED` are diagnostic.
A verified `RoleLocalReadyReceipt` satisfies only that role's local-preparation
latch. It is not a prerequisite for issuing Selection to another Provider and
is never collected into a global ReadySet.

## Role execution invariant

For role `r`:

```text
eligible(r) =
    validFinalSelection(r, request, attempt, plan, bootEpoch, generation)
    AND freshLocalPreparation(r)
    AND every required direct input is complete and authenticated
    AND r has not started
    AND the attempt generation is not cancelled, failed, expired, or superseded
```

Source-role request input becomes accessible only through final Selection.
Downstream inputs are signed, digest-verified outputs from declared direct
predecessors. Selection validation, local-preparation completion, and each
input arrival re-evaluate only the affected role. The first successful
transition atomically starts the role once under the
request/attempt/plan/role/boot-epoch/execution-generation binding.

If local readiness arrives first, the role waits only for missing inputs. If
all inputs arrive first, their verified references are retained while the role
waits only for local preparation. A completed role publishes authenticated
output references that wake only its direct consumers.

For a linear `stage0 -> stage1 -> stage2` plan, Stage 0 starts as soon as it has
valid Selection, local readiness, and request input, even if Stages 1 and 2 are
still preparing. Stage 0 output sets Stage 1's input latch. Stage 1 starts
immediately if locally ready, or when its later local-ready event arrives. The
same rule repeats for Stage 2.

## Partial Selection delivery

Exact-target Selection delivery is not cross-Provider atomic and does not
create a new barrier. A Provider that has durably accepted Selection may
prepare and, when its local/data dependencies permit, execute while another
Selection is `UNKNOWN` and still being retried.

The requester retries each missing exact target within the total deadline. If a
required Selection cannot be delivered or validated, the affected dependency
closure is cancelled, compensated, or replanned. Partial output can never
become the terminal inference Response.

## Deadline, release, and restart

ACK timeout closes candidate discovery; it is not the lifetime of the complete
request. The final Selection deadline is no later than:

```text
min(request total deadline,
    every selected positive-ACK offer expiry,
    every selected ProviderToken expiry,
    every selected admission-lease expiry)
```

Provider pending state stores one wire deadline plus a local monotonic
remaining-time budget and one authoritative, generation-fenced cleanup timer.
Signed messages bind the sender wall-clock instant, allowed skew, and expiry;
local scheduling never relies on synchronized wall clocks. Duplicate Request,
ACK, Selection, status observation, or retry does not extend the budget.

A Provider restart changes its boot epoch and invalidates old ACK offers,
ProviderTokens, reservations, Selections, local-ready evidence, and outputs.
Failure, cancellation, requester loss, replay, or deadline expiry releases or
tombstones pending and selected resources and retains only bounded audit
evidence.

The requester persists only the minimum exact bytes needed to retransmit or
abort an active Selection, encrypted at rest and bounded by the attempt
deadline. The Provider crash-atomically persists the acceptance record and
resource/tuple decision. After restart, missing or contradictory local evidence
fails closed: the requester observes `UNKNOWN` and retries, queries status, or
starts a new attempt; the Provider never reconstructs authority from
unauthenticated logs.

## Failure, compensation, and replanning

Before Selection, a missing validated DI offer, insufficient GPU-RAM envelope,
stale offer, invalid plan, failed materialization, cancellation, or expiry produces
no role execution. Unselected ACK offers are explicitly released or expire.

After Selection:

- local preparation or physical GPU allocation failure fences that role and
  its unfinished downstream dependency closure;
- a Provider that cannot honor a valid in-envelope ACK offer is recorded as a
  Provider failure;
- declared compensation may reassign only the failed or unfinished dependency
  closure;
- reusable intermediate output must satisfy an explicit
  `AdoptedInputEvidence` record: signed and digest-verified source object,
  semantic/schema/segment equivalence, old and new attempt/plan lineage,
  authorizing policy, expiry, and new consumer binding;
- otherwise the safe action is abort or a new attempt.

A replan increments `attempt` and uses fresh ACKs, ProviderTokens,
reservations, plan, and Selection bindings. Terminal or superseded-generation
tombstones defeat late local-ready, input, output, and status events.

## Output, result, and Response

Every inter-role object is described by an `InputOutputObjectManifest` that
binds producer and consumer roles, invocation/attempt/plan lineage, tensor
schema and segment bounds, content name, size, digest, AEAD suite/nonce/key
grant reference, signer, production sequence, and expiry. A signature proves
origin and integrity, not that a malicious Provider computed the tensor
correctly; semantic attestation or redundant computation is outside the
baseline claim.

Non-misuse-resistant AEAD requires a unique nonce for every key/object/segment;
associated data binds the complete canonical identity, lineage, schema,
segment, policy, and expiry. Public manifests expose ciphertext digests only;
the plaintext digest is keyed or recipient-encrypted. Opaque object components
reduce name disclosure, but NDN routing prefixes, ciphertext size/segment
count, timing, and traffic relationships remain observable baseline metadata.

Each sealed plan contains exactly one `ResultContract`. It identifies the
accepted terminal role set and aggregation rule. Multiple sinks require an
explicit deterministic DI aggregator role or a complete aggregation rule;
otherwise plan validation fails. NDNSF-DI accepts at most one
`DIResultEnvelopeV2` for an attempt after verifying the ResultContract, output
manifests, terminal generation, lineage, and the recomputed required
Provider/Selection-acceptance-set digest. Every Provider in the result
dependency closure must be durably accepted; `UNKNOWN`, failed, cancelled,
expired, or not-selected deliveries cannot satisfy success. Base NDNSF then
carries that opaque result in one generic Response. A local first-terminal-wins
compare-and-set resolves Response-versus-cancel/expiry races.

## Cancellation and convergence

Cancellation is not a distributed atomic operation. The requester and each
Provider independently perform a generation-fenced local terminal
compare-and-set, stop admitting new work, tombstone late events, and release
resources idempotently. Authenticated cancellation/status messages are retried
until observed or until the deadline; partitions may delay convergence but
cannot revive an expired generation. Work already executing may finish
physically, but its late output cannot be admitted into a terminal result.

## No revived global barrier

The accepted path has no `PreparationCommit`, `PreparationCommitReceipt`,
`CommittedRoleCover`, complete `PreparedSet`, `READY_HELD`, `allRolesReady`,
network-published ReadySet, or `ExecutionActivateMessage`.

The plan must be complete and every assignment must be backed by a validated DI
offer carried by a positive generic ACK, but there is no second cross-Provider
acceptance cover.
The requester-side `commit_plan` is not Provider preparation or willingness:
it only seals the complete generic collaboration plan and authorizes the
existing final-Selection machinery to proceed. Provider acceptance remains the
separate per-Provider Selection transaction.
Per-Provider Selection delivery, role-local preparation, and direct-input
availability are independent latches; there is no simultaneous-start or
distributed atomic-commit claim. Selection delivery may remain pending for one
Provider while roles at another selected Provider execute.

## Compatibility

- The new path is explicitly negotiated inside
  `DIRequestEnvelopeV2`/`DIProviderOfferV2`; it is not a model-aware enum added
  to base NDNSF.
- `ACK.status=true` has the binding role-neutral DI-offer meaning only after
  NDNSF-DI validates that profile; legacy opaque ACK payloads are not silently
  reinterpreted.
- Existing one-time ProviderToken, generic ACK metadata, optional
  opaque leases, exact-target Selection, `CollaborationContext` assignment/
  data/status/Response operations, and signed operation status are reused from
  Core.
- Existing roles/dependencies-before-Request calls are `PREPLANNED`
  compatibility calls. Spec 163 dynamic planning defaults to `DEFERRED`; it
  MUST NOT inject placeholder roles or use a coordinator-only collaboration
  followed by a parallel DI lifecycle.
- `SELECTION_LOADS_V1` remains quarantined legacy DI behavior until migrated
  out of base NDNSF; it is never the implicit fallback for a V2 attempt.
- One attempt never mixes legacy synchronous-loading semantics with V2
  asynchronous preparation/dataflow semantics. Unknown schema, capability,
  canonical-encoding, or acceptance-predicate versions fail closed rather than
  downgrade.
- Current implementations that publish advisory positive ACKs, lack a bound
  GPU-RAM offer, fail to validate the Selection assignment against the ACK
  digest, or retain a complete READY/activation barrier do not yet satisfy this
  contract.
