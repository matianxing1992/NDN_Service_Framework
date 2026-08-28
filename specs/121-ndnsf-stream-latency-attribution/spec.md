# Feature Specification: NDNSF Stream Latency Attribution and Continuity

**Feature Branch**: `Experimental`
**Created**: 2026-07-18
**Status**: Frozen complete for continuity and attribution; optimization acceptance superseded by Spec 122
**Input**: Diagnose and optimize NDNSF Streaming so live video continues beyond the initial Mapping horizon and every delay above expected one-way delivery time is attributable to a real stage.

## Scope Disposition

Spec 121 owns the completed Mapping-frontier continuity repair, latest-join correction, sequence-domain separation, and withdrawal of invalid FIFO latency attribution. Its single baseline/candidate pair is preserved as interim evidence only. The originally planned five-pair optimization and trace-overhead decision cannot establish camera-acquisition-to-display latency because the current H.264 group/output-frame relationship is ambiguous. That decision is therefore not executed again under Spec 121; Spec 122 owns exact source-frame identity, true capture-to-widget/presentation measurement, counterbalanced confirmatory pairs, and the final runtime-default decision. This transfer does not convert the Spec 121 candidate into an accepted default.

## User Scenarios & Testing

### User Story 1 - Continuous Live Consumption (Priority: P1)

As a live-stream consumer, I keep receiving newly produced media for the full session, including after the stream crosses its initial Mapping block and prefetch horizon.

**Why this priority**: A stream that renders only its initial seconds is not live; latency optimization is meaningless until continuity is correct.

**Independent Test**: Run a 60-second zero-loss stream across at least three Mapping blocks and verify publication and consumption continue throughout the measured window without an unhandled scheduling error or unexplained stall.

**Acceptance Scenarios**:

1. **Given** a latest-mode consumer, **When** the provider publishes new Mapping blocks and payloads, **Then** every scheduling component advances to the verified Mapping frontier and continues issuing exact-name Interests.
2. **Given** the safe join point is above the beginning, **When** decoder reorder starts, **Then** it does not wait for media items deliberately skipped before joining.
3. **Given** Mapping, payload, FEC, timeout, or callback failure, **When** scheduling cannot continue safely, **Then** the handle reports a bounded failure/recovery reason rather than silently stopping.

---

### User Story 2 - Trustworthy Latency Attribution (Priority: P2)

As an operator or researcher, I can separate startup latency, network delivery, security processing, reorder waiting, decoder input/output, and GUI delivery using measurements for the same media item.

**Why this priority**: The current approximately one-second result combines cold start and mismatched cursor domains, so it cannot identify the optimization target.

**Independent Test**: Replay deterministic publication cursors, media sequences, repair items, and multiple decoded frames; verify no stage pair is correlated across different identities and unsafe cross-clock intervals remain unavailable.

**Acceptance Scenarios**:

1. **Given** an item has a publication cursor and media sequence, **When** stages are correlated, **Then** one immutable source identity follows it and both sequence domains remain separately labelled.
2. **Given** one encoded input group yields several decoded frames, **When** latency is reported, **Then** the correlation cardinality is explicit and frames are never paired with unrelated groups.
3. **Given** a cold decoder, **When** the first frame appears, **Then** startup delay is separate from the steady-state distribution.
4. **Given** clocks without bounded offset uncertainty, **When** a report is generated, **Then** it does not claim measured one-way cross-host delay.

---

### User Story 3 - Evidence-Driven Low Latency (Priority: P3)

As a UAV operator, I receive continuously updating video with bounded steady-state delay, and each retained optimization has matched evidence without reducing correctness, security, or prefetch effectiveness.

**Why this priority**: Window, grouping, FEC, join, and decoder changes should be retained only after corrected measurement identifies a benefit.

**Independent Test**: Compare a frozen baseline and one-variable candidates in matched 60-second MiniNDN runs; accept a candidate only when latency improves while continuity, exact-name validation, FEC behavior, and resource bounds pass.

**Acceptance Scenarios**:

1. **Given** the corrected baseline, **When** a candidate is evaluated, **Then** only its named variable changes.
2. **Given** a candidate lowers latency but stalls, drops frames, weakens validation, or increases unnecessary Interests, **When** audited, **Then** it is rejected and retained as a negative result.
3. **Given** names are mapped early enough, **When** source Data is produced, **Then** eligible exact-name Interests are already pending and Mapping discovery adds no second round trip.

### Edge Cases

- Latest join falls on a repair item, empty padded shard, or non-decodable delta frame.
- A Mapping block arrives while older callbacks complete out of order.
- Mapping is unavailable, malformed, stale, or signed by the wrong provider.
- A future Interest outlives its Mapping generation or is answered after retry.
- FEC is disabled, enabled without loss, or repairs exactly one source item.
- One encoded input group emits zero, one, or multiple decoded frames.
- Provider production continues while consumer scheduling makes no progress.
- Trace sampling omits one or more stages.

## Requirements

### Functional Requirements

