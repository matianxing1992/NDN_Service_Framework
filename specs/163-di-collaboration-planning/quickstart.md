# Validation Quickstart: DI Collaboration Planning

## Model-family and inference-state acceptance fixtures

Before any real-model performance run, exercise the same public
`APPClient.request(model, task, input, options, timeout)` path, forwarded to
`AutomaticPlanningCoordinator.request()`, with:

1. an LLM text-generation adapter whose state contract includes ordinary
   request KV and optional `EXACT_PREFIX_KV_V1`;
2. an object-detection adapter with image input, detection result schema, and
   `STATELESS` or `REQUEST_SCOPED` state;
3. an opaque-container adapter that emits one unsplittable graph node.

All three must use `begin_collaboration(DEFERRED) -> ACK_CLOSED ->
commit_plan -> Selection -> CollaborationContext -> Response`. Base NDNSF logs
and static checks must contain no prompt, token, logits, sampling, KV, model,
graph, split, GPU, or role semantic parsing.

For complete generation, use `GenerationRequest`/`APPClient.generate()` rather
than a token callback:

```text
one full input token sequence
  -> one begin_collaboration(DEFERRED)
  -> one ACK_CLOSED (optionally early-closed by validated role coverage)
  -> one post-closure graph/split/placement decision and commit_plan
  -> internal prefill/autoregressive decode
  -> one complete Response
```

The complete-response form is the normative production contract. It amortizes
Request/ACK/Selection, authentication, planning, artifact preparation, and
distributed-stage coordination over the whole generation. Providers may
exchange hidden states, KV records, and token-step control records internally,
but those records are data-plane details and never reopen this NDNSF
invocation. A future presentation layer may render the finished answer
incrementally, but that is not a per-token NDNSF call or a second planning
round.

The ACK coverage predicate is an optimization hint only. It cannot carry a
precomputed split or assignment, and it cannot reopen or replace the immutable
ACK_CLOSED snapshot. The existing one-token collaboration loop remains a
diagnostic stress fixture, not the application API.

Run the derived-state matrix with byte-sized fake state before Qwen:

```text
cold clean compute
request-scoped terminal cleanup
exact authorized Provider-local prefix-KV hit
model/content/semantics mismatch
adapter/runner ABI or digest mismatch
split/manifest/role/layer-range mismatch
prefix-token/position/context mismatch
precision/KV-layout mismatch
tenant/security-domain denial
boot/cache/entry epoch mismatch
expiry and eviction
Provider restart
pin/reservation loss
migration disabled or failed -> clean compute/replan
```

For every row, retain the state profile/key digest, sanitized hit/miss reason,
covered layers/prefix length, avoided-work evidence, bytes/tier, epochs,
fallback, cleanup, and frozen-reference result. Never retain prompts, token
IDs, plaintext KV, keys, or unrestricted cache membership. An exact hit may
avoid only the covered prefill work; it does not waive model-shard, capacity,
Selection, input, deadline, or result validation.

## Hard boundary

This feature has no default live TigerCluster validation step. Do not run `sbatch`,
`srun`, `scancel`, `scontrol requeue`, model download, remote artifact
promotion, or remote cache mutation without separate explicit user
authorization.

Validation progresses from pure contracts to local fake-artifact execution and
then MiniNDN with a frozen `Qwen/Qwen3-0.6B` `ModelRef` containing exact
content and semantics digests. Model weights are
unnecessary for the lower tiers, but the final MiniNDN real-output and
cold/warm-cache gate uses this small model. TigerCluster or a larger model
requires a separate explicit authorization.

## 0. Ownership and migration boundary

Before lifecycle tests, statically verify that base NDNSF owns only generic
Request/ACK/Selection/Response transport, identities, signatures, permissions,
one-time tokens, opaque payload/lease/status/deadline/replay mechanics, and
that a non-DI service still works. Fail if base NDNSF defines, imports, parses,
or branches on model, split, GPU, role, artifact, preparation, tensor, DAG,
result, or DI-recovery semantics. Existing V1 DI branches must be explicitly
quarantined migration debt, not V2 authority.

