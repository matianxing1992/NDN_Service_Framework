# Spec179 Revocation Validation Matrix

**Status**: Normative inventory with an initial executable component slice.
Rows remain pending until the named test executes through the real behavior
described for its layer. A mocked unit result cannot be promoted to component
integration or network evidence.

**Execution state 2026-09-04 (T010–T013)**: the authoritative per-row executed
cases are the `## Executed-case mapping (2026-09-04, T010)` tables below
(RV-U/RV-I rows, security-critical negative branches, and the MiniNDN
network-only halves). The narrative sections earlier in this file are
historical 2026-09-01/09-02/09-03 snapshots and are superseded by that mapping,
by `evidence/minindn-campaign-20260904.md` (T011), and by
`evidence/release-gate.md` (T013) wherever they still say a row is pending or
unrun. RV-U12 was amended for the T012 removal of the service-wide
response-key carrier and its compatibility switch/counters.

## Executed slice (2026-09-02; current-source focused rebuild)

> **Superseded by the 2026-09-04 executed-case mapping** (`## Executed-case
> mapping (2026-09-04, T010)` below) and `evidence/release-gate.md`. Status
> cells in this frozen 2026-09-02 checkpoint that say "remains open" /
> "remain pending" for items closed on 2026-09-04 must be read against the
> 2026-09-04 mapping; the three residual items (NAC-ABE internal cache
> renewal, persistent runtime-cache restoration, production live status
> installation under a configured file trust anchor) are formally recorded
> as deferred non-goals in `spec.md` `## Out of Scope` (2026-09-04).

The following tests now execute against the C++ implementation (not a text-only
fixture):

- `tests/unit-tests/controller-revocation-policy.t.cpp`: 31 source cases
  covering
  canonical version ordering/round-trip, typed status and scope validation,
  invalid intervals/digests/duplicates and malformed revocation-target wire,
  all six protected transitions, identity/certificate/service revocation,
  cache-family invalidation, forward-only reauthorization, message field
  round-trip and zero/duplicate ControllerVersion rejection,
  restart/clock rollback, single-writer fencing, corrupt-state
  fail-closed behavior, complete target validation, non-canonical field order,
  same-identity multi-role withdrawal, invalid-status replacement preservation,
  and missing-status / incomplete-subject / expiry fail-closed reasons, future
  validity windows, global certificate revocation, conflicting equal-version
  status, repeated starts under clock rollback, stale-writer fencing, and
  global certificate matching across identities and service scopes, bound
  certificate non-over-revocation, and exact validity-end rejection. A
  current-source focused executable relinked with the matching
  `NDNSFMessages.cpp` object passes all 31 cases, including exact
  `validFrom`/`validUntil` wire preservation and half-open boundary validation.
  `GenerationStoreRejectsInvalidWriterAndStartInputs` also verifies that
  missing writers, empty owners, zero timestamps, and pre-start epoch
  advances fail closed without creating authority. This is a focused
  Controller-policy gate; it is not a claim that the
  monolithic all-project Waf unit target links on this host.
  The focused executable uses the current source objects with system Boost
  1.71/ndn-cxx linkage; its `ldd` closure resolves Boost libraries to 1.71.0.
- `tests/unit-tests/controller-revocation-state.t.cpp`: 11 focused cases
  covering unauthenticated-hint rejection, authenticated-hint/no-status
  startup, highest-candidate coalescing, hintless pre-expiry scheduling,
  bounded invalid-status retry, stale and conflicting equal-version rejection,
  service/missing-status fail-closed behavior, exact version-addressable
  PolicyStatus names, accepted-status cache-family invalidation, same-status
  idempotence, and cross-service rejection. The no-status case proves that a
  message hint starts only
  a bounded exact fetch; an invalid result exhausts retries without installing
  authority, while a later valid exact result is the first accepted authority.
- `tests/integration-tests/controller-revocation-flow.t.cpp`: 34 component
  integration cases are present in the checked-in source, covering paired unaffected controls, certificate
  replacement, service-scoped Provider revocation at all six cut points,
  cross-service non-interference, equivalent Controller/Provider/cache status
  copies in the state model, exactly-one terminal recording, Controller restart with clock
  rollback, equal-version operation until signed status expiry, real
  `ServiceController` authority mutation/issuance-state, status wire
  round-trip and policy-snapshot filtering checks, direct policy-status handler
  refusal for malformed/unavailable requests, immutable version-addressable recipient-encrypted
  permission issuance with wrong-recipient rejection, identity-wide and
  certificate-only zero-grant denial snapshots plus a same-service unaffected
  Provider renewal control, global certificate-only digest scope across
  identities and services, status wrong-signer/content-tamper
  negatives, the real LocalMock User/Provider normal Request
  publication/execution revocation boundary, certificate-only/service-scoped
  runtime withdrawal and reauthorization, ParametersSha256Digest normalization,
  Controller status-version monotonicity across repeated restarts, and fail-closed behavior when
  its durable generation state is corrupt. The live renewal case additionally
  relays Controller-signed permission and PolicyStatus Data through runtime
  Faces, verifies exact ControllerVersion refresh after empty revoked renewals,
  and denies User publication and Provider execution. The established
  executable passes all 34 cases, including the real Controller target-kind ×
  six-cut-point matrix with cross-service and replacement controls. The
  restart/status case also rejects an older exact status name after a new
  generation starts. The stale exact-status case also proves that a revoked
  epoch and an unavailable forged future epoch cannot be served. The runtime
  scope case also rejects an old message-carried ControllerVersion at Provider
  execution without a second handler invocation. The
  current-source focused executable, relinked with the
  current Controller-flow object, passes all 34 cases, including the repeated
  exact-status publication and timestamped-snapshot assertions.
- `tests/integration-tests/controller-version-refresh.t.cpp`: 1 component
  integration case composing a real `ServiceController` status snapshot with
  `PolicyRefreshCoordinator` and `RevocationState`; it covers hintless
  pre-expiry refresh, single-flight coalescing, status wire round-trip,
  higher-version revocation installation, and paired affected/unaffected
  authorization decisions.
- `tests/unit-tests/invocation-stream-message.t.cpp`: the current-source
  focused executable passes 16/16 cases, including two ControllerVersion
  stream-binding regressions. They verify optional version-wrapper
  round-trip/canonical encoding, legacy versionless decoding, invalid-version
  rejection, and that changing the version changes the authenticated
  stream-binding digest.
- `tests/integration-tests/request-scoped-selection.t.cpp`: the current-source
  focused executable passes 3/3 cases. The existing 9,000-byte response covers
  request-scoped large-response reconstruction, cross-user requester-name
  rejection, wrong-provider transport-evidence rejection, stale
  ControllerVersion rejection, and Selection replay rejection;
  `ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery` flips one byte
  in an inline response envelope, observes authentication rejection before
  delivery, and then confirms that the untouched response still completes.
  `RevokedUserCannotReceiveResponseAfterProviderExecution` installs a newer
  revoking status after Provider execution and verifies that User response
  delivery is denied with a single timeout and no application callback.
- `tests/integration-tests/request-scoped-response-confidentiality.t.cpp`: the
  dedicated response-confidentiality gate fetches a retained request-scoped
  large-response segment through the Provider IMS, validates its Provider
  signature through the configured trust schema, checks the exact segment
  binding and unique nonce, and rejects ciphertext, Provider-certificate, and
  ControllerVersion/AAD changes. It complements rather than duplicates the
  full reconstruction/selection flow above; end-to-end configured
  User/Provider trust installation and all segmented retry cases remain
  separate gates.
- `tests/integration-tests/invocation-stream-flow.t.cpp`: the current-source
  `Spec175InvocationStream` executable passes 19/19 cases. The added
  `StreamEventAfterUserRevocationIsRejectedBeforeDelivery` case lets the first
  event reach the application, installs a newer identity-revoking status from
  that callback, and verifies that an already-buffered later event is rejected
  before its application callback. This closes the LocalMock final-delivery
  race. The `TargetedStreamRevocationStopsBeforeBootstrap` case now exercises
  the User-side Controller discovery gate before Targeted token/bootstrap
  preparation. `StreamEventAfterProviderRevocationIsRejectedAtPublication`
  installs a newer Provider-revoking status after the first event and verifies
  that the next event fails at the Provider Face commit point rather than
  being reported as a successful publication. Configured trust-schema,
  restart/rejoin, Targeted token/refill, and MiniNDN stream propagation remain
  separate gates.
- `tests/unit-tests/generic-dynamic-api-targeted.t.cpp`: the focused Targeted
  executable passes 27/27 cases. In addition to the existing revoked-subject
  checks, the new service-scope cases prove that a newer accepted status evicts
  only the affected User pool and Provider token set; the unaffected service
  retains its fast path. The existing revoked-User case also proves that a
  withdrawn pool cannot be consumed or refilled, and the revoked-Provider case
  proves that withdrawal cannot trigger a second handler execution.
- `tests/minindn/test_request_scoped_confidentiality.py`: 3 non-privileged
  contract tests pass. They validate the tracked topology, the named
  representative cross-process scenarios, their coverage tags, and
  the rule that `--execute` must return a readiness/root result rather than
  fabricate cross-process revocation evidence. This is a launcher contract,
  not a MiniNDN network result.

These results close only the deterministic/component portions exercised by
those cases. They do **not** close production-runtime trust-schema installation or full
runtime certificate-only/unaffected renewal enforcement, persistent
runtime-cache recovery, cache-source equivalence,
  configured live stream trust/restart propagation, or the MiniNDN rows
below. The normal request-scoped large-response component path now passes; the
  current focused Controller, refresh, and crypto binaries pass 31/31,
  11/11, and 13/13 cases respectively. The current Controller component
  executable passes 34/34 cases. These remain component results, not evidence
  for production-runtime trust-schema delivery or the MiniNDN network matrix. The
  request-scoped large-response component run also passes after fixing shared
  ownership of IMS-retained segments. This remains component evidence only;
  production-runtime trust-schema, Targeted/stream revocation, and MiniNDN delivery are
  still separate gates.

## Coverage Model

## Controller test-family index

The following IDs are the normative bridge between the Controller withdrawal
requirements and executable tests. They intentionally separate deterministic
authority logic from runtime and network evidence.

