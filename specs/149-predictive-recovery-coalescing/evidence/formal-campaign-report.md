# Spec 149 Formal Campaign Report

## Frozen result

Result root:

```text
results/spec149-predictive-recovery-formal-20260726T045802Z
```

The campaign ran both frozen cells once. `automaticRetry=false`,
`rerunAllowed=false`, and source/binary hashes remained unchanged. The terminal
campaign result is **FAIL (1/2 accepted)**.

| Cell | Measured | Delivery | Mapping | Recovery control | Retry | Timeout | FEC attempts | FEC recoveries | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| zero-loss | 76.051 s | 99.7793% | 0 | 0 | 0 | 0 | 0 | 0 | PASS |
| 1% loss + 1% reorder | 76.225 s | 1.4901% | 0 | 2,314 | 709 | 1,461 | 1,267 | 474 | FAIL |

The impaired cell also recorded 3,153 coalesced waiters and 3,529 verified
metadata cache hits. This proves the new recovery-control mechanism executed
and removed the old false Mapping count. It did not restore ordered delivery.

## Failure interpretation

The consumer delivered exactly cursors 0 through 399. It then stopped
application admission although recovery continued: 474 recovered sources were
validated, but no recovered item was delivered to the application. The final
status contains two cumulative terminal gaps and 400 delivered items.

Current source shows that terminal-gap insertion, concurrent `drainReady()`
calls, and late recovery insertion share mutable ordered-delivery state without
a single serialized drain owner or an explicit wake/progress invariant. The
frozen evidence therefore identifies an ordered-drain/terminal-gap boundary,
not another Mapping lookup failure.

Spec 149 MUST NOT be rerun. A successor feature must add deterministic
terminal-gap progress tests and ready-queue observability before any new formal
campaign.
