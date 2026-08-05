# Architecture Audit: Canonical Layers and Provider-Local Assembly

**Date**: 2026-08-04  
**Verdict**: **ACCEPT AS A NEW SPEC, WITH CRITICAL CONTRACT CHANGES**  
**Implementation status**: Not implemented; this audit does not authorize a
new TigerCluster candidate.

## Executive Finding

The proposal fixes a real identity-boundary defect: the current design treats a
placement candidate, a role fragment, and a durable Repo artifact as almost the
same object. A stable model release is therefore republished when only the
ephemeral role layout changes.

The correct abstraction is a two-plane design:

```text
Stable artifact plane
  exact model -> canonical logical layers/components -> immutable Repo objects

Ephemeral execution plane
  ACK_CLOSED -> partition/placement -> RoleAssemblySpec
  -> Provider-local assembled fragment -> loaded state
```

This improves cross-plan reuse and makes Provider locality useful. It does not,
by itself, complete tensor parallelism: true intra-layer sharding also needs a
validated parameter-partition recipe and group collective semantics.

The same boundary must also support Providers with zero, one, or several
accelerators. Cluster allocation, container exposure, Provider discovery, and
placement are separate decisions. A correct strategy plans over the devices
actually visible inside the Provider container; it cannot infer feasibility
from one aggregate GPU-memory number.

## Current Source-Backed Boundary

The current on-disk implementation is role-artifact-centric:

- `AutomaticPlanningCoordinator.request()` performs graph inspection,
  candidate enumeration, strategy selection, artifact preparation, plan seal,
  and final `commit_plan()` after ACK closure in
  `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`.
- `_prepare_artifacts()` calls the Requester-side
  `SplitMaterializer.materialize()` and `DistributedArtifactPublisher.publish()`.
- `SplitCandidate` binds `fragments_by_role` and `artifacts_by_role`; each graph
  node has one role owner in
  `NDNSF-DistributedInference/ndnsf_distributed_inference/splitter.py`.
- `MaterializedSplit` and `PublishedSplit` are role-keyed maps. The catalog
  resolver currently requires exactly one artifact per role.
- The Qwen path publishes `stage-<index>-<digest>` objects and Providers fetch a
  complete selected stage package through
  `examples/python/NDNSF-DistributedInference/llm_pipeline/`.
- Existing role assignments bind one artifact digest and a contiguous layer
  range. Provider residency includes the partition identity, so it cannot prove
  that unchanged layers are reusable under a different split.
- `DIProviderOfferV2`, `DIProviderOfferIssuer`, `ProviderPlanningView`, and
  `DIRoleAssignmentV2` currently require positive GPU memory. This makes a
  no-GPU Provider invalid even though lower device-resolution code contains CPU
  branches.
- `ProviderPlanningView` exposes one `usable_gpu_memory_mb` scalar plus a tuple
  of device strings. The native `ProviderResourceProbe` measures host memory and
  process RSS but not per-GPU UUID, free memory, MIG, architecture, health, or
  interconnect. Tiger preflight records some GPU evidence outside the signed ACK
  path, which is not planning authority.
- `ProviderAssignment` and the lower execution-target contract currently carry
  one `device` string. They cannot bind a role to a device set or bind several
  roles on one Provider to disjoint devices.
- `PreSplitFirstStrategy._select_device()` requires exactly one CUDA device for
  a non-CPU backend. Joint placement and Selection admission sum GPU memory at
  Provider level rather than by device, and the current GPU ledger cannot admit
  a device set atomically.
- Current loaded residency and Qwen model-cache entries hold one GPU device per
  key, so promoting or preparing the same fragment on another GPU can overwrite
  or collide rather than represent two exact live runtime identities.
- Current Spec 168 TigerCluster jobs request exactly one RTX 5000 with
  `#SBATCH --gres=gpu:rtx_5000:1`; `apptainer exec --nv` exposes that scheduler
  allocation to the SIF. This validates a one-GPU deployment profile only, not
  general zero/multi-GPU behavior.
- The current container launch helper adds `--nv` unconditionally, so a true
  CPU-only SIF launch also needs an explicit compatibility path.

This is not merely an inefficient policy choice. It is encoded in the data
model, catalog, selection assignment, Provider preparation, and cache identity.

