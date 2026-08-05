# Implementation Plan: Reusable Canonical Layers and Adaptive Device Placement

**Branch**: `Experimental` (feature identity `170-reusable-layer-artifacts`) | **Date**: 2026-08-04 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/170-reusable-layer-artifacts/spec.md`

**Status**: Planning complete; this document does not authorize implementation
or a TigerCluster submission until tasks and the preceding gates exist.

## Summary

Replace request-specific, role-scoped model packages with reusable canonical
model-layer artifacts, then let each selected Provider assemble and retain the
exact fragment required by a sealed post-ACK placement plan. Introduce a
versioned NDNSF-DI placement contract that treats a Provider as a security and
local-scheduling domain containing zero, one, or multiple compute devices.
Signed ACK offers describe the actual container-visible topology and mutable
capacity; `LayerReuseFirstStrategy` selects CPU, single-device, or device-set
bindings without reserving resources during ACK collection.

Hybrid inference uses a heterogeneous topology `N x {M_i}`, not a compulsory
rectangular `N x M`: the model has `N` pipeline stages and every stage `i`
independently chooses tensor degree `M_i >= 1`. `M_i = 1` leaves that stage
unsplit. Only stages whose requirements justify horizontal partitioning receive
multiple ranks, and every transition whose adjacent degree or declared tensor
layout changes has an adapter-certified gather, scatter, or reshard contract.

Implementation proceeds through four behavioral phases:

1. canonical layer artifacts plus CPU/no-GPU and one-GPU compatibility;
2. one Provider executing multiple independent single-GPU roles;
3. one logical role spanning multiple GPUs and heterogeneous hybrid execution;
4. deployment-faithful MiniNDN/exact-SIF gates followed by TigerCluster 0/1/2
   GPU qualification.

Each phase is blocked by complete-output, identity, admission, security, and
failure-injection evidence. V2 role-scoped pre-split assignments remain an
explicit single-device compatibility path and are never silently interpreted as
the new contract.

## Technical Context

**Language/Version**: Python >=3.8 for the NDNSF-DI SDK/planner/adapters and C++
for native Provider/resource/dataflow execution

**Primary Dependencies**: NDNSF Collaboration API, ndn-cxx/NFD, MiniNDN,
NDNSF-DistributedRepo, ONNX graph metadata, PyTorch 2.6, CUDA 12.4, NCCL 2.21,
Qwen/transformers adapters, Apptainer, and Slurm

**Storage**: Content-addressed canonical model/layer manifests and objects in
NDNSF-DistributedRepo; Provider-local assembled fragments on disk/RAM; exact
loaded-runtime identities bound to a process, device set, and topology epoch

**Testing**: pytest unit/contract/integration tests, C++ unit/regression tests,
real MiniNDN multi-process gates, exact OCI/SIF parity, and bounded Slurm jobs
for TigerCluster 0/1/2-GPU visibility and inference qualification

**Target Platform**: Linux; local CPU/MiniNDN within the 8-GiB development-host
budget, exact Apptainer SIFs, and TigerCluster Slurm allocations with zero, one,
or two GPUs visible to one Provider process

**Project Type**: C++ framework plus Python SDK, distributed-inference extension,
model-family adapters, immutable artifact repository, container/deployment
tooling, and experiment/evidence analyzers

**Performance Goals**: Correctness gates take precedence over headline
throughput. An exact loaded-runtime warm hit transfers zero model bytes, performs
zero fragment assembly, and performs zero model reload. Equal canonical layers
are published once across placement changes. Cold/warm distributions retain
TTFT, total latency, per-token latency, tokens/s, transfer bytes, assembly/load
events, and per-device utilization.

**Constraints**: NDNSF Core remains model-neutral; all model, topology, rank,
collective, and redistribution semantics stay in NDNSF-DI. ACKs do not reserve
GPU resources. Device-set admission is atomic. CPU fallback is explicit, never
silent. Provider configuration can only restrict the scheduler/container-visible
device set. No global model-ready barrier or second start command is allowed.
No phase assumes a uniform tensor degree, and no TigerCluster model campaign is
admitted before local and exact-container gates pass.

**Scale/Scope**: Canonical artifacts for a minimal real Qwen model; zero/one/two
visible GPUs; one Provider with multiple local device bindings; pipeline-only,
tensor-only, and heterogeneous hybrid degree vectors including `[1,2,1]` and
`[2,1,2]`; repeated cold/warm requests and concurrent admission; large-model
qualification remains outside the first Spec 170 closure.

## Constitution Check

*GATE: Passed before Phase 0 research and re-checked after Phase 1 design.*

- **Canonical Dynamic Runtime**: PASS. The application continues to use the
  generic collaboration path and one unified service name. NDNSF Core carries
  opaque NDNSF-DI assignments rather than gaining model-specific messages.
- **Security Is Part Of The Data Path**: PASS. Offers, manifests, Selections,
  dependencies, collective epochs, progress, and Responses remain identity-
  bound and authenticated. No authorization, token, or replay bypass is planned.
- **CodeGraph First**: PASS. Current V2 offers, assignment validation, GPU
  admission, role graph, Provider preparation, residency, and TigerCluster
  launch surfaces were mapped through the live index and verified against source.
- **Spec-Driven Changes**: PASS. Versioning, migration, data models, contracts,
  validation, and rollback are owned by Spec 170 before implementation.
- **Right-Scope Verification**: PASS. Contract tests and real MiniNDN precede
  exact-SIF gates; TigerCluster is used only for CUDA/device-topology evidence
  that the local 8-GiB host cannot supply.
- **Cohesive Tasks**: PASS. Each future task must close one behavioral slice with
  its tests and evidence, not split test/implementation/run/documentation into
  mechanical task chains.
- **Resumability and evidence retention**: PASS. Each phase has an immutable exit
  gate and may resume from its last verified contract/evidence identity. Failed
  formal runs remain classified evidence rather than being overwritten.

No constitution exception is accepted. The additional V3 contract is justified
as a migration boundary because mutating V2 would make retained single-device
evidence and cache equality ambiguous.

## Architecture and Ownership

```text
Application
  request(model identity, task, input, options, deadline)
      |
