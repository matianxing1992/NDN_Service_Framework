# Data Model: NDN-SVS Experimental Convergence

## MigrationBaseline

- `capturedAt`: timestamp
- `remoteUrl`: expected origin URL
- `remoteMasterOid`: pinned target baseline
- `remoteExperimentalOid`: optional unchanged remote reference
- `localMasterOid`: original local master
- `experimentalOid`: original Experimental HEAD
- `mergeBaseOid`: common ancestor used for audit
- `statusDigest`: digest of porcelain status
- `trackedDiffDigest`: digest of binary-safe tracked diff
- `selectedArtifactDigests`: local-only design/workflow artifact hashes

Validation: all OIDs exist locally; status/diff/artifact digests recompute before
any branch move.

## RecoverySnapshot

- `branchName`: permanent `backup/...` reference
- `snapshotOid`: commit containing the captured delta
- `parentOid`: original Experimental HEAD
- `manifestPath`: immutable migration evidence
- `includedPaths`: tracked and force-added local recovery inputs
- `excludedSensitivePaths`: explicit proof that no credential/auth file entered

State transitions:

```text
Proposed -> Captured -> Verified -> Permanent
                    \-> Invalid (stop)
```

## ReviewCommit

- `oid`
- `concern`: mapping | recovery | publication | build
- `sourceCommits`
- `changedPaths`
- `requirements`
- `tests`
- `rollbackEffect`
- `scopeSearchResult`

Validation: one concern, at least one requirement, tests appropriate to risk,
and no local-only workflow paths.

## PublicationTransaction

- `nodePrefix`
- `bootstrapTime`
- `sequence`
- `packets`
- `mapping`
- `storeState`: none | partially-stored | stored | removed
- `commitState`: prepared | queued | committing | retry-wait | advertised | aborted
- `attempts`
- `nextRetry`
- `visibleSequence`
- `terminalReason`

Allowed transitions:

```text
prepared -> queued -> committing -> advertised
                          |             
                          +-> retry-wait -> committing
prepared/queued/retry-wait -> aborted (shutdown only)
```

Invariant: visible sequence and commit cursor cannot advance beyond the first
non-advertised transaction.

## ValidationCandidate

- `candidateId`
- `experimentalOid`
- `masterOid`
- `backupOid`
- `trackedDiffDigest`
- `binaryDigests`
- `scriptDigests`
- `configuration`
- `cellIds`
- `outcomes`
- `createdAt`

Validation: identity is immutable; any source/binary/configuration mutation
requires a new candidate.
