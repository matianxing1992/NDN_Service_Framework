# Feature Specification: Reusable Canonical Model-Layer Artifacts

**Feature Directory**: `specs/170-reusable-layer-artifacts`

**Created**: 2026-08-04

**Status**: Active architecture correction — implementation and evidence produced
under the superseded multi-role/multi-Provider-role interpretation are not
closure evidence until revalidated against this specification

**Input**: Replace request-specific, role-scoped model-shard publication with a
stable model-layer artifact plane. The Requester makes an immutable model
available as reusable logical layers; after ACK-based planning, each Provider
fetches the required layers and deterministically assembles its own role-local
pipeline, tensor, or hybrid fragment. The same Provider artifact must discover
and safely operate over zero, one, or multiple runtime-visible accelerators.

## Problem Statement

The current post-ACK path selects one split candidate and then materializes and
publishes artifacts indexed by role. Two requests for the same exact model can
therefore publish different large objects merely because they choose different
stage boundaries, Provider counts, or tensor-shard layouts. This couples durable
model storage to an ephemeral placement decision, reduces cross-plan reuse, and
makes the Requester a repeated shard-construction bottleneck.

This feature separates the exact **model identity** from the exact **canonical
artifact profile** used to layerize, serialize, chunk, and protect it. One model
may have more than one trusted artifact profile without becoming a different
model. Within one profile, it then separates three materialization levels:

1. **Canonical layer identity** is stable across requests, roles, Providers,
   and placement strategies.
2. **Assembled fragment identity** is derived from canonical layers plus one
   exact role assembly specification and pinned assembler/backend ABI. It is a
   Provider-local byte representation, normally on disk or in host memory.
3. **Loaded runtime identity** binds an assembled fragment to one live Provider
   boot/process/device-set/topology/runtime generation. Only this level is an
   exact accelerator-ready hit; it must not be inferred from fragment bytes
   alone.

The terms *vertical* and *horizontal* are ambiguous in isolation. This
specification uses the following normative axes and preserves the informal terms
only as aliases:

- **Pipeline/layer-range partition** (*vertical stage*): different roles own
  disjoint ordered logical-layer ranges.
- **Intra-layer/tensor partition** (*horizontal shard*): multiple roles own
  compatible slices of the same logical layers and participate in explicit
  merge or collective operations.
- **Heterogeneous hybrid `N x {M_i}` partition**: `N` pipeline stages, where
  stage `i` independently chooses tensor degree `M_i >= 1`; `M_i = 1` means the
  stage is not horizontally split. Different stages need not use the same
  degree.

## Scope and Ownership

- **NDNSF Core** remains a model-neutral collaboration carrier. It transports
  Request, ACK, the final Selection plan, authenticated dependency data, status,
  and Response; it does not interpret layers, tensors, model formats, or
  collective operators.
- **NDNSF-DistributedInference** owns model graph inspection, canonical
  layerization, placement strategy interfaces, role assembly specifications,
  Provider resource-topology and device-binding contracts, Provider-local
  assembly, cache reporting, and pipeline/tensor execution semantics.
- **NDNSF-DistributedRepo** stores and transports immutable manifests and layer
  content. It does not decide model partitions or execute model code.
- The **Requester-side preparation coordinator** is logically responsible for
  ensuring that the canonical layer set exists. It may reuse an existing set or
  invoke a trusted local/remote preparation service, but the public application
  call does not provide a deployment or pre-split layout.
- A **Provider** owns construction, loading, and caching of its request-selected
  derived fragment and live runtime instance. Derived fragments are
  Provider-local by default and are not republished as new global model objects
  merely because a placement changed.
- A **Provider runtime** discovers the resources actually exposed by its host,
  scheduler, and container boundary. Its configuration may restrict that set;
  it does not allocate cluster devices or advertise resources outside that set.

### ONNX deployment-runtime boundary

The deployed NDNSF-DI application runtime uses ONNX Runtime as its model
execution backend. PyTorch and Transformers are permitted only in a separately
sealed, offline exporter/conformance environment that converts a pinned source
model into canonical ONNX graph, initializer, tokenizer, and adapter metadata.
They MUST NOT be installed, importable, or copied into the final application
SIF/runtime. The runtime may use standalone ONNX graph/initializer tooling and
the `tokenizers` library for model-family tokenization.

Partitioning remains request-scoped. After the immutable ACK_CLOSED snapshot,
the trusted placement strategy chooses a complete `RoleAssemblySpec` from the
actual Provider offers and shares the sealed projection with the selected
Providers. Each Provider then fetches only the canonical ONNX layers/tensors it
needs, performs adapter-certified graph/initializer assembly locally, verifies
the resulting ONNX artifact, and executes it with ONNX Runtime. A precomputed
role/stage layout is not required by the public call and is not a substitute
for ACK-driven planning.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Reuse one model across different placements (Priority: P1)

An application invokes the same immutable model more than once while available
Providers, stage boundaries, or tensor-shard counts differ. The Requester reuses
one canonical layer set, and each Provider assembles the fragment required by
its current role without creating a second global copy of unchanged model
weights.

**Why this priority**: Decoupling stable model bytes from ephemeral placement is
the central value of the feature and removes repeated publication from the
critical path.

**Independent Test**: Run two valid plans for the same model identity with
different role boundaries. Verify that both plans bind the same canonical model
and layer identities, while their role assembly specifications differ and no
unchanged canonical bytes are published twice.

**Acceptance Scenarios**:

1. **Given** a complete verified canonical layer set, **when** a new ACK snapshot
   leads to a different pipeline partition, **then** planning creates new role
   assembly specifications but performs no model-layer materialization or Repo
   publication.
2. **Given** no canonical layer set, **when** the first request reaches post-ACK
   planning, **then** the Requester-side coordinator publishes each required
   layer exactly once, atomically activates the complete model manifest, and
   references that manifest in final Selection.
3. **Given** two concurrent Requesters publish the same exact model, **when**
   their publication races, **then** content-addressed put-if-absent semantics
   converge on one verified object set without a cross-request resource lock or
   conflicting active manifest.

---

### User Story 2 - Provider assembles and retains its own role fragment (Priority: P1)

After final Selection, a Provider verifies its assignment, obtains only the
canonical layers and auxiliary objects required by that assignment, constructs
the exact backend-compatible fragment locally, loads it, and retains safe
reusable state across later invocations.

**Why this priority**: Moving request-specific construction to the selected
Provider enables locality-aware reuse and prevents the Requester from becoming
the repeated data-conversion and upload bottleneck.

**Independent Test**: Select a Provider twice with the same exact assembly key.
The first invocation fetches, assembles, and loads the fragment; the second
reports an exact cache hit and executes without Repo model-byte transfer or
fragment reconstruction.

**Acceptance Scenarios**:

1. **Given** canonical layers on disk but no assembled fragment, **when** the
   Provider receives Selection, **then** it verifies the manifests, constructs
   the fragment within declared transient-memory bounds, records its exact
   assembled-fragment identity, loads it, and separately records the live
   runtime identity.
2. **Given** an exact GPU-ready loaded runtime identity, **when** a compatible
   later request assigns the same role assembly identity and reusable-state
   contract, **then** the Provider reuses it without fetching, rebuilding, or
   reloading model bytes.
3. **Given** only a partial canonical-layer cache, **when** the Provider prepares
   a role, **then** it fetches and verifies the missing objects only and reports
   progress rather than waiting behind a fixed deployment timeout.
4. **Given** a Provider that declines new preparation but advertises an exact
   assembled role/rank artifact, **when** the sealer proves the proposed
   `RoleAssemblySpec` matches that artifact, **then** its positive
   `ACCEPT_IF_EXACT_REUSE` ACK remains selectable and execution skips model
   fetch and assembly; a non-match publishes no Selection for that Provider.

---

### User Story 3 - Plan pipeline, tensor, and hybrid execution (Priority: P1)

An externally replaceable placement strategy consumes the immutable dependency
graph and validated ACK offers, then assigns Providers both role topology and
precise assembly specifications. The result may use layer-range partitioning,
intra-layer tensor partitioning, or both.

**Why this priority**: Weight slicing alone is not tensor-parallel inference.
Correct horizontal sharding also requires compatible activation contracts,
fan-out/fan-in, and collective operations, all owned by NDNSF-DI.

**Independent Test**: Execute one small inspectable model under a pipeline-only
plan, a tensor-only plan, and a hybrid plan. Compare complete outputs with an
unsplit reference under a declared numerical tolerance and prove that every
cross-role tensor and collective operation is represented in the sealed plan.

**Acceptance Scenarios**:

1. **Given** a pipeline plan, **when** a role's local fragment and authenticated
   predecessor data are ready, **then** it starts independently without waiting
   for unrelated Providers or a global model-ready barrier.
