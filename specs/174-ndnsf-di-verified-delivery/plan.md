# Implementation Plan: NDNSF-DI Verified Design Delivery

**Branch**: active integration worktree | **Date**: 2026-08-20 | **Spec**: [spec.md](spec.md)

**Input**: `specs/174-ndnsf-di-verified-delivery/spec.md` and the frozen design authority `docs/NDNSFDI/slides/main.pdf` (48 pages, SHA-256 `5a1096e5d9d5cf3899de7c305c7b98ea7981c56986118253d63feb91043bd668`).

## Summary

Deliver the current NDNSF-DI design as one verified execution path. A User submits the operation, input, deadline, policy, and acceptance intent. NDNSF-DI closes the signed ACK set, runs a pure `PreSplitFirstStrategy`, seals a one-to-one Provider/role plan, sends Provider-specific Selection, lets each Provider fetch and assemble canonical ONNX components locally, and executes pipeline, tensor, or hybrid dependencies through explicit NDN Interest/Data. The same source tree and exact locally qualified SIF must pass ordered unit, in-process integration, real MiniNDN+CPU, and TigerCluster gates.

This is a closed plan, not an invitation to improvise. Each task must follow the contracts in this directory and include implementation, negative cases, acceptance commands, and evidence. A new design decision, wire field, runtime transport, ownership rule, deployment route, or oracle requires updates to `spec.md`, this plan, the affected contract, and `tasks.md` before code work continues.

## Technical Context

**Language/Version**: C++17; Python 3.10 inside the qualified SIF/runtime boundary; Bash and Slurm scripts for deployment. Host Python is not SIF ABI authority.

**Primary Dependencies**: ndn-cxx; NDN-SVS `Experimental` with Boost 1.71; NFD/MiniNDN; ONNX Runtime; pybind11; python-ndn only at the Python application boundary where required; Apptainer and Slurm.

**Storage**: Content-addressed canonical ONNX graph, initializer, adapter, and tokenizer artifacts in the repository namespace; producer-owned NDN namespaces for request-scoped tensors; bounded local caches; immutable evidence manifests with necessary logs and hashes.

**Testing**: Boost.Test through Waf (`unit-tests`, `integration-tests`); pytest for SDK/planner/packaging/gate contracts; real MiniNDN/NFD CPU execution; local SIF ABI checks; staged TigerCluster Slurm runs.

**Target Platform**: Linux host, real MiniNDN network namespaces, immutable Apptainer SIF, and TigerCluster CPU/GPU allocations.

**Project Type**: Mixed C++/Python distributed-systems runtime, experiment harness, and immutable cluster deployment package.

**Performance Goals**: Correctness and delivery, not performance claims. Every workflow has no-progress and hard deadlines, first-failure diagnostics, and no indefinite wait. Timing is diagnostic only.

**Constraints**:

- The slide PDF and Spec 174 are authority; Spec 170 is historical evidence and reusable inventory only.
- One selected Provider owns exactly one complete role or tensor-rank role per attempt; every role is assigned once; a logical role never spans Providers.
- Tensor parallelism uses explicit rank roles in a sealed `TensorGroup`; ranks do not fail over independently.
- Placement starts only after immutable ACK closure and is pure over sanitized snapshots. The trusted core revalidates and seals before Selection.
- Every cross-Provider activation, partial, and collective result uses planned, authenticated NDN Interest/Data. Hidden TCP/RPC push, shared files, and opaque cross-Provider NCCL are forbidden.
- Providers run canonical or locally assembled ONNX with ONNX Runtime. PyTorch/Transformers may exist only offline and cannot be deployed runtime requirements.
- Plaintext cannot reach ONNX Runtime before exact Provider grant, group capability, policy, plan, attempt, role/rank, epoch, and wrapped-key validation.
- The shared dirty worktree must be preserved. Tasks name their paths; no broad staging, reset, clean, or destructive history operations.
- TigerCluster cannot repair a missing local proof. No remote rebuild or candidate mutation after promotion.

