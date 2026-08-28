# Performance and Compatibility-Deletion Gate

Date: 2026-07-14  
Performance verdict: **PASS**  
Compatibility deletion verdict: **RETAIN ALL — external-use gate unresolved**

The clean final matrix completed 20/20 cells and 1,200/1,200 measured requests
with zero correctness/completion failure. The deterministic p50 and p95 latency
bootstrap upper bounds are +4.51% and +4.01%; the throughput lower bound is
0.00%. All satisfy the frozen 5% non-regression margin, so T189 does not invoke
the performance rollback.

Passing performance does not independently authorize compatibility deletion.
Both caller snapshots report zero repository callers, but no compatibility
entry has external migration confirmation or a user-approved expiry. The
separate `compatibility-exit-gate.json` therefore evaluates 54 entries, marks
0 eligible and preserves `compatibility/exports.py` unchanged. Every retained
entry records its owner, rollback release and remaining external gate.

Historical failed matrices remain diagnostic-only negative evidence. The gate
does not relabel them, and it does not create any OCI/SIF/iTiger claim.
