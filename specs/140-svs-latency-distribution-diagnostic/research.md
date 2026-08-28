# Research Notes

## Why Raw Samples Are Required

A p50 or p95 cannot be recovered from a p99-only summary. Likewise, the
percentile of two peers' combined population cannot be obtained by averaging
their percentiles. Retaining the measured latency samples is the smallest
auditable representation for this two-cell diagnostic.

## Estimator Definitions

- Mean: integer sample sum divided by sample count.
- p50/p95/p99: nearest-rank percentile, matching the benchmark's existing p99
  implementation.
- Unit of record: integer nanoseconds; reports convert to milliseconds.

## Claim Boundary

The experiment is descriptive. One fresh cell per mode can confirm what
happened in that run and correct the missing-median evidence problem, but it
does not estimate run-to-run variance or establish statistical significance.
