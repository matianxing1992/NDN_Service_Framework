# Spec 127 Completion Summary

**Closed:** 2026-07-20  
**Implementation/audit status:** COMPLETE / PASS  
**Measured generality verdict:** NEGATIVE; broader useful-prefetch claim withheld

## Outcome

Spec 127 is complete, but its preregistered cross-application claim does not
pass. The final admissible campaign is:

```text
results/spec127-cross-application-20260720-confirmation03
```

It contains 12/12 unique one-shot cells and a 12-row CSV. The runner reports
`automaticRetry=false`, `sourceUnchanged=true`, and
`historicalEvidenceUnchanged=true`. No failed repetition was selectively
replaced. The campaign exits `FAIL` because only periodic zero-loss meets every
absolute gate.

| Workload/profile | Accepted | Required | Exact 95% interval | Verdict |
|---|---:|---:|---|---|
| periodic / zero-loss | 1/1 | 1 | [0.025, 1.0] | PASS |
| periodic / combined | 0/5 | 4 | [0.0, 0.521824] | FAIL |
| variable / zero-loss | 0/1 | 1 | [0.0, 0.975] | FAIL |
| variable / combined | 0/5 | 4 | [0.0, 0.521824] | FAIL |

## Per-criterion evidence

- **SC-001 PASS (deterministic):** 27/27 generality tests preserve zero
  duplicate, partial, out-of-order, or post-stop application mutation.
- **SC-002 PASS:** periodic zero-loss delivers 600/600; final-window maximum
  stall is 102 ms; measured p95/p99 are 3.218/4.756 ms.
- **SC-003 PASS for delivery/latency but not sufficient for acceptance:**
  variable zero-loss delivers 600/600 across all four size classes; maximum
  stall is 115 ms; p95/p99 are 11.210/13.004 ms.
- **SC-004 FAIL:** periodic zero-loss has 100% future hits, 0% Payload overhead,
  and 100% Mapping novelty. Variable zero-loss has 5.5803% Payload overhead and
  100% Mapping novelty, but only 94.4970% Provider-confirmed future hits versus
  the 99% gate.
- **SC-005 FAIL:** periodic combined delivers 586-592/600 and accepts 0/5;
  variable combined delivers 576-592/600 and accepts 0/5. Both require 4/5.
- **SC-006 FAIL as a shared claim:** every accepted run demonstrates proactive
  work, but only periodic zero-loss is accepted; one workload cannot compensate
  for the other.
- **SC-007 PASS:** every run contains Payload, Mapping, Mapping-new ratio,
  retry, timeout, Nack, continuity, coverage, measured latency, and explicit
  full-run traffic-counter scope.
- **SC-008 PASS:** all 12 commands have unique paths and one invocation;
  automatic retry is absent; workload/source/history checks remain stable.
- **SC-009 PASS:** Core, Python binding, and accepted UAV hashes still equal
  Spec 126 confirmation07. Spec 127 adds no workload/codec/UAV branch to Core.
- **SC-010 PASS:** this report makes no population, physical-wireless, causal,
  or untested-workload claim.

## Traffic attribution

`trafficCounterScope=full-run-including-warmup` is deliberate: subtracting
cumulative counters at the warm-up boundary can split one future Interest from
the Data produced just after the boundary and yield more hits than Interests.
Completion, continuity, and publication-to-delivery latency remain measured-
sample-only. Across final runs, Mapping novelty is 100%, Nacks are zero, and
Payload overhead is 0% for periodic and 5.5803% for variable. The limiting
metrics are loss coverage and variable-stream future-hit utility, not Mapping
novelty or raw delivery latency.

## Preserved failed evidence and corrections

- `results/spec127-cross-application-20260720-formal01`: first cell exposed
  root ownership, versioned semantic-prefix, and reservation-budget defects.
- `results/spec127-cross-application-20260720-confirmation02`: complete 12-cell
  failure retained; it exposed excessive 32-sample announcement lead and mixed
  publication/receipt clock domains.
- `results/spec127-diagnostic-lead4-clock01`: non-acceptance periodic probe;
  confirms 600/600 and correct clock/lead behavior.
- `results/spec127-diagnostic-variable-combined02`, `03`, and `04`: non-
  acceptance probes that tested bounded generic lead/horizon explanations.
  Their 97.3-98.0% coverage showed that further tuning would be result-driven;
  unsuccessful lead/horizon changes were removed before confirmation03.

Every reproducible harness defect received deterministic coverage before the
separately named complete confirmation03. No diagnostic counts as a campaign
repetition.

## Compatibility and verification

- full C++ build and suite: 338/338 PASS;
- forced Python binding rebuild: PASS;
- Python Core streaming: 19/19 PASS;
- Spec 127 generality: 27/27 PASS;
- Spec 127 campaign runner: 6/6 PASS;
- UAV stream security contract: 12/12 plus native 112/112 PASS;
- Spec Kit strict structure: PASS (20 FR, 10 SC, 4 stories, 6 tasks);
- final campaign: executed and measured, 12/12, aggregate negative.

## Claim boundary and next work

The accepted Mapping v2 mechanism is fast and efficient for the tested
periodic zero-loss stream and delivers the tested variable zero-loss stream
quickly and completely. Spec 127 does **not** establish robust cross-application
generality: one-item periodic samples lack repair under combined loss, while
variable multisegment traffic falls below both the combined coverage gate and
the future-hit utility gate. Any improvement belongs in a new formal Spec with
a generic multi-loss/retry contract; Spec 127 evidence and thresholds must not
be reopened or selectively rerun.
