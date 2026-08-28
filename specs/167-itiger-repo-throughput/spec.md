# Feature Specification: TigerCluster DistributedRepo Throughput Validation

**Feature Branch**: `167-itiger-repo-throughput`

**Created**: 2026-07-31

**Status**: Draft

**Input**: Continue from the completed MiniNDN and the current TigerCluster
Qwen requalification by measuring how much NDNSF-DistributedRepo improves over
the legacy path on real TigerCluster nodes, without treating staged model
artifacts or total Qwen job time as repository-throughput evidence. The Qwen
attempts are useful for discovering cold-publication and catalog-activation
phases, but they are not matched transport subjects.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Obtain a trustworthy real-cluster throughput result (Priority: P1)

As an NDNSF-DI researcher, I can compare the scalable repository path with the legacy repository path and matched network ceilings on the same TigerCluster allocation so that I know whether the MiniNDN improvement survives deployment on real nodes.

**Why this priority**: The current 2.65-times improvement is MiniNDN evidence only. Model-distribution and publication decisions require an external-validity result from the actual cluster.

**Independent Test**: Run one immutable two-node campaign containing the physical-network, raw segmented NDN, legacy exact-packet, digest-only, and signed-manifest subjects, then independently recompute every reported ratio from the retained sample ledger.

**Acceptance Scenarios**:

1. **Given** one frozen allocation, payload, route, artifact identity, and measurement schedule, **when** all five subjects execute, **then** their goodput, completion, failures, CPU, memory, wire bytes, retransmissions, and storage amplification are retained under unique run identities.
2. **Given** a completed campaign, **when** the analyzer reports an improvement, **then** the report distinguishes absolute goodput, improvement over legacy, preservation of raw NDN goodput, and utilization of the measured physical-network ceiling.
3. **Given** any failed measured run, **when** the campaign closes, **then** the failure remains in the denominator and is not silently retried or replaced.

---

### User Story 2 - Catch deployment defects before consuming cluster time (Priority: P2)

As an experiment operator, I can validate the exact source bundle, container, executable modes, host/container paths, package data, API signatures, evidence schema, cleanup, and concurrent logging before submission.

**Why this priority**: Seven Spec 166 failures were locally detectable deployment-fidelity defects rather than cluster-only failures.

**Independent Test**: Execute the same preflight against the exact immutable candidate bundle and require every check to pass before the single authorized submission identity can be created.

**Acceptance Scenarios**:

1. **Given** an incomplete or incompatible candidate, **when** local preflight runs, **then** submission is blocked with a precise failure and no Slurm job is created.
2. **Given** a passing preflight, **when** the remote source is staged, **then** its checksum manifest binds every executable, configuration, analyzer, and container identity used by the job.

---

### User Story 3 - Separate model reuse from repository transfer (Priority: P3)

As an NDNSF-DI designer, I can distinguish cold publication, cold retrieval, and warm content-addressed reuse so that slow first requests and fast later requests are attributed correctly.

**Why this priority**: Earlier Qwen jobs used pre-staged artifacts; the new
requalification exercises publication, but mixes 53.79 GB model staging with
registration, provider preparation, and inference. It therefore cannot
quantify isolated DistributedRepo publication or fetch throughput.

**Independent Test**: Measure cold publication and retrieval into a fresh destination, then perform a checksum-verified warm reuse operation without copying the payload again.

**Acceptance Scenarios**:

1. **Given** a fresh destination, **when** an artifact is retrieved, **then** completion is reported only after the full immutable digest is verified and the destination is atomically visible.
2. **Given** the same immutable artifact already present, **when** it is requested again, **then** the report identifies a cache hit separately and records zero duplicate payload bytes written.

### Edge Cases

