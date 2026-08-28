# Feature Specification: Selection-Gated Boundary Repair and Real Fault Validation

**Feature Branch**: `130-concurrent-fault-boundaries`

**Created**: 2026-07-21

**Redefined**: 2026-07-21

**Status**: Redesigned and pre-implementation audited; begin at T001 only

**Input**: Replace the withdrawn centralized-ordering version of Spec 130 with
an independent repair and real-fault validation of the unverified Spec 129
boundaries: late ACK, two concurrent Requesters, long-running resource pinning,
production randomized retry, production multi-stage dependency execution, and
NDNSF/NDNSF-DI ownership separation.

## Goal Reset and Frozen Baseline

Spec 129 and its formal matrix are immutable baseline evidence. Spec 130 MUST
use a new manifest, runner, result namespace, source snapshot, acceptance
decision, and immutable output directory. It MUST NOT rerun, append to, tune,
replace, or reinterpret any Spec 129 cell.

The earlier Spec 130 objective—an epoch-scoped centralized admission
coordinator, global conflict ordering, `DIConflictAdmissionV1`, authority
epochs, split-brain coordination, and a centralized-versus-lease-only
comparison—is withdrawn. Existing partial code, tests, runner logic, contracts,
and audit claims for that objective are unaccepted prototypes. They MUST NOT be
extended or cited as Spec 130 evidence. The implementation plan must first
isolate or remove those prototype entry points without disturbing unrelated
worktree changes.

Spec 130 does not introduce a controller or a distributed consensus protocol.
Each NDNSF-DI Provider atomically decides whether its own finite resource can be
reserved before returning a positive ACK. Each Requester closes its own ACK
window using the established selection rule. If a request cannot obtain a
usable complete set, it releases every tentative reservation, waits for release
receipt or bounded expiry, then retries after bounded randomized backoff.
Progress is therefore probabilistic and bounded; fairness and starvation
freedom are explicitly not claimed.

## Framework and Application Boundary

NDNSF is the reusable foundation. It owns generic Request/ACK/Selection/
Response transport, exact-target Selection delivery, identity, authorization,
encryption, one-time tokens, replay protection, generic opaque application
metadata, and generic post-window late-ACK lifecycle support.

NDNSF-DI is an application built on NDNSF. It owns the meaning of
`DIReservationSelectionV1`, finite resource reservation, model/resource
residency, deployment plans, exact roles and assignments, retry policy,
multi-stage dependency policy, and execution pinning. Generic NDNSF code MUST
NOT compare the literal `DIReservationSelectionV1`, construct a DI
`DeploymentPlan`, infer member roles, parse DI reservation fields, or decide DI
resource state. The application may request generic per-positive-ACK terminal
decision delivery and attach authenticated opaque application context through
framework extension seams.

`SelectionGatedInputV1` remains application-neutral: request input is encrypted
before REQUEST publication and its key is granted only to exact selected
recipients. Spec 130 must preserve that independent capability and all Spec 129
identity, token, encryption, and replay bindings while relocating DI-only
interpretation.

## Research Questions and Falsifiable Claims

- **RQ1**: Does every reservation-bearing positive ACK received after ACK-window
  closure receive an exact-target `NOT_SELECTED` decision even when another
  Provider was already selected or the main pending call has completed?
- **RQ2**: With two concurrent Requesters and no central orderer, do Provider-
  local reservations prevent double ownership while release-before-retry and
  full-jitter backoff avoid permanent hold-and-wait within declared bounds?
- **RQ3**: Can a selected long-running role keep its resource/model binding
  pinned until local completion or confirmed abort, including lease-renewal
  loss, without releasing capacity while executable work is still live?
- **RQ4**: Does the production NDNSF-DI path, rather than a local deterministic
  probe, enforce direct-predecessor stage eligibility and transport real stage
  outputs across Providers without a complete-set ready barrier?
- **RQ5**: After boundary migration, can generic NDNSF execute ordinary
  non-reserving applications without importing or interpreting DI policy while
  NDNSF-DI still closes all reservation and execution invariants?

The safety claim is falsified by any double allocation, selected late ACK left
without a terminal decision, retry carrying live ownership, capacity release
while a role can still execute, stage execution without authenticated
predecessor data, stale-attempt execution, or plaintext protected input/exact
assignment exposure. A completion failure remains a negative result even when
safety is preserved.

