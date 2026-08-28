# Data Model: Synchronous NDN-SVS Stage Profiling

## 1. ProfileSubject

| Field | Type | Rule |
|---|---|---|
| `subjectId` | string | `sync-publish-no-internal-parallelism-profiled` |
| `baseCommit` | 40-char hex | Exact `a9944019f76791773604999f00128057b9534ace` |
| `baseTree` | 40-char hex | Exact base tree |
| `boostPatchSha256` | hex | Canonical Specs 131/132 patch identity |
| `profilePatchSha256` | hex | Frozen diagnostics-only patch |
| `profilePatchPaths` | string array | Explicit reviewed allowlist |
| `cleanHead`/`cleanTree` | hex | Base plus Boost-only patch |
| `profiledHead`/`profiledTree` | hex | Clean subject plus profiling patch |
| `cleanLibrarySha256`/`cleanBinarySha256` | hex | Exact preflight control artifacts |
| `profiledLibrarySha256`/`profiledBinarySha256` | hex | Exact formal artifacts |
| `compileCommand`/`linkage` | object | Compiler, flags, libraries, `ldd`, Boost version |
| `publishApi` | string | Exactly `publish` |
| `asyncSymbolsReachable` | boolean | Exactly false |
| `parallelWorkers` | null | No receive/production pools |
| `compressionEnabled` | boolean | Exactly false |
| `securityProfile` | object | Resolved signer/validator modes |
| `profileConfig` | object | Logger, sample modulus, clock, schema versions |

T002 first writes a `subject-foundation.json` containing the base/Boost-only
identity and clean library artifact. T008 writes `subject-manifest.json` and
both same-driver binaries only after the profiling patch and driver exist. The subject becomes immutable
before overhead preflight. A change to either patch, binary, library, driver,
logger configuration, or stage registry creates a new subject and invalidates
an unstarted manifest.

## 2. StageDefinition

| Field | Type | Rule |
|---|---|---|
| `stageId` | enum string | Stable identifier from the stage contract |
| `displayName` | string | Reviewer-facing label |
| `path` | enum | publisher, sync-produce, sync-receive, mapping, payload, app-boundary |
| `kind` | enum | leaf-cpu, aggregate, lock-wait, queue-wait, external-wait, milestone |
| `threadRole` | enum | face-io-app, face-io-sync, face-io-fetch, callback, external, mixed |
| `operationUnit` | enum | publication, sync-interest, mapping-range, payload-fetch, callback |
| `parentStageId` | string/null | Aggregate containment relationship |
| `eligibleForCpuSum` | boolean | True only for mutually exclusive leaf CPU spans |
| `sourceAnchor` | string | Historical file and symbol |

## 3. StageSpan

| Field | Type | Rule |
|---|---|---|
| `schemaVersion` | string | `spec133-stage-span-v1` |
| `cellId`/`peerId` | string | Matches sealed cell and process |
| `stageId`/`kind`/`threadRole` | enum | Matches registry exactly |
| `traceKey` | string | Stable sampled correlation key |
| `parentTraceKey` | string/null | Aggregate relationship when applicable |
| `nodeId` | NDN URI/null | Producer identity where known |
| `seqLow`/`seqHigh` | uint64/null | Publication or range identity |
| `startRawNs`/`durationNs` | uint64 | `CLOCK_MONOTONIC_RAW`; duration nonnegative |
| `outcome` | enum | success, miss, invalid, nack, timeout, exception, skipped |
| `bytes`/`items` | uint64 | Work size where applicable |
| `sampleModulus` | uint | Exactly frozen subject value |
| `correlationMode` | enum | exact, ambiguous, censored |

All stages for one sampled operation unit share the sampling decision. A child
must lie within its aggregate parent when both exist.

## 4. StageAggregate

Exact process-end record for one stage:

| Field | Type | Rule |
|---|---|---|
| `schemaVersion` | string | `spec133-stage-summary-v1` |
| `stageId` | string | Registry entry |
| `calls`/`successes`/`failures` | uint64 | Covers every operation |
| `sampledCalls`/`droppedRecords` | uint64 | Diagnostic accounting |
| `totalDurationNs` | uint64 | All-call measured duration for this stage |
| `minDurationNs`/`maxDurationNs` | uint64 | All-call extrema |
| `totalBytes`/`totalItems` | uint64 | Work volume |
| `threadRole` | enum | Registry value |

`totalDurationNs` for aggregate parents is never added to child totals.

## 5. OverheadAdmission

One diagnostic receipt binds the clean control, profiled-disabled, and
profiled-enabled arms to their binary hashes and matched runtime settings. It
reports A-vs-B instrumentation, B-vs-C logging, and A-vs-C total deltas for
attempted rate, delivery ratio, and CPU utilization, plus schema/process status.
It is never a formal `ProfileCell`.

## 6. ProfileCell

| Field | Type | Rule |
|---|---|---|
| `ordinal` | integer | 1..5 |
| `ratePpsPerPeer` | integer | 200, 400, 600, 800, 1000 |
| `aggregateTargetPps` | integer | Twice the per-peer rate |
| `peers` | array | Exactly peer-a and peer-b |
| `timing` | object | 10 s warmup, 60 s measure, 10 s drain |
| `attempt` | integer | Exactly 1 |
| `outputPath` | path | Unique and nonexistent before start |
| `profileConfigSha256` | hex | Frozen logger/stage configuration |
| `terminalReceipt` | object | Exactly one immutable outcome |

State: `PLANNED -> RUNNING -> COMPLETE | SUBJECT_FAILURE | INFRA_INVALID`.
No terminal state returns to PLANNED.

## 7. PathSummary

For each peer/direction/rate: exact publications, Sync Interests, coalesced
updates, Mapping candidates, Mapping piggy hits, Mapping fallbacks, Payload
piggy hits, Payload fallbacks, Nacks, timeouts, validation failures, delivered,
duplicates, and invalid payloads. Derived ratios use attempted or relevant path
opportunity as an explicit denominator.

## 8. StageSummary

Joins StageAggregate with valid sampled spans and reports calls, valid samples,
mean, p50/p95/p99/max, calls/attempt, calls/delivery, all-call duration,
estimated exclusive demand per publication, thread CPU share, residual, and
validity flags. Wait kinds report wait distributions but never CPU share.

## 9. BottleneckFinding

| Field | Type | Rule |
|---|---|---|
| `rank` | integer | Separate sequence for main, Face, external wait |
| `stageId` | string | Registry stage or grouped path |
| `affectedRates` | integer array | Observed rate boundary |
| `demandEvidence` | object/null | Demand/share and change |
| `latencyQueueEvidence` | object/null | p95/p99 and growth |
| `pathEvidence` | object/null | Branch/calls-per-publication change |
| `throughputEvidence` | object | Attempted/delivered boundary |
| `verdict` | enum | supported, candidate, inconclusive |
| `limitations` | string array | Alternative explanations/confounds |

A `supported` finding requires at least two non-null agreeing evidence classes.
