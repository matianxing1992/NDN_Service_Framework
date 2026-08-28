# Feature Specification: Acoustic Loss/Reorder Stability

**Feature Branch**: `[146-acoustic-impaired-stability]`  
**Created**: 2026-07-24  
**Status**: Complete  
**Input**: Repair the generic acoustic loss/reorder instability found by the
immutable Spec 144 campaign, validate the repair with one new 16-cell
confirmation, and defer all public API simplification to Spec 147.
**Predecessor evidence**: Spec 144, immutable measured-negative baseline

## Problem

Spec 144 proved that the current generic Mapping-v2 live-stream path is stable
for the acoustic workload only at zero loss. Its loss-only, reorder-only, and
combined treatments failed latency, continuity, or Interest-utility gates.
The failure must be fixed without editing or rerunning Spec 144 and without
adding UAV, acoustic, audio, codec, or workload-specific behavior to Core.

Frozen evidence also reveals a generic recovery defect. Under reorder-only
traffic, Core recovered sources that were still in flight or undergoing
validation, and the original Data later became a late arrival. For example,
`acoustic-reorder-r01` recorded 1,502 recovered sources and 1,501 late
arrivals despite no configured loss. Another repetition made 1,909 recovery
attempts for 399 recovered sources while latency grew from tens of
milliseconds to more than four seconds.

## User Scenarios & Testing

### User Story 1 - Reordered Data Is Not Mistaken for Loss (Priority: P1)

As a generic stream consumer, I want an in-flight or processing source to
retain ownership until Data, Nack, or timeout establishes a terminal outcome,
so harmless reordering does not launch duplicate FEC work or deliver twice.

**Why this priority**: Mistaking reordering for loss creates duplicate work,
late arrivals, and application-visible instability even when the network drops
no packets.

**Independent Test**: Deliver repair Data before source Data while the source
is in flight and while it is validating. The source is delivered exactly once
as signed Data, recovery is not attempted, and the original is not counted
late.

**Acceptance Scenarios**:

1. **Given** an exact-name source Interest is in flight, **when** authenticated
   repair Data arrives first, **then** FEC does not claim or deliver that source.
2. **Given** source Data is being validated, **when** repair becomes sufficient,
   **then** validation ownership remains exclusive and the application receives
   the source at most once.

---

### User Story 2 - Unowned Missing Sources Recover Once (Priority: P1)

As a consumer of a protected multi-item stream, I want one or two sources with
an expired exact-name attempt and no remaining network/validation ownership to
recover exactly once from valid authenticated repair symbols. If enough repair
symbols have not arrived, the existing finite exact-name retry path continues;
over-capacity loss remains explicitly incomplete.

**Why this priority**: The repaired ownership rule must not disable legitimate
finite recovery after an exact-name attempt has actually ended.

**Independent Test**: Exercise zero, one, two, and three missing sources in a
variable 2/3/4-source group with two repairs. Only the first three admissible
cases deliver the expected original bytes and all recovery counters conserve.

**Acceptance Scenarios**:

1. **Given** sufficient authenticated repair and a timed-out source with no
   remaining owner, **when** recovery runs, **then** exact bytes cross the
   application boundary once without a needless additional retry.
2. **Given** more missing sources than the declared recovery capacity, **when**
   all finite exact-name attempts terminate, **then** the group remains
   explicitly incomplete and its recovery state is retired without looping.

---

### User Story 3 - Continuous Impaired Streaming Remains Bounded (Priority: P1)

As an application author, I want the generic scheduler to avoid historical
repair scans and optional-repair head-of-line amplification, so a 25 block/s
multi-item stream remains useful under the already frozen loss/reorder
profiles.

**Why this priority**: Correct per-packet ownership is insufficient if Mapping
or recovery work still grows with stream history and destabilizes a continuous
session.

**Independent Test**: A fresh one-shot two-node MiniNDN acoustic matrix
contains exactly 16 cells: zero-loss 1, loss 5, reorder 5, combined 5. No
failed cell is replaced or selectively rerun.

**Acceptance Scenarios**:

1. **Given** the frozen acoustic topology and workload, **when** the 16 formal
   cells execute once, **then** zero-loss passes 1/1 and each impaired treatment
   passes at least 4/5 without replacement runs.
2. **Given** a long sequential Mapping history, **when** a verified strict
   successor arrives, **then** admission remains bounded while fork, gap, and
   reuse security checks remain fail-closed.

### Edge Cases

- Repair arrives before the original source while the original is in flight or
  being validated.
- One, two, or more-than-capacity sources are absent from a protected group.
- A Mapping successor is continuous, out of order, forked, quarantined, or
  attempts to reuse an old semantic name.
- A formal cell crosses one latency gate while preserving complete delivery;
  it remains a failed cell and is not rerun.

## Requirements

### Functional Requirements

- **FR-001**: Spec 144 documents, runner inputs, results, and hashes MUST remain
  byte-for-byte unchanged. Spec 146 MUST use a new runner name and output root.
- **FR-002**: FEC MUST NOT treat a source in `payloadInFlight` or
  `payloadProcessing` as recoverable. Mere absence from the signed-payload
  cache is never evidence of loss.
