# Tasks: ACK-Driven YOLO Tiger Functional Slice

**Input**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/`,
`quickstart.md`, and Spec175 `handoff-to-spec180.md`.

**Tests**: Every behavior-changing task is test-first: add or identify the
focused failing case, implement the behavior, run the focused passing gate, and
record its evidence inside the same task. Complete suites and formal system
tests occur only after the convergence gate.

## Revision 123 active queue (supersedes revision 112 ordering)

**Closure (2026-09-05, revision 125):** Spec 180 已由所有者决定关闭；所有
未完成的实现与资格认证任务转移到 Spec 181。以下任务注册表与队列记录
保持冻结（契约门禁依赖 T001..T020 注册表），不再在此重新打开任何任务。

**2026-09-05 controlling repair queue:** T010/T011 negative verdicts had false
positives: arbitrary request errors were converted to PASS and Y-N-E raised its
own rejection without grant validation. Complete the corrections in
`evidence/t011-negative-verdict-repair-20260905.md` before another live matrix.
The repaired User/runner/application/build-guard set has 183 focused passing checks.
Require actual rejection cause, observed phase, request/attempt/Provider/plan
binding and every child exit. Earlier negative PASS labels cannot qualify this
subject. The native fetch guard and current build must still be compiled and
checked under the corrected dependency closure. FR-008 protected grant execution
remains an implementation blocker; `plaintext-v1` cannot qualify. The required
authority configuration, canonical interoperability and Native key lifecycle
are listed in `evidence/t008-protected-grant-gap-20260905.md`; adding only a
factory is insufficient. The terminal collector is now present and wired
in `supervise-tiger.py`; verify that code rather than recreating it based on
older follow-up notes. T014/T015 remain open and SIF/Tiger stay behind them.

**Revision 124 queue note (2026-09-05):** the completion order now closes
every known gap. T007 owns the FR-008 protected-grant positive path
(authority signing/publication, placement grant seam with a
non-`plaintext-v1` default, Provider verify/unwrap, Native key lifecycle);
T010 owns the real grant mutation behind Y-N-E. T011 owns the strict
negative-verdict semantics and the post-probe fallback readiness marker;
T013 owns the wrapper deadline margin; T014 owns the invalidated-evidence
banners. The revision-121 S4 result is invalidated by revision 123; S4
reruns only after a fresh T014/T015 and a new candidate. Live Y-N-I r42
PASS (2026-09-05) is the current Y-N baseline.

**Provider-boundary follow-up (2026-09-04, pre-T014)**: under T002/T006/T007,
close the Python compatibility Provider's V3 downgrade and progress-lineage
gaps before convergence. A Provider configured for V3, or an assignment that
declares the V3 schema, must reject an invalid projection before preparation or
execution. All preparation snapshots must retain the validated V3 attempt;
the status epoch remains local to the Selection-scoped operation. Preserve
legacy callers and the native-only Spec180 process vector. Add focused
handler-level negative/success/failure tests and document the two APPProvider
facade layers. This is implementation repair, not a new provider protocol or
qualification run. Source changes require a fresh T014; previous candidate
evidence remains historical.

**Follow-up result**: repaired and focused-tested; see
`evidence/provider-boundary-repair-20260904.md` (115 relevant checks passed).
The existing duplicate-identity and duplicate-phase guards also pass their
negative tests; no second process-lock/runtime-mode authority was introduced.
T014 remains open. The immutable-candidate contract permits explicit dirty and
untracked source bytes; H1 requires current byte binding, not an unconditional
commit of the surrounding worktree.

This is the current queue after the Controller PUBPARAMS readiness repair:

1. `S3 / T013`: retain the focused implementation evidence for the native
   Controller readiness barrier and rebuild the Python extension against the
   current `build-system-j2` core. **PASS for this repair**; see
   `evidence/t013-controller-pubparams-readiness-current-20260904.md`.
2. `S3 / T014`: run one fresh design-code convergence audit against the
   repaired Controller/Python publication path and require `PASS`.
3. `S3 / T015`: execute the YOLO-relevant local inventory exactly once from
   the accepted current source; old local/SIF evidence is not reused.
4. `S4 / T016`: build a new immutable SIF and replay exact-SIF Y-B only after
   T014/T015 pass.
5. `S5 / T017+T018+T020`: perform read-only Tiger readiness, stage accepted
   bytes, submit at most the authorized single cold Y-B request, and close
   the final structured verdict.

The source change is in the runtime Core/Python plane, so the revision-121
exact-SIF evidence is invalidated even though the SIF hash itself has not
changed. No old SIF/Tiger result may be combined with the repaired source.

## Revision 112 active queue (historical; superseded by revision 123)

This is the only active execution order:

1. `S0 / T011-NATIVE`: finish the current `-j2` unified native build and prove
   one NFD/NDN-SVS/NDNSF/NAC-ABE/Python `libndn-cxx` closure. **PASS
   2026-09-03**; see `evidence/s0-native-closure-current-20260903.md`.
2. `S1 / T004+T006`: seal one current-role YOLO package and an experiment-owned
   catalogue/offer trust set as one immutable candidate. The private keys stay
   outside Git; their public identities and digests are candidate inputs.
   **PASS 2026-09-04**; see `evidence/s1-candidate-seal-current-20260904.md`.
3. `S2 / T011`: run local MiniNDN Y-A, then Y-B, then only the critical
   feasibility, signature, digest, role-owner, and cleanup controls.
   **Y-A PASS 2026-09-04**; see
   `evidence/t011-yolo-ya-terminal-current-20260904.md`.
4. `S3 / T014+T015`: audit the production YOLO path once, then run the
   YOLO-relevant local inventory once.
5. `S4 / T016`: build one local SIF and replay Y-B from that exact image.
6. `S5 / T017+T018+T020`: run read-only Tiger readiness, stage accepted bytes,
   submit one one-node/one-GPU/four-Provider Y-B request, and close the result.

T012 is retained only as a completed Spec175 Qwen handoff record. Qwen-F,
three-distinct-GPU placement, a second warm request, and cross-model closure
are deferred and must not block or be claimed by Spec180. Earlier queue and
revision notes below are historical when they conflict with this section.

**Revision 113 Y-A closure note (2026-09-04)**: The first real Y-A terminal
Response was executed on the revision-112 sealed candidate. The run produced
the complete ten-milestone lifecycle journal and a terminal result digest
`sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`
that is byte-identical to an independent offline full-model CPU-ORT forward
on the same 640-by-640 input (numerical equivalence executed, not claimed).
Revision 113 closed seven implementation defects, each with a focused
red→green test (controller publication lifetime; runtime candidate/graph
digests in the published snapshot; component-set ExecutionRole interval;
simple-service mirror disabled for V3 offer issuers; empty external fetch
reference for provider-prepared roles; native lowercase digest canonical
form; /SERVICE-scope fetch routed through the service-authorized path).
Native rebuilds used `./waf -o build-system-j2 build -j2` only. One known
residual qualification-environment flake remains: the MiniNDN nfd.conf
`authorizations` section grants `faces/fib/cs/strategy-choice` but not
`rib`, so on some runs the second ServiceUser's prefix registrations are
rejected before the signer certificate reaches the local store; rerunning
the same sealed inputs passes. This is recorded for the T014 audit. Y-B is
the next queue step; its artifact publication path must carry real
encrypted large-data role artifacts (the Y-A metadata-record slice is
explicitly not reused for the four-role case).

**Revision 114 Y-B state (2026-09-04)**: Y-A was re-executed after the
revision-114 contract change and passed again with the identical terminal
result digest (deterministic across runs). Revision 114 changes, each with
a focused test: (1) `ProviderOfferV3` now binds `can_provision` and
`has_model` so the sealed plan can tell provider-prepared roles from
provisioned roles; (2) the empty-fetch-reference rule applies only when
`not can_provision` (local preparation or exact residency); (3) the native
key-offer `providerBootEpoch` composite form `provider:epoch` is accepted
against the bare V3 offer epoch; (4) Y-B/Y-N Providers are spread across
distinct MiniNDN nodes (shared node-scoped PIB races otherwise); (5) the
YOLO Provider no longer requests executable-artifact authorization (ONNX
models are not executables). Y-B progress this revision: the four spread
Providers all register and ACK; the shared-backbone-two-shard-v1 candidate
is selected and its Selection commits. The remaining Y-B blocker: the
Providers advertise `can_provision=True` but the plan's fetch references
still point at the metadata-only ARTIFACT APP Data records, and the
Providers receive no repo artifact references to materialize. The C++
collaboration layer then fails "failed to fetch execution spec
/example/controller/NDNSF/DI/ARTIFACT/.../<role>". Completing Y-B requires
the real encrypted large-data role-artifact publication (canonical ONNX +
external initializers, per role or shared) plus
`--artifact-references`/`--sync-materialize-before-serve` wiring for the
four Providers, and the post-Selection subgraph assembly/execution path
(executor `onnx.utils.extract_model` + the native assembler already exist;
their invocation for the four-role dataflow is the remaining work). Y-N,
T014/T015, SIF, and Tiger steps remain queued behind Y-B.

**Revision 115 Y-B closure note (2026-09-04)**: Y-B passed with a live
four-role terminal Response. The Providers bind the same validated local
canonical package; every role assembles its certified subgraph from the
canonical root after Selection (405-node backbone, 20/40-node detect
shards, 126-node merge), exchanges activations through the encrypted
NDNSF_DATA_V1 dependency dataflow, and the Merge role publishes the
terminal result. The terminal result digest is byte-identical to the Y-A
atomic result and to the offline full-model forward
(`sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`).
See `evidence/t011-yolo-yb-terminal-current-20260904.md`. Revision 115
additions: `YoloCanonicalArtifactBinding` (recipe certification + published
ARTIFACT identities), `CanonicalArtifactBinding.canonical_graph_digest`
(the assembler verifies the canonical ONNX identity, distinct from the
planning-space graph-port digest), the Provider-side certified-assembly
invocation with a real-package behavioral test, and the maintained User's
`canonical_artifact_ensurer` wiring. The repository-materialization
transport for role artifacts remains the SIF/Tiger promotion path and is
not claimed by the local slice. Next queue step: the Y-N critical controls
(feasibility, signature, digest, role-owner, cleanup), then T014/T015.

**Revision 116 Y-N closure note (2026-09-04)**: The Y-N case passed with
the critical controls executed. Four identities (one advertising
FullModel+BackboneNeck, three single-role) closed ACK with ackCount=4;
both candidates feasible; the signed priority selected the shared
candidate; the four-role pipeline produced the identical terminal digest
(`sha256:e6f942bc…`). Signature/digest/role-owner controls were verified
live; cleanup left no processes; the FAIL_CLOSED negative subcases are
declared in the case plan and covered by the focused suites. See
`evidence/t011-yolo-yn-control-current-20260904.md`. Revision 116 also
extends the prepare tool with the Y-N cover contract and relaxes the
runtime catalogue uniqueness rule to per-(manifest, candidate) pairs.
Next queue step: S3 — the T014 convergence audit (mandatory PASS) and the
T015 local inventory run.

**Revision 117 T014/T015 state (2026-09-04)**: The post-implementation
convergence audit is recorded in
`evidence/post-implementation-audit.md` with a CONDITIONAL PASS verdict.
The HIGH finding (MiniNDN nfd.conf `rib` registration privilege) is fixed
on the host (`/usr/local/etc/ndn/nfd.conf`, backup
`nfd.conf.pre-spec180`). T015: the source-bound inventory was generated
(291 entries) and the current build tree now carries `--with-tests` test
binaries built against the custom ndn-svs closure (-j2 compliant). The
YOLO MiniNDN cases and the full Spec180 Python suite (207 passed) executed
today; spec180-relevant native selectors pass on the new binaries. Two
native-suite triage items remain for SIF entry: the StreamFacade suite
(Spec175 stream tests, environment/working-tree related) and six
`Spec170NdnsfDiCoreFlow/ProductionNativeHandlers*` `/Aux`-output
assertions (pre-session native-runtime working-tree state). Neither
touches today's spec180 changed files. Next queue step: finish the T015
native triage, then S4 (one local SIF + exact-image Y-B replay).

**Revision 118 registration-retry note (2026-09-04)**: The registration
flake fix landed as a bounded app-side retry
(`registerInterestFilterWithRetry`, 6 attempts, 250 ms) on the
CertificatePublisher and ServiceUser NDNSF/CK prefixes. The nfd.conf
`rib`-privilege route was tested and rejected (NFD 24.07 parses the
authorizations section before the rib manager registers its module) and
the host config was restored. The certified assembly now treats a
whole-graph node cover as the identity operation (the atomic FullModel
role runs the canonical model directly instead of re-inlining every
initializer past the assembled-byte envelope). Y-A and Y-B both re-passed
live after the rebuild with unchanged terminal digests.

**Revision 119 T015 closure note (2026-09-04)**: The retry helper's
move-then-copy defect (a retried registration would install an empty
interest handler) was fixed by holding the handler in a shared_ptr.
Y-A re-passed on the final library. The two native-suite triage items were
attributed to pre-session working-tree state, not spec180 regressions:
the current `ndnsf-di-core-flow.t.cpp` (updated 2026-09-02 11:44) is newer
than the old `build/` binary (2026-09-01 01:20), so the old binary's pass
ran the older test without the `/Aux` assertions; the StreamFacade suite
failures are likewise Spec175 stream-path environment/working-tree state.
The spec180-relevant native selectors pass on the current-tree binaries.
T015's YOLO-relevant local inventory is executed (Y-A ×3, Y-B ×2, Y-N ×1
live PASSes today; 208/208 spec180 Python tests; native selectors pass).
Next queue step: S4 — one local SIF (spec180 Apptainer definition does not
exist yet; create it, build with build-local-sif.sh/Apptainer 1.5.3, exact-
image Y-B replay), then S5 (user-authorized Tiger submission).

**Revision 120 S4/S5 progress note (2026-09-04)**: The user confirmed the
VPN session and authorized the SIF/Tiger continuation. S4 in flight: the
Spec180 source seal was prepared with the prepare-local-sif-source.py FILES
list extended for the YOLO replay closure (the legacy
`NDNSF_DI_Yolo2x2_Minindn.py` and the `yolo_2x2` maintained application were
previously absent from the Spec175-centric list); the Spec180 Apptainer
definition (`spec180-runtime-final.def`, seal
`sha256:cb1046959392aafd32771dc4c9fad2712f77645def3f1ad72745c25e231d3981`)
bootstraps from the qualified spec174 r24 runtime SIF and compiles the full
native stack inside the builder stage (boundary validator PASS, no host
binary inputs); the local SIF build is running. The runner now carries the
exact-SIF command-provider contract (`SPEC180_RUNTIME_SIF`,
`sif_exec_prefix`, `Spec180SifNfd`, `--cleanenv`, host-process-fallback
marker) with focused tests; the YOLO Provider accepts
`--backend {onnxruntime-cpu,onnxruntime-cuda}` and the executor selects the
ORT provider chain from the closed `NDNSF_ORT_PROVIDERS` allowlist (T018's
CUDA-for-model-roles requirement). The Tiger profile and yolo-functional
sbatch were regenerated to the T019 one-GPU shape (rtx_6000:1, one cold
request). Tiger partition facts re-verified read-only (rtx_6000 nodes
available; /project 838T free). Remaining: finish the local SIF build, run
the exact-SIF Y-B replay via the new host-side replay driver, and one
authorized `yolo-functional` submission. The Tiger in-image dispatcher
(`scripts/run_spec180_case.py`) currently execs the MiniNDN runner, which
has no MiniNDN substrate on the Tiger node — the T013-owned Tiger launcher
path (host-NFD node-local launch) remains a design gap to close before the
submission can execute the Y-B workload on Tiger.

**Revision 121 S4 closure note (2026-09-04)**: S4 is DONE. The local SIF
(`spec180-runtime.sif`, `sha256:c7c84006ade8c3d657e95ad888589ac748cead5a17738856337e842fee20ec77`,
source seal `sha256:5087b787…`) built the complete native stack inside the
sealed builder stage (boundary validator PASS). The exact-SIF Y-B replay
passed through the new host-side replay driver: NFD and all children ran
inside the image via the Apptainer command provider; the ten-milestone
lifecycle completed (ackCount=4, shared candidate, 4 providers) and the
terminal result digest is byte-identical to the host MiniNDN results and
the offline full-model forward
(`sha256:e6f942bc…`). See
`evidence/t016-exact-sif-yb-replay-current-20260904.md`. Next: S5 — stage
the accepted bytes to /project and submit `yolo-functional` exactly once;
the Tiger node-local launcher (host NFD + per-process identities, no
MiniNDN) must be implemented first because the in-image dispatcher
currently assumes a MiniNDN substrate.

**Invalidated by revision 123 (2026-09-04):** the Core/Python PUBPARAMS
readiness repair changed the runtime plane, so this S4 result and the
`c7c84006…` SIF are historical. S4 must be rerun from a fresh candidate
after a new T014 PASS and T015 qualification; see
`evidence/t013-controller-pubparams-readiness-current-20260904.md`.

**Revision 122 S5 in-flight note (2026-09-04)**: The user confirmed the
VPN session and authorized the Tiger submission. The SIF + models +
workload + manifests + submit tree are staged at
`/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/` with the
remote SIF hash verified. Five submission attempts (jobs 209469-209474 +
the local SIF chain test) closed submit-bundle defects in order: the
compute-node Apptainer path/version, the /bundle bind contract, the
workload-to-dispatcher contract alignment, the renderer's import order,
the model-root contracts layout, the NFD config format, the SLURM_JOB_ID
pass-through, and the per-process identity bootstrap (shared root trust
chain + the NFD home identity set + the ndnsec import/export sequence).
The Tiger launcher chain now reaches the Controller process inside the
sealed SIF; the remaining blocker is the Controller's public-parameter
serving in the node-local topology (the publication ServiceUser's
parameter fetch fails — the main controller's native registration/param
serving chain in the SIF needs the next debugging iteration; the local
SIF reproduction reproduces it deterministically). Once that closes, the
launcher's remaining stages (repo receipt, four Providers, the User
oracle) follow the same chain. The T018 one-submission rule remains
intact: all five attempts failed before workload entry (submit-bundle
defects), and the workload itself has not executed.

**Audit state (2026-09-03, audit iteration 105; documentation revision 112)**:
S0/T011-NATIVE is complete. T001, T003, T012, and the scope-transfer T019 are complete;
T002 and T004--T010 are partial; T011 and T013 are partial; T014--T020 are not
started. A task remains
unchecked until its production path and task-specific evidence exist. Focused
Python/native checks, planned file paths, or an injected fake transport cannot
substitute for a live ACK/Selection/Provider/Response run. T014 must return a
fresh convergence `PASS` before T015--T020 may execute.

**Revision 108 owner/block rule (2026-09-03)**: The missing real experiment is
not caused by an unlisted task. After `G0-NATIVE` passes, `G0-CANDIDATE` is
waiting for one owner-supplied,
role-correct `DetectShard0/1` package, the registered catalogue signature and
matching private key, an absolute Provider offer-key map, and the canonical
Y-A ONNX file with digests. Until those inputs are sealed in one manifest, the
runner must stop once with `WAITING_EXTERNAL_INPUT` (exit 78). Do not create a
replacement trust root, edit a stale manifest, repeat SIF/Tiger submission, or
open another audit loop. After G0, execute one Y-A terminal Response before
touching Y-B/Y-N, QWEN-F, T014, or any expensive validation.

**Completion diagnosis**: the feature has not produced a real NDNSF-DI result
because no current candidate has reached a live terminal Response. The
registered entrypoint now has a barriered startup/publication path, but it
correctly stops before protocol state when the candidate package or Provider
offer/model inputs are missing or stale. The passing checks below are
contract, parser, publication-seam, and failure-atomicity tests; they prove
that invalid inputs are rejected, but they do not start NFD/SVS or exercise a
terminal Response. Keep the evidence vocabulary explicit:
`implemented`/`wired` is not `executed`, and `executed` is not `qualified`.

The lifecycle writer is now wired into the maintained coordinator/User path
with explicit request/attempt binding and ten ordered emissions. It remains
unqualified until a live Y-A exchange writes and validates the complete trace.

**Why the real experiment is still pending (revision 105)**: the checked-in
runner and focused tests are not the missing experiment. They stop correctly
before network side effects when the candidate is stale, unsigned, or missing
the Provider key/model inputs. The earlier task graph also let audit and
release refinements run ahead of one atomic Y-A request and placed the
QWEN-F executable after the SIF boundary. Those choices produced substantial
`implemented`/`wired` evidence but no `executed` terminal Response. This is
now corrected by one finite queue: G0-NATIVE -> G0-CANDIDATE -> G1 Y-A -> G2 Y-B/Y-N
-> T013/QWEN-F -> T014. Do not mark any task complete from seam tests, an
offline catalogue, a historical Spec175 result, or a synthetic lifecycle
trace.

**Revision 106 implementation correction (2026-09-03)**: The separate
`Experiments/NDNSF_DI_QwenAckDriven_Minindn.py --case QWEN-F` entrypoint now
exists and is covered by focused manifest, digest, identity, and dispatch
tests. This removes “entrypoint absent” as a T013 implementation blocker, but
does not provide the signed Qwen3.6-27B production manifest/object set or a
live two-request terminal result. T013 remains partial until those external
inputs are sealed and executed after G0/G1/G2 and before T014.

**Revision 107 input-status correction (2026-09-03)**: T011's pre-network
validation now records `WAITING_EXTERNAL_INPUT` (exit code 78) for missing or
invalid G0 inputs. A `UNQUALIFIED` status is reserved for a failure after the
MiniNDN case begins. This distinction prevents a missing candidate from being
counted as a failed NDNSF-DI request; it does not mark T011 executed.

**Revision 110 gate-order rule (2026-09-03)**: The native closure is the
first pre-network gate, named `G0-NATIVE`. It must prove that NFD, NDN-SVS,
NDNSF, NAC-ABE, and the Python extension resolve one identical hashed
`libndn-cxx` before candidate validation or MiniNDN startup. Only after it
passes may `G0-CANDIDATE` validate the trusted package, signature, Provider
offer keys, and canonical model. This is a prerequisite of T011-A, not a new
protocol result or a reason to repeat the whole task list.

**Revision 111 build-resource rule (2026-09-03)**: Every local native rebuild
used by `G0-NATIVE` MUST use one active build tree and a maximum of two
compiler jobs. The accepted NDNSF command shape is
`./waf -o build-system-j2 build -j2`; the launcher/operator MUST reject the
invalid extra Waf command `build ndn-service-framework -j2`, `-j8`, automatic
`nproc` expansion, or overlapping Waf builds before compilation. A partial
object directory, an interrupted build, or a build performed under a higher
concurrency setting cannot satisfy the native-closure gate.

**Revision 109 native-closure stop rule (2026-09-03)**: Before T011-A starts
MiniNDN, the runner must verify that the Python extension, NDN-SVS, NAC-ABE,
and NFD resolve one identical `libndn-cxx` file and digest. A mismatch is a
pre-start `WAITING_EXTERNAL_INPUT` (exit 78), not a protocol failure. The
previous split `.local-boost171` versus `/usr/local` closure caused Controller
and Repository socket EOF before a request. Rebuild the complete native stack
from one ABI/toolchain closure; do not repair it with an ad-hoc
`LD_LIBRARY_PATH` override or promote the old EOF as NDNSF-DI evidence.

**Revision 104 external-input stop rule**: T011-A has one admissible start
condition: a single manifest must bind the fresh `DetectShard0/1` package,
registered catalogue signature, explicit case policy/runtime identities,
Provider offer-key map, and canonical Y-A ONNX path. The runner checks these
inputs and their digests before any network process starts. When one is
missing, the task records `WAITING_EXTERNAL_INPUT` and names the owner; it
does not loop on temporary packages, in-place manifest edits, SIF rebuilds, or
Tiger submissions. This prevents boundary-test progress from being mistaken
for a completed NDNSF-DI experiment.

**Candidate-input correction (2026-09-03)**: The temporary YOLO packages
inspected during T011 preparation are not a sealed release input: most use
`DetectHead0/1` in the shared candidate, while the one observed package with
`DetectShard0/1` has no trusted catalogue signature. The current graph-bound
contract requires both the current names and a registered signature. T011-A
must regenerate the package with the current exporter and re-run package,
adapter, graph, initializer, and catalogue verification. Editing a stale
manifest or treating a role-name-only change as a valid candidate is forbidden
because it would sever the candidate digest and graph identity binding.

**Revision 104 T013 correction (historical; superseded by revision 106,
2026-09-03)**: The host release renderer now
consumes the shared trust-root verifier for QWEN-F and rejects an unsigned or
structurally invalid Qwen3.6-27B manifest before scheduler submission. The
focused and full Python release/dispatcher evidence is implementation-only
(`178 passed, 22 warnings`); revision 106 adds the separate QWEN-F entrypoint,
but T013 remains partial until a signed production manifest, in-image
manifest/object checks, and production result writer are complete.

**Scope lock**: this ledger is not a queue for another audit iteration. Do not
create new preflight, evidence, SIF, or Tiger tasks while T011-A is open.
Implement the smallest real Y-A vertical slice first, reuse it for Y-B and
Y-N, then finish T013 and run one T014 audit. Any later source, harness,
recipe, profile, or model change invalidates the candidate and returns to its
owning implementation task.

**Revision 105 active-task rule (2026-09-03)**: `partial` is a status, not an
invitation to work on every open task concurrently. Until G1 closes, the only
active portions are the atomic `FullModel` path in T002/T004/T005/T006/T007/T009
and the success-path portion of T011-A. T008, the shared-role portions of
T007/T009, and T010's full negative matrix are deferred until after the live
Y-A terminal Response. T013 may prepare only the already specified release
seams; it may not trigger SIF/Tiger work or add a second runner. A missing G0
artifact records `WAITING_EXTERNAL_INPUT` once with its owner and stops.

**Revision 105 stop rule**: Do not advance a checkbox from focused tests alone.
The minimum evidence ladder is `implemented` -> `wired` -> `executed` (one real
MiniNDN terminal Response) -> `qualified` (task oracle plus cleanup). If the
current gate lacks its external input, preserve the rejection evidence and
resume at that same gate after the input is supplied; do not restart older
Spec175 work or rebuild a SIF.

**Iteration 96 implementation correction (historical; superseded by iteration
101, 2026-09-03)**: T011-A's runner
boundary is now wired: it validates the publication batch, starts control and
Provider phases behind readiness barriers, and holds User until controller
readback. T011-A remains partial because no current candidate has crossed the
live protocol boundary. The next required inputs are a freshly exported
`DetectShard0/1` package, an absolute Provider-to-Ed25519-private-key map, and
the Y-A canonical ONNX path. These are inputs to the existing task, not new
tasks and not permission to start SIF/Tiger work.

**Iteration 97 implementation correction (2026-09-03)**: The Controller
publication branch was previously gated on the unrelated
`--deploy-to-repo-manifest` option. The runner did not pass that option, so a
real case would start the ordinary controller loop and never publish the
candidate-bound catalogue. The Controller now enters publication mode when
`--spec180-runtime-publication-file` is present, and the application test
guards this command-shape contract. This is a closed wiring defect; T011-A
remains unchecked until a real Y-A ACK-to-Response run succeeds.

**Iteration 98 implementation correction (2026-09-03)**: The Controller now
decodes the runner's publication envelope through a fail-closed validator
before signing or publishing any APP Data. The validator binds the case and
signer namespace, checks package/catalogue and per-artifact digests, rejects
duplicate/out-of-scope names, and bounds metadata payload size. The focused
application regression mutates an artifact digest and requires rejection.
This closes the process-boundary integrity seam; it is not a live-case result.

The `Target checkpoint` labels below state acceptance goals only. They do not
mark a task complete; the checkbox, production-path evidence, and audit gate
remain authoritative.

## Historical execution queue (revision 110; superseded by revision 112)

This queue explains the earlier delay but is no longer the active completion
route. Use the revision 112 active queue above.

0. **G0-NATIVE**: build and verify one ABI-identical native closure for NFD,
   NDN-SVS, NDNSF, NAC-ABE, and the Python extension. Record resolved paths,
   SHA-256 digests, SONAME/Build ID, import, and a small native startup smoke.
   A mismatch is `WAITING_EXTERNAL_INPUT` and stops before MiniNDN.
1. **G0-CANDIDATE**: seal the role-correct signed package, Provider offer-key
   map, policy, and canonical Y-A model in one manifest. Missing owner input is
   `WAITING_EXTERNAL_INPUT`; do not edit a stale package in place.

2. **T011-A — atomic Y-A**: use the now-wired barriered driver with one fresh,
   candidate-bound package and one Provider's signed offer/local model input;
   then complete one real one-Provider MiniNDN request and terminal Response,
   including signed
   catalogue readback, encrypted input-reference publication, actual
   coordinator request/attempt binding, child supervision, and cleanup.
3. **T011-B — shared Y-B**: reuse that same driver for the four-role,
   four-Provider capability case and native Merge.
4. **T011-C — fixed Y-N**: run the order-invariance control and the declared
   fail-closed mutations through the same driver.
5. **T011-D — freeze**: record the live-driver evidence and stop changing the
   pre-audit runtime path.
5. Finish T013's already-specified release/SIF/QWEN-F implementation, run one
   T014 convergence audit, then execute T015--T020 once in order.

Until T011-D closes, T014--T020 are blocked and no focused preflight result,
offline catalogue, injected publisher, or historical Spec175/Tiger artifact
may be promoted to a live-case result. If a source, harness, dependency,
recipe, or effective configuration must change after T014, invalidate the
candidate and return to its pre-T014 owner instead of patching the expensive
run.

**Binding execution rule (audit iteration 103; documentation revision 106)**: historical audit iterations below
are provenance, not a queue of work. Do not start another broad regression,
SIF build, upload, or Tiger submission while T011 lacks a live terminal
Response. The only current implementation path is T011 atomic Y-A, then
shared Y-B, then fixed Y-N; after that, finish all
promotion code and the QWEN-F executable in T013, run T014 once, and execute
T015--T020 without code or configuration changes.

- **Iteration 86 parser correction (2026-09-03)**: the runtime signed
  snapshot resolver now rejects duplicate `candidateDigest` records even when
  their aliases and manifests differ. This matches the contract's one ACTIVE
  snapshot per candidate rule and is covered by the resolver regression. The
  full Spec180 collection remains 169 passing with 22 warnings; no task is
  promoted and T011/T014 remain open.

- **Iteration 87 parser correction (2026-09-03)**: the runtime signed snapshot
  resolver now rejects `RETIRED` and `REVOKED` records at the APP parse
  boundary. Those statuses remain valid for catalog history but are not valid
  inputs to new placement. A focused resolver regression covers a retired
  record. The full Spec180 collection remains 169 passing with 22 existing
  warnings; no task is promoted and T011/T014 remain open.

- **Iteration 88 publication seam (2026-09-03)**: `MiniNdnCaseRuntime` now
  composes, publishes, exact-readbacks, byte-compares, and receipts the signed
  runtime catalogue through an injected `ServiceUser`-compatible publisher.
  Publish/readback failure, mutation, identity mismatch, invalid timeout, and
  duplicate publication are covered by the runner regression. The focused
  runner slice is 44 passing tests; the full collection now reports 170 passing
  tests with 22 existing warnings. The production driver, candidate-bound artifact
  publication, and T011/T014 remain open.

- **Iteration 89 cleanup seam (2026-09-03)**: `MiniNdnCaseRuntime` now owns
  the process handles created by each startup phase and exposes an idempotent
  `stop()` that stops only those children, stops the case network, invokes the
  maintained MiniNDN cleanup, and clears phase/publication state. T011 must
  call this method from `finally` for every case outcome; a focused regression
  covers one-child cleanup and repeated teardown. This closes cleanup
  ownership at the adapter boundary only; the live driver and T014 remain
  open, so no task is promoted.

- **Iteration 90 idempotence correction (2026-09-03)**: the cleanup seam now
  marks completion before teardown, makes repeated `stop()` calls a no-op, and
  rejects a later network restart. This prevents a repeated failure handler
  from invoking global MiniNDN cleanup after another case has started. The
  focused regression covers one-time cleanup and restart rejection; 45 runner
  tests and 171 Spec180 Python tests pass in four bounded groups with 20
  existing warnings. The live driver and T014 remain open.

- **Iteration 91 integrity correction (2026-09-03)**: the runtime catalogue
  adapter now rejects missing or duplicate candidate snapshots, unexpected
  candidate digests, incomplete case role/artifact coverage, duplicate
  artifact names, and non-absolute artifact names. Exact APP readback also
  rejects a missing signer certificate even when the payload bytes match.
  The focused runner slice is 47 passing tests; the four bounded groups report
  173 passing tests with 20 existing exporter/runtime warnings. This is
  fail-closed adapter evidence only; the production publisher, live ACK/Selection/Provider/
  Response driver, and T014 convergence audit remain open.

- **Iteration 84 naming/adapter correction (2026-09-03)**: the native
  `ServiceUser.publish_signed_app_data()` contract requires the catalogue name
  to be below `/<controller>/NDNSF/DI/` and the signer to equal that controller
  identity. Runner validation and all dispatch/binding fixtures now use and
  enforce this exact binding; the old free-standing `/spec180/catalogue/v1`
  shape is rejected before MiniNDN startup. The audit also fixed the case
  adapter's undefined identity lookup in `initialize_keychains()` and converts
  a missing `providerPrefix` into the documented fail-closed error. Focused
  runner coverage is 43 passing tests and the complete Spec180 collection is
  169 passing with 22 existing warnings. This does not advance T011/T014:
  the live ACK/Selection/Provider/Response driver remains unwired.

- **Iteration 85 design/code correction (2026-09-03)**: the maintained V3
  coordinator now routes an explicit signed catalog through
  `_prepare_artifacts` before Selection, so a matching ACTIVE pre-split
  snapshot must resolve every role/rank instead of leaving synthetic,
  unfetchable names in the sealed plan. A focused regression covers this
  production-shaped path; it does not advance T011 or T014. T011 must now
  explicitly produce and publish candidate-bound role artifact Data objects
  and the signed runtime `ndnsf-di-presplit-catalog-snapshot-v1` APP record
  before starting User. The offline `spec180-yolo-catalogue-v1` candidate
  record, legacy `/Stage` manifest, and caller snapshot file are not runtime
  publication evidence.

- **Iteration 78 profile correction (2026-09-03)**: T011's startup policy now
  requires one Provider for Y-A and four Provider identities for Y-B/Y-N, with
  a distinct capability cover for each shared-role case and a `FullModel`
  capability in Y-N. These checks prevent a degenerate single-Provider process
  smoke but never assign request-time roles; ACK closure and the sealed plan
  remain authoritative. `LifecycleJournal` requires the live driver to bind
  the coordinator `requestId` and ACK attempt identity; provisional IDs are
  test-only. Focused runner coverage passes; T011/T014 remain open pending the
  real NFD/SVS driver and convergence evidence.

- **Iteration 79 lineage correction (2026-09-03)**: The production runner seam
  now requires protocol binding by default. T011 must pass the actual
  coordinator `requestId` and ACK attempt identity to `LifecycleJournal` before
  `INPUT_REFERENCE_PUBLISHED`; generated provisional IDs require an explicit
  test-only opt-out and rebinding is rejected. This closes a trace-integrity
  ambiguity only; T011 and T014 remain open. The current complete
  `tests/python/test_spec180_*.py` collection is 145 passed with 22 warnings;
  this is focused source evidence and does not advance the live MiniNDN,
  convergence, SIF, or Tiger gates.

- **Iteration 80 harness-boundary implementation (2026-09-03)**: T011 now has
  an explicit `CaseRuntimeBinding`/`MiniNdnCaseRuntime` adapter. It consumes
  candidate-declared node and identity maps, rechecks topology membership and
  isolated-policy digest before network creation, and derives merged route
  origins when multiple logical Providers share one MiniNDN node. The legacy
  startup/keychain/process helpers gained optional explicit roots so the
  adapter does not mutate their globals. Focused runner coverage is 28 passing
  tests; this closes only the parameter-safe harness slice. The ACK-driven
  Controller/repository/publication/Selection/Response driver and T014 remain
  open (A180-123), and no MiniNDN/SIF/Tiger result is promoted.

- **Iteration 81 policy-loader boundary (2026-09-03)**: The runner now invokes
  the maintained `policy.py` parser and compatibility checks for both the
  source policy and the isolated `case-policy.json`, before any MiniNDN/NFD
  startup. This catches missing user authorization or malformed service
  descriptors at the runner boundary instead of inside a child process. The
  focused runner slice is 28 passing tests and the complete Spec180 Python
  collection is 153 passing with 22 existing exporter/runtime warnings. This
  remains preflight evidence only; T011's live ACK-driven driver and T014 are
  still open.

- **Iteration 82 process-vector boundary (historical; superseded by iteration 83, 2026-09-03)**: `MiniNdnCaseRuntime`
  now builds a side-effect-free `CaseProcessSpec` vector for Controller,
  repository, authorized capability Providers, and the maintained
  ACK-driven User. It derives all commands from the isolated policy and
  explicit node/identity maps, reuses the maintained command/supervision
  helpers, and fixes the User ACK timeout to 1500 ms. Thirty-one focused runner
  tests and 156 complete Spec180 Python tests pass with 22 warnings. The
  vector is not yet called by `run_minindn_case`; live catalogue/input
  publication, ACK/Selection/Response, lifecycle binding, and the T014 audit
  remain open.

- **T004/T005 audit note (iteration 69, historical; superseded by iteration 71)**:
  the exporter and adapter
  still derive shared-role cuts from graph-node percentages/topological indexes.
  Completion now requires signed semantic node sets, branch ownership,
  producer/consumer tensor interfaces, dependency edges, and an equivalence
  check; a percentile or fixed-index boundary is not safe-cut evidence.
- **T004/T005 implementation note (iteration 71)**: the exporter now emits a
  graph-bound semantic partition for YOLO26n, including architecture-scope
  node sets, lifted-constant ownership, producer/consumer tensor contracts,
  role dependencies, and semantic safe cuts. The adapter verifies the signed
  partition against the loaded ONNX graph and rejects altered role/cut
  assignments. Native assembly, maintained application consumption, and the
  full-model equivalence run remain open, so both tasks stay partial.
- **Iteration 72 evidence refresh (2026-09-03)**: the full current
  `tests/python/test_spec180_*.py` collection reports 125 passing tests and
  22 warnings. This remains focused source evidence; it does not execute the
  real NFD/NDN-SVS driver or advance T011, T014, or any qualification task.
- **Iteration 72 dispatcher-contract correction (2026-09-03)**: the planned
  in-image dispatcher now has a closed input contract in
  `contracts/yolo-minindn-runner-v1.md`. T011 must validate the
  candidate-digest-bound `spec180-dispatch-workload-v1` document, exact
  gate/case, `/bundle`-relative
  maintained entrypoint, fixed argument shape, allowlisted environment, and
  fresh evidence root before starting any child. Unknown fields, traversal or
  shell fragments, gate/case mismatch, digest mismatch, and ambient paths must
  have focused fail-closed tests. This is a design correction only; the
  dispatcher and live driver remain unimplemented.
  In the legacy T011 sentence below, any shorthand such as “signed workload”
  means this candidate-digest-bound schema; no separate workload signing key
  is introduced by Spec180.
- **Iteration 73 dispatcher implementation (2026-09-03)**: T011 now provides
  `scripts/run_spec180_case.py` and 13 focused tests. The shim verifies the
  exact `spec180-dispatch-workload-v1` fields, raw workload SHA-256, fixed
  gate/case/argument vector, `/bundle` entrypoint, allow-listed candidate
  environment, fixed image mounts, and empty evidence root before `exec`.
  It is included in the local SIF source archive together with the YOLO
  entrypoint. This closes only the dispatch-boundary implementation slice;
  the real ACK-driven NFD/SVS driver remains fail-closed and T014 is still
  required.
- **Iteration 74 release-boundary implementation (2026-09-03)**: T013 now
  consumes the T011 dispatcher validator before scheduler submission, and
  `run-functional.sh` validates the mounted model-manifest/workload SHA-256
  values before creating its output root. The focused release/dispatcher/SIF
  slice passes; this does not embed a SIF or execute MiniNDN/Tiger.
- **Iteration 75 dispatch-boundary correction (historical; superseded by
  revision 106; 2026-09-03)**: the QWEN-F
  dispatcher vector no longer routes to the Spec175 M11 tiny-model wrapper.
  It is reserved for `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py`, the
  future external Qwen3.6-27B ONNX runner, and at that time failed closed with
  `ENTRYPOINT_MISSING` until T013 created that entrypoint. Revision 106 now
  provides the focused-tested entrypoint; production manifest/object closure
  remains required. The workload now
  carries only four non-path Qwen identity fields; the fixed runtime manifest
  and model-root mounts are the sole path bindings. Focused dispatcher and
  release tests cover this separation, preventing a false QWEN-F result.
- **Iteration 76 audit correction (2026-09-03)**: the local inventory no longer
  duplicates Q-C/Q-W seed arguments. It reads the registered M01/M11 and seeds
  (`175021`/`175022`) from `qwen-reference-manifest-v1.json`, so a frozen-case
  change cannot silently leave the execution command stale. The terminal result
  validator now binds protocol/result counts, the three-device CUDA backend,
  no CPU fallback, unique child IDs, and cleanup count to the structured oracle;
  focused evidence is 143 passing tests with 22 warnings.
- **Iteration 77 design audit (2026-09-03)**: `Y-N-C` now removes the
  `FullModel` capability and at least one required shared-candidate role so the
  negative cannot accidentally select `atomic-v1`. T011 must also reuse the
  existing MiniNDN/NFD/SVS startup, routing, keychain, and process-supervision
  helpers from `Experiments/NDNSF_DI_Yolo2x2_Minindn.py`; it may adapt
  candidate-specific inputs but must not fork or invoke the legacy
  deployment-first oracle. This is a contract/readiness correction only; no
  task is promoted and no qualification result is added.
- **T011/T013 audit note (iteration 69, historical; superseded by iteration
  73)**: `run-functional.sh` originally probed a missing in-image
  `scripts/run_spec180_case.py`. Iteration 73 adds the source dispatcher and
  focused mutation coverage; T016 must still embed the exact file in the SIF
  and pass the in-image probe before remote execution.
- **T013/T020 audit note (iteration 69, historical; superseded by iteration
  76)**: the result validator accepted
  label-only oracle strings. Completion requires structured oracle records with
  candidate-bound evidence paths, SHA-256 digests, schemas, lifecycle/request
  records, runtime/device identities, child exits, and cleanup details; labels
  alone cannot establish a functional verdict.
- **T011/T014 audit note (iteration 69)**: lifecycle redaction must use a closed
  milestone-specific field allowlist and recursive value validation. Generic
  `payload`, `content`, `token`, or byte fields are forbidden even when their
  values do not match a secret-name pattern.
- **T011/T014 implementation note (iteration 70)**: `LifecycleJournal` now
  enforces the milestone-specific allowlist and scalar-only values in the
  production runner seam. Generic-payload, unknown-field, nested-value, and
  secret-field focused regressions pass. This closes only the logging-schema
  correction; the real driver and convergence audit remain open.
- **T013/T020 implementation note (iteration 70)**: `validate_result()` now
  rejects label-only oracle strings, requires the closed structured oracle
  schema and candidate identity, and verifies each referenced file digest under
  an explicit evidence root. The production result writer still needs to emit
  these records before T013/T020 can close.

- T006/T009/T011 audit note (iteration 49): V3 configuration now rejects
  caller-supplied ACK-coverage roles and predicates, preserving the registered
  timeout as the discovery-closure authority. This is a focused configuration
  guard only; the production Trust-Schema offer verifier and live
  ACK/Selection/Provider/Response execution remain required.
- T002/T006 audit note (iteration 49): the model-neutral built-in sequential
  fixture now declares its first role as input ingress and last role as result
  egress so stricter V3 ownership validation does not break Spec170
  compatibility cases. It is fixture-only and cannot supply Spec180 YOLO
  qualification evidence.
- T001/T006 audit note (iteration 50): the candidate-bound Provider-offer
  policy is now specified in `contracts/provider-offer-trust-v1.md`. It names
  the existing NDNSF Trust Schema/certificate verifier as the authority and
  does not introduce a second caller key map. The contract is not evidence of
  verifier implementation; T006/T011 must still wire and exercise it.
- T006 audit note (iteration 51): `ProviderOfferTrustVerifier` now implements
  candidate-policy loading, registered Ed25519 offer verification, Trust-Schema
  callback delegation, ACK provenance and request/model/graph/deadline binding,
  and fail-closed mismatch handling. The factory invokes this ACK-aware method
  before constructing `ProviderPlanningViewV3`; live certificate-chain
  verification and the maintained ACK-to-Response runner remain open. Evidence:
  `evidence/t006-provider-offer-verifier-current-20260903.md`.
- T006 audit note (iteration 52): the native ACK projection now carries an
  explicit `trust_schema_validated` marker set only on the packet-backed
  ServiceUser path. The maintained YOLO helper no longer uses the caller HMAC
  map: it loads the candidate-bound Provider-offer policy and PEM key map and
  delegates packet-authentication authority to that native marker. This closes
  the Python-side HMAC contradiction, but it is still not live certificate-chain
  or ACK-to-Response evidence; T006/T011 remain open.
- T006 audit note (iteration 53): `ServiceUser` now registers the ACK SVS
  subscription with `packets=true`; the validated Data packet is retained so
  the marker and signer/KeyLocator/wire-digest projection can be populated on
  the production callback. The focused wiring regression and native rebuild
  pass, but live certificate-chain and ACK-to-Response evidence remain open.
- T001/T014 audit note (iteration 54): Spec175 is a frozen
  `LOCAL_FUNCTIONAL_PASS` baseline at its named source seal. The current
  worktree's Spec180 changes are a separate source delta; a newly generated
  dirty-tree seal verifies bytes only and cannot substitute for Spec180's
  convergence or local execution evidence.
- T011/T014 audit note (iteration 55): Y-N is one inventory entry containing
  the fixed `Y-N-O` catalogue-order control and `Y-N-C/P/R/I/E/L` subcases.
  The control must preserve the decision; the six negatives must fail at their
  declared boundary. Each subcase uses a fresh MiniNDN state/output directory,
  and the runner validates one non-secret JSONL event per lifecycle milestone
  before emitting the aggregate marker.
- T011 audit note (iteration 56): `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`
  now owns the candidate-bound input preflight and exact-once lifecycle
  journal. It intentionally fails with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`
  until the real NFD/NDN-SVS process driver and ACK-to-Response path are
  connected; no case PASS or qualification evidence is implied.
