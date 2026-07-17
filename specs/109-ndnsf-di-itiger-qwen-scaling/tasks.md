# Tasks: NDNSF-DI iTiger Qwen Scaling

**Input**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`
**Tests**: Tests-first is mandatory for model identity, storage, submission, evidence, backend, correctness, and aggregation.
**Execution rule**: Diagnostic and acceptance identities are disjoint. Every acceptance submission is exactly once; failures are retained. A bundled job closes each keyed cell independently. No live task starts until its exact source, predecessor, deployment, storage, and semantic-validator gates pass.
**Authority**: Spec 109 owns iTiger Qwen scaling evidence. Specs 107/108 retain runtime/deployment ownership; Spec 106 retains physical-production authority.

## Phase 1: Setup and protected baselines

**Purpose**: Freeze known substrate/code facts and create a new campaign identity without modifying predecessor evidence.

- [X] T001 Seal the current clean commit or commit plus binary diff/untracked manifest archive in `specs/109-ndnsf-di-itiger-qwen-scaling/baselines/source-snapshot.json`
- [X] T002 [P] Enumerate Spec 107 T027/T028-T038 and Spec 108 T091-T102 statuses, schemas, artifacts, identities, and digests in `specs/109-ndnsf-di-itiger-qwen-scaling/baselines/predecessor-lock.json`
- [X] T003 [P] Record current hard-coded Qwen2.5-0.5B/three-stage/CPU paths in `specs/109-ndnsf-di-itiger-qwen-scaling/baselines/code-reality.md`
- [X] T004 [P] Add official Qwen2.5 size/parameter/license planning fixture at `tests/container/itiger-qwen/fixtures/qwen25-model-ladder.json`
- [X] T005 [P] Add observed iTiger GPU/GRES/version/storage planning fixture at `tests/container/itiger-qwen/fixtures/itiger-observed-20260713.json`
- [X] T006 Add tests that predecessor artifacts and physical-production authority cannot be rewritten in `tests/python/test_ndnsf_di_spec109_lineage.py`
- [X] T007 Implement exact predecessor-entry and deployment-profile digest verification in `tools/ndnsf-di/spec109_predecessors.py` (depends on T002, T006)
- [X] T008 Implement clean/sealed-dirty source capture and a campaign ID derived from source/predecessor/deployment/matrix digests in `tools/ndnsf-di/spec109_source.py`
- [X] T009 Create ignored local result roots and retention notes for `results/spec109-itiger-qwen/` in `.gitignore` and `specs/109-ndnsf-di-itiger-qwen-scaling/quickstart.md`
- [X] T010 Run strict Spec Kit structural scan and retain the pre-implementation output in `specs/109-ndnsf-di-itiger-qwen-scaling/checklists/pre-implementation-audit.md`

**Checkpoint**: Spec 109 has disjoint identity and immutable predecessor boundaries; no download/job has run.

---

## Phase 2: Foundational contracts and offline gates

**Purpose**: Make model, storage, matrix, backend, submission, and evidence behavior executable before any external mutation.

- [X] T011 [P] Add valid/invalid model registry fixtures at `tests/container/itiger-qwen/fixtures/model-registry/`
- [X] T012 [P] Add quota, shared-capacity, scratch, projected-peak, reserve, and protected-path fixtures at `tests/container/itiger-qwen/fixtures/storage/`
- [X] T013 [P] Add source/predecessor/deployment/workload/candidate profile fixtures for every Qwen2.5 size at `tests/container/itiger-qwen/fixtures/profiles/`
- [X] T014 [P] Add keyed diagnostic/oracle/artifact/staged-baseline/correctness/performance matrix and partial-bundle fixtures at `tests/container/itiger-qwen/fixtures/matrix/`
- [X] T015 [P] Add Slurm lifecycle and GPU mapping fixtures for one-node 1/2/3/4/8-GPU allocations at `tests/container/itiger-qwen/fixtures/slurm/`
- [X] T016 [P] Add complete, missing, mixed, fallback, tampered, duplicate, partial, and false-authority evidence fixtures at `tests/container/itiger-qwen/fixtures/evidence/`
- [X] T017 Add positive and adversarial Schema tests for source, predecessor, profile, keyed matrix, and evidence contracts in `tests/container/itiger-qwen/unit/test_schemas.py`
- [X] T018 Add immutable revision, tokenizer, license, LFS pointer, file-size, and digest tests in `tests/container/itiger-qwen/unit/test_model_registry.py`
- [X] T019 Add storage projection, reserve, actual-quota precedence, and protected-cleanup tests in `tests/container/itiger-qwen/unit/test_storage_admission.py`
- [X] T020 Add source/predecessor/deployment/workload/candidate fingerprint and changed-binding/new-identity tests in `tests/container/itiger-qwen/unit/test_candidate_identity.py`
- [X] T021 Add keyed uniqueness, partial bundle, scoped systemic/model/placement gates, continuation, and terminal-cell tests in `tests/container/itiger-qwen/unit/test_scale_matrix.py`
- [X] T022 Add Slurm resource rendering, shell-injection rejection, and exact-once ledger tests in `tests/container/itiger-qwen/unit/test_job_render.py`
- [X] T023 Add node-level execution-provider coverage, all-CUDA, GPU UUID correlation, and CPU fallback degradation tests in `tests/container/itiger-qwen/unit/test_backend_gate.py`
- [X] T024 Add exact token arrays plus hidden/KV/logit tolerance/top-1-margin and reference-link tests in `tests/container/itiger-qwen/unit/test_correctness_gate.py`
- [X] T025 Add measured-cell immutability and no-auto-retry tests in `tests/container/itiger-qwen/unit/test_once_only.py`
- [X] T026 Add evidence promotion/original-exit/redaction/checksum tests in `tests/container/itiger-qwen/unit/test_evidence.py`
- [X] T027 Add three-plane authority, matched staged-baseline fingerprint, workload/cache/run-order, sample-threshold, CI, and unmatched-comparison rejection tests in `tests/container/itiger-qwen/unit/test_comparison.py`
- [X] T028 Implement model registry parsing/sealing in `tools/ndnsf-di/spec109_model_registry.py` (depends on T018)
- [X] T029 Implement storage admission/projection/protection/cleanup planning in `tools/ndnsf-di/spec109_storage.py` (depends on T019)
- [X] T030 Implement source/predecessor/deployment/workload/candidate and matched-comparison fingerprinting in `tools/ndnsf-di/spec109_candidate.py` (depends on T007-T008, T020)
- [X] T031 Implement keyed matrix expansion, partial-bundle closure, scoped gate propagation, and immutable transitions in `tools/ndnsf-di/spec109_matrix.py` (depends on T021, T025)
- [X] T032 Implement the canonical semantic validator for uniqueness, source/predecessors, comparison, backend, correctness, percentiles, authority, and redaction in `tools/ndnsf-di/validate_spec109.py` (depends on T017, T023-T027)
- [X] T033 Implement repository-local snapshot/validate/render/status/wait/cancel/evidence CLI without submit-by-default in `tools/ndnsf-di/ndnsf-di-qwen` (depends on T022, T028-T032)
- [X] T034 Add `ndnsf-di-qwen` to the container package without adding credentials or models in `packaging/ndnsf-di-container/bin/ndnsf-di-qwen`
- [X] T035 Run all T017-T027 tests plus duplicate/contradictory/source/predecessor/matched-baseline behavioral probes offline and retain JUnit/summary under `results/spec109-itiger-qwen/offline-foundation/`

**Checkpoint**: Offline gates fail closed and submit zero jobs.

---

## Phase 3: User Story 1 - Storage-safe model staging (P1)

**Goal**: Discover live capacity, create protected project layout, and seal model artifacts without local or `/home` bulk data.

**Independent Test**: Stage and seal 0.5B while proving path policy, immutable files, complete manifest, cleanup safety, and zero local bulk artifacts.

- [X] T036 [P] [US1] Add discovery output parser tests for quota/Lustre/shared-capacity/GRES/Apptainer/egress signals in `tests/container/itiger-qwen/unit/test_discovery.py`
- [X] T037 [P] [US1] Add project-layout ownership/mode/path-policy tests in `tests/container/itiger-qwen/unit/test_project_layout.py`
- [X] T038 [P] [US1] Add partial/resumed/completed transfer-state tests in `tests/container/itiger-qwen/unit/test_model_transfer.py`
- [X] T039 [P] [US1] Add local-workstation and `/home` bulk-write sentinel tests in `tests/container/itiger-qwen/integration/test_no_local_or_home_models.sh`
- [X] T040 [US1] Implement canonical read-only discovery/parsing in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/discover-itiger-qwen.sh` and `tools/ndnsf-di/spec109_storage.py`; keep any Skill wrapper optional (depends on T036)
- [X] T041 [US1] Implement protected layout initialization at `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/prepare-qwen-project.sh` (depends on T037)
- [X] T042 [US1] Implement CPU Slurm transfer template with bounded resources and no embedded secrets at `packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/qwen-transfer.sbatch.in`
- [X] T043 [US1] Implement immutable revision/file/license manifest transfer logic at `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/stage-qwen-model.py` (depends on T038, T042)
- [X] T044 [US1] Implement partial-download quarantine and manifest-verified promotion at `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/finalize-qwen-model.py`
- [X] T045 [US1] Implement content/reference-aware cleanup dry-run at `tools/ndnsf-di/spec109_storage.py` (depends on T029, T037)
- [X] T046 [US1] Add transfer render/preflight/status/evidence operations to `tools/ndnsf-di/ndnsf-di-qwen` (depends on T041-T045)
- [X] T047 [US1] Run read-only VPN/SSH/cluster/quota/GRES/Apptainer discovery and preserve exact output under `results/spec109-itiger-qwen/discovery/`
- [X] T048 [US1] Derive actual 0.5B-72B capacity projections and large-model eligibility under `results/spec109-itiger-qwen/discovery/storage-admission.json` (depends on T047)
- [X] T049 [US1] Initialize `/project/$USER/ndnsf-di/{src,images,models,cache,manifests,evidence}` once and retain modes/owners at `results/spec109-itiger-qwen/storage-layout/manifest.json` (depends on T047-T048)
- [X] T050 [US1] Preflight and submit exactly one bounded 0.5B transfer job; preserve first outcome under `results/spec109-itiger-qwen/models/0.5B-transfer/` (depends on T046, T049)
- [X] T051 [US1] Validate and seal the 0.5B model/tokenizer/license/file registry entry at `/project/$USER/ndnsf-di/manifests/models/qwen25-0.5b.json` (depends on T050)
- [X] T052 [US1] Prove local workstation and `/home` bulk-model sentinels remain clean under `results/spec109-itiger-qwen/storage-policy/`
- [X] T053 [US1] Execute cleanup dry-run and prove protected 0.5B/current-release/evidence paths remain excluded under `results/spec109-itiger-qwen/cleanup-dry-run/`
- [X] T054 [US1] Record compute-node egress availability or administrator-approved transfer fallback in `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/transfer-path.md`
- [X] T055 [US1] Derive the storage/model-staging verdict in `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/storage-verdict.md`

