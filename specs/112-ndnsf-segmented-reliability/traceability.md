# Spec 112 Traceability

**Status vocabulary**

- `implemented-only`: supporting code or a source fact exists, but the required
  defect-closing execution evidence does not yet exist.
- `proposed`: the named task/test/evidence is specified but has not run.
- `executed-pass` / `executed-fail`: reserved for immutable executed evidence.

Final update: all rows below are backed by executed evidence for integrated
candidate `spec112-62b57fe47b2e3537ad23` or by the explicitly named focused
candidate/lifecycle artifact. Residual platform and sanitizer limits remain
stated rather than upgraded into unsupported claims.

## Functional Requirements

| Requirement | Email defect | Tasks | Required test or observation | Evidence target | Current level |
|---|---:|---|---|---|---|
| FR-001 final packet-size segmentation | 1, 2 | T013, T017, T020 | exact/below/above 8800-B signed inner/outer boundaries | `evidence/us1-focused-validation.md` | executed-pass: 16/16 `TestSVSPubSub` |
| FR-002 prepare before advertise | 1 | T014, T018, T020 | injected preparation failure exposes no sequence or unreadable publication | `evidence/us1-focused-validation.md` | executed-pass: preparation/store/put failures roll back |
| FR-003 contain async failures | 1, 2 | T014, T015, T019, T020 | encode/sign/store/put/validation failures do not terminate or poison later work | `evidence/us1-focused-validation.md` | executed-pass |
| FR-004 byte-exact Normal/Targeted sync/async | 1 | T004, T005, T016, T039 | six boundary sizes in all four mode combinations | final candidate four boundary cells | executed-pass: 24/24 |
| FR-005 same-epoch post-burst health | 1 | T010, T014, T040 | 80x8 KB then 10x64 B and 12x4 KB without Provider restart | `final-burst-async-normal/cell-summary.json` | executed-pass: 102/102, restart 0 |
| FR-006 reclaim failed receive state | 1 | T015, T019, T020 | malformed/missing/duplicate/timeout/shutdown cleanup | `evidence/us1-focused-validation.md` | executed-pass |
| FR-007 preserve automatic externalization | 1 | T010, T012, T039, T042 | isolated roles record test flag and logs prove no reference path | final six cell summaries plus scope diff | executed-pass: forced path verified; implementation untouched |
| FR-008 real binding registers both modes | 3 | T021, T023, T024 | one compiled handler completes Normal and Targeted | `evidence/us2-python-targeted.md` | executed-pass |
| FR-009 production tokens remain enabled | 3 | T021-T024 | compiled binding positive path and public-surface inspection | `evidence/us2-python-targeted.md` | executed-pass; no tokens-off API |
| FR-010 Targeted token failures fail closed | 3 | T022-T024 | missing/mismatch/consumed/replay negative cases | `evidence/us2-python-targeted.md` | executed-pass |
| FR-011 deadline starts at acceptance | 4 | T025, T028, T031 | admission/bootstrap/publication stalls cannot delay deadline | `evidence/us3-targeted-timeout.md` | executed-pass |
| FR-012 one terminal callback | 4 | T026-T031, T041, T048-T050 | absent/degraded Provider yields exactly one callback | US3 and final timeout cell | executed-pass: one timeout in 1064.569 ms |
| FR-013 late/duplicate events stay terminal | 4 | T026, T029-T031, T048 | late response, duplicate event, and sync local-fallback return do not revive or access returned stack state | `evidence/us3-targeted-timeout.md`, pre-completion convergence | executed-pass |
| FR-014 repeated safe OpenABE exit | 5 | T032-T036 | 300 initialized role exits without SIGSEGV/SIGABRT | `evidence/us4-openabe-exit.md` | executed-pass: 300/300 |
| FR-015 preserve security architecture | 3, 4, 5 | T022-T024, T026-T031, T037 | focused permission/routing/token/replay regressions | focused/final validation evidence | executed-pass |
| FR-016 Boost 1.71 matched rebuild | build prerequisite | T007-T009, T020, T037 | configure/build and rebuilt `TestSVSPubSub` | pre-fix build and final focused evidence | executed-pass: 16/16 |
| FR-017 exact candidate binding | evidence prerequisite | T001-T003, T010-T012, T038-T042 | manifest hashes repos, diffs, untracked inputs, binaries, tools/config | final manifest and summaries | executed-pass; identity reverified post-matrix |
| FR-018 run each candidate/cell once | evidence prerequisite | T002, T003, T010-T012, T038-T041 | existing/colliding paths rejected; failures retained | immutable result tree | executed-pass |
| FR-019 final 0% forced inline SVS | 1, 2, 4 | T010-T012, T039-T042 | MiniNDN reports 0% and forced flag in isolated roles | final campaign summaries | executed-pass: 6/6 cells |
| FR-020 five-defect scope only | all | T043-T047 | scope search plus code-aware final audit | `evidence/pre-completion-audit.md`, `evidence/final-audit.md` | executed-pass |