## User Scenarios & Testing

### User Story 1 - Close Every Late Positive ACK (Priority: P1)

A Requester that has already closed its ACK window continues to recognize
reservation-bearing positive ACKs for the same authenticated attempt. It sends
one exact-target `NOT_SELECTED` decision for each late reservation instead of
dropping the ACK merely because another Provider was selected or the normal ACK
callback is closed.

**Why this priority**: A dropped late positive ACK can retain scarce Provider
capacity until expiry and violates the per-reservation terminal-decision
contract.

**Independent Test**: Delay a real Provider ACK until after selection of another
Provider and verify decrypt/validate, tombstone match, one targeted negative
decision, Provider release, duplicate idempotency, and zero execution.

**Acceptance Scenarios**:

1. **Given** one Provider is already selected, **When** a different Provider's
   valid reservation-bearing positive ACK arrives after the ACK window, **Then**
   the late Provider receives `NOT_SELECTED` for its exact reservation.
2. **Given** the primary request callback has completed, **When** a late ACK
   arrives before its reservation horizon, **Then** a bounded post-window
   tombstone still closes it without reopening selection or response handling.
3. **Given** a duplicate late ACK with the same authenticated digest, **When**
   it is delivered again, **Then** decision delivery is idempotent and does not
   create a second application transition.

---

### User Story 2 - Resolve Two-Requester Contention Without Central Ordering (Priority: P1)

Two independent Requesters concurrently compete for one or more finite
NDNSF-DI Provider resources. Providers make only local atomic reservation
decisions. A Requester that cannot form its required selected set releases all
offers, waits for receipt or expiry, and starts a fresh fenced attempt after
production full-jitter backoff.

**Why this priority**: Spec 129's formal evidence used one Requester and did not
exercise real competing request lifecycles.

**Independent Test**: Run two Requester processes against distinct Provider
processes for disjoint, one-resource contention, and complementary partial-
reservation cases; verify unique ownership, concurrent disjoint progress,
release-before-retry, non-identical delays, bounded attempts/deadline, and no
central coordinator process or decision.

**Acceptance Scenarios**:

1. **Given** disjoint Provider/resource sets, **When** two Requesters start
   concurrently, **Then** both may progress without a global serialization
   step.
2. **Given** both Requesters compete for the same finite resource, **When** one
   Provider publishes a positive ACK, **Then** the other attempt receives no
   overlapping ownership and terminates or retries within its bound.
3. **Given** the Requesters obtain complementary partial reservations, **When**
   neither can form a complete set, **Then** both close all partial reservations
   before independently jittered new attempts; no ownership crosses attempts.

---

### User Story 3 - Pin Resources for Long-Running Roles (Priority: P1)

A selected Provider keeps the exact resource and loaded-model binding reserved
while its local role is executable. Lease expiry or renewal loss cannot make
that capacity available to another Requester until the role has completed or a
fenced abort has stopped it.

**Why this priority**: Releasing a committed lease purely by wall-clock expiry
can let another request replace a model/resource while the first role is still
running.

**Independent Test**: Execute a real long-running role while a second Requester
contends for the same slot; cross the original committed lease boundary and
inject renewal loss, completion, and abort. Verify that capacity remains pinned
until completion or confirmed stop and becomes available exactly once after
release.

**Acceptance Scenarios**:

1. **Given** a selected role is still running at its lease-renewal boundary,
   **When** another Requester asks for the same exclusive slot, **Then** the
   second request cannot reserve or execute it.
2. **Given** renewal delivery is lost, **When** the Provider reaches its local
   execution deadline, **Then** it first fences/stops the role and only then
   releases capacity; timer expiry alone never releases a live executable.
3. **Given** the role completes or a cancellation is confirmed, **When** cleanup
   runs, **Then** the pin is released once with an attributable reason and a
   later request can reserve it.

---

### User Story 4 - Execute Real Multi-Stage Dependencies (Priority: P1)

Selected Providers run an application-defined DAG using real predecessor data.
A source stage starts after its own selection and preparation. A downstream
stage starts only after its direct predecessor payload and binding are
authenticated and available; it does not wait for a global ReadySet or
ExecutionActivate barrier.

