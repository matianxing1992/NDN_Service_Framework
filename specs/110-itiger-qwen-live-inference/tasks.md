# Tasks: NDNSF-DI iTiger Distributed Qwen Execution

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts/](contracts/), [quickstart.md](quickstart.md)

**Execution rule**: A task that says submit/run/execute/measure remains unchecked
after any pre-start blocker. It closes only with the exact required execution
boundary and durable evidence. Post-start failures close as measured negatives;
no acceptance run is automatically retried. All Spec 110 identities are new.

## Phase 1: Governance, source reality, and immutable inputs

- [X] T001 Record the current branch, clean/sealed-dirty source digest, CodeGraph status, and Spec 109 erratum digest in `specs/110-itiger-qwen-live-inference/evidence/source-baseline.md`
- [X] T002 [P] Snapshot open Spec 107 generation-session tasks and code owners in `specs/110-itiger-qwen-live-inference/evidence/spec107-capability-gap.md`
- [X] T003 [P] Snapshot open Spec 108 OCI/GPU/multi-node tasks and code owners in `specs/110-itiger-qwen-live-inference/evidence/spec108-capability-gap.md`
- [X] T004 [P] Add immutable Qwen2.5 model/revision/license planning records for 0.5B through 72B at `tests/container/itiger-qwen-live/fixtures/model-ladder.json`
- [X] T005 [P] Add the fixed prompt, tokenization, greedy decoding, 1/2/32-token correctness, warmup, 60-second workload, and metric thresholds at `tests/container/itiger-qwen-live/fixtures/workload.json`
- [X] T006 Define new candidate/campaign/cell/run/submission identity derivation and Spec 109 collision rejection in `tools/ndnsf-di/spec110_candidate.py`
- [X] T007 Add identity stability, changed-binding, collision, replacement-link, and freeze tests in `tests/container/itiger-qwen-live/unit/test_candidate_identity.py`
- [X] T008 Define the live cluster snapshot and mutable-fact expiry contract in `tools/ndnsf-di/spec110_cluster.py`
- [X] T009 Add stale partition/GRES/quota/version/address snapshot rejection tests in `tests/container/itiger-qwen-live/unit/test_cluster_snapshot.py`
- [X] T010 Freeze the first campaign protocol and identity namespace in `specs/110-itiger-qwen-live-inference/experiment/campaign-v1.json` (depends on T001-T009)

## Phase 2: Blocking runtime and evidence foundation

**Checkpoint**: No live iTiger submission is permitted until T011-T030 pass.

- [X] T011 [P] Implement the Spec 110 execution-state machine from `contracts/execution-state.md` in `tools/ndnsf-di/spec110_state.py`
- [X] T012 [P] Add RED transition tests proving pre-start blockers cannot close tasks and post-start failures can in `tests/container/itiger-qwen-live/unit/test_execution_state.py`
- [X] T013 [P] Add `contracts/run-evidence.schema.json` and `contracts/allocation-topology.md` positive single-node/multi-node and mutation fixtures under `tests/container/itiger-qwen-live/fixtures/evidence/`
- [X] T014 Implement evidence schema, digest, cross-reference, and authority validation in `tools/ndnsf-di/spec110_evidence.py`
- [X] T015 Add rejection tests for missing/distinct-provider stage proof, single-node with a claimed cross-node edge, multi-node without an edge, CPU fallback, duplicate terminal response, wrong SIF, stale identity, false physical PASS, and invalid failure-boundary narration in `tests/container/itiger-qwen-live/contract/test_evidence_mutations.py`
- [X] T016 [P] Add crash-safe `INTENT_RECORDED`/`SUBMISSION_UNKNOWN`/reconcile, deterministic job-name, at-most-once `sbatch`, replacement-link, and no-auto-resubmit tests in `tests/container/itiger-qwen-live/unit/test_submission_guard.py`
- [X] T017 Implement the pre-`sbatch` submission journal, unknown-outcome `squeue`/`sacct` reconciliation, atomic transitions, and replacement links in `tools/ndnsf-di/spec110_submission.py`
- [X] T018 [P] Add project/home/scratch/quota/partial-copy/protected-cleanup fixtures under `tests/container/itiger-qwen-live/fixtures/storage/`
- [X] T019 Implement storage admission, reserve calculation, atomic promotion, and dry-run protection in `tools/ndnsf-di/spec110_storage.py`
- [X] T020 Add storage and evidence-promotion fault tests in `tests/container/itiger-qwen-live/unit/test_storage.py`
- [X] T021 [P] Add command/profile injection and credential-field rejection tests in `tests/container/itiger-qwen-live/unit/test_operator_safety.py`
- [X] T022 Implement `discover`, `render`, `submit`, `status`, `wait`, `cancel`, `collect`, `validate`, `aggregate`, and `cleanup` command skeletons in `tools/ndnsf-di/ndnsf-di-itiger-qwen`
- [X] T023 Add CLI contract and exit-code tests in `tests/container/itiger-qwen-live/contract/test_operator_cli.py`
- [X] T024 [P] Add deterministic workload/token/evidence sampling helpers in `tools/ndnsf-di/spec110_workload.py`
- [X] T025 Add exact-token, warmup-boundary, 60-second-window, and percentile sample-threshold tests in `tests/container/itiger-qwen-live/unit/test_workload_metrics.py`
- [X] T026 Implement stage/node/GPU/backend and dependency correlation in `tools/ndnsf-di/spec110_execution_proof.py`
- [X] T027 Add execution-boundary negative tests for ACK-only, standalone-only, GPU-visible-only, pre-stage crash, and incomplete promotion in `tests/container/itiger-qwen-live/contract/test_execution_boundary.py`
- [X] T028 Add a repository-wide secret scanner wrapper for OCI/SIF/profile/log/evidence inputs at `packaging/ndnsf-di-container/oci/scripts/scan-secrets.py`
- [X] T029 Run T007-T028 offline and preserve JUnit plus exact commands at `results/spec110-itiger-qwen-live/offline-foundation/`
- [X] T030 Validate `experiment/campaign-v1.json` against source, model, workload, state, storage, and evidence contracts and record the gate at `specs/110-itiger-qwen-live-inference/evidence/foundation-gate.md`

## Phase 3: User Story 1 - Complete iTiger runtime environment (P1)