**Scale/Scope**: one-role baseline; four-role pipeline on four Providers; two-rank tensor group on two Providers; four-Provider hybrid `[pipeline role, two tensor-rank roles, pipeline role]`; Tiger single-device reference plus genuine cross-Provider multi-GPU tensor execution.

## Source And Decision Precedence

1. Approved Spec 174 and its current contracts.
2. Frozen `docs/NDNSFDI/slides/main.pdf` semantics.
3. Constitution and current security/runtime invariants in `AGENTS.md`.
4. Current source behavior verified with CodeGraph and tests.
5. Spec 170 documents, prior SIFs, logs, and results as historical evidence.

Existing code is not automatically correct. A conflict with levels 1–3 is a recorded gap to fix, not permission to redefine Spec 174.

## Constitution Check

*GATE: Passed before research; re-check after contracts and tasks.*

| Principle | Compliance | Gate |
|---|---|---|
| I. Canonical Dynamic Runtime | Generic NDNSF request/ACK/Selection path; no service-specific or hidden DI control plane. | Application-facing tests, not helper-only paths. |
| II. Security Is Part Of The Data Path | Authorization, tokens, replay, grants, capabilities, and zeroization are lifecycle/dataflow requirements. | Wrong identity/attempt/plan/role/epoch/digest/token/capability is rejected before execution. |
| III. CodeGraph First | Current placement, execution, collective, protected-runtime, and Provider paths were mapped before planning. | Each task starts with impact review and ends with source/test evidence. |
| IV. Spec-Driven Durable Work | Spec, plan, data model, contracts, quickstart, and tasks precede implementation. | Deviation requires document change and re-audit. |
| V. Verify With The Right Scope | Unit → in-process → MiniNDN+CPU → exact SIF → Tiger. | Any failed gate blocks later gates. Inspection is not execution. |
| VI. Cohesive Outcome Tasks | One behavioral outcome includes implementation, negative tests, commands, and evidence. | No “tests later” or “evidence later” tasks. |

No constitution violation is planned. Existing cross-language and deployment surfaces are narrowed to one path rather than expanded into a new runtime.

## Project Structure

### Feature documentation

```text
specs/174-ndnsf-di-verified-delivery/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── traceability.md
├── quickstart.md
├── contracts/
│   ├── runtime-lifecycle-v1.md
│   ├── ndn-tensor-dataflow-v1.md
│   ├── ownership-security-v1.md
│   └── verification-gates-v1.md
├── checklists/requirements.md
└── tasks.md
```

### Production and verification surfaces

```text
NDNSF-DistributedInference/
├── cpp/ndnsf-di/
│   ├── NativeExecutionPlan.*
│   ├── NativeProviderHandler.*
│   ├── NativeProviderRuntime.*
│   ├── NativeProviderSession.*
│   ├── ProviderRoleWorker.*
│   ├── NdnsfCollaborationDependencyIo.*
│   ├── TensorBundleCodec.*
│   ├── CollectiveRuntime.*
│   ├── NdnsfCollectiveControl.*
│   ├── ProviderGroupCoordinator.*
│   └── ProtectedRuntime.*
└── ndnsf_distributed_inference/
    ├── sdk/placement.py          # PlacementPlanCoreV3 and V3 sealer/projection
    ├── core/{v3_lifecycle.py,placement.py,runtime_contracts.py,state.py}
    ├── app_sdk/{presplit.py,placement.py,coordinator.py,provider.py,canonical_artifacts.py}
    ├── planner/
    ├── adapters/onnx/
    └── backends/onnxruntime.py

tests/
├── unit-tests/distributed-inference-*.t.cpp
├── integration-tests/{ndnsf-di-core-flow.t.cpp,ndnsf-integration-fixture.*}
├── python/test_spec170_*.py       # reusable regression inventory only
├── python/test_spec174_*.py       # new acceptance contracts
├── fixtures/spec174/
└── container/

Experiments/
├── NDNSF_DI_LlmPipeline_Minindn.py
└── NDNSF_DI_Run_Minindn_Regressions.py

packaging/ndnsf-di-container/
├── oci/
    └── adapters/slurm-apptainer/{scripts,profiles,templates}/

ServiceUser.{hpp,cpp}             # CollaborationAckClosure and User lifecycle owner
```

