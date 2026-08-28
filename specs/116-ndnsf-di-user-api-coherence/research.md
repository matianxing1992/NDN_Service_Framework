# Research: Current NDNSF-DI User API Audit

## Review Method

This audit used CodeGraph first, then targeted source inspection and public
attribute introspection. It applied a methodology-review lens: a public API is
accepted only when the documented user journey is executable, role ownership is
unambiguous, extension contracts are explicit, and tests validate the journey
rather than isolated implementation pieces.

## Initial Audit Verdict

At planning time, **major revision was required before the API could be called
easy to use.** The core
mechanisms are substantially stronger than the public experience: durable
request state, fencing, authenticated readiness, partial optimizer replacement,
and separate runner adapters already exist. The problem is composition and
contract coherence, not absence of deep runtime functionality. The resulting
revision is implemented and its final disposition is recorded in
`audit-report.md` and `completion-summary.md`.

## Evidence-Based Findings

### F1 - Public namespaces leak implementation helpers (High)

`app_sdk`, `core.ports`, several contract modules, and status modules construct
`__all__` from every non-underscore global. Runtime inspection found imported
typing names, dataclass helpers, modules, and other implementation symbols in
the public-looking surface. Root-level compatibility additionally exposes a
large intentional legacy manifest.

**Decision**: keep the root compatibility manifest for migration, but make every
canonical namespace an explicit allowlist and test it as a versioned contract.

### F2 - Canonical classes wrap duplicate classes through dynamic delegation (High)

`app_sdk.client.APPClient` wraps `app_sdk.facades.APPClient`, and
`app_sdk.deployment.APPDeployment` wraps another class of the same name. Both
canonical wrappers delegate unspecified behavior with `__getattr__`. Static
documentation, type checking, autocomplete, and ownership inspection therefore
cannot identify the actual contract reliably.

**Decision**: retain one implementation owner per public facade. Internal
collaborators may remain, but canonical behavior must be explicit methods or
typed composition rather than dynamic public delegation.

### F3 - Deployment phases are correct but exposed at the wrong abstraction (High)

`APPDeployment` exposes `validate`, `resolve`, `plan`, and `apply`; `apply`
requires readiness and activation receipts. These are useful administrative
operations but unsuitable as the only application deployment experience. A
handle is not bound to a deployment/revision, so callers also pass identifiers
back into `status` and `wait`.

**Decision**: introduce a thin application composition root whose `define`
creates Application-signed intent and whose primary `request(definition, ...)`
orchestrates request-time preparation and execution through existing owners.
Keep optional `deploy` for prewarming and phase APIs under the advanced surface;
both use the same ensure-deployment coordinator. Do not weaken receipt
validation or give the deployment owner definition-authoring authority.

### F4 - DeploymentDefinition under-specifies public semantics (High)

The current definition provides deployment/model IDs, artifacts, roles, and a
generic configuration map, but has no Application creator identity, signature,
or authorization evidence. It also treats roles as caller-authored even though
the accepted architecture makes model variant, partition graph/roles, Provider
assignment, and execution target policy outputs.

**Decision**: `InferenceApplication.define` creates and signs typed intent:
service/request semantics, allowed model alternatives, artifact references,
objectives, hard constraints, and an allowlisted optimization profile. The
bounded policy graph produces concrete graph/role/Provider/execution-target evidence for
the immutable revision. Never serialize policy objects, secrets, or executable
code into a definition.

### F5 - Request entry points overlap and `submit` has two meanings (High)

The client exposes `distributed_inference`, `async_distributed_inference`,
`infer`, `infer_async`, and `submit`. `submit` switches between local positional
execution and network submission, includes an internal `_network_submitter`,
and interprets numeric deadlines using magnitude heuristics. Result-returning
methods also disagree between raw bytes and a typed `InferenceResult`.

**Decision**: one network `request` returns one bound request handle. A
synchronous helper is only `request(...).result(...)`. Existing `submit`
becomes a bounded compatibility delegate, while local deterministic execution
moves to an explicit adapter. Use aware `datetime` or `timedelta` timing
contracts and reject ambiguity.

### F6 - Deployment discovery is detached from the client (High)

Legacy discovery functions require a raw `ServiceUser`, read
`serviceMetaInfo["deployments"]`, ignore pump/parse errors, and return
dictionaries. No current path publishes or validates the authenticated ACTIVE-
revision announcement assumed by the first draft.

