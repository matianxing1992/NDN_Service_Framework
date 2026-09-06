# Spec179 Post-Implementation Audit — Independent Re-Audit (2026-09-05)

This is an independent re-audit of the 2026-09-04 release audit
(`AUDIT.md`, verdict PASS). It was requested as "审核并修复" (audit and fix),
so findings are reported and the cross-document fixes were applied. The audit
follows `.specify/memory/speckit-audit-principles.md` (12 principles) and
reports in four separate layers: 文档声称 / 代码实现 / 测试执行 / 实验测量.

Method: authoritative checkpoint documents (`tasks.md`, `traceability.md`,
`evidence/release-gate.md`, `AUDIT.md`) as source of truth; CodeGraph against
real source; an independent re-run of the gate suites on the current
post-removal binary; per-scenario `result.json` inspection of the MiniNDN
campaign.

## 1. 文档声称 (proposed)

- `spec.md` (FR-001–FR-038, SC-001–SC-022), `plan.md`, `contracts/`,
  `data-model.md`, `tasks.md` T001–T013, `traceability.md`,
  `validation-matrix.md`, `AUDIT.md`, `evidence/release-gate.md`.
- Release claim under review: all 13 tasks complete; every RV-U01–RV-U21 and
  RV-I01–RV-I31 row has an executed mapping; 14/14 MiniNDN campaign scenarios
  `gatePassed=true`; T012 migration removal executed; verdict PASS.
- **Inconsistencies found and fixed** (details in findings R179-A1–A5):
  - `tasks.md` T005–T008 were unchecked `[ ]` with "remains open" closing
    sentences while T013/AUDIT claim all 13 tasks complete.
  - `validation-matrix.md` frozen 2026-09-02 sections still said
    "remains open/remain pending" for items closed on 2026-09-04.
  - `release-gate.md` called three residual items "documented non-goals"
    without a corresponding entry in `spec.md` `## Out of Scope`.
  - `results/spec179-minindn/campaign-summary.tsv` predated the final runs
    (mtime 2026-09-04 13:08) and showed `gatePassed=False` for scenarios whose
    final `result.json` says true.
  - `release-gate.md` used the scenario name
    `hintless-scheduled-revocation-discovery` while the campaign matrix uses
    `hintless-scheduled-refresh`.

## 2. 代码实现 (implemented)

Verified against the working tree with CodeGraph (verbatim, line-numbered):

- **Provider fail-closed after carrier removal**:
  `ServiceProvider.cpp:5136-5157`
  (`finishRequestExecutionOnEventLoop`): with request-scoped state the
  response is AEAD-encrypted (inline or per-segment large-data reference);
  without it, a large response returns the typed error "large response
  requires request-scoped confidentiality (service-wide response-key carrier
  removed)" and becomes an error response. No plaintext fallback, no
  resurrected carrier. Request-scoped keys are zeroized and the nonce registry
  invalidated after publication (`ServiceProvider.cpp:5263-5271`).
