# Tasks: NDNSF-DI Verified Design Delivery

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md), [data-model.md](data-model.md), [traceability.md](traceability.md), [quickstart.md](quickstart.md), and all files under [contracts/](contracts/).

**Primary rule**: This is convergence work over substantial existing code. Every task starts by locating/running the named current owners and regressions. Preserve conforming code, make the smallest cohesive repair to partially conforming code, and create a new production owner only after documenting why no current owner can contain the gap.

## Task Execution Protocol

Every task follows this order; skipping an item means the task is not complete:

1. **Preconditions** — confirm required predecessor manifests, source/config identities, and unmodified authority documents.
2. **Current-owner check** — use CodeGraph on named symbols, inspect only focused extra paths, and run the smallest existing regression that claims the behavior.
3. **Gap record** — classify current behavior as conforming, partial, conflicting, or unverified and name the observable failing contract.
4. **Focused RED** — add or select a deterministic positive/negative test that fails for the confirmed gap. Do not rewrite already passing behavior to manufacture a RED state.
5. **Smallest cohesive implementation** — modify the current owner and compatibility boundary; do not add a parallel Spec174 subsystem.
6. **Focused GREEN** — pass new test plus named existing regressions and `git diff --check` on owned paths.
7. **Gate evidence** — write a bounded manifest with source/config/input/oracle hashes, exact command/status, and first failure. Inspection is recorded as `implemented`/`wired`, never `executed`.
8. **Exit review** — verify no later-gate work started and no unrelated user changes were staged, reverted, or overwritten.

If a task discovers a new wire field, name component, role type, transport, authority rule, SIF route, or oracle, stop. Update the spec, plan, data model, affected contract, quickstart, and tasks; rerun analysis/audit; then resume. Do not improvise in code.

## Phase 1 — Freeze Inputs And Reuse Map

### T001 — Freeze Spec174 authority and source identity

- [X] T001 Freeze the exact planning and source inputs in `specs/174-ndnsf-di-verified-delivery/`, `.specify/feature.json`, `docs/NDNSFDI/slides/main.pdf`, and a bounded `results/spec174/<run>/source-manifest.json`.
  - **Current inputs to reuse**: the active Spec Kit pointer, existing source-identity/build-record helpers, and current dirty-worktree evidence conventions; do not invent a second source-sealing format if an existing schema can be extended.
  - **Record**: design PDF SHA-256, spec/plan/contracts hashes, `HEAD`, tree identity, submodule/dependency revisions, NDN-SVS branch/commit, compiler/linker/Python/ORT/Boost identities, and an explicit allowlist plus digest for in-scope dirty files. Record unrelated dirty files as excluded paths without reading or staging their content unnecessarily.
  - **Reject**: changed design PDF, wrong active feature, unresolved dependency checkout, broad “dirty=true” without file hashes, or a source manifest that silently omits in-scope changes.
  - **Acceptance**: two consecutive source-manifest calculations over unchanged inputs are byte-identical except for explicitly excluded timestamp fields; `git diff --check` over the Spec174 and active-project instruction paths passes.
  - **Evidence**: source manifest and one concise authority-check report. No production code changes.

### T002 — Produce the current-owner, regression, and gap traceability map

- [X] T002 Verify and enrich the planning FR/SC inventory in `specs/174-ndnsf-di-verified-delivery/traceability.md` using CodeGraph and existing tests, with machine-readable run output under `results/spec174/<run>/`.
  - **Current owners to map**: all native/Python owners listed in `research.md`, existing `tests/unit-tests/distributed-inference-*.t.cpp`, `tests/integration-tests/ndnsf-di-core-flow.t.cpp`, `tests/python/test_spec170_*.py`, `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`, `Experiments/NDNSF_DI_Run_Minindn_Regressions.py`, and current SIF/Tiger scripts.
  - **For every FR/SC record**: current owner symbol/file, caller/entry path, current test, last current-session execution status (or `not executed`), classification (`conforming`, `partial`, `conflicting`, `unverified`), smallest target gap, intended task, negative case, and gate evidence.
  - **Mandatory runtime-path check**: distinguish production application entry paths from helpers, stubs, fixtures, legacy Spec170 alternatives, and historical evidence. A source symbol with no proven application wiring is `implemented`, not `wired` or `executed`.
  - **New-file rule**: every proposed production file must state why all existing owners are unsuitable. Default answer is to extend an owner.
  - **Acceptance**: 29 FRs and 11 SCs each map exactly once; no `unknown owner`, empty proof, or unassigned requirement remains; automated counts agree with `spec.md`.
  - **Evidence**: reviewed traceability table plus JSON hash. This task does not “fix” gaps.

