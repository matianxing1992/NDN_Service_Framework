# Traceability: Pluggable DI Collaboration Planning

## Requirement coverage index

`FR-001`, `FR-002`, `FR-003`, `FR-004`, `FR-005`, `FR-006`, `FR-007`,
`FR-008`, `FR-009`, `FR-010`, `FR-011`, `FR-012`, `FR-013`, `FR-014`,
`FR-015`, `FR-016`, `FR-017`, `FR-018`, `FR-019`, `FR-020`, `FR-021`,
`FR-022`, `FR-023`, `FR-024`, `FR-025`, `FR-026`, `FR-027`, `FR-028`,
`FR-029`, `FR-030`, `FR-031`, `FR-032`, `FR-033`, `FR-034`, `FR-035`,
`FR-036`, and `FR-037` are mapped below.

## Dissertation scope to feature

| Dissertation proposal authority | Feature behavior | Story | Requirements | Success evidence |
|---|---|---|---|---|
| Ch. 5.3 layered NDNSF-DI architecture and common splitter output | Model adapters emit common graph/split values; base NDNSF owns generic wire/security and NDNSF-DI owns all inference semantics | US1, US2 | FR-001–FR-009, FR-026 | SC-001, SC-003, SC-009, SC-011 |
| Ch. 5.4 automatic collaboration-plan generation from model, provider, network, backend, and artifact state | Joint split/provider strategy, capacity-safe generation, trusted repository publication, and plan assembly | US1, US2, US3 | FR-001–FR-012, FR-023–FR-024, FR-031 | SC-001–SC-004, SC-008–SC-009 |
| Ch. 5.5 one dependency-driven executor for chain, fan-in, and fan-out | Generic role DAG execution | US5 | FR-021 | SC-007–SC-008 |
| Ch. 5.6 remove model-specific provider assumptions | Model-specific splitter only; no model-specific wire or executor branch | US1, US5 | FR-007–FR-009, FR-021 | SC-007, SC-009 |
| Ch. 5.7 artifact placement and provider failure evaluation | Pre-split catalog, tiered residency/cache reuse, repository publication, preparation attribution, compensation | US3, US4, US5 | FR-010–FR-020, FR-024, FR-031 | SC-002, SC-005–SC-008 |
| Ch. 1.7 and Ch. 3.3 keep Core provider collaboration application-independent | The existing generic Collaboration API is the carrier: Core owns begin/immutable ACK closure/opaque plan commit plus generic ACK/Selection/data/status/Response primitives; only NDNSF-DI interprets offers, roles, GPU, preparation, objects, DAGs, results, and recovery | US1, US4 | FR-013–FR-019, FR-022–FR-023, FR-026, FR-032–FR-033 | SC-004–SC-006, SC-009, SC-011, SC-013 |
| Advisor correction: preparation must occur inside inference invocation | One public handle and one successful wire Request/ACK closure; final Selection assigns roles and starts request-scoped asynchronous preparation inside that invocation while allowing eligible prewarm/cache reuse | US4 | FR-014–FR-020, FR-023–FR-024 | SC-005–SC-006, SC-009 |
| User correction: `ACK=true` already means willingness to accept feasible roles in the DI service | A generic positive ACK acquires that meaning only when it carries a validated signed `DIProviderOfferV2` bound to profile/request/attempt/model intent/boot epoch/resource sequence/deadline/expiry; one final Selection per selected Provider carries its complete role tuple and consumes the existing ProviderToken once | US4 | FR-013–FR-020 | SC-004–SC-006 |
| User correction: no all-model-ready barrier; execution is data-driven | Final Selection follows planning over valid DI offers without a second role-negotiation/ACK round; mandatory per-Provider acceptance evidence is not a global cover; each role starts on Selection + local readiness + verified direct inputs | US4, US5 | FR-014–FR-021 | SC-005–SC-007 |
| User correction: personal dissertation scope, not all project Thrusts | Baseline and API support, not universal joint horizontal/vertical optimizer | All | Assumptions and plan scope boundary | No unsupported claim in T013 |
| User decision: validate locally by default | MiniNDN plus pinned Qwen3-0.6B; TigerCluster/large model only by explicit separate authorization | All | FR-025 | SC-008, SC-010 |
| Lifecycle/security audit: crash-safe acceptance, complete results, and bounded recovery | Per-Provider acceptance linearization, invocation/attempt hierarchy, object/result contracts, local terminal CAS, explicit adoption, and conditional liveness | US4, US5 | FR-027–FR-030 | SC-004–SC-007, SC-012 |
| User decision: make current Collaboration API the normative carrier | `begin_collaboration(DEFERRED)` publishes the one Request, `ACK_CLOSED` freezes candidates, trusted DI planning/materialization completes, and `commit_plan` seals the same invocation before existing Selection; pre-request roles/dependencies remain `PREPLANNED` compatibility | US1, US4 | FR-032–FR-033 | SC-013 |
| User decision: support many model families through adapters | A composed `ModelFamilyAdapter` separates graph/split, task-I/O, state, and runner ports; opaque unsplittable models use one graph node; public request and Core carrier remain common | US1, US5 | FR-007–FR-009, FR-023, FR-034 | SC-009, SC-011, SC-014 |
| User decision: support KV cache without binding the framework to LLMs | General `InferenceStateContract`; exact prefix KV is one Provider-local derived-state profile with opaque ACK evidence, plan/Selection binding, exact authorization/identity, revalidation, cleanup, and optional non-baseline migration | US3, US4, US5 | FR-012, FR-024, FR-035–FR-037 | SC-002, SC-004–SC-006, SC-015 |

