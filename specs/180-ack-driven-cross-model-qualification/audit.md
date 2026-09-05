# Spec180 Tiger-MVP Scope and Readiness Audit

## Controlling 2026-09-05 correction — T010/T011 implementation repair

The User exception handler could turn unrelated failures into Y-N-C/P/R/E
PASS; a 12-case red regression reproduced it. The real grant verifier was
not exercised by Y-N-E. Current repairs require exact errors, observed phases,
bound request/attempt/Provider/plan evidence and all child exits. See
`evidence/t011-negative-verdict-repair-20260905.md`. Previous negative PASS
labels cannot establish safety qualification. Current FR-008 production roles
still default to `plaintext-v1` and the Native protected factory is not wired:
this is an explicit implementation blocker, not an invitation to weaken the
Spec. It is not just a missing callback: the authenticated authority, canonical
cross-language grant contract and Native key lifecycle are incomplete. See
`evidence/t008-protected-grant-gap-20260905.md`. Full T014 remains **BLOCK**.

**Closure by reassignment (2026-09-05, revision 125):** Spec 180 已关闭；
剩余实现与资格认证工作全部转移到 Spec 181
（`181-ndnsf-di-protected-grant-qualification`）。本文件冻结为历史记录。

**Revision 124 closure (2026-09-05):** the grant gap is now owned by task
bodies: T007 (positive grant path) and T010 (Y-N-E real grant mutation);
T011 carries the strict negative-verdict semantics and the post-probe
fallback marker rule; T013 carries the wrapper deadline margin; T014 must
verify the invalidated-evidence banners and the audit-pointer coherence.
The revision-121 S4 exact-SIF result is historical. Executing T001--T020
in completion order now closes every known open problem.
The terminal collector already exists and
is called by the supervisor; prior statements that it is absent are superseded
by current source inspection, without claiming its qualification.

**Date**: 2026-09-04 (current-source addendum; historical audits retained below)

**Audit iteration**: 123 with provider-boundary, Tiger-path and numerical follow-ups

## Latest T007/T013 native-evidence follow-up — T014 BLOCK (2026-09-04)

`evidence/t013-native-evidence-repair-20260904.md` records 99 focused checks
and the limits of host syntax checks. Existing ExecutionEvidence now carries
request/cache/process observations; V3 ONNX assembly enables first-request
profiles, and CUDA UUID comes from selected-device PCI/driver queries rather
than environment metadata. The observer emits original per-role records;
aggregate readiness is not per-request proof. See
`contracts/native-execution-evidence-v1.md`. Compiled helper tests use synthetic
CUDA libraries, not a real GPU or Provider network run. TP-02 remains partial:
the candidate-bound terminal collector and full cleanup are still absent.
TP-04 live Y-N and TP-05 full candidate binding remain open. No task is newly
complete and no formal convergence PASS is claimed.

## Prior T010/T013 numerical follow-up — T014 BLOCK (2026-09-04)

`evidence/t013-numerical-repair-20260904.md` records 83 focused passing
checks. CodeGraph plus exact production-branch tests confirm fixed registered
input publication and received-response comparison before the User success
marker. `contracts/yolo-numerical-evidence-v1.md` defines the component record.
The prior random-input/oracle mismatch and missing Tiger native tensor flag
are repaired. The existing package preflight passes, but preprocessing is
not byte-identical to offline Torch (maximum absolute difference 1.19e-7).
No model, network, GPU or container qualification was executed. TP-02 remains
partial: actual native CUDA/PID/physical-device evidence and its candidate-bound
terminal collector, transient-helper closure and secret disposal remain open.
TP-04 live Y-N and TP-05 candidate binding remain open; earlier source-only
snapshots cannot qualify this changed subject. No new task completion or full
convergence PASS is claimed.

## Prior T013 supervision follow-up — T014 BLOCK (2026-09-04)

`evidence/t013-supervision-repair-20260904.md` records 44 focused passing
checks. The production Tiger launcher now has ordered readiness, peer-exit
monitoring, bounded runtime-process-group cleanup, and terminal validation
against observed children/PIDs. The probed and executed scripts use the same
sealed replay tree. This closes the source-level TP-01/TP-03 mismatches, not
full in-image qualification. TP-02 remains partial: real numerical/CUDA oracle
production, transient-helper closure and secret/scratch cleanup remain open.
TP-04 live Y-N and TP-05 full candidate binding remain open. A new 408-file
source-only checkpoint invalidates reuse of the previous 406-file snapshot
for the changed harness; neither is a promoted candidate.

## Current T014 Tiger-path follow-up — BLOCK (2026-09-04)

`evidence/t014-tiger-path-audit-20260904.md` is the latest scoped audit.
It supersedes any inference that local native Provider wiring proves Tiger
launch wiring. The renderer/validator repairs have 118 focused passing checks,
but T013 still lacks barriered/bounded Tiger supervision and a production
structured numerical/runtime/cleanup result chain. The bundle launcher differs
from the in-image launcher probed by preflight. T011 still substitutes focused
checks for required live Y-N negatives. Full candidate identity binding is open.

The source-only checkpoint records 406 exact current files, including dirty
bytes; it is neither a built runtime nor a promoted candidate. T014 is not PASS,
and no T015/SIF/Tiger qualification is authorized by this follow-up. Preserve
all earlier frozen evidence and return remaining work to T011/T013.

**Documentation revision**: 123 with dated implementation addendum

**Mode**: pre-Tiger implementation checkpoint; not the mandatory T014
design-code convergence audit

**Verdict**: **BLOCK FOR TIGER — FINISH THE SINGLE YOLO VERTICAL SLICE; DO NOT
EXPAND TO QWEN OR THREE-GPU QUALIFICATION**

## Provider-boundary follow-up (2026-09-04, current)

`evidence/provider-boundary-repair-20260904.md` records the current repair and
115 focused checks. The Python V3 downgrade and preparation-attempt findings
are closed at the handler boundary. Native-only runner ownership and existing
duplicate-identity/phase guards are verified; facade documentation now explains
the public/network layers. This is a scoped repair review, not full T014 PASS.

**Correction to H1**: absence of a commit alone is not an FR-015 violation.
`contracts/immutable-candidate-v1.md` explicitly admits recorded modified and
untracked source bytes, and `scripts/spec180_candidate.py` validates an explicit
dirty source delta. The earlier report's unconditional commit prerequisite is
withdrawn. A fresh complete source/runtime/harness binding and full T014 audit
are still required; the hashes in the repair report are a scoped checkpoint,
not a replacement candidate seal. Old qualification evidence remains stale.

## Revision 123 current-source checkpoint (2026-09-04)

This is the current checkpoint for the revision-123 implementation repair. The
report-only design/code audit is preserved at
`evidence/audit-revision123-design-code-conformance-20260904.md`; this section
records its controlled follow-up without overwriting historical audit text.

**Current gate verdict**: **CONDITIONAL PASS FOR IMPLEMENTATION; BLOCKED FOR
FORMAL VALIDATION while H1 source binding remains open.**

| Finding | Current disposition | Evidence / next gate |
|---|---|---|
| A180-123 `NOT_WIRED` | **CLOSED for the current source** | The maintained Controller/Repo/Provider/User path is now wired; a fresh real-NFD Y-A reached terminal Response in `evidence/t011-y-a-live-current-20260904.md`. |
| H1 source binding | **CURRENT BINDING REQUIRED; COMMIT-ONLY CLAIM CORRECTED** | Bind all current source bytes using the immutable-candidate contract, including an explicit dirty/untracked delta when applicable. A commit alone is neither necessary nor sufficient to bind runtime/harness/configuration. |
| M1 audit pointer | **CLOSED** | This section and `traceability.md` identify audit iteration 123/documentation revision 123; T014 is still the required fresh convergence audit. |
| M2 invalidated PASS artifacts | **CLOSED** | The revision-121 exact-SIF and S1 artifacts now carry explicit invalidation banners; their historical bytes are not reused. |
| M3/M4/L3 documentation/marker drift | **CLOSED** | Traceability names one cold Y-B request; the no-publication fallback uses `SPEC180_CONTROLLER_READY`; the status tail records T019 complete while T014--T018/T020 remain open. |
| L1/L2 readiness boundary | **CLOSED for implementation** | Python retains a 15 s outer wait over the 10 s Core deadline; Core exits the probe when the Face is stopped instead of spinning to the deadline. |

This checkpoint is implementation and focused local evidence only. It is not
the mandatory T014 convergence PASS, T015 qualification, SIF evidence, or
Tiger evidence. The next allowed gate is T014; SIF/Tiger work remains paused.

## Revision 123 native-boundary follow-up (2026-09-04)

The native Provider path now binds request-backed application input only to
declared V3 `APPLICATION_INPUT` ingress edges. Non-ingress roles reject an
injected Y-N-I fetch at the Provider execution boundary, and the real native
Y-N-C command vector removes `FullModel` and `Merge` before child startup.
The shared Y-N-C/P/R/I/E/L driver records a negative marker only after its
case-owned children have exited. The focused checkpoint is
`evidence/t011-native-boundary-repair-20260904.md` (180 relevant checks plus
the 396/396 `-j2` native build). This closes a source-level wiring gap only: live
NFD/NDN-SVS negative execution, protected grant verification, candidate
binding, and T014 convergence remain open.

## Revision 112 audit decision

Spec180 repeatedly failed to reach Tiger workload entry because its completion
boundary combined too many independent goals before proving one deployment:
25 functional requirements, 20 tasks, a new YOLO adapter and split, local
Y-A/Y-B/Y-N, Qwen references and a Qwen3.6-27B runner, a complete local suite,
one SIF, two Tiger jobs, three distinct GPUs, two requests per job, and final
cross-model closure. Focused implementation and audit work could advance across
many partial tasks while no single Controller/Repository/Provider/User request
reached a terminal Response.

The immediate technical blocker remains earlier than Tiger:

1. `G0-NATIVE` found two different `libndn-cxx.so.0.9.0` files in the same
   process system: the Python extension/NAC-ABE closure used `.local-boost171`,
   while NFD/current NDN-SVS used `/usr/local`. Controller/Repository therefore
   exited with socket EOF before a request.
2. `G0-CANDIDATE` has no single sealed, current-role signed YOLO package bound
   to the Provider offer keys and canonical ONNX model.
3. The current `-j2` unified build is implementation repair, not Tiger or
   protocol evidence. The earlier `-j8` tree is invalid and must never be used.

After the corrected build completed, read-only linkage showed partial progress:
NFD and `build-system-j2/libndn-service-framework.so` now both resolve the
`/usr/local` `libndn-cxx`, while the active Python 3.8 `_ndnsf` extension still
loads the old `build/libndn-service-framework.so.0.1.0` and the
`.local-boost171` `libndn-cxx`. S0 is therefore still open, with a concrete
owner: rebuild the Python extension and its NAC-ABE/native closure against the
accepted build, then rerun the hash/import/startup check.

The corrected Spec180 target is one one-node/one-GPU/four-Provider YOLO Y-B
request. The three CUDA model roles may share the allocated physical GPU;
`Merge` remains an explicit CPU role. This proves functional deployment only.
Qwen, a second warm request, distinct-GPU placement, and performance claims
are transferred out of the Spec180 completion boundary.

| Finding | Severity | Correction |
|---|---:|---|
| A180-124: first Tiger result depended on two models and two remote jobs | CRITICAL | Spec180 closes on one YOLO Y-B job only |
| A180-125: three distinct GPUs and two requests were required before any remote functional proof | HIGH | First profile uses one GPU and one cold request; later work may scale it |
| A180-126: Qwen production artifacts and runner blocked the unrelated YOLO path | HIGH | Preserve the handoff but remove Qwen from Spec180 acceptance |
| A180-127: broad local inventory preceded the first vertical result | HIGH | Run only YOLO-relevant selectors plus Y-A/Y-B/critical Y-N |
| A180-128: readiness work was repeated while the native closure and candidate were invalid | CRITICAL | Permit SIF/Tiger only after S0--S3 pass in order |
| A180-129: checked-in `profile.json` and Slurm scripts still encode the superseded three-GPU/two-request/Qwen route | HIGH | T013 must render and focused-test the revision-112 one-GPU/one-request YOLO profile before T014; the current files are not submission-ready |
| A180-130: focused tests cover runner seams but no current test executes `run_minindn_case()` through a real terminal Response | CRITICAL | T011 must close Y-A and Y-B through the production driver before any SIF/Tiger gate opens |

The next action is not another audit or Tiger submission. Complete the one
`-j2` native closure, seal the experiment candidate, and obtain Y-A then Y-B
terminal Responses locally. After that, perform T014 once and follow the
single SIF/Tiger route without changing configuration.

**Historical iteration 96 input-integrity note (2026-09-03)**: The T011-A BLOCK remains
authoritative, but its cause is no longer an unwired function stub. The
barriered driver and controller-owned catalogue publication path now exist and
their focused tests pass. A separate candidate check found that the temporary
`/tmp/spec180-yolo-*` set is not a sealed release input: most packages use the
obsolete shared-role labels `DetectHead0/1`, while the one observed package
with the accepted `DetectShard0/1` names has no trusted catalogue signature.
No package with either defect may be used for MiniNDN, SIF, or Tiger evidence.
The owning repair is fresh signed export plus full package/adapter digest
validation, followed by binding the Provider's offer-signing key and canonical
local model path; it is not an in-place manifest edit.

The sections below preserve earlier diagnosis. Revision 112 is authoritative
where their broader cross-model scope conflicts with the finite target above.

## Revision 110: why the real experiment is still not complete

The delay is now separated into two pre-network gates and one execution gate:

| Gate | Evidence currently available | Why it cannot close the experiment |
|---|---|---|
| `G0-NATIVE` | Linkage inspection and a fail-closed runner check | The extension/NAC-ABE use `.local-boost171`, while NFD/current NDN-SVS use `/usr/local`; hashes and Build IDs differ, so Controller/Repository stop with socket EOF before a request |
| `G0-CANDIDATE` | Focused exporter/adapter and trust-root checks | No owner-supplied role-correct signed package, Provider offer-key map, and canonical Y-A model are sealed together |
| `G1-Y-A` | Barriered process vector and publication seam | No live Controller→Repository→Provider→User exchange has reached terminal Response |

The previous task graph let these gates appear as many independent partial
tasks. That produced useful rejection and wiring evidence but no executed
result. The corrected order is `G0-NATIVE → G0-CANDIDATE → G1-Y-A → G2 →
T013 → T014 → T015--T020`; only the first open gate is active. A
`WAITING_EXTERNAL_INPUT` result is a readiness classification, not a failed
NDNSF-DI request. No SIF, Tiger, or broad matrix run can repair an earlier
gate.

## Revision 108: why the real experiment is still not complete

This is a pre-execution readiness block, not evidence that NDNSF-DI's
ACK-driven protocol failed. The source has reached `implemented` and `wired`,
but not `executed`, for four independently required reasons:

| Boundary | Current fact | Correct next action |
|---|---|---|
| Candidate identity | Temporary packages are stale-role or unsigned | Export a fresh `DetectShard0/1` package and obtain the registered catalogue signature; never rename fields in place |
| Trust/input closure | No one manifest binds the Provider offer-key files, their digests, and the canonical Y-A ONNX object | Owner supplies the matching private key and absolute model/key paths; seal and hash one candidate manifest |
| First protocol result | No current Y-A request has reached terminal Response | Run exactly one Controller/Repository/Provider/User exchange through ACK, Selection, execution, and cleanup |
| Downstream qualification | T014--T020 depend on the first live result | Do not build SIF, submit Tiger, or expand the matrix until G0/G1/G2/T013 close |

The fail-closed `WAITING_EXTERNAL_INPUT` result (exit 78) is therefore the
correct status while G0 inputs are absent. It must not be counted as a failed
request, and it must not trigger another broad audit or downstream rebuild.
Once G0 is closed, any protocol/cleanup error after MiniNDN starts is recorded
as `UNQUALIFIED` and repaired at the owning task before the next gate.

## Revision 109: host native-library closure is a separate pre-start gate

The first developer Y-A launch that used the current barriered runner did not
reach ACK or Selection. Controller and Repository failed at native client
construction with `socket read error (End of file)`. Linkage inspection found
that the Python extension and NAC-ABE used
`/home/tianxing/NDN/ndn-service-framework/.local-boost171/lib/libndn-cxx.so.0.9.0`
while NFD and the current NDN-SVS library used
`/usr/local/lib/libndn-cxx.so.0.9.0`; the files have different SHA-256 digests
and ELF Build IDs. Forcing one directory with `LD_LIBRARY_PATH` is unsafe: it
can load an older installed framework or leave NDN-SVS and NFD on different
ABIs. The new `_validate_native_library_closure()` check rejects this state
before MiniNDN/NFD startup and the entrypoint reports
`WAITING_EXTERNAL_INPUT` with exit 78.

This diagnostic explains one repeated delay but is not an NDNSF-DI failure or
success result. The owner must rebuild NFD, NDN-SVS, NDNSF, NAC-ABE, and the
Python extension from one explicit ABI/toolchain closure, record the resolved
paths and digests, and then rerun exactly one Y-A request. The overall verdict
remains **BLOCK** at `G0-NATIVE`/`G0-CANDIDATE` before T011-A; no
SIF/Tiger action is authorized.

## Revision 105 scope and ordering correction (2026-09-03)

The delay has two separate causes. First, it is an external-input block: no
role-correct, registered-signature YOLO package is bound together with the
Provider offer keys and canonical model file. Second, it was a planning-order
problem: the original open tasks mixed the minimum atomic Y-A path with the
shared-role and negative matrix, so implementation/audit work could grow while
the first request remained absent. The corrected execution unit is one atomic
`FullModel` Y-A request. Only after its terminal Response may the same driver
expand to Y-B/Y-N. QWEN-F/release work must be sealed before T014/SIF, but
cannot replace Y-A evidence.

This correction changes task ownership and sequencing only; it does not promote
any focused test, offline package, or historical Spec175 result. The gate
remains `BLOCK` until G0 and G1 are closed.

## Revision 106 QWEN-F entrypoint correction (2026-09-03)

The separate `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py --case QWEN-F`
entrypoint now exists and is covered by focused manifest, digest, identity,
tokenizer/prompt, and fixed-dispatch tests. It validates the registered
Qwen3.6-27B ONNX contract, CUDA execution-provider declaration, and
`cpuFallback=false` before delegating to the maintained request-first runner.
The complete `tests/python/test_spec180_*.py` collection passes with
**183 passed, 22 existing warnings**; this remains implementation evidence.
This removes “entrypoint absent” as an implementation blocker, but does not
create a signed production manifest, staged 27B graph/initializer objects, or
live ACK/Selection/Response evidence. T013 is therefore still partial and the
overall audit remains **BLOCK** at G0/T011-A.

## Revision 107 input-status correction (2026-09-03)

The YOLO entrypoint now separates the pre-network input phase from protocol
execution. With G0 inputs absent, a direct run emits
`SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT` and exits 78 before NFD,
SVS, or any Provider starts. Once `run_minindn_case()` begins, a protocol or
cleanup failure remains `UNQUALIFIED`. The focused regression and a direct
missing-input probe pass this distinction. It improves evidence classification
only; the complete Spec180 Python collection now passes with **184 passed, 22
existing warnings**. G0 remains open and no real NDNSF-DI result is claimed.

## Revision 104 release-boundary correction (historical; superseded by revision 106, 2026-09-03)

The host release renderer now verifies the external QWEN-F model manifest
before any scheduler adapter can be called. It requires the registered
`spec180-model-manifest-v1` identity, Qwen3.6-27B model identity, three stage
records, CUDA ONNX Runtime, and `cpuFallback=false`, then delegates signature
verification to the shared trust-root gate. The focused release/dispatcher
collection and the complete `tests/python/test_spec180_*.py` collection pass
(**178 passed, 22 existing warnings**). The earlier **194** count included
that collection plus separate automatic-planning lifecycle/process-digest
regressions; it is not a second qualification result. This closes a
T013 release-validation seam only; at that time the QWEN-F entrypoint, a
signed external manifest with real model objects, T011 live Response, T014
convergence, SIF, and Tiger execution remained open. Revision 106 adds the
focused-tested entrypoint; the signed manifest/object set and live execution
remain open. Because this changes a release behavior,
the next formal gate must be a fresh T014 audit rather than a qualification
run on an earlier candidate.

## Iteration 103 current blocking record (2026-09-03)

The repeated delay is explained by a deliberate fail-closed boundary plus an
overlong pre-execution path. Source and lifecycle seams are now
implemented/wired, but no valid candidate reached the first live request. The
previous audit history is provenance, not a queue. The controlling actions are
therefore limited to G0 candidate closure, G1 Y-A, G2 Y-B/Y-N, T013 QWEN-F and
release completion, and one T014 convergence audit.

