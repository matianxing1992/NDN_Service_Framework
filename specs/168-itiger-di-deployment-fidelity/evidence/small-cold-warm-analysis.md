# Small-model cold/warm analysis checkpoint

The immutable Job 182777 schedule has been requalified from its retained raw
evidence with the repaired concurrent-stream analyzer. The Slurm batch remains
`FAILED` as submitted because analyzer v92 rejected stdout interleaving; the
raw directory and that terminal state are not rewritten. The requalified
analysis is a separate derived artifact and is accepted for the schedule
claims below.

Evidence identity:

- Job: `182777`, source bundle `sha256:4b57ef47...`, source identity
  `sha256:ce906305...`, SIF digest
  `sha256:1f616fa7...`, small-model stage manifest
  `sha256:8d8475db...`.
- Requalified analysis:
  `evidence/tiger-small-repeated/182777-v92-fetch-event-boundary-requalified/analysis.json`
  with SHA-256
  `sha256:14b587e55f9546556bac47ed2c165523ca230880ba106538b316070433ab3e64`.

The analyzer reports `PASS`: all 30 scheduled rows completed (one
`GENERATED` cold row and 29 `REUSE_CACHED` warm rows), with 30 wire Requests,
zero per-token Requests, zero CPU fallback, and a passing security and causal
reuse verdict. Every row contains the same request/plan lineage and a complete
authenticated response. Across all rows, median TTFT is 172.95 ms and median
total latency is 5,185.30 ms; these are descriptive values, not significance
claims. The warm subset has median TTFT 172.55 ms and median total latency
5,185.30 ms. The cold row is intentionally reported separately (74,125.16 ms
TTFT and 78,630.26 ms total latency) because it includes distribution and
preparation.

The earlier Job 182508/182509 evidence remains retained as historical negative
evidence: it stopped at 19/30 after a bf16 top-logit tie, and the exact-token
oracle was classified as numerically equivalent. It is not merged into the
30-row distribution and no missing rows are imputed. Likewise, Job 182773 is
retained as the prior analyzer-cardinality failure.

The accepted claim is therefore bounded to one immutable 30-row, single-
allocation small-model schedule with descriptive cold/warm reuse evidence. It
does not establish inferential significance, cross-allocation reproducibility,
or any large-model result; those remain separate Spec 168 tasks.
