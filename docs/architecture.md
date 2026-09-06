# NDNSF Architecture

Before changing a cross-layer runtime, DI, deployment, or validation path,
follow [`architecture-reading-guide.md`](architecture-reading-guide.md) and
inspect the newest entry in [`failure-log.md`](failure-log.md). This document
is the concise ownership map; the active Spec, contracts, source, and evidence
remain authoritative for a particular feature.

NDNSF is organized as a framework core plus application layers that validate
and stress the core mechanisms.

```text
Applications
  UAV workload, distributed inference, repo clients, examples

NDNSF application packages
  NDNSF-DistributedInference
  NDNSF-DistributedRepo
  pythonWrapper

NDNSF core runtime
  ServiceController
  ServiceProvider
  ServiceUser
  ServiceContainer
  LocalServiceRegistry

NDN dependencies
  ndn-cxx, NFD, ndn-svs, NDNSD, NAC-ABE/OpenABE
```

## Core Runtime

The core runtime lives in `ndn-service-framework/`. Its primary public actors
are:

- `ServiceController`: distributes controller-signed permission responses.
- `ServiceProvider`: registers services, emits ACKs, handles selections, runs
  service handlers, publishes responses, and supports collaboration context.
- `ServiceUser`: prepares requests, collects ACKs, applies selection policy,
  sends selection messages, and receives responses.
- `ServiceContainer`: composes users, providers, and trusted local services in
  one process.
- `Stream`: app-neutral stream/session/chunk helpers for services that need a
  control invocation plus a named Data stream.

The standard runtime flow is:

```text
permission bootstrap
  user/provider fetch encrypted permissions from ServiceController

request
  user publishes RequestMessage under the V2 request namespace

ACK
  authorized providers reply with RequestAckMessage and provider token

selection
  user chooses one or more ACK candidates after the ACK window

response
  selected provider executes and publishes ResponseMessage
```

For live or long-running data streams, the service invocation should normally
carry control information such as start/stop/status and return a stream prefix.
The high-rate data path can then use signed named Data chunks under that prefix.
The reusable stream/session/chunk helpers live in `ndn-service-framework/Stream.*`;
application codecs, tensor formats, and GUI behavior stay in the application.

### Runtime Transfer Boundary

NDNSF has two reusable data-transfer surfaces, and applications should choose
between them by semantics rather than by byte size alone:

| Need | Runtime API | Naming model | Typical data |
| --- | --- | --- | --- |
| Continuous publication where new chunks keep arriving and late data may be less useful | `StreamInfo`, `StreamChunk`, stream buffers, stream fetch state | A stream prefix plus sequence numbers and session metadata | live UAV video, telemetry feeds, logs, status streams |
| Exact retrieval of one complete named object | Core large-data references, `publishLargeNamed(...)`, `fetchLarge(...)`, `fetchLargeExact(...)`, SegmentFetcher-style retrieval | A deterministic Data name or versioned object name | model artifacts, recorded video objects, manifests, DI tensor bundles |

Use the stream substrate when the application needs stream state: sequence
gaps, duplicate suppression, reordering, freshness, FEC metadata, or live
consumer buffering. Use the large-data path when the producer already knows the
object name and the consumer wants that exact object, even if the object is
large or segmented. A recorded video file is therefore large data; a live video
feed is a stream. A DI activation tensor with a planned dependency name is
large data; a future token-by-token LLM output feed would be a stream.

The two surfaces can be combined by an application, but one should not be used
as a vague replacement for the other. Stream chunks may carry application
frames or metadata for ongoing publication; they should not replace exact-name
SegmentFetcher retrieval for files, model chunks, or deterministic dependency
objects.

## Naming Direction