**Structure Decision**: Native C++ remains the authority-bearing execution core. Python owns application policy, orchestration, adapters, and packaging support. No second Spec174 runtime package is created. Compatible Spec170 tests remain regression inputs until explicitly retired, but cannot serve as Spec174 authority by filename or historical pass status.

## Architecture Ownership And Mutation Boundaries

| Concern | Owner | May decide | Must not decide |
|---|---|---|---|
| Application request | User/API | operation, input, deadline, policy, acceptance intent | Provider list, role map, precomputed split |
| ACK closure | trusted core | valid offer set and immutable closure identity | placement preference or execution authority |
| `PreSplitFirstStrategy` | pure planner | graph-valid role/rank proposal over sanitized snapshots | network, storage, key, lease, device, or mutable runtime actions |
| Plan sealer | trusted core | legality, one-to-one ownership, identities, plan digest | heuristic mutation after sealing |
| Selection projection | trusted core | exact Provider-local role, dependencies, grants, plan digest | unrelated Provider authority |
| Provider runtime | selected Provider | fetch, assemble, admit, execute, publish owned output | another role, plan rebinding, unplanned transport |
| Tensor group | sealed group contract | ranks, operation/round schedule, result owner | independent rank failover or implicit computation |
| Artifact repository | content store | immutable canonical retrieval | placement or runtime authority |
| Evidence writer | gate runner | bounded manifests/hashes/status/first failure | secrets, plaintext tensors, mutable authority |

## Planned Delivery Phases

### Phase 0 — Freeze inputs and gap map

1. Record PDF, source tree, dependency identities, and dirty-worktree exclusions.
2. Map every FR/SC to code symbols, existing tests, missing behavior, and future evidence.
3. Classify evidence as `implemented`, `wired`, `executed`, or `measured`; never promote one class into another.
4. Reject Spec170 behavior that conflicts with current authority while retaining reusable code/tests.

**Exit**: every requirement has one owner and one planned proof; unknowns are explicit blockers.

### Phase 1 — Lifecycle and ownership

1. Make ACK closure immutable and request/attempt bound.
2. Make `PreSplitFirstStrategy` deterministic over closure plus graph/artifact/resource snapshots.
3. Validate and seal all roles, edges, groups, dependencies, and identities before Selection.
4. Project one exact Selection per Provider and enforce bijective ownership.
5. Define retry, replan, cancel, restart, stale-output, and terminal fencing.

**Exit**: invalid or ambiguous plans cannot reach Provider preparation.

### Phase 2 — Execution, NDN dataflow, and security

1. Fetch canonical ONNX artifacts and assemble complete local roles.
2. Bind tensors to deterministic producer-owned names and authenticated manifests.
3. Drive pipeline and tensor dependencies through one scheduler; computed merges are explicit roles.
4. Seal tensor-group membership and collective schedules without a global prep barrier.
5. Verify grants/capabilities before ONNX plaintext and zeroize every terminal path.

**Exit**: the four-Provider hybrid request completes without hidden transport or authority.

### Phase 3 — Deterministic local gates

1. Focused unit tests for every invariant and negative case.
2. In-process four-Provider integration with production codecs, handlers, state machines, and ONNX Runtime.
3. Real MiniNDN/NFD CPU pipeline, tensor, and hybrid cases with controlled impairment/recovery.
4. Frozen reference oracle and exact intermediate name/digest checks.
5. Three independent clean-process repetitions per acceptance case with fixed inputs/seeds/config and hard timeouts.