2. **Given** a tensor-sharded layer, **when** its shards execute, **then** the
   sealed plan identifies shard membership, tensor dimensions, padding/layout,
   and every required all-reduce, all-gather, reduce-scatter, broadcast, or
   model-specific merge operation.
3. **Given** a Provider that cannot implement the selected shard axis,
   collective, model adapter, backend ABI, precision, or peak assembly memory,
   **when** offers are evaluated, **then** it is excluded before Selection.
4. **Given** a hybrid plan with per-stage degrees `[1, 2, 1]`, **when** the plan
   is sealed, **then** only the middle stage has two tensor ranks; the first and
   last stages remain unsplit, and both stage boundaries carry adapter-certified
   redistribution contracts without phantom ranks or unnecessary collectives.

---

### User Story 4 - Operate on zero, one, or multiple accelerators (Priority: P1)

An operator may run one Provider in a container with no accelerator, one
accelerator, or several accelerators. The Provider discovers only the resources
actually visible inside its runtime boundary, applies an optional local policy
that may further restrict those resources, and advertises a signed per-device
topology. The placement strategy then chooses an explicit CPU, single-device,
or device-set binding for every assigned role.

**Why this priority**: A cluster scheduler, container runtime, and Provider own
different decisions. Treating several GPUs as one summed memory pool admits
impossible plans, while rejecting a CPU-only Provider prevents valid storage,
pre/post-processing, assembly, and adapter-certified CPU roles.

**Independent Test**: Run the same Provider artifact with zero, one, and two
visible accelerators. Verify that its signed offer exactly reflects the runtime
view, that configuration can only reduce that view, and that the strategy emits
only topology-feasible device bindings.

**Acceptance Scenarios**:

1. **Given** a TigerCluster job allocated no GPU, **when** the SIF starts the
   Provider, **then** the Provider starts in CPU-only mode, advertises zero
   accelerators without a dummy device or fake GPU memory, and remains eligible
   only for roles whose task/adapter policy permits CPU execution.
2. **Given** two visible GPUs, **when** the strategy considers one Provider for
   one Attempt, **then** it selects at most one complete role and binds that role
   to one exact CPU or single-device target. The other device remains available
   for another request/Attempt; the strategy MUST NOT turn local devices into
   hidden tensor ranks or treat their memories as one allocation.
3. **Given** an explicit configuration that names an unavailable device,
   **when** the Provider starts or reloads configuration, **then** the
   configuration transaction fails. Under `AUTO`, a device absent from the
   refreshed runtime view is excluded and the resource sequence changes; neither
   path advertises a resource not visible inside the container.
4. **Given** a device disappears or changes after ACK_CLOSED, **when** the
   Selection is revalidated, **then** the Provider rejects the stale binding
   without silently remapping ranks or weakening an accelerator requirement.

---

### User Story 5 - Prefer exact reusable state without overcommitting ACKs (Priority: P2)

The default `PreSplitFirstStrategy` first constructs feasible adapter-certified
role topologies from the immutable ACK snapshot. Within those feasible plans it
uses signed information about exact loaded-runtime residency, assembled-fragment
residency, canonical-layer residency, capacity, queue state, RTT, bandwidth,
supported assembly axes, and runtime compatibility as cost signals.

**Why this priority**: A later invocation should become much faster when the
right Provider already holds the exact fragment, while an ACK remains a bounded
willingness/capability snapshot rather than a long-lived GPU lock.

**Independent Test**: Present feasible Providers with an exact live GPU runtime,
RAM/disk assembled fragments, layer-only content, and no-cache states. Verify
deterministic ordering, capacity safety, and revalidation before use.

**Acceptance Scenarios**:

1. **Given** an exact feasible loaded-runtime hit, **when** the strategy assigns
   a role, **then** it ranks that hit ahead of otherwise comparable assembled-
   fragment, layer-only, or missing caches.
2. **Given** no exact fragment hit, **when** Providers cache different subsets of
   canonical layers, **then** the strategy accounts for missing verified bytes,
   expected transfer time, local assembly cost, queue delay, and peak memory.
3. **Given** stale cache evidence, **when** Selection reaches the Provider,
   **then** the Provider revalidates the exact content and either prepares it
   normally or reports a narrow preparation failure; it never executes on an
   approximate cache match.

---

### User Story 6 - Preserve provenance, confidentiality, and failure evidence (Priority: P2)

Operators can prove which immutable model, graph, layers, assembly algorithm,
Provider offers, and role topology produced a response. Corruption, identity
aliasing, unsupported code, and partial publication fail closed at their
narrowest lifecycle boundaries.

**Why this priority**: Increased reuse must not weaken model provenance,
authorization, integrity, or experiment reproducibility.

**Independent Test**: Inject a wrong model digest, altered layer chunk, stale
manifest, wrong adapter digest, oversized tensor declaration, incomplete model
cover, and incompatible shard group. Verify deterministic rejection before
execution with the original request identity and last valid progress checkpoint.

**Acceptance Scenarios**:

1. **Given** two releases with the same human-readable name but different
   weights or parameters, **when** they are published, **then** they have
   different immutable model and layer namespaces and cannot cross-hit caches.
2. **Given** a valid signed root manifest but one corrupted layer chunk,
   **when** a Provider fetches it, **then** content verification fails before
   assembly or GPU loading.
3. **Given** a protected model, **when** an unauthorized Provider can retrieve
   ciphertext, **then** it still cannot obtain the model-scoped decryption key or
   execute the assignment.

### Edge Cases

- A model contains embeddings, final normalization, output heads, tied/shared
  tensors, mixture-of-experts blocks, or non-repeating graph regions that do not
  fit a numbered transformer-layer pattern.
- A logical layer is larger than one Provider's disk, RAM, or GPU capacity.
- Tensor slices are strided or do not align with Repo chunk boundaries; the
  baseline MAY retrieve a full layer only when its staging envelope permits.
  When a full layer cannot fit, independently verifiable selective tensor/chunk
  retrieval is mandatory for plan feasibility; either path preserves the same
  canonical layer identity.
- The canonical model manifest exists but one referenced object is missing,
  quarantined, expired, or fails verification.
- Publication stops after some layers. No incomplete model manifest becomes
  ACTIVE, and a later idempotent publisher can resume missing objects.
- Two aliases point to the same immutable model bytes, or one alias is reused for
  different bytes. Aliases never determine cache equality.
- Providers use different accelerators, endianness, precision, quantization,
  kernels, or assembler ABI versions.
- A container exposes no accelerator, exposes only a scheduler-assigned subset,
  renumbers visible devices, or exposes multiple devices with asymmetric free
  memory, peer-access, interconnect, or NUMA locality.
- Two 12-GiB devices are visible but one unsplittable role requires 20 GiB on a
  single device. Aggregate free memory does not make that assignment feasible.
- A strategy attempts to assign two roles from one Attempt to a multi-device
  Provider, or attempts to split one role across Providers; both violate the
  one-to-one Provider-role invariant even when aggregate capacity is sufficient.
- Accelerator visibility, MIG partitioning, driver generation, or device health
  changes after ACK but before Selection or loaded-runtime reuse.
- A strategy emits overlapping/gapped pipeline coverage, incompatible tensor
  shard counts, impossible collectives, or a cyclic role graph.
- Capacity changes between ACK and Selection, or several Selections arrive
  concurrently. Providers enqueue/admit exact assignments without ACK-time GPU
  locks and expose queue/progress state.
- A Provider refuses new layer fetch/assembly but owns an exact verified role
  artifact. It advertises `ACCEPT_IF_EXACT_REUSE` with generic ACK `status=true`;
  it is excluded if the final role recipe is not an exact match. An overall
  negative ACK is never selected.
- Container scratch disappears after exit while a configured persistent cache
  mount remains, or no persistent mount exists. Catalog recovery must validate
  every sidecar/object before advertising reuse and must not confuse local cache
  lifetime with Repo durability.
- A Provider crashes after fetching layers but before assembly, after assembly
  but before load, or after producing dependency data. Restart recovery never
  reports an unverified partial fragment as reusable.
- Model confidentiality policy rotates. The protection epoch changes artifact
  identity without changing the human alias.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST distinguish exact model identity, canonical
  artifact-profile identity, immutable layer artifacts, Provider-local assembled
  fragments, and live loaded-runtime instances.
- **FR-002**: Canonical layer identity MUST NOT contain request ID, attempt,
  Provider identity, role name, stage boundary, shard count, or placement
  strategy identity.
- **FR-003**: Canonical model equality MUST be determined by cryptographic
  identity, not a human-readable model name, source revision, parameter count,
  filename, or object size alone.
- **FR-004**: A model identity MUST bind the original publisher/provenance,
  exact source weights/content digest, canonical parameter/configuration digest,
  execution-semantics digest, graph digest, and tokenizer/preprocessing identity
  when applicable. Human alias, source revision, and advertised parameter count
  are manifest metadata rather than equality keys.