Spec175 is not reopened: its frozen CPU/MiniNDN result remains the baseline,
while the current dirty tree is a new Spec180 source subject. Combining the
old source seal with later lifecycle, candidate, or release changes would make
a subsequent PASS untraceable.

### Current blocking record

The maintained YOLO User now instantiates the fail-closed `LifecycleJournal`,
binds the explicit request/attempt identity before input publication, and
receives the coordinator's seven transition callbacks. It appends the input,
Provider-execution, and terminal-response milestones on the same production
path and validates the ten-event trace. This is wired source evidence only;
no live Y-A process has yet produced the trace and terminal Response. T011
remains partial until one real request crosses ACK closure, Selection,
Provider execution, and terminal Response.

No task is promoted by this wiring correction. The candidate, catalogue
signature, Provider key/model inputs, and live MiniNDN exchange remain the
controlling blockers.

### Why Spec180 has not produced a real result

The current state is a readiness stop, not a slow or failed NDNSF-DI run. The
runner has reached the `implemented`/`wired` levels, but no candidate has
reached `executed`. Earlier work allowed boundary tests, audit refinements,
and release preparation to proceed before one atomic Y-A request was runnable;
the QWEN-F executable was also deferred beyond the SIF-sealing boundary. The
remaining blockers are:

| Gate | Evidence | Required action |
|---|---|---|
| G0 candidate closure | temporary packages are obsolete-role or unsigned; no one manifest has valid key/model files | fresh export with `DetectShard0/1`, registered signature, Provider key map, canonical ONNX path, and all digests |
| G1 live Y-A | lifecycle and launch seams are wired, but no terminal Response exists | run one real Controller/Repository/Provider/User request through ACK, Selection, execution, and cleanup |
| G2 cross-model closure | QWEN-F entrypoint is focused-tested, but its signed production manifest/object set and result writer are absent; release tooling is partial | finish T013 before T014 and before any SIF is sealed |

Until G0--G2 close, focused tests remain implementation evidence only and
T014--T020 must stay open. This re-audit changes no protocol semantics and
does not justify another broad regression or remote submission.

### Revision 103 focused verification

- The complete Spec180 Python collection plus the automatic-planning lifecycle
  and process-input digest regressions passes: **194 passed, 22 existing warnings**.
- The maintained coordinator, client, runner, and YOLO User pass
  `py_compile`; `git diff --check` is clean.
- The Spec180 structure audit and `scripts/spec180_contract_gate.py` pass with
  `contractReady=true` and `qualificationReady=false`.
- No live Y-A trace, terminal Response, local qualification, SIF replay, or
  Tiger result is claimed by these checks.

## Iteration 100 current blocking record (historical; superseded by iteration 102)

The source contained a fail-closed `LifecycleJournal` writer and protocol-
identity binding guard, but the maintained User had not yet instantiated it.
That documentation state is superseded by revision 102; the absence of a live
trace remains unresolved.

## Iteration 99 current blocking record (historical; superseded by iteration 102)

The delay is an input-closure and ordering problem, not a measured NDNSF-DI
protocol failure. Focused tests prove fail-closed boundaries, but no current
candidate has crossed the live ACK-to-terminal-Response boundary.

| ID | Severity | Dimension | Finding | Required correction |
|---|---|---|---|---|
| A180-143 | HIGH (open) | Candidate identity | Temporary packages are role-incompatible or unsigned; none is a trusted current candidate | Re-export with `DetectShard0/1`, obtain the registered catalogue signature, and seal package/graph/initializer/catalogue digests |
| A180-144 | HIGH (open) | Runtime input closure | The live case still lacks a complete explicit policy/identity map, Provider offer-key map, and canonical Y-A model path | Bind all inputs in one manifest and fail before NFD/SVS if any is absent |
| A180-145 | MEDIUM (resolved in plan) | Work ordering | Repeated audit/preflight refinements ran before one runnable Y-A slice | Enforce the single queue Y-A → Y-B → Y-N → freeze → T013 → T014; no SIF/Tiger work before it |

The next checkpoint is therefore not another broad test. It is one real Y-A
MiniNDN request using the sealed candidate and the maintained Controller,
Repository, Provider, and User processes. A missing external key or model is
reported as `WAITING_EXTERNAL_INPUT`, not converted into synthetic evidence.

## Iteration 97 controller-entry correction (2026-09-03)

A code-aware recheck found that the runner's publication argument was not
enough to reach the Controller publication function: `controller.py` previously
returned through ordinary `controller.run()` unless a repository deployment
manifest was also supplied. The Spec180 runner intentionally supplies no such
manifest, so the signed runtime catalogue and artifact records were never
published and the receipt barrier could not complete. The Controller now
selects its publication mode from `--spec180-runtime-publication-file` alone;
the unrelated repository deployment path still requires its own manifest.
Focused application and runner tests cover this branch. The correction removes
one production wiring blocker but does not alter the BLOCK verdict: no current
candidate has yet reached live ACK closure, Selection, Provider execution,
Merge, and terminal Response.

## Iteration 98 publication-envelope correction (2026-09-03)

The Controller now validates the runner-generated publication envelope before
any signing or APP publication. It requires a registered case, a catalogue
name below the declared signer namespace, a valid package-manifest digest, an
exact catalogue payload digest, unique artifact names below the controller's
artifact namespace, bounded metadata payloads, and exact per-artifact digests.
The focused tamper regression passes. This closes an input-integrity seam but
does not alter the qualification BLOCK: no current candidate has reached a
live ACK-to-terminal-Response exchange.

## Iteration 94 documentation correction (2026-09-03)

Spec175 and Spec180 now state the evidence boundary in the same vocabulary:
`implemented`/`wired` checks are not `executed` protocol results, and
`executed` is not `qualified` evidence. Spec175 remains a sealed,
read-only CPU/MiniNDN baseline; Spec180 remains blocked at T011-A because no
current candidate has reached a live terminal Response. The correction records
the new barriered driver and its fail-closed candidate gate; it does not
promote focused seam results or authorize a SIF build, upload, or Tiger
submission. The binding next action remains one real Y-A vertical slice,
followed by the existing T011-B/T011-C/T011-D order.

The current Spec175 contract-gate `DIRTY_INPUT_TREE` result (295 in-scope
paths) is also recorded
as expected candidate-integrity behavior. It does not reopen the frozen
Spec175 subject and cannot be used as a reason to rerun its historical cases.

## Iteration 96 execution-readiness reconciliation (2026-09-03)

The current code and task graph explain why Spec180 has not produced a real
Tiger result. `run_minindn_case()` now performs validated setup, writes a
candidate-bound runtime publication, and launches the maintained phases, but
no admissible candidate has passed the input gate and reached a real Y-A/Y-B/Y-N
ACK-to-Response path. The task graph also deferred executable
work until after the gate that was supposed to audit it: T016 could modify SIF
tooling after T014, and the absent QWEN-F executable was owned by T019 after
the SIF had already been sealed. Repeated focused audits improved fail-closed
boundaries but could not close either missing production path.

### Current findings

| ID | Severity | Dimension | Location | Finding | Correction |
|---|---|---|---|---|---|
| A180-139 | HIGH (open) | Candidate-bound production execution | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:run_minindn_case`; T011 | The barriered entrypoint and Controller publication branch are now wired, and the process vector carries the Provider key map plus Y-A model path. However, no fresh signed package with the current `DetectShard0/1` roles and no sealed manifest containing valid key/model files is available. Consequently no live ACK closure, Selection, Provider execution, Merge, or terminal Response has been evidenced. | Regenerate and validate the signed package; supply the external signing-key/model files in one candidate manifest; execute T011 as atomic Y-A, shared Y-B, fixed Y-N, then freeze the driver. |
| A180-140 | HIGH (resolved in plan/tasks) | Dependency graph / candidate immutability | T013--T019 | SIF tooling and the QWEN-F executable could be implemented after T014 or after SIF sealing, making a valid immutable candidate impossible. | T013 now owns all release/SIF/preflight implementation and the QWEN-F executable before T014. T015--T019 are execution-only; any change returns to T013/T014 and creates a new candidate. |
| A180-141 | MEDIUM (resolved in plan/tasks) | Task executability | T011 and iteration history | T011 combined all positive/negative cases and evidence machinery in one large acceptance unit, while 91 audit iterations became a de facto work queue. This obscured the first runnable vertical slice. | T011 now has mandatory Y-A, Y-B, Y-N, and freeze slices. Historical iteration notes are provenance only; they cannot trigger broad retesting or expensive work. |
| A180-142 | HIGH (open) | Cross-model runtime | `scripts/run_spec180_case.py`; planned `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py`; T013 | QWEN-F correctly fails closed, but its external-manifest-bound Qwen3.6-27B ONNX entrypoint does not exist, so a SIF built now cannot later qualify Qwen without mutation. | Implement and focused-test the production QWEN-F entrypoint under T013 before T014 and SIF construction; T019 only executes it. |

Spec175 is confirmed closed as a frozen `LOCAL_FUNCTIONAL_PASS` baseline; it
has no active repair, SIF, or Tiger task. The next allowed action is T011-A,
not another complete regression, SIF build, upload, or cluster submission.
The BLOCK verdict remains until A180-139 and A180-142 are closed in production
code and a fresh T014 audit returns PASS.

The concise recovery record is
`evidence/revision103-delay-diagnosis-20260903.md`. It is the operational handoff
for the next implementation turn; the historical iteration notes below are not
a work queue.

All iteration sections below this line are historical evidence. Their earlier
verdict text does not override iteration 103 and must not be interpreted as the
current execution queue.

## Iteration 91 current-source reconciliation (2026-09-03)

This checkpoint rechecked the runtime catalogue boundary after the one-shot
cleanup correction. The adapter now binds snapshots to the case's registered
candidate digests, requires exactly one snapshot per candidate, and verifies
that each snapshot contains the complete expected role set with non-empty,
unique absolute artifact Data names. Exact APP readback also requires a
non-empty signer certificate rooted at the configured controller identity;
matching payload bytes without a certificate are rejected.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-138 | HIGH (resolved at adapter boundary) | Runtime catalogue integrity / signer provenance | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:build_runtime_catalogue_payload,publish_and_verify_runtime_catalogue`; `tests/python/test_spec180_yolo_minindn.py` | A snapshot could previously be accepted without proving exact candidate-to-role/artifact coverage, and a readback result could omit the signer certificate while still matching payload bytes. Either gap could let an incomplete or unverifiable runtime catalogue reach the User boundary. | Keep the exact coverage and signer-certificate checks fail-closed. T011 must still publish the candidate-bound objects and invoke the real controller `ServiceUser` path; T014 must audit the production call path and live ACK-to-Response evidence. |

The correction is adapter evidence, not a live protocol result. The production
publisher, ACK-to-Selection-to-Provider-to-Response driver, and T014
convergence audit remain open. The focused runner slice now has 47 passing
tests; the four bounded groups report 173 passing tests with 20 existing
exporter/runtime warnings. The verdict remains
**CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

## Iteration 90 current-source reconciliation (2026-09-03)

This checkpoint rechecked idempotence of the new cleanup boundary. Repeated
`stop()` calls now return before invoking any global MiniNDN cleanup, and a
runtime that has completed cleanup rejects a later `start_network()` call.
This prevents a duplicate error path from touching another active case; the
completion flag is set only after the maintained helper module is available,
so an import failure remains retryable instead of hiding a leak.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-137 | MEDIUM (resolved at adapter boundary) | Cleanup isolation / lifecycle | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:MiniNdnCaseRuntime.start_network/stop`; `tests/python/test_spec180_yolo_minindn.py` | Repeated teardown could have invoked global MiniNDN cleanup again after another case started, and a stopped adapter did not explicitly reject restart. | Record cleanup completion before teardown, make repeated `stop()` a no-op, and reject restart; retain the focused regression. |

The correction is still adapter evidence, not a live protocol result. The
production driver, candidate-bound artifact publication, ACK-to-Response path,
and T014 convergence audit remain open. The verdict remains
**CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

The refreshed focused evidence is 45 runner tests and 171 Spec180 Python tests
across four bounded groups, with 20 existing exporter/runtime warnings.

## Iteration 89 current-source reconciliation (2026-09-03)

This checkpoint rechecked the lifecycle boundary after reviewing the staged
startup adapter. The adapter now retains only the process handles created by
its own phase launches and exposes an idempotent teardown. The teardown stops
those children, stops the case network, invokes the maintained MiniNDN cleanup,
and clears phase/publication state even after a failed publication or request.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-136 | HIGH (resolved at adapter boundary) | Cleanup / failure atomicity | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:MiniNdnCaseRuntime.stop`; `tests/python/test_spec180_yolo_minindn.py` | The staged runtime had no instance-owned teardown, so a future live driver could leave its children or MiniNDN network running after a readiness, publication, ACK, or Response failure. | T011 must retain the runtime and call `stop()` from `finally` on every case path; cleanup failure remains `UNQUALIFIED` with preserved evidence. A focused one-child/idempotence regression is required (and now passes). |

The cleanup correction is still adapter evidence, not a live protocol result. The
production driver, candidate-bound artifact publication, ACK-to-Response path,
and T014 convergence audit remain open. The verdict therefore remains
**CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

## Iteration 88 current-source reconciliation (2026-09-03)

This checkpoint rechecked the catalogue publication boundary after the
runtime snapshot status correction. The adapter now has one explicit
publish-and-readback operation: the runner composes the candidate-bound
payload, delegates signing/transport to the controller `ServiceUser`, fetches
the exact Data name with the expected signer, compares payload bytes, and only
then records the publication receipt. A publisher call without readback is not
accepted as evidence.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-135 | HIGH (partially resolved) | Catalogue publication / runtime wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:MiniNdnCaseRuntime.publish_and_verify_runtime_catalogue`; `tests/python/test_spec180_yolo_minindn.py` | The runner previously had only a receipt recorder, so the required exact signed APP publication/readback had no executable owner. The adapter now implements and tests the publish→readback→receipt seam, but `run_minindn_case()` still does not instantiate a controller `ServiceUser`, publish candidate-bound role/rank objects, or invoke the seam. | Wire the production driver to this seam after Controller/Repo/Provider readiness and before User; publish/verify every candidate-bound object and the active snapshot. Keep T011/T014 open until a real ACK-to-Response trace passes. |

The focused runner slice now has 44 passing tests and the complete Spec180
collection reports 170 passing tests with 22 existing warnings. The overall
gate remains
`CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED`.

## Iteration 87 current-source reconciliation (2026-09-03)

This checkpoint rechecked runtime snapshot status handling after the duplicate
candidate correction. `PreSplitCatalogSnapshot` permits `ACTIVE`, `RETIRED`,
and `REVOKED` for catalog history, but the runtime APP envelope is explicitly
an active publication used for new placement. The resolver now rejects a
non-active record before it can reach the coordinator.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-134 | MEDIUM (resolved) | Runtime catalogue lifecycle | `ndnsf_distributed_inference/app_sdk/placement.py:NetworkCatalogSnapshotResolver`; `tests/python/test_spec180_catalog_resolver.py` | The resolver accepted `RETIRED` or `REVOKED` snapshots even though the runtime envelope is defined as an ACTIVE publication for new placement. | Reject non-ACTIVE snapshots at the signed APP parse boundary and retain the regression. |

The complete Spec180 Python collection remains 169 passing with 22 existing
warnings. This correction does not publish candidate artifacts, wire the live
ACK/Selection/Provider/Response driver, or advance T011/T014. The overall gate
therefore remains `CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
BLOCKED`.

## Iteration 86 current-source reconciliation (2026-09-03)

This checkpoint rechecked the signed runtime snapshot parser after the
iteration-85 publication-boundary repair. The contract permits at most one
ACTIVE snapshot for each registered candidate. The resolver already rejected
duplicate aliases and manifests, but would accept two distinct records for the
same candidate and defer the ambiguity to placement. It now rejects duplicate
candidate digests at the signed APP parse boundary; a focused regression uses a
distinct alias and manifest with the same candidate digest.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-133 | MEDIUM (resolved) | Runtime catalogue integrity | `ndnsf_distributed_inference/app_sdk/placement.py:NetworkCatalogSnapshotResolver`; `tests/python/test_spec180_catalog_resolver.py` | The resolver did not enforce the contract's one ACTIVE snapshot per candidate rule when aliases and manifests differed, leaving an avoidable ambiguity for later placement. | Reject duplicate candidate digests together with duplicate aliases/manifests and retain the regression. |

The complete Spec180 Python collection remains 169 passing with 22 existing
warnings. This correction does not publish candidate artifacts, wire the live
ACK/Selection/Provider/Response driver, or advance T011/T014. The overall gate
therefore remains `CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
BLOCKED`.

## Iteration 85 current-source reconciliation (2026-09-03)

This checkpoint re-audited the maintained V3 placement path after comparing
the signed active-catalog contract with the actual coordinator call graph. The
audit found and repaired a production-shaped omission: V3 previously skipped
`_prepare_artifacts` when no canonical ensurer was installed, then synthesized
role artifact names even though dynamic provisioning was disabled. The repair
uses the existing catalog publisher/resolver authority whenever the caller
explicitly supplies a catalog snapshot provider and resolves every role/rank
before sealing Selection. The new focused regression exercises a matching
ACTIVE snapshot and asserts exact artifact resolution; it does not claim live
NDN execution.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-131 | HIGH (resolved) | Artifact authority / production wiring | `ndnsf_distributed_inference/app_sdk/placement.py:AutomaticPlanningCoordinator._request_v3`, `_v3_catalog_snapshot_matches_candidate`; `tests/python/test_spec170_default_application_path.py` | The maintained V3 path could seal synthetic, unfetchable artifact names while dynamic provisioning was disabled because it never called `_prepare_artifacts` for an explicit active catalog. | Route an explicit catalog through `_prepare_artifacts`, require a matching ACTIVE snapshot for every role/rank, and retain the focused regression. |
| A180-132 | HIGH | Catalogue publication / runtime wiring | `tools/ndnsf-di/export_spec180_yolo26n_onnx.py`; `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:run_minindn_case`; `app_sdk/placement.py:CatalogSnapshotArtifactPublisher` | The package `spec180-yolo-catalogue-v1` certifies candidates but does not carry runtime `artifactDataNames`, while the User resolver requires the separate signed `ndnsf-di-presplit-catalog-snapshot-v1` envelope. The runner currently only validates/records a receipt and then fails closed; no candidate-bound role objects or active snapshot are published before User. | T011 must publish or verify every Y-A/Y-B candidate role/rank object, construct and sign the active snapshot with exact names/digests/backend/precision, publish it through `ServiceUser.publish_signed_app_data`, record its receipt, and only then start User. Do not republish package bytes or use the legacy `/Stage` manifest/offline file. |

The focused V3/application slice now reports 20 passing tests; the complete
`tests/python/test_spec180_*.py` collection now reports 169 passing with 22
existing warnings. `run_minindn_case()` still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no ACK/Selection/Provider/Response,
MiniNDN, SIF, or Tiger qualification is claimed. A180-132 therefore keeps
T011 partial and blocks the mandatory T014 convergence PASS.

## Iteration 84 current-source reconciliation (2026-09-03)

This checkpoint rechecked the staged runner against the native signed APP Data
implementation and the case identity validation path. The native publisher
accepts only names below `/<local-identity>/NDNSF/DI/`; the former fixture name
`/spec180/catalogue/v1` therefore could never be published by the controller
identity used by the maintained path. The runner and dispatch/binding fixtures
now require an exact `/NDNSF/DI/` name whose prefix equals the configured
controller signer. The same pass found an undefined identity lookup in
`initialize_keychains()` and a missing `providerPrefix` guard that raised a
`KeyError` rather than the fail-closed runner error. Both are corrected and
covered by focused regressions.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-127 | HIGH (resolved) | Protocol naming / production wiring | `ndn-service-framework/ServiceUser.cpp:4070-4082`; `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:_validate_catalog_identity` | The previous Spec180 catalogue name was outside the native `/<signer>/NDNSF/DI/` publication namespace, so the planned controller publication would fail after startup. | Enforce the native name/signer binding before startup; use `/example/controller/NDNSF/DI/catalogue/v1` with `/example/controller` in fixtures and future case manifests. Keep the live publication receipt and exact-name fetch in T011. |
| A180-128 | MEDIUM (resolved) | Fail-closed validation | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:_validate_case_config` | A missing `runtime.identities.providerPrefix` raised `KeyError` instead of a stable `RunnerError`, weakening the pre-start mutation boundary. | Validate and report `CASE_CONFIG_IDENTITY_INVALID:providerPrefix`; retain the focused negative. |
| A180-129 | HIGH (resolved) | Harness wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:initialize_keychains` | The explicit case adapter referenced `identities_parent` through an undefined local, so the first real keychain initialization would fail before protocol startup. | Bind `self.binding.identities` locally and verify the call arguments with a focused adapter test. |
| A180-130 | HIGH (resolved) | Failure atomicity / cleanup | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:MiniNdnCaseRuntime.start_processes` | A failure while launching a later child in one startup phase could leave earlier children running while the phase was reported as failed. | Roll back only the entries added by the failed phase with the maintained bounded process-group cleanup helper; add a partial-launch regression. |
| A180-126 | HIGH | Startup ordering / production wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:CaseProcessSpec`, `MiniNdnCaseRuntime.start_processes`, `wait_for_ready` | The previous implementation could start User before Controller/repository/Provider readiness and before the signed catalogue publication barrier, allowing `REQUEST_SENT` to race setup. | T011 live driver must call `start_processes(phase="control")` and wait, then `phase="providers"` and wait, publish the signed catalogue, then `phase="user"`; use retained handles for terminal/result and bounded cleanup. No implicit all-process launch is permitted. |

The four corrected findings are implementation/preflight evidence only. The
focused runner slice has 43 passing tests and the complete Spec180 Python
collection has 169 passing tests with 22 existing exporter/runtime warnings.
The real entrypoint still raises `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, so no
ACK/Selection/Provider/Response, MiniNDN, SIF, or Tiger qualification is
claimed. T011 remains partial and the mandatory T014 convergence `PASS` is
still required before formal validation.

