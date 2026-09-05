# Online authorization audit and MiniNDN follow-up

Scope: online grant/revoke correctness on `UAV-Experimental`, starting at
`d2833215`; user authorized audit, reproduction and fixes. T017–T018 track
cohesive Controller recovery and cross-process authorization outcomes.

## Findings and gate

Current verdict: **CONDITIONAL PASS for the bounded repair plan; verification
in progress**. This is not a release PASS. Earlier grant-only network success
counts require reevaluation because the collector censored failures.

| ID | Severity | Evidence | Required repair / acceptance |
|---|---|---|---|
| OA-1 | HIGH | Executed Controller regression: same-target retry does not recover; grant succeeds during persistent rotation failure and removes the target. | Retry the same failed withdrawal and guard grant before any policy mutation. A failed retry retains the withdrawal; successful reauthorization uses a fresh ABE pair. |
| OA-2 | HIGH | Executed Controller regression: direct recovery changes parameters at the unchanged version; real RevocationState rejects the conflicting wire. | Persist a newer epoch before each recovery attempt; status already exposed at an earlier version never changes. |
| OA-3 | HIGH, evidence | Executed collector regression reduced success+failure to one success. Reprocessing the actual baseline yields 16 rows, 14 success, 2 failures (old result claimed 14/14). | Keep every terminal row; allocate startup/workload/drain time and rerun genuine MiniNDN. |
| OA-4 | MEDIUM, automation | Executed CLI regression returned 0 for completed + gatePassed=false. | Require explicit passing gate for exit 0. |
| OA-5 | MEDIUM, coverage | Existing grant arrives during permission retries; no post-exhaustion renewal evidence. | New scenario must observe timeout exhaustion < grant < App refetch <= first request, plus target-only DKEY replacement and successful unaffected control. |
| OA-6 | MEDIUM, harness | Runtime trace kept enqueueing for about 51 seconds; launcher passed milliseconds to App_User's seconds-based --duration (and open-loop ignores --count). | Convert units and reserve independent late-grant control and drain windows. |
| OA-7 | HIGH, runtime | Actual late-grant network probe cannot emit App readiness or perform permission renewal: User construction waits for a DKEY. Provider has the same loop. | Return with asynchronous bootstrap pending; verify real constructors remain unauthorized and execute late online grant without restart. |
| OA-8 | MEDIUM, harness | Inflight scenario retains two incomplete control rows because one-second drain is shorter than its Provider delay/request timeout. | Reserve full request drain and role lifetime; rerun with every row retained. |

## Repair design and traceability

- FR-017/019/035/036, immutable status contract -> T017 ->
  `PendingRotationFencesGrantAndPreservesImmutableStatus`: independent same-target
  revoke, grant, and direct retry legs; force persistent failure before allowing
  recovery, retain targets, require fresh parameter digest and increasing version,
  and install both failure/recovery statuses through real `RevocationState`.
- FR-038/SC-022 -> T018 -> `grant-after-permission-exhaustion`: App-owned one-shot
  `NDNSF_PERMISSION_REFETCH_AFTER_MS`; no new runtime polling mechanism or wire API.
- T011/SC-011/SC-012 -> T018 -> retain failures and fail the launcher exit code;
  regression inputs cross the real evidence collector and CLI boundary.
- Repeated completed revokes remain no-ops. Only a pending failed operation
  gains successful retry behavior. No rollback of persisted epochs or remote
  key-erasure claim; an unsuccessful crypto recovery preserves the withdrawal.
- Rollback: revert the scoped repair commits; do not roll back durable Controller
  state. Old code still understands the same generation-store format. Reverting
  the recovery fix reintroduces OA-1/OA-2 and cannot be called a passing release.

## Verification checkpoints

- Context Mode stats screened only; project health passed, stale active hashes
  repaired by indexing canonical documents and active health passed. Repository
  tasks/source are authority. Two malformed project guard attempts corrected;
  the successful final query used `ndn-service-framework` in query and requirement.
- CodeGraph current at entry; verified affected source after semantic exploration.
- Spec Kit strict structural check and prerequisites passed. Read constitution,
  active artifacts, relevant contracts/evidence, architecture and failure log.
- GSD debug state: `.planning/debug/online-grant-revoke.md` (local ignored state).
  GSD health passed. Diagnosis performed inline. ARS is not applicable to this
  implementation/security regression task; no literature or performance claim.
- Harness red: 2/2 new regressions failed before patch; green: all 11 launcher
  tests pass. Checkpoint `4ceb8ce3` contains harness fixes and late-renewal probe.
