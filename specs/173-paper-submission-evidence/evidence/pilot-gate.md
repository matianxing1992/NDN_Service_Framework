# Spec 173 Pilot Gate

**Decision**: PASS — proceed to the frozen confirmatory campaign.

The canonical v2 pilot ran from `2026-08-12T04:49:52Z` through
`2026-08-12T05:02:30Z` and completed all four registered matched blocks and
all six cells. It is operational evidence only: the campaign manifest records
`mode=pilot` and `manuscriptEligible=false`. No system ranking or pilot
performance value may enter the manuscript.

## Gate checks

| Check | Result |
|---|---|
| Campaign reached terminal pass | PASS |
| All six registered cells produced parseable summaries | PASS |
| Issued outcomes reconcile as success + timeout + other failure + pending | PASS |
| All normalized runs satisfy their mechanism-aware validity rule | PASS |
| Admission-enabled cell exposes admission counters | PASS |
| Custom-selection cell exposes a Provider-selection distribution | PASS |
| Selective-ACK correctness regression passes | PASS |
| Analysis exclusions | 0 |
| Outcome-based retries | 0 |

This decision inspects only operational completeness. It intentionally does
not compare success, throughput, latency, or Provider rankings.

## Frozen identities

- Registration SHA-256: `64fce70880fd0451999d5920b9c7730a9c1b74578ad0a193f356529dcdff5310`
- Toolchain-manifest SHA-256: `a633411bacce596e20ceed585d16a36966a929394d97fa8682da865ee3f1ed7c`
- Campaign-runner SHA-256: `03ef256be0af762ec21dc294ac65d1bb74c2e3eefab2d263523079cfd1969e94`
- Campaign manifest SHA-256: `0526b1ed4b36769b95408176d010a861222c2ba6670d7d0374b87759aad40032`
- Campaign summary SHA-256: `3b3e8c3b8e93e990a68b673173b62e811c9eda803f8db439a9f2da32c31e6832`
- Analysis manifest SHA-256: `a851cd6582290c9100ee2ce9abd45d3c598925d983dadee5af87da591917286b`
- Normalized-run SHA-256: `304e4cc2e1f7d651dd875c408a3b1749c95781327d5092dd750b50259c39cf3a`
- Exclusion ledger SHA-256: `f68d0ffa39fdc9427fc02038eef12507ca6c867cba024d308b58ea6ec74a17e6`

The retained canonical pilot root is
`results/spec173-paper-submission-pilot-v2/`. The earlier
`results/spec173-paper-submission-pilot/` is superseded because its App_User
used a target-count end condition in the no-admission path instead of the
registered absolute wall-clock generation boundary.
