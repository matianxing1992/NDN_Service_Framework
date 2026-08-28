# Data Model: NDNSF External Bug Report Corrections

These are implementation/evidence records for the five reported defects. They
are not new public NDNSF wire types or APIs.

## SegmentedPublicationRecord

- `publicationName`, `sequenceNumber`, `providerEpoch`
- `applicationByteCount`, `segmentCount`
- per segment: inner name, inner wire size, outer name, final outer wire size
- `packetLimit`, `preparedPacketCount`, `storedPacketCount`, `advertised`
- `reservationState`: none, tentative, committed, rolled-back
- `status`: prepared, advertised, delivered, failed, timed-out
- `failureStage`, `failureReason`, `cleanupComplete`
- Validation: every final outer wire size is at most 8800 B; failed preparation
  is not advertised and does not leave a visible sequence gap; terminal cleanup
  is recorded.

## TargetedRequestDeadline

- `requestId`, `providerName`, `serviceName`
- `acceptedAt`, `timeoutMs`, `deadlineAt`
- `phase`: admitted, bootstrapping, publishing, waiting, terminal
- `terminalOutcome`: response or timeout
- `terminalAt`, `responseCallbackCount`, `timeoutCallbackCount`, `lateEventCount`
- Validation: response plus timeout callback counts equal exactly one; timeout
  completes no later than the configured 500-ms scheduling tolerance.

## ProviderHealthEpoch

- `providerIdentity`, `processId`, `nodeId`, `startedAt`
- boundary request results
- 80-response 8-KB burst results
- 10 64-B and 12 4-KB post-burst results
- `exitCode`, `exitSignal`, `restartObserved`
- Validation: all burst and health checks use one process/node-id epoch and no
  restart is counted as recovery.

## PythonTargetedRecord

- `serviceName`, `handlerRegistrationMode`
- `providerTokensEnabled`, `userTokensEnabled`
- `invocationMode`: normal or Targeted
- `tokenCase`: valid, missing, mismatched, consumed, replayed
- `handlerCount`, `responseCount`, `timeoutCount`
- Validation: valid normal/Targeted calls invoke one handler; invalid token cases
  invoke no unauthorized handler.

## ProcessExitRecord

- `role`: Controller, Provider, or User
- `nacAbeInitialized`, `nacAbeUsed`, `shutdownKind`
- `exitCode`, `signal`, `timedOut`, `sanitizerFinding`
- Validation: initialized lifecycle evidence has no SIGSEGV or SIGABRT.

## EvidenceCandidate

- NDNSF, ndn-svs, NAC-ABE, ndn-cxx revisions and dirty-diff digests
- built library/test binary hashes and compiler/dependency versions
- diagnostic script/configuration hashes
- `candidateId`
- Validation: any changed input changes `candidateId`.

## EvidenceCell

- `candidateId`, `cellId`, `lossPercent` (fixed at 0)
- `invocationMode`: normal or Targeted
- `svsPublishMode`: synchronous or asynchronous
- `externalizationDisabled`, `payloadSequence`, `timeoutMs`
- `launcherPid`, `providerEpoch`, `resultDirectory`
- `status`: pass, fail, crash, hung, incomplete
- Validation: one candidate/cell has one owner and one immutable directory.

## Relationships

```text
EvidenceCandidate 1 -> N EvidenceCell
EvidenceCell 1 -> 1 ProviderHealthEpoch
EvidenceCell 1 -> N SegmentedPublicationRecord
EvidenceCell 1 -> N TargetedRequestDeadline
PythonTargetedRecord and ProcessExitRecord -> EvidenceCandidate
```