**Checkpoint**: One sealed 0.5B model exists durably; no candidate inference has run.

---

## Phase 4: User Story 2 - Standalone GPU reference (P1)

**Goal**: Establish an independent full-model correctness oracle and the inputs required for a matched staged ONNX baseline.

**Independent Test**: The 0.5B full-model oracle returns exact tokens; exported stages pass numerical checkpoints; the staged baseline uses the same workload/deployment fingerprint as the later candidate.

- [X] T056 [P] [US2] Add full-model oracle and matched staged-baseline profile/render tests in `tests/container/itiger-qwen/unit/test_standalone_profile.py`
- [X] T057 [P] [US2] Add tokenizer/prompt/input/output token digest tests in `tests/container/itiger-qwen/unit/test_reference_tokens.py`
- [X] T058 [P] [US2] Add GPU peak memory/utilization/UUID sampler tests in `tests/container/itiger-qwen/unit/test_gpu_sampler.py`
- [X] T059 [P] [US2] Add count-qualified percentile, CI, TTFT/inter-token/token-throughput parser tests in `tests/container/itiger-qwen/unit/test_reference_metrics.py`
- [X] T060 [US2] Add pinned standalone runtime dependencies to `packaging/ndnsf-di-container/oci/locks/qwen-reference.lock`
- [X] T061 [US2] Add a Qwen experiment layer extending the pinned Spec 108 GPU image at `packaging/ndnsf-di-container/oci/experiments/Dockerfile.qwen` without model weights, drivers, or credentials
- [X] T062 [US2] Implement deterministic full-model oracle runner at `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-qwen-reference.py` (depends on T057, T059-T061)
- [X] T063 [US2] Implement bounded GPU/resource sampler at `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/sample-qwen-resources.py` (depends on T058)
- [X] T064 [US2] Add oracle/staged-baseline Slurm templates with traps, scratch, read-only model bind, and durable promotion under `packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/`
- [X] T065 [US2] Add `oracle` and `staged-baseline` render/submit/status/wait/cancel/evidence operations to `tools/ndnsf-di/ndnsf-di-qwen`
- [X] T066 [US2] Verify and pin the Spec 108 GPU base release plus the Qwen experiment OCI/SIF materialization; preserve both lineage digests under `results/spec109-itiger-qwen/releases/gpu/` (depends on Spec 108 T091-T102 and T060-T061)
- [X] T067 [US2] Lock the 0.5B prompt order, arrival/load/concurrency/timeout, cache, warmup, logging, run-order seed, tokenizer, decode, context/output, and Spec 108 deployment digest in `specs/109-ndnsf-di-itiger-qwen-scaling/experiment/0.5b-reference.json`
- [X] T068 [US2] Submit one diagnostic 0.5B standalone smoke under a diagnostic-only run ID at `results/spec109-itiger-qwen/diagnostics/0.5B-standalone/` (depends on T051, T065-T067)
- [X] T069 [US2] Resolve diagnostic-only defects without promoting T068 and lock the acceptance candidate in `results/spec109-itiger-qwen/candidates/0.5B-reference.json`
- [X] T070 [US2] Submit exactly once the 0.5B standalone 1-token reference at `results/spec109-itiger-qwen/reference/0.5B/token-1/`
- [X] T071 [US2] Submit exactly once the 0.5B standalone 2-token reference at `results/spec109-itiger-qwen/reference/0.5B/token-2/` (depends on T070)
- [X] T072 [US2] Submit exactly once the 0.5B standalone 32-token reference at `results/spec109-itiger-qwen/reference/0.5B/token-32/` (depends on T071)
- [X] T073 [US2] Reconcile exact token arrays and hidden/KV/logit/top-1-margin tolerances across 1/2/32 cells at `results/spec109-itiger-qwen/reference/0.5B/token-verdict.json`
- [X] T074 [US2] Validate GPU UUID, node-level provider profile, load/TTFT/inter-token/sample counts/resources/promotion evidence at `results/spec109-itiger-qwen/reference/0.5B/manifest.json`
- [X] T075 [US2] Derive the standalone reference verdict in `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/0.5b-reference-verdict.md`

