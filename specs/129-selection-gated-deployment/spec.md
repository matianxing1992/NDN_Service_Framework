# Feature Specification: Reservation-Bearing ACK and Dependency-Driven Execution

**Feature Branch**: `Experimental`

**Created**: 2026-07-21

**Revision**: R1, 2026-07-21

**Status**: Draft; R0 contracts and completed-task claims are superseded. No R1
implementation or MiniNDN acceptance is claimed.

**Input**: Revise Spec 129 so a positive ACK for an NDNSF-DI deployment request
that explicitly negotiates `DIReservationSelectionV1` means that a Provider has
created a bounded tentative resource reservation. Ordinary NDNSF applications
retain their existing ACK semantics and need not reserve exclusive resources.
After the ACK collection timeout,
the USER sends one authenticated Selection decision to every positive-ACK
Provider: selected Providers receive a recipient-encrypted exact assignment;
unselected and late positive-ACK Providers receive `NOT_SELECTED` and release
their reservation. Selected stages prepare independently and execute when
their local preparation and direct predecessor data dependencies are complete,
without a complete-set READY barrier. Keep resource policy, retry, DAG, and
model lifecycle logic in NDNSF-DI; change NDNSF only where generic targeted
Selection transport, decision closure, security, and late-ACK handling require
it. Independently define `SelectionGatedInputV1` as a generic opt-in NDNSF
security capability: REQUEST contains encrypted input, and only `SELECTED`
recipients authorized for original input receive a public-key-wrapped input key.
Both capability identifiers belong to generic Request metadata;
`SelectionGatedInputV1` MUST NOT require a DI `DeploymentIntent`, whereas
`DIReservationSelectionV1` MUST require one.
Do not introduce UAV, codec, model-family, or workload special cases.

## Baseline, Revision, and Numbering Boundary

`specs/128-generic-multiloss-recovery/` remains closed and immutable. Spec 129
R0 required REQUEST/ACK to have zero reservation side effects and required a
complete request-scoped READY barrier before any selected Provider executed.
Those two decisions are now rejected by explicit design feedback.

R0 implementation and tests remain useful migration evidence, but their former
T001--T005 checkmarks do not prove R1. R1 replaces the authority and execution
contracts before further implementation. No Spec 128 source, result, threshold,
task, or completion claim may change.

## User Scenarios & Testing

### User Story 1 - Obtain a Redeemable Resource Offer (Priority: P1)

As an inference requester using `DIReservationSelectionV1`, I receive positive
ACKs that are backed by real,
bounded Provider capacity rather than advisory availability that may disappear
before Selection.

**Why this priority**: In a resource-constrained deployment, an ACK that cannot
be redeemed creates selection races and unpredictable deployment failure.

**Independent Test**: Submit one deployment-bearing request to several
Providers. Verify that every `DIReservationSelectionV1` positive ACK follows an atomic tentative reserve,
contains a signed reservation lease, and cannot be produced when local capacity
or requester quotas are exhausted. Verify that negative ACKs reserve nothing.

**Acceptance Scenarios**:

1. **Given** an authenticated and authorized R1 deployment intent, **When** a Provider can atomically
   reserve the requested generic capacity, **Then** it returns one idempotent
   positive ACK containing the exact reservation identity, resource commitment,
   Provider boot epoch, recipient certificate binding, and expiry.
2. **Given** insufficient capacity, quota exhaustion, failed authorization, malformed intent, or a
   failed local reserve, **When** the Provider decides the request, **Then** it
   returns a negative ACK and creates no reservation.
3. **Given** duplicate delivery of the same request/attempt, **When** the
   Provider has a live reservation, **Then** it returns the same logical
   reservation and does not allocate twice or extend its lease implicitly.
4. **Given** no valid Selection decision arrives, **When** the bounded lease
   expires, **Then** the reservation is released without USER participation.
5. **Given** a normal NDNSF request that does not negotiate
   `DIReservationSelectionV1`, **When** Providers ACK it, **Then** existing
   application-defined ACK semantics remain unchanged and R1 creates no
   reservation or mandatory negative Selection.

---

### User Story 2 - Close Selection Privately and Release Every Candidate (Priority: P1)

As a requester, I close the ACK collection window once, send a separate
authenticated decision to every positive-ACK Provider, reveal each selected
Provider's exact assignment only to that Provider, and promptly release every
unselected or late reservation.

