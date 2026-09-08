# T007 Design-Code Convergence Audit

**Date**: 2026-09-07
**Scope**: submit → worker → application → Core/DI/Repo → collector closure
per the 12 speckit-audit principles (`.specify/memory/speckit-audit-principles.md`).
**Method**: CodeGraph symbol verification against real source, the 913-test
focused suite, and the real container execution receipts produced this cycle
(a3ad4454, 13c5e58a).  No task checkbox was trusted on its own.

## Verdict: BLOCK (HIGH)

T002/T005/T006 remain open on controlling semantic/evidence gaps (G1, G2, G3
below).  The audit is report-only; no fix is applied by this document.

## Four evidence layers

### 1. 文档声称 (documents)

- spec.md/plan.md/tasks.md traceability is intact through T004-T006; the
  task list accurately records open status (2/17 complete).
- `contracts/experiment-profile.md` workload semantics were found and fixed
  DURING this cycle: `workload.descriptor` must point at the existing service
  JSON template and `workload.packageManifest` at the package-root
  manifest.json.  The profile initially pointed both rows at the wrong files;
  corrected in a3ad4454 (descriptor → `specs/181/.../local-case-configs/y-b.json`,
  packageManifest → `.cache/model/spec183-signed/canonical-package/manifest.json`).
  This was a cross-document consistency failure that production code
  (`resolve_provision_inputs`, `prepare_in_container`) correctly rejected.

### 2. 代码实现 (code, CodeGraph file:line)

Verified against real source this cycle:

- `check_plane`/`check_chain` (runtime/yolo_profile.py:126/159) — content
  integrity recomputes every ancestor; plane identities are canonical
  document shas, never plane.json file shas (fixed in 652d13c4).
- `resolve_run_plan` (yolo_profile.py:414) — CLI `--output` now anchors to
  cwd, matching `_safe_output` (submit.py:42); previously profile-anchored,
  which made plan.output disagree with the actual freeze location.
- `_prepare` (jobs/yolo/submit.py:298) — real execution: froze a 21-file
  bundle (PREPARED, candidateDigest c7ec3d7f) and wrote prepare.json.
- `resolve_provision_inputs` → `stage_provision_inputs` → `provision_run`
  (yolo_profile.py:328, yolo_operator.py:55/97) — real execution produced
  tiger-yolo-preparation-v1 receiptDigest 968d0c93 inside the base SIF.
- `run_rank` (yolo_operator.py:253) → `run_normal_node` (apps/yolo.py) —
  real execution reached the Controller launch: in-SIF NFD 24.07 started and
  four nfdc commands exited 0 (tiger-yolo-network-setup-v1 receipt).
- Owner boundary `_installed_yolo_owner` (apps/yolo.py) — binds installed
  ndnsf/py_repoclient before the owner mutates sys.path; scoped to the SIF
  path so host MiniNDN keeps its own compiled pythonWrapper.
- `issue()` (runtime/identities.py:162) — clears only the root role's import
  side-effect keychain; real role homes still fail closed.
- `container_command` (runtime/baseline.py:98) — `--home` uses the absolute
  container-path form so apptainer never injects skeleton files after the
  caller's emptiness checks.

### 3. 测试执行 (tests)

913 focused tests pass (75s) after the fixes; the full suite was the commit
gate for 652d13c4/a3ad4454/13c5e58a.  Failures found during the cycle were
either real defects (fixed) or stale assertions of the old `--home` form
(updated to the corrected contract).

### 4. 实验测量 (measurements)

- Real containerized offline issuer: preparation receipt with 30+ public
  files (8 role certificates, case-policy, native-execution-plan,
  trust-schema, runtime-publication, service-manifest), placementCandidateDigest
  3fd5fb9d, protectionEpoch spec183-yolo-protected-v1.
- Real in-SIF NFD/network layer: NFD 24.07 (ndn-cxx 0.9.0, Boost 1.71),
  4/4 nfdc commands exit 0.