**Checkpoint**: The 0.5B standalone oracle is available; it is not NDNSF-DI PASS.

---

## Phase 5: User Story 3 - Real 0.5B NDNSF-DI GPU candidate (P1)

**Goal**: Prove actual NDNSF-DI correctness, security, backend truth, and performance on iTiger.

**Independent Test**: 1/2/32 exact tokens plus three original 60-second repetitions pass through the real secured runtime.

- [X] T076 [P] [US3] Add parameterized model/service/stage manifest tests replacing hard-coded 0.5B/CPU fields in `tests/python/test_ndnsf_di_spec109_qwen_manifest.py`
- [X] T077 [P] [US3] Add FP16 tensor/KV/stage contract tests in `tests/unit-tests/di-qwen-onnx-gpu.t.cpp`
- [X] T078 [P] [US3] Add per-node ORT provider-profile, all-CUDA/no-fallback, CPU-partition, incomplete-profile, and GPU-UUID negative tests in `tests/unit-tests/di-onnxruntime-gpu-evidence.t.cpp`
- [X] T079 [P] [US3] Add candidate-versus-oracle exact-token and numerical-checkpoint tests in `tests/python/test_ndnsf_di_spec109_exact_tokens.py`
- [X] T080 [P] [US3] Add permission/NAC-ABE/UserToken/ProviderToken/replay/provider-permission tests for packaged GPU execution in `tests/container/itiger-qwen/integration/test_packaged_security.sh`
- [X] T081 [US3] Parameterize Qwen model/revision/dtype/stage manifests in `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` without changing wire names (depends on T076)
- [X] T082 [US3] Correct only FP16 tensor/KV interoperability gaps demonstrated by T077-T078 in `NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.cpp`; retain existing supported Float16 behavior unchanged otherwise
- [X] T083 [US3] Enable ORT profiling and emit every model node's provider assignment plus stage/fallback/runtime/GPU/artifact evidence in `NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.cpp`
- [X] T084 [US3] Wire digest-bound parameterized Qwen stage artifacts and CUDA metadata into `examples/DI_NativeProviderExecutable.cpp`
- [X] T085 [US3] Implement allocation-local controller/NFD/user/provider orchestration in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-ndnsf-qwen.sh`
- [X] T086 [US3] Add bounded host-scoped NFD readiness, face/route, and cleanup handling in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-ndnsf-qwen.sh`
- [X] T087 [US3] Add NDNSF-DI Qwen Slurm template with one-node multi-GPU resource mapping at `packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/ndnsf-qwen.sbatch.in`
- [X] T088 [US3] Add candidate render/submit/status/wait/cancel/evidence operations to `tools/ndnsf-di/ndnsf-di-qwen`
- [X] T089 [US3] Validate the exact digest-bound Spec 107 T027/T028-T038 and Spec 108 T091-T102 manifest plus source/deployment digests at `results/spec109-itiger-qwen/predecessor-gate.json`
- [X] T090 [US3] Run focused C++/Python/packaged MiniNDN security and generation-session tests and retain output at `results/spec109-itiger-qwen/preflight/0.5B/`
- [X] T091 [US3] Export 0.5B stages once in allocation scratch and promote sealed artifacts under `/project/$USER/ndnsf-di/models/onnx/` (depends on T082-T084, T089-T090)
- [X] T092 [US3] Validate graphs/external data/tensor/KV/dtype/hash, hidden/KV/logit tolerances, top-1 margins, and 1/2/32 exact outputs under `results/spec109-itiger-qwen/artifacts/0.5B/`
- [X] T093 [US3] Lock the keyed 0.5B oracle/staged-baseline/candidate matrix, matched fingerprints, and per-cell bundle ledger at `results/spec109-itiger-qwen/candidates/0.5B-ndnsf-di.json`
- [X] T094 [US3] Submit exactly once the 0.5B NDNSF-DI 1-token correctness cell at `results/spec109-itiger-qwen/ndnsf-di/0.5B/token-1/`
- [X] T095 [US3] Submit exactly once the 0.5B NDNSF-DI 2-token correctness cell at `results/spec109-itiger-qwen/ndnsf-di/0.5B/token-2/` (depends on T094)
- [X] T096 [US3] Submit exactly once the 0.5B NDNSF-DI 32-token correctness cell at `results/spec109-itiger-qwen/ndnsf-di/0.5B/token-32/` (depends on T095)
- [X] T097 [US3] Derive exact-token/numerical/security/node-level-CUDA/promotion correctness verdict at `results/spec109-itiger-qwen/ndnsf-di/0.5B/correctness-verdict.json`
- [X] T098 [US3] Submit matched staged-baseline then candidate repetition 1 exactly once in locked order with excluded warmup and 60-second windows at `results/spec109-itiger-qwen/ndnsf-di/0.5B/perf-1/`
- [X] T099 [US3] Submit matched pair repetition 2 exactly once without replacing repetition 1 at `results/spec109-itiger-qwen/ndnsf-di/0.5B/perf-2/` (depends on T098)
- [X] T100 [US3] Submit matched pair repetition 3 exactly once without replacing earlier repetitions at `results/spec109-itiger-qwen/ndnsf-di/0.5B/perf-3/` (depends on T099)
- [X] T101 [US3] Derive separate per-repetition counts, confidence intervals, and validity-qualified p50/p95/p99/resource results for baseline and candidate at `results/spec109-itiger-qwen/ndnsf-di/0.5B/performance.json`
- [X] T102 [US3] Reconcile stage compute, queue, dependency wait, and NDN bytes/segments at `results/spec109-itiger-qwen/ndnsf-di/0.5B/critical-path.json`
- [X] T103 [US3] Compute candidate-minus-matched-staged-baseline overhead without Transformers timing or pooled rescue at `results/spec109-itiger-qwen/ndnsf-di/0.5B/overhead.json`
- [X] T104 [US3] Validate the original 0.5B bundle against both Spec 109 and Spec 108 T103 contracts, then run mutation/fallback/false-authority tests at `results/spec109-itiger-qwen/ndnsf-di/0.5B/mutation-tests/` without submitting a duplicate job
- [X] T105 [US3] Derive the 0.5B candidate verdict in `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/0.5b-candidate-verdict.md`