**Why this priority**: Candidate reservations amplify scarce-resource use.
Targeted positive and negative decisions are required both to commit chosen
capacity and to return unused capacity quickly.

**Independent Test**: Collect multiple positive ACKs, close the configured ACK
timeout, and select a multi-Provider plan. Verify exactly one targeted decision
per reservation, receiver-only decryption of selected assignments, immediate
release on `NOT_SELECTED`, and an immediate negative decision for every valid
positive ACK processed after closure.

**Acceptance Scenarios**:

1. **Given** the ACK timeout fires, **When** decision closure occurs, **Then**
   authenticated ACKs whose verification and decryption completed before the
   closure form the only candidate set; decision closure is atomic and final.
2. **Given** a live R1 positive-ACK reservation in the candidate set, **When** the
   USER resolves the immutable global plan, **Then** the USER sends that
   Provider one exact-target `SELECTED` or `NOT_SELECTED` decision.
3. **Given** a valid positive ACK completes after decision closure, **When** it
   is processed, **Then** it cannot enter the plan and receives an exact-target
   `NOT_SELECTED` decision using the retained decision tombstone.
4. **Given** a selected Provider, **When** it receives `SELECTED`, **Then** only
   that Provider can decrypt its minimum exact assignment projection; all
   selected projections bind one common immutable global plan digest.
5. **Given** an unselected Provider, **When** it receives `NOT_SELECTED`,
   **Then** it releases only the matching requester/request/attempt/reservation/
   boot-epoch tuple and treats duplicate decisions idempotently.
6. **Given** a lost decision, USER crash, or partition, **When** bounded
   exact-target delivery is exhausted, **Then** lease expiry remains the final
   release guarantee; no message can extend a reservation implicitly.
7. **Given** one valid Selection decision has committed or released a
   reservation, **When** a conflicting `SELECTED`/`NOT_SELECTED` decision
   arrives at any sequence, **Then** it is rejected; cancellation and abort use
   their own state transitions rather than rewriting Selection history.

---

### User Story 3 - Execute by Direct Stage Dependencies (Priority: P1)

As a selected Provider, I prepare my assigned role and begin execution as soon
as my local preparation and direct predecessor inputs are complete, without
waiting for every selected Provider to become READY.

**Why this priority**: A global complete-set barrier prevents useful pipeline
overlap and adds control latency even when a stage's own prerequisites are met.

**Independent Test**: Select a three-stage DAG. Verify that the first prepared
source stage begins without a global activation, each downstream stage begins
only after local preparation plus authenticated predecessor data, independent
branches overlap, and a failed stage aborts dependent work without accepting a
partial result as request completion.

**Acceptance Scenarios**:

1. **Given** a valid committed reservation and recipient assignment, **When**
   verify/load/warm succeeds, **Then** the Provider becomes locally READY while
   retaining the committed resources until its role completes or aborts.
2. **Given** a source stage with no predecessors, **When** it is locally READY,
   **Then** it may execute without a complete-set READY or activation message.
3. **Given** a non-source stage, **When** it is locally READY and all direct
   predecessor data for the same request/attempt/plan are authenticated,
   **Then** it may execute independently of unrelated stages.
4. **Given** missing, stale, replayed, wrong-plan, wrong-role, or wrong-sequence
   predecessor data, **When** the stage evaluates eligibility, **Then** it does
   not execute.
5. **Given** preparation or execution failure, cancellation, deadline, or
   retry exhaustion, **When** the request cannot complete, **Then** abort
   propagates to dependent stages, remaining resources are released, and no
   partial output is accepted as the final Response.
6. **Given** competing requests that acquire only subsets of their required
   reservations, **When** a complete plan cannot be formed, **Then** they send
   `NOT_SELECTED` for every acquired reservation, wait until release receipt or
   lease expiry, and retry only after bounded randomized exponential backoff;
   progress is explicitly probabilistic and starvation freedom is not claimed.

---

### User Story 4 - Read Confidential Progress on Demand (Priority: P2)

As the original requester, I can pull reservation, preparation, stage, and
request status without unsolicited progress traffic or plaintext disclosure.

**Independent Test**: Query a requester-bound opaque handle, validate and
decrypt monotonic snapshots, and reject wrong-recipient, tampered, replayed,
stale-attempt, and wrong-Provider responses. Verify no progress message is
emitted without a query.

**Acceptance Scenarios**:

1. Status queries are authenticated, exact-name, fresh, bounded, and explicitly
   initiated by the Requester.
