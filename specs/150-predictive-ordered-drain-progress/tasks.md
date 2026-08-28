# Tasks: Predictive Ordered-Drain Progress

- [X] T001 Freeze the Spec 149 negative baseline and ordered-drain contract.
- [X] T002 Add deterministic failing tests for terminal-gap progress,
  concurrent wake, late recovery discard, and stop cleanup.
- [X] T003 Implement single-owner drain, stale-ready fencing, signed frontier
  cursor ranges, and direct recovery-group selection in `Stream.{hpp,cpp}` and
  `StreamFacade.{hpp,cpp}`.
- [X] T004 Add C++/Python/UAV ordered-drain status fields and parity tests.
- [X] T005 Run focused/full build and tests plus API/workload scans.
- [ ] T006 Add independent Spec 150 runner/analyzer, smoke, then run the exact
  two formal cells once with immutable hashes and no retry.
- [X] T007 Run post-implementation CodeGraph/Spec Kit audit and close only if
  SC-001..SC-005 pass.

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006 -> T007
```
