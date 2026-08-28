# Research And Architecture Decisions: Spec 174

This document freezes implementation decisions before task execution. It is based on the current repository, CodeGraph inspection of the native runtime, the Spec170 regression inventory, the frozen slide design, and the ordered verification requirement. It does not treat historical pass claims as current execution evidence.

## Decision 1 — Converge Existing Code; Do Not Rebuild NDNSF-DI

**Decision**: Treat current production modules as mandatory owners. Begin each task with an owner/regression/gap map. Preserve conforming behavior, repair partially conforming behavior at the existing boundary, migrate conflicting behavior explicitly, and execute unverified tests before replacing anything.

**Initial native owners**:

- ACK and placement: `CollaborationAckClosure`, `PlacementPlanCoreV3`, `NativeExecutionPlan`.
- Provider lifecycle: `NativeProviderHandler`, `NativeProviderRuntime`, `NativeProviderSession`, `ProviderRoleWorker`.
- Cross-Provider data: `NdnsfCollaborationDependencyIo`, `TensorBundleCodec`, `AsyncDataflowRuntime`.
- Tensor groups: `CollectiveRuntime`, `NdnsfCollectiveControl`, `ProviderGroupCoordinator`.
- protected execution: `ProtectedRuntime`.

**Initial Python owners**: `core/v3_lifecycle.py`, `core/placement.py`, `core/runtime_contracts.py`, `app_sdk/presplit.py`, `app_sdk/placement.py`, `app_sdk/coordinator.py`, `app_sdk/provider.py`, `app_sdk/canonical_artifacts.py`, `planner/`, `adapters/onnx/`, and `backends/onnxruntime.py`.

**Regression inputs**: existing distributed-inference C++ unit tests, `tests/integration-tests/ndnsf-di-core-flow.t.cpp`, `tests/python/test_spec170_*.py`, real MiniNDN scripts, and local SIF/Tiger preflight tests.

**Rejected**: a new Spec174 runtime package or clean-room implementation. It would duplicate authority, discard verified work, and introduce migration risk without solving a target gap.

## Decision 2 — Freeze Authority And Traceability Before Code Changes

**Decision**: Freeze the design PDF hash, Spec 174 revision, source tree/allowed dirty-input hashes, dependency identities, and FR/SC-to-owner map. Every requirement receives one implementation owner, one negative proof, and one gate artifact.

**Rationale**: The repository contains extensive prior work and a dirty shared worktree. Traceability prevents a later test from accidentally exercising an older helper, stale SIF, or alternate code path.

**Rejected**: inferring completion from filenames, old task checkboxes, source inspection, or a previous cluster result.

## Decision 3 — ACK Closure Linearizes Placement

**Decision**: One immutable request/attempt-bound ACK closure is the only planner input. `PreSplitFirstStrategy` remains pure and side-effect free. The trusted core validates the proposal, seals the final plan, and only then emits Provider-specific Selection.

**Reuse intent**: extend `CollaborationAckClosure`, Python V3 lifecycle/placement, and `PlacementPlanCoreV3`; do not create a second coordinator.

**Rejected**: planning while ACKs are still arriving, mutating a plan after Selection, or allowing planner code to hold network/key/device authority.

## Decision 4 — One Provider Owns One Complete Role

**Decision**: Final assignment is bijective within an attempt: every role exactly once, every selected Provider exactly one role. A tensor-parallel rank is a complete rank role. The tensor group owns collective semantics; the rank role owns one Provider-local executable.

**Rationale**: If one logical role were shared across Providers, its ownership, failure, input, output, and authorization would be ambiguous. Explicit rank roles retain tensor parallelism without weakening ownership.

**Rejected**: assigning two pipeline stages to one Provider by default; spreading one role across Providers; treating a tensor shard as an ordinary independently replaceable service.

## Decision 5 — Keep `PreSplitFirstStrategy` As The Reference Policy

**Decision**: Determine graph-valid split/rank roles before assigning Provider offers, then bind roles using sealed resource/capability snapshots. Cache/residency may influence cost but does not define a different architecture. All proposals are revalidated by the trusted core.

**Reuse intent**: preserve existing Python and native V3 placement/sealing code where it conforms; retain older alternative strategies only as explicit non-default experiments outside Spec174 completion.

## Decision 6 — Offline Canonical ONNX, Request-Scoped Provider Assembly

**Decision**: Offline tooling exports a canonical content-addressed ONNX graph, initializers, adapter metadata, tokenizer/config data, and graph-valid split candidates without Provider bindings. After Selection, each Provider fetches only its sealed canonical inputs and assembles or materializes its role locally. ONNX Runtime executes the local role.

**Reuse intent**: use current canonical-artifact, ONNX adapter, repository, native plan, and model-runner owners.

**Rejected**: prebuilding the final Provider layout offline; deploying PyTorch/Transformers; splitting an opaque model node that has no graph-valid cut.

## Decision 7 — No Second Global Preparation Barrier

**Decision**: A role becomes runnable when its own Selection, local assembly/admission, and sealed dependencies are ready. Dependency-driven execution provides local readiness. Group contracts coordinate only the ranks/rounds that require coordination.

**Rationale**: The ACK closure and sealed plan already form the global planning boundary. Another all-Provider prepare/commit barrier adds latency and failure coupling that the design does not require.

**Rejected**: “all Providers ready” before any role runs. A Provider that is not locally ready fails its role/attempt boundedly.