## Alternatives Considered

| Alternative | Persistent reuse | Transfer efficiency | Complexity | Verdict |
|---|---:|---:|---:|---|
| Publish one full model object | High across plans | Poor for partial roles | Low | Correct fallback, insufficient target |
| Publish role/stage packages | Low across plans | Good for one fixed plan | Existing | Reject as default |
| Publish one monolithic file per logical layer | High across pipeline plans | Tensor roles may refetch full layers | Medium | Minimum viable compatibility path |
| Layer manifests over canonical tensor/chunk objects | High across pipeline/tensor plans | Supports selective/range fetch | Higher | Chosen target |

The chosen design still presents a logical **layer** as the public reusable
unit. Its layer manifest may reference canonical tensor/chunk objects so
intra-layer roles need not force a new global stage package.

## Required Identity Separation

`ModelIdentity` first establishes the normalized source model independently of
checkpoint filenames and packing order. A separate `CanonicalArtifactProfile`
binds the layerizer/adapter, serializer/schema, chunking/layout, base precision,
and protection transformation. The following residency identities are then
relative to that exact pair.

### Canonical model/layer identity

Must bind:

- human-readable model alias (metadata, not equality);
- exact canonical weights/content digest;
- canonical architecture/parameter/configuration digest;
- graph and execution-semantics digest;
- tokenizer/pre/post-processing identity where applicable;
- model format, base precision, and protection epoch;
- layer kind plus fixed ordinal or stable graph-region identity;
- layer tensor/chunk manifest and content digest.

It must exclude request, attempt, Provider, role, stage boundary, shard count,
and strategy identity.

### Provider-local assembled-fragment identity

Must bind:

- canonical model-manifest digest;
- selected layer set/ranges;
- each tensor's shard axis, rank, world size, padding, and layout;
- adapter and assembler descriptor digests;
- backend/device ABI, precision, quantization, and protection epoch;
- collective group and reusable-state contract.

Only exact equality is a warm fragment hit. A Provider may separately advertise
that it has some or all canonical layers.

### Loaded-runtime identity

GPU readiness is a third level, not a storage tier on the fragment identity. It
must additionally bind runner/kernel or compile profile, backend/runtime/driver,
every device identity and architecture in the device set, its topology digest,
Provider boot epoch, process generation, and the exact reusable-state contract.
A disk/RAM fragment hit may avoid assembly but still require load; only an exact
live runtime hit on the same valid device set can avoid both.

## TigerCluster and Provider Device Responsibility

On TigerCluster, the Provider does **not** request GPUs from inside Docker/SIF.
The Docker/OCI image is first materialized as a SIF; the cluster executes that
SIF with Apptainer rather than starting a Docker daemon. Therefore the job asks
Slurm for GPUs, not `docker run --gpus` inside the image.
The deployment stack has five distinct responsibilities:

| Layer | Normative responsibility |
|---|---|
| Slurm job | Allocate zero, one, or N physical GPUs and enforce the job boundary |
| Apptainer/SIF launch | GPU mode uses `--nv` to expose the allocated NVIDIA devices/libraries; CPU-only mode does not request GPUs and omits GPU exposure |
| Provider startup probe | Enumerate only runtime-visible CPU/RAM/storage/devices and measure a fresh per-device topology |
| Provider configuration | Optionally use all visible devices, none, or an explicit visible subset; never create capacity |
| `ModelPlacementStrategy` | Select feasible Providers, roles, and explicit CPU/single-device/device-set bindings from signed offers |

Host ordinals are not a stable application contract: a scheduler/container may
renumber the assigned devices. Offers should therefore use topology-bound,
offer-scoped handles and, where policy permits, stable device UUID evidence.
`CUDA_VISIBLE_DEVICES` is a discovery/filtering input, not the security boundary;
Slurm and the container runtime enforce device isolation.

A zero-GPU Provider is valid. It can advertise storage/Repo transfer, canonical
layer preparation, assembly, tokenizer/pre/post-processing, and
adapter-certified CPU execution. It is excluded only when a role explicitly
requires an accelerator. Silent CPU fallback is not acceptable evidence of a
GPU plan.

A multi-GPU Provider has two different execution modes:

1. **Several independent roles**: the strategy can bind role A to device 0 and
   role B to device 1. This is the simpler first milestone and allows local
   concurrency under per-device accounting.
2. **One role across a device set**: the strategy can bind one role to devices
   0 and 1 only when an adapter/backend-certified tensor or pipeline recipe
   defines slices, local ranks, collectives, ordering, and whole-group failure.

Two 12-GiB GPUs are not one 24-GiB GPU. They cannot satisfy a 20-GiB
single-device allocation unless a certified sharding recipe converts that role
into valid per-device allocations. NVLink/NVSwitch, PCIe/NUMA locality, peer
access, and link bandwidth may also change feasibility and cost.

## Corrected Lifecycle

```text
0. Scheduler allocates zero/one/N devices, container runtime exposes the
   allocation, and each Provider probes its actual resource topology.
1. Application sends Request(ModelIdentity, task, input, options, deadline).
2. Providers return signed capability/willingness offers, including a fresh
   per-device topology; no role is prechosen.
3. Requester freezes ACK_CLOSED.
4. Digest-pinned adapter inspects the immutable graph and certified legal
   layer/tensor partition recipes.
5. Strategy uses graph + ACK capacity/cache/network/queue information to choose
   topology, Providers, RoleAssemblySpecs, CPU/single-device/device-set bindings,
   and data/collective dependencies.
6. Requester-side layerizer idempotently ensures one canonical model/layer set:
   existing verified objects are reused; missing objects are published; the
   complete root manifest becomes ACTIVE last.
7. Plan is sealed against ACK_CLOSED, model/graph, strategy, offers, recipes,
   and resource estimates; final Selection is sent once.
8. Each Provider validates its complete projection and atomically accepts one
   bounded queue record without reserving GPU/device capacity.
9. Each Provider reuses an exact local fragment or fetches missing canonical
   objects and assembles host-side. Immediately before load it revalidates the
   selected handles/topology/resource sequence and atomically acquires the
   complete local device vector plus fencing token, or acquires nothing.
10. A pipeline role starts when locally ready and its authenticated predecessors
   are ready. A tensor group starts a collective epoch only when all selected
   ranks and its stage input are ready. Neither requires unrelated Providers.
11. One authenticated Response closes the invocation; canonical layers,
    assembled fragments, and eligible live runtimes remain separately visible
    to later ACK offers under bounded eviction policy.
```

Canonical publication may be performed before a request as an idempotent
optimization. It must not reintroduce an application-supplied deployment or
allow planning to ignore the actual ACK_CLOSED snapshot.

## Pipeline Versus Tensor Parallelism

### Pipeline/layer-range partition

The existing runtime already has a useful data-driven foundation: independent
role preparation, stage dependencies, and hidden-state transfer. The migration
replaces prebuilt stage packages with Provider-local assembly from canonical
layers, while preserving dependency-driven start.

### Tensor/intra-layer partition

The current generic role graph is not sufficient to claim true tensor
parallelism. A correct design needs:

- parameter/tensor views and adapter-certified legal shard axes;
- rank, world size, member-set digest, and compatible layouts;
- all-reduce, all-gather, reduce-scatter, broadcast, or model-specific merge
  operators with ordering and epoch identity;
- complete shard coverage and group-wide failure/abort semantics;
- tensor-sharded KV/reusable-state identity;
- group-local readiness and deadlines.

Weight files divided among Providers without these execution semantics remain
storage sharding, not demonstrated tensor-parallel inference.

### Heterogeneous hybrid partition

Hybrid placement is not a compulsory rectangular `N x M` grid. Its normative
shape is `N x {M_i}`: every pipeline stage independently chooses a tensor degree,
and `M_i = 1` leaves that stage unsplit. For example, `[1, 2, 1]` tensor-shards
only the middle stage. A boundary whose degree changes or whose declared source
and target layouts are incompatible requires an adapter-certified redistribution
operation and rank/layout contract; the trusted plan sealer must reject
unsupported `1 -> k`, `k -> 1`, or `k -> l` transitions before Selection.
This avoids wasting devices and collectives on stages that already fit one
device while still allowing selected memory- or compute-heavy stages to scale.