### T003 — Freeze deterministic fixtures, oracle, fault schedules, and evidence schema

- [X] T003 Extend the current Spec170 fixture/evidence foundation into a Spec174 profile in `tests/fixtures/spec174/`, reusing `tests/fixtures/spec170/generate_cpu_onnx_fixture.py`, current evidence helpers, and current release-gate code instead of creating a competing harness.
  - **Fixture**: one small inspectable canonical ONNX graph with graph-valid one-role, four-stage pipeline, two-rank tensor, and `[1,2,1]` hybrid cuts; fixed input/seed; canonical artifact and unsplit output hashes; declared normalization only if exact byte equality is impossible.
  - **Topology/config**: fixed Provider identities, role/rank expectations, deadlines, NDN name schema version, resource peak vectors, and MiniNDN routes. Provider positions are deterministic and documented.
  - **Fault schedule**: exact packet/event index for loss, reorder, duplicate, conflicting duplicate, corruption, stale attempt/plan/epoch, replay, cancellation, and missing peer/rank. One fault dimension changes per case.
  - **Evidence schema**: implement/extend one owner for gate manifests and validate source/config/input/oracle/candidate identities, command/cwd/env allowlist, process tree, lifecycle outcome, hidden transport/fallback status, first failure, and bounded log references.
  - **Tests**: schema rejects missing identities, mutable/floating artifacts, secret/plaintext fields, inconsistent result hashes, overwritten PASS, and a later gate without predecessor PASS.
  - **Acceptance**: fixture regeneration is deterministic; ORT unsplit CPU result matches the frozen oracle in three fresh processes; schema positive/negative tests pass.
  - **Evidence**: fixture manifest, oracle hash, fault-matrix hash, evidence-schema test report.

## Phase 2 — User Story 1: Execute One Dynamically Placed Request

**Story outcome**: the existing NDNSF-DI lifecycle converges on one ACK-closed, pre-split-first, one-to-one plan and completes one application result without application-supplied Provider/stage placement.

### T004 — Converge ACK closure, PreSplitFirst, plan sealing, and Selection projection

- [X] T004 [US1] Repair only confirmed lifecycle/placement gaps in `ServiceUser.{hpp,cpp}` (`CollaborationAckClosure`), `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py` (`PlacementPlanCoreV3` and its sealer/projection), `NativeExecutionPlan.*`, `core/v3_lifecycle.py`, `core/placement.py`, `app_sdk/presplit.py`, and `app_sdk/placement.py`.
  - **Existing regressions first**: run the focused Spec170 ACK-closure, placement V3, assignment V3, plan-sealer, default-application-path, and native-plan tests named by T002. Preserve all passing semantics compatible with Spec174.
  - **Required behavior**: immutable request/attempt-bound closure; deterministic pure proposal from sanitized snapshots; graph-valid split before Provider binding; trusted-core revalidation; bijective one-Provider/one-role mapping; committed plan before any Selection; least-authority Provider projection.
  - **No rewrite**: do not add another planner or coordinator. If native/Python placement disagree, make one canonical core contract and adapt the other boundary explicitly.
  - **Negative tests**: late/mutated ACK, duplicate Provider, missing role, one role across Providers, one Provider owning two roles, wrong closure/policy/model digest, incomplete tensor group, Selection before commit, non-deterministic identical-input proposal.
  - **Acceptance**: one-role and four-role plans have stable canonical digests; invalid proposals publish zero Selection; application request contains no Provider list/stage map; focused old/new C++ and Python tests pass.
  - **Evidence**: lifecycle trace through `ACK_CLOSED -> PLAN_COMMITTED -> SELECTION_PUBLISHED` plus rejected-case authority counts.

### T005 — Converge Provider-local canonical artifact assembly and ONNX Runtime execution