2. Status Data is Provider-signed and encrypted only for the original
   Requester, binding reservation, plan, assignment, role, state, progress,
   reason, and sequence.
3. Polling stops on completion, failure, cancellation, expiry, deadline, or
   caller stop; cursor retrieval presents only events newer than the cursor.
4. No reservation, preparation, or stage transition produces unsolicited
   progress traffic.

---

### User Story 5 - Preserve Generic NDNSF and Produce Fresh Evidence (Priority: P3)

As a framework maintainer, I can add the minimum generic NDNSF transport and
security seams while keeping reservation, resource, retry, and pipeline policy
inside NDNSF-DI and validating the new protocol without reusing R0 evidence.

**Independent Test**: Run generic V2 compatibility/security tests, R1 contract
and binding tests, then one fresh frozen MiniNDN matrix. Verify that normal
non-DI ACK/Selection behavior remains compatible and no application identity
or model family controls generic decisions.

**Acceptance Scenarios**:

1. NDNSF owns exact-target Selection naming/transport, generic decision fields,
   receiver encryption, bounded receipt/retry, decision closure, and late-ACK
   dispatch; it does not interpret GPU, model, DAG, or backoff policy.
2. NDNSF-DI owns reservation capacity, quotas, leases, assignment projections,
   DAG eligibility, resource locking, abort propagation, and contention retry.
3. Existing non-deployment V2 REQUEST/ACK/Selection/Response, NAC-ABE,
   permissions, tokens, replay protection, and Targeted behavior remain valid.
4. C++ and Python expose equivalent R1 identities, decisions, states, reasons,
   counters, and terminal outcomes.
5. Every R1 network cell is fresh and exactly once; all failures are retained;
   Spec 128 evidence hashes remain unchanged.

### Edge Cases

- A Provider reserves capacity but crashes before publishing its ACK.
- ACK publication succeeds but USER never observes it.
- ACK is observed before timeout but verification/decryption completes after
  decision closure.
- Timeout and ACK completion are queued in the same event-loop iteration.
- A late positive ACK arrives after pending request state would normally be
  deleted but before its reservation lease expires.
- `NOT_SELECTED` is delivered before a duplicate positive ACK.
- An old negative decision arrives after a new attempt reserved new capacity.
- Provider restarts and reuses a reservation identifier under a new boot epoch.
- The selected Provider certificate rotates between ACK and Selection.
- A selected assignment ciphertext is copied to another Provider or name.
- A valid `SELECTED` is followed by a higher-sequence `NOT_SELECTED`, or the
  reverse, for the same immutable Selection decision.
- One Requester obtains many tentative reservations and sends no decisions.
- Multiple Requesters each acquire a different subset of a multi-stage plan.
- Randomized retries repeatedly collide and reach the bounded request deadline.
- A downstream stage is prepared but its predecessor fails or sends stale data.
- A stage completes after cancellation or after a newer attempt becomes active.
- One Provider serves multiple roles in the same plan.
- Decision receipt is lost although the Provider committed or released.
- A normal non-DI or shared-resource NDNSF application returns a positive ACK
  without negotiating the R1 reservation capability.
- `SelectionGatedInputV1` is negotiated without DI reservation, so the selected
  Provider needs a key grant although no reservation, plan, role, or assignment exists.
- A caller attempts to combine `SelectionGatedInputV1` with the Selection-free
  Targeted fast path.
- An input-authorized selected role terminates while another selected role is
  still running and key material must be erased without breaking that role.

## Requirements

### Functional Requirements

- **FR-001**: A deployment REQUEST MUST bind requester, request ID, attempt,
  bounded artifact/model intent, required roles, generic resource constraints,
  protected input or locator, ACK timeout, and total deadline; it MUST NOT
  embed model bytes.
- **FR-002**: Only for an authenticated, authorized NDNSF-DI deployment request
  that explicitly negotiates `DIReservationSelectionV1`, a positive ACK MUST be
  emitted only after an atomic local tentative reservation succeeds and a
  negative ACK MUST create no reservation. Without that capability, this
  reservation invariant MUST NOT be imposed on generic NDNSF ACKs.
- **FR-003**: A `DIReservationSelectionV1` positive ACK MUST carry a signed bounded `ReservationLease`
  binding requester, request, attempt, Provider, boot epoch, reservation ID,
  resource commitment, certificate identity/digest, offer digest, and expiry.