## Iteration 83 current-source reconciliation (historical; superseded by iteration 84)

This checkpoint corrects the startup barrier exposed by comparing the runner
contract with the actual child-launch method. The prior vector was ordered but
`start_processes()` launched the User in the same call as Controller,
repository, and Providers. The runner now labels each child with an explicit
startup phase and exposes only phase-scoped launch: control, providers, then
user. Readiness is checked from each declared marker and an early child exit or
timeout fails closed. This remains a harness slice; the live driver still does
not call it.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-126 | HIGH | Startup ordering / production wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:CaseProcessSpec`, `MiniNdnCaseRuntime.start_processes`, `wait_for_ready` | The previous implementation could start User before Controller/repository/Provider readiness and before the signed catalogue publication barrier, allowing `REQUEST_SENT` to race setup. | T011 live driver must call `start_processes(phase="control")` and wait, then `phase="providers"` and wait, publish the signed catalogue, then `phase="user"`; use retained handles for terminal/result and bounded cleanup. No implicit all-process launch is permitted. |

The phase split, publication barrier, and input-type/node preflight are covered
by 38 focused runner tests. The real entrypoint still raises
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, so no ACK/Selection/Provider/Response,
MiniNDN, SIF, or Tiger qualification is claimed.

## Iteration 82 current-source reconciliation (historical; superseded by iteration 83)

This checkpoint closes the next bounded T011 harness slice. The case runtime
now derives an explicit Controller, repository, Provider, and User command
vector from the isolated policy and validated identity/node map. The vector is
data-only until `start_processes()` is called; no legacy role-map helper,
hard-coded AI-LAB node, or ambient path participates in command construction.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-125 | MEDIUM | Harness/reproducibility | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:CaseProcessSpec`, `MiniNdnCaseRuntime.process_specs/start_processes` | The explicit child-process vector is generated from the case policy, with fixed ACK-driven User arguments and Provider capability advertisements. It is covered by focused tests but no live child has yet been started by `run_minindn_case`. | T011 must call the vector after network/routing/keychain setup, wait on readiness markers, publish the signed catalogue and encrypted input, and bind actual coordinator request/attempt IDs before writing lifecycle evidence. |
| A180-123 | HIGH | Production wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:run_minindn_case`; `examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py`; `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` | The production entrypoint still constructs the validated adapter and stops with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no Controller/catalogue publication or ACK/Selection/Provider/Response exchange is reachable. The existing controller can deploy repository artifacts but does not publish the active signed catalogue APP Data record required by the resolver. The maintained User can publish encrypted input, but no live runner currently starts it. | Keep the fail-closed boundary. T011 must start the maintained process vector, wait for readiness, make the runner orchestration publish the exact signed catalogue APP Data, let the maintained User child publish the encrypted `REPO_REF`, bind live request/attempt IDs, emit the complete lifecycle, and add an ACK-to-Response oracle before T014. |
| A180-124 | MEDIUM | Code reality / validation | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:_validate_policy_loader_compatibility`, `validate_inputs`, `CaseRuntimeBinding.from_inputs` | Case-specific role and identity checks did not previously exercise the maintained `policy.py` parser, so missing user authorization or malformed service descriptors could fail only after child launch. | Resolved for the preflight boundary: validate the source policy and exact isolated `case-policy.json` with the maintained parser, authorization, role-coverage, and runtime-compatibility checks; retain the focused negative for missing users. |

The process-vector correction is historical implementation evidence only. The
focused runner slice had 31 passing tests; the complete Spec180 Python
collection had 156 passing tests with 22 existing exporter/runtime warnings. These checks
do not prove a live NFD/SVS exchange, numerical YOLO result, MiniNDN
qualification, SIF replay, or Tiger execution. T011 remains partial and T014
must re-audit the complete production path.

## Iteration 81 current-source reconciliation (historical; superseded by iteration 82)

This checkpoint follows the iteration-80 harness-boundary correction and the
subsequent policy-loader review. The runner now validates the maintained
application policy shape before any network creation, and revalidates the
isolated case policy that the future child processes will consume.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-123 | HIGH | Production wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:run_minindn_case` | The production entrypoint still constructs the validated adapter and stops with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no Controller/catalogue publication or ACK/Selection/Provider/Response exchange is reachable. | Keep the fail-closed boundary. T011 must wire the maintained User/Controller/repository/Provider process path, bind live request/attempt IDs, emit the complete lifecycle, and add an ACK-to-Response oracle before T014. |
| A180-124 | MEDIUM | Code reality / validation | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:_validate_policy_loader_compatibility`, `validate_inputs`, `CaseRuntimeBinding.from_inputs` | Case-specific role and identity checks did not previously exercise the maintained `policy.py` parser, so missing user authorization or malformed service descriptors could fail only after child launch. | Resolved for the preflight boundary: validate the source policy and exact isolated `case-policy.json` with the maintained parser, authorization, role-coverage, and runtime-compatibility checks; retain the focused negative for missing users. |

The policy-loader correction is implementation evidence only. The current
focused runner slice has 28 passing tests; the complete Spec180 Python
collection has 153 passing tests with 22 existing exporter/runtime warnings.
These checks do not prove a live NFD/SVS exchange, numerical YOLO result,
MiniNDN qualification, SIF replay, or Tiger execution. T011 remains partial and
T014 must re-audit the complete production path.

## Iteration 80 current-source reconciliation (historical; superseded by iteration 81)

This checkpoint follows the iteration-79 harness finding. The runner now
constructs an explicit case binding from the candidate descriptor and wraps
the maintained MiniNDN helper module without invoking its deployment-first
`main()` or role-assignment helper. Runtime identity maps are required beside
node maps, and the binding rechecks topology membership and isolated-policy
digest before a future network start. Shared MiniNDN nodes are represented by
merged route origins rather than by silently dropping one logical process.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-122 | HIGH | Harness/reproducibility | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:CaseRuntimeBinding`, `MiniNdnCaseRuntime`; `Experiments/NDNSF_DI_Yolo2x2_Minindn.py:python_cmd`, `start`, `initialize_di_keychains` | The prior explicit-adapter requirement had no executable adapter; a future implementation could still import legacy globals or lose controller/user/provider origins when sharing one MiniNDN node. | Fixed for the startup boundary: explicit node/identity binding, topology and policy-digest recheck, merged route-origin plan, and optional explicit roots on reused helpers. The live ACK-driven driver still must call this adapter and provide protocol IDs before any lifecycle event. |
| A180-123 | HIGH | Production wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:run_minindn_case` | The production entrypoint constructs the validated adapter but still stops with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no Controller/catalogue publication or ACK/Selection/Provider/Response exchange is reachable. | Keep the fail-closed boundary. T011 must wire the maintained User/Provider/Controller path through this adapter, bind live request/attempt IDs, emit the complete lifecycle, and add an ACK-to-Response evidence oracle before T014. |

The adapter correction is implementation evidence only. It does not start
MiniNDN, create an ACK snapshot, choose a Provider, execute Selection/Response,
or qualify a case. A180-113 (real ACK-driven MiniNDN driver), A180-114 (QWEN-F
27B ONNX entrypoint), and A180-115 (protected epoch enforcement) remain open.
The focused runner collection is 25 passing tests; the full Spec180 suite and
the T014 convergence audit remain required. No MiniNDN, SIF, or Tiger result is
promoted.

## Iteration 79 current-source reconciliation (historical; superseded by iteration 80)

This checkpoint corrects the lifecycle evidence boundary identified while
reviewing the iteration-78 profile repair. A generated request or attempt ID
inside the evidence writer could produce a plausible trace that was not bound
to the actual coordinator and ACK exchange. Production journaling now has an
explicit binding mode: the live driver must provide both identities before the
first milestone; provisional IDs remain limited to isolated journal tests.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-120 | MEDIUM | Evidence lineage | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:LifecycleJournal`; `contracts/yolo-minindn-runner-v1.md` | The journal previously generated both request and attempt identities, allowing an unwired driver to emit an unbound lifecycle trace. | Corrected with binding required by default, `bind_protocol_identity`, identity validation, and fail-before-event behavior. T011 must pass the coordinator request ID and ACK attempt identity from the live exchange. |
| A180-121 | HIGH | Harness/reproducibility | `Experiments/NDNSF_DI_Yolo2x2_Minindn.py:43-46,5447-5574,5793-5852`; `contracts/yolo-minindn-runner-v1.md` | The maintained helper module still contains hard-coded `AI_LAB_*` nodes and a deployment-first role-map path. Directly invoking those globals from Spec180 could silently run the wrong candidate topology or preassign roles while appearing to reuse the harness. | T011 must wrap the reusable startup/routing/keychain/process helpers behind one case-runtime function that consumes the validated `caseRuntime.nodes`, policy, output root, and identity list. If direct import cannot be made parameter-safe, extract the helpers once into a shared module and add a pre-start node/identity mismatch negative. Never call the legacy `main()` or `provider_role_assignments()` for qualification. |

The correction is evidence-integrity hardening only. It does not advance T011 or
T014. A180-113 (real ACK-driven MiniNDN driver absent), A180-114 (QWEN-F 27B
ONNX entrypoint absent), and A180-115 (protected epoch enforcement absent)
remain open. The focused runner collection is 20 passing tests and the full
Spec180 Python collection is 145 passing tests with 22 existing
exporter/runtime warnings; these are preflight/schema checks only. The
contract gate remains PASS with
`qualificationReady=false`, and no MiniNDN, SIF, or Tiger result is promoted.

## Iteration 78 current-source reconciliation (historical; superseded by iteration 79)

This checkpoint re-audited the fixed MiniNDN capability profiles and lifecycle
identity after the iteration-77 contract correction. The runner now rejects a
configuration that merely mentions every role but cannot exercise distinct
Provider ownership. It also records an attempt identity for every lifecycle
event; the iteration-79 audit subsequently required that identity to be bound
to the live coordinator/ACK exchange. The focused runner collection was 19
passing tests; these were preflight/schema checks only, and the real runner
remained fail-closed.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-118 | HIGH | Test validity | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:_validate_case_config`; runner contract/profile | Role-presence-only validation allowed fewer than four usable Provider identities, or a non-matching capability layout, for Y-B/Y-N. A nominal multi-Provider policy could therefore execute as a single-Provider smoke or fail before the intended candidate decision. | Resolved with exact Y-A=1, Y-B=4, Y-N=4 identity counts and a distinct bipartite capability-cover witness. The witness is preflight-only; live ACK/sealed-plan assignment remains required. |
| A180-119 | MEDIUM | Evidence lineage | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:LifecycleJournal`; runner contract | Lifecycle records carried `requestId` but no explicit attempt identity, so retries/attempt lineage would be inferred from timestamps. | Resolved historically by adding the field; the current iteration-79 audit additionally requires coordinator/ACK binding. |

The profile correction does not advance T011 or T014. A180-113 (real
ACK-driven MiniNDN driver absent), A180-114 (QWEN-F 27B ONNX entrypoint
absent), and A180-115 (protected epoch enforcement absent) remain open.
The verdict is still **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
BLOCKED**.

## Iteration 77 current-source reconciliation (historical; superseded by iteration 78)

This checkpoint re-audited the candidate-negative semantics and the ownership
boundary for the planned MiniNDN runner. The structural and contract gates
still pass, and the focused collection remains 143 passing tests with 22
warnings. These are source and boundary checks only; the registered runner
still fails closed before starting a live workload.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-116 | HIGH | Evidence integrity | `spec.md` Y-N matrix; `contracts/yolo-minindn-runner-v1.md`; `data-model.md` | The former Y-N-C wording could remove one shared-role capability while leaving `atomic-v1` feasible, despite declaring a no-feasible-candidate outcome. | Resolved in the current contract, data model, plan, task, and traceability records: the closed ACK snapshot must remove `FullModel` and at least one required shared-candidate role capability. T011/T014 still require executable proof. |
| A180-117 | MEDIUM | Architecture/reproducibility | `tasks.md:T011`; `Experiments/NDNSF_DI_Yolo2x2_Minindn.py`; `contracts/yolo-minindn-runner-v1.md` | T011 did not previously require reuse of the maintained MiniNDN/NFD/SVS startup, routing, keychain, and process-supervision helpers, allowing a second inconsistent harness. | Resolved in the current contract, plan, task, and traceability records. Prefer direct import; if extraction is required, use one shared helper module for both callers. Do not invoke the legacy deployment-first `main()` or copy its output. |

A180-113, A180-114, and A180-115 remain open: the real ACK-driven driver,
QWEN-F 27B ONNX entrypoint, and protected-epoch enforcement are still absent.
The detailed evidence is preserved in
`evidence/audit-iteration77-current-20260903.md`.

The verdict remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
BLOCKED**. No task status or qualification result is promoted by this audit.

## Iteration 76 current-source reconciliation (historical; superseded by iteration 77)

This audit rechecked the current Spec180 source, contracts, inventory, and
result-validation boundary after the iteration-75 dispatcher correction. Two
qualification-affecting drifts were found and repaired. First, the local
inventory duplicated the obsolete seed `1750001` for both Q-C and Q-W even
though the frozen Qwen manifest binds `175021` and `175022`; the inventory now
reads the source-case/seed tuple from that manifest. Second, the terminal
validator checked only oracle shape and digests, so semantically false counts,
CPU backend values, duplicate children, or incomplete cleanup could pass; it
now enforces the fixed two-request, three-device CUDA/no-CPU-fallback, unique
child, and cleanup assertions. The focused collection is 143 passing tests
with 22 warnings.

These are boundary repairs, not qualification evidence. The production
`run_minindn_case` entrypoint still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no live ACK/Selection/Provider/Response
run, T014 convergence PASS, MiniNDN qualification, SIF replay, or Tiger result
exists. The gate therefore remains **CONDITIONAL PASS FOR IMPLEMENTATION;
FORMAL VALIDATION BLOCKED**.

### Current findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-111 | HIGH | Evidence integrity | `scripts/spec180_inventory.py:49-77`, `contracts/qwen-reference-manifest-v1.json:26-40` | The local inventory used a duplicated historical seed for Q-C and Q-W, so the registered case IDs did not identify the frozen workloads in the manifest. | Resolved by deriving source-case/seed arguments from the frozen manifest and adding inventory regression coverage. Keep T012 manifest authoritative; T015 remains open until the current candidate executes the corrected entries. |
| A180-112 | HIGH | Evidence integrity | `scripts/validate_spec180_results.py:111-167`, `tests/python/test_spec180_release_workflow.py` | Structured oracle paths were checked, but their scalar assertions were not bound to the fixed two-request Tiger contract; a CPU backend, duplicate child, wrong counts, or incomplete cleanup could be labeled PASS. | Resolved with semantic count/backend/device/fallback/child/cleanup checks and focused negative tests. T020 still must validate real candidate-bound oracle files. |
| A180-113 | HIGH | Production wiring | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:672-680` | The registered Y-A/Y-B/Y-N entrypoint still raises `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, so the ACK-driven protocol path has not executed. | T011 must wire real NFD/NDN-SVS startup, authenticated ACK closure, Selection, Provider execution, dependency/result flow, lifecycle evidence, and cleanup; then T014 must re-audit. |
| A180-114 | HIGH | Cross-model execution | `scripts/run_spec180_case.py:79-86`, `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py` | QWEN-F is correctly fail-closed at dispatch, but its separate Qwen3.6-27B ONNX entrypoint does not yet exist. | Superseded by iteration 92: T013 must implement the external-manifest-bound 27B ONNX path before audit/SIF; T019 only runs it. Never route QWEN-F to Q-C/Q-W or the Spec175 M11 wrapper. |
| A180-115 | HIGH | Security/architecture | `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:1324-1331`, `spec.md:704-708` | `RoleAssemblySpec` still defaults `protection_epoch` to `plaintext-v1`; without a qualification-only rejection, a V3 assignment can satisfy the shape checks while carrying an unprotected epoch. | The contract now explicitly rejects this source-compatible default for Spec180 evidence. T007/T010 must enforce the protected epoch in Python/native validation and add a focused regression before T014 can pass. |

The iteration-75 section below is historical and remains useful only for the
dispatcher-routing correction; it is superseded by this iteration.

This audit rechecked the dispatcher/release contract after the QWEN-F routing
slice. The previous mapping sent `qwen-functional` to the Spec175 M11 wrapper
with a QWEN-F label. That was a qualification-invalid fallback: it could run
the tiny local fixture while the fixed Tiger gate claimed Qwen3.6-27B. The
dispatcher now reserves `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py` for the
external ONNX runner, uses `--case QWEN-F`, and rejects the absent entrypoint
before starting a workload. The four Qwen workload fields are identity
metadata only; the fixed runtime manifest/root mounts are the sole path
bindings, and model/prompt identity fields are required to be `sha256:`
digests. The host release validator performs the same source-entrypoint
readiness check before any scheduler call.

The correction is covered by dispatcher and release tests. It does not close
T019: no Qwen3.6-27B ONNX runner, Tiger result, or QWEN-F qualification exists.
The verdict therefore remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL
VALIDATION BLOCKED**.

### Current finding

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-110 | HIGH | Production/evidence integrity | `scripts/run_spec180_case.py`, `scripts/spec180_release.py`, `contracts/yolo-minindn-runner-v1.md` | QWEN-F previously reused the Spec175 M11 tiny-model entrypoint and duplicated model path bindings, so a remote gate could be mislabeled or resolve a different mounted artifact. | Resolved at the dispatch/release boundary: reserve the external Qwen3.6-27B ONNX entrypoint, use a fixed `--case QWEN-F` vector, keep only identity metadata in the workload, and reject the absent entrypoint before scheduler or child startup. Superseded by iteration 92: T013 implements it before T014/SIF; T019 only executes it. |

## Iteration 74 current-source reconciliation (historical; superseded by iteration 75)

This iteration is a focused evidence refresh after the iteration-71 semantic
YOLO exporter/adapter repair, the T011 dispatcher implementation, and the
T013 release/wrapper digest checks. The current
`tests/python/test_spec180_*.py` collection reports 140 passing tests
and 22 warnings. The structural audit
still passes (25 FR, 9 SC, 20 tasks), and the Spec180 contract gate still
reports `contractReady=true` and `qualificationReady=false`.

The dispatcher is now present at `scripts/run_spec180_case.py` and is included
in the source archive together with the YOLO entrypoint. Focused positive and
negative tests cover duplicate/unknown fields, digest mismatch, fixed routing,
traversal/shell/private-material rejection, mount binding, output freshness,
and ambient-environment removal. This is execution-boundary evidence only.
The real NFD/NDN-SVS ACK-to-Response driver remains fail-closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no MiniNDN, SIF, or Tiger result is
promoted, and T014 remains the mandatory convergence gate before T015--T020.

The dispatcher boundary remains closed: its only model-specific input is the
candidate-digest-bound workload document. It validates the closed field set,
gate/case binding, digest, `/bundle` entrypoint, fixed argument and environment
allowlists, fixed mounts, and fresh evidence root before starting any
maintained process. It never derives Provider assignments or model cuts.

The host release path now imports the same dispatcher validator before
submission, and the remote `run-functional.sh` rechecks the mounted model
manifest/workload digests before `mkdir -p` on the evidence root. This closes
the host-to-image content-replacement gap at the wrapper boundary only. The
actual SIF embedding and in-image probe remain T016 work.

The current evidence is recorded in
`evidence/audit-iteration74-current-20260903.md`.

## Iteration 71 current-source reconciliation (historical; superseded by iteration 72)

The iteration-71 implementation checkpoint closes the heuristic safe-cut
finding at the canonical exporter/adapter boundary. The exporter classifies
YOLO nodes by registered architecture scopes (including lifted constants by
data-flow), emits a complete semantic partition certificate, and records all
cross-role tensor contracts and role dependencies. The adapter reconstructs
the role map from the signed node sets and verifies interfaces, dependency
edges, and safe cuts against the loaded graph before creating a runtime
candidate. No percentage or fixed-index cut remains in this path. Focused
exporter, catalogue, planning, and altered-partition regressions pass.

This is still an implementation checkpoint, not T014 convergence: native
component assembly, the live NFD/NDN-SVS ACK-to-Response driver, production
result writing, MiniNDN, SIF, and Tiger execution remain open.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-108 | HIGH | Design/code convergence | `tools/ndnsf-di/export_spec180_yolo26_onnx.py`; `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/{candidates,adapter}.py` | The registered YOLO package now binds semantic node sets, lifted-constant ownership, tensor interfaces, role dependencies, and safe cuts; the runtime adapter checks the certificate against the ONNX graph. | Keep the semantic certificate and graph revalidation in the production/native assembly path; close T004/T005 only after the registered signed package is consumed by the maintained application and the full-model equivalence/assembly evidence exists. |
| A180-109 | MEDIUM | Evidence semantics | `tools/ndnsf-di/export_spec180_yolo26_onnx.py:498-504`; `tests/python/test_spec180_yolo_export.py` | The oracle is generated from the PyTorch reference and compared with CPU ONNX Runtime; labeling it as an ONNX-only oracle would overstate what was measured. | Resolved in iteration 71: the manifest names `pytorch-reference-compared-with-cpu-onnxruntime`, and the 640-by-640 regression performs the explicit comparison. |

Iteration-70 findings A180-103 and A180-106 remain current. A180-104 is closed
at the validator boundary but its production result-writer integration remains
open. A180-107 remains closed at the lifecycle journal seam; the future driver
must carry its allowlist into the terminal oracle.

## Iteration 70 current-source reconciliation (historical; superseded by iteration 71)

The iteration-70 implementation checkpoint closes the lifecycle-schema finding
from iteration 69. `LifecycleJournal.append()` now uses a milestone-specific
allowlist, rejects generic payload/content/token/byte/credential fields and
nested or byte-like values, and preserves the secret-field rejection path.
The focused runner and release-validator tests pass after this change. The
real ACK-driven MiniNDN driver, semantic safe-cut replacement, in-image
dispatcher, and structured terminal-result production path remain open, so
this correction does not advance qualification.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-107 | HIGH | Design/code convergence | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:67-90,568-610`; `tests/python/test_spec180_yolo_minindn.py` | Closed lifecycle field validation is implemented and its generic-payload, unknown-field, nested-value, and secret-field negatives pass. | Retain the allowlist in the production driver and include the lifecycle digest/schema in the terminal structured oracle. T011/T014. |

