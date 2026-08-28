# Spec 128 Completion Summary

**Closed:** 2026-07-20  
**Implementation/audit status:** COMPLETE / PASS  
**Measured confirmation verdict:** NEGATIVE; periodic impaired acceptance failed

## Outcome

All five tasks are complete. The final admissible campaign is:

```text
results/spec128-generic-recovery-20260720-confirmation03
```

It contains 16/16 unique one-shot cells and 16 CSV rows. Every command ran
once; `automaticRetry=false`, `sourceUnchanged=true`, and
`spec127EvidenceUnchanged=true`. No failure was replaced or selectively rerun.
The runner correctly exits `FAIL` because periodic impaired recovery accepts
only 2/5 repetitions instead of the required 4/5.

| Workload/profile | Accepted | Required | Exact 95% interval | Verdict |
|---|---:|---:|---|---|
| periodic / zero-loss | 1/1 | 1 | N/A | PASS |
| periodic / multi-loss retry | 2/5 | 4 | [0.052745, 0.853367] | FAIL |
| periodic / capacity-plus-one | 2/2 | 2 | N/A | PASS |
| variable / zero-loss | 1/1 | 1 | N/A | PASS |
| variable / multi-loss recovery | 5/5 | 4 | [0.478176, 1.000000] | PASS |
| variable / capacity-plus-one | 2/2 | 2 | N/A | PASS |

The three retained periodic impaired failures are exact boundary evidence:
repetitions 3 and 5 deliver 600/600 but have 235 ms and 234 ms tail stalls;
repetition 4 delivers 598/600, has a 203 ms tail stall, and reports the
application lifecycle failure. The frozen limits are 600/600 (99.9% of 600)
and 200 ms. They were not adjusted after observation.

## Criteria

- **SC-001 PASS:** deterministic Core and neutral-fixture suites report no
  duplicate, partial, out-of-order, stale-session, or post-stop delivery.
- **SC-002 FAIL as a treatment claim:** the two accepted periodic impaired
  runs meet every bound, but only 2/5 are accepted; three fail continuity or
  coverage as listed above.
- **SC-003 PASS:** variable impaired accepts 5/5 with 600/600 complete samples,
  zero invalid/partial delivery, p95 39.908-42.608 ms, and tail stall
  118-281 ms.
- **SC-004 PASS:** all four capacity-plus-one cells fail closed, record terminal
  skips, deliver no invalid items, and retain later progress.
- **SC-005 PASS:** periodic and variable zero-loss future-hit ratios are
  1.000000 and 0.951546; Mapping novelty is 1.000000; Payload overhead is
  0% and 6.4408%.
- **SC-006 PASS for every accepted impaired run:** future-hit ratio is at
  least 0.95, retry work is bounded and separate, and Payload overhead is
  below 25%.
- **SC-007 FAIL overall:** periodic impaired is 2/5; variable impaired is 5/5.
  Exact intervals are reported above. Capacity-plus-one is evaluated only as
  an all-cell safety gate and passes 4/4.
- **SC-008 PASS:** all required traffic/status fields are present or explicitly
  unavailable; identity, qdisc, hashes, invocation count, and verdict exist for
  every cell.
- **SC-009 PASS:** the scoped Core/binding audit finds no UAV, workload,
  application-identity, payload-semantic, or codec-selected recovery branch.
- **SC-010 PASS:** all Spec 127 evidence hashes match before and after; no
  Spec 127 destination or repetition was created.
- **SC-011 PASS:** this report preserves the negative result and makes no
  population, physical-wireless, or untested-workload generality claim.

## Traffic attribution

Every run separately reports Payload Interests, Mapping Data and new Mapping
Data, retry work, timeout, Nack, initial/retry future hits, recovery, skips,
coverage, continuity, and latency. Mapping novelty is 1.000000 in all 16 cells.
Accepted periodic impaired runs use 15-16 retry Payload Interests and 2.31-2.46%
Payload overhead. Accepted variable impaired runs use 97-121 retry Payload
Interests, 7.28-7.95% overhead, and recover 667-700 sources. Timeout and Nack
counts remain observable rather than being interpreted as application policy.

## Verification

- full build: 346 targets, PASS;
- forced in-place Python binding rebuild: PASS;
- full C++ suite: 341/341, PASS;
- Python Core/generality/runner: 19 + 27 + 6 = 52/52, PASS;
- security contract: 12/12 plus focused native 115/115, PASS;
- fresh MiniNDN confirmation: 16/16 executed once, aggregate NEGATIVE;
- source and Spec 127 before/after hashes: identical.

## Claim boundary and next work

Spec 128 establishes the generic two-erasure path, binding/metric parity,
one-shot evidence controls, zero-loss utility, variable multisegment impaired
recovery, and capacity-plus-one fail-closed behavior. It does not establish the
periodic one-item impaired continuity claim. Any attempt to improve the latter
belongs in a new Spec based on the retained 2/5 result; confirmation03 and its
thresholds are immutable and must not be tuned or rerun.
