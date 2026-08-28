# T212 Negative Result and Journal Remediation

Date: 2026-07-15  
Scope: local MiniNDN only; no Docker, Apptainer, iTiger or Slurm activity.

## Preserved negative candidate

Candidate `spec111-local-825e9b7da919-c46998d0823c` ran one clean formal
20-cell matrix at:

```text
results/spec111-core-app-separation/
  non-regression-spec111-local-825e9b7da919-c46998d0823c-final
```

All 20 cells and all 1,200 measured requests completed, with zero request or
cleanup failures. The paired gate nevertheless **failed** and the result is not
accepted:

- p50 median paired relative change: `+380.16%`, bootstrap 95% interval
  `[+217.00%, +551.48%]`;
- p95 median paired relative change: `+343.65%`, bootstrap 95% interval
  `[+207.91%, +492.57%]`;
- throughput: unchanged at `1.0 request/s`;
- correctness/completion: PASS;
- latency p50/p95: FAIL.

Immutable evidence:

- `campaign-summary.json` SHA-256:
  `6af203182d9287a72ed506af1170613c234a495bee4e7a0ff3bd85a07f4d5e02`;
- `paired-analysis.json` SHA-256:
  `bd18356493c577cd19a632a384efa6294c663dabec3b42772d2dd4ad536a080c`.

No T212 cell was rerun, replaced or deleted.

## Root cause

The treatment path used the durable canonical `APPClient`, while the immutable
baseline did not. Every treatment process used the same default
`/tmp/ndnsf-di-app-state`; after readiness and ten treatment cells it contained
770 request identities, 6,930 logical journal records and a 2,409,330-byte
journal. `RuntimeJournal.append()` recursively scanned every spool file for
each record, and `APPClient._events()` reparsed or traversed all historical
records for every transition. This made the durable path O(N^2). Treatment p50
rose from `253.09 ms` in pair 1 to `915.20 ms` in pair 10, while baseline stayed
near `100-127 ms`.

## Remediation

- `RuntimeJournal` now maintains incremental usage and parsed-record caches.
- `APPClient` indexes handles and events by request ID.
- adjacent ordered state records use one checksummed journal transaction and
  one `fsync`; a torn transaction recovers none of its logical records.
- every treatment readiness/cell command uses a unique
  `/tmp/spec111-app-state-<digest>` root and deletes it after MiniNDN cleanup;
  Provider/request keys and encrypted spools never enter the result tree.
- the obsolete shared state root was removed after its counts were recorded.

The 300-request local microbenchmark changed from a growing `3.73 -> 4.60 ms`
first/last-50 median to a flat `2.22 -> 2.12 ms` after indexing and batching.

## Diagnostic convergence

Two isolated treatment-only 70-request MiniNDN diagnostics were run, not a
replacement formal matrix:

1. `diagnostic-journal-index-treatment-01`: 70/70 PASS, p50 `128.19 ms`,
   first/last-10 `116.41/124.17 ms`; it proved O(N^2) removal but retained too
   much fixed `fsync` overhead. Result SHA-256:
   `ed7a8e220074fd1955b3c0323b708127c88c045e058feda6063a6ceb8f46f08d`.
2. `diagnostic-journal-batch-treatment-02`: 70/70 PASS, p50 `114.66 ms`,
   first/last-10 `116.87/120.96 ms`; the matched baseline readiness p50 was
   `115.14 ms` and approximate p95 was `142.07 ms` versus treatment
   `136.20 ms`. Result SHA-256:
   `d322bbb22940cc4b43cfaa8b7d24e994c08c9986e48bc3d48625ed9c6fbdadd2`.

Both diagnostics had zero fatal log findings, zero survivors and confirmed
removal of their ephemeral APP state roots. T215 must use a new candidate and
new result root; T212 remains negative evidence.