Iteration-69 findings A180-103 and A180-106 remain current. A180-102 is
resolved at the exporter/adapter boundary above. A180-104
is closed at the validator boundary: structured oracle records, candidate
identity, safe relative paths, schemas, and on-disk SHA-256 verification are
now required. Its production result-writer integration remains open. A180-105
is superseded by the iteration-70 implementation correction above; the future
driver must still carry the same allowlist into its terminal oracle.

### Iteration-69 current-source reconciliation (historical context)

The iteration-69 review found four implementation-boundary gaps and one
specification ambiguity. The YOLO exporter and adapter still infer shared-role
cuts from node-count percentages/topological indexes rather than proving the
YOLO branch and tensor interfaces. The remote job probes a planned
`/bundle/scripts/run_spec180_case.py`, but that dispatch entrypoint is absent.
The terminal validator accepts label-only `PASS` strings without evidence
paths or digests. The lifecycle journal rejects obvious secret names but
accepts arbitrary fields such as `payload`. Finally, the preflight wording did
not clearly distinguish recording an opaque catalogue digest from exposing
candidate requirements before ACK closure. These are now explicit in the
requirements, contracts, tasks, and traceability; they do not advance any
qualification gate.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-102 | HIGH | Design/code convergence | `tools/ndnsf-di/export_spec180_yolo26_onnx.py`; `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/adapter.py` | Historical: shared-role cuts were derived from a 55% prefix and fixed topological indexes. | RESOLVED at the exporter/adapter boundary in iteration 71; production/native assembly and equivalence evidence remain T007/T008/T014 work. |
| A180-103 | HIGH | Production wiring | `packaging/ndnsf-di-container/jobs/spec180/run-functional.sh:56-63`; `scripts/run_spec180_case.py` | RESOLVED at the repository/dispatcher boundary in iteration 73: the candidate-bound dispatcher now exists, is source-archive included, and has focused mutation coverage. A SIF must still embed the exact file and pass the in-image probe before remote execution. | T016 must embed and probe the exact dispatcher in the candidate SIF; T014/T015 still require the real ACK-driven driver and local evidence. |
| A180-104 | HIGH | Evidence integrity | `scripts/validate_spec180_results.py:48-86` | The validator accepts five oracle fields as the string `PASS`; a minimal fabricated manifest therefore passes without lifecycle, numeric/token, device, path, digest, or cleanup evidence. | T013/T020 must require structured schema-checked, digest-verified evidence references bound to the candidate and reject label-only fields. |
| A180-105 | MEDIUM | Security/evidence | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:560-579` | Lifecycle redaction uses a substring secret check and accepts generic `payload`/content-like fields that can carry plaintext. | T011/T014 must add a milestone-specific field allowlist and recursive non-secret value validator with focused generic-payload negatives. |
| A180-106 | MEDIUM | Specification consistency | `spec.md:FR-003`; `contracts/ack-driven-yolo-v1.md` | Preflight catalogue validation and post-ACK candidate enumeration were described in ways that could be read as contradictory. | Permit only opaque signed revision/digest retention before `ACK_CLOSED`; keep candidate records, feasibility, and Provider binding post-ACK. |

The reproducible details and gate commands are preserved in
`evidence/audit-iteration69-current-20260903.md` and the iteration-70 focused
result in `evidence/audit-iteration70-current-20260903.md`.

## Iteration 68 current-source reconciliation (2026-09-03)

The runner-input audit found a semantic mismatch in the pre-start policy check.
`service.providers[*].roles` was described as one fixed Provider owner per role,
which could make an authorization allowlist look like a placement decision and
would reject a legitimate Provider capability advertisement covering multiple
roles. The runner now checks role coverage rather than ownership, permits
overlapping capability advertisements, and reports `roleCapabilities`; only the
authenticated post-ACK snapshot and sealed plan may choose the final one-to-one
Provider assignment. The node map remains a process-start concern. The focused
runner slice remains 37 passing tests (the test was changed, not added); this is
a boundary correction, not live qualification evidence.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-101 | HIGH | Design/code convergence | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:243-389`, `contracts/yolo-minindn-runner-v1.md` | Startup capability entries were labeled fixed role owners and multi-role advertisements were rejected, creating an apparent offline placement authority. | Resolved in iteration 68: validate capability coverage and node mapping only; preserve ACK/sealed-plan assignment authority and add focused overlap coverage. |

### Current gate results

- Structural audit: PASS (25 FR, 9 SC, 20 tasks, 25 traced requirements).
- Document/trust-root contract gate: PASS, `contractReady=true`,
  `qualificationReady=false`.
- Combined runner/local-gate/inventory/contract tests: 37 passed.
- Full current Spec180 Python collection: 125 passed, 22 existing
  exporter/runtime warnings.
- Live qualification: NOT RUN. The preflight-only runner returns
  `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no MiniNDN, SIF, or Tiger result is
  attached to this candidate.

The controlling verdict remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL
VALIDATION BLOCKED**. T011 (real ACK-driven case driver) and T014 (fresh
design-code convergence `PASS`) are still the next gates. No expensive local,
container, or Tiger validation may start before both gates close.

## Iteration 67 current-source reconciliation (historical; superseded by iteration 68)

The current documents, runner, focused tests, and contract gate were compared
again. T012's existing checked-complete status is consistent with its frozen
reference evidence. Two stale evidence references were corrected: the combined
runner/local-gate/inventory/contract count is 37 rather than 36, and the full
current collection is 122 rather than the earlier 121. Traceability now points
to the current iteration-67 checkpoint. These changes do not alter runtime
behavior or qualification scope.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A180-97 | MEDIUM | Evidence integrity | `evidence/audit-iteration65-current-20260903.md` | The prior checkpoint recorded 36 combined focused tests although the current command returns 37. | Resolved in current records; retain the old file as historical evidence and use iteration 67 for the current count. |
| A180-98 | LOW | Traceability | `traceability.md` | The audit evidence link still named iteration 64 after newer runner corrections. | Resolved: it now links to `evidence/audit-iteration67-current-20260903.md`. |
| A180-99 | HIGH | Code reality / validation | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:588-596` | The registered entrypoint still has no real NFD/NDN-SVS driver and deliberately fails closed before protocol startup. | Open: implement and execute T011, then rerun the mandatory T014 convergence audit before T015--T020. |
| A180-100 | MEDIUM | Evidence integrity | `evidence/audit-iteration67-current-20260903.md`, full `test_spec180_*.py` collection | The full current collection grew from the previously recorded 121 to 122 tests after the latest source/test state; an outdated current count would make the checkpoint non-reproducible. | Resolved: current records report 122 passed and preserve earlier counts only as historical snapshots. |

### Current gate results

- Structural audit: PASS (25 FR, 9 SC, 20 tasks, 25 traced requirements).
- Document/trust-root contract gate: PASS, `contractReady=true`,
  `qualificationReady=false`.
- Combined runner/local-gate/inventory/contract tests: 37 passed.
- Full current Spec180 Python collection: 122 passed, 19 existing
  exporter/runtime warnings.
- Live qualification: NOT RUN. The preflight-only runner returns
  `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no MiniNDN, SIF, or Tiger result is
  attached to this candidate.

The controlling verdict remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL
VALIDATION BLOCKED**. T011 (real ACK-driven case driver) and T014 (fresh
design-code convergence `PASS`) are the next gates. No expensive local,
container, or Tiger validation may start before both gates close.

## Iteration 65 runner-input correction (historical; superseded by iteration 68)

The registered YOLO runner now validates the candidate policy and MiniNDN node
map before starting any protocol process. Y-A, Y-B, and Y-N must declare their
registered role sets; each role must have exactly one explicit Provider owner;
multi-role, duplicate-owner, and missing-owner declarations fail closed. The
controller, user, repository, and Provider identities must also map to nodes
declared in the supplied topology. This check prevents an invalid process
layout from being mistaken for an ACK-driven result, but it is not placement
authority: Provider assignment still comes only from the authenticated
post-ACK snapshot. The focused runner/local-gate/inventory/contract slice now
passes 37 tests. The NFD/NDN-SVS driver, live ACK-to-Response trace, and
candidate-bound inventory execution remain open; the formal verdict is
unchanged.

At iteration 65, the complete Spec180 Python collection was also rerun after
that source change: **121 passed, 19 warnings**. The warnings are exporter/runtime
diagnostics and do not provide live NFD/NDN-SVS evidence. The runner still
returns `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED` after preflight, so this larger
focused collection does not advance T011 or qualification readiness.

The runner now additionally materializes a fresh case-local
`case-policy.json`, filtering the candidate config to the registered case
roles and explicit role owners without changing the source policy. Its digest
is recorded in `case-input.json`. This closes the previously implicit
process-policy boundary but does not add a runtime placement authority or a
live protocol result. The formal verdict remains unchanged.

## Iteration 64 terminology and status correction (2026-09-03)

The current source and maintained YOLO example were rechecked after the
iteration-63 audit. No qualification evidence or task completion evidence was
added. The maintained path consumes the candidate-bound Ed25519 policy and PEM
key map, while the historical caller HMAC map is fixture-only; the former still
depends on the native Trust-Schema certificate-chain callback, which has not
been exercised by the real MiniNDN driver. The quickstart and implementation
anchor now use that distinction consistently. T010, T011, and T013 remain
partial, T014--T020 remain blocked or not started, and the overall verdict is
unchanged.

## Iteration 63 status and verifier correction (2026-09-03)

The current-source audit found no new qualification evidence and no reason to
promote a task. The candidate-bound `ProviderOfferTrustVerifier` is a focused
implementation and still delegates packet/certificate authentication to the
native Trust-Schema path; the source-bound YOLO preflight, `case-plan.json`,
lifecycle journal, inventory generator, and local-gate supervisor are likewise
preflight/focused evidence. `run_minindn_case` still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, so the real NFD/NDN-SVS Y-A/Y-B/Y-N
driver, candidate-bound inventory execution, and live ACK-to-Response trace
remain open. No MiniNDN, SIF, or Tiger result is added.

This iteration corrects two current-status descriptions. First, the
maintained discovery row now distinguishes the implemented candidate-bound
offer verifier from the still-missing live certificate-chain callback and
production ACK-to-Response execution. Second, the readiness summary now names
the missing real case driver and inventory execution rather than describing the
already-existing preflight and supervisor as absent.

## Iteration 62 candidate-plan refresh (2026-09-03)

The runner now writes a candidate-bound `case-plan.json` after validating the
canonical package. The plan is limited to signed candidate role metadata and
the fixed Y-N expected outcomes; it deliberately contains no Provider names,
role assignments, or caller-selected subset. This makes the runner's intended
case topology inspectable without weakening the post-ACK placement authority.

The focused runner tests pass (**34 passed** for the runner/local-gate,
inventory, and contract subset), and the strict structure/document gates remain
passing. This is still preflight evidence: `run_minindn_case` remains fail-
closed with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, `qualificationReady=false`,
and T014--T020 remain blocked. No MiniNDN, SIF, or Tiger result is added.

## Iteration 61 status-consistency refresh (2026-09-03)

The current task ledger, plan, traceability matrix, and code-reality table were
compared again after the iteration-60 runner-boundary corrections. No runtime
behavior changed. The focused native canonical-assembly slice is present but
does not prove the registered YOLO fan-out/fan-in execution, dependency
transfer, native Merge, or numerical oracle; A180-05 is therefore `PARTIAL`,
matching T007/T008/T010 and the top-level status. The iteration-28 and
iteration-30 descriptions in `spec.md` are explicitly historical so their old
caller-HMAC/key-map wording cannot be mistaken for the current trust boundary.

The strict structure gate and document/trust-root gate remain passing. T011's
entrypoint is still a fail-closed preflight seam rather than a live NFD/SVS
driver, so `qualificationReady=false`; T014--T020 remain blocked. No MiniNDN,
SIF, or Tiger result is added by this documentation audit.

## Iteration 55 design/code and execution-contract refresh (2026-09-03)

This audit rechecked the current Spec180 documents, source-owner map, runner
contract, local inventory contract, and CodeGraph status. The document and
trust-root gates remain passing (`25` FRs, `9` SCs, `20` tasks; no contract
issues), but the Y-N description had an execution ambiguity: catalogue-order
permutation is a required invariance control, whereas insufficient capability,
bad ACK provenance, invalid role kind, non-ingress input fetch, stale protection
epoch, and plaintext logging are fail-closed negatives. Treating all of these
as one class could make a runner either reject a valid control or accept an
unexpected success.

The contracts and task ledger now register `Y-N-O` plus `Y-N-C/P/R/I/E/L`.
They require one inventory command with fresh state/output subdirectories for
each subcase, fixed subcase order, no caller-selected subset, and one aggregate
marker only after all expected outcomes agree. The runner contract also now
requires one non-secret JSONL record per lifecycle milestone, with unique
monotonic sequencing and strict event order. These are documentation and
acceptance-boundary corrections; no runner, live ACK-to-Response trace, local
qualification, SIF, or Tiger result has been created.

- **A180-85 (HIGH, resolved in the design boundary)** — Y-N mixed an order
  invariance control with failure cases and did not define whether each case
  needed a separate topology/state root. The fixed seven-subcase matrix and
  fresh-subcase rule now make the expected result and evidence ownership
  executable.
- **A180-86 (MEDIUM, resolved in the evidence boundary)** — The lifecycle
  sequence was prose-only, with no uniqueness or machine-readable event
  requirement. The runner contract now requires exactly-once JSONL events,
  monotonic sequence, non-secret fields, and rejection of missing, duplicate,
  out-of-order, or plaintext-bearing events.
- **A180-87 (MEDIUM, resolved in wording)** — `SC-004` said “current-source”
  even though Spec180 evidence is candidate/source-bound and the worktree has a
  separate post-Spec175 delta. It now says “source-bound local gate,” matching
  the candidate and invalidation contracts.
- **A180-88 (LOW, resolved in plan text)** — The plan contained stale duplicated
  Qwen scale text and an ambiguous Y-N summary. The scale line is deduplicated
  and the case list now points to the fixed runner matrix.
- **A180-89 (MEDIUM, resolved in the event contract)** — The lifecycle event
  paragraph listed `REQUEST_SENT` onward but omitted the required
  `INPUT_REFERENCE_PUBLISHED` event for `REPO_REF`. The exact event list now
  includes that publication milestone, preserving the required pre-request
  ordering.

## Iteration 60 runner-boundary refresh (2026-09-03)

The missing T011 path was rechecked after the Spec180 audit correction. The
source-bound entrypoint now exists at
`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`. Before it can start any NFD,
SVS, repository, Provider, or User process, it requires all eight declared
`SPEC180_YOLO_*` inputs plus a fresh `SPEC180_CASE_OUTPUT_DIR`, verifies the
canonical YOLO package and signed catalogue through the maintained adapter,
checks graph/initializer digests, validates the explicit catalog Data
name/signer and trust-root schema, records trust-root/public-key-map/per-key
digests, recursively rejects private/HMAC/secret metadata, and creates no
protocol evidence on failure. The supervised local gate owns
creation of the empty per-case output root; the runner rejects a missing or
pre-populated root. Its `LifecycleJournal` enforces the
exact-once, monotonic event order required by the runner contract and rejects
secret-bearing fields.

This is a preflight and evidence-writer slice, not a MiniNDN result. The
entrypoint intentionally returns `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED` until
the real NFD/NDN-SVS process orchestration and ACK-to-Response path are
connected; it emits no `status=PASS` marker. Focused runner, contract,
inventory, and local-gate tests pass (**31 passed**). The contract and strict
structure gates still pass, while `qualificationReady=false`; T011 remains
partial, and T014--T020 remain blocked by the missing live driver.

- **A180-90 (HIGH, resolved at preflight boundary)** — The registered
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` path was absent, so the
  inventory could not even name the intended case owner. The new entrypoint
  closes the source-bound preflight and exact-once lifecycle-writer gap.
- **A180-91 (HIGH, open)** — The entrypoint still has no real MiniNDN driver.
  A preflight or injected callback cannot satisfy the required Trust-Schema
  ACK, Selection, Provider execution, encrypted input fetch, and terminal
  Response evidence. T011 must wire the existing MiniNDN/NFD/SVS setup and
  then rerun the T014 convergence audit before any local or remote
  qualification.

- **A180-92 (MEDIUM, resolved at evidence-root boundary)** — The first runner
  draft created a missing case-output path itself, which could hide a bad
  supervisor path. The local gate now creates each empty case root before
  launch, and the runner rejects missing/non-directory/non-empty roots.

- **A180-93 (MEDIUM, resolved at candidate-input boundary)** — The preflight
  initially checked the files but did not persist a candidate-bound descriptor
  or reject a legacy `/Stage`/caller-HMAC policy before driver startup. It now
  writes a non-secret `case-input.json` digest descriptor and rejects those
  policy forms; the descriptor is preflight evidence only.

- **A180-94 (HIGH, resolved at candidate-binding boundary)** — The preflight
  required catalog/trust inputs by environment presence but did not bind the
  catalog Data name/signer, trust-root schema, public-key-map, or nested
  manifest metadata to the case descriptor. It now validates and records these
  digests before any process startup; the live driver and certificate-chain
  execution remain open.