**Why this priority**: Existing classes and deterministic probes do not prove
that the production client/provider path transports and enforces stage
dependencies.

**Independent Test**: Run a three-stage chain and a fork/join plan across
separate Provider processes. Verify actual stage payload digests, direct-
predecessor gating, branch overlap, replay/stale rejection, downstream abort on
missing predecessor, and one exact terminal result.

**Acceptance Scenarios**:

1. **Given** a source role is selected and locally prepared, **When** it emits
   authenticated stage data, **Then** only its direct successors become
   eligible.
2. **Given** two independent successor branches, **When** their shared
   predecessor completes, **Then** both may overlap without waiting for all
   Providers to report ready.
3. **Given** a predecessor payload is lost, replayed from an old attempt, or
   digest-mismatched, **When** the downstream Provider evaluates eligibility,
   **Then** it does not execute and the affected request aborts/releases within
   its bound.

---

### User Story 5 - Restore DI Interpretation to the Application (Priority: P1)

An NDNSF maintainer can trace every DI reservation, deployment, role,
assignment, retry, and pinning decision to NDNSF-DI. Generic NDNSF handles only
application-neutral transport/security/lifecycle primitives and opaque
application context.

**Why this priority**: Keeping DI literal checks and DeploymentPlan synthesis in
the foundation makes other applications inherit semantics they did not request.

**Independent Test**: Run source/import scans and ordinary NDNSF regressions,
then execute the NDNSF-DI path through application callbacks. Verify zero DI
literal/resource/DAG interpretation in generic Core, unchanged non-DI ACK
semantics, and equivalent secured DI decisions.

**Acceptance Scenarios**:

1. **Given** an ordinary NDNSF service, **When** it publishes a positive ACK,
   **Then** no reservation or mandatory negative Selection is implied.
2. **Given** an NDNSF-DI request, **When** its ACK and Selection flow runs,
   **Then** NDNSF-DI creates and interprets reservation/plan/assignment state
   through generic framework hooks.
3. **Given** generic NDNSF source and public bindings, **When** boundary scans
   run, **Then** they contain no DI capability literal, model/resource policy,
   role inference, or DI DeploymentPlan construction.

---

### User Story 6 - Produce Real, Independent Fault Evidence (Priority: P2)

A researcher receives a new exact-once MiniNDN confirmation whose fault cells
run real requester/provider processes on distinct hosts and exercise the
production network/runtime path. Local counter manipulation or deterministic
probe output cannot satisfy a live cell.

**Why this priority**: The previous Spec 129 confirmation did not validate the
six boundaries above, and the withdrawn Spec 130 runner simulated several
outcomes locally.

**Independent Test**: Freeze the manifest before live execution; run every
unique cell once; verify per-host process/NFD evidence, real message or process
fault injection, complete event lineage, retained failures, and unchanged Spec
129 hashes.

**Acceptance Scenarios**:

1. **Given** a formal fault cell, **When** it executes, **Then** its effect is
   evidenced by actual provider delay, link `netem`/routing manipulation,
   process signal/restart, or real payload suppression at the named production
   seam—not by assigning an expected counter.
2. **Given** a failed or unavailable cell, **When** analysis runs, **Then** it
   remains in the denominator and is not automatically rerun or replaced.
3. **Given** the campaign completes, **When** hashes and invocations are audited,
   **Then** every Spec 130 case has one invocation and Spec 129 evidence is
   byte-for-byte unchanged.

### Edge Cases

- A late positive ACK arrives after another Provider's final response but before
  the late reservation expires.
- The same late ACK is reordered before and after its decision receipt.
- Two Requesters use the same request ID under different identities.
- Delayed ACK/Selection/stage data from attempt N arrives during attempt N+1.
- Both Requesters draw very small backoffs; collision repeats until maximum
  attempts or the absolute deadline.
- One release receipt is lost even though Provider release succeeded.
- A role crosses multiple renewal periods and then completes at the deadline
  boundary.
- Provider restart occurs while an old worker may still hold a resource.
- A source stage finishes before a downstream Provider completes local
  preparation; data remains bound but does not bypass selection/preparation.
- Fork branches finish in either order; join executes only after both exact
  predecessor bindings exist.