- [X] T005 [US1] Repair only confirmed post-Selection assembly/execution gaps in `NativeProviderHandler.*`, `NativeProviderRuntime.*`, `NativeProviderSession.*`, `NativeExecutionPlan.*`, current native model runner, `app_sdk/provider.py`, `app_sdk/canonical_artifacts.py`, `adapters/onnx/`, `backends/onnxruntime.py`, and distributed-repository owners.
  - **Existing regressions first**: canonical layers, content-addressed reuse, Provider assembly, ORT boundary, exact residency, runtime topology, and current native post-Selection integration tests.
  - **Required behavior**: exact Selection verification; canonical graph/initializer/adapter fetch by digest; complete local role assembly; graph/input/initializer/output coverage; fresh complete peak-vector admission; ONNX Runtime execution; no PyTorch/Transformers runtime requirement.
  - **Readiness**: a role becomes locally ready from its own assembly, grant/admission, and sealed dependencies; do not add a global preparation barrier.
  - **Cache rule**: reuse only on exact artifact/role/runtime/policy/authority compatibility; otherwise clean assembly/session creation.
  - **Negative tests**: wrong/missing initializer, corrupt artifact, opaque node falsely split, incompatible ORT provider, stale cache/session, incomplete peak vector, admission change, required PyTorch/Transformers import, execution before Selection.
  - **Acceptance**: one-role CPU request produces the frozen full oracle using Provider-local ORT; four selected Providers independently assemble only their roles; no unplanned runtime dependency or CPU/GPU fallback is hidden.
  - **Evidence**: artifact/assembly/runtime identity hashes and local execution phase trace without model/plaintext bytes.

### T006 — Close final-response, retry, replan, cancellation, and stale-work fencing

- [X] T006 [US1] Converge recovery and completion behavior in existing native Provider/session/worker state machines, Python `core/recovery.py`, `core/state.py`, `app_sdk/execution_control.py`, runtime journal/status, and normal NDNSF response handling.
  - **Existing regressions first**: admission lifecycle, async runtime, default application path, dependency evidence, integrated flow, token/replay, and current cancellation tests.
  - **Required behavior**: transport retry preserves exact name and authority; local preparation retry cannot rebind a role; replan creates new attempt/plan/generation/group authority; cancel/restart fences old work; only the sealed complete result is accepted once.
  - **No speculative reselection**: Provider replacement after Selection is an explicit replan, not an automatic response-level retry that risks duplicate execution.
  - **Negative tests**: late old-plan output, duplicate final response, missing downstream role, Provider restart/old boot epoch, cancel during fetch/assembly/execution/publication, result that is only one stage/rank/token, permanent dependency failure.
  - **Acceptance**: every terminal path stops boundedly, releases execution authority, publishes no partial accepted response, and records the first failure; valid path returns exactly one complete oracle result.
  - **Evidence**: state-transition matrix and terminal authority/output counts.

## Phase 3 — User Story 2: Exchange Pipeline And Tensor Data Through NDN

**Story outcome**: existing dependency I/O, tensor codecs, role workers, and collective runtime execute pipeline, tensor, and hybrid graphs with all cross-Provider tensors carried by exact NDN Interest/Data.

### T007 — Freeze production tensor naming, manifest, segmentation, and consumer-pull behavior

- [X] T007 [US2] Converge `NdnsfCollaborationDependencyIo.*`, `TensorBundleCodec.*`, `AsyncDataflowRuntime.*`, current NDNSF messages/codecs, and Python runtime contracts on `contracts/ndn-tensor-dataflow-v1.md`.
  - **Existing regressions first**: role-dataflow contract, dependency-evidence, native-plan tensor bundle, async runtime, and current segmented/fetch tests.
  - **Required behavior**: producer-owned request-scoped names bind all required identities; signed manifest precedes segments; exact full-name/layout/size/digest verification; out-of-order acceptance; exact duplicate deduplication; same-name retry; one ready event.
  - **Production route**: integration/MiniNDN must call actual production codec/dependency owners, not a test-only serializer or shared-memory shortcut.
  - **Negative tests**: wrong producer/request/attempt/plan/generation/group/epoch/op/round/role/rank/tensor/microbatch/digest/segment; oversize/shape/layout mismatch; corruption; conflicting duplicate; late Data after cancellation; permanent missing segment.
  - **Acceptance**: canonical name/manifest golden fixtures are stable; all packet cases meet the contract; timeout reports first missing exact name; payload bytes never appear in evidence.
  - **Evidence**: name/manifest hashes, publish/fetch counts, retry/failure summaries.

