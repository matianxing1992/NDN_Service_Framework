# Feature Specification: TigerCluster NDNSF-DI Deployment Fidelity

**Feature Branch**: `168-itiger-di-deployment-fidelity`

**Created**: 2026-08-03

**Status**: Draft

**Input**: User description: "在 TigerCluster 的真实多节点 GPU 部署环境中，以部署保真为目标，验证 NDNSF-DI 从用户请求、ACK 收集、模型分割与角色规划、DistributedRepo 模型获取、Provider 准备、数据依赖驱动的分阶段执行到最终 Response 的完整生命周期。通过真实的 NDN 网络、进程、GPU、内存、磁盘和模型适配器条件，发现、定位并修复 NDNSF-DI 相关逻辑缺陷，并以可重复证据证明多节点分布式推理正确工作。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Complete a deployment-faithful invocation (Priority: P1)

As an NDNSF-DI researcher, I can submit one real generation request to a clean
TigerCluster allocation and observe one complete, secured, multi-node lifecycle
from the original Request through the final Response.

**Why this priority**: A registered repository, successful ACKs, selected roles,
downloaded files, or one executed stage are intermediate milestones. Only a
terminal Response proves that the distributed service invocation completed.

**Independent Test**: Use a pinned small model and three real GPU nodes to run
one bounded, multi-token request. Independently reconstruct the lifecycle from
retained wire, planning, preparation, stage, and response evidence.

**Acceptance Scenarios**:

1. **Given** a clean three-node allocation and immutable runtime, model, graph,
   strategy, and prompt identities, **When** the User submits one request,
   **Then** the same request identity appears in every ACK, plan, role,
   preparation operation, stage event, token record, and terminal Response.
2. **Given** three selected Provider roles, **When** each role becomes prepared
   and receives all predecessor data, **Then** it starts without waiting for a
   global readiness barrier or an operator start command.
3. **Given** GPU acceptance mode, **When** the request terminates, **Then** the
   response contains a complete ordered answer and the evidence records zero
   CPU fallback.

---

### User Story 2 - Distinguish cold preparation from warm reuse (Priority: P2)

As an NDNSF-DI researcher, I can submit repeated requests for the same immutable
model and determine whether the planning strategy reused model shards already
resident on the selected Providers.

**Why this priority**: The first request may legitimately spend most of its time
publishing, fetching, verifying, and loading model shards. Later requests should
reuse compatible state; otherwise the cache-aware ACK and planning design is not
working.

**Independent Test**: In one unchanged allocation, execute a registered prompt
set with one warmup and five measured invocations per prompt. Compare the first
cold lifecycle with later lifecycles without pooling cache states.

**Acceptance Scenarios**:

1. **Given** no compatible shard residency, **When** the first request is
   planned, **Then** the evidence records the required publication, transfer,
   verification, host-memory residency, and GPU residency transitions.
2. **Given** compatible shards remain resident, **When** a later request is
   planned, **Then** ACK metadata reports that residency, the strategy prefers
   compatible Providers, and no duplicate model payload is transferred or
   loaded for unchanged role assignments.
3. **Given** a model, revision, semantics, graph, or partition identity change,
   **When** reuse is considered, **Then** incompatible state is rejected rather
   than treated as a warm hit.

---

### User Story 3 - Diagnose and repair real deployment failures (Priority: P3)

As an NDNSF-DI developer, I can identify whether a failed invocation originated
in deployment, security/bootstrap, NDN routing, planning, repository transfer,
preparation, stage execution, response handling, or cleanup, then demonstrate
the repair through the same acceptance path.

**Why this priority**: Real-cluster testing is useful only when failures lead to
an attributable component and a durable regression, instead of repeated remote
experimentation with changing conditions.

**Independent Test**: Inject or preserve one failure from each major lifecycle
boundary locally, require a precise terminal record, and verify that an exact
container/MiniNDN gate rejects the defective candidate before any replacement
TigerCluster submission.

**Acceptance Scenarios**:

1. **Given** a stalled artifact range or preparation operation, **When** progress
   stops, **Then** a bounded progress deadline reports the Provider, artifact,
   range or operation, last checkpoint, and terminal classification.
2. **Given** a locally reproducible logic defect, **When** it is repaired,
   **Then** the same real MiniNDN and exact-container path passes before a new
   immutable TigerCluster source identity can be submitted.
3. **Given** a cluster-only environmental failure, **When** evidence is
   analyzed, **Then** it remains separately classified and is not represented
   as an NDNSF-DI correctness or performance result.

---

### User Story 4 - Requalify the same lifecycle at meaningful model scale (Priority: P4)

As an NDNSF-DI researcher, I can run the same request, planning, distribution,
preparation, and data-driven execution logic with a model that cannot fit on one
target GPU, without introducing a separate large-model control path.

