# Feature Specification: NDNSF-DI iTiger Distributed Qwen Execution

**Feature Branch**: `110-itiger-qwen-live-inference`
**Created**: 2026-07-13
**Status**: In progress — offline foundation and MiniNDN packaged-path work are
implemented; live iTiger runtime release and inference remain open
**Input**: Install the complete NDNSF-DI environment on iTiger and use it to
perform genuine distributed Qwen inference across model sizes.

## Scope and authority

Spec 110 owns the first real iTiger execution of Qwen through the NDNSF-DI
distributed data path. It includes the OCI-to-Apptainer environment, NFD/NDN
network, ServiceController, identities and NAC-ABE policy, NDNSF Core/Python
bindings, NDNSF-DI orchestration and providers, Qwen stage artifacts, CUDA GPU
backend, Slurm lifecycle, evidence, correctness, and performance.

The shortest primary candidate uses one iTiger compute node, three distinct GPU
stage-provider processes, three allocated GPUs, and one allocation-scoped NFD.
This proves the real NDNSF-DI request/security/stage/dependency/response path
without making inter-node networking a confounder. A separately identified
0.5B extension then places the same candidate across at least two compute nodes
and requires one cross-node NDN dependency. A full-model Qwen run is a
correctness/capacity oracle only; it cannot satisfy either candidate task.

Spec 110 consumes the source and contracts created by Specs 107 and 108, but
old unchecked task IDs are not acceptance evidence. Missing generation-session,
GPU-release, process-topology, or multi-node-network capability must be
implemented and validated before its dependent live cell; a blocker leaves the
live task open.

Spec 110 grants experimental iTiger candidate evidence only. Spec 106 retains
physical UAV/network, production-security, long-soak, and operational-release
authority.

## User Scenarios & Testing

### User Story 1 - Provision a complete reproducible iTiger runtime (Priority: P1)

An operator materializes one digest-bound Apptainer SIF and project-storage
layout containing every user-space dependency required to run NFD, NDNSF,
NDNSF-DI, Qwen export/reference code, ONNX Runtime GPU, PyTorch, and CUDA-bound
inference inside Slurm allocations.

The primary build path compiles and tests the expensive NFD, NDN, security, and
GPU-independent NDNSF foundation locally from checksum-bound sealed archives.
Only that foundation's immutable GHCR digest may enter the dispatch-only GitHub
workflow. Buildx then adds the pinned ONNX Runtime GPU SDK, CUDA/PyTorch Python
closure, and the small native ONNX provider adapter; it does not rebuild NFD or
OpenABE. An iTiger CPU allocation materializes the resulting immutable digest as
SIF, and iTiger GPU allocations provide the only GPU acceptance evidence.

**Why this priority**: The cluster supplies Slurm, Apptainer, and the NVIDIA
driver, but not the project runtime. A GPU-visible shell is not an NDNSF-DI
deployment.

**Independent Test**: In one bounded compute allocation, start the SIF with
`--nv`; run version/link/import checks; start and stop an unprivileged NFD; load
the NDNSF Python binding; and preserve OCI digest, SIF checksum, host driver,
container CUDA, library, and command evidence.

**Acceptance Scenarios**:

1. **Given** a clean iTiger account, **When** bootstrap runs, **Then** sealed
   release records, SIFs, models, identities, and evidence are placed under
   `/project/$USER/ndnsf-di`; the OCI is fetched only by immutable GHCR digest,
   and any fallback rootless build storage uses compute scratch rather than the
   default `/home/$USER/.local/share/containers` graphroot.
2. **Given** a compute allocation, **When** the SIF starts with `--nv`, **Then**
   NFD, ndn-cxx tools, NDNSF bindings, NDNSF-DI, PyTorch, Transformers, ONNX
   Runtime, and the allocated CUDA devices all pass pinned probes.
3. **Given** host/container CUDA incompatibility or a CPU-only backend, **When**
   preflight runs, **Then** the candidate is blocked before inference and no GPU
   PASS is recorded.
4. **Given** any identity or credential, **When** the image is scanned, **Then**
   no private key, password, MFA material, or access token exists in OCI/SIF.

---

