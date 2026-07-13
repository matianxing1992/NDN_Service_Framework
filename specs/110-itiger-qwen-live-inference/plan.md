# Implementation Plan: NDNSF-DI iTiger Distributed Qwen Execution

**Branch**: `110-itiger-qwen-live-inference` | **Date**: 2026-07-13 | **Spec**: [spec.md](spec.md)

**Input**: Install a complete NDNSF-DI runtime on iTiger and execute real
multi-node, GPU-backed Qwen distributed inference across seven sizes.

## Material Passport

- **Origin Skill**: Academic Research Suite `experiment-agent`, plan mode
- **Created**: 2026-07-13
- **Artifact Version**: `spec110-plan-v2-rootless-itiger-build`
- **Verification Status**: `IN_PROGRESS`; live login-node build facts measured,
  compute-node rootless build and GPU runtime still unverified
- **Human-read sources**: repository source and Specs 107–109 only

## Summary

Build one digest-bound OCI GPU runtime containing the full NDN/NDNSF/NDNSF-DI
and Qwen software stack, convert it to a SIF stored in iTiger project storage,
then launch it only through Slurm. The first vertical slice uses one GPU node,
one unprivileged NFD, three distinct GPU-backed Qwen stage-provider processes,
a ServiceController, and a User. It proves the secured NDNSF request, stage
dependencies, CUDA execution, and exact response without an inter-node
confounder. A separately identified 0.5B extension then spans two or more nodes
and proves a cross-node NDN dependency. The controlled seven-size ladder stays
single-node/multi-GPU; standalone full-model and local staged runs are references,
not substitutes.

## Technical Context

**Languages**: C++17, Python 3, Bash, YAML, JSON
**Primary dependencies**: NFD, ndn-cxx, ndn-svs, NAC-ABE, NDNSF Core, pybind11
bindings, NDNSF-DI, ONNX Runtime GPU, PyTorch, Transformers, CUDA user-space
runtime, Slurm, Apptainer
**Storage**: immutable files/manifests under `/project/$USER/ndnsf-di`; job-unique
scratch selected from `SLURM_TMPDIR`, Slurm `TmpFS` (`/scratch` observed), or a
validated `/tmp` fallback; `/home` small config only
**Testing**: Boost.Test, pytest, shell integration tests, MiniNDN security/network
regressions, offline adapter fixtures, bounded live Slurm probes and experiments
**Target platform**: iTiger `bigTiger` compute nodes selected by live GRES
discovery; current recorded H100/RTX6000/RTX5000 facts are provisional
**Project type**: C++/Python distributed runtime plus OCI/Apptainer packaging,
operator CLI, experiment harness, and evidence schemas
**Performance protocol**: deterministic correctness first; then three original
60-second candidate repetitions and three matched staged repetitions per passing
size; warmup outside measurement
**Constraints**: no Docker daemon or persistent processes on iTiger; rootless
builds only in bounded CPU allocations with explicit scratch graph/run roots;
no local bulk models; no automatic rerun; no CPU fallback under GPU identity;
no physical production authority
**Scale**: seven model sizes on one-node/three-GPU placements, one separately
identified multi-node 0.5B extension, three correctness token lengths, and six
performance repetitions per passing size

## Constitution Check

| Gate | Design response | Status |
|---|---|---|
| Canonical dynamic runtime | Uses unified `/LLM/Qwen2.5/Generate` service and current collaboration APIs | PASS |
| Security in data path | Preserves permission, NAC-ABE, tokens, replay, provider permission, lease and digest checks | PASS |
| CodeGraph first | Current provider/runtime/deployment source inspected before planning | PASS |
| Spec-driven durable work | Spec, plan, contracts, tasks, traceability, and audit are required | PASS |
| Right-scope validation | MiniNDN validates code/security; iTiger is explicitly required for cluster/network/GPU claims | PASS |
| GSD resumability | Phase/task state and candidate evidence are durable and independently recoverable | PASS |

Post-design recheck: no new wire protocol or security bypass is required. The
new work composes existing NDNSF collaboration, provider-role, dependency, and
deployment primitives. Physical authority stays outside this feature.