## Requirements to design, tasks, and tests

The design authorities, task target paths, and validation filenames in this
section are **planned future artifacts or edits** unless separately identified
as existing. Their presence here is not a claim that they currently exist or
already implement Spec 163. Only the separate **Current-source reality
anchors** table below makes claims about the present implementation.

| Requirements | Design authority | Closing task(s) | Planned focused validation |
|---|---|---|---|
| FR-001–FR-006 | `contracts/public-planning-api.md`; `data-model.md` PlacementRequest/Decision | T002, T004, T005 | `test_ndnsf_di_placement_strategy.py`; `test_ndnsf_di_automatic_collaboration_plan.py`; `test_ndnsf_di_external_placement_strategy.py` |
| FR-007–FR-009 | `research.md` Splitter ownership; `contracts/public-planning-api.md` Splitter contract | T003, T004, T010 | `test_ndnsf_di_split_candidate_contract.py`; `test_ndnsf_di_dependency_dag.py` |
| FR-010–FR-012, FR-031 | `contracts/presplit-catalog-api.md`; `contracts/public-planning-api.md`; default strategy/materialization/publication contract | T006, T007 | `test_ndnsf_di_presplit_catalog.py`; `test_ndnsf_di_presplit_first_strategy.py`; repository publication and partial-staging cases |
| FR-013–FR-018 | `contracts/preparation-selection-flow.md`; generic Core opaque seam plus DI offer, complete per-Provider Selection tuple, mandatory acceptance, and RoleExecutionGate | T008, T009, T010, T011 | `opaque-selection-lifecycle.t.cpp`; `test_ndnsf_opaque_selection_lifecycle.py`; `test_ndnsf_di_selection_dataflow.py`; `test_ndnsf_di_dependency_dag.py`; `test_ndnsf_di_compensation.py`; concurrent GPU-offer exclusion, one-token/two-role bundle, acceptance-loss/UNKNOWN, and upstream-progress-during-downstream-Selection-retry cases |
| FR-019 | ACK-offer/final-Selection security invariants | T002, T008, T009, T012, T013 | sensitive-field, token, input-key-grant binding/tamper/cross-role replay, permission denial, NAC-ABE routing, plaintext-permission rejection, UserToken mismatch, envelope, capacity, replay, tamper, restart cut, noncanonical encoding, downgrade, and mixed-version cases |
| FR-020 | Request lifecycle plus orthogonal per-Provider Selection delivery and per-role state in `data-model.md` | T004, T008, T009, T010, T011 | ACK closure, partial/complete Selection delivery, concurrent dataflow activity, local-ready, input-ready, once-only start, and terminal-state assertions |
| FR-021 | Event-driven role gate and generic DAG contract in spec/plan/quickstart | T009, T010, T011, T012, T013 | chain/fan-in/fan-out, both readiness/input arrival orders, duplicate events, and partial-terminal negative cases |
| FR-022 | Migration and compatibility in plan and preparation contract | T001, T005, T009, T013 | V1/V2 negotiation and no-mixed-mode tests |
| FR-023–FR-024 | Application integration and per-role evidence contract | T004, T005, T009, T010, T012, T013 | canonical request workflow, recomputable identity, latch/timeline, and no-global-barrier assertions |
| FR-025 | MiniNDN/Qwen3-0.6B validation contract and explicit TigerCluster boundary | T013; separate later requalification only if authorized | Real-model MiniNDN cold/warm matrix; no default live-cluster command |
| FR-026 | `contracts/lifecycle-security-contract.md` ownership matrix; plan migration section | T001, T008, T009, T012, T013 | static Core ownership gate, finite V1 allowlist, and non-DI opaque-seam fixture |
| FR-027 | Lifecycle/security trust and non-goal model; public planning API | T002, T005, T012, T013 | allowlist/digest pinning, untrusted-output validation, timeout-not-sandbox assertion, malicious signed-result handling |
| FR-028 | `contracts/core-opaque-selection-transaction.md`; Core WAL, participant seam, mandatory acceptance, requester/Provider protected journal | T008, T009, T012, T013 | crash at every prepare/WAL/token/lease/blob/projection/receipt cut, torn WAL, receipt loss, UNKNOWN, identical retry, callback failure, restart |
| FR-029 | `InputOutputObjectManifest`, `ResultContract`, `DIResultEnvelopeV2`, Response CAS | T009, T010, T012, T013 | object substitution, multi-sink aggregation, result completeness, cancel/expiry/Response races |
| FR-030 | Invocation/attempt/recovery state machines and `AdoptedInputEvidence` | T009, T011, T012, T013 | partitioned cancel convergence, bounded cleanup, replan fencing, authorized and unauthorized cross-attempt reuse |
| FR-032–FR-033 | `contracts/ndnsf-collaboration-carrier.md`; existing Collaboration API plus DI-opaque deferred extension; `PREPLANNED` compatibility and `DEFERRED` default | T001, T004, T008, T009, T012, T013 | non-DI deferred fixture; immutable ACK closure; one same-invocation commit; early/conflicting/late/cross-invocation rejection; byte-identical idempotent retry; preplanned source compatibility; default-DI call-path trace |
| FR-034 | `contracts/model-adapter-state-cache.md`; composed model-family adapter and generic task/input/options/result request | T003, T004, T010, T012, T013 | LLM, object-detection, and opaque one-node container fixtures on one carrier; static zero-LLM-Core gate |
| FR-035–FR-037 | `contracts/model-adapter-state-cache.md`; `InferenceStateContract`, exact-prefix-KV profile, Provider-local default, optional encrypted migration | T002, T003, T007, T009, T011, T012, T013 | cold/cleanup/exact-hit and all identity, tenant, epoch, expiry, eviction, restart, pin, and migration fallback rows |