### User Story 2 - Run the first real single-node multi-GPU candidate (Priority: P1)

An investigator runs Qwen2.5-0.5B-Instruct through one NDNSF-DI generation
session with three distinct stage-provider processes and three allocated GPUs
on one iTiger node, sharing one job-scoped NFD and returning exact final tokens
through the secured NDNSF response path.

**Why this priority**: This is the shortest real vertical slice. It proves
NDNSF-DI distributed role execution while controlling away inter-node routing,
firewall, and heterogeneous-node effects.

**Independent Test**: Execute deterministic 1-, 2-, and 32-token cells. Each
cell proves three distinct provider PIDs, three allocated GPU mappings, all
three stage executions, NDN dependency production/fetch, one final response,
and token equality with a pinned full-model oracle.

**Acceptance Scenarios**:

1. **Given** one GPU allocation, **When** the process supervisor starts, **Then**
   it launches exactly one NFD, distinct Controller/User/Provider identities,
   three provider processes, readiness barriers, and deterministic teardown.
2. **Given** one generation request, **When** it completes, **Then** stage 0,
   stage 1, and stage 2 each publish execution/dependency evidence on a distinct
   allocated GPU without CPU fallback.
3. **Given** an unavailable stage, wrong digest, token mismatch, replay, stale
   lease, missing dependency, or duplicate terminal response, **When** detected,
   **Then** the candidate fails closed without fabricated output.
4. **Given** only a standalone model process or three stages in one process,
   **When** evidence is validated, **Then** it cannot satisfy this story.

---

### User Story 3 - Extend the candidate across iTiger nodes (Priority: P1)

After the single-node candidate passes, an operator launches one unprivileged
NFD per participating compute node, creates explicit TCP faces/routes inside
one Slurm allocation, and runs the same 0.5B candidate with at least one stage
dependency crossing a node boundary.

**Why this priority**: This proves inter-node NDN behavior while preserving the
single-node candidate as a matched placement baseline. Network failure does not
invalidate the already measured model/runtime path.

**Independent Test**: A five-minute two-node CPU probe first proves the selected
NFD transport and secured generic invocation. The separately frozen multi-node
0.5B candidate then passes the 32-token exact-output cell with three GPU stage
records and at least one cross-node dependency.

**Acceptance Scenarios**:

1. **Given** two allocated nodes, **When** network preflight runs, **Then** it
   records allocation-local addresses, selected transport, NFD configs, faces,
   routes, and round-trip Interests without assuming public ingress.
2. **Given** the selected TCP transport, **When** TCP and NFD face/route probes
   pass but diagnostic UDP is unavailable, **Then** TCP execution remains
   eligible; UDP blocks only a candidate that explicitly selects UDP.
3. **Given** controller/user/provider startup, **When** one generic service is
   invoked, **Then** permission distribution, NAC-ABE attributes, UserToken,
   ProviderToken, selection, response, and teardown evidence are retained.
4. **Given** the multi-node candidate completes, **When** compared, **Then** its
   network/placement delta uses the exact single-node NDNSF-DI candidate—not
   the standalone oracle or local staged baseline—as its matched reference.
5. **Given** job termination, **When** cleanup runs, **Then** all NFD and
   application processes terminate and no login-node daemon exists.

---

### User Story 4 - Scale distributed execution across Qwen sizes (Priority: P1)

An investigator executes the same single-node multi-GPU NDNSF-DI protocol for
Qwen2.5-Instruct 0.5B, 1.5B, 3B, 7B, 14B, 32B, and 72B, with explicit stage
boundaries and GPU placements suitable for each size. Cross-node size scaling
is a separate future matrix after the 0.5B network extension.

**Why this priority**: The requested experiment concerns real model-size
scaling, not one toy model.

**Independent Test**: Every size reaches a real single-node, three-provider GPU
candidate boundary and records PASS or a model/runtime failure after candidate
execution begins. Pre-start quota, scheduler, export, or environment blockers
do not close that size's task.

**Acceptance Scenarios**:

1. **Given** a new size, **When** its artifacts are produced, **Then** the
   tokenizer, full model, stage exports, tensor interfaces, dtype, revisions,
   sizes, and digests receive a new immutable candidate identity.
