# Feature Specification: Generic Multi-Loss Recovery and Bounded Future-Interest Retry

**Feature Branch**: `Experimental`

**Created**: 2026-07-20

**Status**: Draft

**Input**: Define Spec 128 from Spec 127's negative result. Add application-neutral
multi-loss recovery and finite future-Interest retry: cover both a one-item
stream without FEC and a multi-segment stream for which one XOR repair is
insufficient. Do not add UAV, codec, payload, or workload special cases. Freeze
Spec 127 as non-rerunnable baseline evidence.

## Baseline Evidence and Claim Boundary

The immutable baseline is
`results/spec127-cross-application-20260720-confirmation03`, summarized in
`specs/127-cross-application-stream-generality/completion-summary.md`.
It contains the complete 12-cell, one-shot campaign: periodic zero-loss passed;
periodic combined impairment accepted 0/5; variable zero-loss failed the 99%
future-hit gate; variable combined impairment accepted 0/5. Mapping novelty was
100%, so this feature addresses missing-content recovery and future-Interest
utility rather than retuning Mapping novelty or claiming a latency regression.

Spec 127's result directory, manifest, CSV, logs, accepted source hashes, and
completion summary are historical evidence. They MUST NOT be rerun, modified,
deleted, relabeled, replaced, or counted as repetitions for this feature. A
new implementation revision requires a separately named, complete Spec 128
campaign; a threshold miss remains a negative result.

## User Scenarios & Testing

### User Story 1 - Recover a Lost One-Item Sample (Priority: P1)

As an application developer, I can consume a generic live stream with
one-item opaque samples when an early future Interest is lost, without treating
that sample as a media frame, a UAV packet, or a special workload.

**Why this priority**: Spec 127 showed that a one-item sample has no repair
item to compensate for loss. A timely, finite retry is the smallest generic
path to recover it while retaining live-stream forward progress.

**Independent Test**: Use signed opaque one-item samples under a frozen
multiple-loss/future-Interest impairment fixture. Verify that the original
exact name is retried only while it remains eligible, then either arrives once
or is terminally skipped without holding later samples.

**Acceptance Scenarios**:

1. **Given** a future Interest for a signed one-item sample times out before
   the sample's usefulness deadline, **When** generic eligibility remains,
   **Then** the consumer issues only its configured finite number of exact-name
   retries and records their cause and outcome.
2. **Given** the retry budget or usefulness deadline is exhausted, **When** the
   item is still unavailable, **Then** it is skipped once with an explicit
   terminal reason and later eligible samples continue.
3. **Given** a delayed, stale-session, wrong-name, wrong-signer, duplicate, or
   replayed response, **When** it arrives during retry, **Then** it cannot be
   admitted or extend the retry budget.

---

### User Story 2 - Recover More Than One Missing Segment (Priority: P1)

As an application developer, I can consume an opaque multi-segment sample when
the declared recovery capacity covers multiple lost source items, without the
framework parsing the content or assuming an XOR/video codec.

**Why this priority**: A single XOR repair can reconstruct one missing source
item only. Spec 127's combined-loss result exposed that limit for variable
multi-segment samples.

**Independent Test**: Publish opaque samples with a signed, generic recovery
declaration that covers at least two missing source items. Induce exactly the
declared recoverable loss count and verify byte-exact, once-only atomic
delivery; induce one additional loss and verify fail-closed forward progress.

**Acceptance Scenarios**:

1. **Given** a sample whose authenticated recovery declaration states a
   recoverable loss capacity of at least two, **When** no more than that many
   authenticated source items are unavailable and timely repair material is
   available, **Then** the original opaque sample is reconstructed and emitted
   once in publication order.
2. **Given** losses beyond the declared capacity, expired repair material, or
   inconsistent recovery metadata, **When** recovery is attempted, **Then** no
   partial or guessed sample reaches the application and later samples remain
   eligible.
3. **Given** a stream declares no recovery material, **When** an item is lost,
   **Then** only the finite exact-name retry policy may attempt recovery; the
   framework must not synthesize a repair or promise FEC capability.