| ID | Layer | Test obligation | Representative entry points | Current status |
|---|---|---|---|---|
| CU-01 | Unit | generation/epoch mutation, duplicate/idempotent and malformed targets, rollback | `PolicyStatusRequiresTypedRevocationTargets`; `PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates`; `RevocationTargetValidationMatrix`; `GenerationStoreRejectsInvalidWriterAndStartInputs` | partial: target, input, and epoch cases execute; direct temporary-write/rename injection remains open |
| CU-02 | Unit | identity/certificate/service scope, role pairing, replacement and unrelated controls | `RevocationStateDistinguishesCertificateAndServiceScope`; `BoundCertificateRevocationDoesNotOverRevokeAnotherIdentity`; `IdentityRevocationAppliesToEveryRoleForOneIdentity`; `RevocationStateSupportsForwardOnlyReauthorization` | covered in the current policy/state slice |
| CU-03 | Unit | canonical status wire/name, validity boundaries, zero/stale/equal-conflicting/new replacement, signature inputs | `PolicyStatusWirePreservesValidityTimestamps`; `RevocationStateRejectsAtStatusValidityEnd`; `PolicyStatusWireRejectsNonCanonicalFieldOrder`; `ProtectedMessagesRejectZeroOrDuplicateControllerVersion`; `EqualVersionConflictingStatusCannotReplaceAuthority`; `InvalidStatusCannotReplaceLastAcceptedAuthority`; `ConfiguredTrustSchemaControlsControllerStatusValidation` | covered for structural, explicit-signer, and configured file trust-anchor validation; runtime User/Provider installation under the production schema remains open |
| CU-04 | Unit | persistence, restart, rollback, corrupt state, writer fencing and lost lease | `GenerationPersistsAcrossRestartAndClockRollback`; `RepeatedControllerStartsRemainStrictlyMonotonic`; `OnlyOneWriterCanPublish`; `LostWriterLeaseCannotPublishWithStaleFence`; `CorruptGenerationStateFailsClosed` | covered for local store/Controller construction; live multi-process issuance remains open |
| CU-05 | Unit | issuance filtering, cache families, replay/in-flight cleanup and redacted reasons | `RevocationStateCoversAllProtectedTransitions`; `RevocationStateInvalidatesOnlyAcceptedServiceFamilies`; `RevocationStateReportsRedactedTypedReasons`; `RevocationStateFailsClosedForMissingOrInvalidSubject` | partial: accepted-status cache invalidation, same-status idempotence, and typed reasons execute; persistent runtime-cache recovery remains open |
| CU-06 | Unit | authenticated hints, scheduled/reconnect refresh, bounded coalescing/retry and fail-closed expiry | `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`; `HigherHintsCoalesceToOneHighestCandidate`; `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; `ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight`; `InvalidStatusRetriesWithBoundedAttempts`; `RevocationStateInvalidatesOnlyAcceptedServiceFamilies` | partial: 11/11 current refresh/state cases execute, including no-status exact-fetch gating and cache-scope rejection; live runtime reconnect/expiry remains open |
| CI-01 | Component | real Controller mutation/publication, exact status retrieval, stale/forged exact-name refusal, signature and malformed/unavailable refusal | `ServiceControllerMutatesTypedRevocationAndAdvancesEpoch`; `ServiceControllerPublishesStableTimestampedRevocationSnapshot`; `ServiceControllerRejectsStaleExactStatusAfterRevocation`; `ServiceControllerRejectsInvalidRevocationTargetsWithoutEpochAdvance`; `ServiceControllerStatusHandlerPublishesSignedCurrentStatus`; `ServiceControllerStatusHandlerRejectsMissingService`; `ConfiguredTrustSchemaControlsControllerStatusValidation` | covered for Controller publication, explicit configured file trust-anchor acceptance, and wrong-signer rejection; production User/Provider status installation and MiniNDN delivery remain open |
| CI-02 | Component | recipient-bound permission snapshots, wrong recipient, target-specific renewal denial and unaffected controls | `ServiceControllerPermissionHandlersEncryptCurrentRevocation`; `ServiceControllerGlobalCertificateRevocationUsesDigestScope`; `LiveControllerStatusRefreshRejectsRevokedRenewal`; `RealRuntimeEnforcesCertificateAndServiceRevocation` | partial: identity/service and certificate-only empty-renewal snapshots, Provider identity execution denial/reauthorization, same-service unaffected and global-digest controls execute; production-runtime trust-schema installation and full runtime enforcement remain open |
| CI-03 | Component | real User/Provider normal Request lifecycle before/after withdrawal, at-most-once execution, and stale message-version rejection | `RealUserAndProviderEnforceControllerRevocationAtRuntime`; `RealRuntimeEnforcesCertificateAndServiceRevocation` | covered for normal LocalMock request/publication/execution and an old message-carried version rejected before a second Provider execution; cryptographic live path remains open |
| CI-04 | Component | every target-scope rule, enforcement owner, and distinct normal/large/Targeted/stream cache or terminal path with controls | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`; `IdentityRevocationDeniesEveryCutPointForBothRoles`; `RealTargetedProviderRejectsRevokedExecution`; `TargetedStreamRevocationStopsBeforeBootstrap`; `RevokedUserCannotConsumeTargetedTokenOrStartRefill`; `StreamEventAfterProviderRevocationIsRejectedAtPublication` | partial: the modeled target/cut-point decisions and live normal/Targeted/Provider-stream boundaries plus the User token/refill pre-gate execute; large-response revocation, Targeted token/refill response handling, configured stream enforcement, and several paired runtime paths remain open |
| CI-05 | Component | atomic per-service install, cross-service hint isolation, stale-status replay, no-status startup, restart/unavailable expiry and reauthorization | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `ConfiguredControllerFailsClosedBeforeStatusInstallation`; `LiveControllerStatusRefreshRejectsRevokedRenewal`; `ServiceControllerStatusVersionIsMonotonicAcrossRestarts`; planned `NewerServiceStatusDoesNotSuppressOtherServiceRefresh` | partial: no-status, hint non-adoption, stale-status replay rejection, cache invalidation after an accepted Controller version, wrong-service status rejection, and one signed renewal path execute; atomic install and cross-service runtime version isolation remain blocking |
| CI-06 | MiniNDN | representative cross-process propagation, loss/reordering, cache-source, restart/offline/rejoin, and one scenario per distinct response-mode cache path | `tests/minindn/run_request_scoped_confidentiality.py` + `spec179-topology.conf` | real launcher and preflight covered; privileged network execution/evidence pending |
| CI-07 | Component/MiniNDN | execution/key disclosure ordering, terminal ownership, counters and redacted traces | release-gate trace collector (planned) | pending |

The IDs are not additional optional examples. A family is `covered` only when
the named executable has run against the current source/build and produced the
required assertions; a checked-in test body or syntax-only compile is
`implemented`, not `covered`. The final release gate must report every ID and
must not collapse CI-04 into CI-06.

The matrix inventories these independent dimensions. Unit tests exhaust pure
decision branches. Component tests use a documented pairwise/risk mapping so
every target rule, enforcement owner, cache family, terminal owner, and
recovery implementation executes at least once. MiniNDN samples only the
cross-process properties. An omitted combination is acceptable only when the
matrix names the executed equivalent implementation path.

| Dimension | Required values |
|---|---|
| Revoked subject | User identity; Provider identity; one certificate; User `/PERMISSION/S`; Provider `/SERVICE/S`; one dual-role identity retaining the opposite attribute |
| Local version relation | equal; newer; older; zero; forged high; skipped but valid newer generation/epoch |
| Invocation cut point | before Request; Request/ACK; ACK/Selection; Selection/execution; execution/Response; active stream |
| Invocation mode | normal; large Response/reference; Targeted bootstrap/fast path/refill; segmented/streaming |
| Cached material | ABE key; permission snapshot; MessageKey; Targeted token/refill; Selection binding; nonce/replay state; incomplete request |
| Status source | Controller; Provider; NDN cache; unavailable/timeout; invalid signer; expired Data |
| Refresh trigger | newer-version message; scheduled pre-expiry refresh without a peer hint; reconnect/manual recovery |
| Initial authority state | no installed status; current authenticated status; expired status; corrupt/unavailable Controller state |
| Controller lifecycle | policy update; sequential restart; clock rollback; lost/corrupt state; competing writer; lost lease |
| Recovery | unaffected continuation; offline rejoin; bounded refresh; reauthorization; process restart |

The executed unit suite covers the deterministic decisions implemented in the
  current revocation slice. The timestamp, message-version, bound-certificate,
  and validity-end regressions now execute as part of the current-source
  31-case policy inventory (the available focused executable must be rebuilt
  after any later source edit before its count is treated as current).
  focused policy suite. The executed
C++ integration suite is a component
integration layer: seven cases compose the version/status/store/state objects
and check paired affected/unaffected outcomes; the remaining 26 cases construct real
`ServiceController`, `ServiceUser`, and `ServiceProvider` objects and exercise
authority mutation, corrupt-generation
fail-closed behavior, status wire round-trip, revocation restoration after
restart, revoked User/Provider permission-snapshot filtering, direct
policy-status handler refusal, recipient-bound permission issuance,
  wrong-signer/content-tamper status negatives, ParametersSha256Digest
  normalization, and the normal runtime Request publication/execution
  boundaries. Two additional cases compose the real Controller status source
  with refresh bookkeeping and RevocationState. The runtime cases use
  `LocalMockTag` only to
avoid NFD/bootstrap; it still calls the public install/status, RequestService,
and Provider dispatch paths. It does not prove configured trust-schema
delivery, cross-node propagation, or the Targeted/large/stream lifecycle.
The RV-I rows below therefore remain pending until those runtime modes and the
MiniNDN network fixture execute them. MiniNDN samples cross-node risks; it does
not replace the deterministic layers.

## Coverage audit of the current slice

> Same supersession rule as above: this is the frozen 2026-09-02 coverage
> audit; final row status is the `## Executed-case mapping (2026-09-04, T010)`
> section and `evidence/release-gate.md`.

This audit is intentionally stricter than a line-coverage report:

| Area | Current evidence | Status |
|---|---|---|
| ControllerVersion ordering, zero rejection, canonical round-trip | `controller-revocation-policy.t.cpp` | covered |
| PolicyStatus structural validation, typed target/scope checks, duplicate/version handling | `controller-revocation-policy.t.cpp`; real Controller status cases in `controller-revocation-flow.t.cpp` | covered for structure, Controller mutation, outer Data signature, configured file trust-anchor acceptance, and wrong-signer rejection; runtime installation under the production schema remains pending |
| Durable generation, restart/clock rollback, corrupt state, single writer/lost lease | unit + component integration suites | covered for local store fencing and Controller construction; live multi-process issuance/lost-lease publication pending |
| Identity, certificate-only, and service-scoped revocation | unit + component integration suites; real Controller mutation case; live User/Provider renewal case | covered for state decisions, identity-bound digest non-over-revocation, Controller filtering, certificate-only empty renewal, Provider identity execution denial/reauthorization, and same-service unaffected issuance control; production-runtime trust-schema and full runtime certificate-only enforcement remain pending |
| Certificate-only digest interoperability | `RealRuntimeEnforcesCertificateAndServiceRevocation` plus canonical certificate advertisement code path | covered for Controller/Provider full-wire digest agreement in LocalMock runtime; production-runtime trust-schema and live renewal remain pending |
| Discovery, ACK, Selection, execution, Response, and stream cut points | `RevocationTargetKindsCoverEveryProtectedTransition`; `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`; runtime cases | covered only for the pre-attribute typed state and real Controller status component path; `/PERMISSION` versus `/SERVICE`, global ABE rekey, other live handlers, and cryptographic bindings remain pending |
| Selection key-envelope replay/conflict boundary | `RequestScopedSelection/SelectedProviderReceivesExactEncryptedInputOnly` | covered for exact Selection replay after terminal completion: the selected Provider does not execute a second time; conflicting pre-terminal variants and other invocation modes remain pending |
| Cache-family invalidation and exactly-one terminal outcome | `RevocationStateInvalidatesOnlyAcceptedServiceFamilies`; `HybridMessageCryptoInvalidatesOnlyAffectedService`; `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `GenericDynamicApi/TargetedInvocation/ControllerVersionChangeEvictsOnlyAffectedUserTargetedPool`; `GenericDynamicApi/TargetedInvocation/ControllerVersionChangeEvictsOnlyAffectedProviderTargetedTokens`; `RequestScopedSelection/RevokedUserCannotReceiveResponseAfterProviderExecution`; `Spec175InvocationStream/StreamEventAfterUserRevocationIsRejectedBeforeDelivery` | the six-family state ledger, service-scoped User/Provider Targeted-pool eviction, pending request-key cleanup with preserved unary timeout, stream Unauthorized terminal reporting, and service-scoped HybridMessageCrypto send/receive/wrapped-key eviction are implemented; the rebuilt CryptoAndAuthorization suite passes 16/16, including the affected/unaffected-service Hybrid case. NAC-ABE internal cache renewal, persistent runtime caches, and restart recovery remain pending |
| Atomic per-service status installation and version isolation | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `ControllerRevocationFlow/RuntimeRestartDropsControllerStatusAndFailsClosed`; service-scoped `isAcceptableControllerVersion()` call sites | partial: both runtime objects stage `RevocationState` and `PolicyRefreshCoordinator` before one commit, and protected message paths compare against the exact service state. The restart test proves stale permission data cannot resurrect authority; a two-service live exact-fetch/source-equivalence assertion remains pending |
| Request/ACK/Selection/Response version field, certificate advertisement, and opaque-container encoding | current focused unit suite; `RequestScopedSelection/ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery`; `RequestScopedSelection/RevokedUserCannotReceiveResponseAfterProviderExecution`; dedicated response-confidentiality component | ControllerVersion, User/Provider encryption-certificate metadata, and protected-message-container round-trips are covered; inline ciphertext tamper, response-time revocation, configured trust validation of a retained response segment, and request-bound AAD tamper fail before application delivery; full production-runtime response modes remain pending |
| Stream ControllerVersion binding and delivery-time revocation | focused `invocation-stream-message` 16/16; `Spec175InvocationStream` 19/19 | Optional stream version wrapper round-trips canonically, legacy versionless streams remain decodable, invalid versions fail closed, the binding digest changes when version changes, an already-buffered post-revocation event is rejected before application delivery, a revoked User is rejected before Targeted stream bootstrap, and a revoked Provider cannot commit the next event after the first event; Provider-side Targeted execution denial/reauthorization is covered separately, while configured trust-schema, restart/rejoin, Targeted token/refill, and MiniNDN propagation remain pending |
| ABE, recipient encryption, certificate validity, nonce/replay, key disclosure | none in this slice | pending; requires real cryptographic/runtime tests |
| Controller policy-status Interest construction, bounded timeout retry, and per-service in-flight deduplication | malformed/missing-service and corrupt-generation refusal plus valid signed PolicyStatus publication, wrong-signer/content-tamper negatives, configured file trust-anchor acceptance, recipient-encrypted permission issuance, and the live User/Provider signed-status relay are covered; production-schema installation and MiniNDN renewal denial are not | partial; explicit configured-validator behavior is covered, while live production-schema User/Provider installation and MiniNDN propagation remain pending |
| Source-independent status retrieval, offline/rejoin, bounded refresh/coalescing, scheduled refresh | `controller-revocation-state.t.cpp` (11/11 current-source focused cases) covers bookkeeping decisions, including authenticated-hint/no-status exact-fetch gating and cache invalidation scope; `controller-version-refresh.t.cpp` covers component scheduling/coalescing, accepted-status cache invalidation, wrong-service rejection, and Controller-produced status; `RuntimeRestartDropsControllerStatusAndFailsClosed` covers loss of process-local authority after restart | partial; exact service-scoped comparison and atomic install are implemented, while two-service live retrieval, persistent cache restoration, offline/rejoin, and MiniNDN propagation remain pending |
| Large response, Targeted token/refill, segmented stream, telemetry redaction | `RequestScopedLargeResponseUsesPerSegmentAeadReference` unit source; `RequestScopedSelection/SelectedProviderReceivesExactEncryptedInputOnly`; `RequestScopedSelection/ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery`; `RequestScopedSelection/RevokedUserCannotReceiveResponseAfterProviderExecution`; dedicated response-confidentiality component; `GenericDynamicApi/TargetedInvocation/RevokedUserCannotConsumeTargetedTokenOrStartRefill`; `Spec175InvocationStream/StreamEventAfterUserRevocationIsRejectedBeforeDelivery`; `Spec175InvocationStream/StreamEventAfterProviderRevocationIsRejectedAtPublication`; `ControllerRevocationFlow/RealTargetedProviderRejectsRevokedExecution` | partial; normal request-scoped large-response segmentation, `key_scope=request`, per-segment nonce/AAD construction, shared IMS ownership, configured trust validation of one retained segment, inline ciphertext rejection, response-time revocation, cross-user requester-name rejection, wrong-provider evidence rejection, stale-version rejection, terminal replay, User token/refill pre-gate denial, LocalMock User delivery-time and Provider commit-time stream revocation, and Provider-side Targeted execution denial/reauthorization execute in focused paths; full segmented fetch/retry, Targeted token/refill response handling, large-response revocation, configured stream trust/restart, and telemetry-redaction network evidence remain pending |

### Controller revocation completeness gate

The following rows make the Controller-specific test obligation explicit. They
are additive to the lifecycle rows below, not substitutes for them.

| Layer | Case family | Minimum cases | Current status |
|---|---|---|---|
| Unit | Mutation transaction | accepted identity/certificate/service targets; duplicate target; empty/contradictory/unknown target; epoch-once; persistence failure rollback | partial; target validation and epoch assertions exist; the lost-writer rollback is now exercised at the real Controller boundary, while an injected temporary-write/rename failure remains pending |
| Unit | Authority publication | first start; repeated same-version publication; exact name; validity start/end; old/equal-conflicting/new replacement; wrong signer/content; corrupt state; clock rollback | partial; most structural and signature negatives exist, restart/replacement execution remains incomplete |
| Unit | Issuance and scope | User and Provider roles; service filtering; replacement certificate; unrelated identity/service; renewal denied after each target kind | partial; local policy decisions and one live identity/service renewal denial exist; certificate-only and unaffected live controls remain pending |
| Unit | Refresh and invalidation | hint-triggered, scheduled, reconnect; duplicate/distinct hints; timeout/invalid status; all cache families; bounded retry and terminal completion | partial; the 11-case coordinator suite now covers hint/no-status gating and bounded retry bookkeeping; runtime cache and reconnect cases remain pending |
| Component | Real Controller-to-runtime path | signed status publication and trust validation; recipient-bound User/Provider permission retrieval; renewal before/after identity, certificate, and service withdrawal | partial; `LiveControllerStatusRefreshRejectsRevokedRenewal` drives Controller-signed permission and PolicyStatus Data through the runtime Face relay, verifies exact version refresh after empty User/Provider renewal, and denies User publication/Provider execution; configured production trust-schema, certificate-only live renewal, unaffected live control, and MiniNDN propagation remain pending |
| Component | Message and lifecycle matrix | every Request/ACK/Selection/Response/Targeted/stream enforcement owner; each distinct normal/large/Targeted/stream cache and terminal path; restart/offline/rejoin/Controller-unavailable classes; target kinds assigned pairwise; affected + unaffected control | pending; the new Controller bridge covers normal Request publication and Provider execution, while remaining distinct live paths are pending |
| Component/MiniNDN | Evidence and limits | source-independent Controller/Provider/cache retrieval; execution/key disclosure before enforcement; exactly-one terminal result; redacted telemetry; historical-key non-retractability | pending; requires cross-process traces and release-gate evidence |

No row is considered covered merely because `ServiceController::isRevoked()`
returns true. A Controller test is complete only when it proves both the
authority mutation/issuance result and the corresponding runtime behavior (or
clearly records that the runtime/network row is still pending).

### Controller authority branch inventory

This table is the minimum audit for the Controller itself. A row marked
`covered` means the local branch has an executed test; it does not imply that
the resulting status or denial has crossed a real User/Provider wire path.

| Controller behavior | Current test/evidence | Status |
|---|---|---|
| Start with a valid durable generation and establish a non-zero version | `GenerationPersistsAcrossRestartAndClockRollback`; real Controller construction | covered |
| Restore revocations before starting the next generation | `GenerationPersistsRevocationsAcrossRestart`; `ServiceControllerRestoresRevocationsAfterRestart`; `ServiceControllerRestoresEveryRevocationKindAfterRestart` | covered |
| Reject missing/corrupt generation state and refuse protected authority | `CorruptGenerationStateFailsClosed`; `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` | covered |
| Reject a second Controller writer for the same identity/state and preserve the first writer | `ServiceControllerRejectsSecondWriterAndPreservesAuthority` | covered |
| Accept identity-wide, certificate-only, and identity-plus-service-plus-attribute targets | Existing `RevocationTargetValidationMatrix`; planned attribute-shape extensions | partial: identity/certificate and pre-attribute service shapes execute; canonical `/PERMISSION` versus `/SERVICE`, missing/mismatched attribute rejection, and dual-role isolation are pending |
| Reject empty, contradictory, unknown, duplicate, or invalid targets without advancing epoch | `PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates`; real Controller mutation case | covered |
| Advance epoch exactly once and roll back the in-memory mutation on persistence failure | generation-store atomic/rollback tests; `ServiceControllerRevocationRollsBackAfterWriterLoss`; real epoch assertions | covered for local state and lost-writer rollback; direct temporary-write/rename injection remains pending |
| Build status with global targets versus attribute-scoped service targets | `ServiceControllerStatusRoundTripPreservesAuthorityScopes` plus planned attribute wire cases | partial: pre-attribute service scoping executes; exact authorization-attribute preservation is pending |
| Filter revoked User/Provider permission snapshots while preserving unrelated services | `ServiceControllerFiltersRevokedPermissionSnapshots` | covered for policy tables |
| Refuse malformed status Interests and unavailable Controller state | `ServiceControllerStatusHandlerRejectsMissingService`; corrupt-state handler case | covered |
| Publish a valid signed status Data and verify it with the configured trust schema | `ServiceControllerStatusHandlerPublishesSignedCurrentStatus`, `ServiceControllerStatusSignatureRejectsWrongSignerAndTampering`, and `ConfiguredTrustSchemaControlsControllerStatusValidation` verify the real handler's outer signature, accept a trusted certificate through a file trust anchor, and reject an untrusted signer/content path; runtime User/Provider installation with the production schema remains pending | covered for the configured-validator component; runtime propagation remains partial |
| Deliver a valid and revoked permission Interest and prove recipient-bound decryption/denial | `ServiceControllerPermissionHandlersEncryptCurrentRevocation` invokes both real permission handlers, checks immutable version-addressable Data names, decrypts with the target identity, rejects a wrong recipient, checks service-scoped filtering, preserves the already-issued historical snapshot, and verifies identity-wide/certificate-only withdrawal produces a current-version snapshot with no grants. It also rejects malformed/unknown permission targets. `LiveControllerStatusRefreshRejectsRevokedRenewal` additionally wires signed permission and PolicyStatus Data through runtime Faces, rejects replay of the older accepted status, and proves empty-renewal status refresh plus User/Provider denial. | partial; configured trust-schema, certificate-only live renewal, unaffected live control, and MiniNDN remain open |
| Notify/refresh User and Provider, invalidate runtime caches, and enforce every cut point | `RealControllerStatusDrivesUserAndProviderRevocation` covers Controller-generated status installation, User publication denial, and Provider execution denial for normal unary traffic; cache/reconnect and other modes remain unrun | partial |
| Refuse protected runtime transitions before any authenticated status is installed | `ConfiguredControllerFailsClosedBeforeStatusInstallation` configures a Controller prefix without installing status, then proves User Request publication and Provider execution are both refused; the full six-cut-point/trust/MiniNDN matrix remains pending | covered for the configured no-status component slice; full lifecycle pending |
| Treat a higher message-carried version as a refresh hint rather than authority | `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch` proves the coordinator keeps v1 until a valid exact status fetch; `ControllerVersionRefresh/HigherVersionHintDoesNotAuthorizeBeforeExactFetch` proves the same component rule. Runtime exact-name retrieval across every message type is not yet wired | partial / blocking for live enforcement |
| Publish immutable version-addressable status names across repeated requests and Controller restarts | `ServiceControllerStatusHandlerPublishesSignedCurrentStatus` repeats the handler, checks that the exact current-version name is stable, and rejects an unavailable exact version; `ServiceControllerStatusVersionIsMonotonicAcrossRestarts` verifies a strictly newer generation, restored revocation, old-name refusal, and current-name publication after restart | covered at component level; simulated clock rollback remains unit-only |

## Unit Tests

Primary suites: `RequestScopedConfidentiality`, `ControllerRevocationPolicy`,
`ControllerRevocationState`, and the focused Hybrid/Targeted cases in
`GenericDynamicApi`.

| ID | Case | Required result | Trace |
|---|---|---|---|
| RV-U01 | Compare equal/newer/older/zero/forged-high ControllerVersion pairs, including a valid jump over unseen epochs | Lexicographic result is deterministic; zero fails; generation timestamp is never compared with local wall time; peer value never becomes authority | FR-018, FR-028–FR-031 |
| RV-U02 | Decode and validate status with valid/invalid signature, wrong service, mismatched name/content version, expired validity, malformed or duplicate revocation fields | Only exact Controller-signed, correctly scoped immutable Data is accepted | FR-015, FR-029, FR-033 |
| RV-U03 | Start Controller three times, inject clock rollback, crash between temporary write and atomic commit, and load missing/corrupt generation state | Generation strictly increases after committed starts; incomplete/corrupt state fails closed and cannot issue status or keys | FR-031–FR-032 |
| RV-U04 | Acquire two writers, expire/replace the first lease, and let the fenced-out writer attempt another publication | At most one writer publishes; stale fencing value is rejected immediately before signing/issuance | FR-032 |
| RV-U05 | Revoke a User identity, Provider identity, individual certificate, User `/PERMISSION/S`, and Provider `/SERVICE/S` independently, including one identity holding both attributes | Controller status, affected-attribute key rotation, and renewal decisions affect exactly the named subject/scope/attribute; the other attribute and all unaffected controls are unchanged | FR-016–FR-017, FR-020, FR-034 |
| RV-U06 | Apply an accepted withdrawal-driven version change to ABE public parameters/DKEY/CK and every request cache kind: permission snapshot, MessageKey, Targeted token/refill, Selection binding, nonce/replay state, and incomplete request | All old NAC-ABE generation entries are removed/tombstoned; request caches are invalidated only when their binding is obsolete; no refill resurrects old tokens. Mixed-generation public parameters, DKEYs, and ciphertext fail closed. Grant-only same-generation retention is covered separately by RV-U21. | FR-017, FR-019, FR-034–FR-036, FR-038 |
| RV-U07 | Inject revocation at each of the six invocation cut points and refresh the node responsible for the next transition | That enforcing node rejects the next affected transition with one terminal outcome; execution/key-disclosure state is recorded; no transition claims rollback or remote key erasure | FR-035 |
| RV-U08 | Replay old DKEYs, permission responses, certificates, Targeted tokens, Selection envelopes, request keys, and Response/stream events after revocation | Every obsolete artifact fails before new application delivery and cannot execute twice | FR-009, FR-020, FR-023, FR-035 |
| RV-U09 | Reauthorize a revoked subject under a newer ControllerVersion | Fresh material authorizes new traffic; retained pre-revocation material remains unauthorized even though an old symmetric key may still decrypt matching historical ciphertext | FR-036 |
| RV-U10 | Observe duplicate and many distinct authenticated higher-version hints, then inject timeout, invalid status, and eventual valid status | At most one fetch is in flight per service; only bounded highest-candidate/retry state remains; waiting deadlines terminate once; unrelated equal-version work is not blocked | FR-021, FR-029, FR-037 |
| RV-U11 | Advance scheduled time toward status expiry without any peer hint, inject Controller unavailable/available outcomes, then repeat with still-valid status | Pre-expiry refresh runs; expired/newer-dependent transitions fail closed after the bound; valid equal-version unaffected transitions continue; no timer creates duplicate fetches | FR-021, FR-037 |
| RV-U12 | Exercise typed errors and audit records for every rejection above; verify the migration default pins request-scoped activation and a stale rollback-switch value is inert | No implicit old-path fallback; one typed reason and redacted record; no key/plaintext leakage. `RevocationStateReportsRedactedTypedReasons` fixes the six transition reason strings and verifies that identity/certificate values are absent. The old service-wide response-key carrier and its `NDNSF_REQUEST_SCOPED_COMPATIBILITY` switch/counters were removed after the migration gates passed (T012); `RequestScopedDefaultActivationWithConfiguredController` pins the post-removal default. | FR-023–FR-026 |
| RV-U13 | Round-trip ControllerVersion through Request, ACK, Selection, Response, Targeted bootstrap/refill, and stream invocation binding, then modify either field after signing/AAD construction | Both fields survive canonical encoding; every modification is detected before authorization or delivery | FR-027–FR-028 |
| RV-U14 | Construct identity, certificate-only, `/PERMISSION` service, `/SERVICE` service, missing-attribute, service-mismatched-attribute, unknown, empty, and contradictory `RevocationTarget` values | Only the canonical target shapes are accepted; every invalid combination fails before epoch advancement or status publication | FR-015–FR-017, FR-034 |
| RV-U15 | Authorize with no status, an incomplete subject, a wrong service, and an expired status | Each missing-authority or invalid-subject branch fails closed with a stable typed reason; no default allow is possible | FR-018, FR-020–FR-021, FR-035, FR-037 |
| RV-U16 | Exercise User/Provider runtime authorization before any authenticated service status is installed | Every protected transition fails closed; a missing local version or missing per-service revocation state cannot provide an implicit legacy/default allow | FR-018, FR-020, FR-035, FR-037 |
| RV-U17 | Submit duplicate, zero, forged-high, and valid higher ControllerVersion hints before and during an exact status fetch | Hints only update bounded refresh bookkeeping; local authority and cache state change only after the exact Controller-signed status is validated, with one in-flight fetch | FR-018, FR-029–FR-030, FR-037 |
| RV-U18 | Call scheduled refresh, completion, failure, retry, current-version, and expiry predicates at their terminal/no-status boundaries | No-fetch completion/failure and missing-authority scheduling fail closed; equal hints are ignored; expiry is detected at the validity boundary; a near-expiry refresh remains single-flight and completes only with the accepted status | FR-018, FR-021, FR-029–FR-030, FR-037 |
| RV-U19 | Stage a withdrawal status/new-ABE-generation pair that the authorization state would accept but the refresh/install transaction must reject, then repeat with a valid pair for one of two services | Rejection leaves authority and every cache unchanged; acceptance commits the service status plus global ABE generation atomically, invalidates only obsolete service-bound non-ABE state, and exposes no partial install | FR-017–FR-019, FR-029, FR-034, FR-037 |
| RV-U20 | Generate old and new global NAC-ABE public parameters/DKEYs around one withdrawal, retain all old material, and try every same- and mixed-generation decrypt combination | Revoked old DKEY cannot decrypt new-generation ciphertext; retained identity's new filtered DKEY succeeds; old ciphertext remains decryptable only as the documented historical limitation; parameter/DKEY/ciphertext generation mismatch fails closed | FR-017, FR-019–FR-020; SC-007–SC-008 |
| RV-U21 | Add `/PERMISSION/S` and separately `/SERVICE/S` without withdrawing any existing grant, including reauthorizing a subject whose old DKEY predates a completed revocation generation | ControllerVersion advances while the exact public-parameter name/digest and master-secret generation remain unchanged; only the target policy changes and one target-only DKEY fetch (normally initiated after signed status installation, with explicit fetch as fallback) yields one complete replacement DKEY containing retained plus newly granted attributes; a DKEY-only refresh fence rejects an overlapping stale fetch and permits at most one coalesced follow-up; the old same-generation target DKEY cannot satisfy the new attribute until replacement install; unaffected DKEYs remain usable and are not refetched; a pre-revocation DKEY remains unusable | FR-017, FR-019, FR-036, FR-038; SC-019, SC-022 |

## C++ Integration Tests (normative target)

The complete layer MUST use real ServiceController, ServiceUser,
ServiceProvider, message encoding, certificate validation, authorization tables,
token pools, and request state machines. A deterministic local network fixture
may control loss and ordering, but it must not bypass signing, encryption,
status validation, or the actual Request/ACK/Selection/Response handlers. The
currently executed file is an explicitly bounded slice: seven cases use the
state/component model, while fourteen cases construct real Controller/User/Provider
objects and exercise authority mutation, corrupt-generation fail-closed behavior, status
wire round-trip, revocation restoration after restart, policy-snapshot
filtering, direct status-handler refusal, recipient-bound permission issuance,
and signature/content-tamper negatives, plus the normal runtime
Request publication/execution boundaries. The separate
`controller-version-refresh.t.cpp` case composes a real Controller status with
single-flight refresh and state enforcement. The LocalMock and component
cases are not a network or trust-schema gate; the remaining rows require
those live modes and MiniNDN before they can be marked passed.

| ID | Case | Required result | Trace |
|---|---|---|---|
| RV-I01 | Withdraw User A's `/PERMISSION/S` while User B uses S and Providers remain authorized | A receives no new attribute material and cannot start/progress old-version requests after refresh; B completes matched controls; a dual-role A retains `/SERVICE/S` | FR-017, FR-020, FR-034–FR-035; SC-008, SC-018 |
| RV-I02 | Withdraw Provider A's `/SERVICE/S` while Provider B offers S | A receives no new attribute material and its later ACK/Selection/execution/Response path is rejected after refresh; B remains selectable and completes; a dual-role A retains `/PERMISSION/S` | FR-017, FR-020, FR-034–FR-035; SC-017–SC-018 |
| RV-I03 | Revoke only an old certificate, install a replacement certificate for the same identity | Old certificate/digest fails; replacement succeeds only with current-version material; identity is not accidentally revoked | FR-016, FR-034, FR-036 |
| RV-I04 | Remove one exact authorization attribute from a dual-role identity with two services | The named `/PERMISSION/S` or `/SERVICE/S` grant fails; the opposite attribute and second service continue without cache-wide denial | FR-017, FR-020, FR-034, FR-037 |
| RV-I05 | Advertise a newer version independently in Request, ACK, Selection, and Response, including service A already at that version while service B remains stale | Each authenticated message type compares against the exact service state, coalesces one exact-name status fetch, revalidates, and resumes or terminates according to deadline; service A cannot suppress service B refresh | FR-018, FR-027–FR-030, FR-037; SC-013 |
| RV-I06 | Return the same immutable status Data from Controller, Provider, and cache; then substitute wrong-signer and wrong-content Data | Valid sources produce identical state; invalid Data never changes authority | FR-015, FR-029, FR-033; SC-015 |
| RV-I07 | Keep a node offline across multiple policy changes, reconnect it, and deliver a valid newer version that skips intermediate epochs | Node refreshes directly to the signed newer version, invalidates obsolete state, rejects old traffic, and resumes authorized current traffic | FR-019–FR-021, FR-029–FR-030 |
| RV-I08 | Revoke at Request/ACK, ACK/Selection, Selection/execution, and execution/Response boundaries, refreshing the next transition's enforcement owner and recording execution/key-disclosure counters | No post-acceptance obsolete transition passes that owner; each request has one terminal result and at most one execution; already disclosed keys/work are reported rather than treated as recalled | FR-035; SC-017 |
| RV-I09 | Revoke during large-Response key/reference handling, Targeted token use/refill, and an active segmented stream | Large-result keys/references, pool/refill, and stream bindings invalidate at their enforcement owner; old events/tokens fail; unrelated transfers/pools/streams continue | FR-019, FR-022, FR-035–FR-037 |
| RV-I10 | Restart User and Provider after revocation with persisted obsolete bindings/tombstones | Restart cannot resurrect old requests, consumed tokens, keys, nonce state, or stream delivery | FR-019, FR-036 |
| RV-I11 | Withhold all newer-version peer hints, exercise scheduled pre-expiry refresh with Controller available/unavailable, then separately advertise a newer version | The component slice proves hintless scheduling and coalescing with a Controller-produced status; the live gate must additionally discover revocation before expiry, handle unavailable Controller, and enforce the configured bound | FR-021, FR-029, FR-037; SC-009, SC-020–SC-021 |
| RV-I12 | Restart Controller repeatedly with clock rollback and attempt a concurrent second writer/lost-lease publication | Versions never collide; a second real Controller cannot revoke or publish status, and only the fenced owner can issue accepted status/keys | FR-031–FR-032; SC-014–SC-015 |
| RV-I13 | Reauthorize a previously revoked subject and attempt both new and archived credentials | Only new-version credentials work; archived material never regains authority | FR-036; SC-019 |
| RV-I14 | Run concurrent affected and unaffected requests while delivering duplicate and many distinct higher-version hints plus refresh failures | One in-flight fetch and bounded highest-candidate state serve affected waiters; unrelated service/identity requests remain live; no duplicate terminal callback or execution | FR-029, FR-034–FR-037; SC-013, SC-018, SC-020 |
| RV-I15 | Construct the real ServiceController, mutate identity-wide, certificate-only, `/PERMISSION/S`, and `/SERVICE/S` revocations, then request status for the affected and an unrelated service | Each mutation advances ControllerVersion exactly once; duplicate, attribute-less, mismatched, and invalid targets are rejected; status contains global/correctly scoped targets only; unaffected attribute/service remains unchanged | FR-015–FR-017, FR-020, FR-031–FR-032 |
| RV-I16 | Construct the real ServiceController with corrupt durable generation state and attempt revocation/status issuance | ControllerVersion remains invalid; revocation and PolicyStatus issuance fail closed without publishing protected authority | FR-031–FR-032 |
| RV-I17 | Construct the real ServiceController, apply identity, certificate, `/PERMISSION/S`, and `/SERVICE/S` targets, encode/decode status, and validate it at the current time | Controller-produced status remains canonical and valid after transport; identity/certificate targets are global, service targets retain their exact attribute, and replacement certificates remain distinguishable | FR-015–FR-017, FR-033–FR-034 |
| RV-I18 | Construct the real ServiceController and build User/Provider permission snapshots, new public parameters, and DKEY renewal material before and after attribute- and identity-scoped revocation | ControllerVersion and ABE generation both advance; `/PERMISSION/S` disappears only from the affected User's filtered new DKEY, `/SERVICE/S` only from the affected Provider's; retained identities receive new-generation DKEYs, old DKEYs cannot decrypt new ciphertext, and the signed on-wire issuance path binds exact parameter name/digest | FR-017, FR-020, FR-034–FR-035 |
| RV-I19 | Invoke the real Controller policy-status Interest handler with a missing service component and with corrupt durable generation state | The handler emits no authority Data for malformed or unavailable state; this refusal is distinct from a valid signed status publication | FR-015, FR-031–FR-033 |
| RV-I20 | Invoke the real Controller policy-status Interest handler for a valid service after preparing its signing identity | The handler publishes one immutable version-addressable Data packet containing the current PolicyStatus; the outer Data signature verifies with the Controller signer, an exact current-version query returns the same name, and an unavailable exact version emits no Data | FR-015, FR-027, FR-031–FR-033 |
| RV-I21 | Invoke real User and Provider permission handlers before and after `/PERMISSION`, `/SERVICE`, identity-wide, and certificate-only revocation using identities present in the Controller keychain | Permission Data is immutable/version-addressable and encrypted to the named target certificate; the target decrypts current status, a wrong recipient fails, each attribute withdrawal removes only its exact grant, and identity/certificate withdrawal returns no grants; retained identities receive current-generation DKEYs | FR-017, FR-020, FR-034–FR-035; SC-008 |
| RV-I22 | Invoke the real status handler, then verify with the Controller certificate, an unrelated certificate, and modified content | Only the Controller signature verifies; wrong-signer and content-tamper packets are rejected before status installation | FR-015, FR-027, FR-033; SC-015 |
| RV-I23 | Install current signed-status snapshots into real LocalMock User/Provider runtimes, publish and execute one normal request, then revoke the User and Provider independently | Current User/Provider traffic succeeds before revocation; User identity withdrawal prevents the next Request publication, while Provider identity withdrawal rejects execution after a current-version Request is published; unaffected-before controls remain successful | FR-018, FR-020, FR-034–FR-035; SC-008, SC-017–SC-018 |
| RV-I24 | Install a real Controller-produced status, trigger hintless pre-expiry refresh, coalesce duplicate higher-version hints, decode the refreshed status, and apply it to revocation state | At most one refresh is in flight; the newer Controller version is accepted after wire round-trip; the revoked subject is denied while a matched unaffected subject remains authorized | FR-018, FR-021, FR-029–FR-030, FR-034, FR-037; SC-013, SC-018, SC-020 |
| RV-I25 | Start real User and Provider runtimes with valid permission tables but no authenticated Controller status, then attempt each protected transition | Request, ACK/Selection, Provider execution, Response delivery, and stream admission fail closed; no missing-version or missing-state legacy bypass is accepted | FR-018, FR-020, FR-035, FR-037; SC-016–SC-018 |
| RV-I26 | Deliver an authenticated higher-version hint on each protected message type, with duplicate/forged/older variants, while the exact status fetch is delayed | The receiver issues one exact-name status fetch, does not adopt the hint as authority, preserves the last accepted status until validation, and applies the newer signed status before the bounded deadline | FR-027–FR-030, FR-037; SC-009, SC-013, SC-020–SC-021 |
| RV-I27 | Request permission/DKEY renewal after identity-wide, certificate-only, `/PERMISSION`, and `/SERVICE` Controller withdrawal, using the real signed Data path and matched unaffected identity/service/attribute controls | The revoked subject receives no usable new grant and cannot start the next protected transition; retained identities receive new-generation filtered DKEYs; replacement certificates and unaffected controls remain authorized; wrong-signer/tampered/expired status or mixed ABE generation is rejected | FR-015–FR-020, FR-033–FR-036; SC-007–SC-009, SC-015, SC-017–SC-019 |
| RV-I28 | Publish policy status repeatedly before and after a Controller restart, including clock rollback and an older cached Data copy | Each Data name is the canonical `(service, generation, epoch)` identity, repeated publication for one version is stable, generation/epoch ordering is strictly monotonic, and old cached status cannot roll authority back. The component case covers real repeated restart/name refusal; the unit store case covers clock rollback. | FR-015, FR-027, FR-031–FR-033; SC-014–SC-015 |
| RV-I29 | Install a newer accepted status for service A while service B remains at an older version, then deliver an authenticated service-B message carrying the newer ControllerVersion | The runtime compares against service B's accepted status, starts exactly one service-B exact-name fetch, does not authorize from the process-wide maximum, invalidates only service B after validation, and preserves service A traffic | FR-018–FR-019, FR-029, FR-034, FR-037; SC-013, SC-018, SC-020; component slice `NewerServiceStatusDoesNotAuthorizeOtherService` covers exact service-B rejection/acceptance and publication counts; exact-fetch/source-equivalence remains a MiniNDN requirement |
| RV-I30 | Through real Controller/AA, revoke `/PERMISSION/S` and separately `/SERVICE/S`, publish status plus exact public parameters, then renew DKEYs for affected and retained identities | Each withdrawal advances ControllerVersion and the global ABE generation atomically; PolicyStatus binds the public-parameter name/digest; affected renewal omits the target attribute; all retained identities receive current filtered DKEYs; old or mixed-generation DKEYs fail on new ciphertext; dual-role opposite-attribute and unrelated-service controls recover after refresh | FR-015, FR-017, FR-019–FR-020, FR-033–FR-035; SC-007–SC-009, SC-018 |
| RV-I31 | Through the real Controller/AA, grant `/PERMISSION/S` and separately `/SERVICE/S` without any withdrawal, then reauthorize one formerly revoked identity under the current post-revocation generation | Each grant advances ControllerVersion but preserves the exact public-parameter name/digest; only the target policy changes and one target-only DKEY fetch (normally initiated after signed status installation, with explicit fetch as fallback) returns one complete replacement DKEY, which is atomically installed before the new attribute is usable; an overlapping old fetch is rejected by the DKEY-only fence and triggers at most one follow-up; no unaffected DKEY is reissued or invalidated; reauthorization succeeds with the new DKEY while retained pre-revocation material still fails | FR-017, FR-019, FR-036, FR-038; SC-019, SC-022 |

The current component slice also includes the following explicit paired
cut-point cases (they are still state/component evidence, not live
User/Provider packet-flow evidence):

| Test case | Coverage added | Status |
|---|---|---|
| `IdentityRevocationDeniesEveryCutPointForBothRoles` | Identity-wide User and Provider revocation at all six transitions, with unaffected User and Provider controls | executed |
| `CertificateRevocationKeepsReplacementAndUnrelatedIdentityLive` | Certificate-only revocation at all six transitions, with replacement-certificate and unrelated-identity controls | executed |

## MiniNDN Network Gate

The network gate is a representative cross-process campaign, not a repeat of
the deterministic branch matrix. It must include at least: User identity
revocation, Provider identity revocation, `/PERMISSION/S` withdrawal,
`/SERVICE/S` withdrawal with an unaffected and dual-role control,
retained-old-DKEY failure against new-generation ciphertext and successful
new-DKEY recovery for all retained identities,
revocation during an in-flight request, offline node skipping epochs,
Controller/cache/Provider status retrieval, Controller-unavailable expiry,
hintless scheduled revocation discovery, Controller restart, and one scenario
that reaches each distinct large-response, Targeted/refill, and stream cache
path. The scenario manifest MUST map every network-only risk to at least one
case and may combine compatible risks in one case. Each scenario records status
source, exact service ControllerVersion, refresh attempts, invalidated cache
counts, execution count, terminal owner, terminal reason, and redacted trace
hashes.

The checked-in launcher and topology are now present at
`tests/minindn/run_request_scoped_confidentiality.py` and
`tests/minindn/spec179-topology.conf`. A dry run is intentionally
non-privileged:

```bash
python3 tests/minindn/run_request_scoped_confidentiality.py \
  --scenario controller-restart
