# NDNSF Key Architecture

Maintained reference for agent task context. When a Core module boundary, a
wire/message flow, or the security model changes, update this file in the same
commit. Sources cited are `ndn-service-framework/` unless noted.

## Repository layout

| Area | Path | Role |
|---|---|---|
| Core framework | `ndn-service-framework/` | reusable NDN service runtime (see below) |
| Apps | `NDNSF-DistributedInference/`, `NDNSF-DistributedRepo/`, `NDNSF-UAV-APP/` | workload policies stay here, never in Core |
| Python binding | `pythonWrapper/` | SWIG wrapper over the C++ runtime |
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
  `8b462d0`, compatibility repair `b3b43c8`, not pushed). These patches change
  public class layouts: dependent C++ applications and language extensions
  require a matched header/library rebuild. The compatibility contract is in
  `specs/179-request-scoped-confidentiality/evidence/nac-abe-compatibility-review-20260905.md`.
  The T019 repair is also captured in
  `specs/179-request-scoped-confidentiality/evidence/nac-abe-late-callback-fence-20260905.patch`.

## NAC-ABE routing

Controller grants use `/PERMISSION/<service>` for service use by a User and
`/SERVICE/<service>` for service offering by a Provider. The Controller example
selects these explicitly with `--grant-additional-role` (default `user`). Both
role examples support application-owned delayed permission discovery through
`NDNSF_PERMISSION_REFETCH_AFTER_MS`; Controller policy mutation alone does not
mean the role has installed new permission/key material. A newly eligible
Provider also requires Users to renew their provider permission table before
targeting it. This is the existing discovery contract, not a new wire mode.

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