**Goal**: Produce a self-complete, GPU-capable SIF and prove it on a compute node.

**Independent test**: NFD/NDNSF/NDNSF-DI imports and both PyTorch/ORT CUDA execute inside one allocated GPU container.

- [X] T031 [P] [US1] Finish the pinned system/Python/GPU dependency locks at `packaging/ndnsf-di-container/oci/locks/gpu.lock`
- [X] T032 [P] [US1] Define the host-driver/CUDA/PyTorch/ONNX Runtime compatibility matrix at `packaging/ndnsf-di-container/oci/compatibility/gpu-matrix.yaml`
- [X] T033 [US1] Add `packaging/ndnsf-di-container/oci/Dockerfile.gpu` to build NFD, ndn-cxx, ndn-svs/NAC-ABE, NDNSF Core/Python, NDNSF-DI, Qwen tools, PyTorch, Transformers, and ONNX Runtime GPU (depends on T031-T032)
- [X] T034 [US1] Add non-root runtime user, read-only rootfs expectations, health probes, and no-daemon entrypoint in `packaging/ndnsf-di-container/oci/Dockerfile.gpu`
- [X] T035 [P] [US1] Exclude identities, secrets, models, results, caches, and local homes in `packaging/ndnsf-di-container/oci/.dockerignore`
- [X] T036 [US1] Add NFD, shared-library, Python import, PyTorch CUDA, ORT CUDA provider, and backend-kernel probes in `packaging/ndnsf-di-container/oci/scripts/probe-runtime.py`
- [X] T037 [US1] Add CPU-only, missing-library, driver-too-old, ORT fallback, and PyTorch/ORT CUDA mismatch tests in `tests/container/itiger-qwen-live/integration/test_runtime_compatibility.sh`
- [X] T038 [US1] Implement immutable OCI manifest/signature/digest recording in `packaging/ndnsf-di-container/lib/release.py`
- [X] T039 [US1] Implement OCI-to-SIF materialization, partial-copy cleanup, checksum, and existing-SIF verification in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/materialize-sif.sh`
- [X] T040 [US1] Add project release/model/identity/evidence bind allowlists and clean environment handling in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh`
- [X] T041 [US1] Add runtime release validation to `packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py`
- [X] T042 [US1] Add release-build, secret-scan, SIF-materialization, and tamper tests in `tests/container/itiger-qwen-live/integration/test_release_pipeline.sh`
- [X] T043 [US1] Preserve `.github/workflows/ndnsf-di-itiger-image.yml` as an optional publication path and record its measured public-dependency failure plus the rootless iTiger route decision in `specs/110-itiger-qwen-live-inference/evidence/runtime-build-route-revision.md` (depends on T033-T042)
- [X] T044 [US1] Implement and test `release build-render` plus a bounded Slurm rootless Podman/Buildah builder that seals source, selects compute scratch, forbids home graph/run roots, exports/verifies/promotes OCI, materializes/verifies SIF, and tears down partial state under `tools/ndnsf-di/` and `packaging/ndnsf-di-container/adapters/slurm-apptainer/` (depends on T030, T038-T042)
- [X] T045 [US1] Connect through VPN/SSH, capture live account/QOS/partition/GRES/storage/Apptainer/Podman/Buildah/TmpFS facts without credentials, and render/review one short CPU rootless-build diagnostic with a new non-acceptance identity at `/project/$USER/ndnsf-di/campaigns/spec110/` (depends on T044)
- [X] T046 [US1] Submit the reviewed CPU rootless-build diagnostic exactly once and validate compute-node selected scratch write/fsync, explicit non-home graph/run roots, tiny OCI build/export, Apptainer OCI-to-SIF conversion/execution, teardown, and atomic durable promotion; preserved as `EXECUTED_FAIL` for job 147712 because compute Podman was missing, with no retry (depends on T045)
- [ ] T047 [US1] Publish the full pinned OCI runtime once through the sealed-source GitHub Buildx route, retain source seal/build logs/secret scan/OCI digest/SBOM/signature, then materialize `/project/$USER/ndnsf-di/releases/<release-id>/runtime.sif` from that exact GHCR digest in a bounded iTiger CPU allocation and validate its checksum; a pre-start blocker leaves T047 open (depends on T159-T160 PASS; T151 is fallback-only)
- [ ] T048 [US1] Render/review and submit one bounded GPU runtime-probe job exactly once, then validate selected compute scratch, NFD lifecycle, NDNSF imports/linking, allocated UUID, PyTorch CUDA, ORT CUDA kernel, teardown, and promoted evidence at `results/spec110-itiger-qwen-live/runtime-probe/manifest.json`; a pre-start blocker leaves T048 open (depends on T047)
- [ ] T049 [US1] Record runtime release PASS/FAIL, build provenance, scratch selection, and compatibility limits in `specs/110-itiger-qwen-live-inference/evidence/runtime-release-verdict.md` (depends on T048 execution)

## Phase 4: User Story 3 - Multi-node NDN and secured NDNSF control plane (P1)

**Goal**: Prove allocation-scoped NFD routing and secured remote invocation before GPUs are consumed.