**Checkpoint**: A PASS proves the first real NDNSF-DI Qwen GPU candidate; failure remains a valid result.

---

## Phase 6: User Story 4 - 1.5B through 14B ladder (P1)

**Goal**: Apply the same three-plane design to small/medium sizes, retain model-local failures without censoring unrelated sizes, and separate descriptive from controlled scaling evidence.

**Independent Test**: Every 1.5B/3B/7B/14B tier is either complete or terminally gated, with no hidden skipped cells.

- [X] T106 [P] [US4] Add per-size model/revision/license/resource fixtures for 1.5B/3B/7B/14B at `tests/container/itiger-qwen/fixtures/ladder/`
- [X] T107 [P] [US4] Add systemic/model-local/placement-local propagation, independent-size continuation, and no-dependent-start tests in `tests/container/itiger-qwen/unit/test_ladder_gate.py`
- [X] T108 [P] [US4] Add stage-balance and transfer-volume estimator tests in `tests/container/itiger-qwen/unit/test_partition_planner.py`
- [X] T109 [US4] Generate an immutable keyed per-cell ledger for per-size transfer/oracle/export/staged-baseline/candidate work; bundled-job tasks close only when all listed cells are terminal in `tools/ndnsf-di/run_spec109_ladder.py` (depends on T106-T108)
- [X] T110 [US4] Preflight, transfer exactly once, validate, and seal 1.5B under `results/spec109-itiger-qwen/models/1.5B/`
- [X] T111 [US4] Run the 1.5B oracle 1/2/32 bundle, numerical artifact validation, and three matched staged-baseline cells under `results/spec109-itiger-qwen/reference/1.5B/`; close each ledger cell independently (depends on T110)
- [X] T112 [US4] Run the 1.5B candidate 1/2/32 bundle and three matched 60-second candidate cells under `results/spec109-itiger-qwen/ndnsf-di/1.5B/`; keep task open until every member is terminal (depends on T111)
- [X] T113 [US4] Derive the 1.5B terminal verdict and scoped gate at `results/spec109-itiger-qwen/verdicts/1.5B.json`
- [X] T114 [US4] If no systemic gate blocks it, preflight, transfer once, validate, and seal 3B with its model-local license record under `results/spec109-itiger-qwen/models/3B/`
- [X] T115 [US4] Run the keyed 3B oracle/artifact/staged-baseline/candidate matrix with per-cell closure under `results/spec109-itiger-qwen/3B/` (depends on T114)
- [X] T116 [US4] Derive the 3B terminal verdict and scoped gate without propagating a model-local failure at `results/spec109-itiger-qwen/verdicts/3B.json`
- [X] T117 [US4] If no systemic gate blocks it, preflight, transfer once, validate, and seal 7B under `results/spec109-itiger-qwen/models/7B/`
- [X] T118 [US4] Run the keyed 7B oracle/artifact/staged-baseline/candidate matrix with per-cell closure under `results/spec109-itiger-qwen/7B/` (depends on T117)
- [X] T119 [US4] Derive the 7B terminal verdict, transfer attribution, and scoped gate at `results/spec109-itiger-qwen/verdicts/7B.json`
- [X] T120 [US4] If no systemic gate blocks it, select and lock a Spec 108-derived RTX 6000 or H100 14B deployment binding from live evidence at `results/spec109-itiger-qwen/profiles/14B.json`
- [X] T121 [US4] Preflight, transfer exactly once, validate, and seal 14B under `results/spec109-itiger-qwen/models/14B/`
- [X] T122 [US4] Run the keyed 14B oracle/artifact/staged-baseline/candidate matrix with per-cell closure under `results/spec109-itiger-qwen/14B/` (depends on T120-T121)
- [X] T123 [US4] Derive the 14B terminal verdict and large-model gate input at `results/spec109-itiger-qwen/verdicts/14B.json`
- [X] T124 [US4] Generate a descriptive full-ladder table plus a separate controlled common-hardware subset; use only staged-baseline/candidate pairs at `results/spec109-itiger-qwen/analysis/small-medium-scaling.csv`
- [X] T125 [US4] Retain every independent size and scoped systemic/model/placement gate in the keyed ledger at `results/spec109-itiger-qwen/scale-matrix.json`