The allowed generic extensions are:

1. the existing Collaboration API's application-independent
   `begin_collaboration -> ACK_CLOSED -> commit_plan` deferred seam; and
2. the generic Core-owned encrypted Selection-decision WAL and opaque
   participant API in
`contracts/core-opaque-selection-transaction.md`. Verify with a non-DI
participant that Core atomically owns only token/opaque-lease disposition and
opaque bytes/digests, while application projections remain idempotent and
semantically external.

The first seam must also pass a non-DI fixture:

```text
begin_collaboration(DEFERRED)
  -> one Request
  -> immutable ACK_CLOSED snapshot
  -> exactly one same-invocation opaque commit_plan
  -> existing final Selection / CollaborationContext / Response
```

Verify byte-identical commit retry is idempotent and early, conflicting,
expired, or cross-invocation commit produces zero Selection. Verify existing
roles/dependencies-before-Request callers remain source-compatible and are
traced as `PREPLANNED`.

## 1. Public API and import boundary

Verify that planning, splitter, catalog, and ACK-offer/Selection-assignment
contracts import without optional model runtimes:

```bash
PYTHONPATH=build/python:pythonWrapper:NDNSF-DistributedInference \
python3 -m unittest -q \
  test_ndnsf_di_placement_strategy \
  test_ndnsf_di_presplit_catalog
```

Expected:

```text
all tests pass
no Torch, Transformers, ONNX Runtime, CUDA, NFD, or model import is required
```

## 2. Pure strategy tests

Use small immutable fake candidates and provider snapshots to prove:

- pre-split-first deterministic selection;
- exact model/semantics/backend matching;
- only signed `ACK=true` snapshots carrying a validated
  `DIProviderOfferV2` are eligible assignment offers;
- every planned role fits the Provider's advertised capability envelope;
- aggregate required GPU RAM for roles assigned to one Provider is no greater
  than its ACK-offered or reserved available GPU RAM;
- stale ACK and boot-epoch rejection;
- exact pinned-GPU, reload-safe-GPU, host-RAM, disk, repository-only, then
  new-materialization ranking;
- default graph-valid dynamic `SplitSpecification` when no feasible pre-split
  exists;
- runtime-peak capacity accounting including activation, KV/workspace,
  transient overhead, and safety margin rather than file bytes alone;
- deterministic near-equal split only for homogeneous graph units and equal
  usable GPU envelopes;
- safe no-feasible result for unknown bounds or insufficient capacity;
- custom strategy replacement;
- timeout, exception, malformed result, and candidate-budget rejection;
- explicit operator-trusted, allowlisted, digest-pinned strategy execution;
- confirmation that the in-process timeout is not treated as a security
  sandbox or hard-preemption boundary;
- no token, raw input, writable path, client, or device object crosses the
  strategy boundary.

Expected: all rejected cases have zero catalog, network, file, and device side
effects.

## 3. Catalog and materialization tests

With byte-sized fake artifacts:

- register the same canonical manifest twice and confirm idempotency;
- reject alias collision with a different digest;
- reject missing, corrupt, cyclic, incomplete, or incompatible manifests;
- reject an untrusted artifact signer or incompatible verification policy;
- distinguish trusted executable adapter/runner/publisher TCB from an
  authenticated but unsafe container, pickle, custom operator, or native code;
- clean incomplete staging state within a bounded lifetime;
- retire and revoke before new preparation without deleting evidence;
- materialize only the selected generated specification;
- publish content-addressed segments and an atomic signed manifest through
  NDNSF-DistributedRepo before Selection;
- require an explicit `ArtifactPreparationMode`: generated candidates execute
  `materialize -> publish -> commit`, while exact pre-splits execute
  `resolve_existing -> commit` without rematerialization;
