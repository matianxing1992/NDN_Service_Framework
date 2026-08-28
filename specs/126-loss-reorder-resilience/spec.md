# Feature Specification: Loss and Reordering Resilience

**Feature Branch**: `Experimental`

**Created**: 2026-07-19

**Status**: Complete (2026-07-20)

**Input**: Define and implement the next formal feature after Spec 125 to cover
its unverified packet-loss and bounded-reordering boundary. Preserve the
accepted zero-loss implementation and evidence; diagnose and repair only
failures demonstrated by deterministic or frozen network evidence.

## User Scenarios & Testing

### User Story 1 - Recover a Partially Lost Sample (Priority: P1)

As a live-stream consumer, I continue receiving usable video when one item from
a sample is lost but an authenticated repair item or a bounded retransmission
can complete the sample before its playout deadline.

**Why this priority**: Loss recovery is the first correctness boundary left
unverified by Spec 125. A sample-atomic scheduler is not useful if one missing
item permanently blocks later complete samples.

**Independent Test**: Inject one missing source, one missing repair, and one
source-plus-repair loss into deterministic sample traces. Verify exact recovery
or an explicit deadline outcome, bounded retry work, and forward progress.

**Acceptance Scenarios**:

1. **Given** one source item is absent and the signed repair item is available,
   **When** repair arrives before the playout deadline, **Then** the missing
   source is reconstructed exactly once and the complete sample is delivered.
2. **Given** a source or repair Interest times out transiently, **When** useful
   time remains, **Then** the same exact semantic name is retried within a
   bounded budget without duplicating application delivery.
3. **Given** recovery cannot finish before the deadline, **When** the sample is
   abandoned, **Then** the reason is explicit and later complete samples
   continue rather than deadlocking behind the gap.

---

### User Story 2 - Accept Bounded Reordering (Priority: P1)

As a live-stream consumer, I accept authenticated Mapping, source, and repair
Data in any valid arrival order and emit media in source sequence without
mistaking late valid Data for corruption or delivering duplicates.

**Why this priority**: Multiple exact Interests are intentionally in flight.
Arrival order is therefore not an integrity property and must not become a
hidden failure condition.

**Independent Test**: Permute Mapping blocks, source items, repairs, validation
completion, and timeout callbacks around group boundaries. Verify one delivery
per source item, ordered decoder input, and unchanged fail-closed validation.

**Acceptance Scenarios**:

1. **Given** all signed items of a sample arrive before their deadline in a
   different order, **When** validation completes, **Then** the sample is
   delivered once in canonical source order with no false gap.
2. **Given** a repair arrives before the source it may replace, **When** the
   late source subsequently arrives, **Then** exactly one authenticated source
   value is admitted and duplicate work cannot reach the decoder.
3. **Given** a stale-session, conflicting-name, invalid-signature, malformed
   extent, or digest-discontinuous item arrives out of order, **When** it is
   validated, **Then** it remains rejected under the Spec 125 security rules.

---

### User Story 3 - Sustain Video Under Matched Impairments (Priority: P2)

As an operator, I can run a 60-second UAV video session under isolated loss,
isolated bounded reordering, and their combination and receive a complete,
machine-readable account of continuity, recovery, retry work, and failures.

**Why this priority**: Deterministic tests establish correctness, but only the
real MiniNDN/UAV/GUI path shows whether recovery completes inside live playout
deadlines.

**Independent Test**: Execute the preregistered frozen impairment matrix once,
with unique result directories and no automatic retry, and evaluate every cell
against declared gates.

**Acceptance Scenarios**:

1. **Given** the matched zero-loss profile, **When** the upgraded code runs for
   60 seconds, **Then** it preserves the Spec 125 correctness and performance
   gates.
2. **Given** isolated loss or reordering, **When** the session completes, **Then**
   the report separates recovered loss, retransmitted loss, deadline skips,
   duplicates, late arrivals, decoder gaps, and lifecycle failures.
3. **Given** any frozen repetition fails, **When** results are summarized,
   **Then** the failure remains an admissible measured outcome and is not
   replaced, silently retried, or tuned away.

---

### User Story 4 - Preserve the Accepted Contract (Priority: P3)

As a framework maintainer, I can upgrade loss/reordering behavior without
changing Mapping v2, semantic Data names, application APIs, security ownership,
or the accepted Spec 125 zero-loss artifacts unless a separate reviewed spec
revision proves such a change necessary.