## Research Questions and hypotheses

- **RQ1**: Can the current NDNSF-DI path execute a secured Qwen generation
  session across three provider processes/GPUs on one iTiger node with exact
  tokens, then preserve correctness when one dependency crosses nodes?
- **RQ2**: How do completion, latency, throughput, stage compute, dependency
  transfer, and orchestration cost change with Qwen2.5-Instruct size?
- **H1 (falsifiable)**: The 0.5B three-stage candidate returns exactly the same
  1/2/32 greedy token IDs as the pinned full-model oracle.
- **H2 (descriptive)**: Single-node framework overhead can be decomposed against
  the matched local staged baseline; cross-node placement overhead can be
  decomposed against the matched single-node NDNSF-DI candidate.
- **H3 (controlled subset)**: On identical RTX5000 topology and workload, model
  size changes at least one performance metric; effect sizes and uncertainty are
  reported without assuming monotonicity.

## Variables and controls

| Class | Variables |
|---|---|
| Independent | model size; execution mode; placement class (single-node/multi-node) |
| Primary dependent | exact token correctness; completion/failure; TTFT; tokens/s; request throughput |
| Diagnostic dependent | stage compute; dependency bytes/time; NDN/security/orchestration; GPU/CPU resources |
| Controlled | prompt IDs, tokenizer, greedy decoding, max tokens, batch/concurrency, stage count/interfaces, timeout, logging, warmup, measurement duration |
| Confounders | GPU class/count, queueing, node placement, driver/runtime version, cache state, shared filesystem contention, NFD route/RTT |

The controlled size-effect subset uses one identical single-node GPU class and
topology for all sizes that fit. Results using different GPU classes/counts are
descriptive. Cross-node placement is analyzed as a separate factor and never
pooled into the size-effect subset.

## Architecture and ownership

| Concern | Owner | Spec 110 action |
|---|---|---|
| Scheduler/allocation | Slurm | Discover, render, submit once, observe, terminate |
| Container lifecycle | Apptainer adapter | Materialize SIF, bind, `--nv`, run, collect |
| OCI content/build | Spec 108 packaging + Slurm/Apptainer adapter | Keep one Dockerfile/lock graph; build rootlessly in compute scratch and promote by digest |
| Inter-node forwarding | one job-scoped NFD per node | Explicit addresses, faces, routes, teardown |
| Permission/security | NDNSF Core and ServiceController | Reuse unchanged mechanisms; run negative tests |
| Model orchestration | NDNSF-DI | Finish generation session, stage placement, dependency I/O |
| Model execution | Native ONNX Runtime CUDA runner | Load sealed stage and emit correlated execution evidence |
| Experimental identity | Spec 110 harness | Digest-bound candidate/cell/run ledger |
| Physical deployment | Spec 106 | Always DEFERRED here |

### Runtime release

The OCI build source is the single dependency source of truth. The primary
delivery path makes every locked dependency commit reachable through its public
repository, creates checksum-bound Git archives in GitHub Actions, verifies the
shared source seal, and builds the existing GPU Dockerfile with
`DEPENDENCY_SOURCE_MODE=sealed`. Buildx pushes the immutable OCI result directly
to GHCR; Actions retains only evidence, SBOM, signatures, checksums, and logs.
Qwen weights never enter the build context or image. A bounded iTiger CPU
allocation pulls the exact GHCR digest and materializes the runtime SIF under
`/project` before the GPU probe.

Measured boundary: exact-once diagnostic job `147712` started on `itiger01` and
failed because host Podman was absent; Buildah was therefore not attempted. It
selected `/tmp` rather than the configured `TmpFS=/scratch`. This is an executed
negative, not a pre-start blocker. It does not block the GitHub primary route.

Rootless Podman/Buildah on iTiger remains a fallback that consumes the same
Dockerfile, lock, and seal. It may use administrator-provided commands or the
separately pinned builder SIF, with all graph/run/cache/temp paths in compute
scratch. Neither route needs a Docker daemon or NVIDIA Container Toolkit on
iTiger.

The SIF contains:

1. NFD/ndn-cxx tools and configuration templates;
2. ndn-svs/NAC-ABE and NDNSF C++ libraries;
3. NDNSF Python extension and NDNSF-DI package;
4. PyTorch/Transformers/tokenizer dependencies for the oracle/exporter;
5. ONNX Runtime GPU/CUDA user-space dependencies for the candidate;
6. provider/user/controller launchers and evidence collectors.

Secrets and private identities are never image layers. They are generated or
provisioned separately and bound read-only at job runtime.

### iTiger storage layout

```text
/project/$USER/ndnsf-di/
├── source/<source-id>/
├── releases/<release-id>/{manifest.json,runtime.sif,runtime.sif.sha256}
├── models/qwen2.5/<size>/<revision>/
├── artifacts/<candidate-id>/<stage-id>/
├── identities/<identity-set-id>/
├── campaigns/<campaign-id>/
└── evidence/<run-id>/

<selected-compute-scratch>/ndnsf-di/$SLURM_JOB_ID/
├── container/{graphroot,runroot,cache,tmp}/
├── nfd/<node>/
├── work/<role>/
├── logs/
└── evidence-staging/
```

### Allocation topology

The process topology is data, not shell-script convention. A frozen placement
map binds every process to a Slurm task, node rank, GPU rank, identity, NFD
socket, role, command, readiness dependency, and shutdown order. A node
supervisor starts exactly one NFD per unique node; all containers on that node
bind the same job-local NFD socket/state directory. It then launches Controller,
User, and distinct Provider processes through `srun`, waits on bounded readiness
barriers, and terminates the entire process group on exit or signal.

Primary controlled topology:

```text
node A: NFD-A + ServiceController + User
        + Stage-0 Provider (GPU-0, distinct PID/identity)
        + Stage-1 Provider (GPU-1, distinct PID/identity)
        + Stage-2 Provider (GPU-2, distinct PID/identity)
```

Multi-node extension:

```text
node A: NFD-A + ServiceController + User + Stage-0 Provider (GPU-A)
                      | selected NFD transport and explicit route
node B: NFD-B + Stage-1 Provider (GPU-B) + Stage-2 Provider (GPU-C)
                      | at least one cross-node dependency Data object
```

The default selected inter-node transport is TCP. UDP is measured and recorded
but blocks only a candidate that explicitly selects UDP. A three-node extension
requires another identity; it is not silently substituted. No public ingress or
campus firewall change is required.

### Qwen execution modes

1. **Full-model oracle**: deterministic PyTorch/Transformers output and capacity.
2. **Matched single-node staged baseline**: exact exports/GPU mapping, with a
   local direct orchestrator and no NDNSF networking/security/selection.
3. **Single-node distributed candidate**: same stages/mapping under NDNSF-DI.
4. **Multi-node distributed extension**: same 0.5B candidate bindings except the
   frozen placement/NFD route; its matched reference is mode 3, not mode 2.

The first candidate uses the existing three-stage Qwen path and ONNX Runtime
CUDA. If the current exporter/runner cannot produce or consume a size, the fix
belongs in the existing exporter/native-runner layer. A different backend,
quantization, tensor parallelism, or stage count is a new candidate identity.

### Initial placement hypotheses

These are planning hypotheses, not current cluster facts:

| Size | First oracle placement | Controlled distributed stage placement |
|---|---|---|
| 0.5B, 1.5B, 3B | 1 RTX5000 | 1 node × 3 RTX5000 GPUs |
| 7B | 1 RTX5000 if admitted | 1 node × 3 RTX5000 GPUs |
| 14B | 1 RTX6000 or H100 | 1 node × 3 RTX6000/H100 GPUs |
| 32B | 1 H100 if admitted | 1 node × 3 H100 GPUs |
| 72B | multi-H100 oracle | 1 node × 3–4 H100 GPUs after measured stage-fit admission |

Live model memory, KV, export, SIF, quota, and GRES discovery controls admission.
No task closes from these estimates.

## Execution and completion state

```text
PLANNED
  -> PREFLIGHT_BLOCKED
  -> READY_TO_SUBMIT
  -> SUBMITTED_NOT_STARTED
  -> CANDIDATE_EXECUTION_STARTED
  -> EXECUTED_PASS | EXECUTED_FAIL | EVIDENCE_INCOMPLETE
```