- **A180-95 (HIGH, resolved at package-boundary)** — A manifest-controlled
  initializer path could otherwise escape the canonical package root even when
  its digest matched. The runner now rejects absolute/traversal paths for both
  graph and initializer objects before the driver boundary; the maintained
  adapter and live execution still require their own convergence evidence.

- **A180-96 (MEDIUM, resolved in the status table)** — The current-code table
  described native canonical ONNX assembly as implemented and wired, but marked
  the same finding `PLANNED`. That understated the focused native
  assembly/role-kind work already present and could cause an implementer to
  repeat it. The status is now `PARTIAL`: production YOLO branched assembly,
  dependency transfer, Merge execution, and the numerical oracle remain open
  under T007--T010.

The overall verdict remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL
VALIDATION BLOCKED**. T011's runner and T014's mandatory convergence audit are
still the next controlling implementation gates.

## Iteration 52 design/code and reproducibility refresh (2026-09-03)

The ACK provenance boundary was re-audited against the native projection and
the maintained YOLO caller. `AckAuthenticationEvidence` now carries an
explicit `trustSchemaValidated` bit set only when the packet-backed
`ServiceUser` path has attached the validated Data packet; the pybind and
Python `AckCandidate` projections preserve it. `ProviderOfferTrustVerifier`
rejects candidates without that marker, and the maintained YOLO helper loads
the candidate-bound Ed25519 policy/key map instead of using the historical
caller HMAC map. Focused tests pass after this correction. This still does not
prove the native certificate-chain validator independently for a live case,
and the Y-A/Y-B/Y-N runner remains absent; formal validation is therefore
still blocked.

The focused Spec180/Spec175 compatibility collection was rerun after the
native rebuild: **114 tests passed with 19 existing warnings**. This confirms
the source projection and fail-closed guards only; it is not a MiniNDN or
qualification result.

- **A180-82 (MEDIUM, resolved at source boundary)** — The maintained YOLO
  example still selected an HMAC key map even though the Spec180 contract
  reserved that map for fixtures. It now constructs `ProviderOfferTrustVerifier`
  from the candidate-bound policy and PEM public-key map, and delegates the
  packet-authentication decision to explicit native provenance. The legacy
  option is rejected in the maintained path.
- **A180-83 (HIGH, open)** — A native provenance marker is not a substitute for
  executing the configured Trust Schema/certificate chain in a real
  ACK-to-Selection-to-Provider-to-Response run. The maintained runner and
  live evidence are still required before T006/T011/T014 can close.

## Iteration 53 design/code and reproducibility refresh (2026-09-03)

The current native subscription wiring was checked against the SVS API. The
ACK regex subscription had been registered with `packets=false`, which makes
SVS deliver only payload bytes and leaves `SubscriptionData::packet` empty.
That silently defeated the iteration-52 Trust-Schema provenance projection on
the real ACK path. The registration now uses `packets=true`, retaining the
already validated Data packet for signer, KeyLocator, and complete-wire digest
projection. A focused wiring regression protects the setting, and the native
library was rebuilt successfully.

The strict structure gate, document/trust-root contract gate, Python/native
focused collection, Python syntax checks, native import, repository linkage,
and whitespace checks pass; evidence is recorded in
`evidence/audit-iteration53-current-20260903.md`. This is still a checkpoint,
not the mandatory T014 convergence audit. A real certificate-chain callback,
maintained Y-A/Y-B/Y-N runner, and live ACK-to-Response trace remain absent;
formal validation and qualification therefore remain blocked.

- **A180-84 (HIGH, resolved at native subscription boundary)** — ACK packet
  retention was disabled even though V3 trust verification required the
  validated Data packet. `ServiceUser::registerNDNSFMessages()` now registers
  the ACK subscription with `packets=true`, and the source-level regression
  fails if it is changed back to payload-only delivery.

## Iteration 54 design/code and handoff refresh (2026-09-03)

Spec175's handoff was re-read against its authoritative source seal and is now
explicitly a frozen `LOCAL_FUNCTIONAL_PASS` baseline. The current dirty tree
contains Spec180 source and harness changes, so the newly generated G0 seal
(`results/spec175/g0/source-seal-current-20260903.json`) is only a byte-level
closure check; it does not inherit the frozen Spec175 T021/T022 execution
results. Spec180 must record this source delta and rerun T014 convergence before
T015 or any SIF/Tiger work.

The execution contract is now stated in one place: encrypted input-reference
publication precedes the generic request; authenticated ACK collection closes
at the registered 1500 ms boundary; candidate planning follows that closed
snapshot; one Selection commits the plan; only selected roles fetch protected
inputs; and exactly one terminal result is accepted. YOLO remains one-shot,
whereas Qwen remains prefill plus automatic decode. No new task is marked
complete by this documentation refresh; the live ACK-to-Response runner and
formal qualification remain blocked.

## Iteration 51 design/code and reproducibility refresh (historical)

The prior iteration-50 structure/contract checks remain valid. This iteration
adds the production-shaped `ProviderOfferTrustVerifier`, which loads the
candidate-bound policy, requires registered Ed25519 keys, delegates packet
authentication to the existing Trust Schema verifier, and validates ACK
provenance before the V3 planning view is built. Its factory integration and
positive/negative focused tests pass. This is still implementation evidence:
the real certificate-chain callback and maintained ACK-to-Response runner are
not present, so T006 remains partial and formal validation remains blocked.

- **A180-80 (MEDIUM, resolved at focused implementation boundary)** — The
  candidate-bound Provider-offer policy previously had no executable verifier
  owner. `ProviderOfferTrustVerifier` now implements the policy/signature/
  provenance checks and is invoked by `v3_provider_view_factory` when supplied;
  focused evidence is recorded in
  `evidence/t006-provider-offer-verifier-current-20260903.md`.
- **A180-81 (HIGH, open)** — The maintained network configuration still needs
  to supply a real Trust Schema certificate verifier and the Y-A/Y-B/Y-N runner.
  No local, SIF, or Tiger qualification may be inferred from the focused tests.

## Iteration 50 design/code and reproducibility refresh (historical)

This audit reran the strict Spec Kit structure gate, the Spec180 contract gate,
CodeGraph freshness, and the current focused Python collection after the
iteration-49 source boundary. The structure gate reports 25 functional
requirements, 9 success criteria, 20 tasks, and complete traceability. The
contract gate reports `status=PASS`, `contractReady=true`, and
`qualificationReady=false`. The focused collection reports 105 passed tests
with 19 warnings in 35.37 seconds. `git diff --check` is clean.

Three documentation defects were corrected. First, the iteration-32 and
iteration-42 notes now explicitly identify their old task/test counts as
historical and state the current T010/T013 partial status. Second, the
candidate-bound Provider-offer policy is now a versioned contract at
`contracts/provider-offer-trust-v1.md`, so T006/T011 have an exact policy
shape and verification boundary rather than an environment-variable name only.
Third, T006's owner list now uses repository-root paths for the native
ServiceUser and pybind sources; the earlier nested `NDNSF-DistributedInference/`
prefix was invalid and could mislead implementation.

These checks remain implementation/audit evidence only. The production
ProviderOfferV3 verifier, real Y-A/Y-B/Y-N runner, live ACK-to-Response trace,
T014 convergence `PASS`, local qualification, exact-SIF replay, and Tiger jobs
are still absent. No additional task was marked complete in this audit and no
formal validation was started.

- **A180-78 (MEDIUM, resolved in documentation)** — The candidate-bound
  Provider-offer trust input was named by environment variable but lacked a
  versioned schema, ownership, and explicit verification sequence. The new
  `provider-offer-trust-v1.md` contract delegates authentication to the
  existing NDNSF Trust Schema and keeps the HMAC map fixture-only. Its presence
  does not claim verifier implementation.
- **A180-79 (HIGH, open)** — The implementation still has no production
  ProviderOfferV3 verifier that consumes the projected ACK provenance and no
  real Y-A/Y-B/Y-N MiniNDN runner. Until T006/T011 wire and execute those
  paths, T014--T020 remain blocked and `qualificationReady` must stay false.

The preceding iteration reran the strict Spec Kit structure and contract gates, the
CodeGraph freshness check, and the focused ACK-planning/provenance regressions
after the iteration-48 source change. It found one remaining production-boundary
contradiction: the V3 configuration entry point still accepted caller-supplied
`ack_coverage_roles` or `ack_coverage_predicate`, even though FR-003 requires
the registered ACK timeout to own closure. The configuration now rejects those
hooks for `DI_PLACEMENT_V3`; the V2 compatibility hook remains available. A
focused regression covers both rejection forms. The historical T001 evidence
file also predated FR-025 and reported 24 requirements; it now explicitly
identifies that count as historical, while the current gate reports 25.

The structural gate remains PASS (25 functional requirements, 9 success
criteria, 20 tasks, complete traceability), the contract gate remains
`status=PASS`, `contractReady=true`, `qualificationReady=false`, and the
focused provenance/planning slice reports 5 passed tests. The real ProviderOffer
V3 verifier, candidate-bound signed catalogue, Y-A/Y-B/Y-N runner, live
ACK-to-Response trace, T014 convergence PASS, local qualification, SIF replay,
and Tiger jobs remain open. This checkpoint does not authorize formal
validation or add a qualification result.

- **A180-75 (HIGH, resolved at configuration boundary)** — V3 callers could
  still inject an ACK-coverage predicate that could close discovery before the
  registered timeout, contradicting FR-003 and making an offline role hint an
  authority. `APPClient.configure_automatic_planning()` now fails closed when
  a V3 strategy is given either hook; the V2 compatibility path is unchanged.
  `tests/python/test_spec180_ack_provenance.py` covers both cases. The live
  NDN path must still prove that no lower-level caller bypasses this boundary.

- **A180-76 (MEDIUM, resolved as evidence clarification)** — The original
  T001 capture counted 24 requirements although FR-025 was later added. The
  capture now labels that number historical and points to this iteration's
  current 25-requirement gate result.

- **A180-77 (MEDIUM, resolved as compatibility repair)** — The built-in
  model-neutral `SequentialFixtureSplitter` emitted candidates without
  explicit input-ingress and result-egress owners. The V3 coordinator now
  requires those identities, so the existing Spec170 default-path regression
  failed before exercising its compatibility behavior. The fixture now marks
  its first and last roles as the ingress/egress owners; this is only a
  source-compatible fixture repair and is not the Spec180 YOLO adapter or
  qualification evidence.

## Iteration 48 design/code and reproducibility refresh (historical; superseded by iteration 49)

This iteration implements the narrow T006 ACK-provenance projection required
by iteration 47. `ServiceUser` now derives non-secret signer identity, typed
KeyLocator reference, and a SHA-256 digest of the complete validated ACK Data
wire from the packet that already passed the configured Trust Schema. The
native `AckSelectionCandidate`, pybind object, and Python `AckCandidate` carry
the same fields; old direct/fixture candidates remain source-compatible with
empty provenance. Focused Python regressions cover preservation, legacy
defaults, and fail-closed V3 identity/digest checks. The source build and
native/pybind import now pass. This does not claim that the production V3 offer
verifier, live ACK-to-Response runner, or trust root is complete, so T006
remains partial.

- **A180-74 (HIGH, partially repaired; production closure open)** — The
  validated ACK packet's signer provenance is now projected end-to-end without
  Python reconstruction. Remaining closure requires a production verifier that
  checks this evidence against the ACK name, ProviderOfferV3 signer key ID,
  service/request/attempt, and candidate-bound Trust Schema, plus malformed and
  mismatch negative tests on the real path.

## Iteration 47 design/code and reproducibility refresh (historical; superseded by iteration 48)

This re-audit preserved the iteration-46 gate and checked the concrete trust
boundary against the current C++/pybind path. The configured Trust Schema
validates an incoming ACK Data before `OnRequestAck` decrypts its payload, but
the current `AckCandidate` projection contains only the ACK name/message and
does not carry the validated packet signer/key-locator provenance or wire
digest. The written design was repaired to make this missing projection an
explicit T006 prerequisite. The production verifier must consume provenance
from the validated Data packet; a Python-reconstructed Provider identity or
caller HMAC map is not sufficient.

No source or behavior change was made in this audit iteration. Focused tests,
the complete Spec180 Python collection, structural gate, contract gate, and
CodeGraph freshness remain the iteration-46 evidence. This finding keeps the
verdict at **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

- **A180-74 (HIGH, open)** — `AckCandidate` drops the authenticated ACK Data
  signer provenance at the ServiceUser/pybind boundary, so the required
  Provider/service binding for a production `ProviderOfferV3` verifier cannot
  yet be exercised. T006 now owns the narrow projection change and negative
  tests. Until it is wired and exercised, the maintained HMAC callback remains
  fixture-only and no local/MiniNDN/SIF/Tiger qualification may start.

## Iteration 46 design/code and reproducibility refresh (historical, 2026-09-03)

This re-audit reran the strict Spec Kit structure check, prerequisite
resolution, CodeGraph freshness check, and the Spec180 contract gate against
the current source and feature directory. It found and corrected two stale
iteration-44 statements in `spec.md` and `plan.md` that still described the
initializer root-publisher gap in present tense, and updated the checklist and
traceability pointers to the current audit iteration. It also made the
production Provider-offer trust boundary normative: formal ACKs use the
candidate-bound NDNSF Trust Schema/Provider-identity verifier, while the
caller HMAC map remains fixture-only.

No behavior-affecting source change was made in this iteration. The iteration-
45 focused evidence remains valid: canonical-layer tests pass 11,
native-helper tests pass 3, and the related regression passes 20. The complete
Spec180 Python collection was rerun in this audit and passes 91 tests with 19
warnings in 35.91s. The structural gate remains PASS (25 FR, 9 SC, 20 tasks,
25 traced requirements); the contract
gate remains `status=PASS`, `contractReady=true`, `qualificationReady=false`.
The real YOLO runner, production offer verifier wiring, live
ACK-to-Selection-to-Provider-to-Response trace, T014 convergence PASS, local
MiniNDN cases, SIF replay, and Tiger jobs remain open. This checkpoint does
not authorize formal validation or add a qualification result.

- **A180-72 (MEDIUM, resolved in this iteration)** — Present-tense iteration-44
  wording in `spec.md` and `plan.md` contradicted the iteration-45 root
  publication repair, and the checklist/traceability still pointed at an old
  audit iteration. The affected passages now identify iteration 44 as
  historical and point to the current iteration-47 evidence.
- **A180-73 (HIGH, explicit but open)** — `ProviderOfferV3` verification was
  described only as “trust-backed” without naming the trust owner or the
  binding checks. The contracts now require the existing NDNSF Trust Schema
  and Provider identity/certificate verifier, anchored by
  `SPEC180_YOLO_OFFER_TRUST_ROOT`, with Provider/service, canonical offer
  digest, request/attempt, model/graph, validity-window, and boot-epoch
  checks. The production verifier and live trace remain T006/T009/T011 work;
  the injected HMAC callback is not qualification evidence.

## Iteration 45 design/code and reproducibility refresh (historical, 2026-09-02)

The previous iteration repaired graph/initializer transport in the Provider
bridge but identified that the root publisher could not emit the initializer
object metadata. This iteration rechecked the canonical artifact API and added
optional initializer name, raw digest, and byte-length fields to the signed root
activation path. `CanonicalCatalogEnsurer` now validates a matching payload and
publishes it before the existing layer manifests and root, preserving root-last
publication. Existing inline and source-only callers remain compatible. The
canonical-layer regression now passes 11 tests; the native-helper regression
passes 3 tests; the full Spec180 Python focused collection passes 91 tests with
19 warnings. This closes A180-71, but T007 remains partial because native /
wire parity, signed YOLO packaging, and the live runner are not complete.

- **A180-71 (HIGH, resolved in this iteration)** — The root publication path
  could not carry or publish an external initializer object even though the
  downstream assembler accepted it. `canonical_initializer_reference`, the
  optional `activate_model()` fields, and `CanonicalCatalogEnsurer` payload
  publication now bind and publish the complete tuple before the root. The
  regression is `tests/python/test_spec170_canonical_layers.py`.

## Iteration 44 design/code and reproducibility refresh (historical, 2026-09-02)

The audit followed the canonical YOLO package into the real Provider-local
assembly boundary. The exporter intentionally emits an ONNX graph plus a
separate external-initializer object, but the previous bridge fetched only the
graph and invoked `onnx.load(..., load_external_data=True)` without staging
`model.onnx.data`. That made an external-data role fail at runtime even though
inline ONNX fixtures passed. The Python assembler now accepts the initializer
bytes as a separate authenticated input, rejects graph-only assembly for an
external graph (and rejects an initializer attached to an inline graph), and
the native helper accepts only a sibling `model.onnx.data` staging file. The
C++ Provider path now reads optional signed root metadata for initializer name,
raw-object digest, and byte length, fetches and verifies that object, and passes
it to the helper. Existing inline fixtures remain source-compatible.

The focused native-helper regression now passes 3 tests, including successful
external-initializer assembly and a missing-initializer negative. The broader
Spec180 focused collection remains 91 passed with 19 warnings because the new
regression is owned by the shared Spec175 native-assembly test module. This is
implementation evidence only: the real signed YOLO package, production offer
trust verifier, Y-A/Y-B/Y-N runner, and live ACK-to-Response trace are still
absent. T007 is therefore still partial; T011/T014/T015 and all SIF/Tiger
qualification remain blocked. No qualification or performance result changes.

- **A180-70 (HIGH, repaired in this iteration)** — External initializer
  transport was underspecified and incomplete: the canonical YOLO export
  contained an external weights object, while the Provider assembler received
  graph bytes only. Graph-only assembly could not load ONNX Runtime and would
  fail late in Y-B. FR-001/FR-009, T007, and
  `contracts/yolo-minindn-runner-v1.md` now require separate name/size/raw
  digest bindings; the Python/native bridge stages and validates both objects.
  The regression is `tests/python/test_spec175_native_assembly_helper.py`.
- **A180-71 (HIGH, open)** — The native bridge can consume signed-root
  initializer metadata, but the existing `CanonicalArtifactCatalog`/
  `CanonicalCatalogEnsurer` publication path still emits only canonical-source
  metadata. A real YOLO root could therefore omit the initializer object from
  the signed publication even though the downstream fetcher supports it.
  `spec.md`, `plan.md`, T007, and the runner contract now make this an explicit
  pre-qualification requirement; T007/T011 must add and test the root-publisher
  binding before convergence.

## Iteration 43 design/code and reproducibility refresh (2026-09-02)

The reviewer-style audit rechecked the execution contract against the current
inventory builder, supervised runner, and maintained YOLO example. Two
production-boundary ambiguities were found and corrected:
T015 previously described a second formal-case execution after the inventory
run, although the five formal cases are already inventory entries. T015 now
requires exactly one supervised child per inventory item; the formal-case
oracle is recorded in that child and no duplicate pass may be used as evidence.

The current source checks remain bounded and honest: the structural Spec Kit
audit passes (25 functional requirements, 9 success criteria, 20 tasks, full
traceability), the contract gate reports `contractReady=true` and
`qualificationReady=false`, and the focused Spec180 Python collection passes
91 tests with 19 warnings. The release, inventory, and local-gate slices pass
6, 5, and 9 tests respectively. These are focused implementation checks only.
The real Y-A/Y-B/Y-N runner is still absent, so inventory materialization,
T014 convergence `PASS`, T015 local qualification, SIF replay, and Tiger jobs
remain blocked. No qualification result or performance claim is changed.
The command outputs and the missing-runner check are preserved in
`evidence/audit-iteration42-current-20260902.md`. The new runner contract
`contracts/yolo-minindn-runner-v1.md` records the required candidate-bound
environment, generated role policy, and trust boundary; it does not imply an
implementation or qualification result.

- **A180-68 (HIGH)** — The checked-in YOLO policy still declares the legacy
  `/Stage/...` deployment-first roles, while the Spec180 candidates declare
  `FullModel` or `BackboneNeck`/`DetectShard0`/`DetectShard1`/`Merge`. Reusing
  that file would execute the wrong planning path. Iteration 43 resolves this
  by making the policy offline-only and requiring T011 to generate an isolated
  candidate policy from the signed catalogue and selected plan.
- **A180-69 (HIGH)** — The maintained caller still verifies offers with a
  caller-provided HMAC key map. This is acceptable for focused fixtures but is
  not a production trust root. Iteration 43 adds an explicit offer-trust input
  to the runner contract and keeps T011/T014/T015 blocked until the
  trust-backed verifier is implemented and audited.

## Iteration 41 audit refresh (2026-09-02)

