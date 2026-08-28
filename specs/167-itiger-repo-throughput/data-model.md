# Data Model: TigerCluster DistributedRepo Throughput Validation

## CampaignManifest

- schemaVersion
- campaignId and submissionId
- sourceManifestSha256 and sifSha256
- node list, interface identities, route identity
- payload identities and sizes
- subject list and randomized schedule
- repetitions, warmup policy, minimum measurement duration
- hard deadline and progress policy
- analyzer version/hash

Immutable after submission.  Any change requires a new campaign identity.

## RunRecord

- runId, pairId, subject, size, repetition, warmup
- node/rank, local data/store/destination paths
- start/end/progress timestamps and terminal state
- logical bytes, Data/Interest wire bytes, retransmitted bytes
- publication, verification, persistence, cold retrieval, and warm reuse times
- logical goodput, CPU, peak RSS, read/write amplification
- content digest, destination visibility, cache state
- failure code and reason

Every scheduled run has exactly one terminal record.

## CoordinationRecord

Small checksum-bound readiness/progress metadata shared between ranks.  It may
contain names, digests, counters, and timestamps but never payload bytes,
private keys, or reusable credentials.

## DerivedResult

- admitted/scheduled/completed/failed counts
- sample distributions per matched cell
- paired ratios and bootstrap confidence intervals
- physical-ceiling utilization
- security, path-isolation, checksum, and cleanup verdicts
- PASS/FAIL/INCONCLUSIVE per success criterion

Derived results are reproducible from CampaignManifest and RunRecord ledgers.