**Why this priority**: A small model validates the lifecycle cheaply, but it
does not exercise the volume, residency, and partition dependencies that make
distributed inference necessary.

**Independent Test**: After the small-model gate is complete, reuse the
qualified runtime and workflow with a pinned larger model partitioned across
three GPU nodes, and require one complete deterministic response before any
multi-request performance campaign.

**Acceptance Scenarios**:

1. **Given** a dependency graph and three available GPU capacities, **When** ACK
   collection closes, **Then** the strategy produces a valid partition and role
   plan whose shards fit the selected Providers.
2. **Given** a qualified large-model plan, **When** model shards arrive at
   different times, **Then** each stage starts as soon as its own preparation
   and predecessor-data conditions are satisfied.
3. **Given** a large-model failure, **When** the request terminates, **Then** the
   original failure and partial lifecycle remain immutable and no automatic
   replacement run is counted as the same experiment.

### Edge Cases

- An ACK arrives after ACK collection has closed or after the request is
  terminal.
- A Provider reports stale GPU residency for a different model or partition
  digest.
- The strategy selects an assignment whose model bytes fit disk but not GPU
  memory.
- Repository publication is active while one or more requested ranges remain
  unavailable or corrupted.
- A Provider restarts after accepting a role but before becoming prepared.
- A predecessor produces data before the successor finishes model preparation,
  or the successor prepares before predecessor data exists.
- A stage emits duplicate, reordered, stale-attempt, or wrong-request output.
- A request reaches EOS, maximum-token, cancellation, timeout, or component
  failure termination while other role work is still in flight.
- A second request arrives while the first request's model shards remain in
  disk, host-memory, or GPU caches.
- A shared filesystem makes a payload visible even though no NDN transfer
  occurred.
- A Slurm allocation, node, GPU, route, or scratch failure occurs independently
  of NDNSF-DI logic.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The User MUST create one durable request identity before the
  initial Request, and every lifecycle record MUST preserve that identity.
- **FR-002**: The system MUST collect ACKs before final role assignment and MUST
  reject late, duplicate, wrong-request, or terminal-state ACKs without
  reopening planning.
- **FR-003**: ACK evidence MUST describe relevant Provider capacity, load,
  reachability, and model-shard residency at disk, host-memory, and GPU levels.
- **FR-004**: Role and partition planning MUST occur after ACK collection and
  MUST bind the model identity, dependency graph, partition plan, Provider
  assignment, and planning-strategy identity.
- **FR-005**: A plan MUST be rejected before Selection if any assigned shard
  exceeds the selected Provider's usable capacity or violates graph
  dependencies.
- **FR-006**: Providers MUST obtain missing immutable model shards through the
  distributed artifact path, verify their content identity, make them durable,
  and record every residency transition before execution.
- **FR-007**: Compatible content-addressed shards MUST remain reusable after a
  request; incompatible model, semantics, graph, partition, or runtime identity
  MUST invalidate reuse.
- **FR-008**: The default planning strategy MUST prefer compatible GPU-resident
  shards, then compatible host-memory or disk state, while still satisfying
  capacity and dependency constraints.
- **FR-009**: Every stage MUST start only when its own model preparation and all
  direct predecessor data are available, without a global all-provider-ready
  barrier, fixed settle interval, or separate execution-start command.
- **FR-010**: Multi-token generation MUST complete as one durable distributed
  invocation; token generation MUST NOT repeat ACK collection or role Selection
  for every generated token.
- **FR-011**: GPU acceptance MUST fail closed when a selected stage uses CPU
  fallback, an unapproved execution backend, or a GPU other than its recorded
  assignment.
- **FR-012**: Every request MUST produce one terminal outcome from Response,
  cancellation, timeout, or classified component failure, and terminal state
  MUST be first-writer-wins and idempotent.
- **FR-013**: Progress monitoring MUST distinguish overall request deadlines
  from bounded no-progress deadlines for publication, transfer, preparation,
  stage execution, and response completion.
- **FR-014**: A failure record MUST identify the request, attempt, component,
  Provider, role, operation or artifact range, last progress checkpoint,
  terminal reason, and retained evidence location whenever applicable.
- **FR-015**: The validation path MUST exercise real NDN forwarding, independent
  Provider processes, allocated GPUs, node-local memory and scratch, and the
  selected model adapter; shared storage MUST NOT substitute for model transfer
  or stage data movement.
- **FR-016**: Permission distribution, NAC-ABE routing, one-time tokens, replay
  protection, Provider permission checks, and response authentication MUST
  remain enabled throughout acceptance runs.
- **FR-017**: Every remote candidate MUST be bound to immutable source, runtime,
  model, tokenizer, graph, shard, strategy, prompt-set, route, and job identities.