- reject materialization/publication exception, role or digest mismatch,
  non-absolute artifact Data name, and deadline expiry with zero plan commit
  and zero Selection;
- reject partial, corrupt, revoked, or wrong-policy repository publication;
- prove provider residency comes from ACK state rather than static catalog
  state;
- verify boot/cache epoch, expiry, reuse pin, reload fallback, selected/in-flight
  non-eviction, and request-state destruction.

## 4. ACK offers, exact Selection assignments, and asynchronous preparation

Run a local fake three-role flow:

```text
one generic Request -> 3 generic ACKs carrying signed DI offers
        -> immutable ACK_CLOSED -> NDNSF-DI strategy and validation
        -> materialize + publish immutable split if no exact manifest exists
        -> commit_plan on the same generic collaboration invocation
        -> one generic Selection per selected Provider carries an opaque,
           complete DI role/fragment/artifact/dependency/resource tuple
        -> pure DI prepare callback
        -> one Core WAL COMMITTED record: token/opaque lease + encrypted DI blob
        -> idempotent DI projection + mandatory Selection acceptance
        -> role-local asynchronous preparation + authenticated input events
        -> role-local execution gates -> DI result contract -> generic Response
```

Negative matrix:

```text
stale ACK
provider restart
tampered plan
missing validated DI offer carried by a positive ACK for a selected Provider
role capability requirements outside the DI-offer envelope
aggregate assigned GPU RAM greater than DI-offered/reserved available GPU RAM
two concurrent requests attempt to claim the same GPU-RAM budget
artifact digest mismatch
ACK-offer or SelectionToken replay
input-key grant tamper or cross-role/cross-attempt replay
role-local preparation expiry
cancel while selected roles are still preparing
wrong-request, wrong-attempt, or cross-Provider token substitution
early cleanup timer versus accepted hard deadline
conflicting duplicate final Selection
commit_plan before ACK_CLOSED
commit_plan after invocation expiry
cross-invocation or conflicting second commit_plan
byte-identical commit_plan retry
incomplete or incremental same-Provider role tuple
crash before/after DI prepare, Core WAL append/fsync, DI projection, and receipt
torn WAL tail, unavailable storage key, callback failure, or blob digest mismatch
lost Selection acceptance followed by identical retry/status query
partial final-Selection delivery
input arrives before local readiness
local readiness arrives before input
duplicate concurrent readiness/input events
post-Selection local preparation failure
cancel versus last required input
failure versus local-ready event
deadline expiry versus execution start
old-attempt output after replan
old output reused without AdoptedInputEvidence
input/output object name, lineage, schema, segment, digest, signer, or expiry substitution
AEAD nonce reuse, associated-data substitution, or plaintext-digest disclosure
noncanonical encoding or schema/capability/acceptance-predicate downgrade
multiple terminal sinks without an explicit aggregation contract
signed but semantically invalid Provider output
cancel versus terminal Response
partitioned cancellation and late output
unauthorized status/acceptance query or secret-bearing status payload
cancel/release control substitution, replay, loss, or deadline extension
```

Expected:

- successful case admits each fake role at most once per generation, prepares
  it once in the test schedule, and accepts one terminal result; it does not
  claim physical exactly-once GPU computation;
- evidence shows one `begin_collaboration(DEFERRED)`, one Request, one immutable
  ACK closure, one same-invocation plan commit, one immutable logical
  final-Selection identity per selected Provider, separately counted bounded
  wire retries, one mandatory durable per-Provider acceptance record, and no
  second role-negotiation/ACK round or global acceptance cover;
- the default Spec 163 call-path trace is `DEFERRED`; a separate fixed-plan
  compatibility trace is `PREPLANNED`; neither uses placeholder roles, and
  neither creates a DI-private Request/Selection/data/status/Response path;
- every selected Provider has one fresh signed `ACK=true` carrying a validated
  `DIProviderOfferV2` bound to the request/attempt, Provider boot epoch,
  capability/resource envelope, and expiry;
