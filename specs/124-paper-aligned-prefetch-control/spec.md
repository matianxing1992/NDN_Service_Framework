# Feature Specification: Paper-Aligned Prefetch Control

**Feature Branch**: `Experimental`

**Created**: 2026-07-19

**Status**: Complete

**Input**: Continue comparing NDNSF Streaming with Gusev et al., repair remaining design and implementation defects, and validate the result.

## User Scenarios & Testing

### User Story 1 - Stable Live-Edge Convergence (Priority: P1)

As a live-stream consumer, I want the prefetch window to converge to the smallest useful pipeline without repeatedly overshooting the live edge.

**Independent Test**: Deterministic arrival traces prove that every chase/withhold action observes its detection hold and that stale data during adjustment restores the previous usable window instead of starting another doubling cycle.

**Acceptance Scenarios**:

1. **Given** a recent chase action, **When** stable samples arrive before the detection hold expires, **Then** the controller does not change phase or window early.
2. **Given** adjustment has reduced the window, **When** arrivals become stale, **Then** the controller restores the last usable window and enters steady fetching.
3. **Given** status is queried after the hold deadline, **Then** the reported remaining hold is zero even if no new sample arrived.

### User Story 2 - Network-Only Pipeline Demand (Priority: P1)

As a live-stream consumer, I want pipeline demand to represent network retrieval time rather than the time an early Interest waited for future data production.

**Independent Test**: Deterministic payload-delay samples prove that known-produced Data updates network delay directly, while ahead-mapped samples cannot make generation wait become a network RTT spike during live-edge search and become normal observations only after stable fetching.

**Acceptance Scenarios**:

1. **Given** an Interest for already-produced Data, **When** Data arrives, **Then** its expression-to-reception delay updates network retrieval delay.
2. **Given** an ahead-mapped Interest waits for production during chasing or adjustment, **When** Data arrives, **Then** the full effective delay cannot raise network RTT.
3. **Given** the consumer has entered stable fetching where generation wait is minimized, **When** network delay changes, **Then** later payload observations can update the estimate in either direction.

### User Story 3 - Honest UAV Network Evidence (Priority: P2)

As a UAV operator and researcher, I want a fresh MiniNDN result that distinguishes consumer ahead-mapped Interests from Provider-confirmed future hits and preserves any negative result.

**Independent Test**: A uniquely named 60-second zero-loss UAV run passes existing continuity, latency, Interest-work, security, and future-hit gates without modifying prior result directories.

## Requirements

- **FR-001**: Chase, adjust, and fetch transitions MUST respect the configured detection hold.
- **FR-002**: Adjustment MUST remember and restore the last usable window when withholding first produces stale arrivals.
- **FR-003**: Hold telemetry MUST be calculated against the current decision time.
- **FR-004**: Network packet demand MUST exclude future-data generation wait from its retrieval-delay estimate.
- **FR-005**: Delay estimation MUST remain able to adapt in stable fetching without adding unbounded state or consumer-specific metadata to immutable Data.
- **FR-006**: The consumer's existing future-Interest counter MUST be documented as an ahead-of-join-checkpoint diagnostic, not Provider-confirmed future eligibility or hits.
- **FR-007**: Provider-side future-interest and future-hit counters remain the authoritative future-prefetch evidence.
- **FR-008**: No wire-format, semantic Data-name, signature, Mapping-validation, encryption, FEC, or user-facing `LiveStream` handle API change is permitted.
- **FR-009**: Existing Spec 121-123 result directories MUST remain unchanged; new evidence uses a unique Spec 124 identity.

## Scope Boundaries

- This feature corrects the existing paper-inspired control loop; it does not copy the paper's complete codec, bitrate adaptation, or jitter-buffer architecture into Core.
- Per-frame capture, encode, decode, and GUI delay remain APP evidence, not network-delay input.
- The paper's security omissions are not adopted; current NDNSF signed Mapping, signed semantic Data, validation, and optional application encryption remain mandatory.

## Success Criteria

- **SC-001**: Focused tests fail on the pre-fix controller and pass after the correction.
- **SC-002**: One future generation-wait outlier cannot increase the network-delay estimate as if all of that wait were network RTT.
- **SC-003**: All Stream unit tests and relevant UAV unit tests pass.
- **SC-004**: A unique 60-second zero-loss MiniNDN run preserves continuous decoded/displayed video through the final ten seconds, capture-to-decode p95 at most 250 ms and p99 at most 500 ms, at least 99% Provider-confirmed future-hit success, and Interest work no more than 15% above configured data-plus-parity items per decoded frame.
- **SC-005**: If SC-004 fails, the failure and exact evidence are retained rather than replaced or described as success.

## Assumptions

- The immutable NDN Data packet cannot carry a consumer-specific producer-wait duration without changing cache identity or adding per-request semantics.
- Ahead-mapped effective delay may safely lower an overestimate during live-edge search but may not raise it; once stable fetching minimizes generation wait, normal adaptive observations resume. Direct known-produced samples remain preferred.
- MiniNDN remains the final network gate for this work.