- **FR-004**: Duplicate REQUEST delivery for the same live tuple MUST be
  idempotent and MUST NOT allocate twice or implicitly extend the lease.
- **FR-005**: NDNSF-DI MUST enforce bounded per-requester, per-service, and
  Provider-wide tentative reservation quotas and expose rejection reasons.
- **FR-006**: Tentative reservations MUST release on matching `NOT_SELECTED`,
  cancellation, failure, Provider shutdown, or lease expiry; committed
  reservations MUST release on role completion, abort, or deadline. `SELECTED`
  MUST atomically commit before tentative expiry, MUST NOT resurrect an expired
  reservation, and MUST create a bounded committed execution lease.
- **FR-007**: USER MUST define `ackDeadline = requestPublishedAt + ackTimeout`
  and atomically close the candidate set when the ACK timeout callback runs.
- **FR-008**: Only authenticated R1 reservation-bearing positive ACKs whose validation and decryption
  complete before decision closure MAY enter the candidate set; every later
  valid positive ACK MUST receive `NOT_SELECTED`.
- **FR-009**: USER MUST retain a bounded decision tombstone through the maximum
  live reservation expiry so late positive ACKs can be rejected and released.
- **FR-010**: For an R1 deployment request, USER MUST send one exact-target
  Selection decision to every valid reservation-bearing positive ACK,
  including both selected and unselected Providers. This requirement MUST NOT
  apply to ordinary non-R1 NDNSF ACK collection.
- **FR-011**: Selection MUST use a versioned decision enum containing at least
  `SELECTED` and `NOT_SELECTED`, not infer release from message absence.
- **FR-012**: Every decision MUST bind requester, request, attempt, target
  Provider, Provider boot epoch, reservation ID/digest, decision sequence,
  deadline/expiry, and Requester signature.
- **FR-013**: A Provider MUST apply the first valid immutable Selection decision
  only to the exact matching live reservation, handle same-digest duplicates
  idempotently, and reject every conflicting `SELECTED`/`NOT_SELECTED`
  decision regardless of sequence. Cancellation and abort MUST use separate
  transitions and MUST NOT rewrite the Selection decision.
- **FR-014**: Exact-target decision delivery and receipt recovery MUST be
  bounded; lost `NOT_SELECTED` MUST still terminate through lease expiry.
- **FR-015**: Each `SELECTED` decision MUST bind one immutable global plan
  digest and a recipient-specific exact assignment digest.
- **FR-016**: Each exact assignment MUST be encrypted with a fresh AEAD content
  key wrapped to the authenticated recipient certificate advertised by the
  corresponding ACK; associated data MUST bind name, decision, request,
  attempt, Provider, boot epoch, reservation, plan, and assignment.
- **FR-017**: A selected Provider MUST receive only the minimum assignment
  projection needed for its role, including direct predecessor/successor data
  names where required; other Providers' unrelated assignments MUST not be
  disclosed.
- **FR-018**: A selected reservation MUST pass verify, load, bounded warm-up,
  and local readiness before executing and MUST remain locked until local role
  completion or abort.
- **FR-019**: A source stage MAY execute when selected and locally READY; a
  non-source stage MAY execute only when selected, locally READY, and all
  direct predecessor inputs for the same request/attempt/plan are authenticated.
- **FR-020**: Stage inputs MUST bind producer, role, request, attempt, plan,
  sequence/chunk, payload digest, and signature and MUST reject stale, replayed,
  wrong-plan, wrong-role, and duplicate material.
- **FR-021**: Stage failure, cancellation, expiry, and deadline MUST propagate
  a bounded idempotent abort to dependent work; partial stage output MUST NOT
  be accepted as the final request Response.
- **FR-022**: Before a contended acquisition retry, USER MUST send
  `NOT_SELECTED` for every reservation acquired by the incomplete attempt and
  wait until release receipt or lease expiry. Retries MUST then use bounded
  randomized exponential backoff, maximum attempts, and total deadline;
  documentation and evidence MUST call liveness probabilistic and MUST NOT
  claim starvation freedom.
- **FR-023**: Progress MUST remain requester-pulled; no reservation,
  preparation, readiness, stage, or release transition may push unsolicited
  progress updates.
- **FR-024**: Status queries MUST be authenticated and exact-name fresh; status
  responses MUST be Provider-signed, recipient-encrypted, strictly bound, and
  admitted monotonically after signature verification.
- **FR-025**: Polling and cursor retrieval MUST be opt-in, bounded, stop on all
  terminal outcomes, signal retention gaps, and present no duplicate event.