```

It validates the scenario contract only. The explicit `--execute` path first
checks NFD/MiniNDN, role binaries, and deterministic Controller withdrawal,
bounded-lifetime, and identity controls. Those C++ role controls are now
wired and preflight-detectable. The launcher now performs bounded NFD/route
setup, shared-PIB role startup, controlled revocation/restart, and redacted
log/CSV evidence collection. A privileged normal-mode run has now produced
cross-process evidence (`gatePassed=true`) and is recorded in
`evidence/minindn-user-identity-revocation-20260902-priv-rerun.md`. This closes
only the normal User-identity row; the remaining target, mode, recovery, and
source-equivalence scenarios still require separate privileged runs before
CI-06/T011 can be closed.

## Existing Regressions Retained

The following current tests remain mandatory regression inputs but do not close
an RV row by themselves:

- `StalePolicyEpochRejectsProviderRequestBeforeHandlers` checks one stale
  request boundary;
- `NewUserUsesUnchangedProviderAfterControllerMaterialRefresh` checks a
  non-revoking refresh case;
- `ReplacesRoleSnapshotAndRejectsOlderEpoch` checks authorization snapshot
  ordering;
- existing Targeted token/replay suites exercise token ownership and
  at-most-once behavior.

Spec179 adds direct Controller-built status, typed revocation, complete cache,
in-flight ownership, reauthorization, availability, and network propagation
assertions around those existing foundations.

## Executed-case mapping (2026-09-04, T010)

Every RV row below maps to executed cases on the current source
(build `build-clang-spec179-nac3`).  Suite executions on 2026-09-04:
full unit run 686 cases (single pre-existing out-of-scope DI codec
SIGFPE, see `evidence/regression-red-green-20260904.md`); integration
families at `--report_level=detailed`: `ControllerRevocationFlow` 38/38,
`ControllerVersionRefresh` 1/1, `RequestScopedSelection` 3/3,
`RequestScopedResponseConfidentiality` 4/4, `Spec175InvocationStream`
19/19; component/unit families in
`evidence/runtime-revocation-lifecycle-20260904.md` and
`evidence/grant-only-single-issuance-20260904.md`.  Row IDs that are
only provable across processes remain mapped to the MiniNDN campaign
(T011) and are marked `network` — no RV row is closed by T011 evidence
alone; deterministic branches are closed by the named unit/component
cases.

### RV-U rows → executed unit/component cases

| RV-U | Executed cases (2026-09-04, green) |
|---|---|
| RV-U01 | `ControllerVersionOrdersAndRoundTrips`; `ProtectedMessagesRoundTripControllerVersion`; `RepeatedControllerStartsRemainStrictlyMonotonic` |
| RV-U02 | `PolicyStatusWireRejectsMalformedRevocationTargets`; `PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates`; `PolicyStatusNameIsExactAndVersionAddressable`; `ConfiguredTrustSchemaControlsControllerStatusValidation`; `ServiceControllerStatusSignatureRejectsWrongSignerAndTampering`; `ServiceControllerStatusHandlerStripsParametersDigestAndRejectsWrongPrefix` |
| RV-U03 | `GenerationPersistsAcrossRestartAndClockRollback`; `CorruptGenerationStateFailsClosed`; `GenerationStoreRejectsInvalidWriterAndStartInputs`; `ServiceControllerRevocationRollsBackAfterWriterLoss`; `ServiceControllerRevocationRollsBackWhenStateCannotBeRead`; `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` |
| RV-U04 | `OnlyOneWriterCanPublish`; `LostWriterLeaseCannotPublishWithStaleFence`; `ServiceControllerRejectsSecondWriterAndPreservesAuthority` |
| RV-U05 | `RevocationStateDistinguishesCertificateAndServiceScope`; `BoundCertificateRevocationDoesNotOverRevokeAnotherIdentity`; `GlobalCertificateRevocationDoesNotOverRevokeReplacement`; `GlobalCertificateRevocationMatchesDigestAcrossIdentitiesAndServices`; `RevocationStateDistinguishesUseAndProvisionForDualRole`; `ServiceControllerMutatesTypedRevocationAndAdvancesEpoch` (dual-role block `controller-revocation-flow.t.cpp:948-1000`) |
| RV-U06 | `RevocationStateInvalidatesOnlyAcceptedServiceFamilies`; `HybridMessageCryptoInvalidatesOnlyAffectedService`; `ControllerVersionChangeEvictsOnlyAffectedUserTargetedPool`; `ControllerVersionChangeEvictsOnlyAffectedProviderTargetedTokens`; mixed-generation fail-closed matrix in `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` (`t.cpp:855-935`); grant-only retention under RV-U21 |
| RV-U07 | `RevocationTargetKindsCoverEveryProtectedTransition`; `RevocationStateCoversAllProtectedTransitions`; `RevokedUserAndProviderHavePairedUnaffectedControls` (exactly-once terminal); `ServiceRevocationCoversEveryCutPointWithoutCrossServiceDenial` |
| RV-U08 | `RevocationStateSupportsForwardOnlyReauthorization`; `NonceRegistryRejectsReuseAndExpiresEntries`; `ReplayedTargetedRuntimeRequestExecutesOnce`; `SelectedProviderReceivesExactEncryptedInputOnly` (terminal replay); stream `NDNSF_PROVIDER_REPLAY_REJECTED reason=duplicate-request-and-token`; `NormalStreamSuppressesDuplicateEventData` |
| RV-U09 | `RevocationStateSupportsForwardOnlyReauthorization`; `RealRuntimeEnforcesCertificateAndServiceRevocation` (`reauthorized=allow` + retained old material rejected); `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` reauth block (`t.cpp:1002-1010`); RV-U20 historical-decrypt note |
| RV-U10 | `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`; `HigherHintsCoalesceToOneHighestCandidate`; `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; `InvalidStatusRetriesWithBoundedAttempts`; `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic` |
| RV-U11 | `ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight`; `RefreshCoordinatorHandlesTerminalAndExpiryBoundaries`; `PolicyStatusDoesNotBecomeValidBeforeItsValidityWindow`; `RevocationStateRejectsAtStatusValidityEnd`; `RestartClockRollbackAndControllerUnavailableAreBounded` |
| RV-U12 | `RevocationStateReportsRedactedTypedReasons`; `RequestCryptoFailureNamesAreStable`; typed failure codes asserted throughout `RequestScopedConfidentiality` and stream/response families; post-removal default pinned by `RequestScopedDefaultActivationWithConfiguredController` (stale `NDNSF_REQUEST_SCOPED_COMPATIBILITY` inert, unit regression); old-carrier removal recorded in [AUDIT.md](AUDIT.md) R179-M4 and [minindn-campaign-20260904.md](evidence/minindn-campaign-20260904.md) |
| RV-U13 | `RequestSecurityBindingIsCanonicalAndRoundTrips`; `ConfidentialityContainersRoundTripThroughProtectedMessages`; `EncryptionCertificateAdvertisementsRoundTripAndBindToMessages`; `ProtectedMessagesRoundTripControllerVersion`; `StreamRequestOptionsControllerVersionRoundTripsAndIsOptional`; `StreamBindingControllerVersionChangesCanonicalDigest`; `NonceAndAssociatedDataAreDeterministicAndCursorBound` |
| RV-U14 | `PolicyStatusRequiresTypedRevocationTargets`; `RevocationTargetValidationMatrix`; `PolicyStatusWireRejectsMalformedRevocationTargets`; `ServiceControllerRejectsInvalidRevocationTargetsWithoutEpochAdvance` |
| RV-U15 | `RevocationStateFailsClosedForMissingOrInvalidSubject`; `ServiceScopeAndMissingStatusFailClosed`; `OlderAndConflictingEqualVersionStatusFailClosed`; `ConfiguredControllerFailsClosedBeforeStatusInstallation`; `RuntimeRestartDropsControllerStatusAndFailsClosed` (stale snapshot) |
| RV-U16 | `RevocationStateFailsClosedForMissingOrInvalidSubject`; `ConfiguredControllerFailsClosedBeforeStatusInstallation` (`no_status=fail_closed user_publish=0 provider_execute=0`) |
| RV-U17 | `UnauthenticatedHintsNeverChangeRefreshState`; `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`; `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; `NewerServiceStatusDoesNotAuthorizeOtherService`; trace `Ignoring non-status ControllerVersion hint` (message hint is not authority) |
| RV-U18 | `RefreshCoordinatorHandlesTerminalAndExpiryBoundaries`; `ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight`; `InvalidStatusRetriesWithBoundedAttempts` |
| RV-U19 | `InvalidStatusCannotReplaceLastAcceptedAuthority`; `InvalidStatusPreservesAuthorityAndCacheEvidence`; `EqualVersionConflictingStatusCannotReplaceAuthority`; `OlderAndConflictingEqualVersionStatusFailClosed`; atomic two-service install: `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `NewerServiceStatusDoesNotAuthorizeOtherService` |
| RV-U20 | `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` (mixed-generation decrypt matrix `t.cpp:855-935`: retained old DKEY vs new-gen ciphertext fails closed; retained-identity replacement DKEY succeeds; fresh mixed combinations fail closed; historical old-pair limitation documented). Component case re-run individually 2026-09-04 exit 0 |
| RV-U21 | `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut` (1 target-only DKEY fetch, 0 fan-out, idempotent; assertions `t.cpp:3041/3042/3050`); `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` (params name/digest unchanged, dual-role + reauth blocks); `LiveControllerStatusRefreshRejectsRevokedRenewal` (pre-revocation DKEY unusable). Wire-level single-issuance timeline in `evidence/grant-only-single-issuance-20260904.md`. **MiniNDN network half executed 2026-09-04**: `grant-only-advance` scenario (epoch advanced with no revocation, `grantOnlyGateOk=true`, 23 executions, 41 target-only refresh attempts) plus the mid-window denial-until-install through the `targeted-refill-invalidation` and `stream-invalidation` post-discovery denial windows — [evidence/minindn-campaign-20260904.md](evidence/minindn-campaign-20260904.md) |