- No GPU execution exists anywhere; no Tiger allocation has been used; no
  PASS is claimed for anything runtime-related.

## Principle review

1. **Intent fidelity** — OK.  Fixed experiment authority (user ruling), real
   inputs only, no fabricated receipts.
2. **Necessity & Occam** — OK.  Planes/harness/authority/dev tools each solve
   a concrete, encountered problem; no redundant mechanism found.
3. **Architecture & ownership** — OK.  Core (ndn-service-framework) untouched;
   all changes live in Experiments/TigerCluster, tools, specs.  DI repairs
   were scoped to the harness boundary, not Core.
4. **Cross-document consistency** — FIXED DURING AUDIT CYCLE (workload row
   semantics, output anchoring).  Residual: oracle.contract still references
   experiment-profile.md prose until T005/T006 produce the numeric contract.
5. **Code fact verification** — OK for what is wired; the verified chain
   matches contracts field-for-field.
6. **Security & distributed correctness** — OK for the wired portion: 0600
   private keys, fail-closed staging/verification, no gate fabrication,
   replay-safe identity issue (side-effect cleanup is root-only).
7. **Task executability** — OK.  Five submit commands exist; check/prepare
   ran for real; local/run/submit are intentionally NOT_WIRED (design, not
   defect — G4).
8. **Validation design** — PARTIAL.  913 unit/focused tests; MiniNDN
   validation (T010) not yet run.
9. **Evidence integrity** — OK.  Real receipts and fixtures are strictly
   separated; failure log entries accompany every fix commit.
10. **Frozen evidence protection** — OK.  No selective rerun; all negative
    results recorded.
11. **Migration & rollback** — OK.  Old profiles/two-node.json untouched;
    gates empty until qualified.
12. **Verdict gate** — BLOCK (HIGH), reasons below.

## Discrepancy registry

| # | Severity | Owner | Discrepancy | Regression |
|---|----------|-------|-------------|------------|
| G1 | HIGH | T005/T006 | `prepare_role_reference` (runtime/yolo_graph_reference.py:22) produces ORT graph references (COMPONENT_ONLY) but no production owner publishes a certifiedGraph into the collector path; the final verdict consumes certifiedGraph that no wired producer generates (evidence/certified-graph-owner-gap.md). | A test that runs the collector with a graph reference produced by the wired owner and rejects a synthetic one. |
| G2 | HIGH | T011 | The base SIF's in-image controller.py predates `--spec180-runtime-receipt-file` (host sources: examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py:116); the full application layer cannot execute against the Spec183 harness until the T011 local SIF rebuild. | A post-rebuild container run that reaches wait_controller_publication with the real receipt file written. |
| G3 | HIGH | T010/T011 | `release.gates` is empty: no qualified hostMinindn or localSif receipt exists. `_local`/`_submit` correctly refuse to run without them. | The real T010/T011 receipts bound into the profile gates, then `submit.py local` opening. |
| G4 | MEDIUM | T004/T012 | Updated 2026-09-07: normal runners, storage, terminal reconciliation, shared receiver/query recovery, SSH transport and native dependency cutpoint are wired; negative User/collector/rank dispatch and full-candidate qualification remain incomplete. | t004-ssh-coordinator.md records login transport; t004-dependency-cutpoint.md records 3 actual in-process native transport checks. Neither is a Slurm/GPU gate. Re-audit required. |
| G5 | LOW | T012 | Remote project roots remain unqualified; scratch now uses locally checked SLURM_TMPDIR or /tmp. statvfs observations do not establish quota/reservation. | T012 must verify the same SIF, actual allocated node-local storage and writable project capacity. |

## Unblock path

T005/T006 owner wiring (G1) → T007 re-audit (PASS) → T008 host-unit build →
T009 integration → T010 MiniNDN host receipt (G3 half) → T011 local SIF
rebuild (G2, G3 half) → T012 qualification → T013-T017 Tiger GPU deployment.
