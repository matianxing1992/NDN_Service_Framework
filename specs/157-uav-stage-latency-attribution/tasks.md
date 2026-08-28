# Tasks: UAV Stage Latency Attribution

- [x] T001 Establish the five-boundary source/code reality and freeze Spec 156
  as a read-only input in `spec.md`, `plan.md`, and
  `contracts/diagnostic-analysis.md`.
- [x] T002 [US1] Implement the exact frame/cursor join, exclusion accounting,
  statistics, immutable-output guard, synthetic fixtures, and offline six-cell
  report in `Experiments/analyze_uav_video_stage_timeline.py`,
  `tests/python/test_spec157_uav_stage_latency_attribution.py`, and a new
  `results/spec157-*` directory; accept only after the focused unittest command
  passes and every frozen `fps-*` cell produces nonempty stage statistics.
- [x] T003 [US1] Audit the resulting report against FR-001–FR-009, record
  coverage and evidence limitations, and close this diagnostic without running
  MiniNDN or modifying Spec 156; verify frozen input hashes before and after
  the offline replay.

```text
T001 -> T002 -> T003
```