The current source and release boundary were re-inspected after the iteration
39 dispatch repairs and the T013 local-gate slice. The structural audit still passes (24 functional
requirements, 9 success criteria, 20 tasks, and complete traceability), the
Spec180 contract gate reports `contractReady=true`, and the focused Python
collection passes `91 passed, 19 warnings`. The release subset also passes with
the explicit Slurm `--export=NONE,<SPEC180_*=...>` serialization and the
delimiter mutation check; the inventory/local-gate subset passes 20 tests.

The re-inspection confirms that the fixed profile values are not silently
overridden and that `run-functional.sh` enters the pinned Apptainer 1.5.3
image with `--nv --cleanenv --pwd /bundle`; an absent in-image case runner
returns `SPEC180_CASE_RUNNER_NOT_READY` before workload execution. These are
launch-boundary checks only. The source-bound inventory generator now exists
and rejects missing, duplicate, and escaped entries. The supervised local-gate
runner validates every source digest before launching any child, snapshots the
inventory, runs one child per entry, records PID/oracle/exit/timeout and
redacted logs, rejects a non-empty output root, and binds each case ID to its
registered command arguments. The current repository
still cannot materialize a qualification inventory because the real Y-A/Y-B/Y-N
runner is absent. Q-C/Q-W now point at the maintained
`NDNSF_DI_StreamedGeneration_Minindn.py` wrapper with the required M01/M11,
seed, and fixed-contract delegation; they still have no current execution
evidence. There is still no real Y-A/Y-B/Y-N/Q-C/Q-W case run, live NDN
ACK→Selection→Provider→Response execution, native YOLO Merge numerical run,
or current MiniNDN/SIF/CUDA/Tiger qualification evidence. Therefore this
refresh closes no task: T013 remains partial because the runner is not yet
exercised against a candidate-bound inventory, and T014--T020 remain unchecked.

## Verdict meaning

After the prior corrections and the T001--T009 audit checkpoint, the
specification is bounded, internally consistent, and executable as a task plan.
No unresolved design choice
requires an implementer to invent a second planner, a new placement invariant,
an input transport, an unregistered YOLO split, role-kind semantics, or an ad
hoc Tiger experiment. Current code does not yet satisfy Spec180, so no MiniNDN,
SIF, CUDA, or Tiger result can be claimed as Spec180 qualification evidence.

T001, T003, and T012 are complete. T002 now has the generic request/input boundary,
candidate-declared V3 input endpoint, Provider-side ownership methods, and a
structured reference-aware fetch path that checks encryption, authorization
scope, size, protection epoch, and content digest; the real Python wrapper
implementation and Provider boundary now have focused tamper regressions. It
is not complete: a maintained production
caller and a real post-Selection ACK-to-result execution have not yet been
exercised. T004 has the fixed fixture, deterministic 640×640
oracle/repeat, and CPU ONNX Runtime equivalence evidence, but remains open
because its acceptance contract requires a catalogue signed with the registered
authority; iteration 17 also changed the signed candidate priorities, which
invalidates the previously recorded manifest digest until the export is
regenerated. Only a checked-in public trust root is available in this worktree.
T005 now has a real ONNX graph/candidate adapter with signature and digest
checks, and candidate ingress/egress identities are copied into the runtime
`SplitCandidate` digest. Its unsigned construction path is test-fixture-only
and is not accepted for a release candidate. T006 has a fail-closed V2 guard plus a generic
deadline-closed ACK option; T007 has role-kind-specific Python/native
validation plus a successful core-object build. The V3 coordinator now emits
one `APPLICATION_INPUT` endpoint for object-detection candidates and derives
the terminal owner from the candidate's declared egress role. The maintained
YOLO helper now resolves artifact snapshots through signed APP Data; the
caller offer-key map remains fixture material and is not qualification
authority. The live capability authority is the network
`CollaborationAckClosed` snapshot; a `PreSplitCatalogSnapshot` only resolves
already-published artifact names. Its qualification ACK window is now fixed at
1500 ms and rejects other values before request publication. T002, T004,
T006--T009 remain
open until their complete contract-specific regressions and production
caller/wire paths are recorded.
The maintained YOLO helper now publishes the encoded input through the
canonical `app_sdk.client.APPClient.publish_application_input_reference()`
method. That method delegates to the native encrypted publisher, verifies
source-bound name/object ID/size/content digest, authorization scope,
protection epoch, encryption, and publication-manifest digest, and records
non-secret `INPUT_REFERENCE_PUBLISHED` metadata. `APPClient.request_task()`
requires the resulting reference digest to match the durable publication
journal; a bare `--input-reference-file` is rejected in ACK-driven mode.
The native C++ result and Python binding expose the required metadata, and a
stale binding that returns only the legacy name/object ID fails closed.
The earlier iteration-50 Spec180 focused Python suite collected 105 tests and
passed, including
the Provider ownership regression, the maintained YOLO handler API-wiring
check, and the machine-readable Qwen reference contract. These are
implementation checks only, not Provider execution or qualification evidence.
The existing native core-object build passed 27/27 objects; it is likewise not
Provider execution or qualification evidence. The resolver tests cover only
the injected signed-APP-Data boundary; they do not prove that a live NDN
publisher has published the catalogue or that a Provider has executed a
selected role.
Spec175 has supplied the named
`LOCAL_FUNCTIONAL_PASS` baseline at source revision
`286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`, sealed by
`evidence/local-closure-current.md` and the T023 contract gate. Spec180 must
record its source delta and independently requalify the changed runtime; the
inherited pass is a prerequisite identity, not Spec180 evidence. Formal
qualification remains blocked until T004's acceptance gaps and the remaining
T005--T013 contract work are implemented, T014 reruns
this audit against the changed production paths and returns `PASS`, and T015
completes the current-candidate local gate. The historical native
`commit_plan`/repository-transfer segmentation fault remains retained as
negative evidence and does not invalidate the sealed Spec175 closure.

## Iteration 33 historical rectification

The following state is now normative for implementation and review:

| Area | What is implemented | What remains unqualified | Owner task |
|---|---|---|---|
| Generic model-first request/input boundary | V2 envelope modes, encrypted reference metadata, candidate ingress/egress projection, fail-closed Python guards, and focused tamper tests | A real signed repository fetch followed by ACK, Selection, Provider execution, and terminal Response | T002, T009, T010 |
| YOLO catalogue and adapter | Canonical graph checks, candidate-local role identities, signed-priority/digest ordering logic, and focused adapter/resolver tests | Registered signed catalogue publication and consumption by the maintained live application | T004, T005, T009 |
| ACK-driven planning | Deadline-closed option and candidate-local feasibility checks in the coordinator | Real NDN ACK closure with no early role predicate, production offer verification, and Selection trace | T006, T009, T011 |
| Role assembly | Python/native role-kind checks and a core-object native build | Native branched YOLO assembly, dependency transfer, Merge execution, and numerical oracle | T007, T008, T010 |
| Frozen Qwen reference | Machine-readable tiny CPU/MiniNDN Q-C/Q-W identity, case oracles, and drift tests | Current-candidate Q-C/Q-W execution | T012, T015 |
| Qualification harness | Contracts, candidate identity, evidence directories, source-bound inventory, and isolated local-gate supervisor | YOLO Y-A/Y-B/Y-N runner, candidate-bound inventory materialization, SIF, and Tiger jobs | T011, T013, T015--T020 |
| REPO_REF publication | Request envelope/reference guards and existing publish facade are available; focused reference-integrity tests pass | Maintained YOLO caller must publish/bind the encrypted input and record `INPUT_REFERENCE_PUBLISHED` before `REQUEST_SENT` | T002, T009, T011 |

The focused suite and the presence of planned files do not advance a task to
`[X]`. T010 is partial; T011 and T014--T020 are not started; T013 is partial,
and T002/T004--T009 remain partial until
their task-specific production paths and evidence exist. No SIF, MiniNDN,
CUDA, or Tiger result may be attached to this candidate before T014 returns a
fresh `PASS`.

## Iteration 34 implementation rectification

The iteration-33 publication finding is partially resolved in source and
focused tests. The canonical APP owner now publishes the encoded image through
the native encrypted NDNSF publisher, binds the native result to a complete DI
`LargeDataReference`, and records a non-secret `INPUT_REFERENCE_PUBLISHED`
journal entry. The native result and Python binding carry plaintext size,
content digest, publication-manifest digest, authorization scope, protection
epoch, and the encryption bit. `APPClient.request_task()` rejects a missing or
altered durable publication digest before it can publish a Request. The
maintained YOLO helper rejects the old bare reference-file input in
ACK-driven mode. The focused Spec180 suite now passes 71 tests, and the active
Python 3.8 extension import exposes all new result fields.

This does not close T002, T009, or T011. No live encrypted NDN fetch, ACK
closure, Selection, Provider execution, terminal Response, or production
trust-backed offer verification has been run. Therefore the convergence gate
remains `BLOCKED` and no MiniNDN, SIF, CUDA, or Tiger result may reuse these
focused checks as qualification evidence.

## Iteration 35 verification refresh

The complete current Spec180 collection was rerun after the source
rectification:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec180_*.py
71 passed, 19 warnings
```

The smaller publication/security regression subset also passes (25 tests).
Python syntax checks and `git diff --check` pass. These checks confirm the
current source and documentation are internally consistent; they do not add a
live NDN execution path or satisfy the T014 convergence gate. The verdict
therefore remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
BLOCKED**, with no change to the open task set.

## Iteration 36 current-source audit

The current source and documents were rechecked after the publication-boundary
and ACK-close corrections:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec180_*.py
71 passed, 19 warnings

./waf build --targets=ndn-service-framework -j2
'build' finished successfully

git diff --check
PASS
```

The source review confirms two controlling gaps remain. First,
`fetch_large_reference()` verifies the reference's size, post-decryption
content digest, encryption bit, authorization scope, and non-plaintext epoch,
but it does not yet compare that epoch with the active key/authorization epoch
maintained by the production repository/security owner. This remains the
T010/T014 protection-epoch blocker; focused metadata checks are not a live
revocation proof. Second, `ProviderOfferV3` still receives an injected
signature verifier and the maintained YOLO caller supplies its HMAC key map;
that fixture verifier is not production trust authority and cannot qualify
T009/T011. No live NDN ACK/Selection/Provider/Response path, MiniNDN case,
local-suite inventory, SIF replay, CUDA run, or Tiger job was started.

The reference contract wording was corrected in `spec.md`, `plan.md`, and
`data-model.md`: the current legacy `ciphertextDigest` wire key aliases the
SHA-256 digest of plaintext returned after authenticated decryption; it is not
an encrypted-segment wire digest. The publication-manifest digest binds that
meaning. This is a documentation correction only and does not close T002,
T009, T010, or T011.

**Iteration-36 verdict**: **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL
VALIDATION BLOCKED**. T014 remains the controlling gate; no expensive
MiniNDN/SIF/Tiger work is authorized until the live trust, epoch, and
production result-path gaps are repaired and a fresh convergence `PASS` is
recorded.

## Iteration 37 re-audit and document rectification

The current source, task graph, and contracts were rechecked after the
iteration-36 digest clarification. The structural gate remains `PASS` (24 FRs,
9 SCs, 20 sequential tasks, 3 checked tasks), and the current focused suite,
C++ framework build, and whitespace checks still pass:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec180_*.py
71 passed, 19 warnings

./waf build --targets=ndn-service-framework -j2
'build' finished successfully

git diff --check
PASS
```

One documentation defect was corrected in this iteration: the normative T002
acceptance sentence now names the maintained
`app_sdk.client.APPClient.publish_application_input_reference()` owner. The
lower `publish_large_payload_reference()` facade remains explicitly
compatibility/offline-only in the plan and contracts; it is no longer present
as an alternative in the normative task sentence. This removes a source-of-
truth ambiguity but does not change runtime behavior.

The implementation verdict is unchanged. The following controlling gaps remain:

1. `ProviderOfferV3` verification still depends on an injected caller HMAC
   key-map scaffold. No production trust-backed verifier has been exercised on
   an authenticated ACK Data path.
2. `fetch_large_reference()` verifies the reference metadata, encryption,
   authorization scope, size, and post-decryption content digest, but the
   production security owner has not yet compared the protection epoch against
   the active/revoked key epoch.
3. No real ACK-closure → Selection → Provider → terminal-Response run exists;
   the MiniNDN Y-A/Y-B/Y-N harness and isolated local-suite inventory are not
   implemented, so T014 and T015 cannot close.
4. No signed registered YOLO catalogue has been consumed by the maintained
   application, and no native branched assembly/Merge numerical run exists.
   Consequently SIF replay, CUDA, and Tiger results are absent.

These are evidence and production-wiring gaps, not reasons to broaden the
feature. T002 and T004--T009 remain partial; T013 is partial; T010--T011 and
T014--T020 remain unchecked. The fresh verdict is **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL
VALIDATION BLOCKED**. No MiniNDN, SIF, or Tiger execution is authorized until
the production gaps are repaired, T014 returns `PASS`, and the current source
is re-audited.

## Iteration 38 T013 release-boundary checkpoint

T013 now has a minimal implementation slice without any remote side effect:

- `packaging/ndnsf-di-container/jobs/spec180/profile.json` is the sole fixed
  profile source for the two functional gates (Apptainer 1.5.3, `/bundle`,
  host-NFD, ONNX Runtime, 5000/1500/60000 ms timing, and disabled admission
  control).
- `scripts/spec180_release.py` rejects duplicate/unknown/missing fields,
  profile mutations, non-absolute paths, digest mismatches, and gate-specific
  resource changes before rendering deterministic Slurm argv/environment.
  `submit()` invokes a scheduler only after that validation succeeds.
- `scripts/validate_spec180_results.py` requires candidate identity, two
  completed requests, clean child exits, protocol/result/runtime oracles,
  redaction, and cleanup to agree before accepting `PASS`.
- `submit.sh` is the only public wrapper; `run-functional.sh` fails closed
  until the T015 case runner exists, rather than pretending a scheduler smoke
  is a functional result.

Focused evidence is recorded in
`evidence/t013-release-workflow-current-20260902.md` and currently passes:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec180_release_workflow.py
5 passed
```

This advances T013 from unstarted to partial only. The source-bound complete
local-suite inventory, real Y-A/Y-B/Y-N/Q-C/Q-W runners, and T015 execution are
still absent. Because the release scripts are behavior-affecting candidate
infrastructure, the previous convergence verdict remains invalidated until
T014 rechecks these paths. The overall verdict is still **CONDITIONAL PASS FOR
IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

## Iteration 39 release-dispatch audit and rectification

The release-boundary review found two concrete mismatches between the fixed
profile contract and the checked-in launch path:

1. `scripts/spec180_release.py` rendered a bare `--export=NONE` while placing
   the candidate/run values only in the scheduler process environment. Slurm
   would therefore discard the `SPEC180_*` values before the job started. The
   renderer now emits `--export=NONE,<explicit SPEC180_* assignments>` and
   rejects comma/newline-delimited values before any scheduler call; the
   mutation/no-side-effect regression covers this boundary.
2. The Spec180 `.sbatch` path previously invoked `run-functional.sh` directly,
   and that script checked `/bundle/scripts/run_spec180_case.py` on the host.
   The wrapper now verifies the pinned Apptainer 1.5.3 executable and SIF hash,
   probes the runner inside the candidate image, and enters it with
   `exec --nv --cleanenv --pwd /bundle` and explicit read-only model/workload
   binds. If the runner is absent it returns `SPEC180_CASE_RUNNER_NOT_READY`
   without executing a workload.

The focused release suite now passes 6 tests. This repairs launch correctness,
but the source-bound inventory, real case runner, live ACK-to-Response path,
MiniNDN execution, SIF replay, CUDA run, and Tiger jobs remain absent. The
T013 task therefore remains partial, T014's convergence verdict is still not
`PASS`, and no expensive validation is authorized.

## Scope and necessity review

- Spec175 now ends at generic streamed-invocation and Qwen local closure.
- Spec180 owns the missing real-YOLO ACK-driven path and one finite cross-model
  deployment qualification.
- The design reuses `AutomaticPlanningCoordinator`; it does not add a second
  planning authority.
- The YOLO catalogue is intentionally limited to `atomic-v1` and
  `shared-backbone-two-shard-v1`; the two Detect roles partition Detect-scale
  branches and are not independent model heads.
- One Provider owns one complete role; the four-role YOLO job uses three GPU
  model roles and one explicit CPU Merge/postprocessing role, not CPU model
  layers.
- One model-neutral SIF and exactly two fixed Tiger jobs replace open-ended
  campaign or parameter-sweep work.
- Timing is diagnostic only. No performance or superiority claim is in scope.
- Trust roots are represented by one machine-readable registry with checked-in
  Ed25519 public-key files and digests. The registry is an implementation
  authority for later verification, not evidence that any model has been
  exported, staged, or qualified.

This is the smallest vertical feature that can prove both the missing YOLO
behavior and reuse of the same deployed runtime for YOLO and Qwen.

## Current code reality

