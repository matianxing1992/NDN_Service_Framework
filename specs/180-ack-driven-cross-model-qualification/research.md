# Research and Design Decisions

## Decision 1: Split Spec175 at the local qualification boundary

**Decision**: Spec175 owns generic streaming/Qwen implementation plus local
unit, integration, and CPU/MiniNDN closure. Spec180 owns YOLO ACK-driven
integration, SIF, CUDA, and cross-model Tiger qualification.

**Rationale**: Spec175 accumulated implementation, packaging, deployment, model,
and performance concerns. Moving the expensive qualification boundary produces
one testable handoff and prevents cluster failures from obscuring generic API
correctness.

**Alternatives considered**: Continue expanding Spec175; rejected because its
acceptance target had changed repeatedly and its historical candidates could no
longer be compared as one subject.

## Decision 2: Reuse the existing coordinator

**Decision**: Generalize the high-level application facade to reach the existing
generic `AutomaticPlanningCoordinator.request()` rather than implement a YOLO
planner.

**Rationale**: Current code already publishes the request, closes ACKs, creates
Provider views, inspects the graph, enumerates candidates, seals artifacts, and
commits Selection in the required order. The missing layer is the non-LLM
high-level entrypoint and real YOLO adapter.

**Alternatives considered**: Call the coordinator directly from the example;
rejected because it would leave the public application API incomplete. Retain
`distributed_inference()`; rejected because it consumes a prebuilt deployment
and cannot prove request-scoped ACK-driven planning.

## Decision 3: Certify a small YOLO candidate catalogue

**Decision**: Support only atomic and shared-backbone two-shard candidates in
Spec180.

**Rationale**: Safe cuts depend on real graph semantics, tensor interfaces,
initializer ownership, and merge behavior. A small certified set is auditable
and sufficient to prove that ACK capabilities change the selected plan. The
shared candidate has exactly one input-ingress role and one result-egress role;
these identities are sealed into the candidate and plan digests.

**Alternatives considered**: Arbitrary ONNX graph cuts and N-by-M layouts;
rejected because syntactic cutability does not prove YOLO semantic correctness.
Replicated backbone; deferred because it adds duplicate compute and another
policy choice without helping the central qualification claim.

## Decision 4: Keep export and live placement separate

**Decision**: Offline tooling creates canonical ONNX objects, metadata, safe cut
descriptions, and the numerical oracle. Live ACKs choose the candidate and
Provider mapping for each request.

**Rationale**: Export can inspect model semantics but cannot know live Provider
availability, memory, device capability, or cache state. Binding Providers
offline would contradict ACK-driven placement.

## Decision 5: Use the same deployed runtime for YOLO and Qwen

**Decision**: Build one SIF with generic NDNSF-DI and ONNX Runtime support; keep
both models external and content-addressed.

**Rationale**: A shared image demonstrates that the framework is model-neutral
and avoids rebuilding large containers for each model. PyTorch/Transformers/
Ultralytics are export dependencies, not deployment dependencies.

**Alternatives considered**: One image per model; rejected because it weakens
the cross-model runtime claim and duplicates packaging risk.

## Decision 6: Make Tiger qualification finite and functional

**Decision**: Run exactly one YOLO job and one Qwen job from the same candidate,
with two requests in each job. Do not tune or repeat for performance.

**Rationale**: The research question is whether the complete runtime works on
real GPUs. Statistical performance inference needs a separate design, repeated
independent runs, matched baselines, and a later Spec.

**Alternatives considered**: Combine qualification and benchmark matrices;
rejected because it would reintroduce the cost and interpretability problem that
motivated the split.

## Decision 7: Use three GPUs for both Tiger jobs

**Decision**: Qwen maps its three stages to GPU0--GPU2. YOLO maps
`BackboneNeck`, `DetectShard0`, and `DetectShard1` to GPU0--GPU2 and runs the
explicit `Merge` Provider as CPU postprocessing on the same node.

**Rationale**: This reuses the previously exercised one-node, three-RTX-6000
resource shape. It keeps four YOLO role owners without pretending that merge is
model inference or requiring an unverified fourth GPU.

## Decision 8: Qualification requires one immutable evidence chain

**Decision**: Pre-build and pre-dispatch closure gates bind every behavior-
affecting plane, include mutation tests with zero side effects, and prohibit
cross-candidate evidence reuse.

**Rationale**: Earlier attempts showed that a SIF can be healthy while copied
planner metadata, working directory, Python ABI, model artifacts, or submission
configuration are stale. File existence alone is not candidate consistency.

## Decision 9: Carry large invocation input by reference

**Decision**: Preserve bounded inline input for small generic/Qwen requests, but
make the registered YOLO image an encrypted repository object referenced by the
generic Request. Only the candidate-declared input-ingress role fetches it
after Selection, and only the candidate-declared result-egress role publishes
the terminal result.

**Rationale**: Current code base64-embeds every `ApplicationInput.payload` in
`DIRequestEnvelopeV2`. That is unsuitable as the only contract for a 640-by-640
image and exposes plaintext to every ACK candidate. Reusing the existing
repository `LargeDataReference` keeps one transport owner and binds size,
digest, encryption, authorization, and protection epoch without inventing a
YOLO-specific request protocol.

## Decision 10: Candidate requirements are candidate-local

**Decision**: Every safe candidate owns its complete role/dependency/resource
requirements. The placement strategy evaluates candidates independently and is
invariant to catalogue order.

