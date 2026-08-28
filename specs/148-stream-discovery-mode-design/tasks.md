# Tasks: Predictive Stream API Replacement

**Input**: [spec.md](spec.md), [plan.md](plan.md), and
[contracts/high-level-api.md](contracts/high-level-api.md)

No task is complete merely because prototype code exists.

## Phase 0 — Freeze replacement and removal boundary

- [X] **T001** Inventory every C++/Python declaration, binding, example, test,
  and application call of high-level `start(initial...)`, `announce()`,
  `publish()`, `start_predictive()`, and old descriptor subscription. Record
  the migration table in `evidence/api-removal-inventory.md`.
- [X] **T002** Add compile-negative and Python surface tests proving the old
  high-level methods/overloads/aliases do not exist. Distinguish allowed
  internal `LiveStreamPublisher` methods from forbidden public facade methods.
- [X] **T003** Freeze the exact replacement descriptor, naming, group metadata,
  lifecycle, concurrency, and error contract in native and Python tests before
  implementation.

## Phase 1 — Sole provider API and exact-wire Core path

- [X] **T004** Replace public C++ `StreamPublisher` declarations with only
  `start()`, `push()`, `flush()`, `status()`, and `stop()`; remove old
  overloads without aliases.
- [X] **T005** Implement `start()` returning `PredictiveStreamDescriptor`;
  register required routes/frontier and return only after readiness or explicit
  failure.
- [X] **T006** Implement `push()` so a valid App-signed Data wire is validated,
  retained, retransmitted, and PIT-matched byte-for-byte without wrapping,
  content extraction, mutation, or Core signing.
- [X] **T007** Implement atomic `flush()` group commit with authenticated source
  membership/count/lengths/epoch, generic configured repair generation,
  frontier advancement, and safe empty-flush behavior.
- [X] **T008** Implement duplicate/equivocation, wrong-prefix, malformed-name,
  stale-epoch, invalid-signature, oversize, concurrent push/flush, and stop
  fencing behavior.

## Phase 2 — Predictive consumer and bindings

- [X] **T009** Complete `PredictiveStreamSubscriber` name scheduling and reuse
  `StreamAdaptiveFetcherState` without changing its algorithm.
- [X] **T010** Invoke the configured validator before admission and enforce
  name, epoch, group metadata, wire budget, duplicate, and equivocation checks.
- [X] **T011** Complete unequal-length-safe repair, bounded retry, terminal gap,
  late-join frontier, and delivery-order behavior with observable recovery
  reasons.
- [X] **T012** Report delivery, AoI/end-to-end mean/p50/p95/p99, longest gap,
  future-hit, Mapping/Payload Interest, retry, timeout, Nack, recovery, and
  useless-Interest ratio without workload-specific Core branches.
- [X] **T013** Replace pybind11/Python publication and subscription surfaces
  with the frozen predictive contract; remove old methods and
  `start_predictive()`.
- [X] **T014** Migrate C++/Python examples and all native/Python unit callers;
  verify language parity and absence of the removed API.

## Phase 3 — UAV application migration

- [X] **T015** Migrate UAV Drone to one predictive path: canonical sequential
  names, App signing, `push()`, and `flush()`; remove dual publication logic.
  Emit the provider runtime markers frozen in
  `contracts/uav-minindn-acceptance.md`.
- [X] **T016** Migrate UAV Ground Station to construct, start, and stop the real
  predictive subscriber; validate, decrypt, reassemble, and deliver frames.
  Emit the consumer runtime markers frozen in the acceptance contract.
- [X] **T017** Remove the new-binary Mapping-first selector and the
  parsed-but-unused `NDNSF_UAV_DISCOVERY_MODE` path. Runtime status MUST be
  derived from the actual Core objects. Preserve old behavior only in a pinned
  pre-migration binary manifest.
- [X] **T018** Extend the existing UAV MiniNDN runner with an end-to-end
  predictive acceptance mode that launches real `App_ServiceController` and
  `UavGroundStationApp` on `memphis`, and real `UavDroneApp` on `ucla`.
  Quick-smoke remains preflight only. The real smoke must prove both endpoint
  markers, decoded video, exact content, and zero separate Payload Interests.

## Phase 4 — Build and fresh evidence

- [X] **T019** Rebuild all native targets and the Python binding; run focused
  predictive tests, full native/Python regressions, API-removal tests, and
  source scans. Preserve every failure.
- [X] **T020** Extend the analyzer and execute a fresh two-node MiniNDN UAV
  smoke followed by two new ≥60-second formal cells: zero loss and one frozen
  light loss/reordering profile. Produce every metric and artifact in
  `contracts/uav-minindn-acceptance.md`. If comparing old/new, use immutable
  pinned binaries with matched configuration and record both hashes. Do not
  rerun frozen Specs 125/144/146/147.
- [X] **T021** Run post-implementation CodeGraph/Spec Kit audit. Close only when
  all requirements and evidence pass; otherwise leave tasks unchecked and
  record blockers.

## Dependencies

```text
T001 -> T002 -> T003
T003 -> T004 -> T005 -> T006 -> T007 -> T008
T005 -> T009 -> T010 -> T011 -> T012
T004,T009 -> T013 -> T014
T006,T011,T014 -> T015 -> T016 -> T017 -> T018
T008,T012,T018 -> T019 -> T020 -> T021
```

## Scope Guard

- Remove the old **public high-level** facade and all its callers.
- Do not remove reusable generic low-level Core primitives merely because they
  share words such as `publish` or `announce`.
- Do not add compatibility aliases, dual paths, or a runtime rollback flag.
- Do not modify adaptive-prefetch or FEC algorithms unless a separately
  approved Spec changes them.
- Do not add UAV, telemetry, audio, video, codec, or workload branches to Core.
- Do not edit or rerun frozen Specs 125/144/146/147 or their results.
