# Tasks: Predictive Recovery Coalescing

- [X] T001 Freeze the Spec 148 failure baseline, exact unchanged formal
  profiles, counter semantics, and pre-implementation audit in
  `specs/149-predictive-recovery-coalescing/`.
- [ ] T002 [US1] Add failing deterministic multi-gap tests, implement
  session-level frontier coalescing, exact-name group-fetch coalescing,
  validated cache reuse/eviction, multi-waiter failure completion, and stop
  cleanup in `tests/unit-tests/stream-facade.t.cpp` and
  `ndn-service-framework/StreamFacade.{hpp,cpp}`; accept only when focused tests
  count actual expressed Interests and every waiter terminates once.
- [X] T003 [US2] Add recovery-control counters to
  `ndn-service-framework/Stream.hpp`, predictive status production,
  pybind11/Python exposure, UAV structured status, and focused assertions while
  keeping predictive `mappingInterests == 0`; accept with native and Python
  field-parity tests plus one structured UAV status fixture.
- [X] T004 [US1] Run focused and full native/Python build gates, API-removal
  scans, workload-neutrality scans, and record every result under
  `specs/149-predictive-recovery-coalescing/evidence/`.
- [X] T005 [US3] Create a new immutable no-rerun runner/analyzer derived from
  the Spec 148 harness, execute smoke then the two fresh >=60-second cells, and
  preserve complete manifests/results in a new `results/spec149-*` directory.
- [ ] T006 Run post-implementation CodeGraph/Spec Kit audit and close only if
  SC-001..SC-006 pass; otherwise preserve the failure and leave this task open.

## Dependencies

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006
```