**Exit**: all local manifests pass; no hang, stale acceptance, hidden fallback, or missing first-failure evidence.

### Phase 4 — Immutable local SIF

1. Build locally with Apptainer from a sealed source/dependency manifest inside the target container ABI boundary.
2. Reject host-built Python extensions and unresolved/host-leaked dependencies.
3. Verify import, CLI, MiniNDN/NFD entry points, ORT providers, Boost/NDN linkage, no PyTorch/Transformers runtime requirement, and hashes.
4. Run the same CPU acceptance workload inside the exact SIF.

**Exit**: one candidate hash/build record qualifies. No alternate SIF or mutable overlay may be substituted.

### Phase 5 — TigerCluster qualification

1. Read-only login/compute/storage/scratch/GPU/driver/Apptainer/network preflight.
2. Upload exact SIF/evidence and verify hashes before allocation work.
3. CPU/no-GPU negative and single-GPU reference before distributed cases.
4. Genuine cross-Provider multi-GPU tensor case with sealed NDN dataflow.
5. Require complete application answer and exact oracle; stage-only success is failure.

**Exit**: remote evidence names the same source, dependency, config, input, and SIF hashes and records no CPU fallback or unplanned transport.

## Change-Control And Stop Rules

- Tasks refine details only inside their named boundaries and contracts.
- New role/message/name/security/retry/transport/artifact/SIF/oracle decisions require plan changes first.
- Failed focused test blocks the task's broad suite; failed unit blocks integration; integration blocks MiniNDN; MiniNDN blocks SIF; SIF blocks Tiger.
- Three repeats mean independent fresh processes with identical frozen input/config, not three assertions in one process.
- Timeout kills the process tree and records last phase, first missing dependency/name, and process/container identity.
- Debug bypasses, CPU fallback in GPU cases, helper-only paths, and synthetic evidence cannot close a gate.
- Failed Tiger work cannot mutate the local candidate. Repair starts locally and yields a new hash.
- Superseded raw results are not current evidence; retain concise manifest, first failure, necessary log, trace hash, and identities.

## Verification Matrix

| Case | Unit | In-process | MiniNDN+CPU | Local SIF | Tiger |
|---|---:|---:|---:|---:|---:|
| One-role baseline | required | required | required | required | single-device reference |
| Four-role pipeline/four Providers | ownership/state | complete answer | complete answer | complete answer | optional diagnostic |
| Two-rank group/two Providers | group/collective | complete answer | complete answer | complete answer | required multi-GPU |
| Four-Provider hybrid `[1,2,1]` | graph/ownership | complete answer | complete answer | complete answer | optional after required path |
| Duplicate/reordered segments | codec/state | required | controlled impairment | required | not required |
| Missing segment/no progress | timeout/fencing | required | controlled impairment | required | not required |
| Wrong attempt/plan/rank/epoch/digest | reject | reject | selected negatives | reject | CPU/no-GPU subset |
| Cancel/replan/restart | state/fencing | required | required subset | required subset | diagnostic after pass |

## Evidence Contract

Every gate manifest records:

- feature/spec revision and design PDF hash;
- source commit/tree and explicit hash/list of permitted dirty inputs;
- dependency, compiler, Python SOABI, ORT, Boost, ndn-cxx, NDN-SVS, NFD, MiniNDN, Apptainer, driver, and GPU identities as applicable;
- exact command, cwd, environment allowlist, configuration/input hashes, seed, time, timeout, process identity, and exit status;
- SIF/build-record hash where applicable;
- expected/actual oracle hash, lifecycle terminal state, hidden-fallback checks, and first-failure diagnostic;
- only bounded necessary logs/traces, never secrets or plaintext tensors.

## Complexity Tracking

No constitution violation is accepted. Explicit tensor-rank roles and ordered environments are required by the frozen design and deployment boundary; neither creates an alternate runtime architecture.