### RV-I rows → executed integration cases

| RV-I | Executed cases (2026-09-04, green at `--report_level=detailed`) |
|---|---|
| RV-I01 | `RealRuntimeEnforcesCertificateAndServiceRevocation` (`user_service_scope=deny`, `user_reauthorized=allow`); `RealUserAndProviderEnforceControllerRevocationAtRuntime` (`unaffected_before=allow`) |
| RV-I02 | `RealRuntimeEnforcesCertificateAndServiceRevocation` (`provider_service_scope=deny`); `RealControllerStatusDrivesUserAndProviderRevocation` (`provider_revoke=execution_denied`); `ServiceRevocationCoversEveryCutPointWithoutCrossServiceDenial`; `RevokedUserAndProviderHavePairedUnaffectedControls` |
| RV-I03 | `CertificateRevocationKeepsReplacementAndUnrelatedIdentityLive`; `CertificateOnlyRevocationAllowsReplacementAndAllCutPointsFailClosed`; `ServiceControllerGlobalCertificateRevocationUsesDigestScope`; `RealRuntimeEnforcesCertificateAndServiceRevocation` (`*_certificate_only=deny`) |
| RV-I04 | `RevocationStateDistinguishesUseAndProvisionForDualRole`; `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` dual-role blocks (`t.cpp:948-1000`, both directions, same service); two-service dual-role runtime row executed in the T011 MiniNDN campaign (`service-scoped-revocation-with-unaffected-control`: `/SERVICE//HELLO` withdrawal with unaffected provider/B + user control, 11/11, 2026-09-04 — [minindn-campaign-20260904.md](evidence/minindn-campaign-20260904.md)) |
| RV-I05 | `NewerServiceStatusDoesNotAuthorizeOtherService` (`service_a=newer service_b=exact`); `RealControllerStatusDrivesUserAndProviderRevocation` (old message-carried version rejected before second execution); RV-U13 binding cases |
| RV-I06 | `ControllerProviderAndCacheCopiesHaveIdenticalAuthority`; `ConfiguredTrustSchemaControlsControllerStatusValidation`; `ServiceControllerStatusSignatureRejectsWrongSignerAndTampering`; `ServiceControllerStatusRoundTripPreservesAuthorityScopes` |
| RV-I07 | `ServiceControllerStatusVersionIsMonotonicAcrossRestarts`; `ServiceControllerRestoresEveryRevocationKindAfterRestart`; `ServiceControllerRejectsStaleExactStatusAfterRevocation`; direct jump to signed newer version: `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; reconnect/offline propagation executed in the T011 MiniNDN campaign (`offline-rejoin-epoch-skip`, 8/8, and `controller-restart`, 15/15, 2026-09-04 — [minindn-campaign-20260904.md](evidence/minindn-campaign-20260904.md)) |
| RV-I08 | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` (6/6 per target kind, incl. Request/ACK, ACK/Selection, Selection/execution, execution/Response pairs); `RevokedUserCannotReceiveResponseAfterProviderExecution`; `RevokedUserCannotReceiveLargeResponseAfterProviderExecution`; `RevocationStateInvalidatesOnlyAcceptedServiceFamilies` (terminal ownership) |
| RV-I09 | `RevokedUserCannotReceiveLargeResponseAfterProviderExecution` (large); `RealTargetedProviderRejectsRevokedExecution` (Targeted exec); `RevokedUserCannotConsumeTargetedTokenOrStartRefill` (token/refill); `TargetedStreamRevocationStopsBeforeBootstrap`; `StreamEventAfterUserRevocationIsRejectedBeforeDelivery`; `StreamEventAfterProviderRevocationIsRejectedAtPublication` |
| RV-I10 | `RuntimeRestartDropsControllerStatusAndFailsClosed` (`status_lost=fail_closed stale_permission=denied`) |
| RV-I11 | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `LiveControllerStatusRefreshRejectsRevokedRenewal`; `RestartClockRollbackAndControllerUnavailableAreBounded`; `ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight` |
| RV-I12 | `RepeatedControllerStartsRemainStrictlyMonotonic`; `GenerationPersistsAcrossRestartAndClockRollback`; `ServiceControllerRejectsSecondWriterAndPreservesAuthority`; `ServiceControllerRevocationRollsBackAfterWriterLoss`; `ServiceControllerStatusVersionIsMonotonicAcrossRestarts` |
| RV-I13 | `RevocationStateSupportsForwardOnlyReauthorization`; `RealRuntimeEnforcesCertificateAndServiceRevocation` (`*_reauthorized=allow`); `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` reauth block; archived credentials rejected (old-version message rejection in `RealControllerStatusDrivesUserAndProviderRevocation`) |
| RV-I14 | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `HigherHintsCoalesceToOneHighestCandidate`; `InvalidStatusRetriesWithBoundedAttempts`; `ControllerVersionRefresh` 1/1 |
| RV-I15 | `ServiceControllerMutatesTypedRevocationAndAdvancesEpoch`; `ServiceControllerPublishesStableTimestampedRevocationSnapshot`; `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`; `RealControllerStatusDrivesUserAndProviderRevocation` |
| RV-I16 | `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt`; `ServiceControllerRevocationRollsBackWhenStateCannotBeRead`; `CorruptGenerationStateFailsClosed` |
| RV-I17 | `ServiceControllerStatusRoundTripPreservesAuthorityScopes`; `ServiceControllerStatusHandlerPublishesSignedCurrentStatus`; `ServiceControllerStatusHandlerStripsParametersDigestAndRejectsWrongPrefix`; `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` |
| RV-I18 | `ServiceControllerPermissionHandlersEncryptCurrentRevocation`; `ServiceControllerFiltersRevokedPermissionSnapshots`; `ServiceControllerGlobalCertificateRevocationUsesDigestScope`; `RealRuntimeEnforcesCertificateAndServiceRevocation` |
| RV-I19 | `ServiceControllerStatusHandlerRejectsMissingService`; `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` |
| RV-I20 | `ServiceControllerStatusHandlerPublishesSignedCurrentStatus`; `PolicyStatusNameIsExactAndVersionAddressable` |
| RV-I21 | `ServiceControllerPermissionHandlersEncryptCurrentRevocation`; `ServiceControllerFiltersRevokedPermissionSnapshots`; `LiveControllerStatusRefreshRejectsRevokedRenewal`; `RealRuntimeEnforcesCertificateAndServiceRevocation` |
| RV-I22 | `ConfiguredTrustSchemaControlsControllerStatusValidation`; `ServiceControllerStatusSignatureRejectsWrongSignerAndTampering`; `LiveControllerStatusRefreshRejectsRevokedRenewal` (production relay); `LargeResponseUsesConfiguredTrustAndRequestBoundAead` |
| RV-I23 | `RealUserAndProviderEnforceControllerRevocationAtRuntime`; `RealControllerStatusDrivesUserAndProviderRevocation` |
| RV-I24 | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `LiveControllerStatusRefreshRejectsRevokedRenewal`; `ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight`; `HigherHintsCoalesceToOneHighestCandidate` |
| RV-I25 | `ConfiguredControllerFailsClosedBeforeStatusInstallation` (`no_status=fail_closed user_publish=0 provider_execute=0`) |
| RV-I26 | `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`; `UnauthenticatedHintsNeverChangeRefreshState`; `NewerServiceStatusDoesNotAuthorizeOtherService`; `ProtectedMessagesRejectZeroOrDuplicateControllerVersion` |
| RV-I27 | `LiveControllerStatusRefreshRejectsRevokedRenewal` (`permission_fetch=accepted user_revoke=renewal_denied provider_revoke=execution_denied`); `RealRuntimeEnforcesCertificateAndServiceRevocation`; `ServiceControllerPermissionHandlersEncryptCurrentRevocation` |
| RV-I28 | `ServiceControllerRestoresRevocationsAfterRestart`; `ServiceControllerRestoresEveryRevocationKindAfterRestart`; `ServiceControllerStatusVersionIsMonotonicAcrossRestarts`; `ServiceControllerRejectsStaleExactStatusAfterRevocation`; `GenerationPersistsRevocationsAcrossRestart` |
| RV-I29 | `NewerServiceStatusDoesNotAuthorizeOtherService` (`service_a=newer service_b=exact`) |
| RV-I30 | `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` (RV-U20 matrix + filtered issuance); `LiveControllerStatusRefreshRejectsRevokedRenewal`; `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut` (retained DKEYs not refetched) |
| RV-I31 | `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut`; `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` (dual-role + reauthorization under current generation); `RealRuntimeEnforcesCertificateAndServiceRevocation` (`reauthorized_after_new_version=allow` at Targeted provider) |