New code should use one unified `serviceName`:

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
/HELLO
```

Avoid designing new APIs around split `ServiceName + FunctionName`.

## Invocation Modes

- **Normal service invocation**: full request, ACK, selection, and response
  path.
- **Targeted invocation**: known-provider low-latency path that still uses
  permission checks, request/response messages, replay protection, and
  one-time tokens. It skips ACK/selection only after Targeted token bootstrap.
- **Trusted local invocation**: same-process helper through
  `LocalServiceRegistry`. It is not a network mode and must not add new wire
  names, NAC-ABE attributes, or externally selectable request modes.

## Distributed Inference Native Path

The native DI path is under `NDNSF-DistributedInference/cpp/ndnsf-di/`.

Key boundaries:

- `NativeExecutionPlan`: service roles and dependency edges.
- `NativeProviderAssignment`: role-to-provider mapping.
- `ProviderRoleWorker`: prefetch inputs, run one role, publish outputs.
- `NativeProviderRuntime`: worker pool and role runner registry.
- `NativeProviderSession`: plan + assignment + dependency I/O + runners.
- `NativeProviderHandler`: adapts the native session to
  `ServiceProvider::CollaborationContext`.

This path is the performance direction for distributed inference. Python should
remain a planning, deployment, GUI, and experiment layer.

## Accepted Core/Application Ownership

The Spec 084 simplification program established one owner per concern:

- Core owns V2 normal/Targeted invocation, security, typed capability and
  operation envelopes, provider-owned leases, exact large-data transfer,
  continuous stream state, discovery facts, and provider-pair telemetry.
- DistributedInference owns model planning, fragments, caches, runtime
  lifecycle, dependency dataflow, and bounded replanning after lease rejection.
- DistributedRepo owns exact packet persistence, manifests, catalog, quorum,
  repair, and replica placement.
- UAV owns MAVLink, mission safety, operator authority, codecs, ROI, FEC policy,
  and ground-station workflow.

There is no generic Core advisory coordinator. Each DI user may plan
independently, but only a provider-owned fail-closed lease authorizes exclusive
execution. See [Core/App Boundary](ndnsf-core-app-boundary.md) for the complete
ownership and compatibility rules.

## Validation and failure ownership

Implementation checks, live local runs, immutable SIF replays, and Tiger jobs
are different evidence planes. A failure in an earlier plane invalidates any
later claim that depends on it; a passing focused check does not silently open
the next plane. The active Spec records the gate order, while the repository
[`failure-log.md`](failure-log.md) records the newest failed or unqualified
boundary and the next permitted action.
# NDNSF-DI Core/APP separation (Spec 111)

NDNSF-DI Core owns immutable execution contracts, eligibility, final proposal
validation, authenticated lease/certificate fencing, attempt epochs and result
authority. It does not import APP, Planner, SDK, ONNX, Qwen or operations code.
The process-local `DistributedInferenceEngine` is APP-owned and invokes ten
independently replaceable Python policies over an objective and one immutable
snapshot. Named Planner defaults use exactly the same SDK seams as external
packages. `RunnerAdapter` is an independent execution mechanism SPI and
`OptimizationObserver` is optional, idempotent and off-path.

Per-request placement is carried by immutable `AssignmentContext`; environment
variables are not placement authority. Deployment and durable request state is
owned by the APP RuntimeJournal. Model weights remain external artifact
references and are never included in owner wheels or container layers.
## Integrated Authorization and UAV Mechanisms

Maintained reference for agent task context. When a Core module boundary, a
wire/message flow, or the security model changes, update this file in the same
commit. Sources cited are `ndn-service-framework/` unless noted.

## Repository layout

| Area | Path | Role |
|---|---|---|
| Core framework | `ndn-service-framework/` | reusable NDN service runtime (see below) |
| Apps | `NDNSF-DistributedInference/`, `NDNSF-DistributedRepo/`, `NDNSF-UAV-APP/` | workload policies stay here, never in Core |
| Python binding | `pythonWrapper/` | pybind11 wrapper over the C++ runtime |
| Examples | `examples/` | App_User / App_Provider / App_ServiceController |
| Tests | `tests/unit-tests/`, `tests/integration-tests/`, `tests/minindn/`, `tests/python/` | unit → integration → MiniNDN order |
| Specs | `specs/<n>-<slug>/` | spec/plan/tasks/contracts/evidence per Spec |
| Docs | `docs/` | architecture, failure log, boundary and workflow docs |

## Runtime roles

- **ServiceUser** (`ServiceUser.cpp/.hpp`): discovery, Request/Selection
  publishing, response fetch/decrypt, Targeted token consumption, streams.
  Holds `MessageValidator`, NAC-ABE `Consumer`, handler pools, IMS.
- **ServiceProvider** (`ServiceProvider.cpp/.hpp`): ACK, Selection processing,
  handler execution, Response publication, Targeted token issuance, streams.
  Holds NAC-ABE `Producer`, execution-lease table, pending-request state.
- **ServiceController** (`ServiceController.cpp/.hpp`): the authorization
  authority — durable `ControllerVersion` generation, revocation targets,
  permission issuance, status/revocation Data publication, ABE attribute
  policies (`AttributeAuthority`).

## V2 message flow (normal path)

```text
User                                Provider(s)                     Controller
 |--- Request (encrypted discovery) --->|                               |
 |<--------- ACK (cert ad, version) ----|                               |
 |--- Selection (K_input/K_response, -- >|  envelope bound to selected   |
 |     encrypted Input Data name)        |  provider cert + ControllerVersion
 |                                        |--- execute handler ----------|
 |<-------- Response (AEAD, K_response)-|                               |
 |                                        |<-- status/permission refresh-|
 |                                        |    (PolicyStatusData, DKEY)  |