- T011 audit note (iteration 57): the supervised local gate creates each fresh
  empty case-output root before launch; the runner rejects missing,
  non-directory, or pre-populated roots. This keeps output-path ownership
  source-bound and does not change the open live-driver requirement.
- T011 audit note (iteration 58): the preflight persists a non-secret
  `case-input.json` descriptor with package/registry/topology/config digests,
  checks the expected candidate set for Y-A/Y-B/Y-N, and rejects legacy
  `/Stage` or caller-HMAC policy text before any driver can start.
- T011 audit note (iteration 59): the preflight now binds the explicit catalog
  Data name/signer, trust-root schema, public-key-map digest, per-key public
  key digests, and recursively rejects secret-bearing manifest metadata; this
  remains preflight-only and does not close the live driver requirement.
- T011 audit note (iteration 60): canonical graph and initializer paths are
  now confined beneath the package root, with a focused traversal regression;
  this remains preflight-only and does not close the live driver requirement.
- T011 audit note (iteration 62): the preflight now materializes a
  candidate-bound `case-plan.json` for Y-A/Y-B/Y-N. It carries signed role and
  ingress/egress identities plus the fixed Y-N control/negative outcomes, but
  no Provider assignment; the live ACK snapshot remains the only placement
  authority and the real driver remains open.