- A non-DI application uses capability fields whose names resemble DI fields;
  generic Core still treats them as opaque.
- A formal cell starts with two logical Providers on one host/NFD; preflight
  must reject it rather than claim distributed evidence.

## Requirements

### Functional Requirements

- **FR-001**: Spec 129 source evidence, formal results, hashes, and
  interpretation MUST remain unchanged; Spec 130 MUST use independent files,
  commands, output paths, and acceptance decisions.
- **FR-002**: Spec 130 MUST NOT introduce or depend on a central/global
  admission coordinator, global request order, conflict graph authority,
  authority epoch, Paxos/Raft/2PC substitute, or post-publication ACK reordering.
- **FR-003**: Existing central-coordination prototype code, tests, contracts,
  manifest cases, and `PASS` audit claims MUST be classified as superseded and
  MUST be isolated or removed before new behavior is accepted.
- **FR-004**: A valid reservation-bearing positive ACK received after ACK-window
  closure MUST be decrypted, authenticated, matched to its exact requester,
  request ID, attempt, Provider, boot epoch, reservation ID, and digest, then
  receive an exact-target `NOT_SELECTED` decision.
- **FR-005**: Late-ACK closure MUST work when another Provider is already
  selected, when the normal ACK callback has closed, and while the bounded
  attempt tombstone remains valid; it MUST NOT reopen selection or application
  response completion.
- **FR-006**: Same-digest duplicate late ACK/decision delivery MUST be idempotent;
  conflicting, stale-attempt, stale-boot, or tampered late ACKs MUST fail closed.
- **FR-007**: Tombstone retention MUST cover the maximum advertised reservation
  horizon plus bounded decision-delivery time and MUST be garbage-collected
  with attributable counters. State MUST be bounded by the original authorized
  Provider set, a per-request Provider limit and global/per-identity quotas;
  lifecycle capacity MUST be secured before REQUEST publication, and a live
  liability MUST NOT be evicted early to admit a newer request.
- **FR-008**: Two concurrent Requesters MUST be represented by distinct
  identities, processes, request attempts, and lifecycle evidence.
- **FR-009**: Each NDNSF-DI Provider MUST atomically reserve only its own
  authoritative finite resources before publishing a positive DI ACK; no
  central process may pre-authorize that ACK.
- **FR-010**: No exclusive resource MAY have more than one live owner, and
  disjoint Requesters MUST remain able to progress concurrently.
- **FR-011**: A Requester unable to form a complete selected set MUST send
  `NOT_SELECTED` for every positive reservation and MUST wait for accepted
  release receipt or bounded expiry before another attempt.
- **FR-012**: A new attempt MUST use fresh attempt identity, tokens, input/plan
  bindings, and reservation state; it MUST inherit no live ownership from a
  prior attempt.
- **FR-013**: Production contention retry MUST use full-jitter exponential
  backoff with a non-deterministic production entropy source, configurable base
  and cap, maximum attempts, and one absolute request deadline.
- **FR-014**: Deterministic randomness injection MAY exist only as a test seam;
  production callers MUST NOT use a fixed seed or fixed synchronized delay.
- **FR-015**: Retry logic MUST be wired into the maintained NDNSF-DI client
  request path and exercised by real Request/ACK/Selection messages; fixture or
  launcher-only invocation is insufficient.
- **FR-016**: Retry evidence MUST report attempt, collision, release wait,
  sampled backoff, exhaustion, deadline, and terminal outcome separately; no
  fairness or starvation-free claim is permitted.
- **FR-017**: After a selected reservation enters executable state, its exact
  resource/model binding MUST remain pinned until local role completion or
  confirmed fenced abort.
- **FR-018**: A committed/executing reservation MUST NOT be released solely
  because a timer expires while the role can still access the resource.
- **FR-019**: Renewal failure or execution deadline MUST transition through a
  stop/fence state; capacity becomes reusable only after the old executable is
  confirmed stopped or the Provider process/boot epoch is fenced.
- **FR-020**: Completion, cancellation, deadline abort, Provider restart, and
  shutdown MUST each produce one attributable terminal pin/release outcome;
  stale completion MUST NOT release a newer attempt's resource.