**Checkpoint**: The practical ladder is complete or honestly stopped; no large-model work starts implicitly.

---

## Phase 7: User Story 5 - 32B/72B and placement gates (P2)

**Goal**: Admit large models only with verified quota/GPU/network evidence and preserve capacity failures.

**Independent Test**: Mechanical 32B/72B decisions precede all transfers; admitted models use one-node/multi-GPU first.

- [X] T126 [P] [US5] Add 32B/72B source/export/cache/evidence amplification fixtures in `tests/container/itiger-qwen/fixtures/large-model/`
- [X] T127 [P] [US5] Add quota-expansion, insufficient-reserve, and no-transfer/no-submit tests in `tests/container/itiger-qwen/unit/test_large_model_admission.py`
- [X] T128 [P] [US5] Add one-node multi-GPU versus multi-node placement tests in `tests/container/itiger-qwen/unit/test_large_model_placement.py`
- [X] T129 [US5] Implement file-manifest-based large-model peak calculation in `tools/ndnsf-di/spec109_storage.py` (depends on T126-T127)
- [X] T130 [US5] Implement placement admission using memory, stage balance, GRES, and network evidence in `tools/ndnsf-di/spec109_matrix.py` (depends on T128)
- [X] T131 [US5] Rediscover quota/GRES and emit 32B/72B admission records at `results/spec109-itiger-qwen/large-model-admission/`
- [X] T132 [US5] If capacity blocks either size, retain the required quota request and prove zero transfer/job start in `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/large-model-capacity.md`
- [X] T133 [US5] If admitted, transfer/seal 32B exactly once and run standalone H100 reference under `results/spec109-itiger-qwen/32B/reference/`
- [X] T134 [US5] If T133 passes, run the keyed one-node multi-GPU 32B artifact/staged-baseline/correctness/performance matrix under `results/spec109-itiger-qwen/32B/ndnsf-di/`
- [X] T135 [US5] Derive the 32B terminal verdict at `results/spec109-itiger-qwen/verdicts/32B.json`
- [X] T136 [US5] If admitted and T135 passes, transfer/seal 72B exactly once and run multi-H100 standalone reference under `results/spec109-itiger-qwen/72B/reference/`
- [X] T137 [US5] If T136 passes, run the keyed one-node multi-GPU 72B artifact/staged-baseline/correctness/performance matrix under `results/spec109-itiger-qwen/72B/ndnsf-di/`
- [X] T138 [US5] Derive the 72B terminal verdict at `results/spec109-itiger-qwen/verdicts/72B.json`
- [X] T139 [US5] Enable a multi-node variant only if Spec 108 T134 network evidence passes and create a distinct candidate under `results/spec109-itiger-qwen/multinode-gate.json`
- [X] T140 [US5] Preserve all deferred/blocked large-model and multi-node cells in `results/spec109-itiger-qwen/scale-matrix.json`