- **FR-005**: The signed model manifest MUST record human-readable parameters
  such as architecture, parameter count, layer count, hidden size, attention
  heads, experts, precision, and source revision; their canonical digest MUST
  participate in immutable naming.
- **FR-006**: Every canonical layer MUST have an explicit semantic coordinate:
  model identity, layer kind, fixed ordinal or stable graph region ID, graph
  node/tensor membership, input/output contracts, and layer content digest.
- **FR-007**: Non-numbered and shared components MUST use explicit layer kinds
  rather than fake transformer ordinals. Required kinds include at least input
  embedding, numbered/block layer, final normalization, output head, shared
  tensor, expert group, and opaque atomic region.
- **FR-008**: For public models, the single canonical namespace MUST be
  `/<publisher>/NDNSF-DI/MODEL/v1/NAME/<model-name...>/MID/<model-identity-digest>/PROFILE/<artifact-profile-digest>`.
  Model manifests, layer manifests, and objects MUST extend that prefix as
  `/MANIFEST/<model-manifest-digest>`,
  `/LAYER/<kind>/<ordinal-or-region>/MANIFEST/<layer-manifest-digest>`, and
  `/OBJECT/<object-digest>/<segment-number>`, respectively. `MID` is the digest
  of the complete `ModelIdentity`; no separate `REV`, `CFG`, or `CONTENT` name
  variant is valid in V3. Concrete encoding rules MUST prevent alias/path
  ambiguity. Protected profiles MUST use the same structural grammar with
  policy-scoped opaque `NAME` components and keyed/non-correlatable identity
  components where policy requires them.
- **FR-009**: Large layer content MUST be deterministically serialized and
  accompanied by a tensor/chunk index sufficient to verify tensor name, dtype,
  shape, byte layout, offsets, and per-object digests. Full verified retrieval
  MUST remain correct when a complete layer fits. Verified selective/range
  retrieval MUST be available whenever an admitted tensor plan relies on less
  capacity than the complete layer or claims partial-transfer savings.
- **FR-010**: A Requester-side trusted layerizer MUST derive layers from a
  digest-pinned model-family adapter and immutable dependency graph; it MUST NOT
  infer legal boundaries from filenames or role labels. Its artifact profile
  MUST separately bind layerizer, adapter, serialization/schema, chunking,
  precision/format, and protection identities.
- **FR-011**: Canonical layer publication MUST be idempotent, resumable, and
  content-addressed. Concurrent publication of equal content converges, while a
  semantic-name/content conflict fails closed.
- **FR-012**: The complete model manifest MUST become visible as ACTIVE only
  after all referenced manifests and objects are durable and verifiable.
- **FR-013**: The public application invocation MUST require only model/task
  identity, input, options, and deadline; it MUST NOT require a precomputed
  deployment, role list, or pre-split artifact layout.
- **FR-014**: The normal lifecycle MUST remain `Request -> ACK collection ->
  immutable ACK_CLOSED -> graph/placement plan -> canonical layer ensure ->
  final Selection -> bounded Provider queue acceptance -> host-side preparation
  -> just-in-time atomic device admission -> load/local-ready -> data-driven
  execution -> Response`. Queue acceptance and device admission are distinct:
  Selection may enqueue bounded work but MUST NOT acquire a GPU/device lease.
- **FR-015**: Canonical layer ensure MAY begin before ACK closure as an explicit
  idempotent prepublication optimization, but final role planning MUST consume
  the immutable ACK_CLOSED snapshot and MUST NOT depend on a preselected layout.
- **FR-016**: A replaceable placement strategy MUST consume the immutable graph,
  compatible canonical artifact profiles/catalog, task requirements, and
  validated Provider offers, including CPU/RAM/storage capacity, per-device
  accelerator topology and memory, queue, RTT/bandwidth, runtime compatibility,
  supported partition axes/collectives, canonical-layer residency, exact
  assembled-fragment residency, and exact loaded-runtime residency.
- **FR-017**: Strategy output MUST include complete Provider-role assignments,
  role assembly specifications, pipeline/tensor coverage, resource/transfer
  estimates, explicit CPU/single-device/device-set bindings, authenticated
  dependency edges, collective groups/operators, and evidence explaining the
  decision.
- **FR-018**: A role assembly specification MUST bind the model-manifest digest,
  graph digest, adapter and assembler descriptor digests, layer selectors,
  per-tensor shard axis/index/count, padding/layout rules, precision,
  quantization, backend ABI, expected input/output contracts, collective
  membership, and expected peak disk/RAM/GPU/transient memory.
- **FR-019**: The exact Provider-local assembled-fragment key MUST be derived
  from model and artifact-profile identities, assembly-specification digest,
  adapter and assembler identities, backend ABI, precision/quantization, and
  protection epoch. Approximate or subset matches MUST NOT be reported as exact
  hits. The ONNX baseline MUST persist each complete role/rank assembly as one
  immutable content-addressed `.ndnsf-onnx-artifact` bundle with an embedded
  signed manifest, cataloged under a
  meaningful NDN name that binds Provider, model identity, profile, graph,
  human model name, canonical role kind plus layer-range/component coordinate,
  rank/degree, assembly recipe, and assembled-object digest. The equality key
  MUST exclude request/attempt IDs, arbitrary stage labels, and filenames. The
  bundle MUST contain inline `model.onnx` when within the implementation's
  declared safe serialization/checking bound, or `model.onnx` plus one verified
  colocated external-data entry for large models. Bundle framing MUST be
  deterministic, uncompressed, length/digest bounded, allow only the two fixed
  safe entry names, and derive the NDN/local object digest from exact final
  bundle bytes without embedding that whole-file digest in its signed manifest.
  The assembling Provider MUST sign the embedded manifest, its validated signer
  identity MUST match the Provider name prefix, and cross-Provider import MUST
  be disabled unless an explicit operator trust rule authorizes signer,
  adapter/assembler, protection domain, and ABI. The NDN name
  MUST NOT be used directly as an unsafe filesystem path.
- **FR-072**: The final deployed NDNSF-DI application SIF MUST contain ONNX
  Runtime and the declared standalone runtime support libraries, but MUST NOT
  contain or expose importable PyTorch or Transformers distributions/modules.
  PyTorch and Transformers MAY appear only in a separately sealed offline
  exporter/conformance environment and MUST NOT be copied from that environment
  into the application runtime. Runtime probes and release manifests MUST
  fail closed when either forbidden dependency is present.
- **FR-073**: After ACK_CLOSED, the trusted placement strategy MUST derive a
  complete request-scoped `RoleAssemblySpec` from the validated Provider offers
  and publish a Provider-specific sealed projection. The public application
  call MUST NOT require a precomputed stage or tensor-shard layout, and a
  Provider MUST be able to fetch canonical ONNX content and assemble its own
  selected fragment after Selection.
- **FR-074**: Provider-local pipeline and tensor partitioning MUST operate on
  the canonical ONNX graph, initializers, tensor index, and adapter-certified
  input/output/collective contracts. A Provider MUST reject mechanical or
  uncertified slicing, mismatched graph contracts, and unsupported layouts
  before activating an assembled artifact. Runtime partition/assembly code
  MUST NOT import PyTorch or Transformers.
- **FR-075**: Every supported ONNX model family MUST provide an offline
  conversion/conformance record that binds source model identity, ONNX graph
  digest, initializer/tensor index digest, tokenizer identity, adapter/assembly
  descriptor, and numerical/token parity evidence. The record MUST be separate
  from the deployed runtime lock and MUST NOT be used as evidence that the
  deployed runtime contains the exporter toolchain.
- **FR-020**: Providers MUST advertise canonical-layer, assembled-fragment, and
  loaded-runtime residency separately, including verified identity, storage or
  device tier, exact device-set identity where applicable, usable bytes,
  freshness/boot/process/topology epoch, and compatibility metadata. Each V3
  offer MUST separately state
  `ACCEPT_IF_EXACT_REUSE | ACCEPT_WITH_PREPARATION | REJECT` and
  `preparationAccepted`. Both selectable dispositions use generic ACK
  `status=true`; `REJECT` alone uses `status=false`. A Provider that refuses new
  preparation MAY therefore remain selectable only when the sealer verifies an
  exact assembled-fragment or compatible loaded-runtime proof for the proposed
  role. The trusted offer validator MUST accept only the three corresponding
  tuples `(true, exact-reuse, false)`, `(true, accepts-preparation, true)`, and
  `(false, reject, false)` and MUST reject every contradictory combination
  before ACK_CLOSED and strategy input.
- **FR-020a**: A Request MAY carry an explicit preparation policy
  `ALLOW_PREPARATION` (the default) or `EXACT_REUSE_ONLY`. When
  `EXACT_REUSE_ONLY` is requested, the strategy MUST exclude
  `ACCEPT_WITH_PREPARATION` offers and select only a Provider whose sealed
  role has an exact, verified assembled-fragment or loaded-runtime proof.
  This policy MUST NOT reinterpret a generic `status=false` ACK as selectable;
  a Provider that can serve only from an existing cache still reports
  `status=true, executionDisposition=ACCEPT_IF_EXACT_REUSE,
  preparationAccepted=false`.