- **FR-021**: A contending Requester MUST remain unable to reserve the same
  exclusive resource throughout the complete pinned interval and MUST become
  eligible only after its terminal release boundary.
- **FR-022**: Production NDNSF-DI MUST execute source and downstream stages from
  an application-defined acyclic dependency plan without complete-set
  ReadySet/ExecutionActivate authority.
- **FR-023**: A source stage MAY start after its own selection, committed pin,
  and local preparation; a downstream stage MUST additionally validate every
  direct predecessor's requester, request ID, attempt, role, chunk/sequence,
  payload digest, and authorization binding.
- **FR-024**: Independent successor branches MUST be able to overlap; a join
  MUST wait only for its declared direct predecessors.
- **FR-025**: Missing, late, replayed, tampered, stale-attempt, or
  wrong-predecessor stage data MUST NOT make a downstream stage eligible and
  MUST lead to bounded abort/release attribution.
- **FR-026**: Retry and dependency executors MUST be called by maintained
  NDNSF-DI client/provider entry points and not only by unit tests, plan
  factories, or experiment-local deterministic probes.
- **FR-027**: Generic NDNSF MUST NOT compare `DIReservationSelectionV1`, build or
  interpret a DI `DeploymentPlan`, infer DI roles/members, parse DI reservation
  policy, or own DI retry/resource/pinning/dependency state.
- **FR-028**: NDNSF-DI MUST own and validate the DI capability, reservation,
  plan, exact assignment, role, retry, dependency, and pinning semantics.
- **FR-029**: NDNSF MAY expose generic opaque application metadata and generic
  callbacks for ACK qualification, per-target terminal Selection decisions,
  recipient encryption, and post-window tombstones, but those interfaces MUST
  not encode DI field names or resource policy.
- **FR-030**: Ordinary NDNSF ACK/Selection behavior MUST remain compatible and
  non-reserving unless an application itself supplies reservation semantics.
- **FR-031**: `SelectionGatedInputV1`, targeted recipient confidentiality,
  NAC-ABE routing, one-time tokens, replay protection, and signer/identity
  validation MUST remain intact during ownership migration.
- **FR-032**: No UAV, codec, model-family, sensor-field, or workload-name branch
  MAY be added to NDNSF or NDNSF-DI control policy.
- **FR-033**: The formal manifest MUST contain a finite, explicit boundary
  corpus covering late ACK, two-Requester disjoint/contention/partial cases,
  long-task pin/renewal/abort, production retry success/exhaustion, multi-stage
  chain/fork-join/failure, and non-DI compatibility/boundary negatives.
- **FR-034**: Every formal live cell MUST use separate MiniNDN hosts/processes
  for its Requesters and Providers, with one NFD per host, and MUST record
  identities, PIDs, topology, routes, source hashes, and process exit status.
- **FR-035**: Fault effects MUST arise from real provider timing, link
  delay/loss/reorder/routing manipulation, payload suppression at the production
  seam, or process signal/restart. Assigning synthetic success/failure counters
  or calling a deterministic local probe cannot satisfy a formal cell.
- **FR-036**: The formal campaign MUST freeze its manifest before live work,
  acquire single-writer ownership, refuse an existing output directory, execute
  each unique cell exactly once, perform no automatic retry, and retain every
  failure or unavailable outcome.
- **FR-037**: The analyzer MUST separately report Payload, Mapping,
  new-Mapping-information ratio, retry, timeout, Nack/rejection, ACK,
  SELECTED/NOT_SELECTED, decision receipt, reservation/pin/release, stage-data,
  execution, safety, availability, and terminal-cause metrics, using explicit
  unavailable values when a denominator does not exist.
- **FR-038**: Before live execution, the full source build, forced Python
  binding/package rebuild, affected C++/Python/security regressions, source
  boundary scans, runner dry gate, and Spec 129 hash preflight MUST pass.
- **FR-039**: Spec 130 MUST run a fresh code-aware pre-implementation audit
  after this goal reset; any audit or completed task from the withdrawn central
  design provides no implementation authorization.

### Key Entities

- **Attempt Tombstone**: Bounded requester-side post-window record containing
  the authenticated attempt and already closed reservation decisions needed to
  reject late positive ACKs idempotently.