- every exact role assignment fits the selected Provider's signed offer, and
  aggregate required GPU RAM for all roles assigned to one Provider is no
  greater than its exclusively offered or reserved available GPU RAM;
- two concurrent requests cannot obtain overlapping promises for the same GPU
  budget;
- a Provider assigned two fake roles receives one Selection bundle; a pure
  participant callback produces one opaque blob, one Core WAL fsync atomically
  commits the ProviderToken/opaque-lease disposition and encrypted blob/
  acceptance bytes, and NDNSF-DI projects the ledger, tuple, grants, and
  generation idempotently; identical retry returns the same acceptance, while
  an incomplete or incremental bundle is rejected;
- a lost acceptance leaves delivery `UNKNOWN`; it never invents acceptance,
  rejection, or a cross-Provider barrier;
- an invalid, missing, stale, expired, or insufficient ACK offer produces zero
  authorized role executions;
- incomplete local readiness alone does not block final Selection;
- Stage 0 executes when selected, locally ready, and given its direct request
  input even while Stage 1 or Stage 2 is still preparing;
- every ACK-offered/reserved or prepared resource becomes selected, released,
  or expired;
- preparation and inference share one request/attempt, and one final Selection
  per selected Provider consumes its existing ProviderToken (SelectionToken)
  exactly once;
- strategy execution, split materialization, publication, and other blocking
  work runs outside the Face event loop;
- only final Selection installs recipient-encrypted per-role/per-input key
  grants, and any grant tamper or cross-binding replay is rejected;
- final Selection starts request-scoped asynchronous local preparation, which
  may reuse eligible prewarm/cache work, while its handler performs no
  synchronous cold fetch/load/warm;
- while one downstream Provider's final Selection is being retried, an already
  selected, locally ready source role with verified input still executes;
  permanent delivery failure cancels or replans the affected dependency closure
  and cannot produce an accepted partial terminal Response;
- every inter-role object satisfies its complete manifest; multiple sinks are
  rejected unless the sealed `ResultContract` defines deterministic
  aggregation;
- cancel/expiry/Response races accept only the first local terminal CAS and
  partitioned cancel converges without reviving a generation;
- cross-attempt output reuse requires explicit `AdoptedInputEvidence`;
- legacy and new profiles never mix or silently downgrade.

## 5. Dependency graph tests

Use the same executor for:

```text
chain: stage0 -> stage1 -> stage2

fan-out/fan-in:
             -> branch-a -
source-role               -> merge-role
             -> branch-b -
```

Expected:

- Stage 0 starts as soon as Selection, its local model, and request input are
  ready; it does not wait for Stage 1 or Stage 2;
- when Stage 1's model is ready first, it starts automatically when Stage 0's
  authenticated output arrives;
- when Stage 0's output arrives first, Stage 1 retains it and starts
  automatically when local preparation completes;
- merge waits for both signed input references;
- both branches independently verify the shared upstream object;
- one ready fan-out branch does not wait for another branch that is still
  preparing;
- concurrent duplicate events cannot admit a role more than once per
  generation;
- cancellation, failure, expiry, or superseding attempts permanently fence
  late role events from starting work or waking consumers;
- terminal output matches a frozen local reference;
- failure compensation never accepts a partial merge result.

## 6. Model/property-based lifecycle history

Generate deterministic histories that reorder, duplicate, lose, and replay
Request, ACK, Selection, acceptance, status, object, cancel, deadline, and
Response events. Inject a restart at every cut of the Provider
token/opaque-lease/DI-ledger/tuple/grant/generation/acceptance transaction and
at requester-journal writes.

Define a sequential reference model and explicit state/history bounds. Enumerate
finite transition and crash-cut cases exhaustively where tractable, then run
generated/randomized schedules with retained seeds. Shrink counterexamples when
supported and preserve every failure as a replayable fixture. The acceptance
claim is only “zero violations of at-most-once admission, one accepted terminal
result, capacity exclusion, terminal fencing, idempotent bounded release, and
safe `UNKNOWN` recovery in the declared bounded/history corpus.” Example traces
alone are insufficient, and property testing is not described as a general
proof.

