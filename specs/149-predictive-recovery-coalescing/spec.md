# Feature Specification: Predictive Recovery Coalescing

**Status**: Active  
**Baseline**: immutable failed light-loss/reorder cell from Spec 148

## Problem

The predictive UAV path is correct at zero loss, but its recovery discovery is
unstable under the frozen 1% loss/reorder profile. Each missing cursor issues
its own frontier Interest and independently walks retained group commits
backwards. FEC recovered 513 sources, yet the duplicated control work expanded
to 39,488 Mapping-labelled Interests, 5,087 retries, and 6,821 timeouts.

## User Stories

### User Story 1 — Bounded generic recovery (Priority: P1)

As a predictive-stream consumer, I need concurrent losses to share authenticated
recovery metadata so that repair work stays bounded and does not collapse the
Payload path.

**Acceptance**: one in-flight frontier retrieval per stream session; one
in-flight fetch per group commit name; verified group commits are reused for
all waiting cursors and evicted at the signed retention boundary.

### User Story 2 — Honest observability (Priority: P1)

As an experiment analyst, I need Mapping, recovery-control, Payload, retry,
timeout, Nack, repair, and cache/coalescing counters to describe different
traffic classes without relabelling recovery metadata as Mapping-first lookup.

### User Story 3 — Real UAV validation (Priority: P1)

As the UAV-APP maintainer, I need a fresh MiniNDN campaign to prove that the
unchanged predictive API survives zero loss and the exact Spec 148 impairment
profile without changing FEC, workload, timing, or adaptive-prefetch policy.

## Functional Requirements

- **FR-001**: Concurrent recovery attempts in one session MUST share at most one
  in-flight frontier Interest.
- **FR-002**: Concurrent requests for one group commit name MUST share one
  in-flight Interest and one validation operation.
- **FR-003**: A verified group commit MUST populate a bounded name cache and be
  reusable by every missing cursor covered by that commit.
- **FR-004**: Cache contents MUST be provider-signed, session/epoch checked,
  bounded by the newest verified frontier retention list, and cleared on stop.
- **FR-005**: Frontier/group validation failure, Nack, timeout, stale session,
  and missing group MUST wake every waiter exactly once and continue through
  bounded retry/terminal-gap policy.
- **FR-006**: Predictive recovery metadata MUST be counted as recovery-control
  traffic, not Mapping-first traffic. Mapping Interest count MUST remain zero
  for the predictive facade.
- **FR-007**: Status MUST separately expose frontier, group, coalesced-waiter,
  and verified-cache-hit counters in C++ and Python.
- **FR-008**: Source Data exact-wire identity, validator checks, repair-first
  ordering, one-XOR FEC configuration, adaptive controller, and public
  `start/push/flush` API MUST remain unchanged.
- **FR-009**: Core MUST contain no UAV, telemetry, audio, video, codec, or
  workload branch.
- **FR-010**: Spec 148 results MUST remain immutable and MUST NOT be rerun.
- **FR-011**: A new campaign MUST use the same topology, real UAV video,
  application placement, 5-second warmup, >=60-second measurement, and exact
  zero-loss plus 1% loss/1% reorder profiles.
- **FR-012**: Both formal cells MUST preserve commands, hashes, qdisc evidence,
  logs, complete latency distributions, traffic counters, and negative results.

## Success Criteria

- **SC-001**: Focused tests prove frontier and group coalescing, cache reuse,
  multi-waiter failure completion, bounded eviction, and stop cleanup.
- **SC-002**: Full native build/tests and focused Python tests pass.
- **SC-003**: Both new MiniNDN cells run once for >=60 measured seconds with
  unchanged source/binary hashes.
- **SC-004**: Zero-loss delivery is >=98%, exact wire holds, future hits occur,
  and Mapping Interests remain zero.
- **SC-005**: The impaired cell delivers >=98%, Mapping Interests remain zero,
  all recovery-control categories are reported, and no uncontrolled
  frontier/group fan-out occurs.
- **SC-006**: No workload-specific Core branch or old high-level API returns.

## Out of Scope

- changing FEC shard counts or codes;
- changing adaptive prefetch, retry limits, Interest lifetime, SVS timing,
  topology, video workload, or netem after observing results;
- modifying or rerunning Spec 148;
- adding a compatibility API or runtime mode selector.
