# Tasks: TigerCluster DistributedRepo Throughput Validation

**Input**: Design documents from `specs/167-itiger-repo-throughput/`

## Phase 1: Foundational Experiment Contract

**Purpose**: Freeze portable role, schedule, path-isolation, and evidence behavior before any remote staging.

- [x] T001 Implement and test explicit shared-coordination versus rank-local payload/store/destination boundaries for raw, legacy, digest-only, signed-manifest, cold-retrieval, and warm-reuse roles in `Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py`, `Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py`, and `tests/python/test_spec167_itiger_artifact_runner.py`; require a real local two-NFD loopback smoke, full digest verification, zero-copy warm reuse, and rejection of measured paths under `/project` or `/home`
- [x] T002 [P] Freeze the randomized 64 MiB/1 GiB schedule, append-only run/evidence schema, retained-failure semantics, paired/bootstrap analysis, 11-fallacy checks, and independent checksum verification in `Experiments/analyze_spec167_itiger_repo.py`, `tests/python/test_spec167_itiger_artifact_runner.py`, and `specs/167-itiger-repo-throughput/contracts/evidence-schema.md`

**Checkpoint**: Portable data roles and analysis contract pass locally without Slurm or TigerCluster.

---

## Phase 2: User Story 2 - Deployment-Faithful Preflight (Priority: P2)

**Goal**: Prevent locally detectable packaging, path, cleanup, schema, identity, and concurrent-log defects from reaching TigerCluster.

**Independent Test**: The exact candidate image/source bundle passes a no-network import/package probe and all job-source contract tests; an injected defect blocks submission.

- [x] T003 [US2] Build the exact-artifact local and remote preflight as one acceptance outcome, including executable modes, host/container path separation, SIF/image API and package-data closure, source/SIF checksums, NFD/nfdc and checksum-bound TCP-ceiling capabilities, rank-local scratch, cleanup ordering, atomic logs, progress/deadline behavior, and immutable `.partial` failure handling in `specs/167-itiger-repo-throughput/jobs/validate-local-candidate.sh`, `specs/167-itiger-repo-throughput/jobs/repo-throughput-preflight.sbatch`, `specs/167-itiger-repo-throughput/jobs/rank.sh`, and `tests/python/test_spec167_itiger_job_contract.py`
- [x] T004 [US2] Stage one checksum-bound Spec 167 source identity, execute one bounded CPU-only two-node remote preflight, preserve its Slurm/node/container/network/scratch evidence, and record PASS or the unreplaced failure in `specs/167-itiger-repo-throughput/evidence/remote-preflight.md`

**Checkpoint**: Formal submission remains blocked unless T001--T004 pass under one immutable source identity.

---

## Phase 3: User Story 1 - Real-Cluster Matched Throughput (Priority: P1) 🎯 MVP

**Goal**: Produce the first defensible real-cluster comparison of new and legacy repository transport.

**Independent Test**: Independently recompute all 10 cell identities, 10 warmups, 50 measured outcomes, medians, confidence intervals, and claim boundaries from promoted evidence.

- [x] T005 [US1] Freeze and submit exactly one two-node CPU campaign using the preflight-qualified source, monitor durable progress every 30--60 seconds under the hard deadline, retain every measured failure, verify cross-node NDN rather than shared-NFS transfer, and promote only checksum-valid sanitized evidence through `specs/167-itiger-repo-throughput/jobs/repo-throughput.sbatch`, `specs/167-itiger-repo-throughput/jobs/rank.sh`, and `/project/tma1/ndnsf-di/evidence/spec167/`
- [ ] T006 [US1] Analyze and independently verify absolute goodput, signed/legacy improvement, digest/raw preservation, signed/digest overhead, physical-ceiling utilization, resource/storage metrics, distributions, bootstrap intervals, and PASS/FAIL/INCONCLUSIVE outcomes without replacing failures in `specs/167-itiger-repo-throughput/evidence/performance-report.md`, `specs/167-itiger-repo-throughput/evidence/independent-verification.md`, and `specs/167-itiger-repo-throughput/audit.md`

T006 throughput/count/integrity analysis is documented and independently
reproduced. It remains open only for resource-cost counters that the current
run schema does not emit (host CPU, peak memory, wire-byte breakdown, and full
read/write amplification).

**Checkpoint**: The report answers how much throughput improved on TigerCluster and whether the new path approaches the measured raw and physical ceilings.

---

## Phase 4: User Story 3 - Cold Transfer Versus Warm Reuse (Priority: P3)

**Goal**: Attribute first-request preparation and later cache reuse correctly.

**Independent Test**: Cold publication/retrieval transfers full verified bytes; warm reuse verifies the same identity while writing zero duplicate payload bytes.

- [x] T007 [US3] Validate and report cold publication, fresh-destination cold retrieval, and content-addressed warm reuse as separate cache states, including full digest, atomic visibility, duplicate-byte accounting, and model-distribution claim boundaries in `specs/167-itiger-repo-throughput/evidence/performance-report.md` and `specs/167-itiger-repo-throughput/audit.md`

---

## Phase 5: Cross-Cutting Closure

- [ ] T008 Run the post-implementation CodeGraph, Spec Kit, security, lifecycle, evidence, ARS 11-fallacy, and Context Mode audit; synchronize the final status and recommended follow-up scale cells in `specs/167-itiger-repo-throughput/traceability.md`, `specs/167-itiger-repo-throughput/tasks.md`, and `specs/167-itiger-repo-throughput/audit.md`

## Dependencies

```text
T001 ─┐
      ├─> T003 -> T004 -> T005 -> T006 -> T007 -> T008
T002 ─┘
```

- T001 and T002 can proceed in parallel because they own portable execution and analysis contracts respectively.
- T003 requires both contracts; T004 requires the exact T003 source identity.
- T005 is blocked by a passing remote preflight and may be submitted exactly once.
- T006--T008 consume immutable campaign evidence and cannot precede T005.

## Implementation Strategy

1. Close portable role/path and analyzer correctness locally.
2. Treat deployment preflight as the first deliverable; do not consume formal cluster time until it passes.
3. Run the smallest external-validity matrix that answers the throughput question.
4. Preserve the first formal outcome, positive or negative.
5. Add replication/concurrency scale only as a later Spec if the r1/c1 result is trustworthy.

## Fragmentation Scan

PASS. Tests, implementation, focused verification, and evidence for each behavior remain in one cohesive task. Remote preflight and formal measurement remain separate because they carry different operational risk and independently meaningful acceptance results.