### Security-critical negative branches (T010 wording) → executed cases

Basis: every unit case cited ran green inside today's full unit run
(686 cases, single pre-existing out-of-scope DI codec SIGFPE — see
[regression-red-green-20260904.md](evidence/regression-red-green-20260904.md));
every integration case ran green today at `--report_level=detailed` in its
named family (`ControllerRevocationFlow` 38/38, `RequestScopedSelection`
3/3, `RequestScopedResponseConfidentiality` 4/4, `Spec175InvocationStream`
19/19, `ControllerVersionRefresh` 1/1).  Branch rows reuse the same cases
where one executed case is the independently meaningful owner of several
negative properties; no equivalent combination is duplicated merely to
inflate the matrix.

| Negative branch | Executed cases (2026-09-04, green) |
|---|---|
| Invalid certificates | `ConfiguredTrustSchemaControlsControllerStatusValidation` (integration, ControllerRevocationFlow); `TargetIdentityCheckRejectsWrongTarget` (unit, encrypted-permission-response); `GlobalCertificateRevocationDoesNotOverRevokeReplacement` (unit, ControllerRevocationPolicy) |
| Wrong recipient | `SelectionEnvelopeWrapsOnlyForTheRecipient` (unit, RequestScopedConfidentiality); `RecipientAssignmentIsFreshPlanBoundAndNotCrossDecryptable` (unit, GenericDynamicApi crypto-auth); `ControllerSignedDataIsEncryptedForTargetOnly` (unit, encrypted-permission-response) |
| Certificate digest mismatch | `GlobalCertificateRevocationMatchesDigestAcrossIdentitiesAndServices` (unit, ControllerRevocationPolicy); `ServiceControllerGlobalCertificateRevocationUsesDigestScope` (integration, ControllerRevocationFlow); `PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates` (unit, ControllerRevocationPolicy) |
| Zero/stale/forged ControllerVersion | `ProtectedMessagesRejectZeroOrDuplicateControllerVersion` (unit, ControllerRevocationPolicy); `ServiceControllerRejectsStaleExactStatusAfterRevocation` (integration, ControllerRevocationFlow); `StreamBindingControllerVersionChangesCanonicalDigest` (unit, Spec175InvocationStreamMessage) |
| Tampered AAD/tag/ciphertext | `AeadEnvelopeRejectsMalformedAndWrongNonce` (unit, RequestScopedConfidentiality); `HybridMessageEnvelopeProtectsAckProviderTokenAndDetectsTamper` (unit, GenericDynamicApi crypto-auth); `ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery` (integration, RequestScopedSelection); `NormalStreamRejectsTamperedEventBeforeDelivery` (integration, Spec175InvocationStream); `ServiceControllerStatusSignatureRejectsWrongSignerAndTampering` (integration, ControllerRevocationFlow) |
| Nonce reuse | `NonceRegistryRejectsReuseAndExpiresEntries` (unit, RequestScopedConfidentiality); `NonceRegistryBatchReservationIsAllOrNothing` (unit, RequestScopedConfidentiality); `HybridKeyEpochRotatesByUsesAndNonceIsUnique` (unit, GenericDynamicApi crypto-auth); `NonceAndAssociatedDataAreDeterministicAndCursorBound` (unit, Spec175InvocationStreamMessage) |
| Duplicate/conflicting Selection | `FirstRespondingAckAfterProviderSelectedIsIgnored` (unit, GenericDynamicApi selection); `SelectedProviderReceivesExactEncryptedInputOnly` (integration, RequestScopedSelection — exact encrypted input and one terminal); `ConcurrentDuplicateCommitsOneRecordAndOneProjection` (unit, opaque-selection lifecycle) |
| Replayed keys/tokens | `ReplayedRuntimeMessagesOnlyTakeEffectOnce` (unit, tokens-replay); `ReplayedTargetedRuntimeRequestExecutesOnce` (unit, GenericDynamicApi targeted); `NormalStreamSuppressesDuplicateEventData` (integration, Spec175InvocationStream — `NDNSF_PROVIDER_REPLAY_REJECTED`) |
| Non-selected response | `UnpermittedProviderAckAndResponseAreRejected` (unit, GenericDynamicApi crypto-auth); `R1LatePositiveAckReceivesNotSelectedWithoutReopeningWindow` (unit, GenericDynamicApi selection); unselected recipients additionally cannot decrypt (`SelectionEnvelopeWrapsOnlyForTheRecipient`, above) |
| Malformed segments | `RequestScopedLargeResponseUsesPerSegmentAeadReference` (unit, GenericDynamicApi prepared — per-segment AEAD, tampered/malformed segment fails auth); `NormalStreamFailsWhenMissingEventWasNeverRetained` (integration, Spec175InvocationStream); `OversizeAndWrongTerminalShapeAreRejected` (unit, Spec175InvocationStreamMessage) |
| Refresh/install failure | `InvalidStatusRetriesWithBoundedAttempts` (unit, ControllerRevocationState); `InvalidStatusCannotReplaceLastAcceptedAuthority` (unit, ControllerRevocationPolicy); `RestartClockRollbackAndControllerUnavailableAreBounded` (integration, ControllerRevocationFlow); `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic` (integration, ControllerVersionRefresh) |
| Generation-state corruption | `CorruptGenerationStateFailsClosed` (unit, ControllerRevocationPolicy); `GenerationStoreRejectsInvalidWriterAndStartInputs` (unit, ControllerRevocationPolicy); `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` (integration, ControllerRevocationFlow) |
| Issuance/renewal denial | `LiveControllerStatusRefreshRejectsRevokedRenewal` (integration, ControllerRevocationFlow — `renewal_denied`); `RevokedUserCannotConsumeTargetedTokenOrStartRefill` (unit, GenericDynamicApi targeted); `RevokedProviderCannotConsumeTargetedTokenOrExecuteHandler` (unit, GenericDynamicApi targeted) |
| Persistence rollback | `GenerationPersistsAcrossRestartAndClockRollback` (unit, ControllerRevocationPolicy); `ServiceControllerRevocationRollsBackAfterWriterLoss` (integration, ControllerRevocationFlow); `ServiceControllerRevocationRollsBackWhenStateCannotBeRead` (integration, ControllerRevocationFlow) |
| Writer fencing | `OnlyOneWriterCanPublish` (unit, ControllerRevocationPolicy); `LostWriterLeaseCannotPublishWithStaleFence` (unit, ControllerRevocationPolicy); `ServiceControllerRejectsSecondWriterAndPreservesAuthority` (integration, ControllerRevocationFlow) |
| Plaintext/key telemetry leakage | `RevocationStateReportsRedactedTypedReasons` (unit, ControllerRevocationPolicy); `RequestCryptoFailureNamesAreStable` (unit, RequestScopedConfidentiality); `RequestKeyBundleHasBoundedLifetimeAndZeroizes` (unit, RequestScopedConfidentiality); revocation runtime telemetry is redacted typed reasons (`NDNSF_REVOCATION_*`, [runtime-revocation-lifecycle-20260904.md](evidence/runtime-revocation-lifecycle-20260904.md)) |

