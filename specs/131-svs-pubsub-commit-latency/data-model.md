# Data Model: NDN-SVS PubSub Commit-Latency Comparison

## 1. VersionSubject

Immutable identity and deliberate behavior of one NDN-SVS subject.

| Field | Type | Rules |
|---|---|---|
| `subjectId` | enum | `baseline-sync-serial` or `latest-async-parallel` |
| `commit` | 40-char hex | Exact pinned commit |
| `baseTree` | 40-char hex | Tree of the exact pinned base commit |
| `temporaryBranch` | string | Local Spec 131 build branch; never pushed or merged |
| `temporaryHead` | 40-char hex | Local build-only commit on the base |
| `temporaryTree` | 40-char hex | Base plus canonical Boost 1.71 `wscript` patch |
| `buildPatchSha256` | hex | Identical canonical patch digest for both subjects |
| `buildPatchPaths` | string array | Exactly `["wscript"]` |
| `sourceStatus` | string | Empty/clean after the sole local build commit |
| `buildCommand` | string array | Exact configure/build invocation |
| `compiler` | object | Compiler path/version and flags |
| `boost` | object | Version 1.71, include/library paths, configure probe, and no-1.74 audit |
| `librarySha256` | hex | Built `libndn-svs` digest |
| `binarySha256` | hex | Matching benchmark digest |
| `dependencyManifest` | array | `ldd`/runtime dependencies; no NDNSF entry |
| `publishMode` | enum | `sync` or `async` |
| `wireVersion` | enum | Always `v2` |
| `workers` | object | Baseline fields `null`; treatment receive/production = 4, queue = 4096 |
| `resolvedTimers` | object | 1 ms lifetime, 500 ms suppression, 30 s periodic, 0.1 jitter |

Validation: a subject identity cannot change after the campaign manifest is
sealed. Both subjects must have byte-identical build patch bytes/hash, no
source delta outside the canonical two-line `wscript` patch, and clean
temporary worktrees. The baseline must sort before the treatment in campaign
order.

## 2. CampaignManifest

Content-addressed authority for one complete formal matrix.

| Field | Type | Rules |
|---|---|---|
| `schemaVersion` | string | `spec131-campaign-v1` |
| `campaignId` | string | Unique and immutable |
| `createdAt` | timestamp | Before first formal cell |
| `manifestSha256` | hex | Digest of canonical manifest content |
| `subjects` | VersionSubject[2] | Exact baseline then treatment |
| `buildPatchSha256` | hex | Shared canonical Boost 1.71 patch identity |
| `ratesPps` | integer[5] | Exactly `[200,400,600,800,1000]` |
| `repetitions` | integer | Exactly 1 |
| `scheduleSeed` | integer | Fixed before execution |
| `cells` | CellPlan[10] | Unique; all 5 baseline entries precede treatment |
| `timing` | object | converge 5 s, warmup 10 s, measured 60 s, drain 10 s |
| `payload` | object | Schema/digest/generator and exactly 256 bytes |
| `topology` | object | Two hosts, 100 Mbps, 10 ms, 0% loss |
| `cpuSets` | object | Frozen shared host affinity: CPUs 0--3 for every cell/role |
| `automaticRetry` | boolean | Must be `false` |
| `driverSha256` | hex | External source digest |
| `runnerSha256` | hex | Campaign runner digest |
| `analyzerSha256` | hex | Analyzer digest |
| `interveningCommits` | array | Exact ancestry from baseline exclusive to latest inclusive |

State transitions:

```text
DRAFT -> PREFLIGHT_PASSED -> SEALED -> BASELINE_RUNNING
      -> BASELINE_COMPLETE -> TREATMENT_RUNNING -> COMPLETE
Any formal failure -> INCOMPLETE (immutable terminal state)
```

`TREATMENT_RUNNING` is illegal unless every baseline cell has one terminal
receipt and the baseline block is complete.

## 3. CellPlan

One planned version/rate/repetition execution.

| Field | Type | Rules |
|---|---|---|
| `cellId` | string | Stable unique identifier |
| `ordinal` | integer | 1..10; baseline ordinals 1..5 |
| `subjectId` | enum | References VersionSubject |
| `ratePps` | integer | One frozen rate |
| `repetition` | integer | Fixed at 1; retained only for schema stability |
| `rateOrderPosition` | integer | 1..5; same schedule reused across subjects |
| `syncPrefix` | NDN name | Unique per cell/campaign |
| `publisherPrefix` | NDN name | Unique per cell |
| `subscriberPrefix` | NDN name | Unique per cell |
| `outputRelativePath` | path | Unique; cannot preexist for a startable cell |
| `maxAttemptCount` | integer | Exactly 1 |