## Success criteria closure

| Criterion | Closing task(s) | Evidence level before implementation |
|---|---|---|
| SC-001 | T004, T005 | Proposed |
| SC-002 | T006, T007 | Proposed: exact cache-tier reuse and safe generated publication |
| SC-003 | T002, T004, T007 | Proposed |
| SC-004 | T002, T008, T009, T013 | Proposed |
| SC-005 | T008, T009, T010, T012, T013 | Proposed |
| SC-006 | T008, T009, T010, T012, T013 | Proposed |
| SC-007 | T010, T012, T013 | Proposed; current NDNSF-DI Core DAG primitives are implemented but not accepted as this feature |
| SC-008 | T006, T007, T009, T010, T011, T013 | Proposed: MiniNDN Qwen3-0.6B paired cold/warm distribution |
| SC-009 | T004, T005 | Proposed |
| SC-010 | T013 excludes TigerCluster; separate explicit requalification owns any later large-model run | Documented boundary, not a model-experiment result |
| SC-011 | T001, T008, T009, T012, T013 | Proposed; current Core contains legacy DI debt that must be migrated/quarantined |
| SC-012 | T009, T010, T011, T012, T013 | Proposed bounded model/property history check |
| SC-013 | T001, T004, T008, T009, T012, T013 | Proposed: 100% default Spec 163 calls use `DEFERRED`, fixed-plan fixtures use `PREPLANNED`, and no DI-private parallel lifecycle exists |
| SC-014 | T003, T004, T010, T012, T013 | Proposed: three model-family fixtures, same application/carrier, opaque unsplittable graph, zero Core LLM fields |
| SC-015 | T002, T003, T007, T009, T011, T012, T013 | Proposed: exact local prefix-KV positive and exhaustive mismatch/security/lifecycle matrix |

