# T215 Negative Result and Durability Audit

Date: 2026-07-15  
Scope: local MiniNDN only; no Docker, Apptainer, iTiger or Slurm activity.

## Preserved second negative candidate

Candidate `spec111-local-d3c6f0517f96-c46998d0823c` completed one clean,
single-candidate 20-cell matrix at:

```text
results/spec111-core-app-separation/
  non-regression-spec111-local-d3c6f0517f96-c46998d0823c-final
```

All 20 cells and all 1,200 measured requests passed correctness and cleanup.
The frozen paired analysis nevertheless failed:

- p50 median paired relative change: `+6.5534%`, bootstrap 95% interval
  `[+0.6123%, +11.3445%]`;
- p95 median paired relative change: `+3.5806%`, bootstrap 95% interval
  `[+0.1106%, +7.8100%]`;
- throughput: unchanged at `1.0 request/s`;
- treatment peak process-tree RSS: approximately `11.65%` lower;
- correctness/completion: PASS;
- latency p50/p95: FAIL.

Immutable evidence:

- `campaign-summary.json` SHA-256:
  `b87cdde0bd0e67c2f2a0faf1c74dd17304cd7dc4e5f96e109a580eb3999be57a`;
- `paired-analysis.json` SHA-256:
  `5cf0c49df24d613d00df548c6843e6a810b6ed2e445386b20e0656150753b7d4`.

No T215 cell was rerun, replaced, deleted or reclassified.

## Timing decomposition

The 60 measured native client timings were separated from the outer public APP
latency in every cell. Across the ten treatment cells, the already-batched APP
durability layer added approximately `3.9-4.2 ms` to each request. Native-only
paired p50 changes had median `+3.0578%` and bootstrap 95% interval
`[-2.8414%, +8.0838%]`; native-only p95 changes had median `+1.0728%` and
bootstrap 95% interval `[-2.5751%, +4.9066%]`. Therefore removing all APP
overhead would still not make the recorded native p50 interval pass. The raw
matrix remains the controlling negative result.

## Durability defect found after T215

Reviewing the fixed mechanism against FR-086 found that `write_envelope()` used
`write + rename` but did not sync either the protected temporary file or its
directory entry. The prior `3.9-4.2 ms` measurement therefore was not evidence
of the required crash-durable submission. The implementation now:

- opens a unique owner-only temporary file with exclusive/no-follow flags;
- flushes and `fsync`s the protected bytes before replacement;
- atomically replaces the target and `fsync`s the spool directory;
- computes the wire digest from the exact encoded bytes without rereading;
- caches the just-certified result for same-process adapters while restarted
  clients still decrypt the durable spool;
- converts asynchronous result-spool failure into terminal
  `RESULT_PERSISTENCE_FAILED` rather than leaving the request in `PREPARING`.

Focused evidence: 12 request-handle tests and 7 RuntimeJournal tests pass. A
300-request mock-network microbenchmark remained flat, with median public APP
latency approximately `3.62 ms`; the durability sync operations dominate that
local cost.

## One post-fix diagnostic, not a matrix rerun

Exactly one isolated treatment readiness diagnostic was run at:

```text
results/spec111-core-app-separation/diagnostic-durable-fsync-treatment-03
```

It completed 70/70 requests with zero fatal findings, zero surviving MiniNDN
processes and removal of its ephemeral APP state root. For its 60 measured
requests:

- outer treatment p50/p95: `130.6245 / 151.3141 ms`;
- native network p50/p95: `124.47 / 143.8645 ms`;
- durable APP overhead p50/p95: `5.8320 / 7.22445 ms`;
- first/last-ten outer medians: `116.04 / 130.7265 ms`.

Evidence SHA-256:

- `readiness-result.json`:
  `7c499fca391d8eccc43cce60db1f17db2ec9301354060b7a518ed6642df9c40d`;
- `llm-pipeline-user-measured.csv`:
  `eb7afb67c500c3829c9dcf66f500b1027914cd957e86a7e92057993d1ae26cb4`.

The diagnostic source-tree identity was
`sha256:c63b8e83f779560e48ff0097f131aae8e6dc0a0e73f9996b4314e0248255d297`;
it was not promoted to a formal candidate.

## Gate decision

Do not start another 20-cell matrix yet. SC-007 currently compares the old
baseline synchronous path, which has no durable APP journal/spool, with a
treatment path that FR-086 and FR-090 require to persist both the protected
request and the recoverable result before the synchronous adapter completes.
This is an unmatched-semantics comparison, while the observed native p50
interval also shows material MiniNDN pair variance. T217 must resolve the
prospective acceptance contract under strict audit. Both historical end-to-end
failures remain visible regardless of that decision.
