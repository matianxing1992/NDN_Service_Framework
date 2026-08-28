# Tasks: Spec 128 Generic Multi-Loss Recovery

## Phase 1: Core Retry and Multi-Erasure Foundation

- [x] T001 [US1] Deliver generic bounded future-Interest retry in `ndn-service-framework/Stream.{hpp,cpp}` and `tests/unit-tests/stream.t.cpp`; include timeout/Nack/deadline/stop/late-Data permutations, finite exact-name attempts, one terminal reason, and no payload/workload decision input. [FR-001..FR-005, FR-010, FR-013, FR-014]
- [x] T002 [US2] Deliver additive signed two-erasure opaque recovery in `ndn-service-framework/Stream.{hpp,cpp}` and `tests/unit-tests/stream.t.cpp`; retain no-FEC/one-XOR compatibility, validate independent repair indices and all integrity bindings, recover any two sources byte-exactly, and fail closed at capacity-plus-one. [FR-006..FR-010, FR-013, FR-014]

## Phase 2: Binding and Neutral Fixture Evidence

- [x] T003 [US3] Deliver Python binding/status parity and deterministic neutral-fixture coverage in `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/streaming.py`, `tests/python/test_ndnsf_core_streaming.py`, and `tests/python/test_ndnsf_live_stream_generality.py`; acceptance requires all new retry/recovery counters and schemes are exposed without application labels and the two workload families exercise the generic contract. [FR-011..FR-014, FR-021]

## Phase 3: Frozen Runner and Formal Confirmation

- [x] T004 [US4] Deliver a single-writer 16-cell Spec 128 runner/analyzer in `Experiments/run_spec128_generic_recovery_matrix.py`, `Experiments/NDNSF_LiveStream_Generality_Minindn.py`, and `tests/python/test_spec128_generic_recovery_runner.py`; acceptance requires frozen 2+10+4 manifest, no rerun/output reuse, full traffic attribution, qdisc/source/baseline hash gates, and preservation of failed cells. [FR-011, FR-015..FR-019, SC-007, SC-008, SC-010]
- [x] T005 Execute T001-T004 validation gates, full C++ build, forced Python binding rebuild, Core/binding/security/runner suites, then every fresh Spec 128 MiniNDN cell exactly once; write completion evidence in `specs/128-generic-multiloss-recovery/` and close only using measured SC-001..SC-011 results. [SC-001..SC-011]

## Dependencies and Cohesion

T001 and T002 are Core prerequisites; T003 depends on them. T004 depends on
the new fixture/binding metric contract. T005 is the only task authorized to
launch the formal matrix. Each task owns its tests, implementation, and
acceptance evidence because splitting those steps would create mechanical,
non-reviewable fragments.
