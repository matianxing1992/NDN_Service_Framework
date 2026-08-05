# Research Decisions: Reusable Layers and Adaptive Multi-Device Placement

**Feature**: `170-reusable-layer-artifacts`  
**Date**: 2026-08-04  
**Status**: Phase 0 complete; no unresolved clarification remains for planning

## Decision 1: Use heterogeneous `N x {M_i}` hybrid topology

**Decision**: Represent a plan as `N` pipeline stages with an independent tensor
degree `M_i >= 1` per stage. `M_i = 1` means that stage is unsplit. The rank
count is `sum(M_i)`, not `N * max(M_i)`.

**Rationale**: Different layer ranges have different weight, activation, KV,
workspace, and compute costs. Requiring every stage to use one global tensor
degree wastes devices and creates unnecessary collectives.

**Alternatives considered**:

- Global `N x M`: rejected because it forces uniform horizontal splitting.
- Pipeline-only: retained as the special case where every `M_i = 1`, but
  insufficient for a stage that cannot fit one device.
- Tensor-only: retained as `N = 1`, but insufficient for heterogeneous devices
  and data-driven layer-range execution.

## Decision 2: Tensor degree does not mean every tensor is sliced

**Decision**: `M_i` defines the participating rank group for stage `i`. The
adapter-certified recipe separately marks every relevant parameter/activation
as sharded on an axis, replicated, rank-owned, or otherwise locally derived.

**Rationale**: Biases, normalization state, embeddings, shared/tied weights,
metadata, and model-specific tensors often require replication or unique
ownership rather than mechanical equal slicing.

**Alternatives considered**:

- Slice every tensor by `M_i`: rejected as numerically and structurally invalid.
- Let each Provider decide locally: rejected because ranks could choose
  incompatible layouts and collective semantics.

## Decision 3: Make degree- or layout-changing stage boundaries explicit

**Decision**: Boundaries with `M_i != M_(i+1)` or incompatible declared tensor
layouts carry an adapter-certified redistribution contract: `1->k`
scatter/broadcast, `k->1` gather/reduce/merge, or `k->l` reshard. Equal degrees,
including `1->1`, use a normal pipeline dependency only when source and target
layouts are directly compatible.

**Rationale**: Rank-count changes alter producer/consumer membership, but equal
rank counts can still use incompatible axes, padding, ownership, or layouts.
Inferring the transition from counts alone is unsafe.

**Alternatives considered**:

- Always gather to one rank at every pipeline boundary: correct for some models
  but rejected as a mandatory design because it can waste bandwidth/memory.
- Hide redistribution inside the runner: rejected because the sealed plan,
  failure evidence, and numerical validation would be incomplete.

## Decision 4: Add a versioned V3 placement contract

**Decision**: Introduce a new NDNSF-DI placement profile for CPU/device-set,
logical-role/rank, topology, canonical-artifact, and atomic-admission semantics.
Keep V2 as explicit preassembled single-device compatibility.

**Rationale**: Current V2 requires positive GPU memory, one device string, one
assignment per role, and Provider-level aggregate memory. Optional fields would
silently change existing equality and evidence.

**Alternatives considered**:

- Mutate V2 in place: rejected because old serialized plans and cache identities
  would acquire ambiguous meaning.
- Remove V2 immediately: rejected because it would unnecessarily break current
  single-device regressions and retained experiment evidence.

## Decision 5: Provider is a scheduling/security domain; device is a resource

**Decision**: One Provider identity may own zero, one, or many devices. The
strategy selects exact offer-scoped devices or device sets; the Provider-local
scheduler enforces but cannot rewrite the sealed assignment.

**Rationale**: Registering each GPU as a Provider loses local topology,
single-flight/cache sharing, atomic admission, and intra-Provider collectives.

**Alternatives considered**:

- One Provider per GPU: retained as a deployable compatibility topology, but it
  is not the general multi-GPU Provider model.
- Provider chooses a GPU after Selection: rejected because ACK evidence and
  loaded-runtime identity would no longer bind the executed resource.

## Decision 6: Separate stable topology from mutable resource state

**Decision**: `DeviceTopologyProfile` binds device identity/architecture,
connectivity, peer access, collective capability, and sharing/failure domains.
`DeviceResourceSnapshot` binds health, free/reservable capacity, queue,
allocation epoch, capture time, and resource sequence.

**Rationale**: Topology and free capacity change at different rates. Selection
and revalidation need both exact digests without retransmitting an unbounded ACK.

**Alternatives considered**:

- One aggregate GPU-memory scalar: rejected because `2 x 12 GiB` is not one
  24-GiB device and connectivity is lost.
- One unbounded inline topology: rejected because ACK size would grow with
  devices and detailed inventory.

## Decision 7: Zero GPUs is a valid Provider state

**Decision**: CPU/no-GPU Providers advertise real CPU/RAM/storage/network
capabilities and may execute only roles whose adapter and accelerator policy
allow CPU. `AUTO`, `NONE`, and `EXPLICIT_SUBSET` configuration can restrict but
never expand the container-visible set.

**Rationale**: Storage, layerization, assembly, preprocessing, and CPU-capable
inference remain useful without a GPU. Requiring fake positive GPU memory is an
incorrect contract workaround.

**Alternatives considered**:

- Reject no-GPU startup: rejected as unnecessary and incompatible with portable
  containers.
- Silent CPU fallback: rejected because it invalidates placement, performance,
  and experimental claims.

## Decision 8: Use canonical layers plus Provider-local assembly

**Decision**: Publish normalized, content-addressed canonical layer/tensor/chunk
objects once. A sealed `RoleAssemblySpec` tells a Provider which objects and
tensor distributions to assemble locally. Keep canonical model, assembled
fragment, and loaded runtime as three distinct identities.

