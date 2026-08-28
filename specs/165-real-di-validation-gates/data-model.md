# Data Model: Real DI Validation Gates

## FidelityRecord

Required fields:

- `schemaVersion`, `caseId`, `gateId`, `runId`
- `startedAt`, `completedAt`, `status`, `failureReason`
- `exactCommand`, `sourceRevision`
- `fidelityTier`
- `realComponents[]`, `simulatedComponents[]`
- `networkMode`, `containerMode`
- `modelIdentity`, `hardwareProfile`
- `skipIsFailure`, `evidencePaths[]`

`status` is `PASS`, `FAIL`, `SKIP`, or `ERROR`. A mandatory case accepts only
`PASS`; unknown schema versions and contradictory inventories are invalid.

## ModelIdentity

- `name`
- `revision`
- `contentDigest`
- `manifestDigest`
- `localSnapshot`

The name is descriptive; immutable revision and content digest are admission
bindings.

## WorkloadManifest

- `schemaVersion`
- `workloadId`, `workloadDigest`
- `modelIdentity`
- `prompts[]`
- `warmupPerPrompt`
- `measuredPerPrompt`
- `minimumGeneratedTokens`
- `maximumGeneratedTokens`
- `seed`
- `requestedBackend`, `fallbackPolicy`

Gate B and Gate C must consume byte-equivalent canonical manifests.

## InvocationIdentity

- `requestId`
- `attempt`
- `planId`
- `selectionDigest`
- `modelIdentityDigest`

It is immutable after Selection. Before Selection, the plan fields may be
absent but cannot change once admitted.

## LineageEvent

- `ordinal`, `observedAt`
- `eventType`
- `provider`, `role`
- `identity`
- `authenticated`
- `admitted`
- `rejectionReason`
- `operationId`, `epoch`, `sequence` where applicable
- `evidenceDigest`

Events are append-only. Rejected events remain visible and never mutate the
invocation.

## GenerationMeasurement

- `promptId`, `kind` (`WARMUP` or `MEASURED`)
- `identity`
- `answer`
- `tokenEvents[]`
- `ttftMs`, `totalLatencyMs`, `generatedTokens`, `tokensPerSecond`
- `requestedBackend`, `actualBackend`, `devicePlacement`, `fallbackCount`
- `terminalStatus`, `failureReason`

Each token event contains ordered index, token ID, decoded fragment, and
latency. A measured record with fewer than the configured minimum token events
is a failure, including early EOS.

## ProgressObservation

- `requestId`, `operationId`
- `provider`, `role`
- `attempt`, `epoch`, `sequence`
- `phase`
- `completedWork`, `totalWork`
- `authenticated`
- `observedAt`

Admission requires correct invocation bindings, a newer epoch/sequence
position, and advancing completed work or an allowed advancing phase.

## DeadlineState

- `startedAt`
- `lastAdmittedProgressAt`
- `idleBudget`
- `idleDeadline`
- `hardDeadline`
- `terminalState`
- `terminalAt`, `terminalReason`

Invariants:

1. `hardDeadline` never changes.
2. `idleDeadline <= hardDeadline`.
3. rejected observations change neither deadline.
4. the first terminal transition is final.
5. simultaneous expiry yields `HARD_TIMEOUT`.

## AggregateVerdict

- `schemaVersion`, `runId`, `sourceRevision`
- `policyDigest`
- `mandatoryCaseIds[]`
- `records[]`
- `passCount`, `failCount`, `skipCount`, `errorCount`
- `passed`
- `externalValidationAuthorized`
- `machineSummaryPath`, `humanSummaryPath`

The human and machine summaries must derive from the same in-memory verdict.
`externalValidationAuthorized` can be true only when all mandatory cases pass.