2. **Given** 0.5B through 7B, **When** the controlled ladder runs, **Then** the
   same GPU class, topology, prompt, decoding, and stage-count policy are used
   wherever live fit permits.
3. **Given** 14B, 32B, or 72B, **When** a larger GPU/count is required, **Then**
   the different placement is preregistered and comparisons are descriptive.
4. **Given** insufficient `/project` quota for 32B/72B, **When** admission runs,
   **Then** it emits a concrete expansion request and leaves the live task open.
5. **Given** an actual post-start OOM, CUDA, export-interface, or correctness
   failure, **When** finalized, **Then** the failure is a valid negative result;
   a changed partition/quantization/placement requires a new identity.

---

### User Story 5 - Measure distributed cost and scaling (Priority: P1)

An investigator compares each successful distributed candidate with its pinned
full-model correctness oracle and a matched staged local baseline that uses the
same exported stages and GPU placement without NDN/security/orchestration.

**Why this priority**: Only a matched staged baseline can isolate the cost of
NDNSF-DI coordination and dependency transport.

**Independent Test**: After correctness passes, execute three independent
60-second candidate repetitions and three matched staged repetitions per size,
with excluded warmup and preserved request/stage/network/GPU samples.

**Acceptance Scenarios**:

1. **Given** a correctness PASS, **When** measurement begins, **Then** warmup is
   outside an exactly 60-second measured window and no run is auto-retried.
2. **Given** candidate and baseline evidence, **When** overhead is computed,
   **Then** model, artifacts, GPUs, topology, tokens, workload, and repetition
   match except for the NDNSF-DI treatment.
3. **Given** a completed run, **When** aggregated, **Then** completion/failure,
   TTFT, inter-token latency, tokens/s, throughput, stage compute, dependency
   transfer, NDN/security/orchestration, CPU RSS, GPU memory/utilization, and
   scheduler wait are reported separately.
4. **Given** too few samples, **When** percentiles are generated, **Then** p50,
   p95, or p99 remains unavailable below 20, 100, or 1000 observations.

---

### User Story 6 - Operate safely and preserve admissible evidence (Priority: P2)

An operator can monitor, cancel, clean up, reproduce, and audit both placement
classes without losing accepted artifacts or confusing experimental evidence
with production authority.

**Independent Test**: Reproduce one accepted 0.5B cell under a new identity,
validate every manifest/digest, dry-run cleanup, scan secrets, and prove all
processes terminate when the Slurm allocation ends.

**Acceptance Scenarios**:

1. **Given** a pending/running/preempted/timed-out/cancelled job, **When** status
   is reconciled, **Then** the original Slurm state, exit, nodes, logs, and
   partial evidence remain visible.
2. **Given** scratch teardown, **When** a job ends, **Then** durable evidence is
   promoted atomically before the recorded job-unique selected scratch path is
   disposable.
3. **Given** cleanup, **When** dry-run executes, **Then** active jobs, identities,
   sealed models, current/prior SIFs, candidate manifests, and accepted evidence
   are protected.
4. **Given** accepted iTiger results, **When** authority is summarized, **Then**
   `candidateExperiment` may PASS while `physicalProduction` remains DEFERRED
   to Spec 106.

### Edge Cases

- VPN/Duo becomes unavailable after job submission.
- Slurm account, QOS, partition, node, or GRES labels change.
- Compute nodes have different Apptainer versions or driver visibility.
- Shared `/project` is visible but a per-user quota command is unavailable.
- Rootless Podman/Buildah exists on the login node but differs or is unavailable
  inside the allocated compute node.
- Slurm advertises `/scratch` but `SLURM_TMPDIR` is unset or a selected scratch
  path is not writable.
- A pinned builder SIF starts but nested Buildah cannot create the required user
  namespace or execute `RUN` steps with VFS/chroot isolation.
- NFD selects a management address unreachable from another compute node.
- NFD TCP works but UDP is filtered, or vice versa.
- One provider starts late, dies, or publishes a stale boot/lease identity.
- A stage artifact loads but exposes incompatible tensor names/shapes/dtypes.
- The container sees a GPU while ONNX Runtime or PyTorch selects CPU.
- A cross-node dependency is partial, replayed, corrupted, or published twice.
- A generation emits zero tokens, invalid IDs, duplicate terminal responses, or
  tokens different from the oracle.