- **FR-021**: After feasibility and graph-cover validation, the default strategy
  MUST use reuse as a cost signal: prefer an exact loaded runtime, then an exact
  host-memory/disk assembled fragment, then canonical layer/profile residency,
  and finally minimize missing verified bytes, transfer/queue/assembly/load time,
  and resource risk. Reuse MUST NOT define role topology, override the one-role-
  per-Provider invariant, or admit unsupported execution semantics.
- **FR-022**: `PreSplitFirstStrategy` MUST be the default for every normal public
  V3 invocation. Here "pre-split" means that the trusted Requester creates and
  validates adapter-certified split candidates *after* `ACK_CLOSED`; it does not
  mean that the application supplies a layout or that role-specific model files
  were published in advance. `LayerReuseFirstStrategy` MAY remain only as a
  subordinate cost scorer or migration implementation detail and MUST NOT be a
  public default or independently own topology. The explicit
  `PREASSEMBLED_PARTITION_SINGLE_DEVICE` V2 profile remains isolated; V3 failure
  never falls back to it and V2/V3 artifact identities never compare equal.
- **FR-023**: After Selection, each Provider MUST independently validate and
  queue its Provider-specific projection and verify model and assignment
  provenance. An exact-reuse Selection MUST pin and revalidate the catalog entry
  without fetch/assembly. A preparation Selection MUST be rejected unless its
  bound offer used `ACCEPT_WITH_PREPARATION`; when allowed, the Provider fetches
  missing canonical objects and assembles its exact fragment under bounded
  disk/RAM/network preparation leases without a GPU lease. Immediately before
  device-resident load, it MUST revalidate and atomically admit the complete
  per-device resource vector; only then may it load and declare the role locally
  ready. CPU-only roles use the same state machine with a CPU/RAM admission
  vector.
- **FR-023a**: A Provider-specific Selection carrying a structured assignment
  MUST preserve the canonical binary container through encryption, decryption,
  and local parsing. It contains exactly one `RoleAssemblySpec` and exactly one
  `RoleDataflowContract` for that Provider and Attempt. The Provider MUST NOT
  append legacy semicolon metadata (`scopeKeyData.*` or `roleProvider.*`) or
  accept a same-Provider multi-role container; malformed, duplicate-role, or
  mixed containers MUST fail closed.
- **FR-024**: Provider-local preparation MUST expose monotonic progress and the
  last verified checkpoint for manifest resolution, fetch, verification,
  assembly, load, and readiness. Progress-capable work MUST use hard and
  no-progress deadlines rather than one blind fixed wait.
- **FR-025**: A role MUST begin automatically when its Selection is committed,
  its local fragment is ready, and every required authenticated predecessor or
  collective input is ready. No global all-Provider preparation barrier or
  second start command is permitted.
- **FR-026**: True intra-layer tensor partitioning MUST include explicit runtime
  semantics for required collective or merge operations. Merely slicing weights
  and sending a sequential activation is not sufficient evidence of tensor
  parallelism.
- **FR-027**: Pipeline, tensor, and hybrid plans MUST prove exact graph coverage,
  legal cuts, compatible tensor layouts, acyclic inter-stage flow, and complete
  collective groups before final Selection.
- **FR-028**: Provider ACK acceptance MUST remain a bounded signed willingness
  and capability snapshot and MUST perform no reservation, lock, queue insertion,
  or device-state mutation. A false `preparationAccepted` field MUST NOT be
  confused with a negative generic ACK: only overall `status=false` is
  unselectable. Final Selection MAY atomically accept the complete
  Provider projection into a bounded queue, but this queue transaction MUST hold
  no GPU/device capacity. When work becomes device-admissible, a second
  just-in-time transaction MUST revalidate the offer-bound topology/resource
  facts and atomically acquire the complete device-set lease or acquire nothing.
- **FR-029**: Successfully verified canonical layers, assembled fragments, and
  eligible loaded runtimes SHOULD remain cached after a request. Eviction MUST
  be bounded, observable, identity-safe, and must never turn partial, stale, or
  corrupt state into an advertised hit. Container-private scratch MUST be
  cleaned at container exit. Cross-container reuse MUST require an explicit,
  bounded persistent cache mount with operator-defined ownership, quota,
  protection, and garbage collection; Repo durability and Provider-local cache
  persistence MUST remain distinct.
- **FR-030**: The original model publisher's signed provenance MUST be distinct
  from the layerizer's signed transformation attestation. The latter MUST bind
  source model identity, artifact-profile/tool identity, and every output layer
  digest. Providers MUST verify both required trust chains, completeness, and
  content before execution without per-packet public-key verification.
- **FR-031**: Protected models MUST use a stable model/protection-epoch security
  domain so authorized cross-request reuse is possible. The artifact policy
  authority MUST issue a signed, Provider-identity-encrypted `KeyGrantV1` bound
  to Provider, request, attempt, immutable `planCoreDigest`, model manifest,
  protection epoch,
  allowed residency tiers, issue/expiry time, and revocation sequence. ACKs,
  public manifests, and strategy inputs MUST contain no plaintext key. Expiry,
  revocation, Provider restart, plan replacement, or protection-epoch rotation
  MUST block new use, fence the old loaded-runtime identity, cancel or drain the
  affected active operation according to the signed policy, and zeroize tracked
  plaintext host and device buffers before reporting `ZEROIZED`; encrypted
  canonical objects may remain cached. NDNSF-DI MUST derive the final
  `planDigest` from canonical plan-core bytes, canonical sorted Provider grant
  name/digest bindings, and the security-policy snapshot digest; incomplete or
  invalid grant cover MUST publish no Selection. Each `GrantRequestV1` MUST bind
  a deterministic, non-executable `ProviderGrantViewV1` derived from that core;
  this view MUST NOT be accepted as Selection and MUST contain neither a wrapped
  key nor final `planDigest`.
  A durable protected assembled bundle MUST contain AEAD ciphertext rather than
  plaintext and MUST be retained/advertised only when the policy grants
  `DISK_CIPHERTEXT_ASSEMBLED`. Its per-assembly/per-entry keys MUST be domain-
  separated from the epoch content key, nonce/key pairs MUST be unique, and
  decrypted ONNX runtime files MUST remain registered plaintext leases until
  verified zeroization/removal. Whole-file identity MUST cover ciphertext.
- **FR-032**: Manifests are untrusted data. Providers MUST enforce declared
  size/shape/resource bounds, use only locally installed allowlisted adapter and
  assembler implementations, and reject executable code or paths supplied by a
  model artifact.
- **FR-033**: Every plan, Selection, preparation record, dependency object, and
  Response MUST retain one public request ID plus explicit attempt/generation
  identity and bind to the exact ACK_CLOSED, model manifest, graph, strategy,
  assembly specifications, and Provider offers. Every final V3 Selection MUST
  bind both immutable `planCoreDigest` and security-finalized `planDigest`;
  protected projections MUST also bind exactly the selected Provider's grant
  name/digest and the common security-policy snapshot digest.
- **FR-034**: Failure classification MUST distinguish graph/layerization,
  publication, manifest activation, resolution, fetch, integrity,
  authorization, assembly, load, dependency, collective, execution, and
  response boundaries without collapsing them into a generic timeout.
- **FR-035**: The feature MUST remain model-family-neutral. Model-specific
  layer discovery, tensor slicing, collective rules, preprocessing, and runners
  MUST enter through digest-pinned adapter ports owned by NDNSF-DI.
- **FR-036**: Because final role IDs and shard counts do not exist before
  planning, Provider offers MUST express bounded role capability classes and
  predicates (supported layer/tensor axes, ranges, group sizes, collectives,
  backends, and resource envelopes) rather than require enumeration of every
  possible final role. Selection MUST validate the exact assignment against
  those signed predicates.
- **FR-037**: A tensor collective group MUST enter a collective epoch only when
  every selected member is locally ready and the group's authenticated stage
  input is ready. This group-local synchronization MUST NOT become a global
  all-Provider model-ready barrier.
- **FR-038**: Reusable execution state, including sharded KV or other model-
  family state, MUST bind to the exact model, assembly, tensor-rank/group,
  adapter, semantics, request-prefix where applicable, and protection identity;
  incompatible state MUST NOT cross-hit merely because layer bytes match.
- **FR-039**: External placement strategies MUST select only adapter-certified
  layer/tensor partition recipes or compose them under adapter-validated rules.
  A strategy result is declarative data and MUST NOT supply executable slicing
  or assembly code to a Provider.
