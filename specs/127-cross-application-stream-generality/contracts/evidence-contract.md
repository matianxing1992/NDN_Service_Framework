# Contract: Spec 127 Evidence

## Per-run schema

Every run summary contains:

```text
schemaVersion, cell, startedAt, endedAt, command, returnCode
automaticRetry, sourceDigest, workloadManifestDigest
effectiveQdiscBeforeApps, effectiveQdiscAtEnd
expectedSamples, publishedSamples, completeSamples
duplicates, partialSamples, outOfOrderSamples, recoveredSamples
skipsByReason, tailMaximumStallMs, measurementCoverage
publicationToDeliveryMs.{samples,p50,p95,p99}
payloadInterests, necessarySourceRepairItems, payloadInterestOverheadRatio
mappingInterests, mappingDataResponses, mappingNewDataResponses
mappingNewDataRatio, mappingBytes
retryAttempts, timeouts, nacks
providerFutureInterests, providerFutureHits, providerFutureHitRatio
trafficCounterScope
checks, accepted, rerunAllowed
```

Publication-to-delivery latency and completion/continuity use measured samples
only. Traffic counters use the complete fixed run, including the identical
five-second warm-up, and declare
`trafficCounterScope=full-run-including-warmup`. This causal cohort avoids
invalid boundary subtraction when a future Interest is expressed just before
the measurement boundary and its Data is produced just after it.

`rerunAllowed` is always false. Missing evidence is an explicit failed check;
the analyzer never imputes zero or a favorable ratio.

## Campaign schema

The campaign summary contains all 12 run objects, per-treatment accepted counts
and exact 95% Clopper-Pearson intervals, source/workload/history hash verdicts,
and one final claim level. `campaign-runs.csv` has exactly 12 data rows and one
unique `cell.id` per row.

## Acceptance aggregation

```text
periodic zero-loss:             1/1
periodic combined:              >=4/5
variable multisegment zero:     1/1
variable multisegment combined: >=4/5
```

All successful-run SC thresholds remain mandatory. Failed repetitions retain
all available metrics but do not need to satisfy success-only latency/utility
thresholds. No population reliability claim follows from five repetitions.