### Network-only rows — executed by the T011 MiniNDN campaign (2026-09-04)

The cross-process halves below were executed by the real MiniNDN campaign
(`tests/minindn/run_request_scoped_confidentiality.py` +
`spec179_scenario_checks.py`, every scenario `gatePassed=true` with
`networkEvidence=true`; per-scenario checks, redacted trace hashes, and
execution counts are in
[evidence/minindn-campaign-20260904.md](evidence/minindn-campaign-20260904.md)):

| Row half | Scenario(s) that close it |
|---|---|
| RV-I04 two-service dual-role runtime | `service-scoped-revocation-with-unaffected-control` (11/11, `/SERVICE//HELLO` withdrawal, unaffected provider/B + user control) |
| RV-I07 reconnect/offline epoch skipping | `offline-rejoin-epoch-skip` (8/8, epoch-3 direct jump, revoked provider stays stopped); `controller-restart` (15/15) |
| RV-I09/CI-04 large/stream/Targeted cache paths at the network layer | `large-response-invalidation` (14/14), `stream-invalidation` (6/6, STARTED→mid-stream code=3→admission START_FAILED, control user/B), `targeted-refill-invalidation` (14/14, bootstrap denial + refill evidence) |
| RV-U21 mid-window denial-until-install + grant-only network half | `grant-only-advance` (`grantOnlyGateOk=true`, unchanged-params advance, 41 target-only refresh attempts) plus the post-discovery denial windows of `targeted-refill-invalidation` and `stream-invalidation` |
| Controller/Provider/cache source equivalence | `controller-cache-provider-status-retrieval` (14/14) |
| Controller-unavailable expiry | `controller-unavailable-expiry` (7/7) |
| Wire-level tamper/replay negatives | `selection-response-tamper-and-replay` (14/14; deterministic malformed branches remain unit/component cases per T010) |

