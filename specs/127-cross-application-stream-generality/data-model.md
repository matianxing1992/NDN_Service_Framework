# Data Model: Cross-Application Stream Generality

## `WorkloadManifest`

```text
workloadId                 periodic-sensor | variable-multisegment
seed                       immutable generator seed
periodMs                   100
warmupSeconds              5
measurementSeconds         60
expectedMeasuredSamples    600
classProfiles[]            opaque class ID, seed extent, hard maximum
payloadRule                deterministic byte-generation rule
fecRule                    none | xor-one-repair
manifestDigest             SHA-256 of canonical manifest
```

Invariant: the manifest is written and hashed before a live cell; no field may
change across repetitions of that workload.

## `OpaqueWorkloadSample`

```text
sampleId
classId
publicationMonotonicUs
actualSourceItems
sourceLengths[]
sourceDigests[]
repairSelected
measurementPhase           warmup | measured
```

Invariant: Core sees only existing signed Mapping/sample-envelope fields and
opaque source bytes. Workload semantics never affect validation or scheduling.

## `CompleteSampleReceipt`

```text
sampleId
classId
expectedSourceItems
receivedOrRecoveredItems[]
contentDigests[]
firstInterestMonotonicUs
completedMonotonicUs
ordered
duplicate
partial
terminalReason
```

Invariant: one sample contributes to `completeSamples` once only after every
source digest matches. Partial or conflicting content never becomes complete.

## `GeneralityCell`

```text
cellId                     unique workload/profile/repetition identity
workloadId
networkProfile             zero-loss | combined
repetition                 1..5
exactCommand[]
outputPath
sourceDigest
workloadManifestDigest
automaticRetry             always false
invocationCount            exactly one
```

State:

```text
planned -> preflight-valid -> invoked -> evidence-complete -> analyzed
                         \-> retained-failure
```

No state transitions back to `invoked`; defect closure creates a new campaign.

## `TrafficUtilityReport`

```text
payloadInterests
necessarySourceRepairItems
payloadInterestOverheadRatio
mappingInterests
mappingDataResponses
mappingNewDataResponses
mappingNewDataRatio
mappingBytes
retryAttempts
timeouts
nacks
providerFutureInterests
providerFutureHits
providerFutureHitRatio
```

All counts are measured-window values. A zero denominator produces an explicit
unavailable reason rather than a favorable default.

## `GeneralityVerdict`

```text
periodicZeroLossPassed
periodicCombinedAcceptedRuns
variableZeroLossPassed
variableCombinedAcceptedRuns
allMetricsPresent
sourceUnchanged
historicalEvidenceUnchanged
claimLevel                  none | bounded-two-family
```

Invariant: `bounded-two-family` requires both zero-loss cells and at least 4/5
combined cells for each workload, plus all shared utility/security gates.