## Current-source reality anchors

Unlike the planned tables above, every row here describes the implementation
that exists before Spec 163 work begins. The Design consequence column states
future work and does not claim that consequence has already been implemented.
The complete T001 caller, test, timer, message, handler, field, and migration
inventory is frozen in
[`evidence/baseline.md`](evidence/baseline.md); this table is its concise index.

Implementation checkpoint (2026-07-28): T002 and T003 now supply the
side-effect-free placement boundary, signed-offer sanitization, common
`SplitCandidate`, ONNX dependency-graph normalization, digest-pinned composed
`ModelFamilyAdapter`, generic task/state contracts, and LLM-text,
object-detection, and opaque-container fixtures. Focused strategy, candidate,
adapter, legacy adapter, and policy tests pass. This checkpoint does not claim
that T004+ lifecycle, catalog, Selection, execution, recovery, or MiniNDN gates
are complete.

Implementation checkpoint (2026-07-28): T004 now supplies the generic,
DI-opaque `BeginCollaboration` -> immutable `ACK_CLOSED` -> one-shot
`CommitCollaborationPlan` carrier in C++, pybind, and Python while preserving
the preplanned API. NDNSF-DI exposes the model/task/input-first
`APPClient.request(...)`, derives its graph and split candidates through a
digest-pinned model-family adapter, plans only over validated closed ACK
offers, and commits trusted role/artifact bindings on the same invocation.
`contracts/app-config.schema.json` and `contracts/app.example.yaml` define the
deployment-free, secret-free operator configuration and separate immutable
model-shard from derived-state cache policy. The focused C++ state-machine
test, five automatic-planning/config tests, syntax checks, and 26 existing
collaboration/assignment/placement/adapter compatibility tests pass. This
checkpoint does not claim that T005+ strategy loading, catalog, final
Selection transaction, execution, recovery, or MiniNDN gates are complete.

Implementation checkpoint (2026-07-28): T005 adds exact distribution,
entry-point, strategy-name, version, and state-digest allowlisting; exact
`app.yaml` strategy selection; bounded caller-side execution; and validation
of strategy output as untrusted data. Documentation states that external
strategies are operator-trusted local code and that the in-process deadline is
not a sandbox or hard-preemption mechanism. The old ten-policy partition and
provider-assignment path is exposed only as non-authoritative compatibility
hints. Four new external-strategy tests plus 17 existing SDK, placement, and
automatic-planning tests pass. This checkpoint does not claim T006+ catalog,
Selection, execution, recovery, or MiniNDN completion.

Implementation checkpoint (2026-07-28): T006 adds the immutable operator
pre-split catalog with validated `SplitterOutput`/graph/candidate/artifact
coverage, exact content and signature verification, content-addressed
DistributedRepo segment publication, signed-manifest activation as the atomic
visibility point, idempotent aliases, conflict rejection, revoke/retire
propagation before reuse, bounded failed-staging cleanup, immutable strategy
snapshots, and retained audit evidence. The catalog contains no Provider
residency assertion. Three catalog tests and six splitter/adapter compatibility
tests pass. This checkpoint does not claim T007+ placement ranking, Selection,
execution, recovery, or MiniNDN completion.