- **FR-001**: A live consumer MUST maintain one monotonic verified Mapping frontier and propagate every accepted advance to all scheduling state that bounds legal future cursors.
- **FR-002**: Crossing a Mapping block or reservation horizon MUST NOT throw from an asynchronous callback, silently terminate scheduling, or leave an active handle without a progress reason.
- **FR-003**: Latest-mode media reorder MUST start from a source-media join boundary derived from verified metadata or the first admissible source item; it MUST NOT synthesize loss for pre-join items.
- **FR-004**: Publication cursors, media sequences, group/frame IDs, and repair IDs MUST remain distinct; metrics MUST NOT join records because numeric values coincide.
- **FR-005**: Every sampled source item MUST carry one stable correlation identity across Mapping, validation, decryption, reorder, decoder input/output attribution, and GUI delivery where present.
- **FR-006**: Decoder correlation MUST define cardinality when one input group yields multiple frames and MUST report uncorrelated outputs instead of pairing them with unrelated groups.
- **FR-007**: Reports MUST separate join/startup, warmup, and steady-state distributions with counts, p50/p95/p99, missing stages, and the steady-state rule.
- **FR-008**: Same-clock intervals MAY be measured directly; cross-clock one-way intervals MUST require bounded offset uncertainty or be marked unavailable.
- **FR-009**: A live handle MUST expose bounded progress diagnostics distinguishing Mapping starvation, payload wait, retry exhaustion, application rejection, reorder gap, decoder startup, and stopped/failed state.
- **FR-010**: Semantic Data names, provider validation, encryption, replay resistance, and optional FEC MUST remain intact; no Data-in-Data or security downgrade is allowed.
- **FR-011**: Experiments MUST use 60-second measured windows, unique identities, frozen matched conditions, and preserved failures without automatic reruns.
- **FR-012**: Candidates MUST change one variable and report continuity, completion, frame delivery, Interest efficiency, CPU, memory, queue high-water marks, and latency.
- **FR-013**: Per-item observability MUST use sampled `NDN_LOG` and bounded state; direct console output and unsampled per-item INFO logs are prohibited.
- **FR-014**: Existing public Stream APIs and Mapping/source/repair wire formats MUST remain compatible unless a separately audited requirement proves a protocol change necessary.

### Key Entities

- **Stream Progress Snapshot**: Verified Mapping frontier, next scheduling cursor, in-flight work, last progress time, lifecycle state, and wait/failure reason.
- **Stream Correlation Identity**: Session-scoped source identity with separately labelled publication cursor, media sequence, group/frame ID, and optional output ordinal.
- **Latency Stage Sample**: Compatible stage events, clock authority, startup/steady classification, duration when valid, and missing/invalid reason otherwise.
- **Latency Experiment Cell**: Frozen workload/topology, unique identity, result, correctness/resource counters, and distributions.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Every accepted 60-second zero-loss run crosses at least three Mapping blocks, receives source items during the final ten seconds, and has no unexplained gap longer than two production periods while the provider produces.
- **SC-002**: At least 99% of eligible zero-loss source items have an exact-name Interest pending before production, with mapping, payload, retry, and unnecessary-Interest counts reported.
- **SC-003**: No accepted run contains an unhandled callback exception, ACTIVE consumer with stale Mapping frontier, or scheduler stall without a machine-readable reason.
- **SC-004**: Deterministic tests reject 100% of publication-cursor/media-sequence collisions and never pair decoded frames with unrelated encoded groups.
- **SC-005**: At least 95% of sampled steady source items retain exact attribution through decoder input. Decoder outputs retain decoded-frame ordinals for GUI timing, while source-to-output attribution is reported unavailable unless a codec-aware source ID and output ordinal are present; FIFO guesses are forbidden.
- **SC-006**: Reports separate startup and steady counts and p50/p95/p99 for available stages; unavailable or cross-clock-unsafe stages are never zero-filled.
- **SC-007**: The corrected baseline reports source-to-decoder-input stages, decoder startup/output cadence, and decoded-callback-to-GUI timing separately, and explicitly identifies any unavailable boundary; it MUST NOT retain the false approximately one-second FIFO aggregate.
- **SC-008**: Any optimization retained from this work MUST ultimately improve an exactly attributable steady stall/latency indicator in four of five matched pairs while meeting SC-001 through SC-007 and losing no more than 1% decoded-frame completion. **Disposition**: not decided by Spec 121; transferred to Spec 122 because exact capture-to-display identity was unavailable.
- **SC-009**: Retained tracing MUST ultimately change exactly attributable steady p95 latency and CPU by no more than 5% in four of five matched trace-on/off pairs. **Disposition**: not decided by Spec 121; transferred to Spec 122 with the exact identity gate.
- **SC-010**: Focused unit, security/protocol, and MiniNDN gates pass without changing semantic names, signatures, encryption, replay checks, or FEC correctness.

## Assumptions

- MiniNDN nodes share a host clock, but deployed nodes may not; reports retain clock authority.
- The current Stream API and names-only Mapping design remain baseline.
- Zero loss is the primary attribution environment; controlled loss follows correctness.
- GPU, Docker, and iTiger are outside scope.
- A correctness/measurement fix need not claim an optimization; SC-008 applies only to a candidate advertised as an optimization.