**Rationale**: Role/stage packages duplicate durable bytes whenever placement
changes. A fragment alone also cannot prove that a live device runtime is reusable.

**Alternatives considered**:

- Publish one full model: simple but forces excessive transfer/staging.
- Continue role-scoped pre-splits: retained only for V2 compatibility because it
  prevents reuse across different pipeline/tensor layouts.

## Decision 9: Queue acceptance and device admission are separate

**Decision**: ACK collection creates no reservation or queue state. A V3 offer
separates generic willingness from new model preparation:
`ACCEPT_IF_EXACT_REUSE` and `ACCEPT_WITH_PREPARATION` are positive ACKs;
`REJECT` is the only negative/unselectable ACK. Final
Selection atomically accepts the complete Provider projection into a bounded
queue without holding a device. Host-side fetch/assembly may proceed under
disk/RAM/network preparation bounds only when the bound offer accepted
preparation; exact reuse skips that work. Immediately before device load, independent
single-device roles use per-device ledgers and a `DEVICE_SET` plus all local
ranks acquires one complete resource-vector lease/fencing token or acquires
nothing.

**Rationale**: Provider-level summation admits impossible placements; acquiring
GPU 0 while waiting for GPU 1 creates hold-and-wait deadlock.

**Alternatives considered**:

- Reserve on ACK: rejected because it harms concurrency and reserves resources
  before an exact assignment exists.
- Select a Provider after an overall negative ACK: rejected because it bypasses
  authenticated willingness. Reuse-only acceptance is an explicit positive
  disposition instead.
- Sequential device acquisition: rejected because partial admission is not a
  valid collective state.
- Device reservation at Selection/queue acceptance: rejected because long Repo
  fetch/assembly would hold scarce GPUs and suppress concurrency.

## Decision 10: Validate locally before TigerCluster

**Decision**: Contract/unit and real MiniNDN gates validate control logic,
security, Repo transfer, identities, failure handling, and full output. Exact-SIF
parity follows. TigerCluster is then used only for real 0/1/2-GPU visibility,
CUDA/NCCL execution, and topology-dependent evidence.

**Rationale**: Most lifecycle and contract failures are deterministic and should
not consume remote allocations. The local 8-GiB host cannot establish actual
multi-GPU CUDA behavior.

**Alternatives considered**:

- Debug directly on TigerCluster: rejected because it repeats expensive remote
  discovery and weakens regression coverage.
- Treat simulated GPUs as final evidence: rejected because simulation cannot
  prove Slurm/Apptainer device exposure, CUDA allocation, or collectives.

## Decision 11: Separate external optimization from trusted plan sealing

**Decision**: `ModelPlacementStrategyV3` returns an untrusted declarative
`PlacementProposalV3`. It receives sanitized, validated
`ProviderPlanningViewV3` values rather than raw wire offers. A framework-owned
`PlanSealerV3` independently resolves, canonicalizes, and validates the
proposal into `PlacementPlanCoreV3`. Protected grants bind `planCoreDigest`;
the sealer then derives final `planDigest` from the core, sorted grant bindings,
and security-policy snapshot before generating Provider-specific Selection
projections. This two-stage identity avoids a grant/plan-digest cycle.

**Rationale**: External strategies must be customizable without gaining the
ability to bypass offer freshness, device ownership, resource, graph, tensor,
dependency, or security invariants.

**Alternatives considered**:

- Let the strategy construct wire Selection: rejected because optimization code
  would become a protocol/security authority.
- Trust only the built-in strategy: rejected because it would contradict the
  public strategy extension requirement.

The initial implementation trust model is explicit: custom strategy code is
installed by the Requester operator and trusted with respect to that process,
but its output remains untrusted and bounded by deadline/candidate budget.
Loading strategy code from a request or network artifact is unsupported. A
future adversarial-plugin profile would require out-of-process CPU/memory/time/
I/O isolation and is not implied by this contract.

## Decision 12: Stage Provider-local and cross-Provider tensor groups separately

**Decision**: First close one logical role across a device set owned by one
Provider. Then close one logical tensor group whose ranks live in multiple
Provider-local bundles. Only after both pass should heterogeneous pipeline/
tensor vectors combine these forms.

**Rationale**: Provider-local multi-GPU execution uses one admission transaction
and local communicator scope. Cross-Provider execution adds authenticated
rendezvous, transport, independent local admission, peer failure, and distributed
epoch handling; treating them as one gate hides distinct failure modes.

**Alternatives considered**:

- One combined multi-GPU milestone: rejected because a local NCCL success would
  not prove distributed rank semantics.
- One global cross-Provider `DeviceBinding`: rejected because device handles are
  meaningful only inside their signed Provider offers.

## Resolved Technical Baseline

- Python package baseline: Python >=3.8.
- Exact GPU container baseline currently pins PyTorch 2.6, CUDA 12.4, ONNX
  Runtime GPU 1.20.1, and NCCL 2.21.5; implementation must bind actual lock-file
  identities rather than rely on these descriptive versions alone.
- Current V2 seams include `DIProviderOfferV2`, `ProviderPlanningView`,
  `ProviderAssignment`, `DIRoleAssignmentV2`, `DISelectionAssignmentV2`,
  `validate_joint_placement`, `GpuMiBAdmissionLedger`, `RoleExecutionPlan`,
  `RoleResourceRequirement`, `ProviderResidencyIdentity`, and Provider model/
  residency caches.
- Existing Spec 164 artifact APIs and Spec 168 MiniNDN/SIF/Tiger tooling are
  dependencies, not evidence that Spec 170 behavior already exists.
