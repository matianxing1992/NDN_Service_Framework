# Post-Implementation Audit: Spec 133

## Verdict

`PASS`

Spec 133 now has the required single-I/O-thread subject, admitted three-arm
preflight, sealed five-cell manifest, five once-only terminal receipts, full
stage/path tables, and a source-linked bottleneck report. The evidence
limitations below bound interpretation but do not invalidate the frozen
descriptive result. No failed or slow formal cell was replaced.

## Findings

| ID | Severity | Dimension | Evidence | Finding | Resolution |
|---|---|---|---|---|---|
| A-001 | HIGH | Historical harness | Spec 134 `io-qualification-01` and Spec 131 route logs | The old runner installed Sync first, then received `Error 409` while trying to create the same UDP face for the remote node prefix; it ignored the failure. | Preserve Spec 134 as `NOT_QUALIFIED`; Spec 133 explicitly creates one face, reuses its face ID for both routes, and verifies the RIB before peer start. |
| A-002 | HIGH | Thread ownership | corrected driver and manifest | Earlier cross-thread calls were not the historical serial application model. | Both peers now run timer, synchronous `publish()`, Face processing, fetch handling, and callbacks on one io_context thread. |
| A-003 | MEDIUM | Preflight analysis | original and corrected overhead receipts | The first derived CPU calculation used one global last sample after one peer exited, so A/C counted one process while B counted two. | Original `REJECTED` receipt retained. Corrected receipt uses each peer's first/last valid sample, binds the original SHA-256, records `networkRerun=false`, and passes all frozen bounds. |
| A-004 | MEDIUM | Span attribution | campaign summary and rate-stage table | 2,073 sampled child spans do not fit a same-trace aggregate parent, especially reused operation-boundary stage IDs. | Retain their exact per-stage all-call demand; exclude them from aggregate residual attribution; report the count and limitation. |
| A-005 | MEDIUM | Statistical scope | formal manifest | There is one ascending run per rate. | Results are descriptive and limited to the frozen topology/security/payload/subject. |
| A-006 | LOW | Local test stability | post-seal repeat of T006 self-test | One post-seal repeat returned 3 after the same clean/profiled self-tests had passed before manifest freeze; formal 5/5 processes remained complete. | Do not alter the sealed driver or rerun formal cells. Record the DummyClientFace self-test timing fluctuation as a follow-up test-hardening item. |
| A-007 | LOW | Workflow tooling | repository agent-context instruction | The documented `.specify/extensions/agent-context/...` script is absent. | Active feature JSON and Spec artifacts were maintained directly; no experiment effect. |

## Evidence scorecard

| Dimension | Result | Evidence |
|---|---|---|
| Intent fidelity | PASS | Historical synchronous, no async/parallel workers, exactly five target rates. |
| Architecture boundary | PASS | NDN-SVS-only profiling patch; no NDNSF runtime; active NDN-SVS checkout unchanged. |
| Build identity | PASS | Old base, Boost-only clean head, profiling patch, driver, binaries, libraries, compiler, and linkage hash-bound. |
| Network validity | PASS | 10/10 per-peer route records contain one face ID plus verified Sync and remote-prefix RIB entries. |
| Overhead admission | PASS | Maximum attempted-rate delta 0.435%; maximum corrected CPU delta 4.37 percentage points; invalid=0; remote delivery observed in all arms; 81 summaries complete. |
| Formal execution | PASS | Five rates in ascending order, five receipts, all `COMPLETE`, both peers rc=0. |
| Analysis completeness | PASS | 810 stage rows, cell/group/path/ranking tables, no invalid cell. |
| Attribution caveats | FLAG | 2,073 containment violations and overload censoring are explicitly bounded in the report. |
| Reproducibility | PASS | Sealed manifest, unique paths, exact commands, raw events/profile/resource logs, route evidence, and hashes retained. |

## Verification commands and results

- `python3 tests/python/test_spec134_svs_sync_crash_recovery.py`:
  15/15 PASS before qualification.
- `python3 tests/python/test_spec133_svs_sync_stage_profile.py`:
  25/25 PASS before subject freeze and formal execution.
- Post-analysis non-network contract subset:
  18/18 PASS (`Spec133AnalyzerContractTests`,
  `Spec133RunnerContractTests`, and `Spec133SubjectBuilderContractTests`).
- Formal campaign:
  `spec133-confirm-io01-20260723T040005Z`, 5/5 `COMPLETE`.
- Analyzer:
  5 valid cells, 0 invalid cells, 810 stage rows, 19 ranked findings,
  first boundary 800 pps/peer.

## Closure

- [x] T006 single-I/O driver and local path evidence.
- [x] T007 runner, route, admission, and five-cell contracts.
- [x] T008 immutable I/O manifest and corrected same-run overhead receipt.
- [x] T009 exactly five once-only MiniNDN cells.
- [x] T011 complete analysis and reviewer-facing report.
- [x] Negative Spec 134 receipt and original rejected preflight retained.
- [x] No formal retry, overwrite, or result-driven rate change.

## Recommended next Spec

Do not rerun Spec 133. A follow-up Spec should compare this frozen synchronous
baseline with the async/parallel implementation using the same verified routing
helper and a repeated/randomized design. It should also separate Fetcher queue
residence from network RTT with representative tail sampling and harden the
DummyClientFace self-test against timing flakes.
