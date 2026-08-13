# Data Model: Canonical Artifacts, Device Topology, and Hybrid Execution

**Feature**: `170-reusable-layer-artifacts`  
**Contract profile**: planned `DI_PLACEMENT_V3`

## Identity Layers

### ModelIdentity

Immutable equality for one model release.

Required fields:

- original publisher/provenance reference and signature identity;
- normalized tensor-map digest;
- canonical parameter/configuration and execution-semantics digests;
- immutable dependency-graph digest;
- tokenizer and pre/post-processing identity where applicable;
- optional human alias/revision as non-equality metadata.

Validation:

- filenames, archive order, parameter-count strings, and source shard packing do
  not decide equality;
- normalized tensor names, dtype, shape, byte order, shared/tied references, and
  floating-point representation do decide equality.

### CanonicalArtifactProfile

Defines one trusted reusable representation of a `ModelIdentity`.

Fields:

- layerizer/adapter descriptor digest;
- serialization/schema and chunk/tensor-index digests;
- base precision/layout/format;
- protection transform and protection epoch;
- deterministic tool/runtime descriptor.

One model may have multiple profiles. Profiles do not cross-hit merely because
their source model identity matches.

### CanonicalModelManifest

Atomically activated root of one model/profile representation.

Fields:

- model and artifact-profile identities;
- graph and adapter descriptors;
- ordered component/layer coordinates;
- canonical layer-manifest digests;
- origin provenance plus layerizer transformation attestation;
- lifecycle state and activation epoch.

States:

```text
ABSENT -> PUBLISHING -> VERIFYING -> ACTIVE
                     -> FAILED/QUARANTINED
```

Only `ACTIVE` is selectable. The root becomes active after every referenced
manifest/object is durable and verifiable.

### CanonicalLayerManifest

Stable logical model component independent of request/role/placement.

Fields:

- component kind and stable ordinal/graph-region identity;
- graph membership and input/output tensor contracts;
- tensor metadata and chunk index;
- content digests and sizes;
- shared/tied tensor references;
- protection/integrity metadata.

Component kinds include transformer layer, embedding, final normalization,
output head, shared tensor, expert group, and opaque adapter-defined region.

### LayerObject

Immutable content-addressed Repo object referenced by a layer manifest.
Object identity excludes request, Provider, role, stage, and tensor degree.

## Provider Resource Evidence

### ProviderResourceTopology

Signed ACK-time resource evidence composed of a stable profile and mutable
snapshot.

Relationships:

```text
ProviderOfferV3
  -> DeviceTopologyProfile
  -> DeviceResourceSnapshot
  -> ResidencyInventorySummary
  -> RoleCapabilityPredicates
```

Zero accelerator devices is valid.

### DeviceTopologyProfile

Relatively stable topology/capability identity.

Fields:

- Provider identity and boot/allocation domain;
- CPU/NUMA, host-memory, storage, and network topology;
- ordered `ComputeDeviceDescriptor[]`;
- `InterconnectEdge[]`;
- sharing/isolation/failure-domain graph;
- supported backend, dtype, partition, redistribution, and collective classes;
- profile digest and generation.

### ComputeDeviceDescriptor

Fields:

- offer-scoped opaque handle;
- optional stable GPU/MIG UUID where disclosure policy permits;
- container-visible runtime locator such as logical CUDA ordinal;
- vendor/type/architecture/compute capability;
- backend/runtime/precision support;
- sharing mode: `EXCLUSIVE`, `MIG`, `MPS`, or `TIME_SHARED`;
- parent and failure/isolation domain;
- peer-access and collective capabilities.

The runtime locator is not a long-term identity. A Selection uses the
offer-scoped handle bound to the exact topology profile.

### InterconnectEdge

Fields:

- endpoint handles;
- link class such as NVLink/NVSwitch/PCIe/NUMA;
- peer-access status;
- declared/measured bandwidth and latency with capture method;
- collective compatibility and failure domain.

### DeviceResourceSnapshot

Mutable capacity/health identity.

Fields:

- topology-profile digest;
- allocation epoch and resource sequence;
- capture time and freshness bound;
- per-device total/free/reservable memory and health;
- per-device queue/admitted workload;
- host CPU/RAM/storage availability;
- local scheduler limits;
- snapshot digest.