- **FR-040**: Canonical weight equality MUST use a specified normalization of
  the tensor map, including tensor names, dtypes, shapes, byte order, shared/tied
  references, and floating-point representation. Archive order, checkpoint
  filenames, source sharding, and packaging timestamps MUST NOT change model
  identity.
- **FR-041**: A loaded-runtime identity MUST additionally bind the assembled-
  fragment identity, runner and kernel/compile profile, backend/runtime and
  driver compatibility, every device identity/architecture in the bound device
  set, device-set/topology digest, Provider boot epoch, process/runtime
  generation, and reusable-state contract. A stale process, missing device, or
  changed topology MUST NOT be advertised as accelerator-ready.
- **FR-042**: ACK cache inventory MUST have a bounded wire representation and an
  exact verification path, such as a catalog/profile digest plus verified layer
  ranges/bitmap or an inventory root with proofs. Probabilistic summaries MAY be
  hints but MUST NOT establish an exact cache or loaded-runtime hit.
- **FR-043**: Provider preparation MUST single-flight concurrent fetches by
  canonical object digest and concurrent builds by assembly-specification
  digest. It MUST use private temporary state followed by complete verification
  and atomic activation; cancellation or crash MAY retain fully verified
  reusable objects but MUST quarantine, resume, or remove incomplete state.
- **FR-044**: Tensor collective execution MUST bind authenticated rendezvous,
  group/member/rank and epoch identity, deterministic operation ordering,
  timeout/cancellation, and whole-group failure propagation. Replanning MUST
  replace/version the complete affected group rather than silently substitute
  one rank inside a live epoch.
- **FR-045**: Immutable base-model layers, immutable adapter/LoRA overlay stacks,
  and mutable request/session state such as KV cache MUST have separate
  manifests and identities. Assembly MAY compose a pinned overlay stack, but
  neither overlays nor mutable state become part of canonical base-layer
  equality.
- **FR-046**: Accelerator allocation MUST remain outside the Provider protocol.
  On a scheduled cluster, the job scheduler allocates devices and the container
  runtime exposes only that allocation; the Provider MUST discover its actual
  runtime-visible resources and MUST NOT attempt to allocate additional cluster
  GPUs from inside the container.
- **FR-047**: Provider accelerator configuration MUST support `AUTO`, `NONE`,
  and `EXPLICIT_SUBSET` semantics. Configuration MAY disable or restrict
  runtime-visible devices but MUST NOT create devices, memory, connectivity, or
  capabilities not observed by the Provider. Explicit device references MUST
  resolve against container-visible identities rather than assume host GPU
  ordinals; an unresolved `EXPLICIT_SUBSET` reference MUST fail startup or the
  configuration transaction instead of being silently ignored.
- **FR-048**: A signed Provider offer MUST represent each visible accelerator
  separately, including an offer-scoped device handle, stable hardware identity
  where disclosure policy permits, type/architecture, supported backend and
  precision capabilities, total/free/reservable memory, health, and allocation
  epoch. It MUST also represent relevant inter-device and CPU/NUMA topology,
  peer access, and interconnect class/bandwidth; one aggregate GPU-memory scalar
  is insufficient planning evidence.
- **FR-049**: Zero visible accelerators MUST be a valid Provider state. Such a
  Provider MUST advertise CPU/RAM/storage/network capabilities and MUST be
  eligible for storage, transfer, layerization/assembly, pre/post-processing, or
  adapter-certified CPU execution roles. Accelerator-required roles MUST
  exclude it, and CPU fallback MUST be an explicit task/plan policy rather than
  a silent runtime substitution.
- **FR-050**: Every selected role MUST carry one declarative Provider-local
  `DeviceBinding` with mode `CPU` or `SINGLE_DEVICE`, its offer-scoped handle
  when applicable, one phase-specific resource envelope, and the exact signed
  offer, topology, and resource-sequence digests used by planning. A role MUST
  NOT span Providers or hide several tensor ranks in a `DEVICE_SET` binding.
- **FR-051**: Within one request Attempt, Provider-role assignment MUST be
  one-to-one: every execution role is owned in full by exactly one Provider and
  every selected Provider owns exactly one role. A tensor-parallel stage with
  degree `M_i` therefore creates `M_i` distinct roles (for example `S0R0` and
  `S0R1`) and requires `M_i` distinct Providers. A Provider with several visible
  devices may use the unused devices for other concurrent requests/Attempts,
  but not for additional roles in the same Attempt.
- **FR-052**: Feasibility and admission MUST be evaluated per device and per
  link, not by summing accelerator memory. Multiple devices MUST NOT satisfy one
  unsplittable single-device peak. Inter-Provider network capability and the
  sealed NDN dataflow bounds MUST independently satisfy every group edge.
- **FR-053**: Before admission, preparation, and loaded-runtime reuse, a Provider
  MUST revalidate the `DeviceBinding` against current visibility, health,
  resource sequence, and topology digest. Device loss, MIG/topology change, or
  stale allocation MUST fail the exact role or entire affected collective group
  without silent remapping; any replacement requires a newly sealed attempt or
  plan under the original public request ID.
- **FR-054**: Scheduler/container isolation, not an environment variable or
  Provider configuration, is the authority for device access. Signed offers and
  Selections MUST use topology-bound, offer-scoped handles so a Selection cannot
  name an unoffered device and need not expose host-global device identifiers.
- **FR-055**: A Provider-local scheduler MUST enforce each selected binding,
  per-device capacity, queue bounds, and concurrency. It MAY optimize transfers
  and execution within that binding, but MUST NOT independently change global
  shard ranks, device-set membership, dependency edges, or collective semantics
  chosen by the sealed placement plan.
- **FR-056**: Planning MUST represent every pipeline stage/rank pair as a
  distinct execution role. Each role binds one Provider, one CPU or single-
  device target, one exact shard/slice, one resource envelope, and its group and
  dependency membership. A collective group is metadata connecting several
  roles; it is not a logical role that spans Providers.
- **FR-057**: Selection and just-in-time admission are atomic for one complete
  role on one Provider. Collective readiness is group-local protocol state and
  MUST NOT reserve or pool remote Provider resources. Failure of any required
  member aborts the affected group epoch and prevents partial downstream output.
- **FR-058**: Each device offer MUST declare its sharing and failure/isolation
  domain, including exclusive device, MIG partition, MPS, or time-shared mode
  where supported, plus parent-device relationships. The initial compatibility
  profile MAY accept only exclusive devices; shared modes require an explicit
  capability, concurrency envelope, and isolation policy and MUST NOT be
  inferred from visibility alone.
- **FR-059**: A per-device resource envelope MUST separately bound weight and
  replicated bytes, activation, KV/state, collective workspace, assembly/load
  transient peak, and supported batch/sequence envelope. Feasibility MUST use
  these phase-specific vectors rather than one total-memory estimate.
- **FR-060**: Provider resource evidence MUST separate a relatively stable
  `DeviceTopologyProfile` from a mutable `DeviceResourceSnapshot`. The profile
  binds architecture, connectivity, peer access, collective capabilities, and
  sharing/failure domains; the snapshot binds health, free/reservable capacity,
  queue, allocation epoch, capture time, and resource sequence. A bounded ACK
  MAY carry summaries plus signed digests/references, but Selection and
  revalidation MUST bind the exact offer, profile, and snapshot used to plan.
- **FR-061**: A hybrid plan MUST encode an independent tensor degree `M_i >= 1`
  for every pipeline stage rather than impose one global tensor degree. A stage
  with `M_i = 1` MUST remain an ordinary unsplit pipeline role. Every boundary
  where `M_i != M_(i+1)` OR the declared producer and consumer tensor layouts
  are not directly compatible MUST carry an adapter-certified authenticated
  gather/scatter/reshard contract, source/target layout, producer/consumer rank
  map, and completion rule. Equal rank counts alone MUST NOT suppress a required
  layout conversion; unsupported degree/layout transitions MUST make the plan
  infeasible before Selection.
- **FR-062**: A stage tensor degree defines its participating rank count but MUST
  NOT imply that every layer parameter, state tensor, or activation is
  mechanically divided into that many equal slices. The adapter-certified role/
  rank recipe MUST classify each required tensor as `SHARDED` with explicit
  axis/layout, `REPLICATED`, `OWNER_ONLY`, or `LOCAL_DERIVED`, and MUST prove
  complete, non-conflicting coverage before Selection.
- **FR-063**: An external `ModelPlacementStrategyV3` MUST receive only the
  immutable request, certified graph/adapter catalogs, ACK_CLOSED evidence, and
  sanitized `ProviderPlanningViewV3` values derived from validated signed offers
  and MUST return a declarative
  `PlacementProposalV3`. It MUST NOT emit wire Selection messages, bypass offer
  or resource validation, or supply executable Provider code. A trusted
  NDNSF-DI `PlanSealerV3` MUST independently canonicalize and validate the
  proposal, prove graph/rank/tensor/dependency/resource/security invariants, and
  generate immutable `PlacementPlanCoreV3`; after complete protected-Provider
  grant acquisition it MUST security-finalize `PlacementPlanV3` and only then
  generate Provider-specific Selection projections. An invalid proposal or
  incomplete grant cover MUST fail before Selection.
