# T007 Design-Code Convergence Audit

## Accepted Deployment Change — 2026-09-08

用户确认[基础SIF + 外置app](../../../Experiments/TigerCluster/docs/runtime-app-layers.md)。
**新增控制项 N4 / HIGH / BLOCK，IMPLEMENTATION_PENDING**；此次为方案更新，
不是生产审计PASS。旧N1/N2修复仍需完成，不因分层设计自动关闭。

源码事实：`adapters/slurm-apptainer/templates/development-runtime.def.in` 的
166/202/481行仍构建DI provider、安装DI Python包并用九产物清单封装SIF；
`runtime/yolo_bundle.py`仍是小型text-only harness冻结器。两者均在
`Experiments/TigerCluster/`下；当前代码不能声称实现新外部native app契约。
CodeGraph探索后核对了这些具体脚本。外置C++诊断209981仅证明基本部署可行。

控制要求FR-005/006，任务T002.layer/T004.layer/T011.layer，验收V12/V20：
分开基础/app源码与产物闭包、版本化身份，构建器增量输出app，运输和worker
实际验证required R并挂载`/app:ro`；错ABI/遮蔽基础库/混用旧回执均拒绝。
T007.layer复审所有实际消费者后才正式验收新组合。不要为了文档更新重建SIF
或重新运行已通过的NDN小例子。下方“立即工作”为此次用户调整前的历史入口。

## Current audit — 2026-09-08

## Follow-up audit — 2026-09-09

The host-gate implementation gap N1 is closed at the code boundary and now has
real retained evidence.  The shared `spec183_yolo_host_gate.py` validator
interprets lifecycle order, request/run binding, numerical shape and match
status, role-specific execution evidence, exact negative boundaries, and owner
network/process cleanup.  The Tiger producer joins v52 normal, v51 permission,
and v51 post-Selection dependency outputs and invokes that same validator
before a receipt can be consumed.  The focused host-gate, builder, and
producer suite passes with 18 tests, and the v33 dispatch consumer accepts the
source-bound receipt.

N2 is closed for the host/MiniNDN evidence boundary.  Exact-SIF v58 is a
formal `submit.py local` collector PASS for the v22 base + v32 APP: it is a
real cross-process normal MiniNDN run with CPU ORT execution, numerical match,
dependency pairing, and clean process teardown.  Retained v51 Y-N reaches `SELECTION_COMMITTED` and
`PROVIDER_EXECUTION_STARTED`, records two native withheld tensor outputs, and
fails closed at `DEPENDENCY_DATA_MISSING`; Y-N-C remains correctly classified
as a placement failure.  The fresh v59 aggregate now runs the repaired helper
through all eight registered Y-N subcases and returns `T010_DONE/returncode=0`;
its supervisor records `processCleanup=CLEAN`.

**Source checkpoint**: `93c8630b`, `TigerClusterExperiments` (v58 collector
closure and native evidence normalization).

**Verdict**: **BLOCK (HIGH)** — host/MiniNDN receipt and the fresh Y-N
aggregate are verified, and v60 closes the empty-HOME/scratch local boundary;
the final T007 production audit and GPU/Tiger gates remain open.

**Progress**: 2/17 parent tasks complete; no current-candidate runtime qualification.

This section supersedes the September 7 current-state claims retained below.
Source wiring, component acceptance, exact-SIF CPU execution, and physical
qualification remain separate.  The v52/v51 campaigns were run only after the
base/app identities and native closure were fixed; no GPU/Tiger campaign has
run.  The N3 repair after `cc638d00` remains verified in [runtime version]
(t004-runtime-version.md).

### Direction and controlling findings

The normal and negative paths now lead toward the specified TigerCluster GPU
YOLO experiment. Keep the existing four-role DAG, one single-node qualification,
two independent normal two-node allocations, and one registered remote dependency
negative. Do not add model/GPU matrices or repeat historical suites to increase
the test count. The immediate work is the host qualification producer/consumer
closure, not another implementation of SSH,
negative rank dispatch or certified graph references.