- A node, route, container, or scratch directory becomes unavailable after preflight.
- The first segmented Interest or a later segment times out and bounded retries are exhausted.
- A measured run completes with a digest mismatch, partial destination, missing evidence row, duplicate request ID, or leaked credential.
- The physical-network ceiling changes materially during the campaign.
- The signed path is faster than its matched digest-only pair because of natural variance.
- Shared project storage is accidentally used as the measured data path instead of cross-node NDN transfer.
- A warm cache hit is accidentally pooled with cold-transfer throughput.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The campaign MUST compare physical-network, raw segmented NDN, legacy exact-packet, digest-only, and signed-manifest subjects under one matched allocation and frozen schedule.
- **FR-002**: The primary large-artifact population MUST include 64 MiB and 1 GiB payloads, one selected replica, and one concurrent artifact; broader scale cells are outside this first external-validity campaign.
- **FR-003**: Every admitted subject/size cell MUST retain one warmup and five measured repetitions, with measured failures retained as zero-goodput outcomes where appropriate.
- **FR-004**: Every rate-over-time measurement MUST cover at least 60 seconds, using repeated immutable transfers when one object completes sooner.
- **FR-005**: The campaign MUST use two distinct TigerCluster compute nodes and MUST record allocation, node, interface, route, link ceiling, container, source, and payload identities.
- **FR-006**: The raw and repository subjects MUST traverse the same cross-node NDN path; shared project storage MUST be limited to immutable inputs and durable evidence and MUST NOT substitute for the measured transfer.
- **FR-007**: Repository measurements MUST distinguish cold publication, cold retrieval to a fresh destination, and warm content-addressed reuse.
- **FR-008**: Every result MUST report logical goodput, physical-ceiling utilization, raw-ceiling preservation, legacy improvement, end-to-end latency, p50/p95, CPU, peak memory, Data and Interest wire bytes, retransmissions, read/write amplification, and failure reason.
- **FR-009**: The signed and digest-only paths MUST validate identical content and differ only in the frozen trust subject; the report MUST not treat public shared HMAC as the default trust model.
- **FR-010**: Local deployment preflight MUST validate the exact candidate artifact, executable modes, host/container paths, runtime API signatures, package-data closure, secret cleanup ordering, evidence fields, request identities, and concurrent log atomicity before submission.
- **FR-011**: Remote staging MUST be immutable and checksum-bound; accepted and failed submission identities MUST never be overwritten.
- **FR-012**: Progress MUST be monitored from durable activity markers with a bounded hard deadline; a fixed silence timeout alone MUST NOT classify slow progress as failure.
- **FR-013**: Any missing sample, duplicate identity, checksum failure, partial destination, credential residue, or unmatched subject pair MUST fail campaign acceptance.
- **FR-014**: The analyzer MUST preserve sample-level evidence and compute paired medians, bootstrap confidence intervals, and descriptive distributions without pooling unmatched cells.
- **FR-015**: The result MUST explicitly state that it measures repository transport and MUST NOT claim Qwen inference, GPU scalability, or model-loading performance.
- **FR-016**: Any model-backed diagnostic MUST report payload transfer,
  root-manifest/catalog `ACTIVE` commit, Provider fetch/cache, and inference as
  separate phases; a stage-file timestamp or total Slurm elapsed time MUST NOT
  be used as a repository-throughput metric.

### Key Entities *(include if feature involves data)*

- **Campaign Identity**: Immutable experiment ID binding the allocation, candidate, source, payload, routes, subjects, schedule, and analysis contract.
- **Matched Cell**: One payload size, replica count, concurrency level, node pair, and repeated observations for all compared subjects.
- **Run Record**: One warmup or measured outcome with identity, timings, bytes, resources, integrity verdict, progress, and failure state.
- **Artifact Identity**: Immutable model-independent content identity containing logical name, byte size, content digest, format version, and trust-policy epoch.
- **Transfer Phase**: Cold publication, cold retrieval, or warm reuse, never pooled across cache states.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The exact-artifact local preflight passes all deployment-fidelity checks before any new Slurm submission is made.
- **SC-002**: One immutable two-node campaign retains 10 admitted subject/size cells, 10 warmups, and 50 unique measured repetitions with no missing or duplicate run identity.
- **SC-003**: At least 95% of scheduled measured runs complete with verified content; every failure remains visible in the result and denominator.
- **SC-004**: For each size, the report provides a paired signed-manifest/legacy median improvement and a 95% bootstrap confidence interval.
- **SC-005**: For each size, the report provides paired digest/raw and signed/digest ratios and physical-ceiling utilization, with no claim of link saturation unless the lower confidence bound reaches 90%.
- **SC-006**: Cold retrieval exposes no partial destination, and all successful cold and warm results match the frozen artifact digest.
- **SC-007**: Warm reuse writes zero duplicate payload bytes and is reported separately from cold-transfer goodput.
- **SC-008**: Independent analysis reproduces campaign counts, medians, ratios, checksums, and PASS/FAIL/INCONCLUSIVE verdicts exactly from retained evidence.
- **SC-009**: No private key, bootstrap token, source payload duplicate, or unbounded scratch artifact remains in promoted evidence.

## Assumptions

- The authorized TigerCluster access and `/project/tma1/ndnsf-di` durable storage remain available today.
- The accepted Spec 166 SIF is reused only if an exact import and package-closure preflight proves it contains the current repository runtime; otherwise a new immutable candidate identity is required before submission.
- CPU-only resources are sufficient for this transport experiment; requesting GPUs would add no experimental value.
- The two-node campaign is an external-validity measurement, not a replacement for the frozen Spec 164 MiniNDN campaigns.
- The first campaign deliberately limits scale to single-replica, single-concurrency cells so that transport and trust overhead can be isolated before replication/concurrency follow-up.
