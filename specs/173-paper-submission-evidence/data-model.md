# Data Model: Submission-Ready NDNSF Evidence

## ManuscriptClaim

| Field | Meaning | Validation |
|---|---|---|
| `claimId` | Stable claim identifier | Unique and non-empty |
| `location` | Manuscript section/table/figure | Must resolve in the current source |
| `text` | Allowed bounded wording | Must not exceed evidence scope |
| `kind` | design, correctness, performance, limitation | Enumerated |
| `evidenceIds` | Supporting evidence packages | At least one for quantitative claims |
| `status` | supported, qualified, removed, pending | Quantitative final claims cannot be pending |

## ExperimentRegistration

| Field | Meaning | Validation |
|---|---|---|
| `registrationId` | Immutable campaign identity | Unique |
| `frozenAt` | Time before confirmatory inspection | Required |
| `hypothesis` | Intended test, including null/neutral possibility | Required |
| `cells` | Complete comparison matrix | All controls listed |
| `primaryOutcome` | One pre-specified result per comparison | Required |
| `secondaryOutcomes` | Diagnostic results | Must be labeled secondary |
| `invalidityRules` | Infrastructure/load failures | Defined before run |
| `analysisRule` | Aggregation across independent repetitions | Required |

## ExperimentRun

| Field | Meaning | Validation |
|---|---|---|
| `runId` | System-condition-repetition identity | Unique |
| `registrationId` | Parent registration | Must resolve |
| `system`, `condition`, `repetition` | Cell identity | Must match registration |
| `sourceRevision` | Runtime and dependency revisions | Required |
| `registrationSha256`, `toolchainManifestSha256` | Frozen input identities | Required and must match the campaign root |
| `command` | Exact execution command | Required |
| `environment` | Relevant runtime configuration | Secrets excluded |
| `startedAt`, `endedAt` | Run interval | Monotonic and complete |
| `status` | planned, running, valid, invalid-infrastructure, failed | Enumerated; partial runs are never reused as valid |
| `rawArtifactHashes` | Integrity identifiers for retained inputs | Required for valid runs |

## RunSummary

| Field | Meaning | Validation |
|---|---|---|
| `runId` | Parent run | Must resolve |
| `sent`, `successful`, `timedOut` | Outcome counts | Non-negative; counts reconcile |
| `actualRps` | Measured generation rate | Must satisfy registered threshold |
| `latencySamples` | Successful-response sample count | Must equal latency observations |
| `meanMs`, `p50Ms`, `p95Ms` | Within-run descriptive latency | Successful responses only |
| `providerExecutions` | Work metric when applicable | Non-negative |
| `notes` | Registered anomaly or disclosed deviation | Cannot silently alter validity |

## EvidencePackage

| Field | Meaning | Validation |
|---|---|---|
| `evidenceId` | Stable evidence identity | Unique |
| `registration` | Registration path/hash | Required for new experiments |
| `runs` | Complete run set | No favorable omission |
| `summary` | Across-repetition analysis | Recomputed from runs |
| `provenance` | Source paths/revisions | Required |
| `integrity` | Hash list | Covers all retained inputs/outputs |
| `limitations` | Scope boundary | Required |

## ArtifactIndexEntry

| Field | Meaning | Validation |
|---|---|---|
| `manuscriptId` | Table, figure, or claim identifier | Must resolve |
| `evidenceIds` | Canonical evidence | Non-empty for numerical items |
| `analysisPath` | Reproduction procedure | Required |
| `status` | supported, replaced, removed | Enumerated |
| `reason` | Qualification/removal explanation | Required unless supported |

## State Transitions

```text
registered -> pilot-only -> confirmatory-running -> analyzed -> admitted
                                                \-> invalid-infrastructure
                                                \-> rejected-evidence

historical-claim -> recovered -> admitted
                 \-> replacement-registered -> admitted
                 \-> unsupported -> removed-or-qualified
```

An `admitted` result may enter the manuscript. Pilot-only, invalid, pending, or unsupported results may inform diagnosis but cannot support final precision.