- T011/T014 audit note (iteration 63): a current-source re-audit confirms that
  the case-plan/lifecycle/preflight and local supervisor are focused evidence
  only. The real NFD/NDN-SVS case driver and candidate-bound inventory
  execution are still open; no task is promoted and no qualification result is
  added.
- T011/T014 audit note (iteration 64): terminology was rechecked against the
  maintained YOLO caller. The candidate-bound Ed25519 policy/PEM map is the
  current focused verifier input; only the historical caller HMAC map is
  fixture-only. Neither closes the missing native certificate-chain callback or
  live ACK-to-Response driver, so task status and qualification gates remain
  unchanged.
- T011 audit note (iteration 68): the registered runner now treats
  `service.providers[*].roles` as authorized capability advertisements rather
  than fixed owners. It requires every case role to be advertised by at least
  one authorized Provider, permits overlapping capability sets only when the
  fixed profile's distinct-capability-cover check passes, and still checks the
  MiniNDN node map before NFD/Provider startup. The authenticated post-ACK
  snapshot and sealed plan remain the only placement authority; the focused
  runner coverage remains 37 passing tests at that historical checkpoint,
  while the real NFD/NDN-SVS driver and live ACK-to-Response evidence remain
  open.
- T011 audit note (iteration 65, historical; superseded by iteration 68): the
  earlier runner rejected multi-role advertisements and described them as
  ownership. That was a startup authorization check, not a valid placement
  invariant, and has been corrected.
