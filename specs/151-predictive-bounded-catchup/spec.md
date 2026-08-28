# Feature Specification: Predictive Bounded Catch-up

**Status**: Complete
**Baseline**: Spec 150 build-Core impaired smoke (BLOCK)

## Problem

Spec 150 removed recovery deadlock and ordered-drain backlog. Its correctly
linked 1% loss / 1% reorder UAV smoke ended with zero ready/gap backlog and 38
real FEC recoveries, but delivered only 13,280 of 41,866 pushed items. The
consumer issued 15,142 Payload Interests because predictive scheduling limits
the future cursor horizon to `packetDemand` even when the adaptive controller
has already granted a much larger bounded `window/lookahead`.

## Functional Requirements

- **FR-001**: Predictive scheduling MUST derive its future cursor horizon from
  the current adaptive decision, not only one-sample `packetDemand`.
- **FR-002**: The horizon MUST be bounded by both decision `lookahead` and the
  aggregate in-flight capacity; it MUST never create unbounded future state.
- **FR-003**: Pending source retries MUST retain priority over new future
  Interests and share the same aggregate budget.
- **FR-004**: The scheduler MUST reserve selected cursors before releasing its
  state mutex and MUST never exceed aggregate capacity.
- **FR-005**: Status MUST expose the effective future cursor horizon in C++,
  Python, and UAV structured output.
- **FR-006**: Deterministic tests MUST prove that catch-up can fill more than
  one-sample demand, remains within its bounds, and yields capacity to retries.
- **FR-007**: Mapping=0, predictive frontier v2, direct group recovery,
  ordered-drain semantics, FEC, retry count, Interest lifetime, topology, and
  UAV workload configuration MUST remain unchanged.
- **FR-008**: Core MUST contain no UAV, video, telemetry, audio, codec, or
  workload-specific branch.
- **FR-009**: Specs 148–150 and all prior formal/smoke evidence MUST remain
  immutable and MUST NOT be rerun or overwritten.
- **FR-010**: MiniNDN processes MUST resolve the campaign-hashed build Core,
  not `/usr/local/lib`.
- **FR-011**: A new impaired smoke MUST pass delivery >=98% before any formal
  campaign is frozen.
- **FR-012**: After all gates pass, exactly one fresh zero-loss and one fresh
  1% loss/1% reorder formal cell MUST run with no retry.

## Success Criteria

- **SC-001**: Deterministic tests prove `packetDemand < horizon <=
  min(lookahead, aggregateLimit)` during catch-up and retry priority.
- **SC-002**: Full 367-target build, all native tests, and focused Python tests
  pass with Boost 1.71 linkage.
- **SC-003**: Correctly linked impaired smoke delivers >=98%, Mapping=0,
  decodes video, has no ordered backlog, and reports all required metrics.
- **SC-004**: Both fresh formal cells deliver >=98% with complete
  AoI/end-to-end mean/p50/p95/p99, gap, future-hit, Interest, retry, timeout,
  Nack, FEC recovery, and useless-Interest evidence.
- **SC-005**: Source/binary hashes remain stable and no formal cell is rerun.

## Out of Scope

- changing FEC code or repair count;
- changing topology, impairment, video, bitrate, or frame rate;
- workload-specific tuning;
- rerunning or modifying Specs 148–150.