- **FR-018**: A real MiniNDN lifecycle gate and an exact-container deployment
  preflight MUST pass for the same candidate before the three-node TigerCluster
  campaign. A host without CUDA MAY run the local gates as explicit
  `CPU_LOGIC` fidelity, which proves no GPU claim. On the 8 GiB development host,
  that gate MUST use a three-role structurally faithful tiny-Qwen fixture under
  a maximum 6 GiB container memory and 7 GiB memory-plus-swap contract, retain
  real Repository transfer and transformer execution, and record zero cgroup
  OOM events. The local fixture and remote Qwen3-0.6B model have distinct frozen
  model identities, bound as `localFixtureManifestDigest` and
  `remoteSmallStageManifestDigest`, while the large negative control uses
  `remoteLargeStageManifestDigest`; none may satisfy another gate. They use the
  same source, V2 assignment contract, adapter family, lifecycle analyzer, and
  process topology. The exact SIF MUST then pass
  a bounded single-node TigerCluster `CUDA` preflight before the three-node run.
  The development host MUST NOT materialize full Qwen3-0.6B or large-model
  weights, run CUDA-capacity trials, or admit any workload whose bounded peak
  cannot be shown to fit the 6 GiB memory and 7 GiB memory-plus-swap limits.
  Graph, placement, manifest, and failure-contract tests MAY run locally only
  when they use metadata or bounded fixtures and do not load the remote model;
  every high-memory or unknown-memory experiment runs inside a TigerCluster
  Slurm allocation.
- **FR-019**: A failed or canceled formal identity MUST remain immutable; a
  repair MUST use a new linked source identity and MUST NOT silently replace,
  retry, or discard the original outcome. If (and only if) the failure is an
  independently evidenced pre-inference resource/environment boundary, such
  as a Slurm memory-cgroup OOM after all admission gates pass, the operator MAY
  admit at most one resource-repaired replacement with a new immutable resource
  profile, source identity, and campaign identity. This is not an automatic
  retry; the original row remains a retained negative, and any failure of the
  one replacement closes the large-model gate with no further attempt.
- **FR-020**: Small-model validation MUST complete before large-model
  requalification, and one complete large-model response MUST precede any
  large-model warmup-plus-measurement campaign.
- **FR-021**: Repeated-request evidence MUST separate cold distribution,
  preparation, warm reuse, execution, and response time and MUST retain every
  warmup, measured success, and measured failure independently.
- **FR-022**: Each discovered defect MUST have a durable record linking the
  observed failure, root cause, affected ownership boundary, repair, local
  regression, candidate identity, and remote requalification result.
- **FR-023**: Distributed-inference planning, preparation, cache, adapter, and
  stage-execution behavior MUST remain owned by NDNSF-DI; changes to generic
  NDNSF MUST be limited to independently justified framework defects and MUST
  preserve unrelated service behavior.
- **FR-024**: The final evidence MUST explicitly distinguish deployment
  correctness, model correctness, repository transport, preparation latency,
  execution latency, and environmental failures; no one category may be used as
  evidence for another.
- **FR-025**: Final Selection transport MUST remain within the effective NDN
  single-packet budget after assignment encoding, hybrid encryption, wrapped-key
  attachment, TLV framing, signatures, and SVS encapsulation. Multi-Provider
  collaboration MUST publish one authenticated, provider-bound assignment
  projection per selected Provider under the original request ID and Selection
  phase rather than one unbounded all-assignment payload. This fanout MUST NOT
  create a new request, ACK window, plan, attempt, or authorization bypass, and
  every selected Provider MUST expose an independently queryable Selection
  status.
- **FR-026**: Candidate admission MUST bind the native NDNSF shared library and
  Python extension actually loaded by every runtime process, not only the source
  bundle and parent SIF digests. The exact-container and three-node launch paths
  MUST use one native-overlay entrypoint and fail before Request publication if
  the mapped binary lacks provider-specific Selection projection, targeted
  Selection prefetch, or Selection-status support. A source/SIF pair with
  divergent compiled control-plane behavior MUST be rejected.
- **FR-027**: Before any model-bearing three-node campaign, the exact immutable
  candidate MUST pass a bounded TigerCluster control-plane canary using a tiny
  collaboration payload. For every selected Provider, evidence MUST distinguish
  Selection publication, sync/retrieval, authentication/decryption, projection
  acceptance, and status response. Missing delivery MUST terminate at a short
  progress deadline and MUST NOT be hidden by the model request timeout. The
  canary MUST NOT add a global preparation barrier or change per-role
  data-driven execution semantics.

### Key Entities

- **Invocation Identity**: The immutable request identity plus attempt,
  planning, model, and candidate lineage used across the lifecycle.
- **Provider Capability Snapshot**: One ACK-time view of capacity, load,
  reachability, and model residency.
- **Model Identity**: Model name and revision plus content, tokenizer,
  semantics, graph, and partition identities needed to determine compatibility.