## 4. PublicationObservation

Joined publisher/subscriber record for one logical publication.

| Field | Type | Rules |
|---|---|---|
| `logicalId` | uint64 | Unique inside cell |
| `phase` | enum | `warmup`, `measured`, or `drain` |
| `scheduledNs` | uint64 | Publisher `CLOCK_MONOTONIC_RAW` deadline |
| `apiEnterNs` | uint64/null | Present if API invocation began |
| `apiReturnNs` | uint64/null | Present if API returned |
| `svsSeqNo` | uint64/null | Returned public API sequence |
| `stateUpdateNs` | uint64/null | First subscriber update covering sequence |
| `deliveryNs` | uint64/null | First matching subscription callback |
| `duplicateDeliveryNs` | uint64[] | Every additional callback |
| `payloadSha256` | hex/null | Must match manifest when delivered |
| `orderIndex` | uint64/null | Subscriber callback order |
| `missedReleaseSlot` | boolean | True if pacer skipped rather than burst |
| `censoredAtNs` | uint64/null | Drain deadline when not delivered |

Derived values:

```text
apiDurationUs       = (apiReturnNs - apiEnterNs) / 1000
stateSyncDelayUs    = (stateUpdateNs - scheduledNs) / 1000
pubsubDelayUs       = (deliveryNs - scheduledNs) / 1000
deadlineCappedUs    = (deliveryNs ?? censoredAtNs) - scheduledNs
duplicateCount      = length(duplicateDeliveryNs)
outOfOrder          = orderIndex violates increasing svsSeqNo order
```

No negative duration is accepted. A payload mismatch is a failed delivery, not
a successful callback.

## 5. CellReceipt

Terminal evidence for one cell.

| Field | Type | Rules |
|---|---|---|
| `schemaVersion` | string | `spec131-cell-v1` |
| `cellPlanSha256` | hex | Binds exact plan |
| `attempt` | integer | Exactly 1 |
| `status` | enum | `SUCCESS`, `NEGATIVE`, `INFRA_INVALID`, `FAILED` |
| `startedAt`/`endedAt` | timestamps | Wall time for audit only |
| `monotonicClockProbe` | object | Namespace offsets and verdict |
| `returnCodes` | object | Publisher/subscriber/NFD/capture |
| `publicationCounts` | object | Scheduled, invoked, returned, delivered, missing, duplicate, reordered |
| `rateMetrics` | object | Offered, achieved, deviation, missed slots, sustained-rate validity |
| `delayMetrics` | object | Delivered and deadline-capped percentiles |
| `resourceMetrics` | object | CPU/RSS/NFD/link and worker metrics |
| `artifactDigests` | object | Raw events, logs, routes, qdisc, commands, binaries |
| `errors` | array | Classified; never silently omitted |

`SUCCESS` means evidence is structurally valid, not that latency met a desired
number. A valid overload/loss result may be `NEGATIVE` and remains analyzable.

## 6. RateComparison

Comparison at one offered rate.

| Field | Type | Rules |
|---|---|---|
| `ratePps` | integer | One of five requested rates |
| `baselineCells`/`treatmentCells` | CellReceipt[5] | No selective omission |
| `runLevelEffects` | array[5] | Matched p95 difference/ratio and delivery difference |
| `medianP95DeltaUs` | number/null | Treatment minus baseline |
| `medianP95Ratio` | number/null | Treatment / baseline |
| `replicationCount` | integer | Fixed to one for each direct subject-rate observation |
| `deliveryDelta` | number | Treatment minus baseline |
| `classification` | enum | `improved`, `regressed`, `inconclusive`, `incomplete` |
| `claimBoundary` | string | Always version-bundle wording |

## 7. CampaignSummary

Final aggregate with 10 terminal receipts or explicit incompleteness.

Required fields include subject identities, manifest hash, execution order,
cell counts by status, every RateComparison, highest sustained rate per
subject, host drift telemetry, intervening commits, limitations, and final
claim text.
