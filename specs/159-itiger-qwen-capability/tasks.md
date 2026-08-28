# Tasks: iTiger NDNSF-DI Qwen Capability

## Phase 1: Foundational gate

- [x] T001 Freeze live discovery, local candidate, remote model completeness, execution identities, and pre-implementation audit in `specs/159-itiger-qwen-capability/evidence/preflight.md` and `audit.md`.

## Phase 2: User Story 1 - Immutable OCI and SIF

- [ ] T002 [US1] Publish the unique Spec 159 capability tag, resolve and record its immutable OCI digest, render/review/submit one bounded CPU materialization job, and promote a verified SIF plus terminal evidence under `results/spec159-itiger-qwen-capability/` and `/project/tma1/ndnsf-di/evidence/spec159/`.

## Phase 3: User Story 2 - Standalone Qwen

- [ ] T003 [US2] Render/review/submit one bounded single-GPU job using the exact SIF and model revision, verify container CUDA/PyTorch/ORT constraints, run deterministic standalone Qwen, and preserve complete terminal evidence under the Spec 159 evidence roots.

## Phase 4: User Story 3 - NDNSF-DI Qwen

- [ ] T004 [US3] Render/review/submit one bounded single-GPU real NDNSF-DI job with job-local NFD and secured controller/requester/provider flow, then correlate the Qwen backend and requester-visible response in the Spec 159 evidence roots.

## Phase 5: Completion

- [ ] T005 Audit every FR/SC against immutable OCI/SIF/job/model/request evidence, preserve all negative outcomes and limitations, update `specs/159-itiger-qwen-capability/completion-summary.md`, and ring the completion bell.

## Dependencies

T001 -> T002 -> T003 -> T004 -> T005. No later live gate may run after an
unresolved predecessor failure.