Implementation checkpoint (2026-07-28): T007 adds the deterministic
`PreSplitFirstStrategy`. It matches exact active manifests, validates
model/semantics/graph/backend/precision and boot/cache epochs, ranks
pinned-GPU, reload-safe-GPU, host-RAM, disk, repository, then new
materialization, and otherwise selects a graph-valid generated candidate from
the closed ACK capacity snapshot. Runtime peak includes the candidate safety
margin; aggregate per-Provider GPU use is enforced and unknown bounds fail
closed. `ReusableStateView` (including KV-cache-class state) is separate,
security-domain/layer/epoch/expiry/pin bounded, and contributes cost evidence
without granting read or mutation authority. Five new strategy tests plus 15
placement/catalog/application regressions pass. This checkpoint does not claim
T008+ Selection transaction, execution, recovery, or MiniNDN completion.

Implementation checkpoint (2026-07-28): T008 adds the base-Core
application-independent opaque Selection transaction. The encrypted,
Provider-scoped `GenericSelectionTxnStore` WAL owns exact transaction,
Selection, ProviderToken, optional lease, participant-version, deadline,
opaque commit/acceptance bytes, `ABORTED`, `COMMITTED`, replay, uniqueness, and
tombstone evidence. Concurrent identical messages join one bounded
`VALIDATING` callback; Core fsyncs `COMMITTED` before the idempotent
`onCommitted` projection, and duplicate cleanup scheduling cannot extend the
original deadline. Deferred collaboration requires externally supplied opaque
assignments and uses a bounded ordered assignment-set carrier when one
Provider owns multiple roles; the old preplanned text projection remains
compatibility-only. Python exposes explicit encrypted-store configuration and
generic participant registration. The legacy Core check for a DI execution
certificate was removed so V2 application semantics live outside base NDNSF.
Seven C++ opaque transaction/seam cases, four Python API/static-boundary cases,
44 existing admission/token/selection/collaboration cases, actual pybind
symbol loading, and the full 375-step project build pass. The first optimized
pybind attempt was OOM-killed; the verified low-memory `-O0 -g0`, single-job
build passes. This checkpoint does not claim T009+ DI envelopes, Provider
projection/preparation, dataflow execution, recovery, PDFs, or MiniNDN
completion.

Implementation checkpoint (2026-07-28): T009 adds strict canonical V2 Request,
signed Provider offer, complete per-Provider Selection assignment, and
mandatory acceptance codecs with closed version negotiation and bounded wire
sizes. Positive V2 ACKs are now issued through a Provider-local
`DIProviderOfferIssuer` that reserves GPU MiB before publishing the signed
offer; concurrent offers cannot overlap, and unused offers release explicitly.
The NDNSF-DI opaque participant validates exact invocation, request, attempt,
plan, Provider/boot, offer, resource sequence, role tuple, artifact,
dependency, deadline, input-grant, generation, and optional state-reuse
bindings, then places the canonical ledger transition and acceptance preimage
inside the Core WAL commit blob. `onCommitted()` reconstructs that projection
idempotently and only then schedules role preparation asynchronously.
Provider-local role gates independently combine Selection, local readiness, and
direct-input readiness, so a selected upstream role can progress while another
Provider's Selection remains unknown. The requester stores only encrypted
byte-identical Selection retry material, treats receipt loss as `UNKNOWN`, and
rejects retry after the attempt deadline. Signed residency revalidation,
repository miss fetch/verify/promote/load, bounded shard retention with
selected/in-flight fences, exact-prefix derived-state binding, terminal state
destruction, and V1/V2 quarantine are covered. Eleven focused Python lifecycle
tests, 47 compatibility tests, four Python Core opaque-transaction tests,
seven C++ WAL/seam tests, four C++ deferred-collaboration tests, `diff --check`,
and the full 375-step `-j2` build pass. This checkpoint does not claim T010+
DAG result execution, compensation, Docker/MiniNDN evidence, or document
closure.