- Real pre-fix baseline: `results/spec179-online-auth-20260905/baseline-grant/`.
  `networkEvidence=true`; one target refresh and zero unaffected refreshes;
  corrected collector finds **14/16 success**, so it is a failed gate.
- Build uses `/usr/bin/g++` (GCC 9), **despite directory name**
  `build-clang-spec179-rv32`, and NAC-ABE prefix
  `/tmp/nac-abe-spec179-exact-prefix`. Actual build config overrides old narrative
  claims that this candidate was built with Clang. Final library closure/hashes
  and repaired network results remain pending.
- Controller red re-executed from retained pre-fix binary:
  `results/spec179-online-auth-20260905/gates/integration-before-fix`,
  `--run_test=ControllerRevocationFlow/PendingRotationFencesGrantAndPreservesImmutableStatus`;
  exit 201, 10 failed assertions; `gates/controller-red.log`. The fixed case
  additionally retains the old DKEY and tests its inability to decrypt fresh
  post-recovery ciphertext, followed by successful replacement-key decryption.
- First late-grant run `late-grant-first` correctly failed its gate: serial
  startup passed the original grant offset, so exhaustion was never observed.
  Also corrected the result-export scenario guard and added its regression.
  `gates/harness-green.log`: 12/12 pass; corrected MiniNDN rerun pending.
- Rebuild `gates/build-green.log`: all 218 build tasks completed, 16m14s.
  Focused fixed Controller case: **36/36 assertions pass**, exit 0,
  `gates/controller-green.log`; expected wrong-generation OpenABE rejection
  appears in this negative crypto test. App_ServiceController resolves the
  candidate framework library and the intended patched NAC-ABE prefix via ldd.
- Expanded unit gate: `unit-tests --run_test=RequestScopedConfidentiality,ControllerRevocationPolicy,ControllerRevocationState,GenericDynamicApi,RuntimeStatusStorePersistence --report_level=detailed`;
  **182/182 cases, 11971/11971 assertions**, exit 0 (`gates/unit-green.log`).
- Expanded integration gate: `integration-tests --run_test=ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,RequestScopedResponseConfidentiality,Spec175InvocationStream --report_level=detailed`;
  **70/70 cases, 1262/1262 assertions**, exit 0 (`gates/integration-green.log`).
  Full 16-scenario network campaign running under `results/spec179-online-auth-20260905/campaign/`;
  `gates/campaign.log` and per-scenario result/manifest files retain failed runs.
- T017 network closure: `campaign/revocation-rotation-failure-retry/result.json`
  **gatePassed=true, 14/14 checks**. Injected rotation failure at epoch 2,
  explicit same-target retry recovered at epoch 3. All four runtime roles
  installed epoch 3; user/A had 16 successes before withdrawal, 10 denial
  log lines during pending rotation, 32 after recovery, and zero post-failure
  successful invocations. User/B had **16/16 successful post-recovery calls**.
- Diagnostic campaign completed **14/16 passing scenarios**, with failures
  retained for late-grant constructor blocking and insufficient inflight drain.
  These results precede the asynchronous startup change and are not its gate.
- Both real constructors now return with initial DKEY fetch pending. The new
  `UnprovisionedRuntimesConstructAndRemainUnauthorized` test passes **8/8 assertions**
  under a five-second timeout: no DKEY/status, zero request publication and zero
  executions of a registered Provider handler. See `gates/bootstrap-constructor-green.log`;
  test build `gates/build-bootstrap-tests.log` succeeded in 6m21s. App rebuild
  and final C++/network regressions remain pending. The network rejection is
  the pre-fix reproduction; this additional constructor case was not executed
  against the old binary.
- Final rebuilt unit gate after asynchronous constructor changes:
  **182/182 cases, 11971/11971 assertions** (`gates/unit-final.log`).
  Integration and final network results are still pending.

## Audit dimensions and limits

Intent/necessity/ownership: scoped to demonstrated recovery/evidence gaps, no
new policy authority. Task cohesion: two outcomes, no mechanical fragmentation.
Security/distribution: fail-closed recovery and immutable authority required;
historical disclosure remains irreversible. Migration: unchanged public wire and
store formats. Validation/evidence: component plus real wired MiniNDN, all
failures retained. Operations: App renewal explicitly controlled, not automatic
discovery promised by the runtime. Documentation: prior PASS is historical;
current closure awaits executed T017/T018 evidence. External NAC-ABE upstream
publication (T014) remains outside this local task.