| ID | Severity | Verified current behavior | Required owner | Status |
|---|---|---|---|---|
| A180-01 | HIGH | Spec175 had not produced the named current local closure baseline required by the handoff. | T001, Spec175 T020--T023, T012, T015 | RESOLVED in iteration 9: `LOCAL_FUNCTIONAL_PASS` is sealed at source revision `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7` in `evidence/local-closure-current.md` |
| A180-02 | HIGH | The generic `InferenceApplication.request(..., task=..., task_options=..., timeout_ms=...)` and `InferenceClient.request_task(...)` entry points now exist, while `InferenceClient.request_model()` remains generation-only. | T002, T009 | PARTIALLY RESOLVED in iteration 24: facade, compatibility, and maintained Provider API-wiring tests pass; real ACK-to-Provider-to-Response execution remains open |
| A180-03 | HIGH | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` invokes `distributed_inference()` or `async_distributed_inference()` with a prebuilt plan rather than submitting a Provider-free task through ACK-driven planning. | T004, T006, T009 | PARTIALLY RESOLVED in iteration 24: the maintained entrypoint uses model-first `request_task()` and the Provider handler uses the V3 input/result APIs; legacy calls remain behind explicit `--offline-oracle`, while live ACK authority and production execution evidence remain open |
| A180-04 | HIGH | `build_object_detection_adapter()` remains a generic synthetic fixture; the real YOLO exporter/adapter now emits and verifies a semantic, graph-bound catalogue, but it is not yet wired into the maintained application or native production path. | T004--T006, T009 | PARTIAL: exporter/adapter and altered-partition tests pass; application, native assembly, and full candidate acceptance remain open |
| A180-05 | HIGH | Native canonical ONNX assembly is implemented and wired, but its production path has not proved the registered YOLO fan-out/fan-in roles, plan-bound dependencies, native Merge execution, and numerical oracle. | T007--T010 | PARTIAL; focused native assembly exists, production YOLO execution remains open |
| A180-06 | HIGH | The local/MiniNDN harness and materialized candidate-bound inventory do not yet exist; the strict inventory generator, fixed profile, explicit Slurm export/SIF dispatch wrapper, terminal-result validator, contract gate, and candidate identity now exist with focused tests. | T011--T013 | PARTIAL after T001/T003; formal validation blocked |
| A180-07 | HIGH | No current candidate has passed Spec180 convergence, full local qualification, exact-SIF replay, YOLO-F, QWEN-F, or terminal closure. | T014--T020 | EXPECTED; formal validation blocked |
| A180-08 | HIGH | The legacy V2 path previously constructed `PlacementRequest.required_roles` from `candidates[0]`, allowing a heterogeneous catalogue to become list-order authority. | T006 | RESOLVED in iteration 17: V2 rejects heterogeneous role sets; V3 uses candidate-local requirements, signed priority, and candidate-digest ordering, with catalogue permutation and atomic/four-role feasibility regressions |
| A180-09 | HIGH | `RoleAssemblySpec` allowlists `COMPONENT_SET` but its common validator still requires `layer_end > layer_begin`, forcing Transformer range semantics onto YOLO graph-component roles. | T007, T010 | PARTIALLY RESOLVED in iteration 15: Python certified-recipe and native V3 JSON validation now require `layer_begin=layer_end=0` plus a non-empty canonical node set; binding/native integration regression remains |
| A180-10 | HIGH | The registered YOLO image needs a post-Selection encrypted reference/fetch boundary. | T009--T011 | PARTIALLY RESOLVED in iteration 24: `REPO_REF` carries no inline bytes and the maintained Provider handler calls the ingress-owned fetch helper; real encrypted NDN fetch and result evidence remain planned |
| A180-11 | HIGH | `RoleAssemblySpec.protection_epoch` defaults to `plaintext-v1`, and the documented assembled-role cache key omitted security domain/epoch and current-request reauthorization. | T007, T010 | PLANNED; plaintext default cannot qualify |
| A180-12 | HIGH | The coordinator can install an `AckRoleCoveragePolicy` from predeclared `ack_coverage_roles`; that would let offline role assumptions terminate discovery even though Spec180 candidates are intentionally enumerated only after ACK closure. | T006 | PARTIALLY RESOLVED in iteration 49: V3 configuration rejects caller `ack_coverage_roles` and `ack_coverage_predicate`; the lower-level/live ACK path still needs an end-to-end regression proving no bypass closes the window early |
| A180-13 | MEDIUM | The prior contract called the two Detect-scale partitions “two heads” and left the input-fetching “first role” implicit, which could allow an implementation to move input or result ownership without changing the candidate digest. | T005--T009 | RESOLVED in the revised contract: `shared-backbone-two-shard-v1` explicitly declares `BackboneNeck` as ingress and `Merge` as egress; atomic uses `FullModel` for both. |
| A180-14 | MEDIUM | ACK arrival order was not distinguished from canonical snapshot identity, allowing semantically identical snapshots to receive different digests. | T006 | RESOLVED in the revised contract: digest order is Provider/ACK identity order; arrival order remains diagnostic. |
| A180-15 | HIGH | The exact-SIF section called a small Qwen packaging smoke `Q-C`, although Q-C is the registered cross-model local cold case and the Tiger subject is Qwen3.6-27B. | T016 | RESOLVED: renamed to `Qwen-runtime-smoke`; it is explicitly non-qualifying and cannot replace Q-C/QWEN-F. |
| A180-16 | HIGH | FR-018's unconditional CUDA/no-CPU-fallback wording conflicted with the required CPU/MiniNDN reference ladder. | FR-018, T011, T015 | RESOLVED: CUDA and no-fallback are scoped to YOLO-F/QWEN-F; local CPU backend identity is recorded and is not GPU evidence. |
| A180-17 | HIGH | Two feasible candidates had no normative tie-break, so a strategy implementation could still depend on catalogue order while claiming deterministic selection. | FR-004, T006 | RESOLVED in iteration 17: the V3 coordinator orders candidates by adapter-signed priority then candidate digest; the priority/order regression passes |
| A180-18 | MEDIUM | The previous local-gate task allowed one aggregate process even though a prior complete integration run exposed cross-suite heap corruption; a crash could be misread as a feature failure or hidden by a rerun. | SC-004, T015 | RESOLVED: complete suites run one supervised child per suite; aggregate execution is diagnostic only and any child crash/hang/nonzero fails the gate. |
| A180-19 | MEDIUM | Tiger profiles named GPU indices but did not require per-process device visibility and physical-UUID proof, allowing accidental GPU sharing. | FR-018, T017--T019 | RESOLVED: profiles bind `CUDA_VISIBLE_DEVICES` per Provider and require matching physical identities with duplicate detection. |
| A180-20 | MEDIUM | T011 said “four distinct Provider roles” for all YOLO local cases even though Y-A is an atomic one-role case and Y-N should use the smallest topology that reaches its failure boundary. | T011, SC-004 | RESOLVED: Y-A, Y-B, and Y-N topology requirements are now explicit. |
| A180-21 | MEDIUM | “Complete relevant C++ and Python suites” did not identify a registered inventory, allowing an implementation to omit a selector while still claiming a complete local gate. | FR-024, T015, SC-004 | RESOLVED: the contract and T015 require an explicit source-bound inventory and one supervised child per item. |
| A180-22 | LOW | Qwen Tiger profile did not state its input transport mode, leaving the bounded prompt encoding implicit. | `contracts/tiger-profile-v1.md` | RESOLVED: QWEN-F is explicitly `INLINE`; YOLO-F remains `REPO_REF`. |
| A180-23 | HIGH | The Spec175 prerequisite previously had conflicting unsealed production-path evidence: one engineering M01 pass and a separate Artifact STORE segmentation fault after a duplicate retry. | Spec175 T022/T023, `evidence/t022-local-release-m01-pass-20260902.md`, `evidence/t022-local-release-failure-20260902-artifact-store-segv.md`, T001/T012/T015 | RESOLVED in iteration 9: the five current production-path cases and contract gate are sealed under one source identity; the fault remains negative historical evidence and is not relabelled |
| A180-24 | HIGH | Candidate priority was described as signed, but the data model and ACK contract did not identify the trusted catalogue signer or the signed field set. An implementation could therefore treat an unsigned priority or altered safe-cut/merge entry as authoritative. | `data-model.md:38-56`, `contracts/ack-driven-yolo-v1.md:110-147`, FR-002/T004--T006 | RESOLVED in iteration 6: signer key identity, covered fields, and fail-before-enumeration verification are normative |
| A180-25 | MEDIUM | `WorkloadCase` said “input bytes/digest”, which could be read as permission to copy the YOLO plaintext into plans or evidence despite the redaction requirement. | `data-model.md:153-166`, FR-022/SC-009 | RESOLVED in iteration 6: only a fixture reference/digest is recorded; bytes exist only in the controlled runner |
| A180-26 | MEDIUM | T003/T013 and T011/T012 both named shared tooling files without stating which task owns the implementation and which only consumes it, creating a review-order ambiguity. | `tasks.md:T003,T011--T013` | RESOLVED in iteration 6: T003 owns candidate identity, T012 owns the Qwen reference manifest, T013 consumes the identity and owns release orchestration |
| A180-27 | MEDIUM | The two cross-model Qwen cases were described as frozen but had no dedicated reference contract, so model/prompt/stop/backend drift could be hidden in the local-gate runner. | `tasks.md:T012`, `contracts/` | RESOLVED in iteration 6: added `contracts/qwen-reference-v1.md` with frozen identities, controls, and ownership |
| A180-28 | HIGH | The shared YOLO `Merge` role allowed an implementation to interpret “CPU ONNX Runtime” as a second model-compute path because the default merge kind and absence of model-layer objects were not explicit. | `plan.md:202-225`, `contracts/ack-driven-yolo-v1.md:119-147`, FR-018 | RESOLVED in iteration 6: default is native postprocessing; an ONNX merge graph requires an explicit manifest digest and cannot host GPU-role layers |
| A180-29 | HIGH | The catalogue verifier required a configured signer but no task explicitly owned the trust-root registration, allowing an ambient key or profile override to become the effective authority. | T001, FR-002 | RESOLVED in iteration 7: T001 owns the signer key ID, public-key digest, and signature algorithm in the contract-gate registry; runtime/profile values cannot override it |
| A180-30 | MEDIUM | The frozen Qwen contract called the Spec175 cases “accepted” while the Spec175 handoff was still unsealed, which could promote provisional evidence into a current baseline. | `contracts/qwen-reference-v1.md`, `spec.md:248`, T012 | RESOLVED in iteration 7: Q-C/Q-W are named references and become qualification inputs only after the Spec175 handoff seals them |
| A180-31 | MEDIUM | T011/T015 and T013/T017/T020 referenced the same runner, profile, and validator paths without stating which task implements versus consumes each artifact. | `tasks.md:T011,T013,T015,T017,T020` | RESOLVED in iteration 7: T011 owns the local runner, T013 owns release/profile/validator implementation, and T015/T017/T020 only invoke or consume them |
| A180-32 | MEDIUM | The catalogue signer was required but had no standalone, versioned trust-root artifact defining the public-key source, covered-field canonicalization, private-key boundary, or evidence fields. An implementer could still satisfy the prose with an ambient key or a profile override. | `contracts/catalogue-trust-root-v1.md`, FR-002, T001/T004/T005 | RESOLVED in iteration 8: T001 owns the fixed trust-root contract; export/runtime consume it and profiles/ACKs cannot replace it |
| A180-33 | MEDIUM | Q-W named continuation and mismatch behavior, while the cross-model table did not state the exact valid-turn/negative sequence; QWEN-F required a 27B identity but did not make its signed external manifest a pre-start gate. | `contracts/qwen-reference-v1.md`, `contracts/cross-model-qualification-v1.md`, FR-012, T012/T017/T019 | RESOLVED in iteration 8: Q-W is two valid turns plus one same-child mismatch rejection, and QWEN-F refuses to start without the signed content-addressed 27B manifest |
| A180-34 | MEDIUM | The design used “catalogue verification before request” and “candidate enumeration after ACK closure” but did not explicitly define whether pre-closure code could expose candidate requirements to placement. | `spec.md:193-204`, `plan.md:89-111`, FR-003/T006 | RESOLVED in iteration 8: pre-closure validation is opaque-revision integrity only; candidate records and requirements remain unavailable until `ACK_CLOSED` |
| A180-35 | MEDIUM | Several task paths were abbreviated relative to the repository root (`app_sdk/...`, `cpp/ndnsf-di/...`, and `provider.py`). An implementer could modify the wrong package or omit the actual `NDNSF-DistributedInference` owner while still satisfying the prose. | `tasks.md:T002,T006,T007` | RESOLVED in iteration 9: all source paths in those tasks are repository-root-qualified |
| A180-36 | MEDIUM | QWEN-F was required to be signed, but the documents did not identify a distinct trusted authority or canonical covered-field set for external model manifests. The YOLO catalogue key could therefore be reused implicitly or a profile key could become authority. | `spec.md:FR-012`, `contracts/qwen-reference-v1.md`, `contracts/cross-model-qualification-v1.md` | RESOLVED in iteration 9: added `contracts/model-manifest-trust-v1.md`; T001/T012/T017/T019 now verify it before staging/workload entry |
| A180-37 | MEDIUM | The Tiger entrypoint had a positional shape but did not define the accepted gate/profile/run-record schema, canonical hashing, or when the wrapper may invoke `sbatch`; this left the no-side-effect C2 boundary partly implicit. | `contracts/tiger-profile-v1.md`, `quickstart.md`, T013/T017 | RESOLVED in iteration 9: the profile contract fixes the two gate values, JSON ownership, field allowlist, hashing, and pre-`sbatch` comparison |
| A180-38 | MEDIUM | T001 required configured trust roots and existing owners but did not name a machine-readable registry or distinguish existing prerequisite paths from future implementation paths. | `tasks.md:T001`, `contracts/*-trust-root-v1.md`, `plan.md:Project Structure` | RESOLVED in iteration 10: `contracts/trust-root-registry-v1.json` is the sole registration record, empty values block T001/formal gates, and the owner map distinguishes `existing` from `planned` paths |
| A180-40 | HIGH | T004 was previously marked complete although its acceptance requires a fixed licensed fixture, a 640×640 CPU oracle/repeat, and a catalogue signature produced by the registered authority. | `tasks.md:T004`, `evidence/t004-yolo-export-current-20260902.md`, `contracts/trust-root-registry-v1.json` | PARTIALLY RESOLVED in iteration 15: fixture, 640×640 repeat, canonical NMS policy, and ORT equivalence now pass; authority private signing remains intentionally external and open |
| A180-41 | MEDIUM | The new YOLO adapter initially used an arbitrary graph-node prefix for signed safe cuts and did not bind the catalogue model/graph digests to the package manifest. | `adapters/yolo/adapter.py`, `tools/ndnsf-di/export_spec180_yolo26_onnx.py`, `tests/python/test_spec180_yolo_adapter.py` | RESOLVED in iteration 15: deterministic partition boundaries are the only signed cuts; graph/model digest binding and all-consumer edge checks are enforced |
| A180-42 | HIGH | Raw PyTorch and ONNX Runtime YOLO NMS outputs differed on low-confidence rows at the 300-row cap, making an unqualified full-array oracle invalid. | `tools/ndnsf-di/export_spec180_yolo26_onnx.py`, `tests/python/test_spec180_yolo_export.py`, `evidence/t004-yolo-export-current-20260902.md` | RESOLVED in iteration 15: the manifest declares a 1e-3 confidence floor and deterministic row ordering; canonicalized 640×640 arrays are ORT-equivalent within tolerance |
| A180-43 | HIGH | The normative contract named the shared Detect roles `DetectShard0/1`, while the exporter, adapter, and focused tests used `DetectHead0/1`; the role identity was therefore not stable across catalogue, planner, and Tiger profile documents. | `spec.md:193`, `contracts/ack-driven-yolo-v1.md:131`, `tools/ndnsf-di/export_spec180_yolo26_onnx.py:274`, `adapters/yolo/candidates.py:59` | RESOLVED in iteration 18: exporter, verifier, and tests use the canonical `DetectShard0/1` names; a repository-wide scan finds no `DetectHead*` references |
| A180-44 | HIGH | The V3 role-kind heuristic treated any name containing `shard` as `TENSOR_RANK`, so the corrected YOLO `DetectShard*` component roles would be rejected by the ordinary V3 validator even though only explicit stage/tensor/rank namespaces denote tensor ranks. | `app_sdk/placement.py:_v3_role_kind`, `app_sdk/placement.py:_v3_role_specs`, `tests/python/test_spec180_yolo_ack_planning.py` | RESOLVED in iteration 18: explicit stage/tensor/rank markers remain tensor-rank; bare YOLO DetectShard roles are `COMPONENT_SET`; regression covers both cases |
| A180-45 | MEDIUM | Catalogue verification converted `selectionPriority` to an integer but accepted negative values until candidate construction, weakening the fail-before-enumeration boundary for malformed signed metadata. | `adapters/yolo/candidates.py:_verify_candidate`, `tests/python/test_spec180_yolo_ack_planning.py` | RESOLVED in iteration 18: verifier rejects negative priority before returning any candidate; focused negative-catalogue test passes |
| A180-46 | HIGH | The document/trust-root contract gate emitted `qualificationReady=true` while T004--T020 and the convergence audit were still open; a release wrapper could mistake a structural PASS for formal qualification readiness. | `scripts/spec180_contract_gate.py:run_gate`, `evidence/t001-contract-gate-current-20260902.json`, `tests/python/test_spec180_contract_gate.py` | RESOLVED in iteration 18: the gate now emits `contractReady=true`, `qualificationReady=false` until all tasks and a fresh audit PASS exist, and labels the readiness scope explicitly |
| A180-47 | HIGH | T002 was marked complete even though its acceptance explicitly requires post-Selection ingress-only input fetch/decrypt, terminal result-egress ownership, and a production caller. | `tasks.md:T002`, `app_sdk/application.py:202`, `app_sdk/client.py:1539`, `app_sdk/placement.py:2997-3458`, `provider.py:715-860`, `examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py:handle_role` | PARTIALLY RESOLVED in iteration 24: V3 projects the candidate-declared `APPLICATION_INPUT` endpoint and terminal owner; the Provider handler now calls the guarded fetch/result methods. A real ACK-to-Provider-to-result run is still required |
| A180-48 | HIGH | The new YOLO model-first helper could be mistaken for a qualified runtime path even though it requires externally supplied signed package/input/catalogue artifacts and has no production ACK-to-Merge result evidence. | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py:_load_yolo_ack_driven`, `tests/python/test_spec180_yolo_application.py`, `evidence/t009-yolo-application-current-20260902.md` | RESOLVED in iteration 20: the helper fails closed when required identities are absent, the legacy path is explicitly offline-only, and T009 remains unchecked until the real Provider path and result oracle pass |
| A180-49 | HIGH | The ACK-driven contract example used `user.request_model(...)`, but the current generic YOLO implementation exposes `InferenceClient.request_task(...)`; `request_model()` remains restricted to `GenerationInput`/`GenerationConfig`. An implementer could therefore copy an invalid API and bypass the intended generic boundary. | `contracts/ack-driven-yolo-v1.md:22`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py:1539-1557`, `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py:46` | RESOLVED in iteration 21: the contract, spec, plan, quickstart, and T002 now name the real generic signatures and explicitly reserve `request_model()` for Qwen convenience calls |
| A180-50 | HIGH | The registered YOLO candidate carried `input_ingress_role` and `result_egress_role`, but the runtime candidate initially dropped them. | `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/candidates.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/adapter.py:87-137`, `NDNSF-DistributedInference/ndnsf_distributed_inference/splitter.py:341-500`, `tests/python/test_spec180_yolo_adapter.py` | RESOLVED in iteration 23: `SplitCandidate` validates the paired roles and includes them in its canonical digest; the adapter copies both fields and the regression mutates egress to prove digest invalidation |
| A180-51 | HIGH | The maintained YOLO helper still uses a caller-supplied HMAC key map for Provider-offer verification. That is a fixture scaffold, not the production trust-backed verifier required by FR-003/FR-004. | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`, `app_sdk/placement.py`, `specs/180-ack-driven-cross-model-qualification/contracts/tiger-profile-v1.md` | PARTIALLY RESOLVED in iteration 30: file-backed catalogue snapshots were removed from the maintained path and replaced with exact-name signed APP Data resolution; T009 still must bind offer verification to production trust and exercise ACK-to-Response. The CLI defaults to and rejects values other than the registered 1500 ms ACK window. |
| A180-52 | HIGH | The new ownership methods and input endpoint are only safe if V3 Provider construction derives flags from the signed local `RoleDataflowContract`; a default-true or caller-provided flag would allow a non-owner to fetch or publish. | `provider.py:2000-2015,2274-2305`, `app_sdk/placement.py:3330-3460`, `tests/python/test_spec180_generic_request_api.py` | PARTIALLY RESOLVED in iteration 23: flags are derived from decoded V3 dataflow and enforcement is enabled only for V3; focused tests cover owner/non-owner and REPO_REF size failure. Native/wire ACK-to-result execution remains open |
| A180-53 | MEDIUM | The V3 ownership patch was initially inserted into the legacy `add_role()` wrapper, which has no V3 projection and would raise `NameError` when that compatibility path executed. | `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py:add_role`, `tests/python/test_spec180_generic_request_api.py:test_legacy_add_role_wrapper_has_no_v3_projection_dependency` | RESOLVED in iteration 24: legacy `add_role()` no longer references V3-only state; a regression invokes the registered wrapper and confirms the handler context is delivered. |
| A180-54 | HIGH | `ProviderRuntimeContext.fetch_application_input()` previously called `fetch_large(dataName, authorizationScope, timeoutMs)` and checked only the returned byte count. | `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py:766-815`, `pythonWrapper/ndnsf/service.py:1260-1318`, `tests/python/test_spec180_generic_request_api.py:230-335`, `tests/python/test_spec180_yolo_security.py:1-141` | PARTIALLY RESOLVED in iterations 26--29: the boundary now requires structured `fetch_large_reference()` support and verifies encryption, matching authorization scope, size, non-plaintext protection epoch, and content digest; direct wrapper, name/size-only, missing-epoch, and tampered-content regressions pass. Full T002/T010 acceptance still requires a real signed NDN repository fetch and production ACK-to-Provider result path. |
| A180-55 | MEDIUM | Several artifacts called the artifact snapshot a “network ACK snapshot,” which blurs two different authorities: network ACK closure supplies request-scoped Provider capabilities, while a pre-split snapshot only names already-published canonical role artifacts. This wording could lead an implementer to let static artifact metadata close discovery or choose a candidate. | `spec.md:35-45`, `plan.md:105-120`, `quickstart.md:27-42`, `contracts/ack-driven-yolo-v1.md:56-65`, `tasks.md:T009` | RESOLVED in iteration 25: documents now name `CollaborationAckClosed` as the live capability authority and require authenticated repository/data resolution for artifact snapshots; no static file may close ACK collection or choose a candidate. |

| A180-56 | HIGH | The Provider wrapper passed V3 ownership flags to every handler, but the flags were undefined for ordinary V2 handlers, causing `NameError` before legacy execution. | `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py:2040-2095,2360-2385`, `tests/python/test_spec168_provider_generation.py:720-790` | RESOLVED in iteration 29: flags default to disabled for legacy paths and derive only from the validated V3 `RoleDataflowContract`; the 20-case Provider-generation compatibility suite passes. |
| A180-57 | HIGH | The maintained YOLO helper still loaded active catalogue snapshots from a caller file, so an operator could accidentally let stale local metadata become the artifact-publication input even though ACK capability remained network-owned. | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`, `app_sdk/placement.py:1434-1515`, `tests/python/test_spec180_catalog_resolver.py` | PARTIALLY RESOLVED in iteration 30: `NetworkCatalogSnapshotResolver` now fetches an exact-name, signer-checked APP Data record after `CollaborationAckClosed` and verifies a canonical snapshot digest; the file is offline-oracle-only. The caller HMAC key-map offer verifier and production ACK-to-Response trace remain open under T009/T011. |
| A180-58 | HIGH | The frozen Qwen references did not machine-bind the local model/workload identity, allowing the 27B Tiger workload (64-token/120-second controls) to be mistaken for the Spec175 local Q-C/Q-W fixture (8-token/60-second controls). | `contracts/qwen-reference-manifest-v1.json`, `contracts/qwen-reference-v1.md`, `packaging/ndnsf-di-container/jobs/spec175/workload.json`, T012 | RESOLVED in iteration 32: the manifest binds the Spec175 tiny CPU/MiniNDN fixture digest, M01/M11 case oracles, and 5000/1500/60000 ms controls; focused mutation/separation tests pass. T015 still owns execution and cannot inherit the Spec175 result. |
| A180-59 | HIGH | The maintained ACK-driven YOLO helper previously read `--input-reference-file` and built a `REPO_REF` without publication; a reference file alone could not prove object publication or request binding. | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py:46-130`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py:338-390`, FR-003/FR-022, T002/T009/T011 | PARTIALLY RESOLVED in iteration 34: the maintained path now publishes source bytes through `APPClient.publish_application_input_reference()`, records `INPUT_REFERENCE_PUBLISHED`, and rejects the bare file. Live NDN ACK/Selection/Provider/Response execution and production offer verification remain open. |
| A180-60 | HIGH | The existing publication facade's legacy wire result did not include the complete Spec180 `LargeDataReference` fields (`manifestDigest`, `authorizationScope`, `protectionEpoch`), so it could not be passed directly to `ApplicationInput.from_repo_ref()` without a trusted DI binding. | `pythonWrapper/ndnsf/service.py:2957-2972`, `pythonWrapper/src/ndnsf/_ndnsf.cpp:539-557,7166-7177`, `ndn-service-framework/ServiceUser.hpp:196-208`, `NDNSF-DistributedInference/ndnsf_distributed_inference/repo_reference.py:26-205`, FR-022, T002/T009 | PARTIALLY RESOLVED in iteration 34: native publication metadata, the Python binding, and the fail-closed DI binder are implemented; focused binding tests and a real Python 3.8 extension import probe pass. A live encrypted fetch must still prove the fields on the production wire path. |
| A180-61 | HIGH | The release renderer used a bare `--export=NONE`, so the `SPEC180_*` candidate/run identity was present only in the submitter environment and would be dropped by Slurm before job start. | `scripts/spec180_release.py:333-370`, `tests/python/test_spec180_release_workflow.py` | RESOLVED in iteration 39: the renderer serializes only validated `SPEC180_*` values as `--export=NONE,<explicit assignments>` and rejects comma/newline delimiters before scheduler invocation. |
| A180-62 | HIGH | The Spec180 `.sbatch` path invoked the functional script on the host and its runner probe checked `/bundle` outside the SIF, so the fixed Apptainer/runtime contract was not actually enforced. | `packaging/ndnsf-di-container/jobs/spec180/*.sbatch`, `packaging/ndnsf-di-container/jobs/spec180/run-functional.sh` | RESOLVED in iteration 39: the wrapper verifies Apptainer 1.5.3 and the SIF digest, probes the runner inside the image, and executes with `--nv --cleanenv --pwd /bundle`; it remains fail-closed until the runner is implemented. |
| A180-63 | HIGH | The first local inventory declaration invoked `NDNSF_DI_LlmPipeline_Minindn.py --spec175-case ...` directly, omitting the required tiny-ONNX, V3, four-stage, positive-seed, and 5-second-settle contract; both Qwen cases would fail argument validation before MiniNDN startup. | `scripts/spec180_inventory.py`, `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`, `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` | RESOLVED in iteration 41: Q-C/Q-W use the maintained wrapper with registered M01/M11 and seed `1750001`; the wrapper delegates the complete fixed command and runner-owned output directory. |
| A180-64 | HIGH | The initial local supervisor did not bind command shape to the inventory source, preserve a pre-execution inventory snapshot, or reject stale evidence roots; command substitution or mixed records could therefore be mistaken for a qualification run. | `scripts/run_spec180_local_gate.py`, `contracts/local-suite-inventory-v1.md`, `tests/python/test_spec180_local_gate.py` | RESOLVED in iteration 41: command/path/argument shapes, source digests, fresh output roots, per-child PID/oracle/exit/timeout/cleanup, and redaction (including assignment values) are enforced before any child starts; the real case runner and candidate execution remain open. |
| A180-65 | MEDIUM | T015 said to execute every inventory item and then execute each formal case again, creating duplicate workload runs and ambiguous evidence ownership. | `tasks.md:T015`, `contracts/local-suite-inventory-v1.md`, `scripts/run_spec180_local_gate.py` | RESOLVED in iteration 42: each formal case is one inventory entry and runs exactly once in its supervised child; no second formal-case pass is permitted. |
| A180-66 | LOW | `spec.md` had moved to the iteration-42 status while `plan.md`, `tasks.md`, and two implementation evidence notes still presented iteration-41 or the historical 71-test count without a current/historical qualifier, which could make the checkpoint look inconsistent. | `spec.md`, `plan.md`, `tasks.md`, `evidence/t002-input-publication-implementation-current-20260902.md`, `evidence/t009-yolo-application-current-20260902.md` | RESOLVED in iteration 42: status/plan/task headers now agree, and 71-test references are explicitly historical; the current focused collection remains 91 tests. |
| A180-67 | MEDIUM | The current status summary called T010 “not started” even though its focused security-boundary implementation and 19 regression tests already exist; this understated partial implementation while the native/wire and full negative matrix remained open. | `spec.md`, `plan.md`, `tasks.md`, `audit.md`, `evidence/t010-yolo-security-current-20260902.md` | RESOLVED in iteration 42: T010 is recorded as partial, while its remaining production/native/replay/deadline/redaction evidence stays explicitly open. |
| A180-75 | HIGH | The V3 configuration entry point accepted caller ACK-coverage hooks even though FR-003 requires the registered ACK timeout to own discovery closure. | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, `tests/python/test_spec180_ack_provenance.py`, FR-003/T006 | RESOLVED at the configuration boundary in iteration 49: both hooks now fail closed for V3; a live lower-level-path regression remains required. |
| A180-76 | MEDIUM | The original T001 evidence capture reported 24 requirements although FR-025 was subsequently added, leaving a misleading “current” count. | `evidence/t001-contract-gate-current-20260902.md`, `audit.md`, `spec.md`, `traceability.md` | RESOLVED in iteration 49: the original count is labelled historical and the current audit records the 25-requirement gate result. |
| A180-77 | MEDIUM | The model-neutral built-in sequential fixture emitted V3 candidates without explicit ingress/egress ownership, so existing Spec170 compatibility cases failed during the stricter coordinator validation. | `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/builtin.py`, `tests/python/test_spec170_default_application_path.py` | RESOLVED in iteration 49: the fixture declares first-role ingress and last-role egress; this preserves legacy tests only and cannot supply Spec180 YOLO qualification evidence. |

Verified implementation anchors:

- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py:1607`
  owns `AutomaticPlanningCoordinator`; its request path begins at line 1985.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py:202`
  owns the current high-level request facade.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py:1528`
  owns the current model request path.
- `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py:46` owns the
  maintained model-first helper; it now resolves catalogue metadata from a
  signed APP Data record with `NetworkCatalogSnapshotResolver`, while the
  historical caller HMAC key map remains a temporary fixture scaffold. The
  maintained path instead loads the candidate-bound Ed25519 policy/PEM map and
  still requires the native Trust-Schema callback to close the live gap. The
  CLI default and fail-closed guard use the registered 1500 ms ACK window. The
  retained preplanned calls remain below the explicit `--offline-oracle`
  branch.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/builtin.py:342`
  owns the current synthetic object-detection adapter.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py:2185`
  contains the guarded V2 `required_roles=candidate_role_sets[0]` path (mixed
  role sets fail closed); `_encode_request()` now emits conditional INLINE
  base64 or metadata-only `REPO_REF` fields for the generic envelope.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py:78`
  owns `ApplicationInput`; `repo_reference.py:26` exposes the reusable
  repository large-data reference conversion.
- `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:1292`
  owns `RoleAssemblySpec`; its current default remains `plaintext-v1`, so the
  protected-role requirement is still an explicit T010/T014 blocker.
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`
  and `NativeProviderHandler.cpp` own the reusable native assembly boundary.

CodeGraph was current at audit time and confirmed these symbols and production
relationships; exact source inspection supplied the cited file locations.

## Cross-artifact analysis

| Check | Result |
|---|---|
| Required artifacts | PASS: spec, plan, research, data model, contracts (including catalogue, external model-manifest, local-suite inventory, and machine-readable trust-root registry), quickstart, tasks, traceability |
| Requirement IDs | PASS: 25 unique FRs and 9 unique SCs |
| Task IDs | PASS: 20 unique sequential tasks, T001--T020 |
| Requirement coverage | PASS: every FR and SC has an owner task and evidence route |
| Task coverage | PASS: every task appears in traceability |
| User-story coverage | PASS: all four stories have independently testable checkpoints |
| Placeholders | PASS: no unresolved template or clarification marker |
| Dependency order | PASS: focused implementation precedes convergence; complete local validation precedes SIF; SIF precedes remote mutation |
| Constitution alignment | PASS: design-code convergence, immutable candidate, no-side-effect mutation tests, single subject, and candidate-bound evidence are explicit |
| Candidate ingress/egress | CONDITIONAL: the candidate digest and V3 `APPLICATION_INPUT` endpoint now carry the signed pair, and Provider flags are derived from the local projection; T002/T006/T009 still require production ACK-to-result and live-authority evidence, while T002/T010 additionally require reference-aware digest verification |
| Candidate choice | PASS: feasibility is candidate-local; signed priority and digest tie-break remove catalogue-order ambiguity |
| Backend scope | PASS: local CPU reference evidence is distinct from Tiger CUDA/no-fallback evidence |
| Suite isolation | PASS: the formal local gate requires supervised process-isolated suite children |
| Exact-SIF scope | PASS: YOLO Y-B and `Qwen-runtime-smoke` are the only exact-SIF smoke inputs; the packaging smoke cannot satisfy Q-C/Q-W or QWEN-F |
| Catalogue trust | PASS: signer identity and covered fields are explicit; unsigned or unknown-key candidates fail before enumeration |
| Merge ownership | PASS: native postprocessing is the default; an optional ONNX merge graph is explicit and cannot replace model roles |
| Task ownership | PASS: shared files are either implementation-owned or explicitly consumed by the dependent task |
| Frozen Qwen reference | PASS for registration: Q-C/Q-W tiny CPU/MiniNDN identity, fixture digest, case oracles, and controls are machine-bound in `qwen-reference-manifest-v1.json`; execution remains T015 |
| Catalogue trust-root ownership | PASS for design: T001 must populate the signer identity in `trust-root-registry-v1.json`; T004/T005 consume it without ambient overrides |
| Catalogue trust-root artifact | PASS: the versioned contract fixes key source, covered fields, private-key boundary, and evidence identity |
| Qwen-F input closure | PASS: the signed external 27B manifest is distinct from local fixtures and packaging smoke and is checked before staging |
| External model-manifest trust | PASS for design: the registered authority, covered fields, private-key boundary, and pre-staging verification are explicit; T001 must populate the registry before its gate can pass |
| Task path closure | PASS: implementation paths are repository-root-qualified for the current package layout |
| Submit-interface closure | PASS: gate/profile/run-record ownership, canonical hashing, and the no-side-effect pre-`sbatch` boundary are explicit |
| Spec175 handoff status | PASS: Q-C/Q-W are registered from the sealed `LOCAL_FUNCTIONAL_PASS` handoff; inherited results remain prerequisite identity only, and T015 must execute the current Spec180 candidate |
| T001 owner/path semantics | PASS: the owner map resolves existing prerequisite/evidence paths and lists future implementation paths only as explicit `planned` entries; the configured registry and public-key digests are checked by `scripts/spec180_contract_gate.py` |
| Generic API signature | PASS for design/code surface: the documented YOLO call matches `InferenceClient.request_task()` and the generic application facade; Qwen-only `request_model()` is clearly separated, but this does not prove ACK-to-Provider execution |
| Maintained YOLO discovery authority | CONDITIONAL: capability authority is the network `CollaborationAckClosed` snapshot; artifact metadata now resolves through exact-name signed APP Data and a digest-bound `NetworkCatalogSnapshotResolver`; the candidate-bound `ProviderOfferTrustVerifier` and native ACK provenance projection have focused implementation evidence. Live certificate-chain callback and production ACK-to-Response execution remain open under T009/T011 |
| Input reference integrity | CONDITIONAL: the maintained APP owner now performs the trusted DI binding and durable publication-event check; the Provider helper and Python wrapper reject name/size-only fetch and verify structured reference encryption, scope, size, epoch, manifest, and content digest. A real signed NDN repository fetch and end-to-end production result remain required by T002/T009/T010/T011 |
| Qualification timeout binding | PARTIAL: the maintained YOLO CLI defaults to and enforces 1500 ms; release/profile validation and a real request trace remain |
| Local-suite inventory | CONDITIONAL: `contracts/local-suite-inventory-v1.md`, `scripts/spec180_inventory.py`, and `scripts/run_spec180_local_gate.py` define/validate source-bound entries and supervised child records; T011/T015 still must provide the real case runner and materialize/exercise a candidate-bound inventory |

## Task-fragmentation and readiness

The task split is acceptable for the current dependency gates. T002/T009/T010
form one request-to-result boundary, T004/T005/T006 form one signed-catalogue
and ACK-planning boundary, and T007/T008/T010 form one native assembly/merge
boundary. They remain separate because each group has a different owner,
focused regression, and failure gate. T011/T013/T015--T020 are intentionally
serial qualification stages; merging them would hide the first failing layer
and weaken the immutable-candidate stop rules. No mechanical duplicate task was
found, but T013 and T015 must continue to keep implementation ownership of the
runner/inventory distinct from execution of that inventory.

| Readiness dimension | Result | Evidence or blocker |
|---|---|---|
| Intent and scope | PASS | Spec175 handoff, two YOLO candidates, frozen Qwen reference, finite Tiger jobs |
| Requirements and traceability | PASS | 25 FRs, 9 SCs, 20 owner-mapped tasks |
| Security and ownership | CONDITIONAL | Python guards focused-tested; native/wire and live trust paths remain |
| Production wiring | BLOCKED | No live ACK-to-Selection-to-Provider-to-Response run |
| Local qualification | BLOCKED | The real T011 Y-A/Y-B/Y-N case driver and candidate-bound inventory execution are not implemented; preflight and supervision slices exist |
| SIF/Tiger qualification | BLOCKED | T014 PASS, T015 local PASS, and signed external artifacts are prerequisites |

The overall gate therefore remains `CONDITIONAL PASS FOR IMPLEMENTATION;
FORMAL VALIDATION BLOCKED`, not `PASS`.

The structural audit passes for both revised Spec175 and new Spec180. Iteration
23 corrected the runtime ownership gap: candidate ingress/egress now bind the
candidate digest, V3 dataflow contains one `APPLICATION_INPUT` endpoint and
declares the terminal owner, and Provider contexts enforce those declarations.
Iteration 24 additionally routed the maintained YOLO handler through the
ownership APIs and protected the legacy `add_role()` wrapper from V3-only state.
Iteration 25 corrected the authority vocabulary and made the input-integrity
gap explicit: `CollaborationAckClosed` is the only live capability authority,
`PreSplitCatalogSnapshot` is artifact metadata, and size-only repository fetch
is not verification. Iteration 26 moved the Provider boundary to structured
reference-aware fetching and added digest/scope/tamper regressions. Iteration
27 added direct coverage of the Python wrapper reference verifier and the
single-use terminal response guard. Iteration 28 bound a non-plaintext
protection epoch into `LargeDataReference`, request-envelope validation, and
the same focused regressions. Live ACK
wiring, signed NDN repository execution, native/wire
execution, and the local inventory runner remain open implementation work;
timeout binding has a focused source-contract check but no production trace. No
qualification evidence is inferred from focused unit tests.
Iteration 29 repaired a V2 compatibility regression in the Provider wrapper:
legacy handlers now receive ownership flags initialized to disabled, and those
flags are enabled only after a validated V3 `RoleDataflowContract` is decoded.
The 20-case Provider-generation compatibility suite passes after this repair;
this does not constitute production ACK-to-Response evidence.
Iteration 30 removed the maintained helper's file-backed catalogue authority:
the active snapshot is now fetched as signed APP Data, checked for exact name,
canonical digest, and duplicate aliases/manifests, and only called after ACK
closure. Three resolver regressions and the existing application checks pass;
the offer-key map and real production request remain qualification blockers.
Iteration 31 corrected the test-count and task-state statements, explicitly
separated injected resolver tests from live NDN evidence, and recorded the
remaining production, native, local-gate, SIF, and Tiger blockers in one
rectification table. These edits are documentation-only and do not advance any
task or invalidate the current candidate identity.
Iteration 33 corrected the REPO_REF publication boundary: the maintained
caller must publish through the existing encrypted large-payload facade (or
consume its source-bound receipt) before `REQUEST_SENT`; a caller reference
file alone is not evidence. The new finding is recorded as A180-59 and in
`evidence/t002-input-publication-audit-current-20260902.md`; no qualification
run was started because this production-path gap is still open.
Requirement
IDs use the repository's machine-readable form so later audits cannot silently
report zero requirements. The added FR-024 closes the local-gate inventory
ambiguity without claiming that any implementation or qualification evidence
already exists. Iterations 7 through 10 additionally close the trust-root
ownership, catalogue lifecycle, provisional Qwen handoff wording, Qwen-F
manifest gate, distinct external model-manifest authority, repository-root task
paths, canonical submit-interface semantics, and implementation-versus-consumer
ownership ambiguities. Iteration 15 additionally corrected the YOLO
oracle/NMS evidence boundary and role-kind validation wording. Iteration 17
corrected the production V3 candidate ordering to honor signed adapter
priority and removed catalogue list order as a decision input. Iteration 18
aligned the canonical DetectShard role names, corrected the V3 role-kind
classification so graph shards are not confused with tensor ranks, and moved
negative-priority rejection into catalogue verification. These
document-level results do not upgrade the focused
T004--T007 implementation evidence to production wiring or qualification.
The YOLO exporter/adapter, role-kind guards, and maintained handler API wiring
now have focused implementation evidence only; no real Provider execution,
ACK-driven application result, or Spec180 qualification evidence exists yet.
Iteration 71 replaces the former percentage/index cut with a signed semantic
partition certificate and graph revalidation; this strengthens T004/T005 but
does not close their native assembly or maintained-application obligations.

## Security, migration, and evidence boundaries

- Existing NDNSF authentication, authorization, confidentiality, tokens,
  signatures, and replay protection are retained. The registered YOLO input is
  an encrypted repository object referenced by Request and fetched only by the
  candidate-declared input-ingress role under assignment-scoped authorization;
  T002/T007/T010 add negative tests at input, assembly,
  cache, dependency, protection-epoch, and plaintext-log boundaries.
- Existing Qwen generation calls remain source-compatible through T002, but
  must adapt to the same generic coordinator rather than retain another owner.
- The old YOLO preplanned path may remain only as a clearly named offline
  oracle. It cannot be the maintained application or qualification path.
- Spec175 PASS names the inherited baseline. Every Spec180 source delta is
  recorded and revalidated by Spec180; evidence is never relabeled across
  source identities.
- Any behavior, dependency, model, SIF, profile, launcher, workload, or oracle
  change applies the invalidation matrix before expensive work resumes.

## Conditions for the mandatory post-implementation PASS

1. Preserve the sealed Spec175 `LOCAL_FUNCTIONAL_PASS` baseline identity already
   imported by T012's machine-readable reference manifest.
2. Close T004's remaining registered-signing gap, then complete T006--T013 with focused red/green evidence, including A180-08--12, A180-51--52, A180-54, and A180-59--60: live ACK authority, authenticated artifact resolution, fixed timeout binding, pre-request encrypted input publication, a live wire proof of the trusted DI reference binding, reference-aware input verification, native ownership enforcement, and the production YOLO caller/Provider path.
3. Reinspect all real entry points, callers, runtime/security owners, harnesses,
   and evidence producers after the final behavior change.
4. Repair every controlling discrepancy and rerun its focused regression.
5. Update this file with a fresh T014 `PASS` before running T015.
6. Treat any later behavior-affecting or candidate-plane change as invalidating
   that verdict.

Until those conditions hold, the only accurate project statement is:
`Spec180 T001, T003, and T012 are complete; T002 and T004--T010 have focused
implementation evidence but remain partial; T011 and T013 have preflight or
supervision slices but remain partial; T014--T018 and T020 remain open, while
T019 is complete as a scope-transfer task, and the feature is not yet
converged or qualified. The current-source T011 Y-A record is focused wiring
evidence only; T015--T020 still require the T014-accepted candidate route.`