A stale or mismatched snapshot cannot be combined with a newer topology profile.

### ProviderOfferV3

Signed bounded willingness/capability snapshot.

Fields:

- Provider, service, boot epoch, offer digest, and expiry;
- topology-profile and resource-snapshot digests/references;
- supported task/adapter/backend/profile predicates;
- accelerator requirement classes;
- `executionDisposition`:
  `ACCEPT_IF_EXACT_REUSE | ACCEPT_WITH_PREPARATION | REJECT`, plus the explicit
  `preparationAccepted` boolean;
- bounded exact residency inventory/proof roots;
- queue, RTT/bandwidth, and preparation estimates;
- refusal/status evidence.

Both selectable dispositions use generic ACK `status=true`; `REJECT` alone uses
`status=false`. `preparationAccepted=false` therefore permits only an exact,
sealer-verified assembled-fragment or loaded-runtime hit and is not itself a
negative ACK. The only legal `(status, disposition, preparationAccepted)`
tuples are `(true, exact-reuse, false)`, `(true, accepts-preparation, true)`, and
`(false, reject, false)`; every other tuple is malformed and excluded before
ACK_CLOSED. An ACK creates no reservation, queue record, or GPU hold. Final Selection may
create a bounded queue record; exact device admission occurs later, immediately
before device-resident load.

### ProviderPlanningViewV3

Sanitized immutable projection derived by the trusted offer validator for an
external placement strategy.

Fields:

- Provider/service/boot epoch and offer expiry;
- offer, topology-profile, resource-snapshot digests and resource sequence;
- verified offer-scoped device views and capability predicates;
- verified execution disposition and preparation acceptance;
- bounded residency evidence views;
- queue, RTT/bandwidth, and preparation estimates.

The view excludes raw wire/signature/proof objects, certificates/private
material, live Provider/runtime objects, and executable code. The trusted sealer
resolves every proposal reference back to the exact signed offer.

## Planning and Assignment

### PipelineStageSpec

Fields:

- stage ID and ordered graph/layer coverage;
- `tensor_degree M_i >= 1`;
- input/output tensor contracts;
- resource envelope by rank/device;
- predecessor/successor boundaries.

`M_i = 1` creates one ordinary unsplit stage role. The degree vector may vary by
stage.

### LogicalRole

Semantic execution unit independent of rank count.

Fields:

- logical role ID and stage ID;
- graph/layer coverage;
- task/adapter/backend requirements;
- expected input/output and terminal semantics;
- rank count equal to the owning stage's `M_i`.

### TensorDistribution

Per-tensor rule inside a logical role/rank group.

Modes:

- `SHARDED(axis, rank, world_size, padding, layout)`;
- `REPLICATED(member_set)`;
- `OWNER_ONLY(rank)`;
- `LOCAL_DERIVED(recipe_digest)` where the adapter certifies derivation.

The stage tensor degree defines participants; it does not imply every tensor is
sharded.

### RankAssignment

One logical-role rank's sealed assignment.

Fields:

- logical role and rank ID;
- Provider identity;
- offer-scoped device handle or CPU binding;
- tensor-distribution references and layer selectors;
- phase-specific resource envelope;
- assembly-specification digest;
- collective-group membership;
- input/output and redistribution endpoints.

### PerDeviceResourceEnvelope

Fields:

- weight and replicated bytes;
- activation bytes;
- KV/request-state bytes;
- collective workspace;
- assembly/load transient peak;
- maximum batch and sequence envelope;
- optional compute/network estimates.

Every field is checked per device; values are not satisfied by Provider-level
summation.

### DeviceBinding

Modes:

```text
CPU
SINGLE_DEVICE
DEVICE_SET
```

`DeviceBinding` is Provider-local because its handles exist only in one signed
Provider offer namespace. A logical role spanning Providers therefore uses:

```text
LogicalRole -> RankAssignment[] -> ProviderLocalRoleBundle[] -> DeviceBinding
```

Fields:

- ordered local `RankAssignment[]` targets;
- Provider-local bundle identity;
- offer, topology-profile, resource-snapshot, and resource-sequence digests;
- sharing/admission policy;
- atomic-admission group ID;
- collective/communicator identity where applicable.

### ProviderLocalRoleBundle

Groups all ranks of one logical role assigned to one Provider.

Fields:

- Provider and logical-role identity;
- local rank IDs and their rank-assignment digests;
- one CPU/single-device/device-set binding;
- local assembly specifications and dependency/collective endpoints;
- complete local resource vector and atomic-admission group;
- offer/profile/snapshot/resource-sequence bindings.

One final Provider-specific Selection may carry several local bundles. Their
resources are validated/admitted as one local transaction, while each logical
role still prepares and executes from its own data dependencies.

### RoleAssemblySpec

Declarative, non-executable recipe.

Fields:

- model/profile/root-manifest and graph digests;
- layer/component selectors;
- per-tensor `TensorDistribution` rules;
- adapter/assembler descriptor digests;
- precision, quantization, layout, padding, and backend ABI;
- expected inputs/outputs;
- rank/group/redistribution contracts;
- expected disk/RAM/device/transient resource envelopes.

Providers execute only locally installed, digest-pinned adapter/assembler code.

### CollectiveGroup

Fields:

- group/member-set/rank and epoch identities;
- ordered devices and Providers;
- authenticated rendezvous contract;
- ordered collective operations and tensor layouts;
- readiness, timeout, cancellation, and whole-group failure rules;
- communicator/runtime compatibility digest.

For a Provider-local group, every rank belongs to one
`ProviderLocalRoleBundle` and admission covers the complete local device set.
For a cross-Provider group, each Provider retains its own local bundle and
`DeviceBinding`; the group additionally binds authenticated rendezvous,
transport endpoints, peer identities, readiness evidence, and whole-epoch
failure propagation. It never pools remote devices into one global binding.

### RedistributionEdge

Cross-stage dependency where adjacent tensor degrees/layouts differ.

Fields:

- producer and consumer stage/rank sets;
- source and target tensor layouts;
- operation: scatter, broadcast, gather, reduce, merge, or reshard;
- adapter recipe and integrity digest;
- ordering/epoch/completion/failure contract;
- expected transfer/resource envelope.

Unsupported transitions make the entire plan infeasible before Selection.

### PlacementProposalV3

Untrusted declarative output of an external `ModelPlacementStrategyV3`.

Fields:

- request/attempt, ACK_CLOSED, model/profile/graph, strategy, and offer
  or sanitized planning-view references;
- proposed stage degree vector, logical roles, ranks, Provider-local bundles,
  device references, assembly recipes, collectives, and redistribution edges;
- proposed resource/cost estimates, fallback order, and decision evidence.

The proposal contains no wire Selection, admission decision, executable
assembler, CUDA/runtime object, or authorization override. It is not executable
until independently sealed.

### PlanSealerV3

Trusted NDNSF-DI validation and canonicalization boundary.

Responsibilities:

- resolve every proposal reference against certified catalogs and the exact
  validated ACK_CLOSED offer set;
- verify graph/rank/tensor/dependency coverage, degree/layout transitions,
  device ownership/topology/freshness, per-device envelopes, and security
  bindings;
- canonicalize semantically equivalent proposals to deterministic plan-core
  bytes and `planCoreDigest`;
- reject invalid/opaque proposal content before Selection;
- produce `PlacementPlanCoreV3`, then finalize a complete non-secret grant cover
  and security-policy snapshot into `PlacementPlanV3`, then produce
  Provider-specific Selection projections.

### PlacementPlanCoreV3

Trusted immutable output of `PlanSealerV3.sealCore`. It contains all fields and
validated invariants of the placement decision except grant references and the
final plan digest. Its canonical bytes bind request/attempt, ACK_CLOSED, model,
profile, graph, strategy, offers, roles/ranks/bundles, assembly, dependencies,
collectives, device bindings, resource estimates, and every Provider's
protection requirement. `planCoreDigest` is the digest of those canonical
bytes. Grant requests bind this digest, preventing a circular dependency on the
final plan identity.

### PlacementPlanV3