- **FR-064**: V3 custom strategy implementations MUST be explicitly installed
  by the Requester operator and are trusted with respect to the Requester
  process; loading adversarial strategy code from a request or network object is
  unsupported. Strategy execution MUST remain bounded by the planning deadline
  and candidate budget, and exception/timeout/cancellation MUST produce a
  classified planning failure or permitted built-in fallback without publishing
  Selection. Regardless of implementation trust, every strategy output MUST be
  treated as untrusted input to `PlanSealerV3`. Supporting adversarial plugins
  later MUST require a separate out-of-process sandbox profile.
- **FR-065**: The V3 runtime MUST expose separate queue and device-admission
  states: `SELECTION_VALIDATED -> QUEUE_ACCEPTED -> HOST_PREPARING/HOST_READY ->
  DEVICE_ADMISSION_PENDING -> DEVICE_ADMITTED -> LOADING -> LOCAL_READY`. Queue
  expiry, cancellation, capacity loss, stale topology, and admission failure MUST
  have distinct terminal/replan outcomes. A monotonic fencing token created only
  by successful device admission MUST bind every subsequent load, collective,
  execution, release, and failure record.
- **FR-066**: The normal `Application`/`APPClient` V3 path MUST instantiate
  `PreSplitFirstStrategy`, ensure only canonical model/layer artifacts on the
  Requester, and carry one declarative `RoleAssemblySpec` plus one
  `RoleDataflowContract` in each Provider-specific final Selection.
  It MUST NOT invoke the legacy Requester-side selected-role split materializer.
  Provider Python and native handlers MUST consume the same sealed projection and
  perform assembly locally. The legacy `_prepare_artifacts()` role-split path MAY
  remain reachable only through the explicit V2 profile.
- **FR-067**: Python and native Provider offer generation MUST serialize the
  same V3 topology/profile/snapshot, residency-proof references, capability
  predicates, and `ackReservation=false` semantics. `NativeProviderReadiness`,
  the native executable entry point, build/install manifests, and exact SIF MUST
  use the real runtime probe rather than fixture-only JSON or Python-only wiring.
  Every compiled Python extension shipped in the SIF MUST be built inside the
  candidate SIF build stage, or a sealed builder rootfs proven ABI-identical to
  it, using the target Python executable, headers, `SOABI`, `EXT_SUFFIX`,
  compiler/glibc, and native dependency roots. Host Python headers, virtual
  environments, site-packages, or native libraries MUST NOT satisfy this build.
- **FR-068**: Every cross-Provider pipeline, tensor, redistribution, and
  collective payload MUST use consumer-pull NDN Interest/Data. Each selected
  role receives a sealed `RoleDataflowContract` containing exact `mayPublish[]`,
  `mustFetch[]`, and `waitFor[]` entries. Tensor names and signed manifests bind
  producer namespace, requester/request ID, attempt, plan digest, group/epoch,
  operation/round, source role, tensor identity/digest, microbatch, and segment.
  A producer output shared by multiple roles MUST have one immutable object
  identity and one publication; its manifest binds the complete authorized
  consumer-role set, and every consumer projection fetches that same name.
  Consumers express Interests only for their declared `mustFetch` names;
  producers publish Data only under `mayPublish`. Large tensors use a signed
  `TensorObjectManifest` and bounded segmented Data, and retransmission reuses
  the same name. Authentication/confidentiality, duplicate/replay handling,
  no-progress deadline, cancellation, and whole-epoch failure are mandatory.
  Raw TCP/RPC/RDMA/NCCL payload channels, shared files, and unnamed side channels
  between Providers are forbidden.
- **FR-069**: Protected runtime state MUST follow the auditable state machine
  `NO_GRANT -> GRANT_VERIFIED -> HOST_PLAINTEXT_LEASED ->
  DEVICE_PLAINTEXT_LEASED -> DRAINING -> ZEROIZED`, with failure transitions to
  `REVOKED` or `FAILED_CLOSED`. Every plaintext allocation MUST be registered
  before exposure and removed from the registry only after zeroization is
  confirmed or the process/device context is destroyed and fenced.
- **FR-070**: All executable source, security logic, build/install declarations,
  experiment harnesses, model/artifact preparation, and local Gate A/B/C checks
  MUST complete before the formal candidate freeze. The freeze record MUST bind
  their hashes. After freeze, formal tasks MAY only execute that immutable
  candidate and write evidence or explanatory documentation that cannot affect
  candidate behavior; any executable change invalidates the candidate and
  returns to the owning pre-freeze task.
- **FR-071**: The pre-freeze real MiniNDN gate MUST include three independent
  Provider processes and three concurrent V3 invocations, plus a matched
  repeated-request sequence. It MUST exercise the normal Application default,
  native/Python offer parity as applicable, real Controller/Repo/security paths,
  request-ID continuity, queue acceptance, progress-driven preparation,
  just-in-time admission, data-driven execution, and complete multi-token
  Responses before TigerCluster authorization.

### Normative Artifact Name Example

For a human alias `Qwen/Qwen3-0.6B`, the readable components identify the model
family/release, while digests decide equality:

```text
/<publisher>/NDNSF-DI/MODEL/v1
  /NAME/Qwen/Qwen3-0.6B
  /MID/<complete-model-identity-digest>
  /PROFILE/<layerizer-serialization-chunking-protection-digest>
  /LAYER/transformer/000012/MANIFEST/<layer-manifest-digest>
  /OBJECT/<object-digest>/<segment-number>
```

The signed model manifest contains the full parameter map and maps this layer
coordinate to its layer manifest and chunk/tensor index. A source revision or
the string `0.6B` is informative only; neither can replace the content and
configuration digests.

For a protected profile, the readable `Qwen/Qwen3-0.6B` path and globally
correlatable source digest may be replaced by policy-scoped opaque components;
the signed, authorized manifest still binds them to the exact model internally.

### Normative End-to-End Flow

```text
Deployment boundary
  -> scheduler/runtime allocates and exposes 0, 1, or N accelerators
  -> Provider probes actual CPU/RAM/storage and per-device accelerator topology
  -> local AUTO/NONE/EXPLICIT_SUBSET policy may only restrict that observed view
Application
  -> Request(model identity, task, input, options, preparation policy, deadline)
  -> Providers return signed ACK offers
       CPU/RAM/storage + per-device memory/health + interconnect topology
       stable topology-profile digest + mutable resource-snapshot sequence
       allocation/queue + RTT/bandwidth
       supported adapters/backends/partition axes/collectives
       canonical-layer + assembled-fragment + loaded-runtime residency
       execution disposition: exact-reuse-only | accepts-preparation | reject
  -> ACK_CLOSED (immutable)
  -> NDNSF-DI inspects the pinned dependency graph
  -> PreSplitFirstStrategy creates feasible adapter-certified split candidates,
       then chooses one-to-one role/Provider assignments using reuse as a cost
       signal; every tensor rank is a distinct role
  -> Requester-side layerizer ensures one compatible canonical artifact profile
       and model/layer set in Repo
       existing verified objects: reuse
       missing objects: single-flight/idempotent publish
       verify origin + transformation attestations; activate root manifest last
  -> seal immutable plan core against ACK_CLOSED + model manifest + graph +
       strategy + offers
  -> for protected Providers, acquire KeyGrants bound to planCoreDigest
  -> finalize planDigest from core + sorted grant bindings + policy snapshot
  -> final Selection carries each Provider's one RoleAssemblySpec and one
       RoleDataflowContract and, for a protected profile, only an
       authorization-bound KeyGrant reference
  -> each Provider independently
       validates the complete projection and atomically accepts it into a bounded
       queue without acquiring GPU/device capacity
       verifies assignment/manifests/key grant, then reuses an exact local
       fragment or, only when the offer accepted preparation, fetches missing
       layers and assembles one immutable role/rank ONNX artifact bundle host-side
       immediately before device load, revalidates its single CPU/device binding
       and atomically admits the complete role resource vector
       loads under the admission fencing token and reports monotonic progress
  -> pipeline role starts when local-ready AND authenticated inputs are ready
  -> tensor group starts one collective epoch when all group ranks and its
       authenticated input are ready; no unrelated/global readiness barrier
  -> every cross-Provider edge runs as declared consumer-pull NDN Interest/Data;
       no hidden socket, shared-file, or push channel is permitted
  -> one authenticated Response
  -> verified layers/fragments and eligible live runtimes remain cached and
       appear separately in later ACK offers
```

### Key Entities

- **ModelIdentity**: Immutable origin/provenance, canonical weight,
  parameter/configuration, semantics, graph, and tokenizer/preprocessing
  identities; human alias and source revision remain descriptive metadata.