Implementation checkpoint (2026-07-28): T010 replaces the old unsynchronized
set-based role gate with a Provider/boot/attempt/plan/generation/deadline-fenced
local DAG authority. Selection, local readiness, and every authenticated direct
input meet in one locked `WAITING -> RUNNING` admission CAS; its callback is
dispatched inside the same critical section, so cancel, failure, supersession,
and deadline events cannot interpose after admission but before launch.
Chain, fan-out, and fan-in use the same model-neutral engine, and Stage 0 does
not wait for downstream preparation. `InputOutputObjectManifest` seals object
name, lineage, schema, segment count/size, payload digest, AEAD/key grant,
signer, producer Provider/boot, consumers, generation, and expiry.
`ResultContract` requires one sink or an explicit deterministic aggregator/
complete aggregation rule; `DIResultEnvelopeV2` is accepted only from the
completed result role and only when contract, manifest, schema, semantics,
payload, and execution fences match. One local first-terminal-wins CAS drives
at most one generic Response callback; the implementation explicitly promises
at-most-once admission rather than physical exactly-once computation.
Seven focused DAG/race/result tests plus 46 affected Core, adapter, planning,
Provider, public-API, documentation, and architecture compatibility tests and
`diff --check` pass. This checkpoint does not claim T011 compensation,
T012 Docker/MiniNDN security evidence, or T013 document/final closure.

Implementation checkpoint (2026-07-28): T011 adds requester-local,
deadline-bounded attempt compensation without claiming distributed atomicity.
Every replan increments the attempt, preserves the original deadline, and
requires fresh plan and token digests. Cross-attempt objects remain unusable
unless signed `AdoptedInputEvidence` exactly binds old/new attempt, lineage,
semantics, schema, segment contract, authorization, consumer, requester, and
expiry; otherwise a complete fallback cover is mandatory and absence of both
aborts the invocation. Canonical closed-field `DICancelAttemptV2`,
`DIReleaseOfferV2`, and `DIStatusQueryV2` payloads use the existing generic
exact-target carrier. Target validation binds requester, Provider, original
deadline, expiry, and signature, while the schemas expose no role-assignment,
deadline-extension, or secret-bearing fields. The idempotent protected outbox
retries cancel/release/status across partitions until authenticated
acknowledgement or the unchanged deadline. Old-attempt events are fenced, and
response/cancel/expiry use one first-terminal-wins authority. Seven focused
compensation tests plus 65 affected recovery, scheduling, readiness, DAG,
Selection, planning, Core-contract, and public-API tests, syntax checks, and
`diff --check` pass. This checkpoint does not claim T012 Docker/MiniNDN
security evidence or T013 document/final closure.

Implementation checkpoint (2026-07-29): T012 closes the bounded local Docker
security and lifecycle gate at
`results/spec163-local-docker-20260729_015529`. One 4 GiB/5 GiB-swap container
ran one NFD, Controller, requester, and three Providers with no Torch, Qwen, or
model load. The real secure deferred invocation, exact multi-role Selection,
acceptance replay, independent role readiness, fan-out/fan-in, security
regressions, model-family carrier, Core ownership, default DEFERRED/fixed
PREPLANNED, and bounded history corpus pass. The corpus exhausts 36 bounded
histories plus seeds 163000–163031 and observes zero violations of its five
declared invariants. Evidence and limitations are in
`evidence/local-docker.md`.

Implementation checkpoint (2026-07-29): T013 closes the frozen local MiniNDN
matrix at `results/spec163-minindn-matrix-v2-20260729_022847`. The real network
used Memphis plus UCLA/Arizona/WUSTL; 59/59 matrix rows, 23/23 gates, 63
row-specific references, and four runtime assertions pass.
The exact `Qwen/Qwen3-0.6B` revision, 1,519,197,900-byte manifest, content
digest, and semantics digest are retained. Five prompts produced five warmups
and 25 measured complete greedy CPU generations; all 30 pass prompt-specific
semantic contracts, terminate within 37 tokens, match their frozen per-prompt
reference, and match the automatic `PreSplitFirstStrategy` plan to the manual
baseline. Raw answers, token IDs, per-token timings, TTFT, total latency,
tokens/s, planning, real repository store/fetch, and byte-payload preparation
observations are retained. The final postflight also proves the trusted
`materialize -> publish -> commit` order and zero commit on preparation
failures. Local CUDA is absent, so GPU
preparation and exact warm-GPU reuse remain explicitly deferred rather than
simulated. `evidence/minindn-qwen3.md` records the accepted and rejected runs
and the exact claim boundary.

## Post-implementation success-criterion closure