**Decision**: add a typed `client.deployments.discover/get` catalog. Existing
NDNSD fields are untrusted locator/digest hints only; the catalog fetches an
Application-signed definition record for ON_DEMAND deployments and a versioned
`DeploymentActivationRecord` for ACTIVE deployments. It validates creator and
lifecycle authority, expiry, revision fence, and rollover/revocation chain.
This closes the missing APP protocol without adding a second NDNSF invocation
mode.

### F7 - Provider serving and lifecycle administration are mixed (Medium)

The provider surface includes serving, agent registration, stage, activate,
drain, delete, and receipt/reporting operations. It also exposes both `serve`
and `serve_service`. Application model authors face more authority-bearing
operations than they need.

**Decision**: one preferred serving verb. Keep lifecycle mutation and receipt
issuance on an explicit administrative port with existing authorization.

### F8 - Optimizer architecture is sound but contracts are weakly typed (High)

The SDK correctly preserves ten policy seams and a separate RunnerAdapter, and
supports partial suites over defaults. However, named request/result types are
aliases of one `PolicyRequest`/`PolicyResult` carrying `metadata`, public exports
leak helpers, and suite construction requires a caller-provided state digest.

**Decision**: preserve the seam inventory and runner separation. Add ten
dedicated typed request/result pairs, a partial-suite builder, and a
deterministically derived suite identity. Do not start algorithm redesign.

### F9 - Primary documentation contradicts canonical signatures (Critical)

The README recommends `APPClient.from_config(...)` followed by
`distributed_inference(service, input)`, while production construction requires
durable state and an envelope-key source and the canonical call requires a
deployment revision. A primary copy/paste example therefore does not describe
the actual canonical contract.

**Decision**: treat documentation snippets as executable tests in both English
and Chinese. Production security inputs remain mandatory and visible.

### F10 - The first draft risked a third facade implementation (High)

The code already has same-named APP wrappers and `app_sdk.facades` classes. A
new behavior-bearing `api/` package with “exact placement may be refined” would
make the one-owner problem worse.

**Decision**: behavior lives once in the canonical `app_sdk` role modules;
`api/__init__.py` is explicit re-export only. Existing same-named facade classes
become private collaborators or compatibility adapters, and canonical behavior
cannot rely on `__getattr__`.

### F11 - ACK/Selection do not close request-time deployment readiness (Critical)

The request protocol can select a Provider that is willing and capable before
its model artifacts are present or loaded. Existing execution-lease PREPARE
reserves capacity, while `ProviderReadiness` proves exact artifact, adapter,
revision, and boot-epoch readiness; these are not equivalent. The Core already
exposes `AckDecision.payload`, `ProviderCapabilityHint.operationStatus`, generic
`ServiceOperationStatus`, collaboration publication/waiting, a signed
`SELECTION-STATUS` Data reply, and signed readiness evidence. However, the
current status is only Received/Queued/Running/terminal, its Python
collaboration wrapper exposes no status query/report API, and
`prepareCollaborationAssignmentAsync` completes before `CollaborationContext`
is constructed. APP-only context events therefore cannot observe the
preparation gap.

**Decision**: keep normal messages semantically narrow. ACK carries a typed
NDNSF-DI deployment offer (`READY`, `NEEDS_PREPARATION`, `UNAVAILABLE`) but only
means willingness/current availability. Selection assigns role responsibility
but does not prove readiness. A shared ensure-deployment coordinator drives
selected Providers through fetch, verify, load, warm, and READY. The existing
execution certificate commits responsibility/membership/leases before this
work and explicitly does not assert readiness; exact signed readiness and an
all-role barrier gate model-runner invocation.
Extend the existing `SELECTION-STATUS` snapshot compatibly with generic
per-member `ServiceOperationStatus` entries, expose pre-context/context Provider
reporters and requester query/watch/wait bindings, and validate signed replies.
NDNSF-DI maps domain phases into bounded application details; it alone decides
exact readiness and the all-role barrier. Response remains the final inference
result. READY-first ranking remains optimizer policy rather than baseline
protocol correctness.

### F12 - Progress inspection is generic; model readiness is not (High)

Repo repair, UAV missions, streaming preparation, and distributed inference can
all run long enough that ACK/Selection followed by a final Response is
insufficient for diagnosis and bounded waiting. This is a Collaboration
observability concern and belongs in NDNSF. Qwen artifact digests, model shards,
RunnerAdapter identity, GPU residency, KV/cache state, and role-complete
readiness have no stable meaning outside NDNSF-DI and must not enter Core.