- **CanonicalArtifactProfile**: Exact layerizer/adapter, serialization schema,
  chunking/layout, base precision/format, and protection transformation used to
  represent one model as canonical layers.
- **CanonicalModelManifest**: Signed, atomically activated root that enumerates
  every required logical component, origin provenance, transformation
  attestation, artifact profile, and cryptographic identity.
- **CanonicalLayerManifest**: One logical layer/region's semantic coordinate,
  graph membership, tensor/chunk index, I/O contract, and content digests.
- **LayerObject**: Immutable Repo content referenced by a layer manifest.
- **ProviderOffer**: Signed ACK-time willingness, capacity, compatibility,
  network/queue estimates, `ProviderResourceTopology` profile/snapshot digests,
  bounded exact three-level residency inventory, and the independent
  exact-reuse/preparation/reject disposition. It is observational and reserves
  no resource.
- **ProviderResourceTopology**: One signed snapshot of runtime-visible CPU,
  memory, storage, accelerator devices, inter-device/NUMA links, local capacity,
  allocation provenance, health, and resource sequence, represented as a stable
  topology profile plus a mutable resource snapshot. Zero accelerators is a
  valid topology.
- **ComputeDeviceOffer**: One accelerator's offer-scoped handle, optional stable
  hardware identity, architecture/capabilities, supported runtimes/precisions,
  total/free/reservable memory, sharing/failure domain, health, and allocation
  epoch.
- **AcceleratorRequirement**: Task/role policy declaring whether accelerator use
  is required, preferred, allowed, or forbidden; it prevents silent CPU/GPU
  substitution.
- **PartitionPlan**: Complete pipeline/tensor/hybrid role topology and evidence.
- **ExecutionRole**: One complete executable pipeline-stage/rank unit. A tensor
  rank is a role, not part of a role spanning several Providers.
- **RankAssignment**: The rank metadata of one execution role, including its one
  Provider, device handle, shard/slice, resource envelope, and group membership.
- **RoleAssemblySpec**: Exact deterministic recipe selecting canonical layers
  and tensor slices for one role, including backend and collective contracts.
- **DeviceBinding**: Selection-time CPU or single-device assignment for one role,
  with an offer-scoped handle, phase-specific budget, sharing policy, and bound
  offer/topology/resource digests.
- **RoleDataflowContract**: The sealed per-role list of exact tensors the role may
  publish, must fetch, and must wait for, including NDN names, producers,
  consumers, group/epoch/operation identities, manifests, segmentation, security,
  deadlines, and completion rules.
- **AssembledFragmentIdentity**: Exact derivation key and meaningful NDN catalog
  name for Provider-local reusable bytes before a live runtime/device binding;
  the ONNX baseline maps it safely to one immutable assembled bundle with an
  embedded signed manifest and bounded inline or external-data ONNX layout.
- **LoadedRuntimeIdentity**: Exact live Provider/process/device/runtime instance
  that can execute without rebuilding or reloading the assembled fragment.
- **OverlayManifest**: Immutable adapter/LoRA composition artifact kept separate
  from canonical base-model layers and mutable request/session state.
- **PreparationProgress**: Monotonic checkpoints across resolve, fetch, verify,
  assemble, load, ready, fail, cancel, and evict.
- **CollectiveGroup**: NDNSF-DI-owned tensor-parallel membership, operator,
  tensor contract, ordering, and completion rule.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Two different valid placement plans for the same immutable model
  reference exactly one canonical model manifest and publish zero duplicate
  canonical layer bytes.
- **SC-002**: Changing only Provider assignment, pipeline boundary, or tensor
  shard count changes role assembly identities but leaves canonical layer
  identities unchanged.
- **SC-003**: Reusing an exact GPU-ready loaded runtime causes zero Repo model-
  byte transfer, zero fragment assembly, and zero model reload for that Provider
  on the later invocation.
- **SC-004**: For a partially cached Provider, measured Repo bytes equal only
  missing verified canonical objects, with no request-specific global shard
  publication.
- **SC-005**: A small inspectable reference model completes authenticated
  pipeline-only, tensor-only, and hybrid executions with full outputs matching
  an unsplit oracle under a predeclared numerical tolerance and with no CPU
  fallback when the plan requires accelerators.
- **SC-006**: Let `eligible_at` be the later of `LOCAL_READY` and the final
  authenticated direct-input/group-ready event, and let `started_at` be the
  first runner/collective operation. Every admitted role/group starts on the
  same or next recorded scheduler wake and within
  `configured_scheduler_tick_ms + 5 ms` of `eligible_at`; no trace contains a
  global model-ready barrier or second execution-start command. The configured
  tick and monotonic-clock error budget are frozen before execution.
- **SC-007**: A real MiniNDN topology with three independently running eligible
  Providers completes three concurrent V3 invocations through the normal
  Application default without ACK-time reservation or GPU locking. Each retains
  an independent request/attempt/plan identity and bounded queue/device
  accounting, while identical object fetches and fragment builds occur once per
  Provider. A one-Provider simulated multi-device test does not satisfy SC-007.
- **SC-008**: Corrupted chunks, stale or incomplete manifests, invalid origin or
  transformation attestations, wrong adapters, incompatible tensor groups,
  rank loss, and alias collisions are rejected before unsafe model execution in
  100% of negative tests.
- **SC-009**: Every injected publication/fetch/assembly/load/collective failure
  reports its narrow lifecycle class, original request identity, and last valid
  progress checkpoint; no accepted result is represented only as a generic
  timeout.
- **SC-010**: Before any TigerCluster or large-model campaign, a repeated-request
  real MiniNDN gate with the minimal real model runs, for every locked prompt and
  clean-start block, one cache-reset measured cold request, one unmeasured warmup,
  and five measured warm requests. Cold and warm rows are never pooled; every
  exact warm loaded-runtime hit has zero Repo model bytes, zero assembly, and
  zero reload.
- **SC-011**: Exact-container/SIF parity proves the same manifests, assembly
  identities, single assembled-bundle inline/large-external-data layouts,
  adapter/assembler ABI
  checks, scratch cleanup, explicit persistent-cache-mount behavior, and failure
  behavior before remote execution. It also records matching build/runtime
  Python ABI facts and rejects host-built or host-mounted Python extensions,
  including extensions whose filename tag appears compatible but whose build
  environment is outside the sealed candidate.
- **SC-011a**: A formal Tiger release is accepted only when the SIF is paired
  with a `ndnsf-local-sif-build-v3` record whose boundary report states
  `containerNativeBuild=true`, `hostBinaryInputs=[]`, and stale-base
  replacement, and whose recorded SIF size and SHA-256 match the promoted
  bytes. The record gate runs before node-local staging; a legacy record or a
  later import/`ldd` pass cannot qualify a host-built extension.
- **SC-012**: Repacking the same normalized tensor map with different checkpoint
  filenames or archive order leaves ModelIdentity unchanged, while changing the
  canonical artifact profile changes profile/layer-object identities without
  falsely changing the source model identity.
- **SC-013**: Protected-profile tests prove opaque naming, authorization expiry
  and revocation, key-epoch/KDF/nonce separation, ciphertext-only durable
  assembled storage, and defined host/device/materialized-file plaintext cleanup
  without allowing an old loaded runtime to remain an exact hit.
- **SC-014**: The same Provider artifact starts successfully with zero, one, and
  two runtime-visible accelerators; each signed offer reports exactly that
  visible set, while `NONE` and `EXPLICIT_SUBSET` configurations only reduce it.
- **SC-015**: In 100% of feasibility tests, two 12-GiB devices do not satisfy an
  unsplittable 20-GiB single-device role. Two roles for the same Attempt are not
  assigned to that one Provider merely because both devices are visible.
- **SC-016**: A tensor degree of two produces two execution roles on two distinct
  Providers. Mutation tests that assign both roles to one Provider or one role
  to two Providers are rejected before Selection.
- **SC-017**: Removing, renumbering, degrading, or changing one selected device
  after ACK causes the stale Selection or affected collective group to fail
  closed in every injected test, with no silent remap or CPU fallback.
- **SC-018**: An exact loaded-runtime warm hit occurs only when the exact
  CPU/single-device binding, topology digest, boot/process/runtime generations, assembly, and
  reusable-state contract match; otherwise only the still-valid disk/RAM
  artifact level may be reused. Tests also prove that
  `ACCEPT_IF_EXACT_REUSE` with `preparationAccepted=false` succeeds only for an
  exact sealer-verified assembled/runtime identity, transfers/builds zero model
  bytes, and that an overall ACK `status=false` is never selected.
- **SC-019**: Before any multi-GPU model campaign, exact-container/SIF gates on
  TigerCluster prove that Slurm allocations of zero, one, and two GPUs produce
  matching Provider offers inside the container and that unallocated devices
  are never advertised or selected.