Even when `M_i > 1`, the degree defines the participant group rather than a rule
to divide every tensor equally. The adapter recipe must separately classify
weights/state/activations as sharded, replicated, rank-owned, or locally derived.
This is necessary for normalization, bias, shared/tied tensors, embeddings, and
other model-specific structures that cannot use one mechanical slice rule.

## API and Compatibility Consequences

Planning APIs must move from:

```text
SplitCandidate -> artifacts_by_role -> PublishedSplit(role -> artifact)
```

to conceptually separate outputs:

```text
CanonicalModelCatalog
CertifiedPartitionRecipes
PlacementDecision(role -> Provider, role -> RoleAssemblySpec)
```

They also need a resource boundary richer than the current scalar/device pair:

```text
ProviderResourceTopology
  CPU/RAM/storage
  DeviceTopologyProfile
    ComputeDeviceOffer[] + InterconnectEdge[] + sharing/failure domains
  DeviceResourceSnapshot
    health + per-device capacity + queue + allocation/resource sequence

PlacementDecision
  role -> Provider
  role -> RoleAssemblySpec
  logical role -> RankAssignment[]
  rank assignments -> ProviderLocalRoleBundle[]
  Provider-local bundle -> DeviceBinding(CPU | SINGLE_DEVICE | DEVICE_SET)
```

`ComputeDeviceOffer` carries per-device memory, architecture, runtime/precision
support, health, and an offer-scoped handle. `DeviceBinding` carries its member
handles, per-device budgets, local ranks, and the signed topology/resource
sequence. Handles are scoped to one signed Provider offer, so a cross-Provider
logical role cannot own one global binding. One Provider may receive multiple
local role bundles in one Selection; the Provider-local scheduler enforces them
but cannot invent new shard ranks or change a collective group chosen by the
sealed global plan.

The contract must separate a semantic `LogicalRole` from `RankAssignment[]`.
Tensor-parallel execution may implement one logical role with multiple ranks on
one or several Providers; validation can no longer assume one assignment per
role. Each rank needs its own device, shard/slice, and phase-specific resource
envelope for weights/replicas, activation, KV/state, collective workspace, and
assembly/load transient peak.

Selection queue acceptance and `DEVICE_SET` admission are distinct atomic
operations. Queue acceptance holds no device. At just-in-time admission, holding
device 0 while waiting for device 1 would create a hold-and-wait deadlock across
concurrent requests, so the Provider must acquire every member/local rank plus a
fencing token together or acquire nothing. Sharing modes and failure domains need
explicit representation; an exclusive GPU, MIG slice, MPS context, and
time-shared GPU are not interchangeable offers.

Final Selection can keep NDNSF Core opaque. Each role assignment should carry
one canonical model-manifest reference plus an opaque NDNSF-DI assembly recipe;
the Core need not carry or interpret every layer name.

Because roles are chosen after ACK closure, Provider offers must advertise
bounded **role capability predicates/classes**, not require exact enumeration of
all future stage/shard role IDs.

The current role-scoped V2 artifact path should remain an explicit compatibility
profile (`PREASSEMBLED_PARTITION_SINGLE_DEVICE`). A new versioned assignment/candidate schema
is required rather than silently changing V2 equality: V2 remains the
single-device compatibility path, while the new schema represents CPU and
device-set bindings, logical roles/ranks, topology snapshots, and atomic
admission. Existing evidence remains evidence for the V2 baseline only.

## Security and Failure Gates

- Separate original-publisher provenance and signed layerizer transformation
  attestation, plus digest-bound layer/tensor/chunk content.
- Mandatory protected-profile state with authorization-bound `KeyGrantV1`,
  revocation/protection epochs, registered plaintext host/device leases, runtime
  fencing, and verified zeroization; a per-request artifact identity would
  destroy intended reuse.
- Non-circular sealing: grants bind immutable `planCoreDigest`; the final
  `planDigest` binds the core, complete sorted Provider grant cover, and one
  security-policy snapshot before any Selection is published.
- Locally installed, allowlisted, digest-pinned adapters/assemblers only;
  external strategies return declarative choices, never executable code.
- Strict tensor shape, byte size, path/sandbox, peak-memory, and complete-cover
  validation before assembly.
- Atomic Provider-local assembly: partial output is quarantined and never
  advertised in an ACK.
- Per-object fetch and per-recipe build single-flight, with bounded exact ACK
  inventory; probabilistic summaries are hints rather than hit evidence.