- **Collaboration Plan**: The post-ACK partition, role, dependency, Provider,
  and strategy decision committed by the User.
- **Artifact Residency Record**: The verified disk, host-memory, or GPU state of
  one immutable shard on one Provider.
- **Stage Operation**: One role's preparation and execution state with direct
  dependencies, inputs, outputs, progress, and terminal outcome.
- **Lifecycle Trace**: The ordered Request-to-Response evidence for one durable
  invocation.
- **Defect Record**: A preserved failure, classification, root cause, repair,
  regression, source identity, and requalification chain.
- **Experiment Identity**: The immutable allocation, candidate, model, prompt,
  strategy, route, and schedule definition for one admitted run.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A clean three-node GPU allocation completes one pinned small-model
  request from Request to authenticated Response with an ordered multi-token
  answer, terminal reason, three stage records, and zero CPU fallback.
- **SC-002**: Independent analysis finds exactly one original request identity
  in 100% of ACK, plan, Selection, preparation, stage, token, and Response
  records for every admitted invocation.
- **SC-003**: Retained traces prove that every stage start follows its own
  preparation and direct-predecessor-data events, with zero global readiness
  barrier, fixed settle wait, or per-token collaboration cycle.
- **SC-004**: For five pinned real prompts, one warmup and five measured
  invocations per prompt terminate with complete answers or retained classified
  failures; no scheduled row is missing, duplicated, or silently replaced.
- **SC-005**: After the first compatible cold request, later unchanged requests
  record zero duplicate model-payload bytes and zero redundant shard loads for
  unchanged assignments, while incompatible identities record zero false warm
  hits.
- **SC-006**: Every admitted failure is classified to exactly one primary
  lifecycle boundary and includes its last progress checkpoint; no generic
  timeout is the sole retained diagnosis.
- **SC-007**: Every logic defect found on TigerCluster has a reproducing local
  or exact-container regression, a root-cause record, and a linked replacement
  candidate that passes that regression before remote requalification.
- **SC-008**: At least one pinned model whose complete weights exceed one target
  GPU's usable memory completes one deterministic three-node response using the
  same public invocation and lifecycle semantics as the small model.
- **SC-009**: Repeating each accepted correctness profile from a clean
  allocation with the same immutable identities reproduces the terminal answer,
  stage assignment, dependency order, cache-state classification, and security
  verdict.
- **SC-010**: Security evidence reports zero authorization bypass, token replay,
  wrong-request acceptance, unauthenticated terminal response, or plaintext
  permission payload in every accepted run.
- **SC-011**: The final audit maps every requirement to source, local gate,
  remote evidence, and ownership boundary and leaves no TigerCluster outcome
  represented as a broader claim than its evidence supports.
- **SC-012**: A three-role assignment whose combined compact plaintext exceeds
  7 KiB produces three provider-specific Selection projections, each selected
  Provider validates exactly its own projection, all three retain the same
  request ID and attempt, and no emitted Selection packet exceeds the admitted
  transport budget.

## Assumptions

- TigerCluster provides an authorized three-node allocation with one RTX 5000
  class GPU per stage and node-local writable scratch.
- The already qualified runtime image, small-model shards, large-model shards,
  schedules, and repository payloads are reused when their immutable identities
  remain unchanged; rebuilding or redistributing them is not routine setup.
- The pinned small-model path represented by the retained successful
  requalification is the first control, while the retained large-model failure
  remains negative evidence and is not overwritten.
- Greedy deterministic generation is used for correctness comparison; sampling
  quality and model-output preference evaluation are outside this feature.
- Real MiniNDN remains the default network and lifecycle regression environment;
  TigerCluster is used for physical multi-node GPU deployment fidelity.
- The development host has 8 GiB RAM. Full Qwen3-0.6B three-stage residency,
  large-model preparation, and CUDA capacity evidence run on TigerCluster;
  local gates must remain within the bounded tiny-Qwen profile in FR-018.
- Repository throughput already measured by the dedicated transport campaign
  remains separate evidence; this feature measures end-to-end lifecycle
  correctness and phase attribution rather than reusing total job duration as
  repository goodput.
- Cluster scheduler delays, allocation failures, VPN interruptions, and node
  faults are retained as environmental outcomes and do not by themselves prove
  or disprove NDNSF-DI logic.

## Scope Boundaries

- This feature does not compare language-model quality, optimize model
  architecture, or claim production-scale throughput.
- This feature does not redesign generic NDNSF collaboration semantics unless
  an independently reproduced framework defect requires a narrowly scoped
  correction.
- This feature does not rebuild the foundation runtime or reprepare immutable
  models merely to repeat an existing identity.
- This feature does not permit fixed sleeps, live route repairs, shared-filesystem
  payload shortcuts, security bypasses, automatic formal retries, or selective
  deletion of negative evidence to obtain a passing result.
