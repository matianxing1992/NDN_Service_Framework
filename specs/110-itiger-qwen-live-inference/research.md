# Research Decisions: NDNSF-DI iTiger Distributed Qwen Execution

## Material Passport

- **Origin Skill**: Academic Research Suite `experiment-agent`, plan mode
- **Created**: 2026-07-13
- **Artifact Version**: `spec110-research-v2-rootless-itiger-build`
- **Verification Status**: `MEASURED` for login-node builder discovery;
  `UNVERIFIED` for compute-node rootless build and GPU runtime

## Decision 1: Build the foundation locally, assemble GPU OCI on GitHub, and execute on iTiger

**Decision**: Build and test the sealed NDN/security/NDNSF foundation locally,
publish that foundation by immutable GHCR digest, and use a dispatch-only GitHub
Buildx job only for the pinned ONNX Runtime/CUDA/Python delta. Materialize the
final digest as SIF in a bounded iTiger CPU allocation and execute through Slurm
using `apptainer exec --nv`. Retain rootless Podman/Buildah on iTiger as a
separately qualified fallback.

**Rationale**: Two GitHub runs failed successively in NDNSD/NFD construction,
and local preflight exposed independent OpenABE/RELIC adapter defects. The stable
C/C++ dependency graph is cheaper to diagnose and cache locally. GitHub still
supplies the useful ephemeral CUDA assembler, GHCR authority, OIDC signing,
SBOM, and provenance. iTiger remains the only valid GPU evidence source.

**Rejected**: rebuilding all sealed C/C++ dependencies in every GitHub run;
installing packages separately on every compute node; running a Docker daemon;
building on the login node or default home graphroot; cloning dependencies inside the Dockerfile;
embedding Qwen weights; uploading the OCI image as an Actions artifact.

**Measured fallback boundary (job 147712)**: login Podman/Buildah was not installed on
the allocated `itiger01` compute environment. The host-tool route is therefore
invalid unless administrators expose it on compute nodes. The next admissible
fallback keeps the same Dockerfile/locks but materializes a pinned builder OCI
as a SIF and tests nested Buildah with VFS/chroot exactly once under a replacement
identity. Official Apptainer guidance confirms that unprivileged definition
builds imply fakeroot but the available modes depend on user namespaces,
subuid/subgid mappings, and host helpers; official Buildah guidance likewise
requires qualifying isolation/user-namespace behavior. Directly rewriting the
runtime as a second Apptainer definition build remains rejected.

## Decision 2: Make the SIF self-complete for user-space software

**Decision**: Package NFD, NDN libraries/tools, NDNSF Core/Python, NDNSF-DI,
Qwen/export tooling, PyTorch, Transformers, ONNX Runtime GPU, and CUDA user-space
libraries. Bind only models, identities, configs, scratch, and evidence.

**Rationale**: Compute nodes are ephemeral and should need no root install.
Digest binding makes each candidate reconstructable.

**Rejected**: relying on login-node packages; mixing unrecorded host libraries;
embedding identities or credentials in the image.

## Decision 3: Establish single-node distributed execution before multi-node

**Decision**: The first candidate runs three distinct GPU stage providers on one
node through one NFD. A separately frozen 0.5B extension then spans at least two
nodes and one cross-node NDN dependency.

**Rationale**: The user requested distributed inference through NDNSF-DI.
Standalone Qwen or multiple stages in one process would not establish that.
The staged order isolates model/runtime/security correctness before adding
inter-node routing and makes one-node/multi-GPU the shortest deployable route.

**Rejected**: standalone inference as completion; `nvidia-smi` as inference;
single-process stage simulation; a generic NFD ping as candidate evidence;
requiring cross-node networking in the first GPU candidate.

## Decision 4: Launch all distributed processes inside one Slurm allocation

**Decision**: Use one bounded allocation and `srun`-managed tasks for NFDs,
Controller, User, and Providers. All processes terminate with the allocation.

**Rationale**: Slurm is the resource and authority envelope. It avoids login-
node daemons and permits deterministic node/GPU/process correlation.

**Rejected**: independent unrelated jobs with races; login-node services;
persistent public endpoints.

## Decision 5: Probe networking before the multi-node candidate

**Decision**: Run one exactly-once, five-minute, two-node CPU probe for addresses,
the selected NFD transport (TCP default), NFD faces/routes, secured generic
invocation, and cleanup. Record UDP diagnostically unless UDP is selected.