- **Provider Reservation**: Provider-local finite resource claim that is
  tentative after positive ACK, committed after `SELECTED`, and terminal after
  release/expiry/abort.
- **Execution Pin**: Provider-local binding between an executable role and its
  exact resource/model residency; it prevents replacement until completion or
  confirmed fenced stop.
- **Contention Attempt**: One fresh identity-bound request attempt with its own
  deadline, offers, decisions, release barrier, sampled backoff, and terminal
  outcome.
- **Dependency Plan**: NDNSF-DI application DAG containing exact roles,
  Providers, direct predecessors, data bindings, and plan digest.
- **Stage Data Evidence**: Authenticated predecessor payload lineage including
  attempt, producer role, consumer role, chunk/sequence, digest, and replay
  state.
- **Boundary Extension**: Application-neutral NDNSF hook carrying opaque
  authenticated context without interpreting DI semantics.
- **Formal Cell Evidence**: Immutable topology, process, fault, message,
  reservation, pin, dependency, and terminal evidence for one exact-once cell.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Every valid reservation-bearing positive ACK that arrives after
  ACK-window closure in the formal late-ACK cases maps to exactly one targeted
  `NOT_SELECTED` terminal decision and zero Provider executions.
- **SC-002**: Across all concurrent Requester cases, simultaneous ownership of
  one exclusive resource by more than one attempt is zero and every partial
  reservation closes before retry.
- **SC-003**: Every production retry case records that its maintained caller used
  production entropy and sampled full-jitter delays within the per-attempt
  configured bound, never exceeds maximum attempts or the absolute deadline,
  and carries zero live reservation across attempts. Coincidentally equal
  samples are not treated as proof of fixed delay.
- **SC-004**: Every disjoint two-Requester case records overlapping progress
  without a central coordinator or global order event.
- **SC-005**: In every long-task case, the contending attempt receives zero
  ownership while the first role remains executable; release precedes the next
  accepted reservation and occurs only after completion or confirmed stop.
- **SC-006**: Timer expiry alone releases an executing pin zero times across
  normal, renewal-loss, cancellation, deadline, restart, and shutdown cases.
- **SC-007**: Every accepted downstream stage execution has complete
  authenticated direct-predecessor evidence; unauthorized or stale downstream
  executions are zero.
- **SC-008**: The fork/join case records at least one interval of successor
  branch overlap and exactly one join execution after both predecessor results.
- **SC-009**: Maintained production entry-point coverage demonstrates at least
  one real caller of randomized retry and dependency execution outside tests,
  plan factories, and experiment-local probes.
- **SC-010**: Generic NDNSF source/binding scans find zero DI capability literal,
  DI DeploymentPlan synthesis, DI role inference, or DI resource-policy branch;
  ordinary non-DI regression behavior remains unchanged.
- **SC-011**: Security negatives reject 100% of wrong-recipient, tampered,
  replayed, stale-attempt, stale-boot, wrong-predecessor, and plaintext-leak
  cases before execution.
- **SC-012**: Every formal cell contains real host/process/NFD and fault-effect
  evidence; cells backed only by deterministic local probes or assigned counters
  are rejected by the analyzer.
- **SC-013**: One fresh exact-once Spec 130 confirmation executes every frozen
  cell once, retains all negatives, creates all required immutable artifacts,
  and leaves Spec 129 hashes unchanged before and after.
- **SC-014**: Application-generality scans find zero UAV, codec, model-family,
  sensor-field, or workload-name special case in the repaired control path.

## Assumptions

- Provider-local atomic reservation plus bounded leases prevents double
  ownership; randomized release-and-retry provides bounded probabilistic
  progress but not deterministic fairness.
- A production entropy source is available. Tests may inject deterministic
  entropy only to verify bounds and reproducibility.
- The Provider can fence or stop its local executable before releasing a timed-
  out pin. If it cannot confirm that boundary, capacity remains unavailable
  rather than being unsafely reassigned.
- Real MiniNDN evidence can use workload-neutral opaque stage payloads and a
  finite exclusive slot/model-binding fixture; GPU hardware is not required to
  prove the control invariant, and no hardware-performance claim is made.
- The existing generic NDNSF security and exact-target transport primitives are
  reusable, but any unavoidable new Core seam must be application-neutral and
  pass the ownership scan.