- [X] T050 [P] [US3] Add single-node/multi-node process maps, independently selected TCP and UDP transport variants, routes, duplicate identity, closed port, partial readiness, signal, and teardown fixtures under `tests/container/itiger-qwen-live/fixtures/network/`
- [X] T051 [P] [US3] Add job-scoped non-root NFD config and frozen Slurm process-map templates at `packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/nfd.conf.in` and `process-map.yaml.in`
- [X] T052 [US3] Implement allocation node/address selection and selected-transport reachability with non-selected transport diagnostics in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/probe-multinode-network.sh`
- [X] T053 [US3] Implement the Slurm node supervisor with exactly one NFD per unique node, shared node-local socket, `srun` task/GPU process map, bounded readiness barriers, process-group traps, PID/exit capture, and teardown in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-allocation-topology.sh`
- [X] T054 [US3] Implement idempotent selected-transport face/route configuration and observed-state capture in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh`
- [X] T055 [US3] Add single/multi-node resource/process-map rendering, one-NFD-per-node, readiness order, signal teardown, and shell-injection tests in `tests/container/itiger-qwen-live/unit/test_allocation_topology.py`
- [X] T056 [US3] Add selected-transport probe/admissibility tests proving TCP PASS is not blocked by diagnostic UDP failure and preventing false multi-node PASS in `tests/container/itiger-qwen-live/unit/test_multinode_probe.py`
- [X] T057 [US3] Add distinct controller/user/provider identity validation and read-only binding in `packaging/ndnsf-di-container/lib/profile.py`
- [X] T058 [US3] Add packaged generic permission/NAC-ABE/token/replay/selection integration launcher driven by the frozen process map at `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/probe-ndnsf-security.sh`
- [X] T059 [US3] Run the packaged topology/security path in MiniNDN and retain exact regression results at `results/spec110-itiger-qwen-live/minindn-packaged/`
- [ ] T060 [US3] Render and review exactly one five-minute two-node CPU selected-transport network/security job at `results/spec110-itiger-qwen-live/network-probe/render.json` (depends on T049-T059)
- [ ] T061 [US3] Submit the reviewed CPU network/security probe through the crash-safe journal exactly once; any pre-start blocker leaves T061 open (depends on T060)
- [ ] T062 [US3] Validate addresses, selected transport, diagnostic transport, NFD faces/routes, secured generic request/response, readiness, teardown, and zero login-node daemons at `results/spec110-itiger-qwen-live/network-probe/manifest.json` (depends on T061 execution)
- [ ] T063 [US3] Extend `packaging/ndnsf-di-container/lib/profile.py` and `lib/adapters/slurm_apptainer.py` to admit/render the exact frozen process map and enable multi-node only for the validated selected-transport probe digest (depends on T062 PASS)
- [ ] T064 [US3] Record the measured network/security PASS/FAIL without retry or transport substitution at `specs/110-itiger-qwen-live-inference/evidence/multinode-network-verdict.md`

## Phase 5: User Story 2 - First 0.5B single-node multi-GPU candidate (P1)

**Goal**: Three distinct provider processes/GPUs on one node return exact tokens through NDNSF-DI.

- [ ] T065 [P] [US2] Add RED generation-session tests for versioned Spec 107/110 candidate IDs, token epoch, provider-local KV, deadlines, cancellation, bounded in-session replacement, stale attempts, and exact-final-once in `tests/unit-tests/di-qwen-generation-session.t.cpp`
- [ ] T066 [P] [US2] Add RED Python one-generation-request, 1/2/32-token equality, three-distinct-provider/GPU evidence, same-node dependency, and multi-node dependency tests in `tests/python/test_ndnsf_di_spec110_qwen_session.py`
- [ ] T067 [P] [US2] Add RED security tests for permission, NAC-ABE, tokens, replay, lease, attempt, digest, mixed candidate, and Spec 109 identity rejection in `tests/python/test_ndnsf_di_spec110_security.py`
- [ ] T068 [US2] Replace the Spec107-only candidate regex with an explicit versioned Spec107/Spec110 identity parser and finish bounded generation-session execution in `NDNSF-DistributedInference/cpp/ndnsf-di/QwenGenerationSession.cpp` without accepting Spec105/109 identities (depends on T065)
- [ ] T069 [US2] Integrate generation session with collaboration dependency I/O, lease/attempt authority, cancellation, and in-session replacement evidence in `NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp` and `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.cpp`
- [ ] T070 [US2] Wire real ONNX Runtime CUDA stage execution and provider PID/node/GPU/fallback evidence into `NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.cpp`
- [ ] T071 [US2] Update `examples/DI_NativeProviderExecutable.cpp` for immutable stage artifacts, generation-session readiness, distinct identity/process proof, and fail-closed backend evidence
- [ ] T072 [US2] Replace acceptance-mode per-token NDNSF requests in `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` with one bounded generation request and exactly-one terminal validation; retain old loop only under an explicit diagnostic identity
- [ ] T073 [US2] Run focused C++/Python generation, dependency, security, lease, backend, candidate-version, and sanitizer tests and retain results at `results/spec110-itiger-qwen-live/session-tests/`
- [ ] T074 [US2] Stage/seal the immutable 0.5B model/tokenizer/license under `/project/$USER/ndnsf-di/models/qwen2.5/0.5b/` after live storage admission
- [ ] T075 [US2] Execute the pinned 0.5B full-model greedy oracle for 1/2/32 tokens in a GPU allocation and retain exact token IDs at `results/spec110-itiger-qwen-live/0.5b/oracle/`
- [ ] T076 [US2] Export/seal three 0.5B stage artifacts and validate graph/external data, hidden-state/KV tensor name/shape/dtype, numerical tolerances, final logits, and top-1 token equivalence at `results/spec110-itiger-qwen-live/0.5b/artifacts/`
- [ ] T077 [US2] Freeze the 0.5B single-node candidate, role graph, three provider identities/PIDs, one-NFD process map, GPU mappings, workload, and evidence schema in `specs/110-itiger-qwen-live-inference/experiment/qwen2.5-0.5b-single-node.json`
- [ ] T078 [US2] Render/review and submit through the crash-safe journal exactly once the single-node 1-token correctness cell; pre-start blockers leave T078 open (depends on T049, T073-T077)
- [ ] T079 [US2] Validate three distinct GPU/provider stage records, same-node NDN dependencies, security, one final response, and exact 1-token equality at `results/spec110-itiger-qwen-live/0.5b/single-node-correctness-1/manifest.json`
- [ ] T080 [US2] Render/review and submit exactly once the single-node 2-token cell; pre-start blockers leave T080 open (depends on T079 PASS)
- [ ] T081 [US2] Validate the 2-token bundle at `results/spec110-itiger-qwen-live/0.5b/single-node-correctness-2/manifest.json`
- [ ] T082 [US2] Render/review and submit exactly once the single-node 32-token cell; pre-start blockers leave T082 open (depends on T081 PASS)
- [ ] T083 [US2] Validate the 32-token bundle at `results/spec110-itiger-qwen-live/0.5b/single-node-correctness-32/manifest.json`
- [ ] T084 [US2] Publish the 0.5B single-node distributed verdict with exact executed/failure-boundary/blocked status at `specs/110-itiger-qwen-live-inference/evidence/0.5b-single-node-verdict.md`

## Phase 6: User Stories 3 and 4 - Multi-node extension and Qwen size ladder (P1)

**Goal**: Prove the cross-node extension independently, while every model size receives a controlled single-node multi-GPU attempt under a size-local immutable identity.

- [ ] T085 [P] [US4] Implement parameterized model download/seal, stage export/interface, and candidate generation in `tools/ndnsf-di/spec110_artifacts.py`
- [ ] T086 [P] [US4] Add 1.5B/3B/7B/14B/32B/72B manifest, interface, memory, quota, and placement fixtures under `tests/container/itiger-qwen-live/fixtures/models/`
- [ ] T087 [US4] Add per-size storage/GPU admission and no-silent-quantization tests in `tests/container/itiger-qwen-live/unit/test_model_admission.py`
- [ ] T088 [US3] Render/review and submit exactly once the 0.5B multi-node 32-token extension using the selected transport and frozen process map; pre-start blockers leave T088 open (depends on T064 PASS and T084 PASS)
- [ ] T089 [US3] Validate and publish the 0.5B multi-node extension verdict, including cross-node edge evidence and the matched single-node NDNSF-DI placement reference, at `results/spec110-itiger-qwen-live/0.5b/multi-node-correctness-32/manifest.json`
- [ ] T090 [US4] Stage/seal, run oracle, export/validate stages, and freeze the 1.5B candidate under `/project/$USER/ndnsf-di/` (depends on T084 PASS and T085-T087)
- [ ] T091 [US4] Execute exactly once and validate the 1.5B single-node multi-GPU 1/2/32-token correctness cells at `results/spec110-itiger-qwen-live/1.5b/single-node-correctness/`; pre-start blockers leave T091 open
- [ ] T092 [US4] Stage/seal, run oracle, export/validate stages, and freeze the 3B candidate under `/project/$USER/ndnsf-di/` (depends on T091 executed)
- [ ] T093 [US4] Execute exactly once and validate the 3B single-node multi-GPU 1/2/32-token correctness cells at `results/spec110-itiger-qwen-live/3b/single-node-correctness/`; pre-start blockers leave T093 open
- [ ] T094 [US4] Stage/seal, run oracle, export/validate stages, and freeze the 7B candidate under `/project/$USER/ndnsf-di/` (depends on T093 executed)
- [ ] T095 [US4] Execute exactly once and validate the 7B single-node multi-GPU 1/2/32-token correctness cells at `results/spec110-itiger-qwen-live/7b/single-node-correctness/`; pre-start blockers leave T095 open
- [ ] T096 [US4] Stage/seal, run oracle, export/validate stages, and freeze the 14B candidate with a live-admitted RTX6000/H100 placement under `/project/$USER/ndnsf-di/` (depends on T095 executed)
- [ ] T097 [US4] Execute exactly once and validate the 14B single-node multi-GPU 1/2/32-token correctness cells at `results/spec110-itiger-qwen-live/14b/single-node-correctness/`; pre-start blockers leave T097 open
- [ ] T098 [US4] Measure actual 32B source/export/SIF/reserve demand and produce a quota/GPU placement admission record at `specs/110-itiger-qwen-live-inference/evidence/32b-admission.md`
- [ ] T099 [US4] Obtain sufficient project quota, then stage/seal, run oracle, export/validate stages, and freeze the 32B candidate under `/project/$USER/ndnsf-di/`; insufficient quota leaves T099 open
- [ ] T100 [US4] Execute exactly once and validate the 32B single-node multi-GPU 1/2/32-token correctness cells at `results/spec110-itiger-qwen-live/32b/single-node-correctness/`; pre-start blockers leave T100 open
- [ ] T101 [US4] Measure actual 72B source/export/SIF/reserve demand and produce a quota/GPU placement admission record at `specs/110-itiger-qwen-live-inference/evidence/72b-admission.md`
- [ ] T102 [US4] Obtain sufficient project quota, then stage/seal, run oracle, export/validate stages, and freeze the 72B candidate under `/project/$USER/ndnsf-di/`; insufficient quota leaves T102 open
- [ ] T103 [US4] Execute exactly once and validate the 72B single-node multi-GPU 1/2/32-token correctness cells at `results/spec110-itiger-qwen-live/72b/single-node-correctness/`; pre-start blockers leave T103 open
- [ ] T104 [US4] Generate a seven-size single-node correctness/capacity table that distinguishes pre-start incomplete, executed PASS, and executed negative at `specs/110-itiger-qwen-live-inference/evidence/size-ladder-verdict.md`
- [ ] T105 [US4] Verify every size reached `CANDIDATE_EXECUTION_STARTED` and reject matrix completion if any size has only a planning blocker in `tests/container/itiger-qwen-live/contract/test_matrix_completion.py`

## Phase 7: User Story 5 - Matched performance and scaling (P1)

**Goal**: Measure framework overhead and placement/network delta through two explicitly matched contrasts.

- [ ] T106 [P] [US5] Implement per-request/stage/dependency/security/GPU metric collection with `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` in `tools/ndnsf-di/spec110_metrics.py`
- [ ] T107 [P] [US5] Implement matched local-staged/single-node/multi-node binding and confound labeling in `tools/ndnsf-di/spec110_compare.py`
- [ ] T108 [US5] Add metric decomposition, sample threshold, unmatched-baseline, placement mismatch, and mixed-hardware rejection tests in `tests/container/itiger-qwen-live/unit/test_performance_analysis.py`
- [ ] T109 [US5] For 0.5B, render/review/execute single-node NDNSF-DI repetitions 1-3 exactly once for 60 seconds each and retain separate bundles under `results/spec110-itiger-qwen-live/0.5b/performance/single-node/` (depends on T084 PASS)
- [ ] T110 [US5] For 0.5B, execute three hardware- and artifact-matched local staged repetitions and publish the framework-overhead contrast under `results/spec110-itiger-qwen-live/0.5b/performance/local-staged/`
- [ ] T111 [US5] For 0.5B, execute three multi-node NDNSF-DI repetitions only after T089 PASS and publish the placement/network contrast against T109 under `results/spec110-itiger-qwen-live/0.5b/performance/multi-node/`
- [ ] T112 [US5] Execute and retain three single-node candidate plus three matched local-staged 60-second repetitions for the successful 1.5B candidate under `results/spec110-itiger-qwen-live/1.5b/performance/`; keep an independent per-cell submission ledger
- [ ] T113 [US5] Execute and retain three single-node candidate plus three matched local-staged 60-second repetitions for the successful 3B candidate under `results/spec110-itiger-qwen-live/3b/performance/`; keep an independent per-cell submission ledger
- [ ] T114 [US5] Execute and retain three single-node candidate plus three matched local-staged 60-second repetitions for the successful 7B candidate under `results/spec110-itiger-qwen-live/7b/performance/`; keep an independent per-cell submission ledger
- [ ] T115 [US5] Execute and retain three single-node candidate plus three matched local-staged 60-second repetitions for the successful 14B candidate under `results/spec110-itiger-qwen-live/14b/performance/`; keep an independent per-cell submission ledger
- [ ] T116 [US5] Execute and retain three single-node candidate plus three matched local-staged 60-second repetitions for the successful 32B candidate under `results/spec110-itiger-qwen-live/32b/performance/`; keep an independent per-cell submission ledger
- [ ] T117 [US5] Execute and retain three single-node candidate plus three matched local-staged 60-second repetitions for the successful 72B candidate under `results/spec110-itiger-qwen-live/72b/performance/`; keep an independent per-cell submission ledger
- [ ] T118 [US5] Generate per-size completion, TTFT, ITL, tokens/s, throughput, stage, dependency, security/orchestration, CPU/GPU, queue, sample, and failure summaries at `results/spec110-itiger-qwen-live/scaling/per-size.json`
- [ ] T119 [US5] Identify the identical-hardware controlled subset and label all other comparisons descriptive in `specs/110-itiger-qwen-live-inference/evidence/scaling-report.md`
- [ ] T120 [US5] Compute effect sizes, run-to-run uncertainty, and negative-result sensitivity without causal claims across changed hardware in `specs/110-itiger-qwen-live-inference/evidence/scaling-report.md`
- [ ] T121 [US5] Freeze a new reproduction identity for one accepted 0.5B single-node cell without reusing any original run
- [ ] T122 [US5] Execute the reproduction exactly once and retain exact token and metric-variation evidence at `results/spec110-itiger-qwen-live/reproduction/`
- [ ] T123 [US5] Publish the final performance/reproducibility verdict without replacing failed or incomplete repetitions in `specs/110-itiger-qwen-live-inference/evidence/performance-verdict.md`

## Phase 8: User Story 6 - Operations, evidence, and authority (P2)

- [ ] T124 [P] [US6] Add Slurm PENDING/RUNNING/PREEMPTED/TIMEOUT/CANCELLED/COMPLETED fixtures in `tests/container/itiger-qwen-live/fixtures/lifecycle/`
- [ ] T125 [US6] Add exact-job status/wait/cancel, submission-unknown reconciliation, and already-terminal handling tests in `tests/container/itiger-qwen-live/unit/test_lifecycle.py`
- [ ] T126 [US6] Implement bounded lifecycle reconciliation and original-exit preservation in `packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py`
- [ ] T127 [US6] Add allocation process audit, one-NFD-per-node verification, and zero-login-daemon verification at `tests/container/itiger-qwen-live/integration/test_process_teardown.sh`
- [ ] T128 [US6] Run one CPU cancellation-path test under a new diagnostic identity and retain the original scheduler outcome at `results/spec110-itiger-qwen-live/cancellation/`
- [ ] T129 [US6] Validate atomic promotion recovery and incomplete-evidence behavior by fault injection in `tests/container/itiger-qwen-live/integration/test_evidence_promotion.sh`
- [ ] T130 [US6] Run the canonical OCI/SIF/profile/log/evidence secret scan and retain only redacted findings at `results/spec110-itiger-qwen-live/secret-scan/`
- [ ] T131 [US6] Execute cleanup dry-run and prove zero protected candidate, model, identity, release, active-job, or accepted-evidence deletion at `results/spec110-itiger-qwen-live/cleanup/`
- [ ] T132 [US6] Validate all accepted bundles against schema/checksum/lineage/authority contracts at `results/spec110-itiger-qwen-live/validation/`
- [ ] T133 [US6] Generate the final authority record with candidate evidence and `physicalProduction=DEFERRED` at `specs/110-itiger-qwen-live-inference/release-gate.json`

## Phase 9: Documentation, audit, and handoff

- [ ] T134 [P] Document OCI build, SIF materialization, dependencies, and GPU compatibility in `packaging/ndnsf-di-container/docs/itiger-runtime.md`
- [ ] T135 [P] Document VPN/SSH, project storage, Slurm, Apptainer, frozen process maps, NFD topology, submit/reconcile/monitor/cancel, and cleanup in `packaging/ndnsf-di-container/docs/itiger-distributed-qwen.md`
- [ ] T136 [P] Document identity provisioning/rotation, secret handling, and evidence redaction in `packaging/ndnsf-di-container/docs/security.md`
- [ ] T137 Update `README.md` with the iTiger distributed experiment entrypoint after implementation
- [ ] T138 Update `README_zh-CN.md` with the same scope, commands, and authority boundary as T137
- [ ] T139 Reconcile every FR/SC/task/test/evidence link in `specs/110-itiger-qwen-live-inference/traceability.md`
- [ ] T140 Run the full offline, C++/Python, packaged MiniNDN, container, and evidence suites and record commands/durations/counts at `specs/110-itiger-qwen-live-inference/checklists/post-implementation-audit.md`
- [ ] T141 Run strict Spec Kit structural analysis and resolve all unambiguous inconsistencies in `specs/110-itiger-qwen-live-inference/`
- [ ] T142 Run code-aware post-implementation audit against CodeGraph, source, tests, and iTiger evidence; unresolved HIGH/BLOCK findings keep the feature open
- [ ] T143 Verify Spec 109 evidence/digests remain unchanged except the formal erratum and links
- [ ] T144 Create the Spec 106 handoff without granting physical authority at `specs/106-ndnsf-di-physical-deployment/handoffs/spec110-itiger-distributed-qwen.md`
- [ ] T145 Update agent/GSD resumable context with exact open task, candidate/run identity, job state, and evidence path
- [ ] T146 Write `specs/110-itiger-qwen-live-inference/completion-summary.md` with actual job IDs, placement/size outcomes, speeds, failures, storage, and residual risks; do not claim completion while any single-node size task or multi-node extension task is open
- [ ] T147 Run the one-second completion bell and record the final recommended roadmap step in `specs/110-itiger-qwen-live-inference/completion-summary.md`

## Dependencies and required order

1. Phase 1 freezes intent and identities.
2. Phase 2 is a hard gate for all live work.
3. Phase 3 produces the real SIF/runtime probe.
4. Phase 4 proves the selected cross-node NDN/security transport without GPUs and does not block the single-node first candidate.
5. Phase 5 completes generation-session capability and the first 0.5B single-node three-GPU vertical slice.
6. Phase 6 uses T064 plus T084 to unlock the independent multi-node extension, while the seven-size single-node ladder depends only on T084 and per-size admission.
7. Phase 7 runs only for correctness-PASS placement/size cells; executed negatives remain in the scale report and every repetition keeps its own submission ledger.
8. Phase 8 gates final evidence acceptance.
9. Phase 9 may claim completion only after T105 and T132 prove the matrix and evidence bundles.

## First deployment route

The shortest safe route is T001-T030 → T031-T049 → T065-T084. This yields
one real 0.5B single-node three-provider/three-GPU candidate with one NFD before
cross-node transport is allowed to affect correctness. In parallel after the
runtime probe, T050-T064 prepares the selected cross-node transport; T088-T089
then runs the independent 0.5B multi-node extension. If any pre-start gate
fails, repair that capability and keep the live task open; do not convert the
blocker into experimental completion.

## Phase 10: Convergence

- [X] T148 [US1] Implement a pinned builder-OCI-to-SIF fallback that runs Buildah with VFS/chroot inside Apptainer, retains the existing GPU Dockerfile/locks as the only stack build graph, uses only selected compute scratch, and fails closed when compute user-namespace/capability support is insufficient per FR-002 and US1/AC1 (missing, CRITICAL)
- [X] T149 [US1] Add offline and container integration tests for missing host Podman/Buildah, pinned builder SIF assets, nested Buildah isolation/storage, immutable OCI export, SIF execution, partial cleanup, and evidence-write failure under `tests/container/itiger-qwen-live/` per FR-002 and FR-004 (partial, HIGH)
- [ ] T150 [US1] Render and review exactly one replacement compute-builder diagnostic with frozen builder assets, a new diagnostic identity, an explicit link to failed submission `spec110-submission-3757081f091030c057ec`, and a human authorization digest; do not submit it in this task per FR-027-FR-028 (partial, HIGH)
- [ ] T151 [US1] After explicit human authorization, submit the T150 replacement exactly once through the crash-safe journal and validate selected scratch write/fsync, Apptainer/user-namespace mode, nested OCI build/export, OCI-to-SIF conversion/execution, teardown, and atomic promotion; preserve any post-start failure and never auto-resubmit per FR-004, FR-006, and FR-027-FR-028 (partial, HIGH)
- [X] T152 [US1] Update the runtime-release gate, traceability, quickstart, and T047 execution dependency so the full build can start only after T151 PASS (or an administrator-supported compute builder probe PASS), while retaining T046/job 147712 as the original executed negative per plan: runtime release (partial, MEDIUM)

## Phase 11: GitHub sealed-source primary route

- [X] T153 [US1] Formally revise `spec.md`, `plan.md`, `research.md`, `contracts/runtime-release.md`, `quickstart.md`, traceability, and T047 so GitHub Buildx/GHCR is primary, iTiger materializes SIF and executes GPUs, Qwen weights remain under `/project`, and builder-SIF is fallback (depends on user route decision)
- [X] T154 [P] [US1] Add RED unit/integration tests for exact dependency archive preparation, unreachable revisions, archive/lock tamper, unsafe tar members, Dockerfile sealed-only extraction, Qwen-weight exclusions, and workflow evidence-only artifact retention under `tests/container/itiger-qwen-live/`
- [X] T155 [US1] Implement one shared sealed-source archive contract for GitHub and rootless fallback in `packaging/ndnsf-di-container/oci/scripts/prepare-sealed-context.py`, `Dockerfile.gpu`, and `rootless-build.sh` (depends on T154)
- [X] T156 [US1] Publish the exact locked NFD and ndn-svs commits to dedicated advertised refs in their configured GitHub repositories and verify all six lock revisions are remotely fetchable without credentials (depends on T155 local validation)
- [X] T157 [US1] Repair `.github/workflows/ndnsf-di-itiger-image.yml` to prepare/verify sealed sources, pass `DEPENDENCY_SOURCE_MODE=sealed`, push only to GHCR, retain only manifest/SBOM/signature/log/checksum evidence, record runner disk, and exclude Qwen weights (depends on T155-T156)
- [X] T158 [US1] Run the full offline/container/workflow static suite, source-secret scan, local six-repository seal creation/verification, YAML parse, and strict Spec Kit audit; recorded 86/86 tests, byte-identical local/remote dependency archives, zero scan findings, and no cloud claim in `evidence/github-sealed-route-gate.md` (depends on T157)
- [X] T159 [US1] After local PASS, commit and push the workflow/packaging revision once so its existing Experimental-path trigger starts one GitHub build; preserved run `29283330479` as `EXECUTED_FAIL` at the combined dependency-build step with no rerun and no GHCR digest in `evidence/github-run-29283330479-verdict.md` (depends on T158)
- [ ] T160 [US1] After the replacement GitHub release in T171 PASSes, render/review and submit exactly one bounded iTiger CPU materialization job through the crash-safe journal, build `runtime.sif` from the exact GHCR digest, verify checksum/static runtime, atomically promote evidence, and never auto-resubmit (depends on T171)

## Phase 12: GitHub dependency-order replacement

- [X] T161 [US1] Diagnose run `29283330479` without rerunning it, prove from the locked NDNSD contract that ndn-svs must be installed first, add a RED build-order regression, split the Dockerfile dependency layers in `ndn-cxx -> ndn-svs -> NDNSD` order, pass 87/87 offline tests, and reproduce the corrected sequence in a disposable Ubuntu 22.04 container
- [X] T162 [US1] Commit and push the reviewed T161 fix once under a new source/release identity; preserved run `29285142648` as `EXECUTED_FAIL` at the isolated NFD layer with no rerun and no GHCR digest in `evidence/github-run-29285142648-verdict.md` (depends on T161)

## Phase 13: NFD sealed-input replacement

- [X] T163 [US1] Diagnose run `29285142648` without rerunning it, reproduce the missing `libpcap` failure from exact locked sources in disposable Ubuntu 22.04, add a RED regression, lock and seal NFD's exact websocketpp gitlink revision, install `libpcap-dev`, prove NFD 24.07 configure/build/install locally, and pass the full offline gate
- [X] T164 [US1] Monitor the exactly-once T167 GPU assembly to terminal state and preserve its immutable GHCR digest or exact failure without automatic rerun; run `29297628957` is frozen as `EXECUTED_FAIL` in `evidence/github-run-29297628957-verdict.md` (depends on T167)

## Phase 14: Local foundation and GPU-delta assembly revision

- [X] T165 [US1] Re-audit the complete build graph, reproduce the local Ubuntu 20.04/OpenSSL 1.1 OpenABE/NAC-ABE ABI and tests, preserve `libpcap-dev`, lock OpenABE's exact RELIC source, Python 3.10/Focal runtime closure, and official ONNX Runtime GPU C++ asset, add regressions for dependency closure/runtime configuration/uninstalled waf examples, and replace the monolithic cloud build with `Dockerfile.foundation` plus a dispatch-only `Dockerfile.gpu`
- [X] T166 [US1] Commit the reviewed revision, create and verify a new source seal, run `build-foundation-local.sh` to completion, validate NFD/NDN/OpenABE/NAC-ABE/NDNSF core inside the builder, push the source-bound foundation tag once, and record its immutable GHCR digest
- [X] T167 [US1] Review the final foundation/CUDA/ONNX inputs and manually dispatch `.github/workflows/ndnsf-di-itiger-image.yml` exactly once; run `29297628957` was dispatched once, assembled only the GPU delta, and retained its terminal failure without automatic rerun

## Phase 15: GPU assembler runtime-closure replacement

- [X] T168 [US1] Diagnose run `29297628957` without rerunning it, preserve its logs/artifacts/disk evidence, add a RED regression for the Foundation runtime-package ordering defect, install the Foundation-measured DSO closure before the GPU assembler's `ldd` scan, and pass focused, full offline, source-secret, and strict Spec Kit gates
- [X] T169 [US1] Commit and push the reviewed T168 repair as `8f6332ce800a1a5130f457fa54454ad968dff638`, create and verify its matching source seal, rebuild the local Foundation, and publish its source-bound tag exactly once as anonymously readable digest `sha256:a9ed75a9fa09acd6e795007e5d58a69fb9f9b349222faab9aeb852c70fbed820` (depends on T168)
- [X] T170 [US1] Review the new source/Foundation/CUDA/ONNX inputs, prove the prior release identity cannot be reused, create exact source tag `spec110-gpu-source-8f6332ce800a1a5130f457fa54454ad968dff638`, and manually dispatch replacement GPU assembly run `29306588187` under the new release identity exactly once (depends on T169 and explicit human authorization)
- [X] T171 [US1] Monitor T170 replacement run `29306588187` to terminal `EXECUTED_FAIL` and preserve its exact optional-torio-library closure failure without automatic rerun; no final GHCR runtime digest exists and T160 remains locked (depends on T170)

## Phase 16: Qwen-minimal Python GPU closure convergence

- [X] T172 [US1] Add a RED regression for run `29306588187`, prove the Qwen text runtime and acceptance probes do not consume `torchaudio` or `torchvision`, remove those unrequested media wheels from `Dockerfile.gpu`, `gpu.lock`, and `gpu-matrix.yaml` without weakening fail-closed ELF checks for required extensions, and pass focused/full offline, preflight, secret-scan, and strict pre/post-implementation audit gates per FR-002 and plan: runtime release (partial)
- [X] T173 [US1] Commit and push the audited T172 repair as `94fc25062aa7fced301b1f9db983de3e9a8910e3`, create and verify source seal `sha256:39cdd27ea73c94ed2ff6801f20f3bef2dbac247e74f55fc3ad67394a87595712`, rebuild the exact local Foundation, publish its source-bound tag exactly once as digest `sha256:d2aaca7b18aa56b9e24ac6b9c6c9c6a98a26117c32d70966845ae1589ea62d15`, and prove the digest anonymously readable per FR-002 (partial; depends on T172)
- [X] T174 [US1] After explicit human authorization, prove all prior source/release identities remain frozen, create an exact new source tag, fsync a crash-safe dispatch intent, and dispatch one replacement GPU assembly exactly once under a new release identity per FR-002 and plan: runtime release (partial; depends on T173)
- [X] T175 [US1] Monitor only the T174 run to terminal state, preserve its final immutable GHCR digest or exact failure with complete logs/artifacts and no automatic rerun, and allow only PASS to supersede T171's failed release gate per FR-002 (partial; depends on T174)
- [X] T176 [US1] If T175 PASSes, revise T160's controlling replacement dependency from failed T171 to T175, then execute T160's exactly-once bounded iTiger CPU SIF materialization; otherwise preserve the blocker and do not submit per US1/AC1 and plan: experiment sequence (partial; depends on T175); T175 failed, so T160 stayed locked and no iTiger job was submitted

## Phase 17: Vendored Python ELF closure convergence

- [X] T177 [US1] Freeze GitHub run `29309152207` as the exactly-once T174/T175 `EXECUTED_FAIL`, preserve its Pillow vendored-library failure and complete artifact digests without rerun, and keep T160 blocked because no runtime digest exists per FR-002 (partial, HIGH)
- [X] T178 [US1] Add a RED real-ELF regression that reproduces a bundled shared object resolving a sibling vendored DSO only from its bundle directory, plus a negative case that removes the sibling and MUST still raise `RUNTIME_LIBRARY_MISSING`, under `tests/container/itiger-qwen-live/` per FR-002 (partial, HIGH)
- [X] T179 [US1] Repair `packaging/ndnsf-di-container/oci/scripts/derive-runtime-packages.py` so each scanned ELF may resolve only its own sibling bundle directory before fail-closed missing-library evaluation, then pass focused/full offline, preflight, secret-scan, and strict pre/post-implementation audit gates per FR-002 and plan: runtime release (contradicts, CRITICAL; depends on T178)
- [X] T180 [US1] Commit and push the audited T179 repair as `01f730d122a4408737443d75a65dc0d5ff99af5b`, create and verify source seal `sha256:dd55acbc8fc5af5a3f2b2a8b0eda7c9457d13356e35350629dba47bd870be298`, rebuild and publish the source-bound Foundation exactly once as anonymously readable digest `sha256:71711601004a6f032fda032037691409324d2adb867efcd94be48b2d879227aa`, and retain the fresh-explicit-authorization gate before any new GPU assembly dispatch; all prior source/release identities remain frozen per FR-002 and FR-027-FR-028 (partial, HIGH; depends on T179)

## Phase 18: Vendored-ELF GPU release replacement

- [ ] T181 [US1] Render and review one candidate-specific GPU assembly authorization record for source `01f730d122a4408737443d75a65dc0d5ff99af5b` and Foundation digest `sha256:71711601004a6f032fda032037691409324d2adb867efcd94be48b2d879227aa`, prove the new source/release identities absent and every prior identity frozen, record an authorization digest, and stop before dispatch so this candidate can receive fresh explicit human authorization per FR-002 and FR-027-FR-028 (missing, HIGH; depends on T180)
- [ ] T182 [US1] After fresh explicit human authorization of the exact T181 record, create the source tag, fsync a crash-safe dispatch intent, dispatch the GPU assembly exactly once, reconcile exactly one workflow run, monitor it to terminal state, and preserve either its immutable final GHCR digest or exact failure with no automatic rerun per FR-002 and FR-027-FR-028 (partial, HIGH; depends on T181 and explicit human authorization)
- [ ] T183 [US1] If T182 PASSes, revise T160's controlling replacement dependency to the T182 runtime digest and execute T160's bounded exactly-once iTiger CPU SIF materialization; otherwise preserve the blocker and submit no iTiger job per US1/AC1 and plan: runtime release (partial, HIGH; depends on T182)

## Phase 19: Experiment-ready image contract convergence

- [X] T184 [US1] Freeze the unstarted T181 candidate as `PRE_START_SUPERSEDED`: preserve source `01f730d122a4408737443d75a65dc0d5ff99af5b` and Foundation digest `sha256:71711601004a6f032fda032037691409324d2adb867efcd94be48b2d879227aa` as valid Foundation evidence, create no source/release tag, dispatch no workflow or iTiger job, and record that the image-contract audit found the official GPU Dockerfile omitted the Qwen experiment entrypoints and the Qwen Slurm template bypassed the canonical least-privilege runner (contradicts, CRITICAL; supersedes T181-T183 before start)
- [X] T185 [US1] Add RED unit/integration regressions proving the official `Dockerfile.gpu` installs all Qwen experiment entrypoints at their exact in-image paths, uses `gpu.lock` as the single Python/CUDA dependency truth, and that rendered Qwen execution uses the canonical runner with explicit read-only model/artifact mounts instead of a writable whole-project bind (missing, CRITICAL; depends on T184)
- [X] T186 [US1] Repair the official one-image assembly contract: install `run-ndnsf-qwen.sh`, `run-qwen-reference.py`, and `sample-qwen-resources.py` in `/opt/ndnsf/bin`, remove the unused conflicting Qwen overlay Dockerfile/lock, and retain NFD, NDNSF-DI, CUDA, PyTorch, ONNX Runtime GPU, and size-neutral external Qwen weights in one immutable runtime image (missing, CRITICAL; depends on T185)
- [X] T187 [US1] Extend `run-container.sh` with a validated project-contained artifacts root bound read-only at `/artifacts`, render `ndnsf-qwen.sbatch.in` through that canonical runner with explicit release/model/artifact/identity/evidence/scratch mounts, and remove the broad writable `/project` bind without changing candidate or exactly-once semantics (partial, HIGH; depends on T185)
- [X] T188 [US1] Pass focused RED-to-GREEN regressions, the full offline/container suite, YAML and shell syntax checks, source-secret scan, sealed-context preflight, CodeGraph synchronization, and strict post-implementation Spec Kit audit before any image build or external publication (partial, HIGH; depends on T186-T187)
- [X] T189 [US1] Commit the audited image-contract repair as `de849b88c25337cc9acf55f6572b081b2f00ab9e`, create and verify exact source seal `sha256:5a14206718b86d732de8194abc9a0e1119a5542c367ba57709a23ed49b7ac245`, and build matching local Foundation image `sha256:3882ca6519f90c29aa0bcec4d706ecc08ee8830f446f63e91d264fd09753d8d1` without publishing it because the GPU Dockerfile fail-closed source-revision binding forbids reusing the `01f730d` Foundation for changed source (missing, HIGH; depends on T188)
- [ ] T190 [US1] Build the complete GPU runtime image locally from the exact new seal and local source-bound Foundation without requiring a host GPU, run static/runtime/mount-sentinel acceptance including every Qwen entrypoint and external model/artifact visibility, record actual image size and peak disk use, and stop before GHCR publication or iTiger submission so a later immutable candidate can receive explicit authorization (missing, HIGH; depends on T189)

## Phase 20: Host-driver ELF boundary convergence

- [X] T191 [US1] Preserve the first exact local full-GPU build as `EXECUTED_FAIL` after 31m04s at the final `libcaffe2_nvrtc.so` closure scan, prove all earlier CUDA/Python/ONNX/NDNSF-DI build stages completed, record that no final image tag or external publication exists, and retain the full log instead of rerunning the same source identity (contradicts, HIGH; depends on T189 attempt)
- [X] T192 [US1] Add a RED real-ELF regression proving an unresolved `libcuda.so.1` is accepted only as an Apptainer `--nv` host-driver injection while an unresolved sibling/user-space DSO remains fail closed (missing, HIGH; depends on T191)
- [X] T193 [US1] Repair `derive-runtime-packages.py` to parse unresolved SONAMEs, allow only the explicit NVIDIA host-driver ABI set required by FR-003, retain fail-closed behavior for every other missing DSO, and pass focused/full offline and static gates (missing, HIGH; depends on T192)
- [ ] T194 [US1] Commit the host-driver-boundary repair under a new source identity, create/verify its seal, rebuild the matching local Foundation without publication, and then unlock T190 for one new local candidate build that may reuse BuildKit downloads but not the failed source identity (missing, HIGH; depends on T193)