- A job is preempted before versus after distributed execution begins.
- Evidence promotion fails after a valid final response.
- 72B cannot be stored or exported within current quota.
- A stale Spec 109 identity is presented to the Spec 110 validator.

## Requirements

### Functional Requirements

- **FR-001**: All compute MUST run in Slurm allocations; no NFD, NDNSF-DI,
  model, export, or inference daemon may run persistently on the login node.
- **FR-002**: The runtime release MUST be a pinned OCI source materialized as a
  checksum-bound SIF and MUST include NFD, ndn-cxx, ndn-svs, NAC-ABE, NDNSF
  Core/Python, NDNSF-DI, Qwen tooling, PyTorch, Transformers, and ONNX Runtime
  GPU. The stable NFD/NDN/OpenABE/NAC-ABE/NDNSF foundation MUST be built and
  tested locally from checksum-bound sealed archives and published by immutable
  digest. The dispatch-only GitHub Buildx stage MUST consume that digest and MAY
  build only the pinned ONNX/CUDA/Python GPU delta and native ONNX adapter before
  publishing the final immutable GHCR digest. The operator MUST configure the
  GHCR package public before dispatch, and the workflow MUST prove anonymous
  digest access without registry credentials before accepting the release. It
  MUST NOT clone or rebuild the sealed foundation dependencies. Rootless
  Podman/Buildah in a bounded Slurm CPU allocation is a separately qualified
  fallback under the same locks.
- **FR-003**: The host supplies only the NVIDIA driver, Slurm, and Apptainer;
  user-space CUDA/runtime libraries MUST be versioned in the release and invoked
  with `apptainer exec --nv`.
- **FR-004**: Bulk code, OCI archives, SIF, models, stage artifacts, identities,
  and evidence MUST use `/project/$USER/ndnsf-di`; build and execution scratch
  MUST use a recorded job-unique compute path selected from `SLURM_TMPDIR`, the
  configured Slurm `TmpFS` (currently `/scratch`), or a validated `/tmp`
  fallback. `/home` is small-config only and MUST NOT be the rootless container
  graphroot, runroot, cache, or build context.
- **FR-037**: Qwen weights and exports MUST remain outside the OCI build context
  and image under `/project/$USER/ndnsf-di`; Actions artifacts MUST retain only
  manifests, SBOM, signatures, checksums, and logs, never the OCI image or model
  weights.
- **FR-005**: Secrets and private identity material MUST be excluded from OCI/SIF
  and bound read-only at runtime with minimal scope and redacted evidence.
- **FR-006**: Live partition, account, QOS, GRES, node, quota, Apptainer,
  rootless Podman/Buildah, compute-scratch, driver, and address facts MUST be
  rediscovered before the submission wave that consumes them; login-node tool
  presence alone MUST NOT prove compute-node build capability.
- **FR-007**: Multi-node mode MUST remain disabled until an allocation-scoped
  probe passes the candidate-selected NFD transport (TCP by default), NFD
  face/route state, and secured generic invocation on at least two compute
  nodes; unselected transports are diagnostic and cannot block eligibility.
- **FR-008**: Each participating node MUST run one unprivileged, job-scoped NFD
  with an explicit config, state directory, listener, face, route, and teardown.
- **FR-009**: Controller, User, and Providers MUST use distinct expected
  identities and preserve permission encryption, NAC-ABE routing, UserToken,
  ProviderToken, replay protection, selection, lease, and provider permission.
- **FR-010**: The primary candidate MUST place three Qwen stage roles in three
  distinct provider processes on three allocated GPUs of one compute node,
  using one job-scoped NFD; the separately identified multi-node 0.5B extension
  MUST span at least two nodes and carry at least one dependency edge through a
  cross-node NDN face.
- **FR-011**: The experiment MUST use one bounded generation session per request,
  preserve token epoch/KV/deadline/cancellation semantics, and publish exactly
  one terminal response.
