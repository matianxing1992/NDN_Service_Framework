# Data Model: Complete SVS V3 Review Commit

## RewriteBaseline

Immutable identity snapshot captured before history construction.

| Field | Meaning |
|---|---|
| `upstreamBaseOid` | Read-only upstream base of PR #36 |
| `oldMasterOid` | Local master before rewrite |
| `oldOriginMasterOid` | Remote-tracking head after fresh fetch |
| `oldExperimentalOid` | Validated Experimental head |
| `oldExperimentalTree` | Functional source-tree preservation oracle |
| `prNumber` / `prHeadOid` | Open review identity |
| `capturedAt` | UTC timestamp |
| `worktreeState` | Clean/dirty inventory and ownership |

Validation: master and origin/master must initially agree unless the mismatch is
resolved before planning a candidate; PR head must equal recorded origin head.

## CommitOwnershipManifest

Declares the semantic owner of every replayed change group.

| Field | Meaning |
|---|---|
| `changeGroup` | Stable behavior identifier |
| `classification` | official-v3, fork-extension, reliability, harness, docs |
| `sourceCommits` | Old commits contributing hunks |
| `targetCommit` | Planned rewritten owner |
| `paths` | Relevant source/test paths |
| `acceptanceGate` | Test or inspection that closes the group |

Invariant: every official-v3 group maps to exactly one replacement V3 commit;
no fork-extension group maps there.

## RewriteCandidate

Locally constructed, unpublished history.

| Field | Meaning |
|---|---|
| `replacementV3Oid` | Complete standards-compliant V3 commit |
| `candidateMasterOid` | Rewritten PR #36 head |
| `candidateExperimentalOid` | Replayed Experimental head |
| `candidateExperimentalTree` | Final tree for equality check |
| `baseOid` | Parent before rewritten range |
| `commitList` | Ordered commit/OID/title/owner list |
| `status` | constructed, isolated-validated, fully-validated, rejected |

## SafetyRef

Recoverable name bound to a pre-mutation identity.

| Field | Meaning |
|---|---|
| `localName` | Immutable local backup ref |
| `remoteName` | Remote backup branch |
| `expectedOid` | Exact old origin head |
| `verifiedLocal` | Local resolution result |
| `verifiedRemote` | Fresh `ls-remote` resolution result |
| `retention` | Must survive Spec 115 completion |

## ValidationRecord

Candidate-bound gate result.

| Field | Meaning |
|---|---|
| `candidateOid` / `treeOid` | Identity being tested |
| `gate` | composition, isolated, per-commit, interop, MiniNDN, consumer |
| `command` | Exact reproduction command |
| `startedAt` / `finishedAt` | Execution timestamps |
| `result` | pass, fail, blocked, not-run |
| `counts` | Test/cell/acceptance counts |
| `artifactPath` | Durable evidence location |

Invariant: a record for an old OID cannot satisfy a new candidate gate.

## PublicationReceipt

Post-mutation proof of remote state.

| Field | Meaning |
|---|---|
| `expectedOldOriginOid` | Lease fence used for update |
| `publishedMasterOid` | New local/remote master head |
| `replacementV3Oid` | Shared complete V3 ancestor |
| `experimentalOid` | Final local Experimental head |
| `remoteBackupOid` | Verified rollback identity |
| `prHeadOid` | PR #36 head after publication |
| `containmentResults` | V3 ancestry across three required refs |
| `rollbackCommand` | Exact lease-protected restoration recipe |

## State Transitions

```text
PLANNED
  -> BASELINE_PINNED
  -> BACKUP_VERIFIED
  -> LOCAL_REWRITE_CONSTRUCTED
  -> ISOLATED_V3_VALIDATED
  -> EXPERIMENTAL_REPLAYED
  -> FULLY_VALIDATED
  -> PUBLICATION_AUTHORIZED
  -> REMOTE_PUBLISHED
  -> REMOTE_VERIFIED
```

Any pre-publication state may transition to `REJECTED` without touching the
remote review branch. `REMOTE_PUBLISHED` may transition to `ROLLED_BACK` only
through the recorded candidate-to-backup lease fence.
