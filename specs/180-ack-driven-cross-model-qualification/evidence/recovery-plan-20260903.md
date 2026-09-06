# Spec180 Recovery Plan — Why Real Qualification Was Delayed

**Date**: 2026-09-03  
**Subject**: `Experimental` working tree, Spec180 ACK-driven YOLO path

## Verified blocker

The registered entrypoint `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`
constructs `CaseRuntimeBinding` and `MiniNdnCaseRuntime`, then
`run_minindn_case()` raises `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. The runtime
adapter therefore has no production caller that starts the staged Controller,
Repository, Providers, and User, publishes/readbacks the signed runtime
catalogue, or waits for the real ACK/Selection/Response exchange.

This is why focused tests can pass while no real Spec180 experiment exists:
the tests cover fail-closed validators and injected seams, whereas the only
real entrypoint deliberately stops before network side effects. The absence of
a terminal Response is a missing execution path, not a model or Tiger result.

## Ordering defects that amplified the delay

- Release/SIF implementation was allowed to remain in a post-audit execution
  task, so repeated preflight work did not produce a runnable candidate.
- The QWEN-F executable was deferred until after SIF sealing, which made the
  candidate definition incomplete and forced rework.
- Historical Spec175 closure and focused Spec180 tests were repeatedly treated
  as if they were current live-case evidence. They are not interchangeable.

## Binding recovery order

```text
T011-A real atomic Y-A
  -> T011-B same driver, shared Y-B
  -> T011-C same driver, fixed Y-N controls/negatives
  -> T011-D freeze driver evidence
  -> T013 finish QWEN-F + release/SIF executables
  -> T014 one design-code convergence PASS
  -> T015 local inventory once
  -> T016 local SIF once
  -> T017 stage once
  -> T018 YOLO-F
  -> T019 QWEN-F
  -> T020 final closure
```

No SIF build, upload, or Tiger submission is authorized before T014. A fake
publisher, offline snapshot, legacy deployment-first runner, or synthetic
terminal marker is useful only for focused seam tests and cannot close T011.

## Evidence vocabulary

`implemented` means source exists; `wired` means the production caller reaches
it; `executed` means the real MiniNDN/NFD/SVS path ran; `qualified` means the
task oracle, security checks, child exits, and cleanup all pass. The current
state is limited to focused `implemented`/`wired` fragments, with T011-A as the
next gate capable of producing `executed` evidence.