---

### User Story 3 - Bound Useful Future Work (Priority: P2)

As a framework maintainer, I can see whether future Interests and recovery
work were useful, bounded, and safe rather than merely increasing traffic.

**Why this priority**: The variable zero-loss stream in Spec 127 delivered all
samples but achieved only 94.497% provider-confirmed future hits; recovery must
not hide that inefficiency.

**Independent Test**: Run the same analyzer for one-item and multi-segment
fixtures. It must report initial versus retry future Interests, retry outcome,
retry suppression reason, repair capacity, recoveries, exhaustion, Payload and
Mapping utility, timeout, Nack, coverage, continuity, and latency.

**Acceptance Scenarios**:

1. **Given** any completed or failed run, **When** results are analyzed,
   **Then** the report separates initial future work from retry work and never
   counts a retry as useful solely because it was issued.
2. **Given** a retry is suppressed, **When** the suppression is recorded,
   **Then** the record identifies one generic reason: deadline expired,
   retry budget exhausted, no longer schedulable, stop fencing, or validated
   terminal failure.
3. **Given** recovery is successful, **When** its data is reported, **Then**
   the report identifies the declared capacity and recovered-source count
   without inspecting application payload semantics.

---

### User Story 4 - Preserve Generic Ownership and Frozen Evidence (Priority: P3)

As a framework maintainer, I can introduce the contract without application
branches and evaluate it in a fresh, single-writer MiniNDN campaign while
leaving Spec 127's negative evidence intact.

**Why this priority**: Recovery specialization or selective reruns would make
the apparent improvement uninterpretable.

**Independent Test**: Audit Core decisions, configuration inputs, experiment
manifests, and historical hashes. Run deterministic, security, compatibility,
and campaign-runner gates before any live cell.

**Acceptance Scenarios**:

1. **Given** the implementation and fixtures, **When** decision inputs are
   audited, **Then** no branch, threshold, parser, service name, or mode is
   keyed to UAV, camera, video, codec, sensor, payload contents, or either
   benchmark's workload identity.
2. **Given** a formal Spec 128 campaign is launched, **When** one planned cell
   fails or is interrupted, **Then** its output remains in the aggregate and
   it is not automatically rerun or replaced.
3. **Given** retained Spec 127 evidence, **When** the Spec 128 campaign closes,
   **Then** its paths and hashes are unchanged and it is cited only as a
   historical baseline, never as a new-code control repetition.

### Edge Cases

- Consecutive timeouts consume the finite retry budget before the producer
  materializes the item.
- The item becomes stale between timeout and scheduled retry.
- A future retry is satisfied by retained/cached authenticated Data rather
  than a new producer transmission.
- A Nack, validation failure, or stop arrives while a retry is queued.
- Two sources are absent and two repair items arrive out of order.
- The repair declaration says two losses are recoverable but one repair item
  is missing, expired, malformed, stale-session, or bound to another group.
- A source arrives after a successful recovery or terminal skip.
- A Mapping update changes a future sample boundary while recovery for an
  older immutable group is pending.
- A loss count exactly equals the declared recovery capacity, and one that is
  exactly one greater.

## Requirements

### Functional Requirements

- **FR-001**: The framework MUST provide one generic recovery policy usable by
  every live-stream sample, independent of application identity, service name,
  payload contents, codec, media type, or workload fixture.
- **FR-002**: A future exact-name Interest that times out MUST be eligible for
  retry only while its authenticated name, stream session, Mapping binding,
  consumer lifecycle, usefulness deadline, and generic retry budget remain
  valid.
- **FR-003**: Retry MUST be finite. The policy MUST define a positive bounded
  maximum number of attempts and a bounded time horizon; it MUST not resubmit
  an expired, unschedulable, or terminally invalid item.
- **FR-004**: Retrying one item MUST NOT consume unbounded in-flight capacity,
  prevent later eligible groups from progressing, or reopen a completed,
  rejected, or stopped item.