**Why this priority**: This feature closes an evidence and state-machine gap;
it is not authorization for a new protocol or a redesign of working behavior.

**Independent Test**: Run golden-wire, C++/Python parity, malformed-input,
security, and zero-loss regressions and compare the original Spec 125 evidence
directories byte-for-byte before and after the work.

**Acceptance Scenarios**:

1. **Given** an existing valid Mapping v2 session, **When** loss/reordering
   handling is upgraded, **Then** its wire encoding and public API remain
   unchanged.
2. **Given** the retained Spec 125 result directories, **When** Spec 126 runs,
   **Then** none is modified or reused as an output directory.

### Edge Cases

- A Mapping block is delayed while later payload names are not yet resolvable.
- A payload timeout callback races with matching Data reception or validation.
- Repair arrives first, source arrives late, or both arrive after recovery.
- More than one source in the same XOR group is unavailable.
- The missing item is a repair rather than a source.
- A predicted suffix is terminal-unproduced while an earlier real source is
  delayed.
- Reordering crosses a Mapping block boundary or a key/delta sample boundary.
- The decoder reorder buffer reaches its bound while the earliest item is
  missing.
- The stream stops or changes session while retries or validation are pending.
- Malformed, replayed, stale-session, or name-mismatched Data arrives during
  recovery.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST preserve Spec 125 Mapping v2 wire encoding,
  semantic Data names, exact-name Interests, public sample APIs, and security
  ownership.
- **FR-002**: The system MUST treat arrival order as non-authoritative while
  continuing to authenticate name, signer, session, Mapping continuity, group
  extent, and item kind before admission.
- **FR-003**: Each cursor MUST have one authoritative lifecycle across pending,
  received, validating, recovered, delivered, skipped, and terminal states;
  racing callbacks MUST NOT produce two terminal outcomes.
- **FR-004**: A source item MUST reach the application at most once even when
  Data, timeout, retry, validation, and repair completion race.
- **FR-005**: Valid source items MUST reach decoder input in canonical media
  order, independently of network or validation completion order.
- **FR-006**: When exactly one source is missing and a valid selected repair is
  available before the playout deadline, recovery MUST reconstruct the same
  opaque source bytes and admit them through the normal application boundary.
- **FR-007**: A repair that cannot recover the group, including multiple-source
  loss, MUST NOT fabricate or partially deliver a source item.
- **FR-008**: Mapping and payload retry budgets MUST be finite, independently
  bounded, and observable by cursor, attempt, trigger, and terminal outcome.
- **FR-009**: Retries MUST reuse the original authenticated semantic name and
  MUST NOT create a new cursor, group, session, or application-visible item.
- **FR-010**: Recovery and retry MUST honor a bounded live usefulness deadline;
  work that cannot complete in time MUST terminate explicitly instead of
  delaying later complete samples without bound.
- **FR-011**: Skipping an unrecoverable sample MUST preserve forward progress
  and MUST distinguish deadline expiry, retry exhaustion, retention expiry,
  invalid evidence, and insufficient repair.
- **FR-012**: Late valid Data for an already recovered, delivered, skipped, or
  stopped cursor MUST be classified and ignored without altering decoder state.
- **FR-013**: Reordering MUST NOT train the sample-size predictor twice, change
  signed group boundaries, or convert normal late arrival into an
  underprediction event.
- **FR-014**: All pending, completed, retry, recovery, reorder, duplicate, and
  diagnostic state MUST remain bounded and MUST be cleared on stop or session
  replacement.
- **FR-015**: C++ and Python-visible status MUST report equivalent aggregate
  counts for retry attempts, recovered sources, late arrivals, duplicates,
  deadline skips, retry exhaustion, and maximum reorder depth.
- **FR-016**: Deterministic tests MUST cover every listed edge case before any
  impairment campaign begins.
- **FR-017**: The frozen network matrix MUST contain a matched zero-loss
  regression, isolated loss, isolated bounded reordering, and combined
  loss-plus-reordering while holding workload, duration, bitrate, width, FEC,
  topology endpoints, logging, warm-up, and policy constant.
- **FR-018**: Each planned network command MUST run exactly once and write a
  unique result directory. Failures, timeouts, malformed evidence, and process
  crashes MUST be retained as measured outcomes and MUST NOT trigger automatic
  reruns.
