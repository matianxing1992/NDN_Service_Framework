# T007 Design-Code Convergence Audit

## Current audit — 2026-09-08

**Source checkpoint**: `0e22b210`, `TigerClusterExperiments`.

**Verdict**: **BLOCK (HIGH)** — N1–N3 below.

**Progress**: 2/17 parent tasks complete; no current-candidate runtime qualification.

This section supersedes the September 7 current-state claims retained below.
Source wiring, component acceptance and physical qualification remain separate.
This re-audit ran no build, model, MiniNDN, SIF or GPU campaign.

### Direction and controlling findings

The normal and negative paths now lead toward the specified TigerCluster GPU
YOLO experiment. Keep the existing four-role DAG, one single-node qualification,
two independent normal two-node allocations, and one registered remote dependency
negative. Do not add model/GPU matrices or repeat historical suites to increase
the test count. The immediate work is the host qualification producer/consumer
closure and the unused version constraint, not another implementation of SSH,
negative rank dispatch or certified graph references.

| ID / severity | Source evidence at this checkpoint | Owner and concrete closure | Smallest useful verification |
| --- | --- | --- | --- |
| N1 / HIGH | `validate_yolo_host_gate` binds file sizes/hashes but does not interpret lifecycle, execution, numeric, failure or cleanup content. Negative `failureBoundary` accepts any nonempty string. The existing `test_spec183_yolo_host_gate.py::write_receipt` positive fixture contains only `{case, kind}` in each evidence file and is expected to validate. The return scope is `YOLO_HOST_GATE_COMPONENT_ONLY`; `jobs/yolo/submit.py::_gate_receipt` and `build-local-sif.sh` consume it as the host prerequisite. | T002/T006 with T010 producer: derive qualification from retained real protocol/numeric/fault/exit/cleanup records, bound to source/build/run. Keep file-integrity validation separate. Both public local entry and builder must consume the same semantic validator. | At both actual consumers reject hash-valid but empty evidence, wrong failure boundary, unrelated request/source or failed cleanup before Apptainer/model calls. Real T010 executions later supply the accepted records; fixtures cannot close the runtime gate. |
| N2 / HIGH | `tools/spec183_minindn.py` accepts only Y-B, not the required normal/permission/dependency set. `_prepared` checks a status marker and reads preparation/case files without the production verifier. Shared `.cache/t010-*` directories, fixed `.keys/offers`, package/epoch and unbounded outer `subprocess.run` remain. `T010_DONE` is not the host manifest its docstring claims. | T004/T005/T010: finish this existing wrapper against the maintained MiniNDN driver with verified run inputs, actual prepared signers, exclusive run output, bounded owned cleanup and three registered case selectors. Produce N1's receipt from those same executions. Do not substitute an unrelated ingress failure for missing dependency Data. | Focused identity/argv/output/timeout checks at the driver boundary, bad inputs rejected before launch. After T007/T008/T009, run exactly three registered cases and collect all evidence in those runs. |
| N3 / HIGH | `runtime.apptainerVersion` is accepted and copied into `resolve_provision_inputs().runtimeProfile`; neither `provision_run` nor `NodeRuntime` checks it before container launch. Legacy `runtime/worker.py` checks its own separate path. The builder checks its supplied expected version, which does not establish the allocated YOLO rank's version. | T004/T012: enforce the frozen version on local issuer and every YOLO rank before workload side effects, with bounded execution and retained observation. Actual compute-version qualification remains T011/T012; do not change the expected value merely to make a check pass. | Wrong version, exit failure and timeout must start zero issuer/Provider processes; matching version reaches the existing owner. No full SIF hash or GPU/model run is needed for these command-boundary checks. |

N1 is a source-proven validation gap, not a claim that forged evidence was used
in a real run. The current profile has no host gate, so its normal entry remains
closed. N2 is a code gap even before MiniNDN execution. N3 concerns the YOLO path,
not the legacy worker's own version check.

`Experiments/TigerCluster/lib` currently resolves into the packaging library
directory. Tiger-specific repairs must establish canonical ownership under
`Experiments/TigerCluster` while retaining old import/CLI compatibility, without
creating a second validator. Generic packaging validation keeps its owner.

### Previous findings and gate order

| Previous finding | Current disposition | Remaining obligation |
| --- | --- | --- |
| G1: certified graph producer disconnected | Source wiring repaired. `RequestReferenceBinding` wraps post-ACK canonical binding, prepares references through the DI assembler, binds actual published MODELROOT and writes per-request graph-reference.json; public collection reads it. [Request wiring](t005-request-reference-wiring.md) includes tiny CPU ONNX/ORT component evidence. | Actual native YOLO acceptance in T008–T011; source wiring alone does not close those gates. |
| G2: old base Controller lacks receipt flag | Known obsolete runtime input, not a requirement to finish T011 before T007. Current host Controller source supports the flag. | Build the new source-bound SIF and retain its real Controller publication receipt; do not retry the unsupported old-base command. |
| G3: no hostMinindn/localSif receipts | Expected pending qualification. T007 checks executable producer/consumer semantics, not future physical PASS. | Close N1/N2, then T010 generates host evidence and T011 qualifies the exact SIF. Empty gate slots remain closed. |
| G4: negative User/collector/ranks disconnected | Source wiring repaired in `0e22b210`; [negative collection](t004-negative-collection.md). Both ranks reuse provision/startup/storage/cleanup; rejection derives from retained Selection/cutpoint/fetch failure/node/GPU records, not raw operator assertions. | Full candidate qualification and actual T015 run. |
| G5: project/scratch unqualified | Retain as T012 environment obligation. [Node storage](t004-node-storage.md) and [SSH coordinator](t004-ssh-coordinator.md) have component/login scopes only. | Observe actual allocated node storage and writable project capacity. |