- **FR-005**: The retry decision and terminal reason MUST use only generic
  authenticated stream state, delivery observations, and declared bounds.
- **FR-006**: A generic recovery declaration MUST explicitly state the maximum
  source-item loss count it can recover, the items bound to its group, its
  integrity bindings, and its validity deadline before the consumer attempts
  reconstruction.
- **FR-007**: The implementation MUST support a declared capacity of at least
  two missing source items for opaque multi-segment samples. It MUST recover
  only when all validation, capacity, and deadline conditions hold.
- **FR-008**: A declared capacity of one MUST retain the current one-loss
  behavior; a stream with no recovery declaration MUST retain no-FEC semantics.
  This feature MUST be additive and wire-compatible for existing streams.
- **FR-009**: Reconstructed data MUST be byte-exact against authenticated
  source identity and may be delivered only as one complete, ordered,
  once-only sample. Partial, mixed-session, guessed, duplicate, stale, or
  unverified data MUST fail closed.
- **FR-010**: Loss beyond declared capacity, invalid recovery material,
  exhausted retry, or elapsed deadline MUST result in one observable terminal
  outcome and MUST preserve later-stream forward progress.
- **FR-011**: The status and analyzer contract MUST separately report initial
  Payload Interests, retry Payload Interests, provider-confirmed future
  interests and future hits for each, retry success/exhaustion/suppression,
  timeout, Nack, declared recovery capacity, attempted recovery count,
  recovered source count, terminal skips, Mapping novelty, latency, continuity,
  and measurement coverage.
- **FR-012**: Utility accounting MUST distinguish work that was issued from
  work that was satisfied. A retry satisfied by retained or cached authenticated
  Data is valid delivery evidence but MUST be labeled separately from a
  producer-confirmed future hit.
- **FR-013**: Existing Mapping validation, packet naming, authorization,
  signer binding, replay protection, stop fencing, and resource caps MUST
  remain enforced for original, retry, and recovery traffic.
- **FR-014**: Deterministic tests MUST cover one-item no-FEC recovery by retry;
  one and two recoverable source losses; capacity-plus-one failure; retry/
  deadline races; Nack and validation failures; stale Mapping/session;
  out-of-order repairs; stop races; and no duplicate or partial delivery.
- **FR-015**: The formal campaign MUST use the same application-neutral
  periodic one-item and variable-size multi-segment opaque workload families
  as Spec 127, but it MUST be a separately named, fully fresh campaign with
  the new source revision.
- **FR-016**: Before the first live cell, the feature MUST freeze a complete
  manifest of command, source and binding hashes, deterministic workload/order
  seeds, recovery parameters, topology, impairment profiles, metrics schema,
  and unique result paths. Each planned cell MUST be invoked once.
- **FR-017**: The campaign MUST include a zero-loss guard, a one-item no-FEC
  multiple-loss/future-retry profile, and a multi-segment profile that both
  exercises the declared two-loss capacity and exceeds it. It MUST retain
  failed and invalid cells with their counters.
- **FR-018**: MiniNDN is the final network evidence environment. Host NFD,
  physical devices, UAV applications, codec benchmarks, and workload-specific
  tuning are out of scope.
- **FR-019**: Spec 127 confirmation03 and every other Spec 127 result are
  immutable baseline evidence. The new runner MUST reject their output paths
  as destinations and record their hashes before and after the Spec 128 run.
- **FR-020**: A positive claim is limited to the two declared opaque workload
  families and frozen impairment profiles. It MUST be withheld if either
  family misses its independent correctness, utility, or bounded-work gates.
- **FR-021**: No UAV, camera, flight-control, sensor-field, video, codec,
  application-name, payload parser, or externally selectable workload-specific
  recovery/prefetch mode is in scope.

### Key Entities

- **RetryEligibility**: Authenticated item identity plus lifecycle, deadline,
  retry budget, scheduling, and terminal-state facts that decide whether one
  more exact-name Interest is permitted.
