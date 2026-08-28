# Spec 133 Pre-Implementation Audit

## 2026-07-22 Threading-Contract Reassessment

The earlier pre-implementation `PASS` is withdrawn for the driver/runner/
preflight portion. Exact NDN-SVS review found that README and the public header
do not promise thread safety, examples claim it without concurrent tests, and
Spec 134 sanitizers contradict the claim under load. T006--T008 are reset to
incomplete. The revised formal model uses one Face/io_context execution thread
per peer process. Instrumentation and analyzer work T003--T005/T010 remains
usable subject to new same-thread fixtures.

**Revised verdict**: `BLOCK` until Spec 134 qualifies the corrected harness.

**Mode**: pre-implementation  
**Date**: 2026-07-22  
**Verdict**: **PASS after remediation**  
**Implementation gate**: T002 may begin; formal cells remain forbidden until
T008 closes the three-arm overhead gate and seals the manifest.

## Findings

| ID | Severity | Dimension | Evidence | Finding | Resolution |
|---|---|---|---|---|---|
| A-001 | HIGH | Evidence integrity | Original FR-009 and plan overhead section compared only one instrumented binary with profiling disabled/enabled | That pair measured enabled logging cost but not the timers, branches, and counters that remain in the disabled path. | FR-009, SC-003, plan, data model, quickstart, and T002/T007/T008 now require clean-control, profiled-disabled, and profiled-enabled arms with A-vs-B, B-vs-C, and A-vs-C gates. |
| A-002 | HIGH | Performance attribution | Historical `core.cpp` and `svspubsub.cpp` explicitly acquire version-vector, scheduler, recorded-vector, and extra-data mutexes on the measured paths. | Protected-operation wall time would have mislabeled lock blocking as CPU demand and could hide the requested bottleneck. | FR-005/FR-006 and the stage registry now define `lock-wait`; protected leaf timers start only after acquisition. |
| A-003 | MEDIUM | Distributed measurement | Sync Interests do not carry a Spec 133 trace field, and FR-008 forbids wire changes. | Cross-peer Sync network wait was not guaranteed to have a unique join key. | The contract now permits only existing wire-derived identities, labels joins exact/ambiguous/censored, and excludes ambiguous/censored joins from wait and critical-path claims. |
| A-004 | MEDIUM | Scope consistency | Historical `core.cpp` contains optional compression and `SVSPubSub::publish` contains a segmented branch. | The fixed 256-byte campaign could otherwise appear to cover branches it never executes. | Compression is frozen off and the result is explicitly limited to the non-segmented 256-byte path. |
| A-005 | HIGH | Code reality | README/header are silent, historical examples claim cross-thread safety, tests are single-threaded, and Spec 134 confirms races. | The old driver does not establish the intended serial capacity and cannot admit formal profiling. | One Face/io_context execution thread per process is now required; preserve the old failure separately. |
| A-006 | LOW | Traceability | Strict structural scan reported no standalone traceability artifact. | Requirement coverage was inferable but not reviewer-visible. | `traceability.md` now maps every FR/SC/story to tasks and evidence; the summary table below mirrors it. |
| A-007 | HIGH | Task executability | Original T002 required profiled and benchmark binaries before T003--T006 created the profiling source and shared driver. | The dependency graph could not produce its requested artifacts in order. | The builder is now explicitly two-mode: T002 `prepare` freezes/builds the clean library foundation; T008 `finalize` freezes/builds both same-driver subjects after T003--T006. |

No CRITICAL or unresolved HIGH finding remains. A-005 is an intentional
historical-subject limitation, not authorization to claim race freedom.

## Code Reality Checked

- Exact base commit: `a9944019f76791773604999f00128057b9534ace`
- Exact base tree: `945a321d473f44f29e8349a83ce60373f3e37420`
- `SVSPubSub::publish` builds/signs/encodes inner Data, queues piggyback Data,
  publishes outer Data, inserts Mapping, and updates state.