- **FR-026**: Generic NDNSF MUST own only capability-gated reusable targeted Selection transport,
  decision closure/dispatch, versioned decision contracts, receiver encryption,
  receipt/retry, and security fencing; NDNSF-DI MUST own resource semantics,
  quotas, leases, assignments, DAG policy, preparation, retry policy, and abort.
- **FR-027**: Existing generic V2, permission, NAC-ABE attribute routing,
  one-time tokens, replay checks, Targeted flow, and non-deployment ACK/Selection
  behavior MUST remain compatible unless `DIReservationSelectionV1` is
  negotiated. A positive ACK from an ordinary or non-exclusive NDNSF APP MUST
  NOT create an R1 reservation, decision tombstone, or mandatory negative Selection.
  `SelectionGatedInputV1` MUST fail before publication when combined with a
  Selection-free Targeted request; ordinary Targeted behavior MUST remain unchanged.
- **FR-028**: R1 contracts and states MUST have C++/Python parity and reject
  malformed, oversized, unknown-version, stale-attempt, wrong-recipient,
  wrong-epoch, tampered, and replayed material.
- **FR-029**: R0 complete READY-set and `ExecutionActivateMessage` authority
  MUST be removed from the maintained R1 execution path only after R1 tests
  pass; any temporary reader MUST be non-authoritative, observable, bounded,
  and assigned a deletion condition.
- **FR-030**: Validation MUST include deterministic unit/security/restart/
  concurrency tests plus one frozen fresh MiniNDN matrix covering selected,
  unselected, late, lost, duplicate, reorder, restart, contention, encryption,
  pipeline overlap, failure propagation, and compatibility behavior.
- **FR-031**: The R1 campaign MUST be single-writer, use unique outputs, forbid
  automatic cell retry or reuse, retain failures, and report message counts,
  reservation hold time, release latency/cause, retries, timeouts, Nacks,
  completion, stage overlap, and end-to-end latency.
- **FR-032**: Spec 128 evidence and hashes MUST remain unchanged.
- **FR-033**: When `SelectionGatedInputV1` is negotiated, application REQUEST
  input MUST be encrypted with a fresh
  symmetric content key and appear only as authenticated ciphertext or an
  authenticated encrypted-object reference. Each candidate ACK MUST carry a
  signed `SelectionInputKeyOffer` binding an encryption certificate without
  implying resource reservation. A targeted `SELECTED` message MUST carry a
  `SelectionInputKeyGrant` wrapping that input key to each exact selected
  Provider certificate authorized to consume the original input; `NOT_SELECTED`
  and unauthorized recipients MUST receive no input key. The grant MUST work
  without any reservation, plan, assignment, or DI role, while binding those
  fields when the DI capability is also negotiated. Input AEAD and wrapped
  key material MUST always bind requester, request, attempt, input
  digest/reference, recipient, expiry, and Selection; when DI is negotiated it
  MUST additionally bind plan, role, assignment, and reservation.
  Requester and Provider MUST erase unwrapped input keys and per-recipient key
  grants when their last authorized local consumer reaches a terminal state or
  the request/attempt expires; persistence MUST never store plaintext content keys.
  This capability MUST remain independent of `DIReservationSelectionV1` and
  MUST NOT silently change callers that do not negotiate it.

### Key Entities

- **DeploymentIntent**: Signed bounded request intent and timing constraints.
- **ReservationLease**: Provider-signed tentative capacity commitment.
- **SelectionInputKeyOffer**: Generic signed ACK extension advertising the
  Provider certificate used for selection-gated input, without reserving resources.
- **SelectionInputKeyGrant**: Generic targeted Selection extension wrapping the
  input key; DI plan/assignment/role/reservation bindings are conditional.
- **SelectionDecision**: Exact-target `SELECTED` or `NOT_SELECTED` resolution
  for one reservation.
- **SelectionDecisionTombstone**: Bounded USER record for rejecting late ACKs.
- **GlobalExecutionPlan**: Immutable DAG and common plan commitment.
- **ProviderAssignmentProjection**: Recipient-minimal exact role assignment.
- **RecipientEncryptedAssignment**: AEAD assignment plus certificate-wrapped key.
- **EncryptedRequestInput**: REQUEST-visible input ciphertext or encrypted
  object reference with no recipient-decryptable key.