- **RecoveryDeclaration**: Signed generic statement of a sample group's
  source/repair membership, integrity bindings, validity window, and maximum
  recoverable missing-source count.
- **RecoveryAttempt**: One bounded attempt to reconstruct an opaque group,
  including declared capacity, actual missing count, outcome, and reason.
- **RecoveryUtilityReport**: Per-run attribution of initial/retry Payload work,
  provider future work, cache/retention satisfaction, recovery, retries,
  Mapping novelty, failures, continuity, latency, and coverage.
- **FrozenBaselineEvidence**: The read-only Spec 127 campaign identity and
  hashes that a Spec 128 run must preserve but cannot reuse as treatment data.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Deterministic coverage reports zero duplicate, partial,
  out-of-order, stale-session, or post-stop application deliveries across all
  retry and recovery boundary cases.
- **SC-002**: In every accepted one-item multiple-loss run, at least 99.9% of
  expected samples are delivered in order, no final-window stall exceeds two
  publication periods, and every retry count is at or below the frozen bound.
- **SC-003**: In every accepted multi-segment run with losses at or below its
  declared two-loss capacity, at least 99% of expected samples are delivered
  completely and in order, with zero partial deliveries and p95
  publication-to-delivery latency no greater than 250 ms.
- **SC-004**: In every capacity-plus-one run, zero incomplete or reconstructed
  corrupt samples are delivered, every unrecoverable group has one terminal
  reason, and later eligible samples continue within the workload continuity
  bound.
- **SC-005**: In each accepted zero-loss workload run, at least 99% of initial
  future Payload Interests are provider-confirmed hits for the exact periodic
  stream and at least 95% are hits for the variable-extent stream, whose
  remaining bounded speculative work must be attributable to authenticated
  terminal-unproduced sample-extent advice; at least 90% of
  accepted Mapping Data responses advance known Mapping information, and
  Payload Interest overhead is no more than 15% above necessary source plus
  selected repair items.
- **SC-006**: In each accepted impaired workload run, at least 95% of all
  future Payload Interests are provider-confirmed hits, retry Payload work is
  separately reported and bounded by the frozen manifest, and total Payload
  Interest overhead is no more than 25% above necessary source plus selected
  repair items.
- **SC-007**: Each workload's preregistered impaired acceptance group has at
  least four accepted runs out of five, with exact confidence intervals
  reported. The capacity-plus-one safety profile is an all-cells safety gate,
  not a population reliability estimate.
- **SC-008**: Every planned cell, including failure, has non-missing or
  explicitly unavailable values for all fields in FR-011, exact identity
  coverage, source/configuration hashes, one-shot invocation evidence, and
  an explicit admissibility verdict.
- **SC-009**: A code-aware audit finds zero recovery/prefetch branches,
  constants, parsers, names, or configuration inputs keyed to UAV, codec,
  application identity, payload semantics, or either workload.
- **SC-010**: The Spec 127 baseline hash manifest is identical before and after
  the Spec 128 campaign, and no Spec 127 path appears as a new result
  destination or repetition identifier.
- **SC-011**: The closing report states the observed result for each workload
  and profile without claiming population reliability, physical-wireless
  performance, or generality beyond the declared fixtures and impairments.

## Assumptions

- The eventual concrete erasure algorithm is selected during planning only if
  it can satisfy the signed generic declaration and byte-exact recovery
  contract; the specification deliberately does not privilege XOR, a video
  codec, or an application library.
- Existing one-loss XOR streams remain valid and unchanged; the new multiple-
  loss capability is opt-in through generic stream metadata, not a change in
  application identity or payload interpretation.
- Retry bounds, usefulness horizon, recovery capacity, and impairment details
  are frozen in the implementation plan before campaign execution and are not
  adjusted after a formal cell begins.
- The two workload fixtures remain opaque, deterministic, and MiniNDN-only.
  They are coverage instruments, not a claim about all stream applications.
- The existing user-visible security and lifecycle contract is a hard boundary;
  recovery cannot relax validation or substitute unsigned material.
