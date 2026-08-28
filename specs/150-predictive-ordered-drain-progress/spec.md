# Feature Specification: Predictive Ordered-Drain Progress

**Status**: Active  
**Baseline**: immutable failed impaired cell from Spec 149

## Problem

Spec 149 removed recovery-control amplification but its impaired UAV cell
delivered only cursors 0..399. FEC remained active (1,267 attempts, 474
recoveries), yet none of those recoveries reached the application. A follow-up
impaired smoke waited at cursor 6 with no terminal gap while recovery scanned
retained group names. The current subscriber lacks both a direct signed
cursor-to-group binding and a single-owner ordered-drain progress contract.

## Functional Requirements

- **FR-001**: Exactly one drain owner MUST advance `nextDeliverCursor` for one
  subscriber generation.
- **FR-002**: Every accepted ready item or newly declared terminal gap MUST
  wake the drain owner or record a pending wake that the owner consumes before
  relinquishing ownership.
- **FR-003**: The drain MUST atomically skip consecutive terminal gaps and then
  deliver every contiguous ready item in cursor order.
- **FR-004**: A validated source or FEC recovery arriving behind
  `nextDeliverCursor` MUST be classified as stale/late and MUST NOT enter the
  ready queue.
- **FR-005**: Stop/generation changes MUST clear drain ownership, pending wakes,
  ready entries, and terminal gaps without invoking the application callback.
- **FR-006**: Status MUST expose `nextDeliverCursor`, ready-queue depth,
  optional oldest-ready cursor, terminal-gap queue depth, drain wake count, and
  stale recovery/source drops in C++, Python, and UAV structured status.
- **FR-007**: Deterministic tests MUST reproduce a terminal gap followed by
  valid later sources and prove ordered progress, concurrent wake safety,
  late-recovery discard, and stop cleanup.
- **FR-008**: Public `start/push/flush`, wire names, validation, FEC,
  recovery-control coalescing, adaptive prefetch, retry limits, Interest
  lifetime, and workload configuration MUST remain unchanged.
- **FR-009**: Core MUST contain no UAV, video, codec, or workload-specific
  branch.
- **FR-010**: Specs 148 and 149 and their formal results MUST remain immutable
  and MUST NOT be rerun.
- **FR-011**: After all deterministic gates pass, one new immutable campaign
  MUST run the exact same zero-loss and 1% loss/1% reorder cells once.
- **FR-012**: Every retained group reference in the provider-signed frontier
  MUST bind its canonical group name to a non-overlapping inclusive
  `[firstCursor,lastCursor]` range.
- **FR-013**: Recovery MUST select exactly the retained range containing the
  missing cursor. A cursor outside all retained ranges MUST fail lookup
  immediately without scanning unrelated group commits.
- **FR-014**: The cursor-range frontier wire contract MUST use
  `contractVersion=2`; version-1 frontiers MUST be rejected rather than
  silently decoded without authenticated ranges.
- **FR-015**: Concurrent scheduler entries MUST reserve aggregate payload
  capacity before releasing the state mutex; recovery fallback MUST NOT
  oversubscribe or throw from the adaptive in-flight budget.
- **FR-016**: Source retries MUST enter the same aggregate in-flight scheduler,
  ahead of new future Interests, rather than bypassing its capacity limit.
- **FR-017**: Formal MiniNDN application processes MUST load the hashed
  `build/libndn-service-framework.so` used by the campaign; resolution to an
  un-hashed `/usr/local/lib` framework copy MUST fail the preflight.

## Success Criteria

- **SC-001**: Focused tests prove single-owner drain and progress beyond an
  unrecoverable cursor.
- **SC-002**: Full native build/tests and focused Python tests pass.
- **SC-003**: New zero-loss cell delivers >=98% with Mapping=0.
- **SC-004**: New impaired cell delivers >=98%, Mapping=0, complete latency and
  traffic metrics, and no persistent ready item remains blocked behind a
  terminal gap.
- **SC-005**: Frozen source/binary hashes remain stable and no cell is rerun.

## Out of Scope

- changing FEC codes or parity;
- changing prefetch, retry, timeout, topology, netem, or video parameters;
- unordered delivery;
- modifying or rerunning prior formal evidence.