Every deterministic branch in those rows remains closed by the named
in-process cases above.
## Amendment 2026-09-05 rows (FR-039–FR-041)

Re-introduced follow-ups (`spec.md` Amendment; `tasks.md` Phase 7). New rows
below the frozen release rows; the 2026-09-04 rows are unaffected.

| Row | Level | Covers | Evidence / cases | Status |
|---|---|---|---|---|
| RV-U22 | compile | FR-041 — Spec179 NAC-ABE contract surface missing on upstream master ⇒ NDNSF framework build fails | `evidence/nac-abe-unpatched-contract-20260905.md` (master 1cc17d9 worktree, clean CMake prefix, `--nac-abe-prefix` rebuild, RC=1: `Consumer::clearCache`/`refreshDecryptionKey`/`getPublicParams{Name,Digest}`, `CacheProducer::refreshPublicParameters` missing) | executed 2026-09-05 |
| RV-U23 | unit | FR-039 — `RuntimeStatusStore` atomic persist round-trip; magic/truncated/corrupt/permission negatives fail closed; disabled mode stores nothing | `RuntimeStatusStorePersistence` suite in `tests/unit-tests/runtime-status-store.t.cpp` — round-trip of every accepted field (service name, version, install time, ABE params name/digest, PolicyStatus wire, signed Data wire); whole-store atomic replacement leaves no `.tmp.*`; missing store is cold start; truncated header/wrong magic/count-bomb/truncated-body/trailing-garbage each fail closed with empty output; empty service/wire refused without creating the file; blocked parent reports failure; opt-in truthy gate ("1" vs "false") | executed 2026-09-05 |
| RV-I32 | component | FR-039 — runtime restart recovery: restored unexpired status decides authorization while Controller unreachable; online confirmation refresh replaces on higher version, idempotent on equal; expired/superseded/unverifiable persisted status fails closed; opt-in disabled keeps `RuntimeRestartDropsControllerStatusAndFailsClosed` behavior | `PersistedRuntimeStatusSurvivesRuntimeRestart` in `tests/integration-tests/controller-revocation-flow.t.cpp` — `NDNSF_PERSIST_RUNTIME_STATE=1` with `$NDNSF_RUNTIME_STATE_DIR`; phase 1 live fetch installs `/example/hello/user`+`/example/hello/provider` statuses at generation-1 epoch-1 and leaves both `.rts` stores on disk; identity revoked, controller advances to epoch-2; phase 2 reconnect-then-construct: synchronous offline seed restores epoch-1 before any round-trip, bounded online confirmation converges both roles to epoch-2; both store files removed on exit; `ControllerRevocationFlow` suite 40/40 green on build `build-clang-spec179-rv32` (canonical configure) | executed 2026-09-05 |
| RV-I33 | component | FR-040 — hierarchical configured trust anchor (root → intermediate CA → Controller cert) accepts live status installation; chain-external signer and broken-chain intermediate are rejected before installation | `HierarchicalConfiguredTrustAnchorControlsControllerStatusValidation` in `tests/integration-tests/controller-revocation-flow.t.cpp` — file-anchored root `/spec179/hier` with `KeyChain::makeCertificate`-issued CA and Controller certificates resolved over the callback-face `CertificateFetcherFromNetwork` relay: anchored depth-2 chain accepted (1 success), self-signed signer inside the Controller name space rejected, STRANGER-signed counterfeit CA intermediate rejected (2 failures); `ControllerRevocationFlow` suite 39/39 green on build `build-clang-spec179-nac3` (canonical configure) | executed 2026-09-05 |

### Amendment 2026-09-05 audit-closure rows (runtime grant/revoke audit Findings A + R1)

Rows added by the 2026-09-05 runtime grant/revoke audit closure (was
CONDITIONAL PASS on `evidence/runtime-grant-revoke-audit-20260905.md`;
re-verified after the fixes below — `ControllerRevocationFlow` 42/42 and the
MiniNDN fix-closure runs, `results/spec179-minindn-fixclosure-20260905/`).

| Row | Level | Covers | Evidence / cases | Status |
|---|---|---|---|---|
| RV-I34 | component | Finding A / SC-022 — the single target-only DKEY refresh of a grant-only wave MUST survive reverse arrival order: the signed status channel installs v+1 first (no pending entry, no refresh), then the permission renewal lands and its equal-version install hits the pending entry — the DKEY-only refresh is still issued exactly once (grant-only wave not silently consumed). Executed on the User side (granted identity); the Provider shares the same-code-shape helper, whose versionChanged path stays covered by the forward grant-only cases | `GrantOnlyRefreshSurvivesReverseOrderStatusFirstInstall` in `tests/integration-tests/controller-revocation-flow.t.cpp:3254` — status-first leg via `installControllerStatus(v2)` asserts zero fetches (wave unset); then `fetchPermissionsFromController` (grant discovered) asserts exactly one granted-identity DKEY fetch, zero for the unaffected provider; repeated permission fetch stays idempotent at one; mechanism analysis (not a separate executed negative): pre-fix the equal-version install consumed the pending entry without refreshing (`ServiceUser.cpp` old 3899-3902), so this sequence would assert 0 fetches | executed 2026-09-05 |
| RV-U24 | unit | Finding R1 / FR-017 — a failed ABE rotation in `revoke()` leaves the revocation enforced in memory and in the durable version (fail-closed) while recording the rotation as pending; the next `revoke()` entry reconciles the pending rotation before accepting another target; the retried rotation is idempotent (master key rolled at most once per epoch advance) | `ControllerRevokeRotationFailureRecordsPendingAndReconciles` in `tests/integration-tests/controller-revocation-flow.t.cpp:3417` (Controller-only) — `NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE=1` makes `rotateAbeGenerationAndReissuePolicies` throw: first `revoke()` returns false, public-params name/digest unchanged, revocation count still advances to 1 (fail-closed enforcement); after the env is cleared the second `revoke()` succeeds via `reconcilePendingAbeRotation()`, params advance to the target `generation*1000+epoch` version, revocations == 2 | executed 2026-09-05 |

FR-041 acceptance additionally re-runs the existing RV-U20/RV-U21 rows on a
prefix rebuilt from the upstream-merged commit (external gate: upstream
maintainer review).

## Completion Rules

Revocation is not complete unless:

1. every RV-U row and RV-I row has an executed result;
2. every FR-015–FR-021 and FR-027–FR-037 maps to at least one unit and one
   integration row, except network-only behavior which additionally maps to a
   MiniNDN scenario;
3. every defined affected transition fails closed once its enforcement owner
   has authoritative status, and every matched unaffected control remains
   successful;
4. failures identify the exact stage and do not leak secrets;
5. unit tests run before integration tests, and integration tests pass before
   the MiniNDN campaign starts.