Trusted, security-finalized post-ACK output produced only by `PlanSealerV3`.

Fields:

- public request ID, attempt/generation, deadline;
- ACK_CLOSED, model, profile, graph, strategy, and offer digests;
- ordered `PipelineStageSpec[]` and tensor-degree vector;
- `LogicalRole[]`, `RankAssignment[]`, `ProviderLocalRoleBundle[]`, and
  `RoleAssemblySpec[]`;
- `DeviceBinding[]`, collective groups, normal dependencies, and
  redistribution edges;
- resource/transfer/cost estimates and decision evidence;
- migration/profile version, `planCoreDigest`, canonical sorted
  `GrantBindingV1[]`, `securityPolicySnapshotDigest`, and final `planDigest`.

`GrantBindingV1` contains only Provider identity, grant name, and grant digest.
The final identity is
`H(canonicalPlanCoreBytes || canonicalSortedGrantBindings ||
securityPolicySnapshotDigest)`. The binding list is empty for an unprotected
plan. It has exactly one valid entry for each selected protected Provider;
missing, extra, duplicate, mismatched, or untrusted grants prevent finalization
and therefore prevent Selection.

### ProviderGrantViewV1

Deterministic non-executable projection of `PlacementPlanCoreV3` used only for
authorization. It binds request/attempt, `planCoreDigest`, selected Provider and
certificate, offer digest, model manifest, protection profile/epoch, requested
residency tiers, role/assembly digests, deadline, and policy authority. It
contains no grant, wrapped key, final `planDigest`, admission state, or runtime
object. `GrantRequestV1` carries its digest; it is never accepted as Selection.

Validation invariants:

- graph/layer/tensor coverage is complete and non-conflicting;
- rank count for stage `i` equals `M_i`;
- a stage with `M_i = 1` has no tensor collective;
- each tensor distribution is adapter-certified and complete;
- every degree- or layout-changing boundary has one complete certified
  redistribution path;
- every selected handle belongs to the bound offer/profile/snapshot;
- per-device resource envelopes fit without Provider-level memory pooling;
- each device-set admission group is complete and atomically admissible;
- dependency flow is acyclic outside explicit ordered collective epochs.

## Residency and Preparation

### CanonicalResidencyIdentity

Verified canonical model/profile/layer objects in Provider-local disk/RAM.
Independent of role, device, and placement.

### AssembledFragmentIdentity

Derived from model/profile, assembly-spec, adapter/assembler, backend ABI,
precision/quantization, and protection epoch. Physical device identity is not
required when the bytes are portable across compatible devices.

The ONNX baseline stores one immutable role/rank assembly as one
content-addressed `.ndnsf-onnx-artifact` bundle with embedded signed manifest.
It contains inline `model.onnx` when bounded, or `model.onnx` plus one external-
data entry when the large-model serialization/checking bound requires it. Its catalog
name binds provider, human model name plus identity digest, artifact profile,
graph, canonical role kind and layer-range/component coordinate, rank/degree,
assembly recipe, and assembled-object digest. Request/attempt IDs, arbitrary
stage labels, and filenames are excluded from equality. The NDN name is catalog metadata,
not a filesystem path. Verified canonical layers and assembled files obey a
bounded disk-cache lifecycle across requests; container-private scratch is
deleted at exit, and cross-container persistence requires an explicit bounded
cache mount.

For protected profiles the durable bundle stores only AEAD ciphertext and is a
valid disk hit only under an allowed `DISK_CIPHERTEXT_ASSEMBLED` residency tier.
The manifest binds protection epoch, domain-separated assembly/entry KDF
context, nonces, ciphertext lengths, and ciphertext digests; whole-file identity
is over ciphertext. Decrypted runtime files are `PlaintextLeaseRegistry`
allocations and cannot become an untracked persistent cache.

### LoadedRuntimeIdentity

Adds exact ordered device set, topology profile, device architecture, driver/
runtime/kernel profile, Provider boot/process/runtime generation, communicator
epoch, and reusable-state contract. It is invalid if any member disappears or
changes incompatibly.

### PreparationProgress

Monotonic phases:

```text
ASSIGNMENT_VERIFIED
-> QUEUE_ACCEPTED
-> MANIFEST_RESOLVED
-> FETCHING
-> CONTENT_VERIFIED
-> ASSEMBLING
-> FRAGMENT_ACTIVE
-> DEVICE_ADMISSION_PENDING
-> DEVICE_ADMITTED
-> LOADING
-> LOCAL_READY
-> EXECUTING
-> COMPLETED
```

Each phase may transition to a narrow `FAILED`, `CANCELLED`, or `EVICTED` state
with the original request/attempt/plan and last verified checkpoint.

## Admission State

### QueueAcceptanceRecord

Created atomically for a complete Provider Selection projection after validation.
It contains request/attempt/plan, projection digest, queue sequence, priority,
accepted/expiry timestamps, host-preparation envelope, and cancellation state.
It contains no device lease, reserved bytes, reservation ID, device ownership,
or admission fencing token.

### DeviceAdmissionLease

Created only when a queued/host-ready assignment reaches the execution-admission
boundary. It binds the current topology/profile/snapshot/resource sequence,
complete Provider-local per-device phase vector, ordered device set, sharing/
failure domains, monotonic fencing token, and lease/release timestamps.

Independent single-device assignments use per-device transactions. A device-set
assignment uses the same two-stage state machine with an atomic complete vector:

```text
PROPOSED -> SELECTION_VALIDATED -> QUEUE_ACCEPTED
                                  -> SELECTION_REJECTED
QUEUE_ACCEPTED -> HOST_PREPARING -> HOST_READY
QUEUE_ACCEPTED/HOST_PREPARING/HOST_READY -> CANCELLED | QUEUE_EXPIRED
HOST_READY -> DEVICE_ADMISSION_PENDING
DEVICE_ADMISSION_PENDING -> DEVICE_ADMITTED(fencingToken)
                         -> REPLAN_REQUIRED | ADMISSION_REJECTED
DEVICE_ADMITTED -> LOADING -> ACTIVE -> RELEASED | FAILED_GROUP
```

No state before `DEVICE_ADMITTED` holds device capacity. No intermediate state
may hold a strict subset of a requested `DEVICE_SET`. Every load/execution/release
operation rejects a stale fencing token.

## Protected Artifact Runtime

### KeyGrantV1

Signed authorization object encrypted to one Provider. Fields bind Provider,
request/attempt/plan core, model manifest, protection epoch, key ID/wrapped key,
allowed residency tiers, issue/expiry, revocation sequence, and active-request
revocation policy. The offer and planning view carry only proof of supported
protection profiles; they never carry the grant or plaintext key.

### PlaintextLeaseRegistry

Tracks every host/device plaintext allocation before exposure:

- allocation ID, address or offer-scoped device handle, and byte bound;
- grant, model/profile/protection epoch, runtime, and admission fencing token;
- state `HOST_PLAINTEXT_LEASED` or `DEVICE_PLAINTEXT_LEASED`;
- zeroization method, start/completion time, and verified outcome.

State machine:

```text
NO_GRANT -> GRANT_VERIFIED -> HOST_PLAINTEXT_LEASED
          -> DEVICE_PLAINTEXT_LEASED -> DRAINING -> ZEROIZED
any live state -> REVOKED -> DRAINING -> ZEROIZED
validation/cleanup failure -> FAILED_CLOSED
```

An expired/revoked/rotated grant or stale Provider/plan/device fence invalidates
the loaded-runtime identity immediately. Encrypted canonical objects may remain;
plaintext host fragments/device buffers may not survive `ZEROIZED`.

## Compatibility

- `DI_PLACEMENT_V3` is the only profile with CPU, device-set, logical-role/rank,
  canonical-layer assembly, or heterogeneous hybrid meaning.
- V2 is explicitly `PREASSEMBLED_PARTITION_SINGLE_DEVICE`.
- V2 and V3 model/cache/assignment digests never compare equal by projection.
- Fully verified canonical bytes may be converted/imported through an explicit
  migration record; live runtime identities never cross versions implicitly.
- V2 is an explicit supported profile for Spec 170, not an automatic V3 fallback
  or a temporary path with an implicit deletion date. A future breaking-change
  spec is required to remove it.