Generic NDNSF Collaboration
  Request -> signed ACKs -> ACK_CLOSED -> opaque final Selection
  authenticated dependency/status/Response transport
      |
NDNSF-DI planning plane
  graph + canonical catalog + Provider offers
  -> PipelineStageSpec[N] with independent tensor_degree M_i
  -> LogicalRole + RankAssignment + ProviderLocalRoleBundle + RoleAssemblySpec
  -> CPU / SINGLE_DEVICE / DEVICE_SET bindings
      |
NDNSF-DI Provider plane
  verify -> bounded queue acceptance without device hold
  -> fetch missing canonical objects -> assemble host-side
  -> just-in-time atomic device admission -> load -> local-ready
  -> dependency/collective-driven execute
      |
NDNSF-DistributedRepo
  immutable manifest/object publication and verified segmented/range fetch
```

Ownership is strict:

- **Slurm/container runtime** allocates and exposes devices.
- **Provider resource probe** observes the actual runtime-visible CPU, memory,
  storage, accelerators, connectivity, health, and allocation sequence.
- **NDNSF-DI placement strategy** is operator-installed Requester code and sees
  only validated, sanitized planning views. It proposes Providers, pipeline
  boundaries, per-stage tensor degrees, ranks, devices, collective groups, and
  redistribution edges from the immutable ACK_CLOSED snapshot; its output is
  declarative and untrusted.
- **NDNSF-DI plan sealer** independently resolves, canonicalizes, and validates
  that proposal against certified catalogs and offers, then alone creates the
  sealed plan and Provider-specific Selection projections.
- **Provider-local scheduler** admits, queues, executes, or rejects the sealed
  binding; it cannot change global cuts, ranks, device-set membership, or
  collective semantics.
- **Model adapters** certify legal layer/tensor partitions, layouts, collective
  operators, and cross-stage degree/layout transitions.
- **NDNSF-DistributedRepo** transports content but does not choose placement or
  execute model code.

### Current-code integration targets

Spec 170 is a migration of the real default path, not a parallel prototype:

| Current source fact | Required V3 change | Closing task |
|---|---|---|
| `app_sdk/application.py` defaults to `PreSplitFirstStrategy` | Default normal calls to `LayerReuseFirstStrategy`; require explicit V2 profile for PreSplitFirst | T010 |
| `app_sdk/client.py` constructs the placement coordinator/materializer/publisher path | Dispatch V3 canonical ensure versus explicit V2 role-split preparation | T010 |
| `app_sdk/placement.py::_prepare_artifacts()` materializes and publishes the selected role split | V3 bypasses role-split materialization, seals `RoleAssemblySpec`, and publishes only canonical layers; V2 retains old path | T010 |
| `provider.py::attach_negotiated_reservation()` can create ACK-time reservation leases | Branch on placement profile: V3 signed offer is side-effect-free; only explicit V2 retains compatibility behavior | T005 |
| `NativeProviderReadinessState::makeAckDecision` and `ProviderResourceProbe` feed the native ACK | Serialize the same V3 offer/no-reservation fields as Python and wire them into executable/build/install/SIF paths | T003 |
| Provider handlers consume role artifacts prepared by the Requester | Python/native handlers consume `ProviderSelectionProjectionV3`, assemble canonical layers locally, and use queue/JIT admission | T009-T011 |

Generated `build/lib` and packaging build-copy trees are never edited directly.
The owning source, build declaration, package manifest, and rebuild/check command
are named in the task that closes each row.

## Versioning and Migration

The new contract is a versioned NDNSF-DI placement profile, conceptually
`DI_PLACEMENT_V3`:

- V3 offers carry a `DeviceTopologyProfile`, a `DeviceResourceSnapshot`, bounded
  exact residency evidence, role-capability predicates, and an explicit
  `ACCEPT_IF_EXACT_REUSE | ACCEPT_WITH_PREPARATION | REJECT` disposition.
- V3 plans separate `LogicalRole` from `RankAssignment[]`, carry canonical model
  and assembly references, and bind every rank to an exact device/resource
  envelope.
- V2 remains an explicit supported profile named
  `PREASSEMBLED_PARTITION_SINGLE_DEVICE`. It does not gain device-set or
  heterogeneous-rank meaning through optional fields and is never an automatic
  V3 fallback. Its future removal requires a separate breaking-change spec.
- A V2 Provider cannot be selected into a V3 device-set or hybrid collective.
  Mixed-version plans fail before Selection.
- Rollback starts a new attempt using an explicitly supported older profile; it
  never mutates a committed V3 plan or reuses a V3 loaded-runtime identity as V2.

## ACK, Preparation, and Reuse Semantics

The generic ACK status and willingness to perform new model preparation are
different facts. NDNSF-DI preserves the generic Collaboration rule that a
negative ACK is not selectable:

| V3 disposition | Generic ACK status | New preparation | Selection condition |
|---|---:|---:|---|
| `ACCEPT_IF_EXACT_REUSE` | true | no | Sealer proves the proposed role/rank exactly matches verified assembled or loaded residency |
| `ACCEPT_WITH_PREPARATION` | true | yes | Exact reuse, canonical-layer reuse, or cold fetch/assembly is feasible |
| `REJECT` | false | no | Never selectable |

This avoids interpreting `ack=false` as both refusal and hidden acceptance.
ACK creation remains side-effect-free in every V3 mode. Only final Selection
may create a bounded queue/preparation record; even then, it reserves at most
declared disk/RAM/network preparation capacity, never GPU/device capacity. An
exact-reuse Selection skips fetch/assembly. A preparation Selection is invalid
unless its offer explicitly allowed preparation. Device execution resources are
re-probed and atomically admitted just in time.

Provider reuse has three distinct lifetimes:

1. canonical layer files remain in the bounded Provider disk cache and are
   reusable across legal placements;
2. the ONNX baseline stores each completed role/rank assembly as one immutable
   content-addressed `.ndnsf-onnx-artifact` bundle with embedded signed manifest;
   it contains inline `model.onnx` for bounded models or `model.onnx` plus one
   external-data entry for large models, and is cataloged by a meaningful NDN
   identity binding model/profile/graph/role/rank/recipe/object digest;
3. an eligible loaded runtime may remain in RAM/GPU as an evictable cache entry,
   but it does not reserve an execution slot for a request.

Container-private scratch is deleted at exit. Cross-container persistence is
possible only through an explicit bounded mounted cache with operator-defined
ownership, quota, protection, and garbage collection; it is not confused with
DistributedRepo durability.

## Non-Circular Protected-Plan Sealing

`PlanSealerV3.sealCore` first produces immutable canonical plan-core bytes and
`planCoreDigest` without any grant references. Every selected protected
Provider's `KeyGrantV1` binds that core digest. After complete grant acquisition,
`finalizeSecurity` sorts the non-secret Provider grant name/digest bindings and
derives final `planDigest` from the core bytes, grant bindings, and security-
policy snapshot digest. Only this finalized plan is projected into Selection.
This makes authorization cover complete and substitution-evident without the
circular requirement that a grant bind a digest that itself contains the grant.

## Heterogeneous Hybrid Topology

For pipeline stages `S = [S_0, ..., S_(N-1)]`, the plan carries:

```text
tensorDegrees = [M_0, ..., M_(N-1)]
M_i >= 1
M_i = 1  -> one ordinary unsplit stage role
M_i > 1  -> one logical stage role implemented by M_i tensor ranks
```

The strategy may therefore select `[1,2,1]`, `[2,1,2]`, or another adapter-
certified vector; it is not required to split every stage. Inside a stage with
`M_i > 1`, every required tensor has an explicit `SHARDED`, `REPLICATED`,
`OWNER_ONLY`, or `LOCAL_DERIVED` rule; only tensors marked `SHARDED` are cut
across ranks. Required collectives are explicit. Across a boundary:

- `1 -> k` requires a certified scatter/broadcast/fan-out mapping;
- `k -> 1` requires a certified gather/reduce/merge mapping;
- `k -> l` requires a certified reshard mapping when degree/layout changes or
  is infeasible;
- an equal-degree boundary, including `1 -> 1`, is a normal authenticated
  pipeline dependency only when its source/target layouts are compatible.

No redistribution operation is inferred from rank count alone. Each transition
binds tensor layout, producer/consumer ranks, operation ordering, epoch,
integrity, timeout/cancellation, and complete-output semantics.

## Delivery Phases

### Phase 1 - Canonical artifacts plus CPU/single-GPU compatibility

**Behavioral outcome**: The same public invocation and Provider artifact work
with no visible GPU or exactly one visible GPU, while model bytes are published
once as canonical layers and selected Providers assemble their own fragments.

- Introduce V3 canonical model/profile/layer manifests, normalized tensor-map
  identity, idempotent root-last publication, and exact inventory proofs on top
  of the existing DistributedRepo public artifact API.
- Introduce CPU and single-device topology/profile/snapshot contracts. Remove the
  assumption that a valid inference-capable Provider must advertise positive GPU
  memory, while requiring explicit accelerator policy for each task/role.
- Implement `LayerReuseFirstStrategy` over immutable graph, ACK offers, canonical
  layer inventory, assembled-fragment residency, and loaded-runtime residency.
- Replace the normal Application/APPClient coordinator wiring so the above
  strategy and canonical ensure path are the actual V3 default; retain the
  Requester-side selected-role materializer only in the explicit V2 branch.
- Carry one `DeviceBinding` in CPU or `SINGLE_DEVICE` mode and keep per-phase
  resource envelopes even when only one device exists.
- Make Provider preparation fetch only missing verified canonical objects,
  single-flight equal fetch/build work, assemble atomically, load, retain exact
  reusable state, and start when local preparation plus direct inputs are ready.
- Preserve explicit V2 preassembled/single-device compatibility and fail closed
  on cross-version cache or assignment equality.

**Exit gate**:

- unit/contract tests reject fake GPU capacity, unresolved explicit devices,
  invalid manifests, stale snapshots, silent CPU fallback, and aggregate-memory
  substitutions;
- real MiniNDN completes the normal default path with three Provider processes,
  three concurrent invocations, one CPU-allowed minimal-model request, and one
  simulated single-device request with security enabled and one request ID per
  invocation throughout;
- changing pipeline boundaries reuses canonical layer identities, and an exact
  warm loaded-runtime request transfers/assembles/reloads zero model bytes;
- existing V2 single-device regressions remain green under their explicit
  compatibility profile.

### Phase 2 - One Provider, multiple independent single-GPU roles

**Behavioral outcome**: A Provider with several visible devices acts as one
security/service endpoint and local scheduling domain while independently
running multiple roles or requests on distinct GPUs.

- Enumerate each device and interconnect/failure domain separately; never replace
  topology with GPU count plus summed memory.
- Extend placement output so multiple assignments may select the same Provider
  as distinct `ProviderLocalRoleBundle` objects with disjoint or capacity-safe
  `SINGLE_DEVICE` bindings.
- Replace Provider-level aggregate admission with per-device resource ledgers and
  an atomic local assignment transaction; ACK remains side-effect-free.
- Key GPU-loaded runtime residency and model caches by exact device identity,
  boot/process/runtime generation, topology digest, assembly identity, and
  reusable-state contract so GPU 0 and GPU 1 do not overwrite each other.
- Add bounded queueing, cancellation, eviction, and concurrency evidence without
  holding a global GPU lock during Repo fetch or assembly.

**Exit gate**:

- a two-device topology concurrently executes two independent roles and three
  concurrent requests without overlapping device budgets or ACK-time holds;
- `2 x 12 GiB` is rejected for one unsplittable 20-GiB role but accepted for two
  independent roles that each fit one device;
- device loss, renumbering, sharing/failure-domain mismatch, and stale residency
  invalidate only the exact affected bindings and never silently remap them;
- single-flight evidence proves equal object fetches/builds occur once per
  Provider while distinct loaded runtime instances remain per device.

### Phase 3 - Multi-GPU logical roles and heterogeneous hybrid execution

**Behavioral outcome**: One logical role may span a device set, and only the
pipeline stages that need tensor parallelism are horizontally split.

#### Phase 3A - One Provider-local logical role across multiple GPUs

- Separate `LogicalRole` from `RankAssignment[]`; remove validators that require
  every logical role to appear exactly once as a single-device assignment.
- First bind every rank to one `ProviderLocalRoleBundle` and one ordered local
  `DEVICE_SET`; device handles remain scoped to that Provider's signed offer.
- Add adapter-certified tensor partition recipes, per-rank assembly selectors,
  ordered device sets, collective group/member/rank/epoch identities, and
  Provider-local authenticated rendezvous.
- Treat `M_i` as the participant count, not a command to split every tensor;
  adapter recipes classify each tensor as sharded, replicated, owner-only, or
  locally derived and prove complete non-conflicting coverage.
- Admit/enqueue all local members of a `DEVICE_SET` atomically. Never hold one
  device while waiting for another; rank loss aborts the complete affected
  collective epoch.

**Phase 3A exit gate**:

- one Provider with two visible GPUs executes one certified logical role with
  two complete local ranks and matches the corresponding unsplit stage oracle;
- duplicate/missing/orphan ranks, illegal tensor coverage, partial admission,
  collective reordering, stale group epoch, and rank loss are rejected or fail
  the complete group deterministically;
- an unsupported device/link/collective topology is rejected before Selection.

#### Phase 3B - One logical tensor group across multiple Providers

- Partition the global `RankAssignment[]` into one
  `ProviderLocalRoleBundle` per selected Provider; never construct one global
  `DeviceBinding` from handles belonging to different offers.
- Send each Provider one authenticated Selection projection. Each Provider
  revalidates and atomically admits only its local resource vector; ACK
  collection still reserves nothing.
- Bind peer identity, transport endpoint, request/attempt/plan/group/epoch,
  tensor layout, communicator compatibility, and timeout/cancellation into the
  authenticated cross-Provider rendezvous contract.
- Activate the tensor group when all of that group's ranks and its direct input
  are ready. This group-local readiness is required by the collective but is
  not a global model-ready barrier and does not block unrelated stages.
- Propagate missing/rejected/stale/lost ranks to the complete affected epoch and
  replan generation; never silently replace a member in a live group.

**Phase 3B exit gate**:

- one certified two-rank logical role spanning two Providers produces an output
  matching the same unsplit oracle and records both Provider-local bundles;
- wrong peer/offer/epoch, incomplete membership, partial Provider admission,
  rendezvous replay, transport loss, and member failure terminate the exact
  group without a partial downstream output;
- the same role executed Provider-locally and across Providers has equivalent
  numerical semantics but distinct, correctly bound transport/admission traces.

#### Phase 3C - Heterogeneous pipeline/tensor hybrid

- Represent `PipelineStageSpec.tensor_degree` independently for every stage and
  validate complete graph/tensor coverage for heterogeneous vectors.
- Implement explicit adapter-certified `1->k`, `k->1`, and `k->l`
  redistribution edges. Stages with degree one remain unsplit and incur no
  tensor collective merely because another stage is sharded.
- Require redistribution when declared source/target layouts are incompatible
  even if adjacent stages have equal rank counts; compatible equal-degree
  boundaries remain ordinary authenticated pipeline dependencies.
- Bind sharded KV/reusable state, collective workspace, batch/sequence envelope,
  loaded-runtime identity, and failure propagation to the exact rank group.

**Phase 3C exit gate**:

- pipeline-only, tensor-only, `[1,2,1]`, and `[2,1,2]` plans produce complete
  outputs matching the unsplit oracle within a predeclared dtype/backend
  tolerance;
- traces contain ranks/collectives only for stages whose degree exceeds one and
  redistribution exactly at declared degree- or layout-changing boundaries;
- unsupported degree/layout transitions, wrong producer/consumer rank maps,
  duplicate redistribution, or omitted redistribution fail before unsafe
  execution or terminate the exact boundary/group deterministically;
- data-driven execution has no global model-ready barrier: an unsplit stage or a
  complete local tensor group starts as soon as its own preparation and direct
  authenticated inputs are ready.

### Phase 4 - MiniNDN/exact-SIF closure and TigerCluster 0/1/2-GPU gate

**Behavioral outcome**: The exact candidate demonstrates that scheduler
allocation, SIF exposure, Provider offers, placement, preparation, and complete
inference agree for zero, one, and two GPUs without changing application logic.

- Gate A: before freeze, run contract/unit/negative suites, including
  topology/profile/snapshot,
  device binding, canonical artifact, rank coverage, redistribution, atomic
  admission, residency, and migration.
- Gate B: before freeze, run real MiniNDN with the minimal model and
  deployment-faithful process, security, Repo, request, three-Provider
  concurrency, cold/warm, queue/JIT admission, progress, and data-driven
  execution paths. Simulated device topology proves control logic; it is not
  CUDA evidence.
- Gate C: build the candidate OCI/SIF once, then before formal freeze run that
  exact SIF in CPU/no-GPU mode and bounded CUDA preflights, proving that the
  container has no hidden test-only defaults and the native offer path is wired.
- After every executable source/security/build/harness change and Gate A/B/C
  result is complete, freeze one source, OCI, SIF, dependency-lock, model,
  canonical-artifact, prompt/workload, security-policy, and route identity.
  After this cut, tasks may execute the candidate and write evidence/docs only.
  Any executable or harness change invalidates the freeze and returns to the
  owning pre-freeze task.
- Gate D0: submit a no-GPU Slurm job with no GPU request and no `--nv`; require a
  zero-device signed offer and correct CPU-allowed selection or GPU-required
  rejection.
- Gate D1: request one GPU and launch with `--nv`; require one offered/selected
  device, one complete minimal-model response, and no CPU fallback.
- Gate D2a: request two GPUs for one Provider task and launch with `--nv`;
  require both and only allocated devices in the offer, then validate two
  independent single-GPU roles and one Provider-local two-rank logical role.
- Gate D2b: within a separately declared two-GPU allocation/topology, launch two
  Provider runtimes with one allocated GPU each and validate one two-rank
  logical role across their authenticated Provider-local bundles. Record this
  separately from D2a; a D2a success is not cross-Provider evidence.
- Gate D2h: after D2a and D2b close, run the heterogeneous `[1,2,1]` and
  `[2,1,2]` profiles on the frozen two-Provider/two-GPU mapping below and retain
  separate rank/collective/redistribution/oracle evidence. If the resource or
  topology envelope is insufficient, the real-CUDA hybrid claim remains
  `BLOCK`; local emulation or combining D2a/D2b evidence is not a substitute.
- D2h mapping is fixed before the run. For `[1,2,1]`,
  `P0/G0={S0R0,S1R0}` and `P1/G1={S1R1,S2R0}`. For `[2,1,2]`,
  `P0/G0={S0R0,S1R0,S2R0}` and `P1/G1={S0R1,S2R1}`. Multiple ranks on one GPU
  are members of one plan-local `EXCLUSIVE_PLAN` admission vector with summed
  phase peaks; this is not MPS or multi-tenant sharing. A different mapping,
  missing envelope, or insufficient memory is `BLOCK`, not an adaptive rewrite.
- In one unchanged accepted allocation, measure five real prompts, each with one
  warmup and five measured requests. Retain complete answers, TTFT, per-token and
  total latency, tokens/s, Repo bytes, assembly/load events, device/rank mapping,
  cache state, progress, failures, and CPU-fallback count.
- Report cold and warm rows separately with median, p95, dispersion, paired
  differences, and confidence intervals where sample size permits. Do not erase
  negative rows or infer large-model readiness from the minimal model.

**Exit gate**:

- every Slurm allocation maps exactly to the signed Provider offer and final
  Selection; unallocated devices are never visible as selectable resources;
- all admitted correctness profiles return complete authenticated answers and
  satisfy the local numerical oracle and lifecycle invariants;
- repeated exact-runtime requests demonstrate zero model transfer/assembly/load,
  while incompatible device sets correctly fall back only to valid lower-level
  disk/RAM reuse;
- any formal failure closes that candidate identity and returns to its owning
  local phase; no blind remote retry or in-place source mutation is allowed.

## Cross-Phase Gates and Rollback

| Gate | Required evidence | Blocks |
|---|---|---|
| Contract integrity | Canonical serialization/digests, V2/V3 rejection matrix, negative tests | all runtime work; pre-freeze |
| CPU/single-device | Real MiniNDN complete response, explicit accelerator policy, warm reuse | Phase 2; pre-freeze |
| Per-device/three-Provider concurrency | Independent roles, per-device budgets, no ACK holds, device-specific residency | Phase 3; pre-freeze |
| Multi-rank/hybrid | Local oracle equivalence for heterogeneous degree vectors, collective/redistribution faults | freeze; pre-freeze |
| Exact SIF | Same code/config/contracts, native ACK path, and truthful visibility | freeze; pre-freeze |
| Candidate freeze | All executable/harness/security/build hashes plus Gate A/B/C closure | every formal TigerCluster run |
| TigerCluster | Immutable campaign bundle and complete retained rows | implementation-complete claim |

Rollback never edits a committed plan. Before Selection, the strategy may choose
another feasible plan from the same frozen ACK_CLOSED snapshot. After Selection,
device/topology or collective failure creates a new attempt/plan under the same
public request ID, subject to the original deadline and policy. Canonical layers
and fully verified fragments may remain reusable; partial objects, stale loaded
runtimes, and incomplete collective state cannot be advertised as hits.

## Experiment Design Summary

The detailed design is in [experiment-plan.md](experiment-plan.md). The primary
hypotheses are:

1. the same Provider artifact reports and obeys exact 0/1/2 runtime-visible
   device sets;
2. per-device placement prevents aggregate-memory and partial-admission errors;
3. one Provider safely executes independent roles on different GPUs;
4. one logical role and heterogeneous `N x {M_i}` plans match an unsplit oracle;
5. exact loaded-runtime reuse removes model transfer, assembly, and reload from
   subsequent matching requests.

Controls freeze source/SIF/model/artifact/prompt/security/route identities.
Correctness and safety gates require 100% pass over deterministic and negative
cases; performance is reported as distributions rather than one best run.

## Project Structure

### Documentation (this feature)

```text
specs/170-reusable-layer-artifacts/
├── spec.md
├── plan.md
├── research.md
├── architecture-audit.md
├── data-model.md
├── experiment-plan.md
├── implementation-guide.md
├── traceability.md
├── quickstart.md
├── contracts/
│   ├── artifact-assembly-v1.md
│   ├── placement-v3.md
│   └── hybrid-execution-v1.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
NDNSF-DistributedInference/
├── ndnsf_distributed_inference/
│   ├── app_sdk/application.py          # normal V3 default dispatch
│   ├── app_sdk/client.py               # V2/V3 coordinator boundary
│   ├── app_sdk/placement.py            # canonical ensure versus legacy split
│   ├── sdk/placement.py              # offer/strategy/assignment V3 SDK
│   ├── splitter.py                   # logical stages, ranks and graph coverage
│   ├── planner/                      # LayerReuseFirstStrategy and compatibility
│   ├── core/contracts.py             # signed DI plan/residency contracts
│   ├── core/runtime_contracts.py     # topology/runtime capability contracts
│   ├── core/decision_validation.py   # per-device/rank/coverage validation
│   ├── core/deployment_control.py    # atomic admission and lifecycle
│   ├── core/protected_artifacts.py   # KeyGrant/plaintext lease state
│   ├── security/artifact_policy_authority.py # grant/revocation authority
│   ├── artifact_deployment.py        # fetch/assemble/load/residency transitions
│   ├── provider.py                   # real Python ACK/Selection path
│   └── adapters/                     # certified Qwen/ONNX partition semantics
├── cpp/ndnsf-di/
│   ├── ProviderResourceProbe.*       # actual runtime-visible resource discovery
│   ├── NativeProviderReadiness.*     # native V3 ACK serialization
│   ├── ProtectedRuntime.*            # protected plaintext lifecycle
│   ├── NdnsfCollectiveControl.*      # NDNSF_DATA_V1 transport
│   └── ProviderRoleWorker.*          # local scheduling/data-driven execution
└── experiments/                      # MiniNDN and campaign orchestration