### T008 — Converge one scheduler for pipeline, tensor-group, and hybrid execution

- [X] T008 [US2] Repair confirmed scheduling/collective gaps in `ProviderRoleWorker.*`, `AsyncDataflowRuntime.*`, `CollectiveRuntime.*`, `NdnsfCollectiveControl.*`, `ProviderGroupCoordinator.*`, and Python hybrid/parallel placement owners.
  - **Existing regressions first**: collective runtime, cross-provider group, hybrid execution, hybrid native bundle, group capability, and current integrated-flow tests.
  - **Required behavior**: dependency-driven readiness; explicit tensor-rank roles; sealed membership/world size/operation/round/result owner; computed merge as explicit role; no global readiness barrier; no rank shrink or independent rank failover.
  - **Reference cases**: four-role pipeline; two-rank group; four-Provider `[1,2,1]` hybrid. Every role/edge/group comes from one committed plan.
  - **Cross-Provider enforcement**: reject hidden TCP/RPC/shared-file/opaque NCCL data paths in acceptance configurations; local collective math is allowed only after named NDN inputs are received.
  - **Negative tests**: duplicate/missing rank, wrong membership/world size/round, premature next round, missing partial, implicit merge, no-progress and hard deadline, group cancellation/replan.
  - **Acceptance**: all reference graphs produce the full oracle; each Provider owns one role; observed NDN edges equal the sealed edge set; no unrelated Provider blocks local readiness.
  - **Evidence**: role/edge/group execution graph, round transitions, and transport classification.

### T009 — Converge security and protected-runtime enforcement on every dataflow path

- [X] T009 [US2] Repair confirmed authorization/key-lifecycle gaps in `ProtectedRuntime.*`, `ProviderGroupCoordinator.*`, current NDNSF permission/token/NAC-ABE/message owners, and existing plan-security/protected-artifact Python modules.
  - **Existing regressions first**: protected-runtime native tests, artifact security, group capability, token-handshake/replay, NAC-ABE routing, admission lifecycle, and zeroization tests.
  - **Required behavior**: normal NDNSF security plus exact Provider grant and local group capability binding; fresh local admission; only local required key projection; plaintext enters ORT only after authorization; terminal release/zeroization on every path.
  - **Cache/residency**: exact authority and compatibility evidence required; model label, shape, or previous tenant alone cannot authorize reuse.
  - **Negative tests**: every wrong identity/binding listed in `ownership-security-v1.md`, expired/replayed token/grant/capability, wrong wrapped-key commitment, old boot epoch, cross-tenant session/cache reuse, cancellation/error during protected execution.
  - **Acceptance**: valid pipeline/tensor/hybrid roles execute; every invalid case reaches ORT zero times, accepts zero output, releases leases, and reports zeroization without exposing protected data.
  - **Evidence**: non-secret validation outcome matrix and lease/zeroization terminal status.

## Phase 4 — User Story 3: Prove Correctness Locally

**Story outcome**: current and repaired behavior passes deterministic unit, production-path integration, real MiniNDN+CPU, and exact local SIF gates. Gate work may improve its harness/fixture owner, but a production failure reopens T004–T009; it is not patched ad hoc inside a deployment script.

### T010 — Close Gate U from one consistent build

- [X] T010 [US3] Complete the unit gate by extending existing `tests/unit-tests/distributed-inference-*.t.cpp`, `tests/python/test_spec170_*.py`, and current Waf/pytest registration only where T004–T009 left an explicit proof gap.
  - **Precondition**: T004–T009 focused tests pass and traceability has no unowned unit requirement.
  - **Build discipline**: one consistent compiler/linker/Boost/NDN-SVS/ORT environment; record configure/build/linkage; do not hide Linuxbrew/system mixing with ad hoc flags.
  - **Suite list**: replace the quickstart wildcard with explicit registered suite/case names and verify test discovery. A required skip/xpass is failure unless the spec explicitly marks the environment unavailable.
  - **Three-process rule**: deterministic fixture/oracle checks that depend on process state run in three fresh processes; ordinary pure unit cases need one clean gate run.
  - **Acceptance**: FR-023 coverage matrix is complete; old relevant regressions and new tests pass; every negative asserts zero accepted output/authority; no hang or orphan process.
  - **Evidence**: Gate U manifest and concise suite/assertion counts, not full redundant raw output.