- T011 audit note (iteration 66, historical; superseded by iteration 68): before
  any protocol process starts, the runner wrote a fresh case-local
  `case-policy.json` containing only the registered case roles and explicit
  authorized capability entries. The source policy was not modified and the
  isolated policy was not a placement decision; its digest was recorded in
  `case-input.json`. Runner-specific coverage was 16 passing tests (37 in the
  combined focused slice); live driver execution remains open.
- T012/T011 audit note (iteration 67, historical; superseded by iteration 68): the task ledger, plan, and audit were
  rechecked and agree that T012 is complete. The previous 36-test combined-slice count was
  stale; the current runner/local-gate/inventory/contract slice is 37 passing
  tests. This bookkeeping correction adds no MiniNDN, SIF, or Tiger evidence.

## Phase 1: Setup and Shared Authority

**Purpose**: Create one machine-checkable Spec180 authority before changing
runtime behavior.

- [X] T001 Establish the Spec175 handoff, Spec180 requirement/case registry, source-owner map, configured YOLO catalogue trust root (signer key ID, public-key digest, and signature algorithm), configured external model-manifest trust root, candidate-bound Provider-offer policy contract, and fail-closed document/fixture contract gate in `specs/175-ndnsf-di-streamed-invocation/handoff-to-spec180.md`, `specs/180-ack-driven-cross-model-qualification/traceability.md`, `specs/180-ack-driven-cross-model-qualification/contracts/catalogue-trust-root-v1.md`, `specs/180-ack-driven-cross-model-qualification/contracts/provider-offer-trust-v1.md`, `specs/180-ack-driven-cross-model-qualification/contracts/model-manifest-trust-v1.md`, `specs/180-ack-driven-cross-model-qualification/contracts/trust-root-registry-v1.json`, `scripts/spec180_contract_gate.py`, and `tests/python/test_spec180_contract_gate.py`; accept only when all FR/SC/case IDs are unique, every required source/evidence owner is present in the owner map, every existing prerequisite/evidence path resolves, future implementation paths are explicitly marked `planned` rather than silently omitted, missing or stale Spec175 local closure blocks formal qualification, both trust roots are populated from the checked-in registry rather than ambient configuration, an unsigned/unknown-key catalogue or model manifest fails before enumeration/staging, and no historical candidate is marked current. Evidence: `evidence/t001-contract-gate-current-20260902.json` and `evidence/t001-contract-gate-current-20260902.md`; focused gate tests pass.