examples/python/NDNSF-DistributedInference/llm_pipeline/
                                        # reference end-to-end adapter path
NDNSF-DistributedRepo/                   # existing immutable artifact APIs
packaging/ndnsf-di-container/            # OCI/SIF and Slurm exposure/evidence
tests/python/                             # unit, contract, integration gates
tests/container/                          # exact-container and allocation gates
specs/170-reusable-layer-artifacts/       # retained design/evidence authority
```

**Structure Decision**: Evolve the existing NDNSF-DI contract, planner,
deployment, adapter, and test surfaces; do not create a parallel inference
framework. Keep NDNSF Core opaque and model-neutral. Reuse Spec 164 Repo APIs and
Spec 168 deployment tooling, changing those owners only when a focused test
proves that the required behavior belongs there.

## Complexity Tracking

No constitution violation is accepted. Two complexities are intentional and
bounded:

| Complexity | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| Versioned V3 beside V2 compatibility | Preserves retained V2 evidence and prevents ambiguous equality/migration | Adding optional multi-device fields to V2 would silently change old offer, assignment, and cache meaning |
| Heterogeneous `N x {M_i}` plus redistribution contracts | Splits only stages that need it and supports real mixed pipeline/tensor execution | One global `M` wastes devices and cannot represent unsplit stages or unequal adjacent rank layouts correctly |