### T011 — Close Gate I with production components and four Providers

- [X] T011 [US3] Converge `tests/integration-tests/ndnsf-di-core-flow.t.cpp` and `ndnsf-integration-fixture.{hpp,cpp}` through their existing production suites (`Spec170NdnsfDiCoreFlow` and `Spec170NativePostSelection`); a duplicate Spec174 suite is unnecessary because the current owner already exercises the required path.
  - **Precondition**: exact Gate U PASS manifest for the same source/config/fixture identities.
  - **Positive cases**: one-role, four-role pipeline, two-rank tensor, four-Provider hybrid, exact complete oracle.
  - **Production components**: application-facing REQUEST path, real ACK closure/planner/sealer/Selection projection, production codecs/handlers/state machines, Provider-local assembly/ORT, dependency scheduler, protected runtime, normal RESPONSE.
  - **Fault matrix**: all FR-024 deterministic packet/state cases; one dimension per case; process-tree/no-progress/hard-deadline monitoring.
  - **Repeat**: three independent binary processes per mandatory acceptance case with identical frozen inputs; reset PIB/TPM/runtime/cache state according to fixture contract.
  - **Acceptance**: exact final result, exact edge publish/fetch set, one execution per role, zero partial/duplicate accepted result, bounded failure/teardown, no helper-only or hidden cross-Provider path.
  - **Failure handling**: reopen the owning production task and rerun its focused suite before repeating Gate I; do not patch around the failure in the fixture.
  - **Evidence**: Gate I manifest, per-run hashes, packet-case summary, first-failure diagnostic.

### T012 — Close Gate M in real MiniNDN+CPU

- [X] T012 [US3] Extend `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`, `Experiments/NDNSF_DI_Run_Minindn_Regressions.py`, `tests/python/test_spec170_real_minindn_gate.py`, and current process/evidence helpers into `tests/python/test_spec174_real_minindn_gate.py`; do not replace working topology/process helpers.
  - **Precondition**: exact Gate I PASS; verified local MiniNDN/NFD/ORT environment; no host-NFD contamination.
  - **Topology**: separate User/controller/repository and four Provider processes/namespaces, per-node NFD, deterministic routes, fixed Provider identities and fixture roles.
  - **Cases/repeats**: baseline, pipeline, two-rank tensor, hybrid, and selected negative cases; three independent clean MiniNDN processes per mandatory positive; deterministic fixed fault schedule.
  - **Required observation**: REQUEST through final RESPONSE, exact closure/plan/Selection identities, Provider fetch/assembly/ORT, every cross-Provider manifest/segment Interest/Data name, terminal result/oracle, no hidden file/socket/NCCL dependency.
  - **Harness checks**: all children use verified cwd and absolute/mounted artifact paths; startup readiness is explicit; timeouts kill process groups; cleanup proves no surviving NFD/Provider/User process.
  - **Acceptance**: FR-025 and SC-004/005 pass; host NFD/in-process-only routes are rejected; first missing/invalid event/name appears on failure.
  - **Failure handling**: production defect returns to T004–T009; topology/harness defect is fixed here with a regression. Do not increase timeout without phase evidence showing the prior bound is invalid.
  - **Evidence**: Gate M manifest and bounded per-node/event/packet trace hashes.

### T013 — Build and qualify the exact local SIF candidate