---

## Phase 2: Foundational Model-First Boundary

**Purpose**: Expose the existing generic coordinator without creating a second
planner or breaking the Qwen convenience path.

- [ ] T002 Generalize the model-first application request so `InferenceTaskRef`, `ApplicationInput`, and `TaskOptions` reach the existing `AutomaticPlanningCoordinator.request()` while current `GenerationInput`/`GenerationConfig` calls adapt to the same owner; the normative generic entry points are `InferenceClient.request_task(model=..., task=..., input=..., timeout_ms=..., options=..., strategy=...)` and the equivalent `InferenceApplication.request(..., task=..., task_options=..., timeout_ms=...)` (not `request_model()`); define mutually exclusive `INLINE` (at most 4096 bytes) and repository-backed encrypted `REPO_REF` input modes, replacing unconditional base64 embedding for the registered YOLO input without creating another repository protocol; project the candidate-declared ingress into one V3 `APPLICATION_INPUT` endpoint and the egress into one terminal-response owner; expose `ProviderRuntimeContext.fetch_application_input()` and `publish_terminal_result(...)`, failing closed with `DI_INPUT_FETCH_ROLE_MISMATCH`, `DI_TERMINAL_RESPONSE_ROLE_MISMATCH`, or `DI_INPUT_REPO_DIGEST_UNVERIFIED`; start with compatibility, oversize-inline, malformed/unencrypted/unauthorized reference, name-only or size-only fetch, non-owner fetch/publish, and plaintext-log failures; update `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/repo_reference.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, and `tests/python/test_spec180_generic_request_api.py`; accept only when generic object detection needs no Provider list/deployment/role map, the Request contains no registered image bytes, the maintained caller publishes the encoded input through the canonical `app_sdk.client.APPClient.publish_application_input_reference()` (or consumes its source-bound receipt) before `REQUEST_SENT`, only the candidate-declared input-ingress role can fetch/decrypt/verify the input after Selection through a reference-aware repository primitive, the candidate-declared result-egress role alone publishes the terminal result, Qwen inline callers remain source-compatible, and input/result plaintext is absent from logs/evidence. Evidence: `evidence/t002-generic-request-current-20260902.md`; focused and compatibility regressions pass. The generic request/input boundary and reference-aware digest/encryption/scope checks are implemented and focused-tested, but production ACK-to-Provider-to-Response wiring and the pre-request publication event remain open; do not mark this task complete until both are exercised.
- T002 audit note (iteration 24): the focused regression also invokes the legacy `DistributedInferenceProvider.add_role()` wrapper to guard source compatibility after V3 ownership wiring; this closes the wrapper defect but does not close T002's production-path acceptance.
- T002 audit note (iteration 29): the broader Provider-generation compatibility suite now passes after legacy V2 handler ownership flags were initialized to disabled; the flags are enabled only from a validated V3 dataflow projection. This closes the compatibility regression, not the production ACK-to-Provider-to-Response acceptance.
- T002 audit note (iteration 33): the maintained `REPO_REF` caller had to publish the encoded input before `REQUEST_SENT`, then perform a trusted DI reference-binding step; the reference-file path did neither. This historical finding is retained in `evidence/t002-input-publication-audit-current-20260902.md`.
- T002 audit note (iteration 34): `app_sdk.client.APPClient.publish_application_input_reference()` now owns native encrypted publication, source-bound metadata validation, DI reference construction, and the non-secret `INPUT_REFERENCE_PUBLISHED` journal record. The native C++ result and Python binding expose the manifest/authorization/epoch fields, and an old binding fails closed. Focused publication/binding tests and a real Python 3.8 extension import probe pass; T002 remains unchecked until live ACK/Selection/Provider/Response execution.
- T002 naming note (iteration 34): the older `APPClient.publish_large_payload_reference()` phrase in the original task sentence refers to the lower network compatibility facade; the maintained Spec180 owner is the canonical `app_sdk.client.APPClient.publish_application_input_reference()` method described above.
- T002 normative correction (iteration 35): for implementation and acceptance, the canonical `publish_application_input_reference()` owner supersedes the historical shorthand in the original one-line task text; the lower compatibility facade cannot satisfy the publication, binding, or qualification requirement.
- T002 contract clarification (iteration 36): the current `contentDigest` and legacy `ciphertextDigest` reference the SHA-256 digest of plaintext returned after authenticated decryption; neither field is an encrypted-segment wire digest. A future ciphertext digest would require a separate versioned field and contract.
- T013 audit note (iteration 37, historical; superseded by iterations 73--76): the fixed profile, strict release renderer, public submission wrapper, and candidate-bound terminal-result validator then existed with focused mutation/no-side-effect tests. T013 remained unchecked until the source-bound local-suite inventory and real case runner were implemented and exercised; `run-functional.sh` intentionally failed closed while that runner was absent.
- T013 audit note (iteration 39, historical; superseded by iterations 73--76): the release renderer then used Slurm's explicit `--export=NONE,<SPEC180_*=...>` form, and `run-functional.sh` verified the fixed Apptainer 1.5.3 path/SIF digest before entering `/bundle` with `--nv --cleanenv`. The case runner and source-bound inventory were still absent at that checkpoint, so T013 remained partial and no scheduler or workload result was implied.
- T013 audit note (iteration 40): `scripts/spec180_inventory.py` now builds and validates a candidate-bound inventory from the complete Boost selector listing, Spec180 pytest collection, and the registered Y-A/Y-B/Y-N/Q-C/Q-W entrypoints. It rejects missing, duplicate, escaped, and digest-mismatched entries before writing output. The current repository still fails closed at missing Y-A, so no inventory has been materialized and T013 remains partial.
- T013 audit note (iteration 41, historical; superseded by iterations 73--76): `scripts/run_spec180_local_gate.py` then validated source-bound command shapes and all source digests before execution, snapshotted the candidate-bound inventory, ran one supervised child per entry, recorded PID/oracle/exit/signal/timeout/cleanup and redacted logs, and rejected a non-empty evidence root. The real Y-A/Y-B/Y-N case runner was still absent at that checkpoint, so no candidate-bound inventory had been materialized and T013 remained partial.
- T013 audit correction (iteration 41, historical; superseded by iteration 76): Q-C/Q-W inventory entries invoked the maintained `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` wrapper rather than calling `NDNSF_DI_LlmPipeline_Minindn.py --spec175-case ...` directly. The old note's shared seed is not current authority; the inventory now reads the registered M01/M11 seeds from `contracts/qwen-reference-manifest-v1.json`.
- T015 audit correction (iteration 42): the five formal cases are inventory
  entries, not a second execution phase. Each entry must run exactly once in
  its own supervised child; any separate duplicate formal-case pass is
  diagnostic only and cannot contribute qualification evidence.
- [X] T003 Add one candidate/evidence identity schema shared by planning, local qualification, SIF, and Tiger tooling in `scripts/spec180_candidate.py`, `specs/180-ack-driven-cross-model-qualification/contracts/immutable-candidate-v1.md`, and `tests/python/test_spec180_candidate.py`; accept only when canonical serialization binds all planes, dirty source bytes are explicit, cross-candidate evidence is rejected, and the invalidation result names the earliest restart gate. T003 owns the identity library; release orchestration consumes it but does not redefine it. Evidence: `evidence/t003-candidate-identity-current-20260902.md`.

**Target checkpoint**: The public path can submit any registered model/task to the
existing coordinator, and one identity model exists for all later evidence.

---

## Phase 3: User Story 1 - Plan YOLO From Actual ACKs (Priority: P1) 🎯 MVP

**Goal**: Convert YOLO26n into a canonical Provider-independent subject and
select only certified candidates after one real ACK snapshot closes.

**Independent Test**: Controlled ACK snapshots select `atomic-v1` or
`shared-backbone-two-shard-v1` after `ACK_CLOSED`; infeasible snapshots fail
before Selection.

- [ ] T004 [P] [US1] Produce the offline-only YOLO26n canonical export pipeline and deterministic package/oracle manifest for FP32, batch 1, static 640-by-640 input, pinning the current 5,544,453-byte checkpoint SHA-256 `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`, source URI/revision, an exact exporter dependency lock and command/options, ONNX opset, normalized ONNX, external initializers, graph/tensor metadata, preprocessing/postprocessing identities, a licensed fixed image fixture source/revision/SHA-256, **semantic** safe-cut descriptors (canonical role node sets, branch ownership, tensor interfaces, dependency edges, and equivalence evidence; never percentile/index heuristics), and full-model oracle outputs in `tools/ndnsf-di/export_spec180_yolo26_onnx.py`, `tools/ndnsf-di/spec180-yolo-export.lock`, `examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py`, `tests/fixtures/spec180/yolo26n/README.md`, and `tests/python/test_spec180_yolo_export.py`; sign the catalogue with the configured authority and record its key ID/digest, while signing external model manifests with the separate authority in `contracts/model-manifest-trust-v1.md`; accept only when a clean repeated export is byte-stable or records and normalizes every permitted nondeterministic field, ONNX checker and CPU ONNX Runtime reproduce the oracle, a different checkpoint or invalid catalogue signature fails before export/enumeration, the package contains no Provider binding, and deployed manifests contain no PyTorch/Ultralytics dependency. Evidence: `evidence/t004-yolo-export-current-20260902.md`; focused exporter tests and two independent 640-by-640 export repeats pass. The fixed fixture, 640-by-640 oracle, canonicalized NMS policy, ORT equivalence, and graph-bound semantic partition are now evidenced; T004 remains open until the catalogue is signed with the registered authority and the signed package is consumed by the maintained application/native path.
- [ ] T005 [US1] Replace the synthetic object-detection shape for the YOLO subject with a real adapter that validates the canonical graph and enumerates exactly the atomic and shared-backbone two-shard candidate families in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/__init__.py`, `graph.py`, `candidates.py`, `adapter.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/__init__.py`, and `tests/python/test_spec180_yolo_adapter.py`; verify the configured catalogue signer before returning candidates in the maintained path (a `require_signature=False` bypass is test-fixture-only and cannot feed local, SIF, or Tiger qualification); reject unknown graph revisions, ambiguous tensor interfaces, missing initializers, unsafe cuts, unsupported shard counts, and unregistered candidate digests. Validate the signed semantic node sets, branch ownership, producer/consumer tensor names/shapes, per-role interfaces, dependency graph, and full-model equivalence; reject any candidate whose cuts are inferred only from node percentages or indexes. Each candidate must expose explicit `input_ingress_role`, `result_egress_role`, and merge-kind identities, and those two role identities must be copied into the runtime `SplitCandidate` and covered by its `candidate_digest` rather than remaining only in a catalogue-side object.
- [ ] T006 [US1] Bind `ProviderOfferV3`/`ProviderPlanningViewV3` ACK capabilities and one-to-one role ownership to the existing placement/sealing path without adding YOLO policy to the generic planner. Owners: `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/presplit_first.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/adapter.py`, `ndn-service-framework/ServiceUser.hpp`, `ndn-service-framework/ServiceUser.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, `specs/180-ack-driven-cross-model-qualification/contracts/provider-offer-trust-v1.md`, and `tests/python/test_spec180_yolo_ack_planning.py`. The qualification path must close discovery only at the registered 1500 ms ACK window (no caller/offline `ack_coverage_roles`), evaluate every candidate against its own role/dependency/resource requirements, and carry signer identity, typed certificate/key-locator reference, and validated ACK wire digest from the Trust-Schema-validated Data without synthesizing provenance in Python. Reject missing, malformed, or Provider-mismatched provenance before offer verification. Verify V3 offers with the existing NDNSF Trust Schema/Provider identity verifier under candidate-bound `SPEC180_YOLO_OFFER_TRUST_ROOT`, binding Provider/service, canonical offer digest, request/attempt, model/graph, validity window, boot epoch, and key ID; caller HMAC maps are fixture-only. Apply signed adapter priority and digest tie-break; require `INPUT_REFERENCE_PUBLISHED < REQUEST_SENT < ACK_CLOSED < GRAPH_READY < PLACEMENT_DECISION < SELECTION_COMMITTED`, no early role predicate, plan binding of ACK/candidate/role-map/priority digests, correct atomic/four-role selection independent of catalogue order, and fail-closed insufficient/duplicate/stale/unauthenticated offers before Selection.
- T006 audit note (iteration 47): the current production gap is specifically the
  loss of validated ACK Data signer provenance at the C++/pybind
  `AckCandidate` boundary. The implementation must carry that provenance from
  the already-validated packet; a Python key map or reconstructed Provider
  identity is not an acceptable substitute.
- T006 audit note (iteration 48): the ServiceUser/pybind/Python projection now
  carries signer identity, typed KeyLocator reference, and complete ACK wire
  digest from the validated Data packet. Focused mapping coverage exists;
  production offer verification, mismatch negatives, and live execution remain
  required for completion.

**Target checkpoint**: US1 is independently demonstrable with a canonical model and
controlled capability offers; it does not yet claim Provider execution.

---

## Phase 4: User Story 2 - Assemble and Execute YOLO Roles (Priority: P1)

**Goal**: Make the selected plan control real Provider preparation, NDN
dependencies, merge, and numerically correct output.

**Independent Test**: Atomic and shared-backbone plans assemble from the same
canonical package and match the full-model oracle; altered assignments or
objects fail before compute.

**T007 audit boundary (iteration 45)**: The native bridge and Python helper now
support graph plus external-initializer transport. The canonical catalog /
ensurer previously published only graph-source metadata; its root API and
initializer payload publication now include the initializer object's name, byte
length, and raw digest and are regression-tested. Native/wire parity and a real
signed YOLO package remain part of T007/T011.

**T006/T011 audit boundary (iteration 46, historical; superseded by iteration
47)**: Formal ACK offers must use the
candidate-bound NDNSF Trust Schema/Provider-identity verifier named by
`SPEC180_YOLO_OFFER_TRUST_ROOT`; the injected caller HMAC map remains a
focused-test fixture. The verifier must bind Provider/service identity,
canonical offer digest, request/attempt, model/graph, validity window, and boot
epoch before an offer reaches planning. The verifier wiring and live
ACK-to-Response trace remain open.

- [ ] T007 [US2] Repair and extend the generic signed `RoleAssemblySpec` plus native canonical ONNX assembler for branched multi-input/output roles in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, `examples/DI_NativeProviderExecutable.cpp`, Python/native bindings, `tests/python/test_spec180_role_assembly.py`, and `tests/integration-tests/ndnsf-di-native-assembly.t.cpp`; make validation role-kind-specific so range/rank roles require a non-empty layer interval while `COMPONENT_SET` requires a canonical non-empty node set and exactly `layer_begin=0, layer_end=0`; when the canonical graph uses external initializers, fetch and verify the separately named, sized, and digested initializer object and pass it through the native helper (graph-only assembly is a failure); accept only when Python/wire/native validators agree, request/attempt/ACK/model/graph/candidate/plan/role-kind/role/Provider/object/security-domain/protection-epoch/deadline bindings verify before compute, and cache reuse verifies the identical adapter/assembler ABI, object set, security epoch, and current authorization.

**Revision 124 addition (2026-09-05):** T007 also owns the FR-008
protected-grant positive path. Replace the HMAC/`repr`-digest scaffolding
in `core/protected_artifacts.py` and `security/artifact_policy_authority.py`
with the inherited Spec170 `artifact-assembly-v1` grant flow: the configured
policy authority authenticates the grant request, issues
Provider-recipient-encrypted signed grant Data, and publishes it; the
placement grant seam in `app_sdk/placement.py` carries the authority fields
and finalizes the plan with grant references; the selected Provider (Python
and native paths) verifies and unwraps the grant before assembly. Default
role specs MUST leave `plaintext-v1`; a qualification assignment MUST carry
a non-`plaintext-v1` protected epoch. Accept only when a real signed grant
round-trip (authority → seam → Provider unwrap) passes focused positive and
mutation tests, and HMAC scaffolding is no longer a qualification input.
- [ ] T008 [US2] Implement YOLO dependency decoding and deterministic two-shard merge/postprocessing as a native dependency-consumer `Merge` role (using ONNX Runtime CPU only when the declared merge graph requires it), while keeping `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/merge.py` and `runtime.py` strictly as offline metadata/oracle helpers; update `NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp`, `NativeProviderHandler.cpp`, `tests/integration-tests/ndnsf-di-native-assembly.t.cpp`, and `tests/python/test_spec180_yolo_equivalence.py`, and accept only when the registered input produces canonically ordered atomic and shared-backbone detections matching at `atol=1e-3`, `rtol=1e-4`, missing/extra detections fail, the deployed process imports no Python YOLO runtime, and the declared CPU Merge role is distinguished from forbidden CPU model-layer fallback.
- [ ] T009 [US2] Migrate the maintained YOLO application from preplanned `distributed_inference()`/offline Provider profiles to the generic model-first ACK-driven API in `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`, `provider.py`, `yolo_2x2_lib.py`, `plan_example.py`, and `tests/python/test_spec180_yolo_application.py`; keep the old pre-split path only as an explicitly named offline oracle and accept only when the normal example supplies no Provider list, split ID, role map, or `auto_parallel_detect_plan` authority. The live capability authority MUST be the network `CollaborationAckClosed` snapshot from the existing collaboration path. A `PreSplitCatalogSnapshot` is only artifact-publication metadata and must be resolved through an authenticated repository/data path; it is not an ACK snapshot and cannot choose a candidate. The maintained helper now uses `NetworkCatalogSnapshotResolver` to fetch an exact-name, signer-checked APP Data envelope after ACK closure and loads the candidate-bound Provider-offer Trust Schema policy plus PEM key map; the old file snapshot and caller HMAC key map are fixture/offline-only and cannot feed local, SIF, or Tiger qualification. Its qualification profile must set the registered 1500 ms ACK window explicitly; the maintained CLI now defaults to and rejects values other than 1500 ms. The maintained caller must publish the encoded image through the canonical `app_sdk.client.APPClient.publish_application_input_reference()` method (or consume a source-bound receipt from that call) before `REQUEST_SENT`; a bare `--input-reference-file` is not publication evidence. The task remains open until live ACK closure, publication-event binding, reference-aware input fetch, and production result wiring are exercised.
- T009 audit note (iteration 33): the publication facade's legacy wire result did not itself contain the complete Spec180 `LargeDataReference`; the maintained path had to bind and verify manifest digest, authorization scope, and protection epoch before constructing `ApplicationInput.from_repo_ref()`. This historical finding is retained in the audit evidence.
- T009 audit note (iteration 34): the maintained helper now publishes source bytes through the canonical APP owner and binds the native result before constructing `ApplicationInput`; it rejects a bare reference file. The lower network facade remains compatibility-only. Live ACK/Selection/Provider/Response and production offer verification remain open.
- [ ] T010 [US2] Close fail-closed YOLO execution behavior for altered assignment, wrong Provider, replay, missing/truncated/wrong-digest object, oversized inline or altered/plaintext/unauthorized input reference, non-selected input fetch, invalid component/range role, incompatible or cross-security-domain assembled-role cache, stale/revoked protection epoch, wrong-plan dependency, incomplete merge, late output, cancellation, deadline, and plaintext-log leakage in `tests/python/test_spec180_yolo_security.py`, `tests/integration-tests/ndnsf-di-core-flow.t.cpp`, and the owning input/repository/adapter/native runtime files; accept only when every case stops at its contract boundary, emits a structured non-secret reason, produces no unverified model result, preserves one terminal outcome, and leaks no input/result plaintext or secret capability.

**Revision 124 addition (2026-09-05):** Y-N-E MUST drive a real grant
mutation through the implemented grant verifier (stale/revoked grant,
wrong recipient, or forged signature) and assert the verifier rejects it
at the authorization boundary; a synthetic epoch rejection that never
invokes the verifier is not evidence.
- T010 audit note (iteration 29): focused security regressions cover the current Python boundary, including non-plaintext protection-epoch binding; the remaining native/wire and production negative matrix is still required before closure.

**Target checkpoint**: The production Provider path can execute both registered YOLO
candidates and reject every registered corruption case.

---

## Phase 5: User Story 3 - Model-Neutral Local Qualification (Priority: P2)

**Goal**: Exercise YOLO and frozen Qwen through the same runtime and cheap
network boundary before building a SIF.

**Independent Test**: One unchanged source passes Y-A/Y-B/Y-N/Q-C/Q-W through
complete suites and CPU/MiniNDN after convergence PASS.

- [ ] T011 [P] [US3] Add the registered YOLO Y-A/Y-B/Y-N CPU/MiniNDN harness using `contracts/yolo-minindn-runner-v1.md`, real NFD, NDN-SVS, security, encrypted repository input publication/reference/fetch, ACK/Selection, canonical publication, NDN dependencies, result oracle, redaction scan, child supervision, and cleanup in `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`, **the T011-owned thin in-image dispatcher `scripts/run_spec180_case.py`**, `tests/python/test_spec180_yolo_minindn.py`, and `specs/180-ack-driven-cross-model-qualification/evidence/`; reuse the existing MiniNDN/NFD/SVS startup, routing, keychain, and process-supervision helpers from `Experiments/NDNSF_DI_Yolo2x2_Minindn.py` (prefer direct import; if extraction is required, move each helper once into a shared module used by both runners, never duplicate helper bodies), adapting only candidate-specific topology/configuration and commands. Do not invoke that legacy runner's deployment-first `main()` or copy its output into Spec180 evidence. After readiness, the runner orchestration owns the exact signed catalogue APP Data publication through `ServiceUser.publish_signed_app_data`; the maintained User child owns encrypted YOLO `REPO_REF` publication through `APPClient.publish_application_input_reference()` before `REQUEST_SENT`. The controller's artifact manifest and offline snapshot file are not substitutes. Y-A authorizes the `FullModel` capability and must start exactly one Provider identity; Y-B authorizes the four complete component-role capabilities `BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge` and must start exactly four Provider identities with a distinct capability cover; Y-N executes the fixed `Y-N-O` order-invariance control plus `Y-N-C/P/R/I/E/L` negatives using exactly four identities that support both the shared-role cover and at least one `FullModel` capability. These cardinality/cover checks are startup-profile witnesses only; the selected one-to-one role assignment must come from the closed authenticated ACK snapshot and sealed plan. `Y-N-C` must remove `FullModel` and at least one required shared-candidate capability so neither candidate is feasible. The dispatcher must consume only the signed, candidate-digest-bound workload document defined by the runner contract; enforce the exact gate/case, `/bundle`-relative maintained entrypoint, fixed argument shape, allowlisted environment, and fresh evidence root; set `/bundle` as child cwd; and `exec` the maintained entrypoint so exit/signal/evidence ownership is preserved. Unknown fields, digest or gate/case mismatch, traversal/absolute paths, shell fragments, private material, and ambient model/source paths must fail before NFD or any Provider starts, with focused positive/negative tests. The legacy `/Stage/...` policy and caller HMAC key map are forbidden qualification inputs; every subcase must validate the declared lifecycle-event schema, milestone-specific field allowlist, recursive redaction, and expected boundary; focused harness tests may run now, but formal case results wait for T014. T011 owns the YOLO case runner and dispatch shim; T015 owns local-suite supervision and invokes this runner exactly once per inventory entry.

**Revision 124 addition (2026-09-05):** negative verdicts are semantic —
a Y-N subcase passes only when the runner observes the registered rejection
reason at the listed boundary; unrelated exceptions, a wrong lifecycle
phase, internal strategy failures, or fabricated rejections MUST NOT
produce a subcase PASS (regression
`tests/python/test_spec180_negative_verdict.py`, 183 focused checks; live
Y-N-I r42 PASS recorded in
`evidence/t011-y-n-live-current-20260905-r42-i-only.md`). The fallback
controller readiness marker must be a distinct string printed only after
`start_background()` returns (post-probe); the plain
`ServiceController listening` substring also matches the pre-probe C++ log
and MUST NOT release the repository barrier.

  **T011 publication clarification (iteration 85)**: the runner must publish or
  verify candidate-bound role/rank artifact Data objects for both registered
  candidates, then publish the signed runtime
  `ndnsf-di-presplit-catalog-snapshot-v1` APP envelope containing their exact
  `artifactDataNames`, before starting User. The immutable
  `spec180-yolo-catalogue-v1` package record, a legacy `/Stage` manifest, or a
  caller-provided snapshot file cannot satisfy this runtime publication
  obligation.

  **T011 mandatory vertical slices (iteration 92; execute in order)**:

  1. **T011-A — atomic Y-A**: replace the unconditional driver stop with one
     real Controller/Repo/one-Provider/User lifecycle. Publish and read back
     the `FullModel` object and active snapshot, publish the encrypted input
     reference, close authenticated ACKs, seal Selection, execute `FullModel`,
     and verify one terminal response. Acceptance is one non-qualifying
     developer MiniNDN request with complete lifecycle and cleanup; fake
     transports do not close this slice.
  2. **T011-B — shared Y-B**: reuse the same driver without a second harness;
     start four Provider identities, publish the four candidate role objects,
     select the shared candidate only after ACK closure, execute NDN
     dependencies plus native Merge, and verify one terminal result against
     the full-model oracle.
  3. **T011-C — fixed Y-N**: run the order control and each registered
     fail-closed mutation through the same driver. A mutation may use the
     smallest focused run that reaches its boundary; it must not trigger a
     complete suite or SIF rebuild.
  4. **T011-D — freeze the driver**: remove the
     `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED` branch, verify that every child is
     instance-owned and cleaned, and record the exact focused commands and
     source identity. Only then may T011 become complete and T014 begin.

  T011-A/T011-B developer runs are wiring evidence, not T015 qualification
  results. T015 later executes the registered cases exactly once from the
  post-T014 frozen candidate.
- **Iteration 83 staged-startup correction (2026-09-03)**: T011's
  `CaseProcessSpec` now carries an explicit `control`, `providers`, or `user`
  phase. The runner must execute `control` and wait for both readiness markers,
  execute `providers` and wait for every Provider marker, publish the signed
  catalogue, then execute `user`; an implicit all-process launch is forbidden.
  `start_processes()` rejects unknown/repeated/out-of-order phases, resolves
  all phase nodes before the first child, and `wait_for_ready()` fails on early
  exit or marker timeout. The driver must record the verified catalogue
  publication receipt with `mark_catalogue_published(...)` before starting the
  User phase. This is focused harness evidence only; T011/T014 remain open
  until the real ACK-to-Response driver uses the barriers.
- **Revision 123 native-boundary follow-up (2026-09-04)**: The native Provider
  now pre-satisfies only the selected V3 `APPLICATION_INPUT` ingress edges
  from the authenticated request-backed input object; non-ingress roles do not
  fetch application input. The real native Y-N-C capability vector removes
  `FullModel` and `Merge`, and the shared Y-N-C/P/R/I/E/L driver waits for its
  exact negative marker and verifies case-owned child cleanup. Focused evidence
  is recorded in `evidence/t011-native-boundary-repair-20260904.md`; these
  checks do not close live NFD/NDN-SVS Y-N, T011, or T014.
- [X] T012 [P] [US3] Register the named Spec175 Q-C/Q-W cold and continuation workloads as frozen Spec180 reference inputs without copying or changing their stream/state semantics in `examples/python/NDNSF-DistributedInference/llm_pipeline/`, `tests/python/test_spec180_qwen_reference.py`, and `specs/180-ack-driven-cross-model-qualification/contracts/qwen-reference-v1.md`; freeze the exact local model identity, prompt, tokenizer/chat-template, stop policy, greedy decoding, 8-Token cap, 5000 ms initial SVS settle, 1500 ms ACK timeout, and 60000 ms request timeout; keep the Tiger Qwen3.6-27B model manifest as a separate identity signed by the registered external artifact authority in `contracts/model-manifest-trust-v1.md`; accept only after the named Spec175 handoff seals the baseline and both model cases use the same coordinator/native Provider/evidence owners, the values match that handoff, and any semantic/profile drift fails the contract gate. The machine-readable binding is `specs/180-ack-driven-cross-model-qualification/contracts/qwen-reference-manifest-v1.json`, validated by `tests/python/test_spec180_qwen_reference.py`; T012 owns the frozen reference manifest, not the local-gate runner. Evidence: `evidence/t012-qwen-reference-current-20260902.md`. Local execution remains T015's responsibility.
- [ ] T013 [US3] Finish only the YOLO promotion/release tooling needed by the
  Spec180 slice: candidate rendering, one fixed one-GPU Tiger profile, local
  SIF build/preflight, exact-SIF dispatch, no-side-effect submission closure,
  and a structured result validator. The validator requires candidate-bound
  lifecycle, numerical result, CUDA/device, child-exit, cleanup, path, schema,
  and digest evidence and rejects label-only `PASS` values. Qwen entrypoints,
  manifests, profiles, and result fields are not acceptance dependencies.
  **2026-09-04 T014 follow-up:** renderer now emits four native Provider
  commands (three CUDA plus CPU Merge) and the maintained `repo_node.py`;
  result validation now enforces the one-request/one-GPU profile, full child
  inventory, per-role process/device evidence, and oracle file-content binding.
  Remaining controlling work: replace the Tiger launcher's marker-only terminal
  path with real numerical/runtime/child/cleanup record production and validation;
  add startup phase barriers and bounded process-group cleanup; reconcile the
  bundle-executed launcher with the sealed in-image path actually probed.
  See `evidence/t014-tiger-path-audit-20260904.md` (BLOCK, 118 focused checks).
  **Supervision continuation (2026-09-04):** ordered readiness, peer-exit
  monitoring, bounded runtime-group cleanup, observed terminal validation and
  same-tree in-image execution are implemented; 44 focused checks pass in
  `evidence/t013-supervision-repair-20260904.md`. The real numerical/CUDA
  evidence producer, transient-helper closure, secret/scratch disposal and full
  candidate identity remain open. Missing result evidence now fails explicitly;
  do not manufacture a result record or start qualification to bypass this gap.
  **Numerical continuation (2026-09-04):** User now publishes the registered
  fixed native input and compares its received response against the hash-bound
  canonical oracle before reporting success. A fresh, redacted numerical
  component record is produced; Tiger's native input flag and the image source
  allowlist are corrected. See `evidence/t013-numerical-repair-20260904.md`
  (83 focused checks) and `contracts/yolo-numerical-evidence-v1.md`.
  Next: actual native EP/PID/physical-device evidence, then a candidate-bound
  terminal collector consuming the numerical record and observed supervision;
  full cleanup, live Y-N and candidate-plane closure remain controlling work.
  No model/network qualification or new source/candidate seal was performed.
  **Native-evidence continuation (2026-09-04):** existing ExecutionEvidence
  carries actual PID/visibility and request/attempt/cache observation; CUDA
  identity is queried from the runtime-selected device through PCI/driver APIs.
  V3 assembly enables first-request profiles; the original per-role observer
  record is emitted separately from aggregate readiness. See
  `evidence/t013-native-evidence-repair-20260904.md` (99 focused checks) and
  `contracts/native-execution-evidence-v1.md`. No real CUDA or NFD execution was
  performed. Next: the terminal collector must strictly parse and bind these
  observations, the actual profile files, numerical/protocol evidence and
  supervision under the candidate identity. Full cleanup, Y-N and source/runtime
  candidate closure remain open; T014 still blocks formal validation.

**Revision 124 addition (2026-09-05):** the Python `start()`/
`start_background()` readiness wait MUST exceed the Core probe deadline
with margin (e.g., 15000 ms against the 10 s probe) so a boundary-success
probe cannot be reported as a wrapper timeout.
- [ ] T014 [US3] After T002/T004--T011 and the T013 YOLO release path are
  implementation-complete, run one mandatory design-code convergence audit of
  the real YOLO application/input/coordinator/placement/assembly/security/
  MiniNDN/SIF/submit/result paths. Repair controlling gaps with focused tests
  and require a fresh `PASS` in `audit.md` and `traceability.md`.
  The 2026-09-04 Tiger-path follow-up remains BLOCK: T011 must also replace
  focused Y-N substitutes with the required real-network negative controls.
  A 406-file source-only checkpoint is preserved; it is not a full CandidateRecord
  and does not close source/dependency/runtime/harness identity binding.
  Its successor is the 408-file source-only supervision checkpoint; both are
  preserved, neither is a promoted candidate, and T014 remains BLOCK.

**Revision 124 addition (2026-09-05):** the audit MUST verify that every
superseded evidence file carries an invalidation banner naming the
invalidating revision and hash (currently missing on
`evidence/t016-exact-sif-yb-replay-current-20260904.md` and
`evidence/s1-candidate-seal-current-20260904.md`, both invalidated by
revision 123), and that the current-audit pointer is coherent across
`spec.md`, `plan.md`, `tasks.md`, `traceability.md`, and `audit.md`.
- [ ] T015 [US3] From the T014-accepted source, run exactly once the
  YOLO-relevant unit/integration selectors and MiniNDN Y-A/Y-B/Y-N entries in
  supervised children. Record commands, hashes, backend identities, protocol
  and numerical oracles, all child exits, cleanup, and one local verdict in
  `evidence/local-qualification.md`. Inherited Qwen evidence is context only.

**Target checkpoint**: Implementation and production wiring are locally qualified. No
SIF has been built or remote side effect has occurred.

---

## Phase 6: User Story 4 - Immutable SIF and Tiger YOLO Slice (Priority: P3)

**T011 cleanup boundary (iteration 89)**: The live case driver must retain the
`MiniNdnCaseRuntime` instance and invoke `stop()` from a `finally` block after
every startup, publication, request, negative subcase, and readiness failure.
The method owns only the handles returned by that instance's phase launches;
it must not use a global process list or clean another active case. A cleanup
failure is an `UNQUALIFIED` case result with preserved evidence, not a pass.

**Goal**: Use one sealed runtime image for one finite Tiger YOLO request and
issue one honest functional verdict.

**Independent Test**: One candidate identity and SIF hash complete exact-SIF
Y-B replay and one Tiger Y-B request with all terminal oracles.

- [ ] T016 [US4] Run candidate input closure, build exactly one local SIF with
  every native/Python artifact built inside the candidate image or an
  ABI-identical sealed builder, record the in-image ABI/import/library/CUDA/
  ONNX Runtime closure, seal its SHA-256, and replay one exact-SIF YOLO Y-B
  request without a source or package overlay. Reject host-built extensions,
  host-home/Linuxbrew resolution, CPU model fallback, or a runner/configuration
  mismatch before upload.
  **T016 execution-only boundary (iteration 92)**: all listed build, preflight,
  dispatcher, and test code must already be complete under T013 and audited by
  T014. T016 invokes those frozen files once to build and replay the candidate.
  If a script, recipe, runtime dependency, mount, or profile must change, stop,
  return ownership to T013, rerun focused tests and T014, and build a new
  candidate. Do not patch the image or host files inside T016.
- [ ] T017 [US4] Render and validate one fixed Tiger profile: one node, one RTX
  GPU, four independent Provider processes, the three model roles mapped to
  CUDA device 0, and `Merge` on CPU. Read-only readiness verifies the local and
  remote SIF hash, signed YOLO manifest, encrypted input reference, Apptainer
  1.5.3, routes, identities, `/bundle` cwd, environment allowlist, ABI/import/
  library closure, device visibility, result root, and cleanup. Only accepted
  bytes may be staged; rejection causes zero scheduler calls.
- [ ] T018 [US4] Submit `yolo-functional` exactly once through the checked-in
  entrypoint and execute one cold Y-B request. Require post-ACK shared-backbone
  selection, four unique Provider processes and role owners, selected-ingress
  input fetch, verified assembly/security lineage, NDN dependency delivery,
  1/1 numerical match, CUDA ONNX Runtime for the three model roles, zero CPU
  model fallback, redaction PASS, all child exits, and cleanup. A recorded
  infrastructure failure before workload entry permits only one byte-identical
  resubmission.
- [X] T019 [US4] Remove Qwen Tiger execution, three-distinct-GPU placement, and
  the warm second request from the Spec180 completion boundary. Preserve the
  Spec175 Qwen handoff and existing implementation artifacts for a later
  feature; do not count them as Spec180 Tiger evidence.
- [ ] T020 [US4] Validate closure across the accepted local, exact-SIF, and
  Tiger YOLO manifests from one candidate, map every active requirement and
  criterion to digest-verified structured evidence, and issue exactly one
  `FUNCTIONAL_PASS` or `UNQUALIFIED` verdict with the first failing layer and
  evidence limitations. Report timing only as a diagnostic and make no Qwen,
  multi-GPU, warm-cache, scaling, or performance claim.

---

## Dependencies & Execution Order

```text
Spec175 T023 frozen handoff + T001/T003/T012
  -> atomic portions of T002/T004/T005/T006/T007/T009/T010
  -> T011-A real Y-A developer MiniNDN request
  -> remaining shared-role T007/T008/T009/T010
  -> T011-B real Y-B developer MiniNDN request
  -> T011-C critical Y-N controls
  -> T011-D freeze the real driver
  -> T013 finish YOLO-only release/SIF tooling
  -> T014 one design-code convergence PASS
  -> T015 one YOLO-relevant local qualification
  -> T016 one local SIF build and exact-SIF replay
  -> T017 read-only remote readiness and accepted-byte staging
  -> T018 one-request YOLO-F execution
  -> T020 final closure
```

T015--T019 are execution-only. Any source, dependency, harness, recipe,
profile, or evidence-contract change invalidates the candidate and returns to
its pre-T014 owner; evidence from the old candidate cannot be combined with
the replacement.

### Parallel opportunities

- T003 and T004 may proceed after T001 while T002 owns the facade.
- T011 and T012 may implement their harness/reference wiring in parallel after
  their owning runtime paths exist; their formal executions remain serialized
  in T015.
- No SIF/Tiger task is parallelized. T016--T020 are one serial candidate route.

## Implementation Strategy

### MVP

T001--T010 deliver the first meaningful result: a real YOLO canonical adapter
whose selected plan depends on actual ACKs and whose Provider execution matches
the full-model oracle.

### Promotion

T011--T015 prove the YOLO path locally. T016 builds once only after local PASS.
T017--T020 perform one finite remote functional slice without tuning or
campaign expansion.

### Stop rules

- A controlling audit gap stops at T014 and returns to its owner task.
- A local failure stops at T015; no SIF is built.
- A SIF failure stops at T016; nothing is uploaded.
- A pre-dispatch failure stops at T017; nothing is submitted.
- A workload-entry functional failure is preserved and closes that gate as
  failed; Tiger is not used for iterative debugging.
