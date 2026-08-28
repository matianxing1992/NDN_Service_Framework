# Data Model: Zero-Loss Fetch Causality

## FetchTraceEvent

```text
peer
component
event
logTimestamp
monotonicNs?          # same-process durations only
interestName
interestNonce
attemptId?
semanticKind?         # mapping | publication | unknown
queueDepth?
pendingCount?
retriesLeft?
interestLifetimeMs?
result?
durationUs?
sourceFile
sourceLine
```

## FetchAttemptTimeline

```text
consumerPeer
producerPeer
interestName
consumerAttemptId
semanticKind
phase                 # warmup | measure | drain
consumerEvents[]
producerEvents[]
classification
classificationEvidence[]
queueToDispatchUs?
dispatchToTerminalUs?
producerLookupUs?
producerLookupToPutUs?
dataToValidationUs?
```

### Classification precedence

```text
validation failed after Data             -> VALIDATION_FAILURE_AFTER_DATA
producer store miss                      -> PRODUCER_STORE_MISS
producer hit/put, no consumer Data        -> PRODUCER_PUT_WITHOUT_CONSUMER_DATA
producer hit, no producer put             -> PRODUCER_STORE_HIT_WITHOUT_PUT
no producer observation                  -> NO_PRODUCER_OBSERVATION
otherwise                                -> UNCLASSIFIED
```

Precedence prevents one timeline from receiving multiple primary causes.
Secondary observations, including later same-name Data and validation
success/failure after Data, remain attached as evidence.

## ResourceWindow

```text
measurementWallMs
userCpuMs
systemCpuMs
totalCpuMs
cpuPctOneCore
cpuPctFourCore
maxRssKiB
threadsAtStart
threadsAtEnd
traceBytes
```

## DiagnosticReceipt

```text
schema
campaignId
cellId
status                  # DIAGNOSED | INCONCLUSIVE | HARNESS_FAILED
manifestHash
profileHash
commandsHash
startedAt
finishedAt
peerSummaries[]
rawTraceHashes[]
timeoutCount
classifiedTimeoutCount
classificationCoverage
classificationCounts{}
conditionalInlineAuthorized
conditionalInlineReason?
```

## Invariants

- `classifiedTimeoutCount <= timeoutCount`.
- `classificationCoverage = classifiedTimeoutCount / timeoutCount` when
  `timeoutCount > 0`; zero timeouts yield `INCONCLUSIVE`, not 100%.
- Cross-peer monotonic timestamps are never subtracted.
- Each consumer attempt has exactly one primary classification.
- Raw event references resolve to retained, hashed trace lines.
