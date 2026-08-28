# Pre-Implementation Audit

**Date**: 2026-07-25  
**Mode**: pre-implementation  
**Verdict**: PASS

## Findings

No unresolved CRITICAL or HIGH finding blocks implementation.

| ID | Severity | Dimension | Finding | Required action |
|---|---|---|---|---|
| A1 | — | Intent | The facade reduces lifecycle/name/reservation plumbing while preserving explicit future announcement. | Implement only the frozen contract. |
| A2 | — | Necessity | Current APIs are functional but expose common Provider identity, session, naming, readiness, reservation, and separate consumer-start plumbing. | Delegate rather than replace. |
| A3 | — | Architecture | Existing publisher, descriptor, open options, and consumer handle remain the only protocol/runtime owners. | New facade may own only bounded orchestration state. |
| A4 | — | Compatibility | Low-level custom naming/reservation and packet-feed use cases cannot fit the small facade without reintroducing complexity. | Keep low-level API public and unchanged. |
| A5 | — | Validation | A syntax/orchestration facade needs golden delegate parity and existing regressions, not a tuned network matrix. | Treat any network-behavior difference as BLOCK. |
| A6 | — | Lifecycle | Existing activation requires registered routes and a materialized safe-join source. | Bootstrap must wait off the Face thread and delegate announce, publish, then activate. |
| A7 | — | Failure atomicity | Existing publication may mutate Mapping or payload state before a later step throws. | Prevalidate deterministic errors; otherwise enter `Failed` and never invent rollback or retry. |

## Code Reality

CodeGraph verified the current on-disk symbols:

- `ServiceProvider::createLiveStream` at `ServiceProvider.cpp:8528`;
- `LiveStreamPublisher::{announceSample,prepareSampleExtent,publishSample,activate}`
  in `Stream.hpp/.cpp`;
- `LiveStreamPublisher::activate` at `Stream.cpp:4578`, including its route,
  Mapping, and materialized-safe-join preconditions;
- `ServiceUser::openLiveStream` at `ServiceUser.cpp:2988`;
- Python `create_live_stream` at `service.py:891`;
- Python `open_live_stream` at `service.py:1188`;
- Python publisher/consumer wrappers in `streaming.py`.

Search found no existing `createStream/create_stream` or
`subscribeStream/subscribe_stream`, so the contract does not duplicate a
current facade.

## Security and Distributed Correctness

- Existing signed Mapping, Provider identity, session, descriptor validation,
  exact-name Data, callback admission, and FEC validation remain authoritative.
- A fresh nonzero session and automatically versioned prefix make accidental
  restart reuse improbable; a process-local live-tuple registry detects
  in-process collisions without claiming distributed uniqueness.
- The facade cannot retry, re-announce, or change class automatically.
- Consumer subscribe returns the existing handle and therefore cannot create a
  second fetch or validation state machine.

## Occam and Scope

The minimal facade retains one essential two-step property:
future `announce()` precedes actual `publish()` after bootstrap. Removing it
would make proactive prefetch impossible. Bootstrap alone combines the first
announce/publication/activation because no consumer can prefetch before the
descriptor exists and activation requires that first materialized source. All
custom naming, manual reservation, packet feed, and advanced recovery control
stay on the low-level API.

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Simplifies common path only |
| Architecture and ownership | Yes | Thin delegate over one engine |
| Security/correctness | Yes | Existing authenticated objects reused |
| Task executability | Yes | Four cohesive, ordered tasks |
| Validation | Yes | Golden parity plus current regressions |
| Migration/rollback | Yes | Additive; remove facade only |
| Code reality | Yes | Exact symbols and absent target facade verified |

## Gate

Implementation may begin at T002. Any proposed hunk in adaptive fetching,
Mapping admission/wire, FEC, retry, timeout, Nack, validation, recovery, or
frozen Spec 144/146 evidence changes this verdict to BLOCK.