- `SVSyncCore::sendSyncInterest` builds Mapping/piggyback data, encodes the
  version vector/ApplicationParameters, signs, and expresses the Interest.
- `MappingProvider::onMappingQuery` performs range lookup, MappingList encode,
  Mapping Data build/sign/put; its consumer validates/decodes/stores Mapping.
- `Fetcher` exposes queue admission, Interest expression, Data, Nack, timeout,
  validation, and retry boundaries.
- The base contains no `publishAsync()` or internal receive/production worker
  pool. Historical examples do contain an application-created Face thread;
  therefore the correct description is “no internal parallelization,” not
  “single-threaded process.”

## Traceability

| Intent / requirement | Design / contract | Tasks | Acceptance evidence |
|---|---|---|---|
| Exact synchronous historical subject; Specs 131/132 untouched (FR-001–FR-003, FR-017) | Subject identity/isolation; rollback boundary | T001, T002, T006 | Subject manifest, patch allowlist, tree/binary/linkage audit |
| Every expensive synchronous stage visible (US1, FR-004–FR-008) | Frozen stage registry and correlation contract | T003–T006 | Unit/contract fixtures plus complete two-peer smoke trace |
| Measure instrumentation without changing the subject claim (FR-009) | Three-arm overhead admission | T002, T007, T008 | Hash-bound overhead receipt with all three comparisons |
| Exactly five bidirectional rates and no replacement (US2, FR-010–FR-011) | Sealed five-cell manifest and terminal receipts | T007–T009 | Exactly five immutable receipts at 200/400/600/800/1000 |
| Strict accounting and complete metrics (FR-012–FR-016) | Stage/data model and analyzer rules | T006, T007, T010, T011 | Schema/accounting validation and required CSV/JSON/Markdown outputs |
| Evidence-backed bottleneck or explicit inconclusive result (US3, SC-006–SC-008) | Two-signal bottleneck rule | T009–T011 | Ranked tables tied to raw counters/spans and rate boundary |

All 17 functional requirements and all 8 success criteria map to at least one
task and an evidence path. No task lacks a requirement or user-story purpose.

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Old synchronous subject only; five formal rates only |
| Architecture and ownership | Yes | Profiling patch isolated in a dedicated historical worktree |
| Security/correctness | Yes, bounded | Formal signer/validator modes frozen; historical shared-state behavior disclosed |
| Task executability | Yes | T001–T011 have paths, dependencies, and acceptance evidence |
| Task cohesion/granularity | Yes | 11 outcome-oriented tasks; no mechanical test/edit/run fragmentation |
| Validation/evidence | Yes for implementation | Formal evidence does not exist yet and is not claimed |
| Migration/rollback | Yes | No production migration; temporary worktree/patch removable after preservation |
| Code reality | Yes | Current CodeGraph plus exact historical `git show` inspection completed |

## Metrics

- User stories: 3
- Functional requirements: 17
- Success criteria: 8
- Tasks: 11
- Completed tasks: 1
- Mechanically fragmented task groups: 0
- Requirement coverage: 100% designed
- Unmapped tasks: 0
- Placeholders: 0
- Findings: 0 critical, 3 high resolved, 3 medium resolved/bounded, 1 low resolved

## Evidence Limits

- No profiling code, clean/profiled binary, overhead receipt, smoke trace, or
  MiniNDN formal result exists yet.
- CodeGraph describes the current checkout; historical behavior was therefore
  verified separately from the exact Git object.
- Stage coverage for Mapping/Payload fallback is a design contract until T005
  and T006 execute their fixtures.
- One observation per formal rate will support descriptive attribution, not
  population-level inference.

## Next Action

Execute T002 only: create and audit the isolated clean foundation and profiling
worktree start. Do not claim or build the profiled subject until T008, do not
instrument the active NDN-SVS checkout, and do not consume a formal cell.