- **DeploymentInstance**: Live Provider realization and resource-lock state.
- **StageInputEvidence**: Authenticated direct predecessor data binding.
- **StageAbort**: Bounded idempotent failure/cancellation propagation.
- **EncryptedStatusSnapshot** and **RequestEventCursor**: Pull-only progress.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Every observed `DIReservationSelectionV1` positive ACK in
  deterministic and MiniNDN tests maps to exactly one live reservation and
  every R1 negative ACK maps to zero, while non-R1 positive ACK regression cases
  create zero R1 reservations and preserve their original semantics.
- **SC-002**: Every positive-ACK reservation reaches exactly one terminal
  release cause or committed role completion, with zero orphan reservations
  after the maximum lease horizon.
- **SC-003**: Every eligible R1 reservation-bearing positive ACK receives exactly one logical targeted
  decision; every late valid positive ACK receives `NOT_SELECTED` and never
  enters the plan.
- **SC-004**: Wrong Provider, certificate, request, attempt, boot epoch,
  reservation, plan, assignment, nonce, AAD, signature, replay, and version
  cases are rejected in 100% of negative tests.
- **SC-005**: Packet/content scans find zero plaintext exact assignment fields
  outside the intended Provider, and cross-recipient decryption succeeds 0 times.
- **SC-006**: A three-stage DAG begins each stage only after local readiness and
  direct predecessor evidence, demonstrates at least one measured overlap, and
  uses zero complete-set READY activation messages.
- **SC-007**: All injected preparation/execution failures, cancellations, and
  expiries prevent final partial-result acceptance and release all remaining
  reservations within their bound.
- **SC-008**: Contention tests remain within configured attempt/deadline bounds,
  report every collision/backoff/exhaustion, and make no deterministic fairness claim.
- **SC-009**: Pull-only progress produces zero unsolicited transition messages,
  zero unauthorized plaintext status, and no duplicate cursor presentation.
- **SC-010**: Existing generic runtime/security regressions pass unchanged for
  callers that negotiate neither new capability, and four-combination tests
  prove the capabilities are independent.
- **SC-011**: C++ and Python expose equivalent fields, decisions, states,
  reasons, counters, and malformed-input behavior.
- **SC-012**: One fresh frozen MiniNDN matrix completes every unique cell once,
  preserves all negative results, records complete attribution, and verifies
  unchanged Spec 128 hashes before and after execution.
- **SC-013**: For `SelectionGatedInputV1`, packet/content scans find zero
  plaintext application input and zero input content key in REQUEST, ACK, or
  `NOT_SELECTED`; only selected recipients explicitly
  authorized for original input decrypt it, with zero cross-recipient or
  unauthorized-role success; terminal-state inspection finds zero persisted
  plaintext content keys and zero live input key beyond its last authorized consumer.
  Input-only selection succeeds with zero reservation, plan, assignment, role,
  tombstone, or negative-Selection requirement.

## Assumptions

- `ackTimeout` is the sole ACK collection window. Eligibility requires
  authentication and decryption to complete before the timeout callback closes
  the decision; no post-timeout drain extends the window.
- Reservation, mandatory negative Selection, late-ACK tombstone, and DI
  assignment semantics are gated by `DIReservationSelectionV1`; they are not
  global NDNSF positive-ACK semantics.
- Input ciphertext and selected-recipient input-key grants are independently
  gated by `SelectionGatedInputV1`. They are reusable NDNSF security semantics,
  not DI resource semantics, and can be negotiated with or without DI reservation.
- Provider-local reservation is atomic. Cross-Provider atomic acquisition is
  intentionally not introduced; leases plus randomized bounded retry provide
  probabilistic recovery from partial acquisition.
- A `DIReservationSelectionV1` positive ACK reserves capacity and queue/model-slot budget but does not by
  itself authorize fetch, verify, load, warm, or inference execution.
- Under `SelectionGatedInputV1`, REQUEST input encryption occurs before candidate discovery. Selection grants
  the existing input key only to selected roles that require original input;
  downstream-only roles receive authenticated protected predecessor data instead.
- Selection fact and target identity may remain visible in NDN names; exact
  assignment contents must remain receiver-confidential. Metadata-hiding names
  are deferred unless required by measured threat analysis.
- A stage may execute before unrelated selected stages are ready. Therefore
  partial work may occur, but only a valid terminal output role may produce the
  accepted final Response.
- Provider and Requester clocks need only support bounded expiry policy; boot
  epoch, attempt, sequence, and signed identities provide fencing.