- **FR-003**: A source becomes provisionally FEC-recoverable only after its
  exact-name Interest times out and network/validation ownership has ended.
  Recovery may then use already authenticated repair symbols; otherwise the
  unchanged finite retry path continues. A Nack alone does not authorize this
  early transition, and retry exhaustion remains the terminal missing
  transition.
- **FR-004**: One source cursor MUST cross the application callback boundary
  at most once, whether its provenance is signed Data or FEC recovery.
- **FR-005**: Recovery MUST be indexed by the source's group and MUST NOT scan
  every historical repair group on each source admission.
- **FR-006**: Completed or expired recovery-group state MUST be retired with a
  bound derived from the generic stream retention/recovery contract.
- **FR-007**: Source delivery remains the critical path. Completed
  out-of-order groups MUST NOT consume the bounded unresolved-group scheduling
  horizon. Within the already authenticated candidate horizon, all eligible
  source Interests MUST be admitted before optional repair Interests consume
  the remaining packet capacity. This ordering MUST NOT change the declared
  FEC strength, controller window, Interest lifetime, or retry threshold.
- **FR-008**: The repair MUST preserve Mapping-v2 signed name/session/provider
  validation, finite retry limits, FEC byte/digest verification, and current
  callback containment.
- **FR-009**: Core and bindings MUST contain no selector, string comparison,
  threshold, or branch for UAV, telemetry, acoustic, audio, codec, block size,
  or the formal workload name.
- **FR-010**: New status/analyzer semantics MUST report recovery success as
  recovered source count and recovered group count separately from recovery
  attempts; no ratio may use unlike units or exceed 100%.
- **FR-011**: The fresh analyzer MUST report delivery, mean/p50/p95/p99/max,
  longest gap, future-hit, Mapping/Payload Interest splits, retry, timeout,
  Nack, late arrival, recovery, protection-only, nonproductive, and unresolved
  counts with conservation checks.
- **FR-012**: Formal execution MUST use the Spec 144 acoustic workload,
  topology, 60-second measured window, thresholds, and network profiles
  unchanged, except for the repaired Core/binding/analyzer identities.
- **FR-013**: Formal treatment acceptance remains zero-loss 1/1 and at least
  4/5 for each impaired profile. All four treatments must pass.
- **FR-014**: The work MUST NOT implement the API simplification requested for
  the successor feature. No public C++ or Python convenience API changes are
  permitted in Spec 146.
- **FR-015**: This reliability feature makes no new confidentiality claim.
  The existing Spec 144 payload-confidentiality finding remains open and MUST
  be stated in closure evidence rather than hidden.
- **FR-016**: A strictly continuous, already signature-verified Mapping block
  with no quarantined gap MUST be committed incrementally. It MUST preserve
  digest continuity, immutable-name reservation, grouped-entry order, cache
  bounds, and fatal fork/reuse handling. Out-of-order or quarantined Mapping
  remains on the existing full atomic rebuild path.
- **FR-017**: Formal latency commands MUST explicitly disable the high-rate
  per-packet Stream timeline probe. The diagnostic probe remains opt-in and
  sampled; disabling it changes no Stream, FEC, retry, or prefetch behavior.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Deterministic reorder tests show zero premature recovery and zero
  duplicate application delivery while a source is in flight or processing.
- **SC-002**: Deterministic timeout and one/two-loss tests recover exact bytes
  once without consuming a needless retry when sufficient authenticated
  repair already exists; the over-capacity case is bounded and explicit.
- **SC-003**: Recovery work per group is bounded; the diagnostic continuous
  stream does not show the multi-second monotonic latency growth preserved in
  Spec 144. Negative preflight cells remain evidence and are not replaced.
- **SC-004**: The zero-loss formal acoustic cell satisfies at least 99.9%
  block delivery, p95 at most 150 ms, p99 at most 250 ms, and longest gap at
  most 160 ms. Every impaired formal acoustic cell satisfies at least 99.9%
  block delivery, p95 at most 250 ms, p99 at most 400 ms, and longest gap at
  most 320 ms. These are the unchanged Spec 144 acoustic gates.
- **SC-005**: Where provider-confirmed future Interests exist, future-hit is at
  least 95%; Mapping novelty is at least 99%; nonproductive Payload Interest
  ratio is at most 10%.
- **SC-006**: All requested counters conserve exactly, all source/binary/config
  identities remain frozen after formal start, and the formal matrix has
  exactly 16 terminal rows.
- **SC-007**: A neutrality scan and CodeGraph review find zero forbidden
  workload semantics in Core/bindings.
- **SC-008**: A deterministic sequential-Mapping test commits at least 20
  successors through the incremental path while verified-block and binding
  caches remain within their declared bounds and an old semantic name still
  cannot be reused.

## Out of Scope

- Editing or rerunning Specs 127, 128, 144, or 145.
- Changing the workload cadence, extent cycle, FEC scheme, thresholds, netem
  profiles, or 60-second measurement window.
- Adding codec, microphone, playback, concealment, or UAV semantics to Core.
- Public Stream/Prefetch API simplification; that belongs to Spec 147.
- A new prefetch algorithm, policy enum, or workload-tuned constant.
- Claiming payload confidentiality before the separate open security finding is
  implemented and verified.