- **FR-012**: The matrix MUST include immutable Qwen2.5-Instruct 0.5B, 1.5B, 3B,
  7B, 14B, 32B, and 72B candidates.
- **FR-013**: Each candidate MUST bind code/source, model/tokenizer/license,
  dtype, full-model revision, stage artifacts/interfaces, OCI/SIF, identities,
  topology, GPU placement, workload, and repetition to one digest-derived ID.
- **FR-014**: Spec 109 candidate, campaign, cell, run, or once-only Slurm IDs
  MUST NOT be reused.
- **FR-015**: Every size MUST have an explicit task that remains open until a
  real NDNSF-DI request reaches all three distinct single-node GPU providers;
  the multi-node 0.5B task is independently keyed and independently closed.
- **FR-016**: Pre-start `BLOCKED`, `DEFERRED`, `NOT_STARTED`, quota, scheduler,
  image, export, or network outcomes MUST NOT satisfy a distributed task.
- **FR-017**: A post-start model load, CUDA, OOM, tensor-interface, dependency,
  timeout, or correctness failure MUST be preserved as an executed negative.
- **FR-018**: GPU proof MUST correlate requested/allocated GRES, node, physical
  GPU UUID/model, container device mapping, CUDA runtime, provider/stage, and
  backend execution evidence; `nvidia-smi` alone is insufficient.
- **FR-019**: CPU fallback MUST fail a GPU cell unless separately preregistered
  as a diagnostic identity that grants no candidate PASS.
- **FR-020**: Correctness MUST use deterministic greedy decoding and retain
  exact prompt/input IDs, oracle output IDs, candidate output IDs, and digests.
- **FR-021**: The 0.5B vertical slice MUST pass 1-, 2-, and 32-token exact-output
  cells before performance or larger candidates start.
- **FR-022**: Each successful size MUST run three original 60-second candidate
  repetitions and three matched staged-baseline repetitions with warmup outside
  the measured window.
- **FR-023**: Single-node NDNSF-DI overhead MUST be computed only against the
  exact single-node staged baseline; multi-node placement overhead MUST compare
  the exact multi-node candidate against the matched single-node NDNSF-DI
  candidate. Full-model Transformers timing MUST NOT be either denominator.
- **FR-024**: Metrics MUST include completion/failure, cold load, TTFT,
  inter-token latency, tokens/s, request throughput, per-stage compute, dependency
  bytes/time, NDN/security/orchestration time, CPU RSS, GPU memory/utilization,
  scheduler wait, and sample counts.
- **FR-025**: p50/p95/p99 MUST require at least 20/100/1000 observations or be
  reported unavailable.
- **FR-026**: Cross-size causal claims MUST use identical hardware, topology,
  stage count, workload, logging, and timeout; other comparisons are descriptive.
- **FR-027**: Every acceptance submission MUST be at most once under its locked
  identity; no crashed, failed, or slow run may be auto-retried or overwritten.
- **FR-028**: A manually authorized replacement MUST use a new identity and
  preserve the original record with a causal link.
- **FR-029**: Every job MUST record lifecycle, allocation, process, NFD, route,
  identity, security, model, stage, dependency, backend, GPU, output, timing,
  resource, terminal, and promotion evidence.
- **FR-030**: Evidence promotion MUST be atomic and checksum-verified before
  scratch cleanup; incomplete promotion cannot PASS.
- **FR-031**: Cleanup MUST default to dry-run and protect active jobs, identities,
  accepted evidence, sealed models, referenced artifacts, and current/prior releases.
- **FR-032**: Results MUST distinguish `substrate`, `standaloneOracle`,
  `distributedCandidate`, and `physicalProduction` authority.
- **FR-033**: Completion MUST require every single-node distributed size task
  and the 0.5B multi-node extension to hold a real executed PASS or an admissible
  post-start FAIL; a matrix of planning blockers is incomplete.
- **FR-034**: The implementation MUST reuse Spec 107/108 mechanisms where valid
  and repair missing capability in their owning runtime/packaging layer rather
  than fork a second scheduler, security path, or generation protocol.
- **FR-035**: The final release MUST retain `physicalProduction=DEFERRED` with
  Spec 106 as owner regardless of iTiger experimental success.