| ID / severity | Source evidence at this checkpoint | Owner and concrete closure | Smallest useful verification |
| --- | --- | --- | --- |
| N1 / HIGH | At the previous checkpoint `validate_yolo_host_gate` accepted hash-valid but semantically empty evidence and arbitrary negative boundaries; the return scope was `YOLO_HOST_GATE_COMPONENT_ONLY`. | Closed at T002/T006/T010: the shared validator now binds lifecycle, execution, numeric, failure, request/source identity, and owner cleanup, and both the producer and v33 consumer invoke it. | Focused validator/builder/producer checks pass; the source-bound v52/v51 receipt is consumed successfully. The qualification label remains component-only by contract. |
| N2 / HIGH | At the previous checkpoint the wrapper accepted only Y-B and the registered negative matrix was not driven by one aggregate command. | Closed at T004/T005/T010: the maintained driver now records network cleanup and all eight Y-N subcases, including the post-Selection dependency cutpoint, under one bounded systemd owner. | v59 exact-SIF aggregate records `T010_DONE/returncode=0`, `processCleanup=CLEAN`, and PASS rows for Y-N-O/C/P/R/I/E/L/D; evidence is in [v59 evidence](minindn-local-v59-exact-sif-yn.md). |
| N3 / HIGH | `runtime.apptainerVersion` is accepted and copied into `resolve_provision_inputs().runtimeProfile`; neither `provision_run` nor `NodeRuntime` checks it before container launch. Legacy `runtime/worker.py` checks its own separate path. The builder checks its supplied expected version, which does not establish the allocated YOLO rank's version. | T004/T012: enforce the frozen version on local issuer and every YOLO rank before workload side effects, with bounded execution and retained observation. Actual compute-version qualification remains T011/T012; do not change the expected value merely to make a check pass. | Wrong version, exit failure and timeout must start zero issuer/Provider processes; matching version reaches the existing owner. No full SIF hash or GPU/model run is needed for these command-boundary checks. |

N1 and N2 were source-proven gaps, not claims that forged evidence was used
in a real run. Their code and current real-run closure are now recorded above;
the current profile still keeps formal entry closed until the ordered audit
gate is satisfied. The [cancellation/ownership follow-up](t010-cancellation.md)
routes SIGINT/SIGTERM through cleanup and removes host-global cleanup.
[Tracked-child reaping](t010-reaping.md) now waits after kill and retains
retryable failures. The [systemd owner](t010-host-supervisor.md) bounds the
process tree, and v59 supplies the actual MiniNDN network cleanup and
registered-case semantics. N3 concerned the YOLO path,
not the legacy worker's own version check. The finding row preserves the initial
defect; its source repair is now closed: issuer and every rank observe the exact
version before workload, public reanalysis requires their bound raw records, and
transport includes those files. See [N3 evidence](t004-runtime-version.md).
Actual compute version/SIF qualification remains T011/T012.

`Experiments/TigerCluster/lib` currently resolves into the packaging library
directory. Tiger-specific repairs must establish canonical ownership under
`Experiments/TigerCluster` while retaining old import/CLI compatibility, without
creating a second validator. Generic packaging validation keeps its owner.

### Previous findings and gate order

| Previous finding | Current disposition | Remaining obligation |
| --- | --- | --- |
| G1: certified graph producer disconnected | Source wiring repaired. `RequestReferenceBinding` wraps post-ACK canonical binding, prepares references through the DI assembler, binds actual published MODELROOT and writes per-request graph-reference.json; public collection reads it. [Request wiring](t005-request-reference-wiring.md) includes tiny CPU ONNX/ORT component evidence. | Actual native YOLO acceptance in T008–T011; source wiring alone does not close those gates. |
| G2: old base Controller lacks receipt flag | Closed for the current layered composition: the v32 APP carries the current Controller/User entrypoints and the v58 exact-SIF run reached the real publication/terminal path. | Keep the v22 base immutable; recheck the same flag on each new APP manifest. |
| G3: no hostMinindn/localSif receipts | HostMinindn is source-bound and consumed by the v33 dispatch profile; v58 supplies a formal local CPU collector PASS and v60 supplies the independent empty-HOME/scratch isolation PASS. | Run the final T007 audit, then promote only after the exact SIF is staged and verified on the target allocation. |
| G4: negative User/collector/ranks disconnected | Source wiring repaired in `0e22b210`; [negative collection](t004-negative-collection.md). Both ranks reuse provision/startup/storage/cleanup; rejection derives from retained Selection/cutpoint/fetch failure/node/GPU records, not raw operator assertions. | Full candidate qualification and actual T015 run. |
| G5: project/scratch unqualified | Retain as T012 environment obligation. [Node storage](t004-node-storage.md) and [SSH coordinator](t004-ssh-coordinator.md) have component/login scopes only. | Observe actual allocated node storage and writable project capacity. |

T007 must not demand a physical T010/T011 PASS before allowing those tasks to
run. N1/N2 implementation and runtime evidence are now closed at their declared
scope; the final audit still controls formal ordering. The unresolved
[base-SIF read instability](input-read-integrity.md) is a separate physical input
condition; no blind rehash/download/build retry is justified.

### Requirement and acceptance coverage

These are audit dispositions, not new PASS receipts. Every parent task retains
its original acceptance criteria in tasks.md.

