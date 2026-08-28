# Tasks: Predictive UAV Multi-rate Validation

- [X] T001 Freeze and audit the multi-rate contract: record current
  source/binary/frozen-result hashes, prove no MiniNDN/UAV writer is active,
  verify the FPS-to-pipeline-to-StreamPublisher and
  PredictiveStreamSubscriber paths with CodeGraph, and close the
  pre-implementation audit. Any Core workload branch or frozen evidence drift
  is BLOCK.

- [X] T002 Correct and prove UAV capture pacing: add a focused regression
  for representative low/high rate pacing, implement clock-synchronized
  capture output without changing decode or predictive transport semantics,
  build affected targets with `-j2`, and pass callback-containment, video
  pipeline, StreamFacade, and UAV protocol regressions.

- [X] T003 Qualify the immutable rate harness: implement an independent
  Spec 152 runner/analyzer that separates frame FPS from segment PPS, reports
  all FR-007 metrics, checks build-Core linkage and API markers, and passes
  short low/high-rate MiniNDN diagnostics in new roots. Preserve every failed
  diagnostic and repair only before formal freeze.

- [X] T004 Execute the fresh six-cell formal matrix: freeze stable
  sources, binaries, commands, topology, environment, thresholds, and cell
  order; prove single-writer ownership; then run 10/20/30/40/50/60 fps exactly
  once with no automatic or selective rerun.

- [X] T005 Aggregate and audit closure: produce comparison CSV/Markdown,
  verify 6/6 cells against FR-008, rescan Core for workload special cases,
  verify Specs/results 148–151 remain unchanged, and write the
  post-implementation audit and completion summary. Mark complete only on full
  PASS; otherwise retain BLOCK/negative evidence.

```text
T001 -> T002 -> T003 -> T004 -> T005
```