## 7. Local Docker fake-payload closed loop

Start one NFD, one Controller, one requester, and three Providers with
byte-sized artifacts and payloads. Exercise real authorization, ACK metadata,
strategy selection, signed ACK-offer validation, exact assignments in final
Selection, status after Selection, interleaved role-readiness/dependency
references, permission denial, NAC-ABE attribute routing, plaintext-permission
rejection, ACK/Response UserToken mismatch, and Response without loading a real
model.

Resource boundary:

```text
--memory=4g --memory-swap=5g
```

Expected: complete secured single-Request lifecycle, zero CPU/GPU model
execution, bounded encrypted-at-rest pending/control state, and durable
ACK-offer/plan/Selection-acceptance/result evidence.

## 8. MiniNDN acceptance

Run a frozen matrix with a `Qwen/Qwen3-0.6B` `ModelRef` containing exact model
manifest and semantics digests, covering:

- cold-cache dynamic split, NDNSF-DistributedRepo publication, NDN Provider
  fetch, verification, disk/RAM/GPU preparation, and a complete real answer;
- exact pre-split and warm-cache success;
- cache mismatch, unpinned eviction, Provider restart invalidating GPU/RAM
  state, disk re-verification, and corrupt/revoked shard rejection;
- custom strategy success;
- real fan-in/fan-out;
- Selection without a validated DI offer carried by a positive generic ACK;
- exact assignment outside a selected Provider's advertised envelope;
- aggregate per-Provider GPU RAM assignment exceeding its ACK offer;
- concurrent requests contending for the same GPU-RAM offer;
- one Provider receiving a complete two-role Selection tuple with one token,
  plus incomplete/incremental tuple rejection;
- loss of mandatory Selection acceptance, `UNKNOWN`, identical retry, and
  restart cut-point recovery;
- input-key grant tamper and cross-role/cross-attempt replay rejection;
- input/output object substitution, noncanonical encoding, downgrade, and
  signed-but-invalid result handling;
- upstream execution while downstream Selection delivery is still retrying,
  followed by bounded affected-closure failure if delivery never succeeds;
- selected provider preparation failure before role execution;
- provider failure after upstream output with safe dependency-closure
  compensation;
- input-before-model and model-before-input event orders;
- explicit cross-attempt adoption, cancel-versus-Response, and partitioned
  cancel convergence;
- permission denial, NAC-ABE routing, plaintext-permission rejection,
  ACK/Response UserToken mismatch, tamper, replay, stale boot epoch, expiry,
  and mixed-version rejection.

Retain negative rows. Do not tune away failures.

Use five real prompts with at most 64 generated tokens. Perform one unmeasured
warmup, then five measured repetitions per prompt. Retain complete answers,
reference-consistency results, TTFT, per-token latency, total latency, tokens/s,
split/materialization/publication/fetch/disk/RAM/GPU preparation timings, cache
tier and hit reason, bytes fetched, utilization, success, recovery, and their
distributions.

An exact warm GPU-reuse row passes only with zero re-split, zero
re-publication, zero repository model-byte fetch, and zero GPU shard reload. If
the MiniNDN host has no CUDA, run the network/correctness/CPU-or-host-cache
parts but mark the GPU-specific row `DEFERRED`; do not report simulated GPU
evidence.

## Exit and optional large-model requalification

Spec 163 does not require TigerCluster. A separate Spec 162 large-model
campaign may be reactivated only after:

1. this Spec has no audit `BLOCK`;
2. public API, default strategy, catalog, preparation lifecycle, and generic DAG
   path are implemented;
3. ownership, unit, model/property-history, negative, security, local Docker,
   and MiniNDN gates pass;
4. maintained documentation agrees on the new authority boundary;
5. Spec 162 adds a fresh runtime/evidence requalification task;
6. a new explicit live authorization is obtained.