- **SC-020**: Under concurrent requests, each Provider admits at most one complete
  role per Attempt and never holds a partial role or remote resource; fault
  injection observes no hold-and-wait deadlock or partial collective epoch.
- **SC-021**: Validation accepts a tensor group only when every rank is a distinct
  role, every role maps to a distinct Provider, and all authenticated NDN
  dataflow/group contracts form exact coverage. It rejects duplicate, missing,
  incompatible, orphan, multi-role-Provider, and cross-Provider-role assignments
  in 100% of negative cases.
- **SC-022**: Exclusive, MIG, MPS, and time-shared device offers are never
  treated as interchangeable. Any sharing mode outside the admitted
  compatibility profile is rejected before Selection, and parent/failure-domain
  loss invalidates every affected binding.
- **SC-023**: Phase-specific per-device envelopes reject a plan whose steady
  weights fit but whose activation, KV/state, collective workspace, or
  assembly/load transient peak exceeds any selected device; accepted plans stay
  within every declared envelope in measurement.
- **SC-024**: A bounded ACK topology summary can be resolved to and verified
  against the exact signed topology profile and resource snapshot; stale
  profile, snapshot, offer, or resource-sequence substitution is rejected in
  every negative test.
- **SC-025**: Heterogeneous hybrid reference plans with degree vectors such as
  `[1, 2, 1]` and `[2, 1, 2]` execute complete outputs matching the unsplit
  oracle within the declared tolerance; traces show tensor collectives only in
  stages whose degree exceeds one and explicit redistribution exactly where
  adjacent degree or declared tensor layout requires it.
- **SC-026**: A sharded-stage fixture containing sharded attention/MLP weights,
  replicated normalization state, and one rank-owned tensor is assembled and
  executed according to its certified per-tensor recipe; mechanical equal
  slicing, missing coverage, or duplicate ownership is rejected in every
  negative case.
- **SC-027**: Mutation tests alter an external strategy proposal to contain an
  unoffered device, stale offer, incomplete rank/tensor cover, incompatible
  layout transition, resource overflow, or executable/opaque assignment data;
  `PlanSealerV3` rejects every mutation before any Selection is published, while
  semantically identical valid proposals and grant sets produce identical
  deterministic core and final plan identities; substituting one grant changes
  or invalidates the final identity.
- **SC-028**: A custom strategy receives no raw wire signature/proof object or
  live Provider/runtime handle; its bounded timeout, exception, cancellation,
  and candidate-budget exhaustion paths publish no Selection and retain a
  classified planning outcome. An attempt to load strategy code from request or
  network content is rejected by configuration/API policy.
- **SC-029**: Python and native V3 ACK paths both report
  `ackReservation=false`, create no reservation-book/lease/queue entry, and emit
  equivalent signed topology/profile/snapshot, capability, disposition, and
  `preparationAccepted` fields. Both accept only the three normative status/
  disposition/preparation tuples. Explicit
  V2 tests retain their former reservation behavior without making it reachable
  from a V3 invocation.
- **SC-030**: An unmodified normal public Application call selects
  `PreSplitFirstStrategy`, publishes/ensures only canonical artifacts on the
  Requester, and causes every selected Provider to fetch/assemble/load its own
  projection. Instrumentation observes zero calls to the legacy Requester-side
  role-split materializer for V3 and observes that path only in an explicit V2
  compatibility test.
- **SC-031**: Cross-Provider consumer-pull NDN dataflow tests accept every valid
  signed manifest/segment sequence and reject undeclared publish/fetch names,
  wrong peer/capability/epoch,
  key/nonce reuse, plaintext wire content, replayed or conflicting segment,
  oversized declaration, operation reordering, no-progress, and cancellation
  mutations with zero partial downstream output
  over the declared deterministic fault corpus.
- **SC-032**: The formal candidate manifest proves that all executable source,
  security, build/install declarations, harnesses, artifact preparation, and
  local Gate A/B/C results precede one freeze timestamp. Every post-freeze formal
  run uses matching hashes; any mismatch is recorded as `INVALID_CANDIDATE`, not
  repaired in place.
- **SC-033**: Every V2 invocation requires an explicit
  `PREASSEMBLED_PARTITION_SINGLE_DEVICE` profile and produces V2-only telemetry;
  V3 failure never auto-falls back to V2, and V2/V3 plan, cache, residency, and
  evidence identities never compare equal.
- **SC-034**: Publication-quality cold/warm evidence contains three complete
  clean-start blocks. The primary performance corpus has five locked prompts;
  each prompt contributes one measured cold and five measured warm requests per
  block. Reports use the declared hierarchical bootstrap unit/iterations,
  paired estimand/effect size, equivalence margin, Holm comparison family, and
  exact failure intervals. Fewer than three complete blocks is labelled
  exploratory and cannot close the criterion.
- **SC-035**: Hybrid tests execute the frozen role vectors with one distinct
  Provider per stage/rank role: `[1,2,1]` uses four Providers for
  `{S0R0,S1R0,S1R1,S2R0}` and `[2,1,2]` uses five Providers for
  `{S0R0,S0R1,S1R0,S2R0,S2R1}`. Every cross-Provider activation,
  redistribution, and collective tensor is fetched through its declared NDN
  name. Any Provider reuse within one Attempt, role spanning Providers,
  undeclared channel, insufficient envelope, or incomplete role/dataflow cover
  leaves the claim `BLOCK`.
- **SC-036**: The final application SIF passes both a static file/distribution
  scan and an in-container import probe showing that PyTorch and Transformers
  are absent, while ONNX Runtime and standalone tokenization remain usable. The
  same negative result is required from the local build gate and the exact SIF
  gate; an exporter/development image does not qualify as deployment evidence.
- **SC-037**: For two different authenticated ACK snapshots, the placement
  layer produces different request-scoped `RoleAssemblySpec` values when
  Provider capabilities differ, while canonical model/object identities remain
  unchanged. Selected Providers fetch only the ranges named by their sealed
  specs and locally assemble valid ONNX stages; no precomputed final role layout
  is treated as the V3 default.
- **SC-038**: Each supported ONNX model family has an offline conversion and
  conformance record binding its source revision to graph, initializer,
  tokenizer, and adapter digests, plus output/token parity evidence. This record
  is separate from the deployment lock and cannot be replaced by a fixture-only
  ONNX smoke or a Transformers runtime result.

## Assumptions and Dependencies

- Spec 163 remains the authority for deferred collaboration planning,
  ACK_CLOSED binding, final Selection, Provider offer validation, and
  data-driven role execution. This feature revises its role-scoped artifact
  assumptions; it does not move DI semantics into NDNSF Core.
- Spec 164 remains the authority for large immutable artifact publication,
  signed root manifests, digest-bound bulk verification, progress, range
  transfer, and repository throughput. This feature supplies a model-layer
  schema on top of that transport.
- Existing pre-split role artifacts remain readable through the permanent,
  explicit V2 `PREASSEMBLED_PARTITION_SINGLE_DEVICE` compatibility profile for
  the scope of Spec 170. It is never the V3 default or automatic fallback;
  removal requires a future breaking-change specification.
- A model-family adapter can identify stable logical layers or expose the model
  as one opaque atomic region. Tensor parallelism is available only when the
  adapter supplies legal per-tensor partition and collective semantics.
- Canonical layer storage reduces duplicate persistent bytes; it does not
  guarantee minimal network bytes for every strided tensor slice. Verified
  selective retrieval is mandatory when required for feasibility; additional
  range-layout efficiency remains an optimization to be measured separately.
- Provider-local assembled fragments may be expensive and backend-specific;
  they are reusable only under exact derivation and ABI equality.
- Cluster resource allocation is an operator/orchestrator concern. For
  TigerCluster, Slurm selects the GPU count/type and Apptainer exposes the
  allocation to the SIF; NDNSF-DI observes and plans over that view but does not
  request new GPUs from inside the Provider.
- A single Provider identity may own multiple visible devices, but one Attempt
  may select only one complete role on that Provider. The selected role uses one
  CPU or single-device binding; other devices remain available for independent
  requests rather than becoming hidden roles or ranks.
- Validation defaults to a minimal real Qwen model under real MiniNDN plus
  exact-container parity. TigerCluster is a later deployment-fidelity gate;
  larger models are not the first correctness test.

## Out of Scope

- Changing NDNSF Core wire messages to understand models, layers, tensors, or
  collectives.
- Treating a human model name, advertised parameter count, or source revision as
  sufficient artifact identity.
- Globally publishing every Provider's transient compiled/quantized fragment by
  default.
- Claiming tensor parallelism from weight slicing without runtime collective or
  merge semantics.
- Rebuilding TigerCluster images, preparing large models, or launching a remote
  campaign as part of this design-only specification.
- Replacing Spec 168's current immutable deployment-fidelity candidate or
  reinterpreting its retained experiment evidence.
