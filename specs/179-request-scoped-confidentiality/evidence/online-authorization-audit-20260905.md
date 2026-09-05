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
| OA-1 | HIGH | `revoke()` checks duplicates before pending recovery; `grant()` has no pending guard. Reproduction being compiled. | Retry the same failed withdrawal and guard grant before any policy mutation. A failed retry retains the withdrawal; successful reauthorization uses a fresh ABE pair. |
| OA-2 | HIGH | `reconcilePendingAbeRotation()` changes public-parameter identity at the unchanged ControllerVersion. Runtime rejects equal-version conflicting wire. Reproduction being compiled. | Persist a newer epoch before each recovery attempt; status already exposed at an earlier version never changes. |
| OA-3 | HIGH, evidence | Executed collector regression reduced success+failure to one success. Reprocessing the actual baseline yields 16 rows, 14 success, 2 failures (old result claimed 14/14). | Keep every terminal row; allocate startup/workload/drain time and rerun genuine MiniNDN. |
| OA-4 | MEDIUM, automation | Executed CLI regression returned 0 for completed + gatePassed=false. | Require explicit passing gate for exit 0. |
| OA-5 | MEDIUM, coverage | Existing grant arrives during permission retries; no post-exhaustion renewal evidence. | New scenario must observe timeout exhaustion < grant < App refetch <= first request, plus target-only DKEY replacement and successful unaffected control. |

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
