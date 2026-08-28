# Tasks: UAV Stop Deadlock and Predictive Future Margin

- [X] T001 Freeze Spec 154 as a 2/6 negative baseline; complete deterministic
  cross-artifact and code-aware pre-implementation audits.
- [X] T002 Add failing regressions for lock/join ordering in
  `tests/python/test_spec155_uav_stop_deadlock_future_margin.py` and generic
  predictive-window margin in `tests/unit-tests/stream-predictive.t.cpp`.
- [X] T003 Move UAV detection-loop join outside `m_containerMutex`; implement
  the workload-neutral 25% new-future-work margin while retaining retry
  priority.
- [X] T004 Pass `./waf build -j2`, the `StreamPredictive` and
  `UavProtocolState` native suites, Spec 152/155 Python tests, `py_compile`,
  Boost-1.71 linkage, workload-token scan, and strict traceability audit.
- [X] T005 Use
  `Experiments/run_spec155_uav_stop_deadlock_future_margin.py` to freeze and
  execute one fresh complete 10/20/30/40/50/60-fps MiniNDN matrix with no
  retry or selective replacement.
- [X] T006 Aggregate every required metric and preserve the terminal 2/6
  negative result. Post-audit hands the unchanged efficiency gate to Spec 156.

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006
```