## Success Criteria

| Criterion | Tasks | Acceptance test | Evidence target | Current level |
|---|---|---|---|---|
| SC-001 four-mode six-size matrix | T016, T039, T042 | 24/24 byte-exact boundary observations | final campaign | executed-pass: 24/24 |
| SC-002 same-epoch health | T014, T040, T042 | 80/80 large, 10/10 64-B, 12/12 4-KB | final campaign | executed-pass: 102/102 |
| SC-003 no oversize/crash/unreadable advertise | T013-T020, T039-T042 | packet maxima, exit/liveness, publication ordering | focused plus final evidence | executed-pass: zero markers/abort, Providers healthy |
| SC-004 real Python Targeted secure behavior | T021-T024 | compiled positive and fail-closed negative cases | US2 evidence | executed-pass |
| SC-005 bounded exactly-once timeout | T025-T031, T041-T042, T048-T050 | one callback by `timeout_ms + 500 ms`, no late second callback or returned-stack access | US3/final evidence | executed-pass: 1064.569/1500 ms, count 1 |
| SC-006 300 clean role exits | T032-T036 | 100 Controller + 100 Provider + 100 User | US4 evidence | executed-pass: 300/300 |
| SC-007 rebuilt Boost 1.71 tests | T007-T009, T020, T037 | matched binary passes current relevant suite | build/focused evidence | executed-pass: 16/16 |
| SC-008 immutable owned evidence | T001-T003, T010-T012, T038-T042 | manifest, owner, command/config, unique directory, outcome | final candidate result tree | executed-pass: 6 unique owned cells |
| SC-009 no scope expansion | T043-T047 | strict structure, scope search, semantic/code-aware audit | `evidence/final-audit.md` | executed-pass |

## Executed Infrastructure Evidence

| Item | Result | Limitation |
|---|---|---|
| T001 current code/environment capture | implemented-only | source facts; no defect reproduction |
| T002/T003 candidate manifest | 6/6 unit tests pass | final identity includes Python `.so` and NAC lifecycle binary |
| T004/T005 deterministic diagnostic tools | 5/5 unit tests pass | final MiniNDN uses these exact payloads |
| T010/T011 campaign safety and summaries | 5/5 unit tests pass | final six-cell acceptance executed |
| T012 pre-fix boundary matrix | executed-fail: 16/24 exact, all four Providers exited on 8917-B outer Data | reproduces current defect; not completion evidence |
| T013-T020 US1 | 16/16 unit plus 24/24 final boundary and 102/102 burst | 0% MiniNDN only |
| T021-T024 US2 | compiled binding positive and token/replay negatives pass | reporter's old fork not rebuilt |
| T025-T031 US3 | 18/18 C++ Targeted; sync/async fault cells pass | final fault cell uses async API |
| T032-T036 US4 | 300/300 initialized role exits | no ASan/UBSan instrumentation |
| T038-T042 integrated candidate | 6/6 cells, aggregate `SUCCESS` | candidate verified before closeout-doc diff changed |
| T048-T050 convergence | shared synchronous Targeted terminal state; rebuilt binding; new candidate 6/6 | old candidate retained, not overwritten |

This T044 update uses retained command outputs and immutable result paths.
The post-convergence final audit is `PASS`; no traceability row remains pending.
