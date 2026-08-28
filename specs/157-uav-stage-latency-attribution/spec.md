# Feature Specification: UAV Stage Latency Attribution

**Created**: 2026-07-26  
**Status**: Closed — offline diagnostic PASS  
**Input**: Attribute UAV video latency using five existing per-frame timeline
boundaries without modifying or rerunning the frozen Spec 156 six-rate matrix.

## User Scenarios & Testing

### User Story 1 - Explain the observed latency (Priority: P1)

As a researcher, I can decompose each exactly correlated decoded frame into
capture, encode, publication materialization, transport/reassembly, and decode
intervals so that a latency explanation names the measured bottleneck.

**Independent Test**: Replay a synthetic pair of provider/consumer logs with
known timestamps and obtain the expected five-stage deltas and percentiles.

**Acceptance Scenarios**:

1. **Given** exact frame and cursor timeline events, **when** the analyzer runs,
   **then** it joins one frame from `capture_origin_ns` through decoder output.
2. **Given** a missing or non-monotonic event, **when** the analyzer runs,
   **then** it excludes that frame from stage statistics and reports why.
3. **Given** the frozen Spec 156 result root, **when** offline analysis runs,
   **then** it reads all six cells without changing any file below that root.

### Edge Cases

- Timeline sampling may leave only a subset of frames fully joinable.
- A frame may span several source cursors; materialization ends at its greatest
  source cursor, not at a repair or control packet.
- Legacy decoder events without exact frame identity are not admissible.
- Provider and consumer timing is comparable only because all MiniNDN
  namespaces share the same host monotonic clock in this campaign.

## Requirements

### Functional Requirements

- **FR-001**: The diagnostic MUST use the five boundaries: capture origin,
  encoded output ready, last source segment materialized, decoder input, and
  decoder output.
- **FR-002**: Capture time MUST come from `capture_origin_ns`; the later
  `source-acquired` log emission time MUST NOT substitute for acquisition.
- **FR-003**: Frame-to-cursor joins MUST use stream ID, session epoch, source
  frame ID, and exact cursor metadata.
- **FR-004**: The last materialized segment MUST be the greatest source cursor
  belonging to the frame; repair/control packets MUST NOT extend the stage.
- **FR-005**: Each stage MUST report sample count, mean, p50, p95, and p99 in
  milliseconds, plus complete-frame coverage and monotonicity failures.
- **FR-006**: The analyzer MUST honor each cell's existing warmup and
  measurement window by selecting decoder outputs inside that window.
- **FR-007**: Spec 156 and its result root MUST remain byte-for-byte untouched.
- **FR-008**: No Core, UAV runtime, API, wire, prefetch, FEC, retry, topology,
  workload, or six-rate campaign behavior may change.
- **FR-009**: Independent stage percentiles MUST NOT be summed into a
  synthetic end-to-end percentile.

## Success Criteria

- **SC-001**: Synthetic fixtures prove exact joining, last-source selection,
  measurement-window filtering, percentile output, and rejection accounting.
- **SC-002**: All six frozen cells produce a diagnostic summary outside the
  frozen campaign root.
- **SC-003**: Every reported stage has a positive sample count and all admitted
  frame timelines are monotonic.
- **SC-004**: The report distinguishes measured attribution from causal
  inference and records coverage limits caused by timeline sampling.

## Assumptions

- The frozen campaign used the exact GStreamer frame-binding path.
- Linux steady-clock values are comparable across the MiniNDN processes on the
  same host.
- This diagnostic localizes delay in the observed campaign; it does not by
  itself establish a universal causal mechanism.

## Terminal Result

The immutable six-cell replay at
`results/spec157-uav-stage-latency-diagnostic-20260726T204922Z` joined
15969/15969 measured decoder outputs with zero missing or non-monotonic
timelines. From 10 to 60 FPS, mean capture-to-output fell by 81.923 ms:
capture-to-encoded accounted for 73.486% of that reduction and
encoded-to-materialized for 25.906%. Materialized-to-decoder-input did not
fall; it changed from 2.320 to 2.884 ms. The observed inverse FPS/latency trend
therefore lies before network fetch/reassembly in this campaign, not in
insufficient future-Interest supply.
