# Implementation Plan: TigerCluster NDNSF-DI Deployment Fidelity

**Branch**: `Experimental` (feature identity `168-itiger-di-deployment-fidelity`) | **Date**: 2026-08-03 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/168-itiger-di-deployment-fidelity/spec.md`

## Summary

Qualify one immutable NDNSF-DI candidate through a deployment-fidelity ladder:
real MiniNDN, the exact Apptainer container, a three-node TigerCluster small-model
request, repeated cold/warm requests, and finally one model that cannot fit on a
single target GPU. The implementation work is driven by lifecycle evidence, not
by repeated remote trial-and-error. It must preserve one request identity, plan
after ACK closure, fetch and verify missing content-addressed shards, distinguish
disk/RAM/GPU residency, execute each role when its own preparation and direct
dependencies are ready, and terminate with an authenticated complete response or
a precise classified failure.

The current all-member `ReadySetCoordinator.activate()` contract is incompatible
with the required data-driven execution semantics because it delays every role
until all selected roles report READY. Spec 168 therefore treats plan commitment
as authorization, local preparation as a per-role prerequisite, and predecessor
data arrival as the execution trigger. Any readiness acknowledgement retained for
security or observability must be per-role and must not become a global start
barrier. This behavior belongs to NDNSF-DI; generic NDNSF collaboration remains
the carrier for Request, ACK closure, committed assignments, operation status,
data publication, and Response.

The migration is explicit: `DATA_DRIVEN_V2` is the default policy bound into the
plan; `LEGACY_READY_SET_V1` remains only for explicitly requested preplanned
compatibility invocations. Providers advertise support in ACKs, mixed policies
and automatic fallback fail closed, and rollback creates a new V1 invocation
rather than mutating a committed V2 request.

## Technical Context

**Language/Version**: C++ runtime with Python bindings; Python package declares Python >=3.8

**Primary Dependencies**: ndn-cxx/NFD, NDNSF Collaboration API, MiniNDN,
NDNSF-DistributedRepo, ONNX dependency-graph metadata, PyTorch/Qwen adapter,
CUDA, Apptainer, Slurm

**Storage**: Content-addressed immutable model-shard store; Provider-local disk,
host-memory, and GPU residency registries; tracked Markdown/JSON/CSV evidence;
TigerCluster node-local scratch

**Testing**: C++ unit/regression tests, pytest contract and integration tests,
real MiniNDN multi-process gates, exact-SIF container preflight, Slurm-based
three-node GPU acceptance

**Target Platform**: Linux; an 8 GiB local MiniNDN host/container and
TigerCluster nodes with one RTX 5000-class GPU assigned to each stage

**Project Type**: C++ framework plus Python SDK, distributed-inference extension,
artifact repository, experiments, and evidence analyzers

**Performance Goals**: This feature does not set a headline throughput target.
It requires zero duplicate model-payload bytes and zero redundant shard loads for
compatible warm requests, phase-attributed latency, and progress rather than
fixed-timeout liveness. Repository goodput remains a Spec 167 claim.

**Constraints**: No foundation rebuild or model re-preparation when immutable
identities match; no shared-filesystem substitution for NDN transfer; no global
readiness barrier, fixed Provider-settle sleep, or per-token collaboration; zero
CPU fallback in GPU acceptance; security enabled; first formal failure remains
immutable. Local Gate B uses a three-role tiny-Qwen fixture with a 6 GiB memory
and 7 GiB memory-plus-swap cgroup limit and must record zero OOM events. Full
Qwen3-0.6B simultaneous residency and every large-model/CUDA capacity run are
TigerCluster work.

**Scale/Scope**: Three Providers/three GPU stages; pinned Qwen3-0.6B control;
five real prompts with one warmup plus five measured invocations each; one pinned
larger Qwen model whose complete weights exceed one target GPU; one complete
large-model response before optional repeated large-model measurement

## TigerCluster operator decision

TigerCluster is an execution backend selected by the agent, not a mandatory
step for every change. Local unit/contract/MiniNDN and exact-container gates run
first. Use TigerCluster only when the registered evidence target needs real GPU,
multi-node capacity, or model/runtime scale that the local cgroup cannot supply;
do not use it for ordinary debugging or unbounded retries. Before any remote
submission freeze the candidate identity and command, run bounded access,
queue/GPU/scratch preflight, allocate a fresh evidence root, and submit at most
once for that identity. Preserve failed jobs and continue local work when the
cluster is unavailable. The canonical policy is
[`docs/tigercluster-execution-policy.md`](../../docs/tigercluster-execution-policy.md).

## Constitution Check

*GATE: Passed for Phase 0 and re-checked after Phase 1 design.*

- **Canonical Dynamic Runtime**: PASS. The plan uses generic collaboration and
  unified service names; it does not introduce generated stubs or split names.
- **Security Is Part Of The Data Path**: PASS. NAC-ABE, permissions, one-time
  tokens, replay checks, signed plans/status, and authenticated responses remain
  active. No debug bypass is admitted.
- **CodeGraph First**: PASS. Current planning, preparation, progress-deadline,
  Provider, ReadySet, and native execution paths were inspected through the live
  CodeGraph index before source-level claims were made.
- **Spec-Driven Changes**: PASS. Spec 168 owns the lifecycle contract, design,
  tasks, regressions, remote admission, and traceability.
- **Right-Scope Verification**: PASS. Real MiniNDN and exact-container gates
  precede TigerCluster; host NFD is not acceptance evidence.
- **Cohesive Tasks**: PASS. Tasks will be organized by behavioral outcomes and
  their acceptance evidence rather than by individual files or commands.
- **Resumability and evidence retention**: PASS. GSD state will identify the
  active phase, and failed formal identities remain immutable.

The global ReadySet barrier is an existing NDNSF-DI design violation, not a
constitution exception. It must be removed from the default execution path or
reduced to non-blocking/per-role evidence before remote qualification.

## Architecture and Ownership

```text
Application
  request(model identity, prompt, generation policy)
      |