| Criterion | Status | Closing evidence |
|---|---|---|
| SC-001–SC-004 | PASS | placement/strategy/candidate/catalog tests in the 23-gate MiniNDN matrix and final artifact-before-commit postflight |
| SC-005–SC-007 | PASS | real deferred MiniNDN lifecycle plus Docker temporal ordering, exact Selection, DAG and result gates |
| SC-008 | PARTIAL BY DECLARED ENVIRONMENT BOUNDARY | five-prompt Qwen3 CPU reference and cold byte-payload lifecycle pass; exact warm-GPU row is deferred because local CUDA is absent |
| SC-009 | PASS | model/task/input application tests and deployment-free `app.yaml` |
| SC-010 | PASS AS EXCLUSION | no TigerCluster or large-model action or claim |
| SC-011 | PASS | static ownership and non-DI Core fixtures |
| SC-012 | PASS FOR DECLARED BOUNDED CORPUS | 36 exhaustive histories and 32 retained concurrent seeds; no universal proof claim |
| SC-013 | PASS | DEFERRED default, PREPLANNED compatibility, idempotent/invalid commit matrix |
| SC-014 | PASS | LLM, object-detection, and opaque-container adapters share one request/carrier |
| SC-015 | PASS FOR LOCAL CONTRACT | exact-prefix KV positive/mismatch/tenant/epoch/expiry/eviction/restart/pin/fallback rows; no KV migration claim |