- **FR-019**: Network evidence MUST record the effective impairment on both
  endpoints, the exact command and environment, process ownership, start/stop
  times, return codes, and enough packet/recovery counters to distinguish loss
  from reordering and application failure.
- **FR-020**: A source change after the frozen matrix starts MUST invalidate all
  later cross-cell comparisons; defect closure requires a documented new
  uniquely named confirmation series rather than selective replacement.
- **FR-021**: No Mapping v3, new wire field, public API mode, codec parsing in
  Core, automatic bitrate tuning, host-NFD final validation, Docker, iTiger, or
  physical-device experiment is in scope. If current contracts are proven
  insufficient, implementation MUST stop for an explicit spec revision.
- **FR-022**: The original Spec 125 acceptance and `confirm01` through
  `confirm06` directories MUST remain unchanged and are contextual evidence,
  not control repetitions for the new code revision.

### Key Entities

- **CursorLifecycle**: One cursor's session, semantic name, group binding,
  attempt count, deadline, current state, and exactly one terminal outcome.
- **RecoveryGroupState**: Authenticated group membership, received sources,
  selected repair, recovered index, deadline, and terminal recovery reason.
- **ReorderObservation**: Arrival rank, canonical source rank, lateness,
  maximum pending depth, and final disposition without payload contents.
- **ImpairmentCell**: Frozen loss/reordering profile, workload constants,
  repetitions, exact command identity, and admissibility rules.
- **RunEvidence**: Immutable run path, effective impairment, process/return
  status, continuity metrics, recovery counters, latency distribution, and
  acceptance verdict.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Deterministic permutations and timeout/Data races produce zero
  duplicate application deliveries, zero out-of-order decoder inputs, zero
  double-trained groups, and one terminal outcome for every exercised cursor.
- **SC-002**: Every exactly-one-source-loss case with a valid timely repair is
  reconstructed byte-for-byte; every multiple-source-loss case fails closed
  without fabricated or partial source delivery.
- **SC-003**: Retry and recovery state never exceeds configured cursor/group
  bounds, and stop/session replacement leaves zero pending callbacks capable of
  changing the new session.
- **SC-004**: The new zero-loss regression completes 60 seconds with all Spec
  125 gates: continuous final ten seconds, zero frame gap, no more than 15%
  Interest overhead, at least 99% Provider-confirmed future hits, capture-to-
  decode p95 at most 250 ms, and p99 at most 500 ms.
- **SC-005**: All isolated-reordering repetitions complete without a stream
  lifecycle failure, duplicate decoder delivery, or decoder stall longer than
  one second during the final ten seconds.
- **SC-006**: At least four of five isolated-loss repetitions and four of five
  combined repetitions complete the 60-second GUI window, have no stream
  lifecycle failure, and have no decoder stall longer than one second during
  the final ten seconds. Exact binomial confidence intervals and every failed
  repetition are reported; no population-level reliability claim is made.
- **SC-007**: In every impaired run counted as a successful repetition under
  SC-005 or SC-006, Interest work is no more than 25% above actual source-plus-
  selected-repair items, future-hit success is at least 95%, and capture-to-
  decode p95 is at most 300 ms and p99 at most 600 ms. Failed repetitions still
  report every available metric but are not required to satisfy success-only
  performance thresholds.
- **SC-008**: All frozen commands have exactly one recorded invocation, all
  expected result directories are unique, and the Spec 125 evidence tree has
  identical file hashes before and after Spec 126.

## Assumptions

- The first frozen matrix uses 1% independent link loss and a bounded delay-
  based reordering profile; exact network parameters are preregistered in the
  implementation plan before execution.
- Isolated loss, isolated reordering, and their combination are boundary probes,
  not proof over all MANET or wireless processes.
- Five impaired repetitions per profile provide a transparent engineering
  acceptance boundary and exact intervals, not high-powered hypothesis testing.
- Existing Mapping and payload bounded retries, one-repair FEC, and decoder
  reorder buffering are reused and repaired before any new mechanism is
  considered.
- MiniNDN is the final network environment until algorithm development is
  complete. Hardware, host NFD, and automatic tuning remain out of scope.
- Spec 125 `confirm06` remains the accepted result for its original code
  revision; Spec 126 adds a new matched zero-loss regression rather than
  rewriting that historical claim.