- [X] T013 [US3] Extend the existing `packaging/ndnsf-di-container/` local Apptainer route, source/build-record validators, runtime probes, and container tests to produce one Spec174 candidate; do not reintroduce Docker/OCI or Tiger-side materialization as the normal route.
  - **Precondition**: exact Gate M PASS for source/config/fixture identities and a bounded compute-node Apptainer version probe.
  - **Build**: seal source/dependencies; build native/Python components inside candidate/ABI-identical builder; reject host-built `_ndnsf.so`; write atomically to a new SIF path; never overwrite a candidate.
  - **Preflight**: Python 3.10/SOABI/import, `ldd`/RPATH closure, Boost 1.71, ndn-cxx/NDN-SVS Experimental, NFD/MiniNDN, ORT/provider inventory, CLI/cwd/artifact resolution, writable paths, secret scan, no runtime PyTorch/Transformers requirement.
  - **Execution**: run the same CPU baseline/pipeline/tensor/hybrid and required negatives inside the exact SIF with clean environment and explicit binds.
  - **Acceptance**: Gate C PASS binds source seal, build record, dependency closure, CPU results, and one SIF SHA-256; no missing/host-leaked library and no alternate image.
  - **Evidence**: candidate/build/ABI/runtime manifest plus SIF hash; raw intermediates are not canonical after manifest closure.

## Phase 5 — User Story 4: Qualify The Same Candidate On TigerCluster

**Story outcome**: TigerCluster only executes the exact local candidate, progresses through predeclared stages, and produces a complete single-GPU and cross-Provider multi-GPU answer without fallback or alternate transport.

### T014 — Enforce remote admission, hash promotion, and staged stop rules

- [X] T014 [US4] Extend current Slurm/Apptainer profiles, login/compute/storage/scratch/network preflights, `run-container.sh`, templates, and promotion evidence so Tiger refuses work without exact Gate C PASS.
  - **Precondition**: VPN/login available and T013 candidate `PROMOTABLE`; otherwise record truthful BLOCK without submitting a job.
  - **Read-only T0**: account/partition/QoS, nodes, storage/scratch, Apptainer, driver/GPU/ORT compatibility, inter-node routes, artifact/identity paths, cwd, process map, resource/time bounds.
  - **Promotion**: upload exact SIF/config/input/oracle/manifests, verify remote hashes before allocation, and render an immutable job script. No rebuild, package install, mutable overlay, or library replacement.
  - **Stop rules**: failed T0 blocks T1–T3; each subsequent stage consumes the predecessor PASS and stops later stages on failure; cleanup/finalizer always preserves bounded evidence.
  - **Tests before remote**: local unit tests for template substitution, path/cwd validation, hash mismatch, stage ordering, cleanup traps, secret scanning, and no Docker/OCI/remote-materialization normal path.
  - **Acceptance**: a dry-rendered job and read-only remote preflight prove every variable/path/hash/resource before the first workload job.
  - **Evidence**: T0/pre-promotion manifest or BLOCK report; no cluster mutation beyond authorized upload/allocation work.

### T015 — Qualify CPU/no-GPU rejection and single-GPU complete reference

- [X] T015 [US4] Run staged T1/T2 with the exact candidate through existing Tiger runner/provider/user binaries and evidence finalizer.
  - **T1 negative**: a GPU-required configuration on CPU/no-GPU resources fails closed with explicit provider/admission cause and zero CPU fallback or accepted response. An explicitly CPU-configured control may run separately and cannot satisfy T2.
  - **T2 positive**: one Provider owns one complete role, uses the intended ORT GPU execution provider, produces the complete application result, and exactly matches the frozen oracle.
  - **Resource evidence**: GPU identity, process-to-device assignment, ORT provider selection, bounded memory/host usage, no CPU fallback; resource observations are evidence, not correctness oracle.
  - **Repeat**: one diagnostic run may establish environment readiness but does not count; qualification requires exactly three independent fresh T2 runs with identical frozen candidate/input/config/oracle identities.
  - **Failure handling**: environment mismatch is T-stage BLOCK/FAIL; code/SIF/config defect returns to the cheapest local owner/gate and produces a new candidate. Never patch the remote SIF.
  - **Acceptance**: T1 rejects correctly; T2 returns the full oracle in every qualification repeat; stage-only/model-load/token-only success is rejected.
  - **Evidence**: T1/T2 manifests, Slurm/job/node/GPU/candidate identities, complete-result hashes, bounded logs.

### T016 — Qualify cross-Provider multi-GPU tensor execution over NDN