**Decision**: NDNSF owns generic operation identity, member binding, state,
optional progress, reason, sequence, freshness, signed snapshot query, and
watch/wait mechanics. NDNSF-DI owns `DeploymentProgress`, its phase vocabulary,
`ProviderReadiness`, model/GPU checks, and readiness aggregation. Reuse the
existing selection-status name and lifecycle instead of adding a second
Targeted status service. Event notification is optional; the latest validated
snapshot is authoritative.

### F13 - Remote on-demand planning needs an authority owner (Critical)

A `DeploymentDefinitionRef` lets a remote requester identify cold intent, but
the requester must not instantiate the Application's optimization profile or
reinterpret its constraints. Otherwise direct request silently transfers policy
and lifecycle authority and also requires every requester to install identical
optimizer code.

**Decision**: the signed definition/reference binds an Application-authorized
deployment coordinator identity/service. Remote request sends the frozen deployment
and input to that coordinator, which alone resolves policy and runs the shared
ensure operation. The requester retains request authority only. Coordinator
unavailability produces a bounded failure; it never enables local fallback.

## Accepted Public Shape

The preferred API is three roles plus two bound handles and one shared internal
ensure-deployment operation:

1. `InferenceApplication`: application-owned composition whose primary
   `request` prepares on demand; optional `deploy` prewarms through the same
   coordinator.
2. `InferenceClient`: requester-only ACTIVE/ON_DEMAND discovery and request.
3. `InferenceProvider`: model/runner serving; a separately constructed advanced
   admin port owns deployment lifecycle actions.
4. `DeploymentHandle`: bound operational view of one deployment revision.
5. `InferenceRequestHandle`: bound durable request view with typed result,
   events, and authoritative deployment status, preserving the Spec 111 term.

Existing `APPClient`, `APPDeployment`, and `APPProvider` remain advanced role
components and compatibility targets. The preferred classes are not new state
machines or authorities.

The two public request entry points are signature-identical views of one
implementation. `InferenceApplication.request` delegates to its configured
`InferenceClient.request`. The Application's additional authority is limited to
definition signing and optional lifecycle administration; it does not create a
different kind of distributed inference request.

## Alternatives Rejected

- **Only improve README**: rejected because duplicate meanings, dynamic exports,
  and detached discovery are source-contract defects.
- **Delete all old names immediately**: rejected because Spec 111 deliberately
  introduced a compatibility manifest and current examples depend on it.
- **Put policies inside DeploymentDefinition**: rejected because serialized
  executable Python creates supply-chain, reproducibility, and authority risks.
- **Merge RunnerAdapter into backend/provider selection**: rejected because
  choosing where/what to execute and executing model mechanics have different
  ownership and validation.
- **Hide state/key requirements with insecure defaults**: rejected because
  usability cannot override durable recovery and request-envelope security.
- **Require deploy/wait before every request**: rejected because it makes an
  optional latency optimization part of baseline correctness and prevents a
  request from realizing an authorized cold deployment.
- **Treat ACK or Selection as READY**: rejected because willingness,
  responsibility assignment, resource reservation, and exact executable
  readiness are distinct security/consistency facts.
- **Add generic NDNSF deployment-progress messages**: rejected. The reusable
  need is generic collaboration-operation status, implemented as an additive
  extension of existing `SELECTION-STATUS`/`ServiceOperationStatus`; deployment
  and model readiness remain APP semantics.
- **Keep all progress inside NDNSF-DI**: rejected because it cannot report the
  current pre-`CollaborationContext` preparation interval and would duplicate a
  capability needed by other long-running NDNSF collaboration users.
- **Give Application and remote clients different request signatures**:
  rejected because both issue the same distributed operation. A signed
  definition can be transported as a value or reference; authorship is enforced
  by signature/authorization, not by withholding the value type from the client.

## Validation Implication

Component unit tests are necessary but insufficient. Acceptance must execute:

```text
creator/client requests signed definition -> providers ACK availability
Selection/certificate commits roles and leases -> providers prepare
exact readiness barrier -> provider collaboration executes -> typed result
optional prewarm -> authenticated active revision announced and reused
remote client discovers ACTIVE or ON_DEMAND deployment -> requests same workflow
creator/client restart -> durable handles rebound
revision rolls over -> old request remains fenced
invalid evidence/cancellation -> fail-closed terminal state
```

Local deterministic tests validate composition quickly; MiniNDN remains the
authoritative network/security gate. iTiger, Docker, CUDA, and Qwen throughput
are deliberately deferred because they do not answer API coherence.
