# Tasks: Stream/Prefetch API Simplification

**Input**: [spec.md](spec.md), [plan.md](plan.md),
[API contract](contracts/high-level-api.md), and
[before/after examples](contracts/before-after.md)

**Current state**: Complete. Post-implementation audit verdict is PASS.

## Phase 1 - Design and blocking audit

- [X] T001 Use CodeGraph to inventory the current C++/Python
  `createLiveStream/create_live_stream`, publisher reservation/activation,
  `openLiveStream/open_live_stream`, consumer start, defaults, callers, and
  tests; freeze the additive facade signatures, semantic parity, delegation
  map, lifecycle failures, compatibility boundary, and paste-ready comparisons
  in `specs/147-stream-prefetch-api-simplification/{research.md,data-model.md,contracts/high-level-api.md,contracts/before-after.md}`; run strict structure,
  cross-artifact, and code-aware pre-implementation audits and record the gate
  in `evidence/pre-implementation-audit.md`.

**Checkpoint**: Implementation is blocked unless T001 verdict is PASS.

## Phase 2 - Provider facade

- [X] T002 [US1] Implement the additive C++17 `StreamConfig` and
  `StreamPublisher` facade test-first, including session/default-name
  derivation and process-local live-collision validation, an APP-thread-only
  bounded route-readiness primitive, validated first-sample bootstrap in
  existing announce/publish/activate order, fail-closed handling of unexpected
  underlying partial mutation, bounded concurrency-safe later-announcement
  ownership, exactly-once activation, publication delegation, lifecycle
  errors, status/stop, pybind11 exposure,
  idiomatic Python parity, and matched before/after provider examples
  in `ndn-service-framework/StreamFacade.{hpp,cpp}`,
  `ndn-service-framework/ServiceProvider.{hpp,cpp}`,
  `pythonWrapper/src/ndnsf/_ndnsf.cpp`,
  `pythonWrapper/ndnsf/{streaming,service,__init__}.py`,
  `examples/{StreamFacadeProvider.cpp,python/live_stream/facade_provider.py}`,
  `tests/unit-tests/stream-facade.t.cpp`, and
  `tests/python/test_ndnsf_stream_facade.py`.

**Checkpoint**: Matched inputs produce identical low-level/facade definitions,
names, descriptors, packets, errors, and status in C++ and Python.

## Phase 3 - Consumer facade

- [X] T003 [US2] Implement `StreamSubscriptionOptions` and the additive
  `subscribeStream/subscribe_stream` facade test-first, including symmetric
  descriptor-aware policy/recovery defaults, callback/admission adaptation,
  exactly-once automatic handle start, returned existing handle, error
  containment, and matched before/after consumer examples in
  `ndn-service-framework/StreamFacade.{hpp,cpp}`,
  `ndn-service-framework/ServiceUser.{hpp,cpp}`,
  `pythonWrapper/src/ndnsf/_ndnsf.cpp`,
  `pythonWrapper/ndnsf/{streaming,service,__init__}.py`,
  `examples/{StreamFacadeConsumer.cpp,python/live_stream/facade_consumer.py}`,
  `tests/unit-tests/stream-facade.t.cpp`, and
  `tests/python/test_ndnsf_stream_facade.py`.

**Checkpoint**: One call creates and starts exactly one existing consumer
handle with equivalent callbacks, status, and stop semantics in both languages.

## Phase 4 - Compatibility and closure

- [X] T004 [US3] Rebuild native source with `-j2` and force-rebuild the Python
  binding; record the ordinary binding attempt and, if the 8 GiB host kills
  its single large pybind translation unit, use the documented bounded
  `-j1 -g0` build fallback without changing runtime optimization; run the
  new facade, existing Stream, validator/security, Python Core/generality, and
  Spec 146 focused suites; compare paired low-level/facade packet,
  descriptor, callback, and status outputs; use CodeGraph and source diff to
  prove zero algorithm/workload branch; verify frozen Spec 144/146 hashes; and
  close implementation with honest evidence in
  `specs/147-stream-prefetch-api-simplification/evidence/post-implementation-audit.md`
  and `closure-report.md`.

## Dependencies and Execution Order

```text
T001 -> T002 -> T003 -> T004
```

Provider and consumer work both touch bindings and wrapper exports, so they are
sequenced even though their behavioral contracts are independently testable.

## Scope Guard

- Do not modify or rerun Specs 144/146.
- Do not remove, rename, or silently redirect the low-level API.
- Do not remove explicit future announcement.
- Do not add sample assembly or application policy.
- Do not change prefetch, Mapping, FEC, retry, timeout, Nack, validation, or
  recovery algorithms.
- Do not claim the frozen facade is implemented until T002-T004 complete.