- [X] T016 [US4] Run T3 using the exact T2-passed candidate and existing multi-Provider process/routing scripts, with at least two distinct Providers owning distinct rank roles.
  - **Frozen plan**: exact ACK closure, pre-split-first plan, role/rank-to-Provider bijection, group/world/epoch/operation schedule, NDN dependency names, deadlines, and oracle.
  - **Execution**: Provider-local ORT on intended GPUs; every inter-Provider activation/partial/result observed through sealed manifest/segment Interest/Data; local collective math only after named inputs; complete final response.
  - **Prohibitions**: no shared-file tensor handoff, TCP/RPC push, cross-Provider NCCL, CPU fallback, independent rank failover, world-size shrink, manual copying of intermediates, or acceptance of stage/rank partials.
  - **Negative control**: wrong/missing rank or blocked NDN dependency terminates boundedly, names the first missing contract, and accepts no partial response; run only if frozen profile/resource budget includes it.
  - **Repeat**: exactly three independent fresh T3 jobs or allocations with identical candidate/input/config/oracle identities; all complete results match the oracle.
  - **Acceptance**: T3 PASS proves one role/rank per Provider, exact NDN edge coverage, intended GPU providers, zero fallback/unplanned transport, complete oracle result.
  - **Evidence**: T3 manifest, group/edge trace hashes, process/device map, candidate and final-result hashes.

### T017 — Close traceability, documentation, and readiness audit

- [X] T017 [US4] Reconcile every FR/SC in the T002 traceability map with current source and immutable Gate U/I/M/C/T evidence; update `specs/174-ndnsf-di-verified-delivery/`, relevant user/operator documentation, and the active context pointer without overstating results.
  - **Classification**: report `implemented`, `wired`, `executed`, and `measured` separately. A Tiger BLOCK remains BLOCK and cannot be rewritten as PASS from local evidence.
  - **Regression**: rerun the concise final acceptance selector and `git diff --check` on owned paths; verify no relevant prior regression was silently removed/skipped and no duplicate Spec174 subsystem exists.
  - **Evidence retention**: retain canonical summary/manifest/trace hashes/necessary logs; mark superseded/debug raw runs non-canonical and follow project retention rules without destructive broad cleanup.
  - **Spec Kit gates**: run deterministic cross-artifact analysis, code-aware audit, and active Context Mode authority health/indexing. Resolve every BLOCK before claiming complete.
  - **Acceptance**: all 29 FRs and 11 SCs map to current owner + passing proof or a truthful external BLOCK; completion is claimed only if every mandatory requirement passes and the exact candidate chain is intact.
  - **Evidence**: final traceability/closure report, audit verdict, commands, candidate/result identities, residual risks and next evaluation work.

## Dependency Graph

```text
T001 -> T002 -> T003
T003 -> T004 -> T005 -> T006
T004,T005,T006 -> T007 -> T008 -> T009
T004..T009 -> T010 -> T011 -> T012 -> T013
T013 -> T014 -> T015 -> T016 -> T017
```

- T005 may begin after T004 defines stable Selection/role contracts, but cannot close before T004.
- T007 may prepare codec golden cases after T003, but production integration waits for T004 plan identities.
- T009 may add focused protected-runtime negatives after T003, but full group/dataflow closure waits for T007–T008.
- No Tiger task is parallel with an unfinished local gate.

## Milestone Checkpoints

| Checkpoint | Required completed tasks | Observable deliverable |
|---|---|---|
| Reuse map frozen | T001–T003 | every requirement has existing owner/gap/test/evidence; deterministic fixture/oracle |
| Lifecycle closed | T004–T006 | complete one-role and four-role REQUEST-to-RESPONSE contract |
| Distributed dataflow closed | T007–T009 | pipeline/tensor/hybrid exact NDN dataflow and protected execution |
| Local source qualified | T010–T012 | Gate U/I/M immutable PASS chain |
| Candidate qualified locally | T013 | exact SIF digest and Gate C PASS |
| Cluster qualified | T014–T016 | T0–T3 chain, complete GPU results, no fallback |
| Feature closed | T017 | traceability/audit with no mandatory gap |

## Implementation Strategy

The first useful slice is not a new implementation; it is T001–T004: freeze authority, prove what current code already does, and close only the ACK/placement/Selection gaps. The smallest end-to-end MVP is T001–T006 plus the one-role portions of T010–T012. Pipeline/tensor/hybrid and Tiger remain mandatory for full Spec174 completion and cannot be dropped merely because the one-role MVP works.