| Requirements / success criteria | Owners and implementation boundary | Remaining acceptance |
| --- | --- | --- |
| FR-001, FR-018; SC-006; US1/US4 | T001/T004/T017: canonical entry/profile, generic Core/DI reuse | Library target above; final clean-checkout instructions/offline reproduction |
| FR-002, FR-003, FR-004; SC-001 | T002/T004: I/R/E, frozen harness/effective profile, prepared binding, builder dispatch; N3 enforced at issuer/ranks/collection | Final candidate refresh after source closure; other fields retain their original audit scope |
| FR-005, FR-006; SC-002; US2 | T008/T011: isolated host/container build owners, nine outputs/two extensions | Current source/ABI closure, exact new SIF and compute-matched version |
| FR-007, FR-009; SC-002/SC-005 | T005/T009/T010: role credentials, public recipients, signed readiness, secure NDN/Repo path | Real multi-process YOLO and registered permission/dependency failures are recorded in v58/v59; formal candidate qualification remains later. |
| FR-008; SC-003; US3 | T004/T005/T012–T014: four Providers, A backbone/merge, B heads, allocation/CUDA readers | Actual single/two-node GPU execution; no shared-file activation shortcut or model CPU fallback |
| FR-010, FR-011; SC-002/SC-003 | T005/T006: independent request references, retained numerical response, role/edge/cleanup reanalysis | Complete actual runtime records for later GPU/Tiger collection; host gate semantics are closed at the declared component-only scope. |
| FR-012, FR-017; SC-005 | T003/T004/T012: process/HOME ownership, once-only journal, unknown-job query, transport lock/storage | Real allocation/terminal/capacity observations remain for T012; v60 covers local HOME/scratch isolation. |
| FR-013; SC-004; US4 | T014/T016: two immutable normal allocations, each 1 warmup + 3 measured | Both actual allocations; second run is required reuse acceptance, not discretionary benchmarking |
| FR-014; SC-002 | T007–T015: audit → unit → integration → MiniNDN → exact-SIF → GPU stages | N1/N2 source/runtime scope is closed; final T007 ordering, physical allocation, and GPU receipts remain later |
| FR-015; SC-001/SC-005 | T002/T006/T009/T010/T015: mutation contracts and retained negative collector | Registered failures and bounded cleanup are evidenced in v59; the remote T015 case remains later |
| FR-016; SC-006 | T006/T017: immutable collection, raw references, terminal reconciliation/failure log | Host evidence is retained; final index and offline reconstruction remain T017 work |

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
| Code reality | Normal/negative/SSH joins exist; N3 now enforced at its actual owners; v59 exercises the maintained N2 aggregate owner with real native evidence. |
| Security / distributed correctness | Preserve isolation/request binding; v59 binds prepared identities and exact negative boundaries while the host label remains component-only. |
| Executability | N1/N2 runtime scope is executable and evidenced. The final T007 audit still controls opening later physical gates. |
| Validation design | Existing stage order/case counts suffice; only affected boundary regressions during repair. |
| Evidence quality | Keep historical scopes explicit; component-only host evidence remains deliberately separate from GPU/Tiger qualification. |
| Frozen evidence protection | Preserve failed runs, reanalyze matching records, never rewrite old profile paths or combine partial runs into PASS. |
| Migration / rollback | Preserve baseline/Spec175 and old import compatibility; native changes require resealing/rebuilding affected consumers. |
| Operations / verdict | BLOCK final T007/GPU/Tiger gates; N1/N2 and N3 focused acceptance recorded; base read instability remains unresolved; no blind retries. |

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

Next: complete the final T007 audit and update T002/T004/T005/T006 acceptance
against the v59/v60 evidence. N3's version owner is implemented; reuse its
focused evidence unless that boundary changes. Then qualify the matching source
through T008–T017 in order, starting with one verified Tiger staging copy and
allocation preflight after the base input condition is resolved. This report
requires no native build, SIF transfer/hash, or repeated local model run.

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
| G2 | HIGH | T011 | The v22 base intentionally contains stable libraries only; the v32 external APP carries the current Controller/User entrypoints and the v58 exact-SIF run reached the real publication/terminal path. | Recheck the same controller receipt flag against each new APP manifest; no base rebuild is required for this unchanged composition. |
| G3 | HIGH | T010/T011 | The v33 profile binds a source- and layer-verified `hostMinindn` receipt; v58 is a formal local CPU collector PASS and v60 independently verifies empty HOME/scratch isolation. | Finish T007, then bind the exact SIF/APP hashes to one verified Tiger allocation before GPU promotion. |
| G4 | MEDIUM | T004/T012 | Updated 2026-09-07: normal runners, storage, terminal reconciliation, shared receiver/query recovery, SSH transport and native dependency cutpoint are wired; negative User/collector/rank dispatch and full-candidate qualification remain incomplete. | t004-ssh-coordinator.md records login transport; t004-dependency-cutpoint.md records 3 actual in-process native transport checks. Neither is a Slurm/GPU gate. Re-audit required. |
| G5 | LOW | T012 | Remote project roots remain unqualified; scratch now uses locally checked SLURM_TMPDIR or /tmp. statvfs observations do not establish quota/reservation. | T012 must verify the same SIF, actual allocated node-local storage and writable project capacity. |

## Unblock path

T007 final audit (including G1) → T008 host-unit closure → T009 integration →
T012 allocation qualification → T013-T017 Tiger GPU deployment.
