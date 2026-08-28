# Tasks: Adaptive Sample-Atomic Prefetch

## Phase 1: Signed variable-group foundation

- [x] T001 [US4] Freeze Mapping v2/group/predictor and high-level `announceSample`/optional name-binding `prepareSampleExtent`/`publishSample`/adaptive-open C++ and Python contracts with golden vectors, then implement fail-closed group-aware Mapping admission and bounded per-class prediction in `ndn-service-framework/Stream.{hpp,cpp}`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/streaming.py`, examples, and `tests/unit-tests/stream.t.cpp`; the focused gate must cover API equivalence, class isolation, cold start, conservative rounding, block crossing, impossible-cap rejection, malformed metadata, v1/v2 session isolation and rollback, and no partial state mutation.

## Phase 2: Sample-atomic scheduling and variable recovery

- [x] T002 [US1] Replace scalar cursor truncation and fixed-source FEC with whole-predicted-group scheduling and variable real-source groups, including red tests, bounded capacity/pressure behavior, authenticated actual-extent correction, under/overprediction diagnostics, and focused C++/Python validation in the Stream implementation and unit suites.

## Phase 3: Real UAV frame integration

- [x] T003 [US2] Remove UAV empty-shard padding, freeze bitrate/FPS/GOP and future class announcements, supply authoritative opaque key/delta class policy, reserve/publish actual variable access-unit groups through only the high-level public Stream API, consume read-only group status, and pass focused UAV tests proving distinct class histories, real source counts, valid encryption/FEC, complete frame assembly, and unchanged GUI delivery.

## Phase 4: Frozen network evidence and closeout

- [x] T004 [US3] Run the relevant full Stream/UAV regressions and exactly one uniquely named 60-second zero-loss MiniNDN acceptance cell with the frozen original-load configuration; retain pass or failure, audit all correctness/security/Interest/future-hit/latency gates, and record exact commands, environment, distributions by class, underprediction RTT events, and residual risks in `results/spec125-*` and `completion-summary.md`.

## Phase 5: User-authorized defect closure

- [x] T005 [US3] Preserve the original negative cell, lock the stale provisional-frontier failure into the real UAV/Core integration seam, fix each diagnosed correctness defect without changing the frozen workload, and run uniquely named 60-second zero-loss MiniNDN confirmation cells until continuous GUI decoding passes or an external blocker is proven; retain every outcome and update traceability and completion evidence without overwriting T004.

## Dependencies

- T001 is the protocol prerequisite for all later work.
- T002 depends on T001's group resolver and predictor.
- T003 depends on the public Core behavior from T002.
- T004 starts only after deterministic and focused integration gates pass.
- T005 depends on T004's retained failure and uses it as the regression baseline.

## Cohesion Audit

The five tasks are independently reviewable outcomes: protocol foundation,
runtime scheduling/recovery, APP integration, frozen network evidence, and the
subsequently authorized defect closure. Tests,
implementation, focused validation, and directly owned evidence remain together;
there are no per-file or per-command task fragments.