Only `EXECUTED_PASS` and an admissible `EXECUTED_FAIL` after the candidate-bound
execution boundary can close a live cell. Evidence records `failureBoundary`
(`stage-load`, `stage-execution`, `dependency-transfer`, `terminal-response`)
and placement class, so a first-stage crash cannot be presented as a complete
multi-node dataflow. Pre-start states create remediation and remain open. No
automatic Slurm resubmission is allowed; bounded provider replacement inside
one frozen generation session remains governed by the Spec 107 attempt/lease
contract and is not a job retry.

## Experiment sequence

1. Seal source, runtime requirements, workload, and new Spec 110 identity rules.
2. Finish/verify the Spec 107 generation-session capability and the Spec 108
   GPU OCI/Apptainer capability in their owning source paths.
3. Run the compute-node rootless diagnostic, then build/scan/promote the OCI and
   materialize/verify the SIF in bounded iTiger allocations.
4. Run one compute-node runtime/GPU probe.
5. Stage/seal 0.5B; run full-model oracle and stage interface validation.
6. Execute the single-node/three-GPU 0.5B 1/2/32-token cells.
7. Run exactly one bounded two-node CPU/NFD/security probe, then execute the
   separately keyed multi-node 0.5B 32-token extension.
8. Execute three 60-second single-node candidate and matched staged repetitions;
   measure the multi-node placement delta separately.
9. Advance 1.5B, 3B, 7B, and 14B on single-node placements.
10. Obtain quota/stage-fit capacity, then attempt single-node 32B and 72B.
11. Reproduce one 0.5B single-node cell under a new identity.
12. Validate, aggregate, audit, clean up safely, and hand only experimental
    evidence to Spec 106.

## Validation strategy

- **Offline**: schemas, identity, lifecycle, rendering, injection, storage,
  GPU mapping, evidence mutation, cleanup, and no-retry tests.
- **Local/MiniNDN**: generation-session, stage dependency, exact-final-once,
  security, replay, lease, digest, failure, and packaged regressions.
- **iTiger substrate**: SIF imports/linking, `--nv`, compute scratch, NFD start.
- **iTiger network**: bounded selected-transport/NFD/service probe; UDP is
  diagnostic unless selected.
- **iTiger candidate**: single-node/three-GPU Qwen first; multi-node 0.5B second.
- **Performance**: matched 60-second repetitions, raw samples, uncertainty,
  negative results, and no post-hoc topology substitution.

## Project Structure

### Documentation

```text
specs/110-itiger-qwen-live-inference/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── traceability.md
├── contracts/
├── checklists/
└── tasks.md
```

### Source and tests

```text
NDNSF-DistributedInference/
├── cpp/ndnsf-di/                    # generation session, dependency I/O, runner
└── ndnsf_distributed_inference/     # orchestration, provider, evidence
packaging/ndnsf-di-container/
├── oci/                             # one OCI source and GPU locks
└── adapters/slurm-apptainer/        # iTiger lifecycle/topology scripts
tools/ndnsf-di/
├── spec110_candidate.py
├── spec110_artifacts.py
└── ndnsf-di-itiger-qwen
tests/container/itiger-qwen-live/
├── unit/
├── contract/
├── integration/
└── fixtures/
results/spec110-itiger-qwen-live/     # ignored local evidence mirror
```

**Structure decision**: Extend the existing runtime and packaging owners. Spec
110 adds only experiment identity/orchestration/evidence glue; it does not fork
NDNSF security, generation, NFD routing, or Slurm adapters.

## Complexity Tracking

| Mechanism | Why needed | Simpler alternative rejected because |
|---|---|---|
| One NFD per unique allocation node | Prevents duplicate listeners/state and proves selected inter-node transport | One NFD per container conflicts on same-node placements |
| Two matched contrasts | Separates framework overhead from network placement overhead | One local staged baseline cannot match a multi-node topology |
| Digest-bound state/evidence | Prevents blocked or mutated runs becoming PASS | Human task checkboxes failed in Spec 109 |