- Separate progress/failure boundaries for layerization, publication, manifest
  activation, fetch, verification, assembly, load, collective, execution, and
  response.
- ACK remains a signed bounded willingness/capability snapshot and creates no
  reservation or queue state. It separately declares exact-reuse-only,
  preparation-accepted, or reject; only the first two use a positive generic
  ACK. Selection creates only a bounded queue/preparation record, and new
  fetch/assembly is legal only for a preparation-accepted offer; device
  admission occurs atomically immediately before load.
- A Selection may reference only offer-scoped device handles bound to the exact
  signed offer, topology profile, resource snapshot, and resource sequence.
  Device loss, MIG/topology change, or stale allocation fails closed; no silent
  rank remap or CPU substitution is allowed.
- Scheduler/container isolation remains the access-control authority. Device
  environment variables and Provider policy only describe or restrict the
  already-exposed resource view.

## Academic Review Verdict

### Strengths

1. Separates invariant model content from variable deployment decisions.
2. Makes the claimed warm-request improvement structurally possible across
   multiple partition plans, not only exact repeat of one prebuilt stage.
3. Preserves the Request-first, ACK-informed planning principle.
4. Places model-specific semantics in NDNSF-DI adapters rather than NDNSF Core.
5. Creates measurable hypotheses: duplicate-byte reduction, transfer savings,
   assembly cost, exact warm-hit latency, and hybrid execution correctness.
6. Makes one Provider artifact portable across CPU-only, single-GPU, and
   multi-GPU allocations without pretending the resources are equivalent.

### Major risks that block an implementation claim

1. A "layer" cannot mean only numbered transformer blocks; shared and special
   components need first-class coordinates.
2. Full-layer downloads may remove storage duplication but still waste network
   and memory for tensor shards; the tensor/chunk index is necessary for the
   long-term target.
3. Arbitrary strategy-provided slicing is unsafe and may produce numerically
   invalid models; recipes must be adapter-certified.
4. Tensor sharding without collective semantics is an overclaim.
5. Dynamic post-ACK roles conflict with exact pre-enumerated role offers and
   require a versioned capability-predicate contract.
6. Existing Spec 168 SIF/campaign evidence cannot validate this new artifact
   identity or execution contract.
7. A fragment identity alone cannot prove that a live GPU runtime is reusable;
   conflating them would make warm-hit evidence unsound.
8. Protected models need opaque namespaces, authorization expiry/revocation,
   and plaintext host/device-memory cleanup rather than only encrypted Repo
   bytes.
9. The current aggregate GPU memory and single-device assignment contract can
   admit impossible plans, rejects zero-GPU Providers, and cannot safely express
   either multi-role or one-role/multi-GPU execution.
10. Current TigerCluster evidence requests one GPU per job. It cannot be cited
   as validation of automatic zero/multi-GPU adaptation until exact-SIF 0/1/2
   visibility and binding gates pass.
11. A flat role-to-device mapping cannot represent one logical role with
    multiple ranks, and non-atomic multi-device admission can deadlock concurrent
    requests even when aggregate capacity appears sufficient.
12. Stable topology and volatile capacity are different evidence. Reusing a
    topology digest with a stale free-memory/queue snapshot, or vice versa, can
    invalidate the placement decision.

## Recommended Milestones

1. **Resource-topology contract**: zero/one/N discovery, signed per-device
   profile/snapshot offers, logical-role/rank separation, CPU/single-device/
   device-set bindings, phase-specific resource vectors, atomic admission,
   stale-topology failure, and exact-SIF visibility parity.
2. **Canonical component plane**: exact naming/manifests, idempotent publication,
   two different pipeline layouts reuse one layer set.
3. **Provider-local pipeline assembly**: cold/warm MiniNDN and exact-container
   evidence, cache identity split, migration from preassembled V2 packages.
4. **Tensor/hybrid execution**: certified tensor recipes, collective group
   runtime, numerical-oracle tests, sharded state, group failure semantics.
5. **Deployment fidelity**: only after the local gates, freeze a new immutable
   source/SIF identity and run one admitted TigerCluster campaign.

Until Milestones 1-4 pass, the design should be described as specified rather
than implemented or experimentally validated.