- **FR-036**: The operator workflow MUST separate render/preflight from explicit
  submit and MUST never store VPN, SSH, registry, or model credentials in specs,
  scripts, profiles, logs, or evidence.

### Key Entities

- **Runtime Release**: OCI digest, SIF checksum, dependency locks, compatibility,
  secret scan, and source revision.
- **Allocation Topology**: Slurm request/allocation, nodes, addresses, NFDs,
  faces/routes, identities, provider roles, GPU mappings, and teardown.
- **Distributed Candidate**: Immutable model, stages, interfaces, security,
  topology, placement, workload, and code binding.
- **Generation Session**: Request, token epochs, KV ownership, stage attempts,
  dependency edges, deadlines, and exactly-one terminal response.
- **Execution Boundary**: Durable proof that a secured NDNSF-DI request entered
  GPU stage execution, separating experiment results from operational blockers.
- **Evidence Bundle**: Checksummed scheduler-to-output record with authority and
  immutable original outcome.
- **Scale Matrix**: Seven sizes multiplied by correctness, candidate performance,
  matched baseline, placement, repetition, and terminal status.

## Success Criteria

- **SC-001**: One compute-node probe proves the pinned SIF can run NFD, NDNSF,
  NDNSF-DI, PyTorch, Transformers, ONNX Runtime GPU, and an allocated CUDA device.
- **SC-002**: One five-minute two-node allocation produces admissible evidence
  for the candidate-selected NFD transport (TCP by default), face/route state,
  generic secured invocation, and teardown; unselected transport diagnostics
  are reported but do not block eligibility.
- **SC-003**: The single-node 0.5B candidate executes three distinct provider
  processes on three allocated GPUs and passes exact 1/2/32-token equality.
- **SC-004**: The separately keyed multi-node 0.5B candidate executes across at
  least two nodes, carries one cross-node dependency, and returns exact tokens
  or an admissible post-start negative result.
- **SC-005**: All seven sizes reach real single-node distributed execution and
  produce PASS or an admissible post-start negative; none closes from a
  pre-start block.
- **SC-006**: Every successful size has three original 60-second candidate and
  three matched staged-baseline repetitions with complete sample/resource evidence.
- **SC-007**: Single-node framework overhead and multi-node placement overhead
  use their respective matched baselines and separate compute, dependency,
  security, orchestration, and network time.
- **SC-008**: GPU evidence correlates every stage to allocated UUIDs and shows no
  undeclared CPU fallback.
- **SC-009**: One new-identity 0.5B reproduction matches exact tokens and reports
  metric variation without replacing the original.
- **SC-010**: All accepted bundles pass schema, checksum, identity, lineage,
  secret, terminal-state, and authority mutation tests.
- **SC-011**: No bulk model/SIF exists in workstation or `/home`, and cleanup
  dry-run proposes deletion of zero protected items.
- **SC-012**: No NFD/NDNSF-DI/model daemon survives allocation teardown or runs
  persistently on the login node.
- **SC-013**: Final summary reports every size, placement, and repetition without
  censoring failures and leaves physical-production authority with Spec 106.

## Assumptions and dependencies

- Cisco VPN and an active iTiger account are available to the human operator.
- iTiger continues to provide Slurm and Apptainer; all recorded partition/GRES/
  node/version facts are provisional until live rediscovery.
- Specs 107 and 108 provide reusable source and contracts, but their current
  task completion states do not prove generation-session, GPU release, or
  multi-node candidate readiness.
- Qwen licenses and immutable revisions permit the planned downloads and use.
- Additional `/project` quota may be required before 32B/72B staging/export.

## Out of scope

- Persistent public inference service or bypassing Slurm.
- Docker daemon execution on iTiger compute nodes.
- Real UAV, radio, field-network, production-security, or 24-hour soak approval.
- Treating a single-process, standalone, fixture, CPU fallback, or GPU-visibility
  probe as distributed NDNSF-DI inference; single-node acceptance still requires
  three distinct provider processes and three allocated GPU mappings.
- Silently substituting quantization, another Qwen family, another stage count,
  or a multi-node tensor-parallel engine under the same candidate identity.