**Checkpoint**: Large-model outcomes are capacity- and evidence-bound, never inferred from smaller sizes.

---

## Phase 8: User Story 6 - Reproducibility and scaling report (P2)

**Goal**: Produce an auditable matrix, matched analysis, reproduction result, and honest authority verdict.

**Independent Test**: Mutation cases block and one accepted small cell reproduces exact tokens with bounded metric variation.

- [X] T141 [P] [US6] Add complete denominator/terminal-state aggregator tests in `tests/container/itiger-qwen/unit/test_aggregate_matrix.py`
- [X] T142 [P] [US6] Add critical-path reconciliation and 99% coverage tests in `tests/container/itiger-qwen/unit/test_critical_path.py`
- [X] T143 [P] [US6] Add exact-token, numerical-checkpoint, sample-validity, and 90%-CI-within-preregistered-±10% engineering-equivalence tests in `tests/container/itiger-qwen/unit/test_reproduction.py`
- [X] T144 [P] [US6] Add secret/prompt/tensor/token redaction scanner tests in `tests/container/itiger-qwen/unit/test_redaction.py`
- [X] T145 [US6] Implement full matrix aggregation without successful-only filtering in `tools/ndnsf-di/run_spec109_analysis.py` (depends on T141)
- [X] T146 [US6] Implement staged-baseline matched fingerprints, candidate-minus-baseline overhead, controlled/descriptive separation, validity-qualified tails, critical-path, and confidence summaries in `tools/ndnsf-di/run_spec109_analysis.py` (depends on T142)
- [X] T147 [US6] Implement evidence/model/SIF/log secret scanner at `tools/ndnsf-di/scan_spec109_evidence.py` (depends on T144)
- [X] T148 [US6] Run all evidence mutation and false-authority cases under `results/spec109-itiger-qwen/final-mutation-tests/`
- [X] T149 [US6] Select one accepted 0.5B or 1.5B cell and submit one preregistered reproduction run under `results/spec109-itiger-qwen/reproduction/`
- [X] T150 [US6] Compare exact tokens, numerical checkpoints, and engineering-equivalence confidence intervals, retaining PASS/FAIL/INCONCLUSIVE under `results/spec109-itiger-qwen/reproduction/verdict.json`
- [X] T151 [US6] Generate complete per-cell and per-size CSV/JSON summaries under `results/spec109-itiger-qwen/analysis/`
- [X] T152 [US6] Generate matched-staged-baseline-versus-NDNSF-DI overhead and attribution report, keeping full-model oracle timing separate, at `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/scaling-report.md`
- [X] T153 [US6] Generate storage/GPU/model-size capacity report at `specs/109-ndnsf-di-itiger-qwen-scaling/evidence/capacity-report.md`
- [X] T154 [US6] Scan final SIF/model manifests/logs/evidence and retain zero-finding or failure result under `results/spec109-itiger-qwen/secret-scan/`
- [X] T155 [US6] Generate mechanical Spec 109 verdict with physical production forced deferred at `specs/109-ndnsf-di-itiger-qwen-scaling/release-gate.json`