| Current fact | Source anchor | Design consequence |
|---|---|---|
| Current Core already exposes `RequestCollaboration(...)` with a generic `CollaborationPlan`, but roles/dependencies must exist before Request and role coverage is checked against that predeclared plan | `ndn-service-framework/ServiceUser.hpp` `CollaborationPlan`; `ServiceUser.cpp` `RequestCollaboration()` and `collaborationAckRoleCoverageSatisfied()` | T004 adds `DEFERRED` begin/ACK-closure/commit while preserving the current call as `PREPLANNED` |
| Python `request_collaboration(...)` requires roles/key scopes before Request; its ACK observer is observational only | `pythonWrapper/ndnsf/service.py` `request_collaboration()` | T004 exposes the generic deferred handle without allowing the observer to mutate the live candidate set or commit a plan early |
| NDNSF-DI already invokes the generic Collaboration API, but current callers submit fixed plans or a coordinator-only role | `NDNSF-DistributedInference/.../client.py`, `app_sdk/coordinator.py`, and `app_sdk/deployment.py` collaboration call sites | T004/T009 make the same API the full normative carrier; no coordinator-only collaboration followed by a private lifecycle |
| APP has a manual `decide(requests)` path separate from canonical request lifecycle | `app_sdk/client.py` `decide()` and `submit()` | T004/T005 make one joint strategy automatic and keep old path compatibility-only |
| Current SDK exposes ten separate policy kinds | `sdk/contracts.py`, `sdk/suite.py` | T002/T005 introduce one placement authority |
| ONNX graph analysis and simple cut candidates already exist | `adapters/onnx/graph.py` `analyze_onnx_graph()`, `estimate_split_candidates()`, `build_chunk_dependencies()` | T003 adapts instead of replacing this work |
| Current application request remains deployment-centric | `app_sdk/application.py` `InferenceApplication.request(deployment, input, ...)` | T004 replaces the canonical surface with model/task/input while retaining explicit compatibility |
| Current `RunnerAdapter` is too narrow for model-family semantics | `app_sdk/contracts.py` `RunnerAdapter.supports()` and `create_runner()` | T003 adds composed graph/split/task/state descriptors around the separately trusted runner |
| V1 has KV telemetry and cache-placement scaffolding only | `core/runtime_contracts.py` `KvCacheTelemetry`; `runtime_v1.py` `choose_cache_placement()` | T003/T007/T009 must not present this as Spec 163 exact identity, authorization, Selection binding, or lifecycle implementation |
| Current dependency execution stores roles and edges and already gates selected + local-ready + direct inputs | `core/execution.py` `DependencyDrivenExecution` | T009/T010 preserve this DI-local predicate, add at-most-once generation admission, and prove chain/fan-in/fan-out without an all-role barrier |
| Current final Selection performs verify/load/warm synchronously | `core/deployment_control.py` `SelectionGatedProvider.select()` and the base `ServiceProvider.cpp` deployment branch | T001 classifies the base branch as legacy debt; T008 retains only a generic Core hook and T009 moves V2 assignment/preparation authority into NDNSF-DI |
| Core and the Python binding currently expose a deployment-control family from `DeploymentIntent` through reservation/decision, readiness/activation, encrypted assignment, stage evidence/abort, and tombstone types; Core also stores deployment plan/readiness/activation state and exposes deployment-prepare handlers | `NDNSFMessages.hpp:65-194`; `ServiceUser.hpp:917-919`; `ServiceUser.cpp:5416-5675,6930-7500`; `ServiceProvider.hpp:260-291`; `ServiceProvider.cpp:8980-9331`; `_ndnsf.cpp:2231-2305,3926-3944` | This is finite V1 semantic debt. T008 may retain only independently justified opaque generic transaction primitives; T009 owns every V2 DI interpretation and handler. The domain-by-domain disposition is in `evidence/baseline.md` |
| Provider `CollaborationContext` already carries generic assignment, large-object fetch/publish, dependency wait/subscribe, operation status, and final Response operations | `ndn-service-framework/ServiceProvider.hpp` `CollaborationContext`; Python wrapper equivalents | T009 reuses these primitives and keeps DI meaning in opaque payload/codecs rather than creating a parallel DI transport |
| Current positive ACK emits one ProviderToken and final Selection consumes it | `ServiceProvider.cpp` ACK/token and Selection validation/cleanup paths | T008 preserves generic one-time token semantics; T009 binds a DI offer and complete role tuple outside Core and records one crash-atomic per-Provider acceptance |
| One V2 Request can schedule Provider cleanup for the same `pendingKey` when replay state is installed and again when a positive ACK stores the pending Request; the scheduler retains no replaceable event handle and each callback can add a grace callback | `ServiceProvider.cpp:3195,6327-6363,7016,7159` | T008 establishes one generic monotonic deadline/transaction owner; T009 derives DI cleanup from it without a parallel timer |
| Current Selection path consumes/deletes token state before later DI deployment/assignment validation and dispatch and has no durable opaque application transaction hook | `ServiceProvider.cpp` token path before deployment/assignment branches | T008 adds the generic Core WAL/participant seam and moves token/opaque-lease disposition into its one commit; T009 supplies the NDNSF-DI blob and idempotent projection |
| Current Core wire/security names are Request/ACK/Selection/Response | NDNSF dynamic runtime and proposal Ch. 3 | Exact DI role assignment and mandatory acceptance evidence remain opaque payload/status profiles, with no second role-negotiation/ACK round or fifth generic service-message kind |
| Current provider pending cleanup is scheduled at Request/ACK paths with a short fixed default and grace | `ServiceProvider.hpp` and `ServiceProvider.cpp` cleanup path | T008 provides a generic monotonic deadline budget; T009 adds DI attempt/generation/resource cleanup without extending Core semantics |
| Spec 129 rejects a global complete ReadySet/ExecutionActivate authority | `specs/129-selection-gated-deployment/contracts/deployment-control-contract.md` | Spec 163 also rejects a coordinator-held complete local-ready cover; final Selection plus each role's local/input latches replace any global activation barrier |
| Spec 116/129 currently trigger preparation from Selection | `specs/116-ndnsf-di-user-api-coherence/spec.md` FR-017/018 and `specs/129-selection-gated-deployment/spec.md` FR-018/019 | `SELECTION_DATAFLOW_V2` keeps Selection as assignment authority, makes local preparation asynchronous, adds data-driven role gates, and preserves old evidence and V1 behavior unchanged |

## Unmapped or intentionally excluded work

- Universal globally optimal model partitioning: excluded; external strategies
  and splitters may pursue it later.
- General horizontal-plus-vertical parallelism research from the broader project
  proposal: excluded from this dissertation feature.
- Training, federated learning, tensor-parallel kernels, global testbed, and
  production scheduling: excluded.
- TigerCluster and large-Qwen performance/answer campaigns remain separately
  authorized Spec 162 work and cannot count as default Spec 163 closure.
- Default real-model evidence is the pinned MiniNDN `Qwen/Qwen3-0.6B`
  cold/warm matrix; GPU-specific rows remain deferred when local CUDA is absent.