## Decision 8 — All Cross-Provider Tensor Movement Uses NDN

**Decision**: `NdnsfCollaborationDependencyIo` and production tensor codecs carry every pipeline activation, tensor partial, and collective result through consumer-pulled NDN Interest/Data. Producer namespaces own request-scoped objects. Consumers authenticate a manifest first, then exact named segments.

**Required proof**: observed producer publication and consumer fetch names for every sealed edge; no TCP/RPC/shared-file/NCCL cross-Provider fallback; duplicate/reorder/loss/corruption/replay/cancellation negatives.

**Rejected**: using an in-memory fixture or filesystem handoff as MiniNDN proof; hiding rank exchange in an opaque collective library.

## Decision 9 — One Scheduler For Pipeline, Tensor, And Hybrid Graphs

**Decision**: Reuse the existing dependency scheduler and role workers. Pipeline edges and tensor-group rounds are sealed dependencies. Any merge that performs computation is a role; the group itself does not silently execute model logic.

**Reference graphs**:

- `P0: Stage0 -> P1: Stage1 -> P2: Stage2 -> P3: Stage3`.
- `TensorGroup(Stage0, world=2): rank0 -> P0, rank1 -> P1`.
- Hybrid `[Stage0 -> TensorGroup(Stage1, world=2) -> Stage2]` on four Providers.

**Rejected**: separate schedulers that implement different retry, readiness, or authority rules for each graph type.

## Decision 10 — Exact Security Binding Precedes Plaintext Execution

**Decision**: Preserve normal NDNSF permission, NAC-ABE, signature, UserToken, ProviderToken, and replay controls. Additionally bind the local Provider grant and tensor-group capability to requester, request, attempt, plan, policy snapshot, role/rank, group/epoch, artifact/dataflow identities, and local wrapped key. Only then may protected data be unwrapped for ONNX Runtime.

**Reuse intent**: extend `ProtectedRuntime`, `ProviderGroupCoordinator`, current NDNSF messages, and security tests; do not build a DI-only authorization service.

**Terminal rule**: success, cancel, timeout, replan, restart, and permanent failure all release leases and zeroize plaintext/key material.

## Decision 11 — Retry Is Same-Name Retrieval; Replan Is New Authority

**Decision**: Segment loss retries the same exact Interest name inside the same attempt/plan/epoch and cannot re-execute a role. Replanning advances attempt/plan/group authority and fences prior work. Old output is accepted only if the new attempt explicitly proves identical authority and result contracts; default behavior rejects it.

**Rejected**: silently changing names on transport retry, independently replacing one tensor rank, or accepting late old-plan Data because its tensor shape happens to match.

## Decision 12 — Deterministic Exact Oracles, Not Timing, Close Correctness

**Decision**: Use a small inspectable deterministic ONNX fixture with frozen input and unsplit reference output. Assert exact canonical result bytes/hash where deterministic and explicitly normalized tensor comparison only when ONNX representation demands it. Every planned edge must also match exact name/digest evidence.

**Repetition**: acceptance cases run in three independent fresh processes with the same frozen seed/config/input. Each run has no-progress and hard timeouts and kills its process tree on failure.

**Rejected**: treating low latency, stage completion, “no exception,” or a source-level test as end-to-end correctness.

## Decision 13 — Ordered Gates Are Promotion Boundaries

**Decision**: Unit → in-process integration → real MiniNDN+CPU → exact local SIF → TigerCluster. Each gate consumes the prior manifest and refuses to run if identities or status do not match.

**Rationale**: This prevents expensive Tiger work from rediscovering deterministic state, path, cwd, Python ABI, library, or packaging defects.

## Decision 14 — Build SIF Locally Inside Its Target ABI Boundary

**Decision**: Use the existing local Apptainer build and validation route. Native/Python extensions are built in the candidate SIF or an ABI-identical sealed builder, never copied from a host boundary. Verify imports, SOABI, `ldd`/RPATH closure, Boost/NDN/ORT identities, runtime entry points, and the CPU acceptance case before hashing and promotion.

**Rejected**: remote materialization as the normal route; reconstructing from Docker/OCI on Tiger; copying a host-built `_ndnsf.so`; repairing libraries after upload.

## Decision 15 — Tiger Runs The Exact Candidate In Stages

**Decision**: Tiger performs read-only preflight and hash verification, then bounded CPU/no-GPU, single-GPU, and cross-Provider multi-GPU stages. It does not rebuild. The multi-GPU pass requires a complete application result, intended GPU providers, no CPU fallback, and observed NDN tensor exchange.

**Rejected**: calling an ORT/GPU smoke test a distributed inference pass; advancing after a failed stage; running an unlimited background campaign.

## Decision 16 — Evidence Is Minimal, Immutable, And Classified

**Decision**: Manifests distinguish `implemented`, `wired`, `executed`, and `measured`. Preserve summary, input/config/source/candidate hashes, first-failure diagnostic, relevant bounded log, and trace hash. Exclude secrets and plaintext tensors. Superseded raw runs do not remain canonical.

**Rationale**: This makes claims auditable without repeated large artifacts or misleading historical output.

## Unresolved Work Is A Gap, Not A Design Choice

The detailed owner/gap inventory is the first implementation task. If current source behavior cannot be conclusively mapped because the CodeGraph index is stale or a path is generated at runtime, the task must refresh or use focused source/runtime evidence and classify the item `unverified`. It must not invent a replacement design to avoid investigation.