**Checkpoint**: Every planned cell and negative result is represented; claims match evidence authority.

---

## Phase 9: Documentation, audit, and handoff

**Purpose**: Make the campaign runnable by another operator and close ownership boundaries.

- [X] T156 [P] Verify repository-local commands/docs remain canonical and no personal wrapper path becomes an execution dependency
- [X] T157 [P] Document model transfer, license, quota, and cleanup operations in `packaging/ndnsf-di-container/docs/itiger-qwen-models.md`
- [X] T158 [P] Document standalone/reference/artifact/candidate authority in `packaging/ndnsf-di-container/docs/itiger-qwen-evidence.md`
- [X] T159 Update `NDNSF-DistributedInference/README.md` with the measured Qwen size scope and exact commands
- [X] T160 Update `NDNSF-DistributedInference/README_ch.md` with commands and claim boundaries matching T159
- [X] T161 Reconcile every FR/SC/task/evidence link in `specs/109-ndnsf-di-itiger-qwen-scaling/traceability.md`
- [X] T162 Run the full offline/contract/MiniNDN suite and retain exact pass/fail/skip counts under `results/spec109-itiger-qwen/final-tests/`
- [X] T163 Run Spec Kit analyze, strict post-implementation code/evidence audit, CodeGraph sync/status, agent-context update, GSD health, and quickstart validation; record separately in `specs/109-ndnsf-di-itiger-qwen-scaling/checklists/post-implementation-audit.md`
- [X] T164 Create Spec 106 handoff listing only physical GPU performance, real-network/UAV, production-security, and soak work at `specs/106-ndnsf-di-physical-deployment/handoffs/spec109-itiger-qwen.md`
- [X] T165 Run the completion bell and record final tests, jobs, evidence, failures, deferred cells, storage retained, and next step in `specs/109-ndnsf-di-itiger-qwen-scaling/completion-summary.md`