T007 must not demand a physical T010/T011 PASS before allowing those tasks to
run. Their actual source/validation gaps N1/N2 still block T007. The unresolved
[base-SIF read instability](input-read-integrity.md) is a separate physical input
condition; no blind rehash/download/build retry is justified.

### Requirement and acceptance coverage

These are audit dispositions, not new PASS receipts. Every parent task retains
its original acceptance criteria in tasks.md.

| Requirements / success criteria | Owners and implementation boundary | Remaining acceptance |
| --- | --- | --- |
| FR-001, FR-018; SC-006; US1/US4 | T001/T004/T017: canonical entry/profile, generic Core/DI reuse | N2 and library target above; final clean-checkout instructions/offline reproduction |
| FR-002, FR-003, FR-004; SC-001 | T002/T004: I/R/E, frozen harness/effective profile, prepared binding, builder dispatch | N1/N3; final candidate refresh after source closure; no claim all fields are consumed |
| FR-005, FR-006; SC-002; US2 | T008/T011: isolated host/container build owners, nine outputs/two extensions | Current source/ABI closure, exact new SIF and compute-matched version |
| FR-007, FR-009; SC-002/SC-005 | T005/T009/T010: role credentials, public recipients, signed readiness, secure NDN/Repo path | N2; real multi-process YOLO and registered permission/dependency failures |
| FR-008; SC-003; US3 | T004/T005/T012–T014: four Providers, A backbone/merge, B heads, allocation/CUDA readers | Actual single/two-node GPU execution; no shared-file activation shortcut or model CPU fallback |
| FR-010, FR-011; SC-002/SC-003 | T005/T006: independent request references, retained numerical response, role/edge/cleanup reanalysis | N1 at host gate; complete actual runtime records for normal collection |
| FR-012, FR-017; SC-005 | T003/T004/T012: process/HOME ownership, once-only journal, unknown-job query, transport lock/storage | N2 host wrapper; real allocation/terminal/capacity observations |
| FR-013; SC-004; US4 | T014/T016: two immutable normal allocations, each 1 warmup + 3 measured | Both actual allocations; second run is required reuse acceptance, not discretionary benchmarking |
| FR-014; SC-002 | T007–T015: audit → unit → integration → MiniNDN → exact-SIF → GPU stages | N1–N3 before T007 PASS; physical receipts remain later |
| FR-015; SC-001/SC-005 | T002/T006/T009/T010/T015: mutation contracts and retained negative collector | N1/N2; actual registered failures, independent evidence, bounded cleanup |
| FR-016; SC-006 | T006/T017: immutable collection, raw references, terminal reconciliation/failure log | N1/N2 host evidence; final index and offline reconstruction |

All 18 FRs, six SCs, four stories and 17 parent tasks are covered. Spec Kit's
structural check found 18/18 FR traceability and two completed parent tasks;
structural PASS does not override this semantic BLOCK.

### Twelve-dimension review

| Dimension | Assessment |
| --- | --- |
| Intent fidelity | Aligned with bounded GPU YOLO correctness/reuse; no additional research campaign. |
| Necessity / Occam | Repair the three actual boundaries, reuse owners and prior evidence; no new supervisor/planner/transport. |
| Architecture / ownership | Generic native cutpoint/User binding retain generic owners. Old claim that Core/DI source is untouched is superseded. Respect library compatibility during N1 repair. |
| Cross-document consistency | Current report, plan and progress table supersede obsolete disconnected-owner claims; chronological checkpoints remain historical. |
| Code reality | Normal/negative/SSH joins exist. N2/N3 remain executable gaps; source presence does not prove native compatibility. |
| Security / distributed correctness | Preserve isolation/request binding; N1 cannot equate content hashes with qualification; N2 must verify prepared identities. |
| Executability | N2 controls. Require executable later-stage code, not future physical results, at T007. |
| Validation design | Existing stage order/case counts suffice; only affected boundary regressions during repair. |
| Evidence quality | Keep historical scopes explicit; component-only host evidence is over-consumed (N1). |
| Frozen evidence protection | Preserve failed runs, reanalyze matching records, never rewrite old profile paths or combine partial runs into PASS. |
| Migration / rollback | Preserve baseline/Spec175 and old import compatibility; native changes require resealing/rebuilding affected consumers. |
| Operations / verdict | BLOCK N1–N3; base read instability remains unresolved; no blind retries. |

### Verification and next checkpoint

- Context Mode strict active health passed; checkpoint authority came from
  maintained repository state, not timeline auto-memory.
- CodeGraph explored provision/local, request-reference/collector and gate paths.
  Exact source inspection checked MiniNDN, host validator/both consumers and
  tracked version-field consumers.
- Spec Kit prerequisites and strict structural audit passed. Historical focused
  results were reused at declared scope; no overlapping totals were summed and
  no current full-suite PASS is claimed.
- GSD health is degraded: W017 old temporary source worktree, W019 noncanonical
  handoff filename. Preserve those worktrees; use tasks.md/current handoff as
  explicit fallback. These warnings are not runtime results.
- ARS is not applicable to this implementation/qualification-path audit; no
  statistical design, literature comparison or scientific claim was added.

Next: implement N1/N2 together at the existing MiniNDN producer and shared host
validator, then N3 at issuer/rank launch. Re-audit affected consumers and update
T002/T004/T005/T006 acceptance before closing T007. Run only new boundary
regressions during repair. Then qualify matching source through T008–T017 in
order, resolving the base input condition before relying on those bytes. This
report itself requires no native build, SIF transfer/hash or model rerun.

## Historical audit — 2026-09-07 (superseded by the current section)

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