- **User fail-closed large-response resolution**:
  `ServiceUser.cpp:7043-7305`
  (`resolveLargeResponseReferencePayload`): `keyScope=="request"` requires
  pending invocation key state, ControllerVersion equality, provider
  certificate match, binding match, and unexpired keys — each failure is a
  typed error. Segments are fetched with the configured trust-schema
  validator (comment at `ServiceUser.cpp:7105-7109`: ValidatorNull "would
  allow a forged segment to reach request-scoped decryption"), then
  decrypted per-segment with unique `segmentOrEventId` enforcement. The
  non-request scope fails closed with "legacy service-wide response-key
  carrier removed" (`ServiceUser.cpp:7345-7346`).
- **Status refresh authority fence**: `PolicyRefreshCoordinator.cpp:33-208` —
  install requires a valid Controller signature; stale and
  conflicting-equal-version statuses are rejected; in-flight fetch
  coalescing; authenticated hints never become authority.
- **Durable Controller generation**: `ServiceController.cpp:248-288`
  (`initializeControllerGeneration`): writer lease acquisition, revocation
  load, `startGeneration` — a failed lease leaves the Controller unable to
  publish rather than reusing a version. ABE public-parameter versioning at
  `ServiceController.cpp:327-344`.
- **Removed symbols**: no residual reference to
  `NDNSF_REQUEST_SCOPED_COMPATIBILITY` or
  `makeResponseWithLargeDataOptimization` in source; the only remaining
  occurrences are the intentional inert-env regression in
  `tests/unit-tests/generic-dynamic-api-prepared.t.cpp:639-716`, stale binary
  objects in the old `build-clang/` tree, and documentation.
- **Binary currency**: last source edit 2026-09-04 17:30
  (`ServiceUser.cpp`), binaries built 17:43/17:44, gate logs 17:58–18:00 —
  the executed gates cover the current source. Re-verified 2026-09-05: no
  source file is newer than the test binaries.

## 3. 测试执行 (executed)

Independent re-run on 2026-09-05 against the current post-removal binary
(`build-clang-spec179-nac3`, `LD_LIBRARY_PATH=/tmp/nac-abe-spec179-exact-prefix/lib`):

| Gate | Command | Result |
|---|---|---|
| Unit | `unit-tests --run_test='RequestScopedConfidentiality,ControllerRevocationPolicy,ControllerRevocationState,GenericDynamicApi'` | exit 0 |
| Integration | `integration-tests --run_test='ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,RequestScopedResponseConfidentiality'` | exit 0 |
| Stream | `integration-tests --run_test='Spec175InvocationStream'` | exit 0 |

241 test cases entered, "No errors detected" ×3. Log:
`/tmp/spec179-reaudit-gate.log` (2026-09-05). This independently confirms the
T013 gate claim.

Named-case existence checks confirmed every case cited by the RV-U/RV-I
executed mapping exists in the cited test file (e.g.
`ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`,
`GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut`,
`RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`,
`LiveControllerStatusRefreshRejectsRevokedRenewal`,
`TargetedStreamRevocationStopsBeforeBootstrap`,
`StreamEventAfterProviderRevocationIsRejectedAtPublication`,
`RequestScopedDefaultActivationWithConfiguredController`,
`GenerationPersistsAcrossRestartAndClockRollback`,
`ServiceControllerRejectsSecondWriterAndPreservesAuthority`, …).

## 4. 实验测量 (measured)

- 14 scenario directories under `results/spec179-minindn/`, each with a
  `result.json`: all 14 report `gatePassed=true`, `networkEvidence=true`,
  `status=completed` (checked programmatically, 2026-09-05). Per-scenario
  applied-and-observed checks, redacted trace hashes and counters are in
  `evidence/minindn-campaign-20260904.md`.
- The campaign doc itself records the two real defects it caught (S9
  versionless Targeted requests, S10 discarded stream admission result) with
  rebuild + genuine rerun — honest negative-result handling.
- `campaign-summary.tsv` was stale (pre-final runs) and contradicted the
  final results; regenerated from the final `result.json` files on
  2026-09-05 (14/14 `gatePassed=True`).

## Findings and fixes applied

| ID | Severity | Principle | Finding | Disposition |
|---|---|---|---|---|
| R179-A1 | MEDIUM | 4 (跨文档一致性) | `tasks.md` T005–T008 unchecked `[ ]` with stale "remains open" sentences contradicting T013/AUDIT "all 13 tasks complete" | FIXED: checkboxes set `[x]`; each closing sentence replaced with the executed closing evidence and citation |
| R179-A2 | MEDIUM | 4, 10 (冻结证据) | `validation-matrix.md` frozen 2026-09-02 sections carried "remains open/remain pending" that the 2026-09-04 mapping closed | FIXED: dated supersession banner added after both frozen section headings; frozen rows themselves untouched |
| R179-A3 | MEDIUM | 1 (意图一致性), 11 (迁移) | `release-gate.md` "documented non-goals" for NAC-ABE internal cache renewal, persistent runtime-cache restoration, and production live trust-anchor installation had no corresponding `spec.md` Out of Scope entry | FIXED: the three items added to `spec.md` `## Out of Scope` as deferred runtime hardening with owner (NDNSF maintainer) and reintroduction criteria (new FR + RV row required) |
| R179-A4 | LOW | 9 (证据完整性) | Stale `campaign-summary.tsv` (13:08, several `gatePassed=False`) contradicted final `result.json` | FIXED: regenerated from final `result.json`; 14/14 `gatePassed=True` |
| R179-A5 | LOW | 4 | `release-gate.md` scenario name `hintless-scheduled-revocation-discovery` vs campaign `hintless-scheduled-refresh` | FIXED: name aligned to the campaign matrix |
| R179-A6 | HIGH | 9, 11 | The entire Spec179 implementation, `specs/179-request-scoped-confidentiality/` (untracked), all source/test changes, and the evidence are uncommitted in the working tree (HEAD `e2d793e8`) | NOT FIXED here: requires a user decision. A working-tree loss would destroy the implementation and all evidence. Recommend committing the Spec179 changeset (code + specs + evidence) as the release closure, mirroring `e2d793e8` for Spec176. |
| R179-A7 | recorded | tooling | context-mode guard `health` returned `HOST_RESTART_REQUIRED` (client predates settings.json) | Recorded; fallback used: direct file reads + ctx tools + git + CodeGraph. No impact on this audit. |

## Verdict

**PASS.** The four layers are now consistent: documents claim completion only
where the executed mapping and measured campaign support it, the three
residual runtime-hardening items are formally recorded as deferred non-goals
with reintroduction criteria instead of being left as contradictory "open"
sentences, and the independent gate re-run (unit 0 / integration 0 / stream
0, 241 cases) reproduces the T013 claim on the current binary. CodeGraph
facts confirm fail-closed behavior at both carrier-removal sites and the
authority fence in the refresh coordinator. No CRITICAL/HIGH design defect
was found. **R179-A6 (uncommitted working tree) is a mandatory
before-commit checklist item, not a spec defect**: commit the Spec179
changeset and evidence before treating the release as durable.