## Dependencies and stop rules

```text
T001-T035 offline foundation
    -> T036-T055 storage + sealed 0.5B
    -> T056-T075 standalone oracle
    -> T076-T105 real 0.5B candidate
    -> T106-T125 1.5B-14B ladder
    -> T126-T140 conditional 32B/72B
    -> T141-T155 analysis/reproduction
    -> T156-T165 closure
```

- T050, T068, T070-T072, T091, T094-T100, each accepted ladder cell, T133-T137, and T149 are once-only under their locked identity.
- Standalone work does not authorize candidate execution; candidate work requires digest-bound Spec 107/108 prerequisites.
- A systemic gate failure stops its dependent cells; a model-local or placement-local failure never blocks an unrelated model identity.
- 32B/72B require actual quota admission; estimates are insufficient.
- Multi-node requires Spec 108 network PASS; otherwise it remains deferred.
- No measured failure may be retried, pooled away, or replaced by quantization/GPU/placement changes.
- Cleanup failure, ambiguous process ownership, or incomplete promotion stops subsequent live cells.

## Parallel opportunities

- Fixture/test tasks marked `[P]` operate on disjoint files.
- Offline schema/model/storage/backend tests can run concurrently.
- Documentation may proceed after interfaces stabilize.
- Live transfers, exports, correctness, and performance cells are deliberately sequential because ordering is part of capacity, candidate, and once-only evidence identity.

## MVP

Phases 1-5 through T105: sealed 0.5B model, standalone oracle, real secured NDNSF-DI GPU exact tokens, and three 60-second repetitions. Later sizes must not delay proof of this first complete vertical slice.

## Completion definition

- All applicable tasks are checked, or conditional tasks have explicit terminal `BLOCKED/DEFERRED/NOT_STARTED` records.
- Every preregistered cell has one terminal state and evidence/reason.
- No bulk model data was stored locally or in `/home`.
- Accepted cells bind source/model/tokenizer/export/OCI/SIF/job/backend/GPU/security/timing evidence.
- Failures and zero-completion cells remain visible.
- Physical production remains `DEFERRED`, owner Spec 106.