Generic NDNSF Collaboration API
  begin_collaboration -> ACK_CLOSED -> commit_plan/Selection
  operation status + authenticated data/Response transport
      |
NDNSF-DI (owner of all inference semantics)
  capability decoding -> graph-aware partition/placement
  -> artifact publication/fetch/residency
  -> per-role preparation
  -> dependency-triggered stage execution
  -> token loop inside one durable invocation
      |
NDNSF-DistributedRepo
  immutable manifest/data publication and verified segmented fetch
```

NDNSF owns generic collaboration, message security, request correlation, and
transport primitives. NDNSF-DI owns model identities, graph-aware splitting,
cache compatibility, GPU residency, stage dependencies, activation semantics,
adapter execution, and inference evidence. DistributedRepo owns artifact naming,
manifest integrity, segmented delivery, and transport progress. A change may
cross an ownership boundary only when a focused regression proves the defect is
in that lower layer.

## Lifecycle Design

1. The User freezes an invocation identity and sends one Request.
2. Providers return side-effect-free ACK capability snapshots containing
   capacity, load, reachability, boot epoch, and compatible disk/RAM/GPU shard
   residency.
3. ACK closure is final. The chosen strategy inspects the immutable model
   dependency graph, generates or reuses compatible partitions, assigns roles,
   validates capacity/dependencies, and commits one signed plan through final
   Selection.
4. Each selected Provider independently verifies the assignment, fetches only
   missing shard content through DistributedRepo, records durable disk/RAM/GPU
   transitions, and publishes monotonic authenticated progress.
5. A role becomes executable when its committed plan is valid, its own model
   state is ready, and every direct predecessor input is present. Stage 0 has no
   predecessor and may start as soon as it is locally ready. No all-role READY
   cover or separate global execution command is required.
6. Stage outputs are request/attempt/plan/role bound. Arrival of a valid output
   wakes only its direct successors. The full prompt enters once; all generated
   tokens are produced within the same distributed invocation.
7. The terminal writer publishes exactly one authenticated complete Response or
   one classified failure. Cleanup releases request-scoped work while reusable
   immutable shards remain cached according to policy.

The application-facing default is
`InferenceApplication.request(model=..., input=..., generation=..., strategy=...)`.
Configuration supplies identity/connectivity/deadlines, not roles or shard paths.
The old deployment-object call is renamed `request_preplanned()` with a bounded,
counted positional compatibility shim and cannot satisfy Spec 168 acceptance.

## Delivery Phases

### Phase 0 - Freeze authority and prior evidence

- Preserve job 181948 as the small-model complete-response control.
- Preserve job 181951 as the large-model segmented-fetch failure; do not relabel
  it as an inference or CUDA failure.
- Preserve Spec 167 repository campaign evidence separately; do not infer Repo
  throughput from total inference-job duration.
- Freeze current SIF, source, model, graph, prompt, route, and schedule identities
  before admitting any new formal run.

### Phase 1 - Close lifecycle contracts

- Define request, plan, artifact-residency, stage, progress, failure, and terminal
  invariants in `contracts/` and `data-model.md`.
- Replace global ReadySet activation semantics in the NDNSF-DI default path with
  per-role local readiness plus dependency-data triggering while preserving plan
  authorization and binding checks.
- Bind an explicit execution-policy version into ACK capabilities and the plan;
  retain V1 only as counted, non-fallback compatibility with fail-closed mixed
  version behavior.
- Define warm reuse as measured compatibility at disk, RAM, and GPU—not merely
  file presence or a strategy prediction.

### Phase 2 - Build deployment-faithful local gates

- Reproduce the same process topology, NDN routes/strategies, security, repository
  fetch, model adapter, request contract, and lifecycle analyzer under real
  MiniNDN.
- On the 8 GiB development host, use one content-addressed tiny-Qwen fixture
  with three roles and two real dynamic dependency edges. Preserve real Repo
  publication/fetch, transformer forward execution, and multi-token generation,
  but cap the container at 6 GiB memory and 7 GiB memory-plus-swap and reject any
  cgroup OOM event. Do not require three simultaneous Qwen3-0.6B CPU residents.
- Keep local large-model coverage non-materializing: validate dependency graphs,
  placement arithmetic, immutable identities, segmented-fetch state machines,
  and failure contracts with metadata or bounded payload fixtures. Full remote
  shards, model loading, CUDA-capacity checks, and any workload without a proven
  peak below the local cgroup limits run only in a TigerCluster Slurm allocation.
- Freeze campaign V3 with separate local-fixture, remote-small, remote-large,
  source-bundle, Gate B manifest, and exact-SIF Gate C result digests. Candidate
  audit must reject every cross-gate substitution and partial package overlay.
- Treat a pre-inference Slurm memory-cgroup OOM as an environmental boundary,
  not a logic retry: retain the failed identity and allow at most one linked
  replacement with a new immutable resource profile, source identity, and
  campaign identity. No second replacement is permitted.
- Run the exact candidate SIF without hidden test-only defaults. A host lacking
  CUDA or Apptainer uses explicit `CPU_LOGIC` for the local lifecycle gate and a
  bounded single-node TigerCluster `CUDA` preflight for the exact SIF; neither
  result is the three-node acceptance campaign.
- Add targeted regressions for request-ID continuity, stalled ranges, progress
  deadlines, cache invalidation/reuse, wrong-attempt data, per-role execution,
  complete multi-token output, and first-writer-wins termination.
- Add a bounded Selection-fanout regression using three realistic opaque
  assignments whose combined compact encoding exceeds 7 KiB. The local
  MiniNDN/exact-container path must prove provider-specific V2 names, one
  authenticated projection accepted per Provider, unchanged request/attempt
  identity, and no second ACK or planning cycle. Keep compact Selection only
  for non-collaboration multi-select calls whose complete protected wire form is
  already bounded.
- Bind the native C++ library and Python extension actually mapped inside the
  container to the candidate identity.  The formal three-node launcher must use
  the same complete native-overlay entrypoint as Gate C and must reject a SIF or
  partial overlay that lacks provider-specific Selection projection, targeted
  Selection prefetch, or independently queryable Selection status.  Source-file
  hashes and a passing CUDA forward alone do not prove this control-plane ABI.
- Before model distribution, run a bounded three-node TigerCluster control-plane
  canary with the exact candidate closure and a tiny collaboration payload.  It
  must prove, for every selected Provider, Selection publication, cross-node
  retrieval, authentication/decryption, local projection acceptance, and status
  response.  This is a delivery admission check, not an all-Provider model-ready
  barrier: role execution remains independently data-driven.
- A candidate that fails any gate cannot enter the three-node campaign.

### Phase 3 - Repair and freeze one candidate

- Classify each observed defect to exactly one primary lifecycle boundary.
- Repair the owning component, add a local regression, rerun all admission gates,
  and freeze a new source identity. Do not rebuild the foundation or model assets
  unless their immutable identity actually changes.

### Phase 4 - TigerCluster small-model qualification

- Reuse the existing qualified SIF, Qwen3-0.6B assets, schedule, and Repo payload.
- First run one real prompt to a complete authenticated multi-token Response.
- In the same unchanged allocation, run five prompts, each with one warmup and
  five measured requests. Retain full answers and per-phase latency, TTFT,
  per-token latency, total latency, tokens/s, cache state, bytes fetched, load
  events, GPU assignment, and failures.
- Compare cold and warm requests only within matching immutable assignments and
  report both distributions; never pool or delete failed rows.

### Phase 5 - Large-model requalification

- Only after Phase 4 passes, reuse the existing larger Qwen assets and the same
  public lifecycle path.
- Run one deterministic prompt. Require all shard fetches, GPU loads, dependency
  transitions, stages, and the complete Response. A failure closes that identity
  and returns to local repair; it is not silently retried.
- Repeated large-model measurement is optional and requires a separate admitted
  schedule after one complete response.

### Phase 6 - Audit and close

- Map every requirement to source, local gate, remote artifact, and owner.
- Distinguish correctness, output equivalence, artifact transport, preparation,
  execution, cache reuse, and environmental evidence.
- Record residual limitations and prohibit claims not supported by retained rows.
- Repeat the accepted small- and large-model single-request correctness profiles
  from a clean allocation using identical material identities before final
  reproducibility claims.

## Project Structure

### Documentation (this feature)

```text
specs/168-itiger-di-deployment-fidelity/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── experiment-plan.md
├── quickstart.md
├── contracts/
│   ├── lifecycle-contract.md
│   ├── cache-residency-contract.md
│   ├── evidence-schema.md
│   ├── failure-taxonomy.md
│   ├── experiment-contract.md
│   └── ownership-boundary.md
├── evidence/
└── tasks.md
```

### Source Code (repository root)

```text
ndn-service-framework/                 # generic collaboration and security carrier
pythonWrapper/ndnsf/                   # public Python collaboration bindings
NDNSF-DistributedInference/
├── ndnsf_distributed_inference/
│   ├── app_sdk/                       # invocation and post-ACK planning strategy
│   ├── core/                          # DI contracts, lifecycle and activation
│   ├── artifact_deployment.py         # fetch, verify and residency transitions
│   └── provider.py                    # selected role preparation/execution
├── cpp/ndnsf-di/                      # native dataflow and GPU-stage execution
└── experiments/                       # deployment-faithful local/remote campaigns
NDNSF-DistributedRepo/                 # immutable artifact transport
tests/python/                           # unit, contract and integration gates
specs/168-itiger-di-deployment-fidelity/evidence/
```

**Structure Decision**: Keep generic collaboration changes narrowly isolated in
the existing NDNSF runtime. Implement inference-specific lifecycle, cache,
activation, and evidence behavior inside `NDNSF-DistributedInference`; change
DistributedRepo only for independently reproduced transport defects. Reuse the
current experiment infrastructure rather than creating a second deployment path.

## Complexity Tracking

No constitution violation is accepted. The local/container/remote validation
ladder is additional process complexity, but each level exercises a distinct
failure boundary and prevents expensive remote debugging from substituting for
reproducible local regressions.