**Rationale**: Current coordinator code initializes a legacy
`required_roles` field from `candidates[0]`. Atomic and four-role YOLO candidates
cannot share that role set. Treating the first candidate as authority can make
valid placement depend on list order; T006 removes that ambiguity while keeping
the generic planner model-neutral.

## Decision 11: Assembly validation follows role kind and security epoch

**Decision**: Pipeline/rank roles are identified by layer intervals;
`COMPONENT_SET` roles are identified by exact ONNX node sets. Cache identity
also binds adapter/assembler ABI, security domain, and protection epoch, and a
cache hit always rechecks current authorization.

**Rationale**: Current `RoleAssemblySpec` allowlists `COMPONENT_SET` but still
requires `layer_end > layer_begin`, which forces Transformer vocabulary onto
YOLO. Its default `plaintext-v1` protection epoch also cannot qualify a
protected request. Role-kind-specific validation and explicit epoch binding
remove both implementation ambiguities before native assembly is exercised.

## Decision 12: Make candidate choice deterministic after feasibility

**Decision**: The YOLO catalogue carries a signed candidate priority. Placement
evaluates every candidate independently, then uses that priority and a stable
candidate-digest tie-break; catalogue order is never authoritative.

**Rationale**: Both atomic and shared candidates can be feasible in the same
ACK snapshot. Without an explicit tie-break, the existing first-candidate
shortcut could silently decide the plan and invalidate the ACK-driven claim.

## Decision 13: Separate local CPU reference from Tiger GPU qualification

**Decision**: CPU ONNX Runtime is permitted for deterministic local and MiniNDN
cases and must be recorded as such. CUDA execution and no CPU model fallback are
mandatory only for the two Tiger functional jobs.

**Rationale**: Local CPU tests are the cheap semantic gate; treating them as
GPU failures would make the validation ladder contradictory, while omitting the
backend identity would make evidence ambiguous.

## Decision 14: Isolate complete suites by process

**Decision**: The formal local gate runs each complete C++/Python suite in its
own supervised child process. A one-process aggregate run is diagnostic only.

**Rationale**: A prior aggregate integration run exposed cross-suite heap
corruption although the affected suite passed alone. Process isolation makes
the qualification result reproducible and prevents a crash from being hidden
by an ad hoc rerun or misattributed to a single test case.

## Decision 15: Register the local-suite inventory

**Decision**: The local gate records the exact C++ binaries/selectors, Python
selectors, and MiniNDN entrypoints before execution; each item runs in its own
supervised child.

**Rationale**: “Complete tests” is not reproducible when the selector set is
implicit. A source-bound inventory makes omissions visible and preserves child
exit, timeout, signal, and cleanup evidence without requiring a fragile
one-process aggregate.

## Decision 16: Keep YOLO Merge outside model-layer execution

**Decision**: The shared YOLO `Merge` role owns dependency consumption and
canonical detection postprocessing. It may use ONNX Runtime CPU only for a
declared merge graph; it must never execute model layers assigned to GPU roles.

**Rationale**: The fourth Provider is required for an explicit fan-in owner, not
as an untracked CPU fallback. Separating merge/postprocessing from model-layer
execution keeps the Tiger hardware claim testable and prevents a successful CPU
path from being misreported as distributed GPU inference.

## Decision 17: Make catalogue trust a versioned input

**Decision**: Store the YOLO catalogue authority and covered-field rules in a
dedicated `catalogue-trust-root-v1.md` contract owned by T001.

**Rationale**: A signer requirement without a fixed public-key source still
allows an ambient key, profile override, or Provider ACK to become accidental
authority. The trust-root contract makes the private-key boundary,
canonicalization, and evidence identity testable before export or placement.

## Decision 18: Separate catalogue integrity from candidate choice

**Decision**: Before ACK closure, the runtime may verify only an opaque signed
catalogue revision. Candidate records and requirements become visible to the
planner only after `ACK_CLOSED`.

**Rationale**: Static package validation is useful for early rejection, but
exposing candidate role requirements before the request-scoped ACK snapshot
would reintroduce offline placement authority and make the request order
ambiguous.

## Decision 19: Freeze Qwen case sequence and Tiger manifest

**Decision**: Q-W contains two valid continuation turns followed by one
same-child stale/mismatch rejection; QWEN-F requires a signed external
Qwen3.6-27B model manifest before staging.

**Rationale**: Naming “continuation plus mismatch” without a fixed sequence
allows a runner to omit one boundary. Requiring a separate signed 27B manifest
prevents a tiny local fixture or packaging smoke from being relabeled as the
Tiger model subject.

## Decision 20: Separate artifact-manifest authority from catalogue authority

**Decision**: YOLO candidate catalogues and external YOLO/Qwen model manifests
use explicitly registered trust roots with distinct covered-field contracts.

**Rationale**: A candidate-selection signature authenticates placement metadata;
it does not by itself authenticate a large model package staged outside the
SIF. A separate artifact authority prevents profile or Provider inputs from
silently replacing the model identity and gives C2 a concrete fail-before-
staging check.

## Decision 21: Make implementation paths repository-root explicit

**Decision**: Every task names the full repository-relative path for an existing
owner; abbreviated paths are not accepted in implementation or evidence
records.

**Rationale**: The Python package contains similarly named `app_sdk`, `core`,
`sdk`, and top-level modules. Root-qualified paths prevent a nominally complete
task from changing a compatibility copy or omitting the production owner.