```

- Naming helpers: `makeResponseNameV2`, `makeResponseNameWithoutPrefixV2`
  (`NDNSFMessages.*`); publication via `PublishMessage`.
- Targeted variant (`tlv::TargetedBootstrapRequest`,
  `attachTargetedTokenBatch`) keeps a token fast path with bounded
  bootstrap/ACK/Selection and recipient-bound key envelopes.
- Streaming (Spec175, `InvocationStream.*`): START/STOP lifecycle with
  per-event version binding; terminal authority fenced at completion.

## Request-scoped confidentiality (Spec179, default V2 protected mode)

- Fresh `K_input`/`K_response` per invocation after Selection; Input Data is
  an exact-named User-signed Data packet encrypted under `K_input`.
- `K_input`/`K_response` travel only inside the selected Provider's
  RSA-OAEP `SelectionKeyEnvelope` (`RequestConfidentiality.*`,
  `SelectionKeyEnvelope` in `NDNSFMessages.*`).
- Responses are AES-GCM (`AeadEnvelope`) with AAD bound to service, request
  ID, both ControllerVersion fields, attempt, cert digests; one
  `K_response` per invocation with unique per-segment/event nonces
  (`NonceRegistry` rejects reuse; keys zeroized at terminal).
- Large responses: per-segment AEAD reference
  (`makeRequestScopedResponseWithLargeDataOptimization`,
  `resolveLargeResponseReferencePayload`) with configured trust-schema
  segment validation. Without request-scoped state, large responses fail
  closed with typed errors — the service-wide response-key carrier was
  removed (Spec179 T012).
- HybridMessageCrypto remains for non-request-scoped service-level paths
  (collaboration, tokens).

## ControllerVersion authority and revocation (Spec179)

- User/Provider construction starts NAC-ABE bootstrap asynchronously. A missing
  first DKEY cannot block the App from running its Face or renewing permissions
  after an online grant. Bootstrap-pending is not authority: protected work
  still requires permission, current signed status and valid key material.
  Real User Request admission explicitly checks initial Consumer readiness;
  an accepted permission response cannot bypass a pending first DKEY fetch.
  Hybrid unwrap errors remain wired independently of the success/AES closure.

- `ControllerVersion` = two uint64 fields (generation timestamp + epoch);
  both are signed or covered by AEAD AAD; message-carried versions are
  hints only — authority is the Controller-signed `PolicyStatusData`
  fetched from the exact version-addressable name
  (`PolicyRefreshCoordinator.*` fences: signature gate, conflicting
  equal-version rejection, in-flight coalescing, bounded retry).
- Durable state: `ControllerGenerationStore.*` — writer lease, atomic
  persistence, monotonic generation, restart/clock-rollback safe.
- Revocation targets: identity, certificate, or exact
  `/PERMISSION/<service>` (User use) vs `/SERVICE/<service>` (Provider
  provision), validated by `RevocationTarget`/`AuthorizationSubject`.
- Withdrawal rotates the global ABE master/public-parameter generation and
  reissues filtered DKEYs; **grant-only** changes advance ControllerVersion
  but keep the same public parameters and replace only the granted
  identity's DKEY policy (DKEY-only refresh fence, atomic replacement
  install). Runtime install is atomic per service
  (`RevocationState.*`); affected caches invalidate by service/binding.
- The single target-only DKEY refresh of a grant-only wave fires exactly
  once per armed permission renewal regardless of arrival order: it is
  issued on a version-advance install and also when a later equal-version
  install consumes the pending entry left by the permission response
  (shared `refreshNacDkeyForControllerStatus` helper, User/Provider
  mirrors). Grant discovery is App-driven — the runtime never polls
  permission records; an applied App-layer renewal (or explicit
  `fetchPermissionsFromController` refetch) arms the refresh.
- If the withdrawal's ABE rotation throws, the revocation stays enforced in
  memory and in every published status (fail-closed), the rotation is
  recorded pending, and the next `revoke()` (including same-target retry) or
  `grant()` entry reconciles it before changing policy. Each recovery attempt
  reserves a newer durable ControllerVersion before rotating, preserving
  immutable failure statuses already accepted by peers. A still-failing retry
  preserves the withdrawal and refuses the grant;
  after restart the durable epoch re-derives the ABE generation.
- NAC Consumer content and CK fetch completion/error callbacks retain the
  cache generation from admission and stop after invalidation. Clearing DKEY
  and cache state alone cannot fence an already-running fetch. The algorithm
  boundary converts OpenABE enum decode failures into `NacAlgoError`, allowing
  runtime error handling to retain control instead of terminating the process.
- CK fan-out detaches its waiter batch before calling applications and checks
  generation between waiters, so reentrant invalidation/retry cannot corrupt
  the queue. Invalidation silently cancels old consumptions; NDNSF retains
  terminal-event/timeout ownership. Parameter fetch, retry and validation
  callbacks likewise retain their generation; name/digest-checked candidates
  replace installed material only after validation. Authority responses name
  the actual current generation. The shared decrypted-CK cache binds scheme,
  public parameters, private key and encrypted CK to preserve caller authority.
- Local NAC-ABE dependency patches (DKEY FreshnessPeriod=0, versioned
  exact public-params fetch, consumer cache invalidation and delayed-callback
  fencing) live on the NAC-ABE `Experimental` branch (base `b1c9c4f`, repair
  `8b462d0`, compatibility repair `b3b43c8`, official merge `c3aafa6`, not pushed). These patches change
  public class layouts: dependent C++ applications and language extensions
  require a matched header/library rebuild. The compatibility contract is in
  `specs/179-request-scoped-confidentiality/evidence/nac-abe-compatibility-review-20260905.md`.
  The T019 repair is also captured in
  `specs/179-request-scoped-confidentiality/evidence/nac-abe-late-callback-fence-20260905.patch`.
- T022 merges official UCLA-IRL master58f3948 without adding public header or
  layout changes beyond the existing repaired dependency. CacheProducer now
  forwards non-default segment limits for new CK/content objects; cached CK
  packetization persists until explicit clear. Consumer preserves complete CK
  object names, including typed segment components, preventing cache aliasing.
  The matched `.deps/nac-abe-spec179-official` / `build-clang-spec179-official`
  pair passes NAC46, NDNSF183 unit/74 integration and all18 MiniNDN scenarios.
  Evidence: `specs/179-request-scoped-confidentiality/evidence/nac-abe-official-merge-20260905.md`.

## NAC-ABE routing

Controller grants use `/PERMISSION/<service>` for service use by a User and
`/SERVICE/<service>` for service offering by a Provider. The Controller example
selects these explicitly with `--grant-additional-role` (default `user`). Both
role examples support application-owned delayed permission discovery through
`NDNSF_PERMISSION_REFETCH_AFTER_MS`; Controller policy mutation alone does not
mean the role has installed new permission/key material. A newly eligible
Provider also requires Users to renew their provider permission table before
targeting it. This is the existing discovery contract, not a new wire mode.
Provider status advances also trigger permission revalidation; an App timer
need not be the first post-grant fetch. User DKEY refresh compares authorized
service sets because an additional Provider route for a held service does not
change that User's ABE attribute policy.

- NAC-ABE protects service-level confidential discovery before Provider
  selection only. `/PERMISSION/<service>` attributes authorize Users for
  ACK/Response; `/SERVICE/<service>` attributes authorize Providers for
  Request/Selection.
- Public parameters are named
  `<AA-identity>/PUBPARAMS/<ABE-TYPE>/v=<version>`;
  `ParamFetcher` binds expected Data name/digest for status-bound exact
  retrieval. DKEYs are versioned segmented Data; discovery Interests are
  unversioned and `MustBeFresh`.

## Trust and signing

- `MessageValidator(trustSchemaPath, group_prefix, &face)` validates all
  received Data; SVS publications carry V03 signed interests.
- Identities hold separate encryption (RSA, for NAC-ABE) and signing
  (ECDSA) certificates; signing uses `signingByCertificate(signingCert)`.
- A message hint is never authority; acceptance requires the exact
  Controller-signed status through the configured trust schema.

## Concurrency and state

- Per-role handler pools (`m_handlerPool`, ACK-processing pool) plus the
  face's io_context; pending-request maps guarded by
  `m_pendingRequestMutex`; per-invocation nonce registry; stream terminal
  authority fencing for at-most-once completion.

## Boundary rules (Core vs App)

- Core owns mechanism: discovery, naming, crypto, revocation, refresh,
  streams. App owns policy: workloads (UAV detection/recognition,
  text-to-image, DI scheduling), codecs, sensors. No workload-specific
  branch may enter `ndn-service-framework/`.
- See `docs/ndnsf-core-app-boundary.md` for the detailed boundary contract.