**Rationale**: Existing evidence has not measured compute-node inter-node NFD
reachability. A cheap CPU probe prevents consuming GPUs on a broken substrate.

**Rejected**: assuming compute node names are routable; enabling multi-node from
fixture tests; using external public ingress.

## Decision 6: Reuse Spec 107/108 code, not their unfinished status

**Decision**: Complete missing generation-session and GPU/multi-node deployment
capabilities at their existing owners, then bind exact code/release digests into
new Spec 110 identities.

**Rationale**: Current code already has provider roles, dependency I/O, runtime
telemetry, Slurm/Apptainer scaffolding, and identity/evidence contracts. A fork
would create duplicate protocols. Unchecked tasks and blocked evidence still do
not prove capability.

**Rejected**: bypassing predecessor failures; marking Spec 110 complete from a
Spec 107/108 BLOCK; duplicating scheduler/security/generation modules.

## Decision 7: Keep four execution modes and two matched contrasts

**Decision**: Full-model Transformers is the correctness/capacity oracle;
matched local stages are the single-node framework-overhead baseline;
single-node NDNSF-DI is the primary treatment and the baseline for the separately
identified multi-node placement treatment.

**Rationale**: Full-model timing includes different execution structure and
cannot isolate framework overhead. The local staged versus single-node candidate
pair isolates NDNSF-DI. The single-node versus multi-node NDNSF-DI pair isolates
placement/network as far as the frozen resource block permits. A local staged
baseline cannot be topology-matched to a multi-node treatment without inventing
another transport.

**Rejected**: candidate versus full-model latency percentage; fixture tokens;
different stage exports or placements between baseline and candidate.

## Decision 8: Use immutable size-local candidates

**Decision**: Every model size binds model/tokenizer/license, dtype, stage
artifacts/interfaces, image, identity set, topology, placement, workload, and
code. Any quantization, partition, backend, GPU count, or topology change creates
a new candidate.

**Rationale**: Large-model feasibility may force different placements. Explicit
identity prevents accidental apples-to-oranges claims.

## Decision 9: Distinguish pre-start blockers from executed negatives

**Decision**: Only a request that enters real GPU stage execution can yield an
executed result. VPN, quota, export, image, network, queue, or scheduler failure
leaves the experiment incomplete. A later model/CUDA/OOM/dependency/correctness
failure is a preserved negative result.

**Rationale**: This directly repairs Spec 109's completion-semantics defect.

## Decision 10: No automatic retry

**Decision**: Each locked acceptance run submits at most once. Replacement
requires explicit human authorization and a new linked identity.

**Rationale**: Automatic reruns hide failure rates, scheduler effects, and
operational defects.

## Decision 11: Stage storage and capacity deliberately

**Decision**: Bulk assets use project storage. Build/execution jobs select and
record job-unique compute scratch from `SLURM_TMPDIR`, configured Slurm `TmpFS`
(`/scratch` observed), or a validated `/tmp` fallback. Rootless container
graph/run roots must stay on that scratch. Durable OCI/SIF/evidence is promoted
atomically before teardown. Admission includes actual-path capacity, quota
signals, model/export size, SIF, reserve, and partial-copy cleanup. 32B/72B
remain open until real capacity exists.

**Rationale**: Recorded initial project guidance may not accommodate 72B source,
exports, and reserve. Shared `df` is not a user quota measurement.

## Decision 12: Use descriptive statistics unless hardware is matched

**Decision**: Report per-repetition distributions, effect sizes, uncertainty,
sample counts, and failure rates. Restrict size-effect claims to identical GPU,
topology, workload, timeout, logging, and stage count.

**Rationale**: H100/RTX6000/RTX5000 and different GPU counts confound model size.
The experiment is a systems scaling characterization, not a powered population
study; three repetitions measure run variability but do not justify broad causal
generalization.

## Current code and evidence reality

- Current source implements NDNSF-DI provider roles, dependency I/O, runtime
  telemetry, admission/lease concepts, and native model execution primitives.
- Spec 107 has unfinished generation-session and live performance work.
- Spec 108 has an implemented offline Slurm adapter but unfinished GPU OCI,
  candidate inference, security packaging, and multi-node NFD probe work.
- Spec 109 submitted zero GPU inference jobs; model transfer/seal jobs are not
  inference evidence.
- Recorded cluster partitions, GPU GRES, quotas, and versions are mutable and
  must be rediscovered before submission.
