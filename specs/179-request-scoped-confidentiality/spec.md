# Feature Specification: Request-Scoped Confidentiality and Epoch Revocation

**Feature Branch**: `UAV-Experimental`

**Created**: 2026-09-01

**Status**: Implementation and validation in progress

**Input**: User description: Make ABE responsible only for service-level confidential discovery before Provider selection; after selection, use request-scoped recipient-specific keys for input/result confidentiality and signed epoch/status data for credential refresh and revocation.

## Authorization-grant key-scope decision (normative)

For a **grant-only** change—adding one `/PERMISSION/<service>` or
`/SERVICE/<service>` authorization to an identity without withdrawing any
existing authorization—the Controller refreshes only the DKEY belonging to the
target identity. The current controller-private ABE master key and public
parameters MUST NOT be rotated; they are not regenerated, transmitted, or
fanned out to other participants. Because the current KP-ABE representation is one
complete policy/DKEY per identity, the target receives one complete replacement
DKEY containing its retained attributes plus the new grant, rather than a
single-attribute patch. Unaffected identities keep their existing DKEYs and
must not refetch them.

The new grant is usable only after the target validates the signed status and
atomically installs that replacement DKEY. Until then, the old same-generation
DKEY continues to work only for attributes it already held. A grant-only
update therefore advances `ControllerVersion` and a target-only DKEY refresh
fence, but does not perform a global rekey. Any transaction that also withdraws
an identity, certificate, or authorization attribute is not grant-only: it
uses the withdrawal path, rotates the global ABE generation, and reissues
filtered DKEYs to every retained identity.

## User Scenarios & Testing

### User Story 1 - Discover a Service Without Revealing the Input (Priority: P1)

A User discovers an authorized service through the existing NAC-ABE path. The Request exposes only a confidential discovery descriptor and the User's response-encryption certificate reference; after selection, application input is published as a separately named encrypted Data packet and is not readable by an unselected Provider.

**Why this priority**: It establishes the boundary between service discovery authorization and invocation confidentiality without changing Provider selection semantics.

**Independent Test**: Two Providers and two Users observe the same Request and named Input Data; only the selected Provider can fetch, verify, and decrypt the input after Selection, while an unselected Provider and another User cannot.

**Acceptance Scenarios**:

1. **Given** a current non-zero ControllerVersion and an authorized User, **When** the User publishes a Request, **Then** only the discovery descriptor is ABE-protected; after selection the User publishes a separately named input Data packet encrypted under a fresh request key, and no plaintext application input appears in any wire payload.
2. **Given** an unauthorized User or stale ControllerVersion, **When** the User publishes a Request, **Then** discovery fails closed before Provider selection and no request key is accepted.

### User Story 2 - Deliver Keys Only to the Selected Provider (Priority: P1)

After ACK collection and Provider selection, the User generates fresh `K_input` and `K_response` keys. Selection delivers both keys only inside an envelope encrypted to the selected Provider's advertised encryption certificate; the User retains `K_response` for response decryption.

**Why this priority**: It removes the current service-wide `/PERMISSION/<service>` wrapping of large-response keys and makes confidentiality match the actual request recipient.

**Independent Test**: Capture Request, ACK, the named input Data, Selection, and Response packets. A selected Provider decrypts the key envelope and fetches the named input; an unselected Provider, a different User with the same service ABE key, and a Provider with a mismatched certificate digest all fail.

**Acceptance Scenarios**:

1. **Given** a valid ACK containing a Provider encryption certificate name and digest, **When** the User selects that Provider, **Then** the Selection envelope is bound to the exact Provider certificate, User certificate, request ID, attempt, ControllerVersion, and canonical Selection digest.
2. **Given** a Selection replayed for a different request, Provider, attempt, or ControllerVersion, **When** the Provider unwraps it, **Then** it rejects the envelope before decrypting input.
3. **Given** a valid selected Provider, **When** it receives the Selection, **Then** it decrypts `K_input` and `K_response` once, marks the request binding consumed, and cannot reuse the keys for another request.

### User Story 3 - Encrypt Results for the Requesting User (Priority: P1)

The selected Provider encrypts Response Content with the request's `K_response`, signs the Response Data with its signing certificate, and uses a unique nonce for every event or segment. The requesting User can decrypt the result; the selected Provider may hold the key transiently while producing it, but other Users and Providers cannot.

**Why this priority**: It provides recipient-specific result confidentiality and preserves NDN Data integrity and provenance.

**Independent Test**: The requesting User decrypts and authenticates a Response; another User with the same service authorization, the Provider's ABE DKEY holder, and a modified or replayed ciphertext cannot decrypt or validate it.

**Acceptance Scenarios**:

1. **Given** a completed selected invocation, **When** the Provider publishes a Response, **Then** ciphertext AAD matches the registered binding tuple and the User decrypts it with its retained `K_response`.
2. **Given** a changed service name, request ID, attempt, ControllerVersion field, User certificate digest, Provider certificate digest, Selection digest, nonce, or ciphertext, **When** the User verifies the Response, **Then** authentication fails closed.
3. **Given** a segmented or streaming result, **When** multiple segments are published, **Then** one invocation key may be reused but every segment has a unique nonce and segment-bound AAD.

### User Story 4 - Refresh and Revoke Credentials by ControllerVersion (Priority: P2)

The Controller publishes signed policy status/revocation Data. Certificates have bounded validity, ABE keys are tied to a non-zero ControllerVersion and an ABE generation, and User/Provider runtimes refresh changed material and clear stale MessageKey, Targeted-token, and in-flight request caches. A grant-only ControllerVersion may retain the same ABE generation, so unaffected same-generation DKEYs remain usable.

**Why this priority**: It makes revocation behavior explicit and prevents stale authorization material from silently remaining valid after a policy change.

**Independent Test**: Revoke a User identity, a Provider identity, one certificate,
a User's `/PERMISSION/<service>` authorization, and a Provider's
`/SERVICE/<service>` authorization
for one service in separate runs. At every
Request/ACK/Selection/execution/Response cut point, obsolete material is
rejected after authoritative status acceptance, unaffected roles, identities,
and services continue, reauthorization requires newly issued material, and
already disclosed plaintext or completed execution is not claimed to be
recalled.

**Acceptance Scenarios**:

1. **Given** signed status Data advancing either ControllerVersion field, **When** a User or Provider refreshes, **Then** it installs the current signed authority and removes obsolete cached keys/tokens and incomplete bindings; a grant-only status with unchanged ABE public parameters refreshes only the affected target DKEY while retaining unaffected same-generation DKEYs.
2. **Given** status Data with a zero or older ControllerVersion, wrong service scope, missing signature, or invalid Controller signature, **When** it is processed, **Then** the runtime rejects it and keeps the last valid non-zero ControllerVersion; a valid newer version may skip unobserved intermediate epochs.
3. **Given** a revoked identity, **When** it requests new service material, **Then** the Controller refuses renewal; old ciphertext is not re-encrypted and previously disclosed plaintext is not claimed to be erased.

4. **Given** a node receives a signed protected message carrying a newer
`ControllerVersion`, **When** its local version is older, **Then** it performs
one bounded fetch of the immutable Controller-signed status Data named by that
exact version, accepts the version
only after validation, clears obsolete material, and resumes the message only
if its deadline and security binding remain valid.
5. **Given** an identity-wide, certificate-only, or attribute-specific service
authorization revocation, **When** current signed status is installed, **Then**
only the affected subject, authorization attribute, and scope lose renewal and
future invocation rights; unrelated identities, replacement certificates, the
other service attribute held by a dual-role identity, and other services retain
their current rights.
6. **Given** revocation is accepted by the node responsible for the next protected transition while a request is in discovery, ACK collection, Selection, Provider execution, Response delivery, or streaming, **When** that transition is attempted, **Then** obsolete-version traffic is rejected with exactly one terminal outcome and no second execution; work, plaintext, or request keys already disclosed before status acceptance are not claimed to be undone or remotely erased.
7. **Given** a previously revoked identity is explicitly authorized again under a newer ControllerVersion, **When** it refreshes, **Then** only newly issued certificates, ABE material, request keys, and tokens work; its old material remains rejected.
8. **Given** User A loses `/PERMISSION/S` authorization while User B remains
authorized, **When** both refresh, **Then** User A cannot publish or progress a
new invocation of S, User B can, and Provider authorization is unchanged.
9. **Given** Provider A loses `/SERVICE/S` authorization while
Provider B remains authorized, **When** all participants refresh, **Then**
Provider A cannot advertise, accept Selection, execute, or publish a result for
S; Provider B remains selectable and completes the request. The User MUST NOT
automatically retry or reselect after an ambiguous post-Selection failure
unless the application declared the operation safe for retry.
10. **Given** an identity receives an additional `/PERMISSION/S` or
`/SERVICE/S` grant and no existing authorization is withdrawn, **When** the
Controller publishes the newer status, **Then** the ABE master secret and
public-parameter generation remain unchanged, the Controller updates only that
identity's complete policy, and the next DKEY fetch for that identity returns
one replacement DKEY containing its complete current attribute set. Every
unaffected identity continues using its existing DKEY and does not trigger a
refresh.

### Grant-only DKEY scope (normative)

The current KP-ABE implementation stores one policy and one monolithic DKEY
per identity. A grant-only change therefore has two separate effects:

1. The Controller advances `ControllerVersion`, updates the affected
   permission table, and publishes the corresponding signed status/permission
   snapshot. It **does not** rotate the controller-private ABE master secret
   (master key) or public parameters; neither is sent to the User or Provider.
2. The Controller replaces only the target identity's complete policy. NAC-ABE
   generates the replacement DKEY lazily when that identity fetches it. Once
   the target has applied the permission renewal that records the grant, the
   runtime initiates one coalesced target-only fetch when it installs the
   corresponding newer signed status, in whichever order the two arrive; an
   explicit fetch is the fallback when automatic refresh is unavailable. The
   replacement contains all of the identity's retained attributes plus the new
   one. No DKEY is issued, invalidated, or refetched for an unaffected
   identity.

   Grant discovery is App-driven: the runtime never polls the Controller's
   permission records on its own schedule. A PermissionResponse that the
   application applies through the permission API is what records the table
   change and arms the target's DKEY-only refresh fence; the runtime then
   performs the single coalesced target-only fetch autonomously, including
   when the signed status was already installed before the response arrived.
   An explicit application permission refetch is therefore the trigger for
   grant discovery as well as the fallback, and grant usability latency is
   bounded by the application's permission-refresh cadence.

   A grant-only refresh MUST advance a DKEY-only refresh fence even though the
   ABE master/public-parameter generation is unchanged. If a prior DKEY fetch
   is in flight, its result is stale once the fence advances and MUST NOT
   overwrite the active key; at most one coalesced follow-up fetch may install
   the replacement. The previous DKEY remains active until a complete,
   authenticated replacement is decoded and atomically installed.

The target's previous same-generation DKEY may continue to authorize attributes
it already held, but it cannot satisfy ciphertext requiring the newly granted
attribute. The new grant is therefore usable only after the target has
validated the newer status and atomically installed the replacement DKEY. If
that fetch or installation fails, the old authority remains intact for its
previous scope and the new grant is not partially applied. A grant-only change
must not be implemented as a global rekey or as an in-place patch of one DKEY
attribute. A transaction that also withdraws any identity, certificate, or
attribute is classified as a reducing/mixed change and follows the global
generation-rotation rule below. Reauthorization after a prior withdrawal uses
the current post-revocation generation and cannot reactivate the old DKEY.

### Edge Cases

- A Request with a missing, expired, non-RSA, or digest-mismatched User encryption certificate MUST fail before key delivery.
- An ACK with a missing, expired, revoked, or digest-mismatched Provider encryption certificate MUST not be selectable.
- Duplicate Selection packets with the same binding MUST be idempotent only until the first successful consumption; conflicting duplicates MUST be rejected.
- A Selection received after the global request deadline, after a ControllerVersion change, or after token consumption MUST fail closed.
- Nonce reuse, wrong nonce length, malformed AEAD tag, unsupported algorithm, or oversized envelope MUST be rejected.
- A response from a non-selected Provider, a response with a wrong terminal owner, or a response signed by a certificate outside the advertised digest MUST be rejected.
- A process restart MUST not resurrect an obsolete-version request binding or consumed key; durable tombstones or explicit loss-of-state behavior MUST be defined.
- Streaming segments MUST not reuse a nonce, even when they share one invocation `K_response`.
- Revocation MUST protect future issuance and future requests only; the system MUST document that it cannot recall cached Data or erase a previously disclosed plaintext.
- A message-carried Controller version MUST NOT itself change local authority; only valid Controller-signed PolicyStatus Data may do so.
- A protected runtime with no installed, authenticated, non-zero Controller
  status MUST fail closed at every protected transition. The legacy behavior
  that treats a missing local version or missing per-service revocation state
  as implicitly acceptable is not a valid Spec179 completion path.
- A newer version carried by an authenticated message is only a refresh hint;
  it MUST trigger bounded retrieval and validation of the exact
  Controller-signed PolicyStatus Data. Merely adopting the hinted version in
  local memory MUST NOT authorize traffic or replace the last accepted status.
- Newer authenticated message versions are coalesced into at most one in-flight status fetch per service with bounded candidate state; repeated or distinct forged/unverifiable higher values MUST NOT create an unbounded queue or Controller workload.
- An older message version is rejected with the receiver's compact current version so the sender can refresh independently.
- Controller generation time is not a per-message timestamp: it remains stable for one Controller generation and changes only after a valid Controller restart.
- If the Controller loses durable generation state, its clock moves backward below the last persisted generation, or two writers claim the same Controller identity, protected issuance MUST fail closed rather than reuse a version.
- Offline or partitioned nodes cannot be forced to update; after reconnect they MUST refresh Controller state before sending protected traffic.
- If the Controller is temporarily unreachable, nodes with unexpired current status may continue unaffected current-version traffic, but a node observing a newer version or holding expired status MUST fail the affected transition closed after bounded refresh attempts.
- Identity-wide revocation, certificate-only revocation, and service-scoped attribute removal MUST not be conflated; each has a distinct affected set.
- A service-scoped target MUST name the exact authorization attribute:
  `/PERMISSION/<service>` for a User grant or `/SERVICE/<service>` for a
  Provider grant. If one identity has both attributes, withdrawing one MUST NOT
  withdraw the other.
- Revocation during Provider execution may not stop work already started. A refreshed Provider MUST not publish a result to a revoked User, and a refreshed User MUST not accept a result from a revoked Provider; no claim is made when the enforcing peer remains offline/unrefreshed or when a revoked party independently uses a previously disclosed request key outside the conforming runtime.
- A timeout or missing Response after Selection is execution-ambiguous. The
  default revocation behavior terminates that attempt without automatic
  reselection; bounded retry/reselection is opt-in and requires an application
  idempotency or deduplication contract.
- Reauthorization MUST issue new-version material and MUST NOT reactivate old DKEYs, certificates, Targeted tokens, key envelopes, or replay state.
- A grant-only change MUST NOT be implemented as a global rekey. It advances
  ControllerVersion for notification and immutable status/permission naming,
  reuses the current ABE public-parameter generation, replaces only the target
  identity's complete policy, and makes one complete replacement DKEY available
  on one target-only DKEY fetch (normally initiated after signed status
  installation, with explicit fetch as fallback). It MUST NOT fan out DKEY refreshes to
  unaffected identities. Until the replacement is installed, the old
  same-generation DKEY remains limited to the attributes it already held and
  the newly granted attribute is not usable. If the grant restores a previously
  revoked identity, that DKEY MUST use the current post-revocation ABE
  generation; no pre-revocation DKEY becomes valid again.

### Revocation Validation Coverage

The normative case inventory is [validation-matrix.md](validation-matrix.md).
Completion requires all of the following rather than a single happy-path
revocation test:

- deterministic unit coverage of version ordering, status validation,
  generation persistence, writer fencing, policy mutation, revocation scope,
  cache invalidation, lifecycle transitions, reauthorization, and bounded
  refresh coordination;
- component integration coverage for User, Provider, and Controller behavior at
  every Request-to-Response lifecycle cut point, including Targeted and stream
  state. A component-only state-model test is useful as an intermediate gate,
  but does not satisfy this requirement until the live runtime handlers are
  exercised;
- network coverage for status propagation, offline/rejoin, source-independent
  status retrieval, Controller unavailability, and concurrent unaffected
  traffic;
- an explicit trace from every revocation requirement and security-critical
  state transition to at least one deterministic test and one cross-component
  test. A line-coverage percentage alone does not satisfy this gate.

### Test-layer sufficiency rules

The revocation tests are deliberately split into three non-substitutable
layers:

| Layer | Required executable behavior | Does not substitute for |
|---|---|---|
| Unit | Pure `ControllerVersion`, `PolicyStatusData`, generation-store, revocation-scope, cache-invalidation, expiry, and typed fail-closed decisions; malformed, duplicate, zero, stale, forged, and invalid-signature inputs | Live Controller issuance or User/Provider message handling |
| Component integration | Real `ServiceController`, `ServiceUser`, and `ServiceProvider` objects with signed/encrypted permission/status Data and the Request → ACK → Selection → execution → Response state machine; affected and matched unaffected controls | Cross-process loss/reordering, cache-source equivalence, or offline/rejoin behavior |
| MiniNDN | At least two Users, two Providers, a Controller, and an NDN cache/relay; real packet loss/reordering, restart, delayed refresh, large/Targeted/stream paths, and redacted lifecycle traces | Deterministic malformed-wire and exhaustive scope branch checks |

Coverage is risk-based rather than a blind Cartesian product. Unit tests MUST
exhaust every target kind, malformed target shape, version relation, and pure
authorization/cache decision. Component tests MUST execute every distinct
production enforcement owner and cache/terminal path at least once, while the
three revocation target kinds are distributed across those paths with matched
unaffected and replacement controls. A combination is repeated only when it
uses a different implementation path (normal versus large Response,
Targeted bootstrap/fast-path/refill, or segmented stream), changes the terminal
owner, or changes persisted/restart behavior. MiniNDN then validates the
cross-process properties that deterministic tests cannot establish: status
propagation, cache/Provider retrieval equivalence, loss/reordering,
offline/rejoin, and restart. It does not rerun every deterministic combination.

Every case records the ControllerVersion and status source, refresh/fetch
attempt count, invalidated cache families, execution count, terminal owner and
reason, whether key/plaintext disclosure preceded enforcement, and a redacted
trace digest. A passing state-model unit test cannot be promoted to an
integration or MiniNDN result. Conversely, a network happy path cannot replace
the unit negative matrix for malformed targets, duplicate versions, corrupt
generation state, or typed fail-closed branches.

### Controller withdrawal test families

The Controller-specific tests use stable IDs so that a passing predicate is
not mistaken for a passing authority or wire path. The following families are
required before the corresponding revocation claim is complete:

| ID | Layer | Required cases | Representative entry points | Completion meaning |
|---|---|---|---|---|
| CU-01 | Unit | valid generation, epoch-once mutation, duplicate/idempotent target, empty/contradictory/unknown target | `PolicyStatusRequiresTypedRevocationTargets`; `PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates`; `RevocationTargetValidationMatrix` | the local mutation is deterministic and cannot advance the epoch twice |
| CU-02 | Unit | identity-wide, certificate-only, `/PERMISSION/S`, and `/SERVICE/S` target validation; global certificate digest; same identity holding both service attributes; replacement certificate and unrelated service | Existing pre-attribute cases plus planned `ServiceAuthorizationRevocationDistinguishesPermissionAndServiceAttributes` and `DualRoleIdentityRetainsOppositeAuthorizationAttribute` | the affected identity/service/attribute set is exact and over-revocation is rejected |
| CU-03 | Unit | status wire canonicality, exact `(generation, epoch)` name, validity half-open interval, zero/stale/equal-conflicting/new versions, invalid signer/digest | `PolicyStatusWirePreservesValidityTimestamps`; `RevocationStateRejectsAtStatusValidityEnd`; `PolicyStatusWireRejectsNonCanonicalFieldOrder`; `EqualVersionConflictingStatusCannotReplaceAuthority`; `InvalidStatusCannotReplaceLastAcceptedAuthority`; `InvalidStatusPreservesAuthorityAndCacheEvidence` | only one authenticated current status can replace the last authority |
| CU-04 | Unit | durable persistence, restart restoration, clock rollback, corrupt state, competing writer, lost lease, rollback after failed commit | `GenerationPersistsAcrossRestartAndClockRollback`; `RepeatedControllerStartsRemainStrictlyMonotonic`; `OnlyOneWriterCanPublish`; `LostWriterLeaseCannotPublishWithStaleFence`; `CorruptGenerationStateFailsClosed` | Controller cannot reuse or publish an unfenced generation |
| CU-05 | Unit | permission filtering, issuance/renewal decisions, cache-family invalidation, nonce/replay/in-flight cleanup, redacted typed reasons | `RevocationStateCoversAllProtectedTransitions`; `RevocationStateReportsRedactedTypedReasons`; `RevocationStateFailsClosedForMissingOrInvalidSubject` | revoked material is removed without invalidating an unrelated service or identity |
| CU-06 | Unit | authenticated message hint, scheduled pre-expiry refresh, reconnect trigger, duplicate/distinct higher hints, bounded timeout/invalid-status retry, and no-status startup | `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`; `HigherHintsCoalesceToOneHighestCandidate`; `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; `ScheduledRefreshStartsOnlyNearExpiryAndIsSingleFlight`; `InvalidStatusRetriesWithBoundedAttempts` | hints never become authority; refresh remains bounded and fail-closed; no-status runtimes remain unauthorized until exact status validation |
| CI-01 | Component | real `ServiceController` mutation, status publication, signature/trust check, exact-name retrieval, stale/forged exact-name refusal, malformed/unavailable status refusal | `ServiceControllerMutatesTypedRevocationAndAdvancesEpoch`; `ServiceControllerPublishesStableTimestampedRevocationSnapshot`; `ServiceControllerRejectsStaleExactStatusAfterRevocation`; `ServiceControllerRejectsInvalidRevocationTargetsWithoutEpochAdvance`; `ServiceControllerStatusHandlerPublishesSignedCurrentStatus`; `ServiceControllerStatusHandlerRejectsMissingService` | Controller authority and publication behavior are exercised through production objects, including a signed, timestamped exact-name status Data route |
| CI-02 | Component | recipient-bound User/Provider permission snapshots before/after every target kind, wrong-recipient rejection, unaffected/replacement controls | `ServiceControllerPermissionHandlersEncryptCurrentRevocation`; `LiveControllerStatusRefreshRejectsRevokedRenewal`; `RealRuntimeEnforcesCertificateAndServiceRevocation` | renewal cannot issue usable grants to an affected subject; Provider identity withdrawal is also denied at execution and restored only by a newer status |
| CI-03 | Component | real User/Provider normal Request → ACK → Selection → execution → Response before and after withdrawal, plus stale message-version rejection | `RealUserAndProviderEnforceControllerRevocationAtRuntime`; `RealRuntimeEnforcesCertificateAndServiceRevocation` | the next protected transition is denied, an old message-carried ControllerVersion is rejected before a second execution, and execution remains at-most-once |
| CI-04 | Component | every production discovery/ACK/Selection/execution/response/stream enforcement owner plus identity/certificate/service targets assigned across those owners, with paired unaffected controls | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`; `IdentityRevocationDeniesEveryCutPointForBothRoles` | each distinct enforcing owner and target-scope decision is executed without requiring redundant all-pairs combinations |
| CI-05 | Component | exact-version hint refresh, no-installed-status startup, signed empty renewal, Controller restart, Controller-unavailable expiry, forward-only reauthorization | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`; `ConfiguredControllerFailsClosedBeforeStatusInstallation`; `ServiceControllerStatusVersionIsMonotonicAcrossRestarts` | stale or revoked material cannot resume; valid newer material can |
| CI-06 | MiniNDN | representative two-User/two-Provider scenarios covering Controller/cache/Provider status sources, loss/reordering, offline/rejoin, restart, and one case for each mode-specific cache path | `tests/minindn/run_request_scoped_confidentiality.py` (real launcher; privileged execution/evidence pending) | cross-process propagation and mode-specific caches enforce the same policy without duplicating deterministic branch tests |
| CI-07 | Component/MiniNDN | execution and key-disclosure ordering, terminal owner/reason, refresh/fetch/invalidation counters, redacted trace | release-gate trace collector (planned) | evidence distinguishes work already disclosed from future denial and leaks no key/plaintext |

Each CI family must include an affected case and a same-version unaffected
control; certificate-only coverage additionally requires a replacement
certificate, and service-scoped coverage requires an unrelated service. The
validation matrix MUST map every security-critical branch to at least one
executed case and explain why any apparently omitted combination is equivalent
to an already executed code path. A source test or state-model loop is recorded
as `implemented` only; it becomes `covered` only after the named executable
runs. CI-04 does not substitute for CI-06: the component layer proves
production object wiring, while MiniNDN is the gate for cross-process loss,
cache-source equivalence, restart, and offline/rejoin. The release claim is
blocked until every required family and distinct implementation path has an
executed result or an explicit, reviewed limitation in the evidence report.

### Controller revocation test contract

Controller withdrawal is tested as an authority operation, not only as a
receiver-side predicate. The executable contract is:

| Layer | Test entry points | Required assertion |
|---|---|---|
| Unit | `ControllerVersionOrdersAndRoundTrips`, `PolicyStatusRequiresTypedRevocationTargets`, `PolicyStatusRejectsInvalidIntervalsDigestsAndDuplicates`, `PolicyStatusWireRejectsMalformedRevocationTargets`, `ProtectedMessagesRejectZeroOrDuplicateControllerVersion` | Version ordering is deterministic; status fields, target kinds, scope, validity, digest, duplicates, malformed wire types, unknown target kinds, and zero/duplicate message versions fail closed. |
| Unit | `PolicyStatusWirePreservesValidityTimestamps` | Controller status `validFrom`/`validUntil` values survive encode/decode exactly; the half-open validity interval remains valid only at `validFrom <= now < validUntil`. |
| Unit | `RevocationTargetValidationMatrix`, `RevocationStateFailsClosedForMissingOrInvalidSubject` | Every target-kind field combination is checked (including unknown kinds); missing authenticated status, incomplete identity/certificate, wrong service, and expired status fail closed with stable reasons. |
| Unit | `RevocationStateCoversAllProtectedTransitions`, `RevocationStateDistinguishesCertificateAndServiceScope`, `RevocationStateSupportsForwardOnlyReauthorization` | Identity-wide, certificate-only, and service-scoped withdrawal produce the correct affected set at all six transitions; newer status is required for reauthorization. |
| Unit | `RevocationStateReportsRedactedTypedReasons` | Every revoked transition returns a stable typed reason without embedding the revoked identity or certificate digest in diagnostics. |
| Unit | `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch` | An authenticated ControllerVersion hint may start one bounded exact-name status fetch, but an invalid/expired result cannot install authority or authorize a transition; only a later valid exact status installs the version. |
| Unit | `PolicyStatusWireRejectsNonCanonicalFieldOrder`, `IdentityRevocationAppliesToEveryRoleForOneIdentity`, `InvalidStatusCannotReplaceLastAcceptedAuthority`, `InvalidStatusPreservesAuthorityAndCacheEvidence` | Non-canonical status encodings, same-identity multi-role withdrawal, and invalid newer status input fail closed without replacing the last accepted authority or falsely invalidating cache evidence. |
| Unit/component | `ConfiguredTrustSchemaControlsControllerStatusValidation` | A trusted Controller status Data is accepted through an explicit file trust anchor while the same status name/content signed by an untrusted certificate is rejected; this closes the validator shortcut at the configured trust-schema boundary, but does not by itself prove live User/Provider schema installation. |
| Unit | `GenerationPersistsAcrossRestartAndClockRollback`, `GenerationStoreRejectsInvalidWriterAndStartInputs`, `RepeatedControllerStartsRemainStrictlyMonotonic`, `OnlyOneWriterCanPublish`, `LostWriterLeaseCannotPublishWithStaleFence`, `CorruptGenerationStateFailsClosed`, `PolicyStatusNameIsExactAndVersionAddressable`, `RefreshCoordinatorHandlesTerminalAndExpiryBoundaries` | Repeated Controller starts cannot reuse a generation, invalid writer/start inputs fail closed, a competing or fenced-out writer cannot publish, corrupt state cannot issue authority, a `(service, generation, epoch)` pair maps to one exact status name, and refresh terminal/expiry boundaries fail closed deterministically. |
| Component | `ServiceControllerMutatesTypedRevocationAndAdvancesEpoch`, `ServiceControllerRejectsInvalidRevocationTargetsWithoutEpochAdvance`, `IdentityRevocationDeniesEveryCutPointForBothRoles`, `CertificateRevocationKeepsReplacementAndUnrelatedIdentityLive` | A real `ServiceController` advances the epoch once per accepted target, rejects every malformed target without mutating authority, and the component slice checks identity-wide and certificate-only withdrawal across all six cut points with matched replacement/unaffected controls. |
| Component | `ServiceControllerPublishesStableTimestampedRevocationSnapshot` | A real Controller emits a newer, currently valid status after revocation; repeated reads at that version are byte-identical and retain the target, validity window, and version together. Its exact-name policy-status Interest route publishes one Controller-signed Data packet whose name, version, target, and validity decode consistently. |
| Component | `RevokedUserAndProviderHavePairedUnaffectedControls`, `ServiceRevocationCoversEveryCutPointWithoutCrossServiceDenial`, `CertificateOnlyRevocationAllowsReplacementAndAllCutPointsFailClosed` | The component state boundary keeps affected and unaffected identities/services paired and verifies service-scoped and certificate-only withdrawal without cross-service or replacement over-revocation. |
| Component | `ServiceControllerStatusRoundTripPreservesAuthorityScopes` | A real Controller status containing identity, certificate, and service targets survives wire encode/decode and validation; global targets remain visible for every service, while service targets remain scoped. |
| Component | `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` | A real Controller with unusable durable state has no valid version and refuses revocation/status issuance. |
| Component | `ServiceControllerRejectsSecondWriterAndPreservesAuthority` | A second real Controller instance using the same identity and durable state remains unready, cannot revoke or publish status, and does not fence the first writer, which can still advance the epoch. |
| Component | `ServiceControllerRestoresRevocationsAfterRestart` | A real Controller restores persisted revocation targets before publishing its new generation; the generation timestamp advances, the epoch restarts at one, and the restored target remains visible in current status. |
| Component | Extend `ServiceControllerRestoresEveryRevocationKindAfterRestart` | A real Controller persists identity-wide, global certificate-only, `/PERMISSION/S`, and `/SERVICE/S` targets together; after restart each exact attribute remains visible, global certificate scope remains cross-identity/service, and neither service attribute is widened to the other. The existing pre-attribute case does not close this row. |
| Component | `ServiceControllerFiltersRevokedPermissionSnapshots` | A real Controller removes a revoked service from User and Provider permission snapshots, preserves an unrelated service, and removes all remaining grants after identity-wide withdrawal. |
| Component | `ServiceControllerStatusHandlerRejectsMissingService`, `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` | The real policy-status Interest handler refuses a missing service component and refuses to publish status when durable Controller generation state is unavailable; neither case emits authority Data. |
| Component | `ServiceControllerPermissionHandlersEncryptCurrentRevocation` | Real User/Provider permission handlers publish immutable version-addressable, recipient-bound encrypted snapshots. The test rejects malformed/unknown targets, decrypts valid snapshots with the target identity, proves a wrong recipient cannot decrypt, preserves the already-issued pre-revocation snapshot as historical evidence, verifies service-scoped withdrawal removes only the affected grant while preserving an unrelated service, and verifies identity-wide and certificate-only withdrawal return current-version snapshots with no grants. |
| Component | `ServiceControllerStatusHandlerStripsParametersDigestAndRejectsWrongPrefix` | The real status handler strips a trailing ParametersSha256Digest before resolving the service and refuses an Interest outside the Controller prefix without emitting authority Data. |
| Component | `RealUserAndProviderEnforceControllerRevocationAtRuntime` | Real LocalMock User/Provider runtimes install current status, successfully publish and execute one normal request, reject a User identity after withdrawal before the next Request publication, and reject a Provider identity at execution after a current-version Request is published. This is runtime boundary evidence, not a live NFD/trust-schema or MiniNDN propagation gate. |
| Component | `RealRuntimeEnforcesCertificateAndServiceRevocation` | The real LocalMock User and Provider runtimes reject certificate-only, service-scoped, and identity-wide Provider targets at the owning boundary (User publication or Provider execution), reject a previously captured request carrying an older ControllerVersion before a second execution, and accept a newer status without the target to reauthorize the same certificate/identity; unaffected publication remains live and execution remains at-most-once. |
| Component | `RealTargetedProviderRejectsRevokedExecution` | A real `ServiceProvider` receives a Targeted request with a current status, executes once, rejects the same protected transition after identity withdrawal without invoking the handler again, and executes again only after a newer status explicitly reauthorizes the Provider. Token checking is disabled in this case so the Controller execution boundary is isolated; token-pool/refill behavior remains a separate gate. |
| Component | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` | A real Controller-generated status is accepted by a component RevocationState for identity, certificate, and service targets at Discovery, ACK collection, Selection, Provider execution, Response delivery, and Stream event; replacement, unrelated, and cross-service controls remain allowed. |
| Component | `LiveControllerStatusRefreshRejectsRevokedRenewal` | A real Controller publishes signed, version-addressable permission and PolicyStatus Data through the runtime Face relay. User and Provider install the current status after empty revoked renewals; User publication and Provider execution are then denied, while the test records the exact refreshed ControllerVersion. |
| Unit/component regression | `invocation-stream-message.t.cpp`, `generic-dynamic-api-targeted.t.cpp`, and `Spec175InvocationStream` | Stream bindings carry an optional ControllerVersion exactly once; canonical round-trip, legacy versionless decoding, invalid-version rejection, digest sensitivity, final-delivery rejection of an already-buffered event after User revocation, Provider-side rejection before a revoked event is committed, and User-side refusal before consuming a cached Targeted token or starting refill are verified. `TargetedStreamRevocationStopsBeforeBootstrap` additionally rejects a revoked User before a Targeted stream can consume credentials or publish its bootstrap. This is LocalMock component/unit evidence; configured trust-schema, restart/rejoin, and MiniNDN propagation remain separate required cases. |
| MiniNDN contract | `tests/minindn/test_request_scoped_confidentiality.py`; `tests/minindn/run_request_scoped_confidentiality.py` | The non-privileged gate declares the two-User/two-Provider topology, all three revocation target kinds, six cut points, four invocation modes, recovery conditions, and required redacted evidence. `--execute` preflights the tracked roles and deterministic scheduled Controller withdrawal/bounded lifetimes, then refuses to fabricate network evidence when the host cannot run privileged MiniNDN. |

### Audit-added controller refresh and fail-closed tests

The existing authority and component cases are necessary but do not close two
runtime branches that are easy to miss: startup before the first authenticated
status, and a message that carries a newer version before the corresponding
status Data has been fetched. The first two cases below are now implemented;
the remaining rows are explicit follow-up gates, not claims that the behavior
already exists:

| Layer | Test entry point | Required assertion |
|---|---|---|
| Component | `ConfiguredControllerFailsClosedBeforeStatusInstallation` | A Controller-configured User refuses Request publication and a Provider refuses execution before any authenticated, non-zero service status is installed; a non-zero permission/manifest hint is not adopted as authority. This is the current no-status runtime regression; it does not cover live status delivery. |
| Unit | `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch` | A higher authenticated hint starts bounded refresh bookkeeping only; local authority remains unchanged until a valid exact status is completed, while an invalid fetch leaves the prior authority in place. |
| Component | `PermissionRenewalDeniedAfterSignedRevocation` (partially covered by `LiveControllerStatusRefreshRejectsRevokedRenewal`; configured trust-schema and certificate-specific live cases remain planned) | A real Controller-issued, immutable version-addressable and signed permission/status Data is delivered through the User and Provider validation/install path. The executed live case covers identity-wide User withdrawal and service-scoped Provider withdrawal: empty renewal advances the exact ControllerVersion, then the affected runtime cannot start the next protected transition. Configured production trust-schema validation, certificate-only live renewal, and matched unaffected live controls remain open. |
| Component | `ServiceControllerStatusVersionIsMonotonicAcrossRestarts`, together with `ServiceControllerStatusHandlerPublishesSignedCurrentStatus` | Repeated status publications return the same exact name for one version, while repeated Controller starts produce strictly increasing generation versions, restore revocations, reject an older exact status name, and publish only the current exact status. The generation-store unit test separately covers simulated clock rollback. |
| Component | `RequestScopedSelection/SelectedProviderReceivesExactEncryptedInputOnly` | A duplicate SelectionKeyEnvelope is delivered after the terminal response; the selected Provider must not execute the handler or publish a second response. The same response publication boundary also rejects a valid ciphertext targeted at another User, authenticated evidence naming another Provider, and a stale ControllerVersion. This is component evidence; configured trust-schema and standalone response-signature/AAD tests remain separate gates. |
| Component | `RequestScopedSelection/ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery` | A short response is carried in the request-scoped AEAD envelope; flipping one ciphertext byte must fail authentication before the User callback or nonce reservation, after which the untouched response remains deliverable. This is an inline-response tamper regression, not a revocation or trust-schema gate. |
| Component | `RequestScopedSelection/RevokedUserCannotReceiveResponseAfterProviderExecution` | A newer revoking ControllerVersion is installed after Provider execution but before User delivery; the User rejects the valid response, times out exactly once, and does not invoke the application callback. This records execution-before-enforcement without claiming rollback or key erasure. |
| Component | `ControllerVersionHintRefreshesBeforeEnforcement` (planned; current component evidence is `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`) | Request/ACK/Selection/Response each carries the compact version hint; the receiving runtime fetches the exact Controller status once, installs it only after signature/trust validation, invalidates obsolete caches, and then either rejects the affected transition or resumes a valid unaffected one. |

### Controller-authority completeness matrix

The Controller part of revocation is a separate acceptance surface from
receiver-side `isRevoked()` checks. The following case families are
mandatory and must be implemented in the named layer before the corresponding
claim is marked complete:

| Layer | Required case family | Branches that must be exercised | Acceptance evidence |
|---|---|---|---|
| Unit | Authority construction and mutation transaction | first valid generation; every accepted target kind; duplicate/idempotent target; empty, contradictory, unknown, and malformed target; epoch increment exactly once; persistence failure and rollback | no partial in-memory mutation, no version reuse, and no status/key issuance after a failed commit |
| Unit | Status publication and replacement | canonical `(generation, epoch)` name; stable repeated publication; validity start/end; wrong service; old/equal-conflicting/new status; wrong signer/tamper; corrupt or missing durable generation; clock rollback | only one valid Controller-signed status can replace the last authority; unavailable or ambiguous state emits no authority Data |
| Unit | Renewal and scope decision table | User identity, Provider identity, certificate-only, and service-scoped withdrawal; same identity holding both roles; replacement certificate; unrelated identity/service; current, stale, zero, and expired status | issuance/renewal decisions match the target exactly; unaffected controls remain allowed; revoked subjects receive no new usable material |
| Unit | Refresh and cache invalidation | message hint, scheduled pre-expiry, reconnect/manual refresh; duplicate and distinct higher hints; timeout/invalid status; every cache family and in-flight request state | one bounded fetch, no hint-as-authority, deterministic invalidation/tombstones, and no resurrection of old keys/tokens/replay state |
| Component | Controller-to-runtime issuance path | real Controller status publication, signature/trust validation, recipient-bound User/Provider permission retrieval, renewal before and after each target kind | revoked renewal returns no usable grant and cannot start the next protected transition; a matched unaffected identity/service succeeds |
| Component | Version propagation and enforcement | Request, ACK, Selection, Response, Targeted bootstrap/refill, and stream binding; newer/older/zero/forged hints; exact-name fetch from Controller, Provider, and cache | receiver preserves old authority until exact signed status validation, then rejects or resumes once within the configured bound |
| Component | Lifecycle/recovery controls | all six enforcement owners, every mode-specific cache/terminal path, and restart/offline/reconnect/Controller-unavailable classes; target kinds are assigned pairwise across these paths, with repeats only for distinct code or persistence behavior | each affected transition has one terminal owner/reason and at-most-once execution; each paired unaffected control remains live |
| Component | Observability and non-retractability | execution/key-disclosure-before-revocation, old ciphertext/key behavior, telemetry for every rejection and refresh outcome | evidence records what already happened without claiming remote key/plaintext erasure and contains no plaintext or key material |

The runtime matrix is complete by independent behavior, not by multiplying
every dimension. A single happy path or state-model loop is not evidence for a
different enforcement owner, cache family, terminal owner, or persistence
path; conversely, identical branches do not need to be rerun for every target
kind and transport mode. Unit tests establish exhaustive authority decisions;
component tests cross the real Controller, User/Provider handlers and signed
Data; MiniNDN remains required for cross-process propagation, cache-source
equivalence, loss/reordering, and offline/rejoin. Every case records the
affected request, a same-version unaffected control, exact service-scoped
ControllerVersion/status source, refresh and invalidation counters, terminal
owner/reason, execution count, and a redacted trace digest.

These tests deliberately separate Controller authority from User/Provider
enforcement. The configured no-status component case does exercise the normal
Request publication and Provider execution boundary, but it does not claim
that a live permission Interest or signed status Data has already been
delivered and installed. Exact status retrieval and Request/ACK/Selection/
Response enforcement require the remaining component tests and the MiniNDN
gate in `validation-matrix.md`. In particular, a state-model pass is
not evidence that permission renewal is denied on the wire, that an offline
node refreshes after reconnect, or that large/Targeted/stream caches are
invalidated.

The Controller-specific additions are intentionally two-dimensional: the unit
matrix proves that malformed targets and every fail-closed authorization
predicate are deterministic, while the component matrix proves that a real
Controller mutates its durable version, restores revocations, emits a
correctly scoped status snapshot, and refuses malformed or unavailable status
requests. Together they cover the authority's local logic, a
Controller-signed status Data publication verified with an explicit test
signer through the exact version-addressable Interest route, and a normal
LocalMock User/Provider publication/execution boundary,
but they still do not cover configured trust-schema verification,
signed permission delivery, or issuance denial over a signed permission
Interest. Those final claims require a live User/Provider permission exchange
and a MiniNDN test with an unaffected control; they must not be inferred from
`isRevoked()` or status round-trip tests.

### Current Controller-revocation checkpoint

The current implementation has an executable local/component slice, but it is
not the completion gate. A current-source focused Controller unit executable
passes all 31 policy cases, including the validity-timestamp, validity-end,
bound-certificate, and zero/duplicate
message-version regressions, and
covers version
ordering, typed target validation, validity and fail-closed decisions,
generation persistence, writer fencing (including a lost lease), cache-invalidation decisions, all six
modeled transition cut points, forward-only reauthorization, canonical wire
ordering, same-identity multi-role withdrawal, and preservation of the last
accepted authority after invalid status input, and global certificate-digest
matching across identities and service snapshots. The refresh-coordinator unit
suite passes 11/11 cases, including authenticated-hint/no-status startup,
bounded exact-fetch retry, and authority installation only after valid status
completion. The checked-in component source
contains 34 cases, including real-Controller lost-writer and durable-read
rollback tests, a
real-Controller-to-User/Provider runtime bridge, and a real Controller
 target-kind × six-cut-point matrix, a timestamped immutable status snapshot,
 stale/forged exact-status refusal after revocation,
and a live Controller-to-runtime renewal bridge. The current Clang/Boost-1.71
executable passes all 34
`ControllerRevocationFlow` cases. The component suite constructs real
`ServiceController`, `ServiceUser`, and `ServiceProvider`
objects and covers accepted and rejected mutations, epoch advancement, global
versus service-scoped status, restart restoration, revoked User/Provider
permission-snapshot filtering, malformed/corrupt status-handler refusal,
same-identity Controller writer conflict refusal,
valid recipient-encrypted permission publication, wrong-recipient rejection,
certificate-only empty renewal for the affected Provider with a same-service
unaffected Provider control, Provider identity withdrawal followed by
newer-version reauthorization, global certificate-only withdrawal by digest
across identities and services,
valid status publication with outer-signature wrong-signer/tamper negatives,
configured file trust-anchor acceptance and untrusted-signer rejection,
ParametersSha256Digest normalization, normal runtime User/Provider
publication/execution denial after identity withdrawal, certificate-only and
service-scoped runtime denial with newer-version reauthorization, and the
configured no-status publication/execution refusal. The live runtime bridge
also verifies signed permission and PolicyStatus delivery, exact version
refresh after empty revoked renewals, User publication denial, and Provider
execution denial. It also checks service-scoped Provider withdrawal followed
by User withdrawal,
with request-publication and Provider-execution counters. Each
affected case has an explicit unaffected control where the local model supports
one.

This checkpoint deliberately leaves the following acceptance cases open:
production-runtime trust-schema installation, full runtime certificate-only
and unaffected renewal controls, atomic per-service status installation,
cross-service version/refresh isolation, persistent runtime-cache recovery,
the remaining distinct mode/cache/terminal paths in component tests, and the
representative cross-process MiniNDN scenarios. These are required before any
claim that Controller withdrawal is complete. The protected-message-container
round-trip now executes in the focused current-tree crypto suite; it proves
only message-container serialization, not authenticated status installation or
runtime revocation propagation.
The audit additionally requires explicit no-installed-status rejection,
hint-only refresh (without local authority adoption), stable exact status naming
and generation monotonicity across repeated status publication and Controller
restart, and signed
permission-renewal denial. No-status rejection, hint non-adoption, and one
live signed-status/empty-renewal denial path, and the component restart/status
monotonicity path are now executed; configured trust-schema, full
certificate-only/unaffected runtime renewal, per-service version isolation,
atomic status installation, and the remaining behavior-path matrix remain open
under RV-U16–RV-U18 and RV-I25–RV-I28.

### Current coverage decision

The added unit and component cases cover the Controller's deterministic
authority logic: target-shape validation, identity/certificate/service scope,
duplicate and invalid mutations, epoch advancement and rollback, durable
generation/timestamp ordering, exact status naming, validity boundaries,
writer fencing, status replacement, cache-state decisions, authenticated
message-hint handling without authority adoption, bounded exact-status retry,
and fail-closed checks for the exercised normal Request path. They also cover the Controller
permission-handler boundary for certificate-only withdrawal and a matched
unaffected Provider issuance control. They do **not** cover every Spec179 case
yet. Production-runtime trust-schema installation, full runtime certificate-only and
unaffected renewals, cross-process propagation/offline rejoin, persistent
  runtime-cache recovery, large-response revocation enforcement, Targeted, and
  configured stream trust/restart/Targeted propagation remain open. The
  service-scoped User/Provider Targeted pools and in-flight unary/stream
  cleanup now have direct behavior assertions: an accepted newer status removes
  only the affected service's cached token material, preserves an unrelated
  service fast path, and leaves the public request/stream terminal callback
  observable. Service-scoped HybridMessageCrypto send/receive/wrapped-key
  eviction is implemented and its focused current-build unit case passes. Atomic
  combined status installation, exact per-service version comparison, and
  NAC-ABE internal cache renewal remain blocking gaps. The normal request-scoped
   large-response fetch/decrypt path is covered as a component regression, but
   the active-stream LocalMock final-delivery revocation case now proves the
   buffered-event race boundary; the six-cut-point matrix otherwise proves the
   typed state decisions and Controller status component behavior; it must not be
reported as full packet-level coverage until those runtime modes execute.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST keep NAC-ABE responsible for service-level confidential discovery before Provider selection and MUST NOT use the service permission DKEY to wrap request/result keys after selection.
- **FR-002**: Every Request MUST carry a canonical discovery descriptor, service name, request ID, attempt, current non-zero ControllerVersion, and the requesting User encryption-certificate name plus digest.
- **FR-003**: Application input MUST be published as an exact named Data packet encrypted with a fresh per-request `K_input` after key creation; plaintext input MUST NOT appear in Request, ACK, Selection, or Response payloads.
- **FR-004**: A Provider ACK MUST advertise the Provider encryption-certificate name, digest, validity state, and supported key-envelope algorithm.
- **FR-005**: The User MUST generate fresh `K_input` and `K_response` after selecting a Provider and MUST retain `K_response` locally for that invocation.
- **FR-006**: Selection MUST deliver `K_input` and `K_response` only in an envelope encrypted to the selected Provider's advertised encryption certificate.
- **FR-006a**: Selection MUST carry the exact name of the post-selection encrypted Input Data packet; the selected Provider MUST fetch and verify that named Data before decrypting the input.
- **FR-006b**: The selected Provider MUST verify that the Input Data signer matches the User certificate digest and that the Input Data name, authenticated binding, request ID, attempt, service, and ControllerVersion match the consumed Selection envelope.
- **FR-007**: Selection key envelopes MUST bind service name, request ID, attempt, non-zero ControllerVersion, User certificate digest, Provider certificate digest, and a canonical Selection digest.
- **FR-008**: The Provider MUST reject a key envelope whose identity, certificate digest, ControllerVersion, request binding, algorithm, or authenticated tag does not match the selected transaction.
- **FR-009**: The Provider MUST consume a valid key envelope at most once for one request/attempt and MUST record replay/conflict diagnostics.
- **FR-010**: Response Content MUST be encrypted with `K_response`, signed as NDN Data by the Provider, and decryptable by the requesting User; the selected Provider may access the key transiently while producing that response, but no other User or Provider may decrypt it.
- **FR-011**: Response AEAD AAD MUST include service name, request ID, both ControllerVersion fields, attempt, User certificate digest, Provider certificate digest, Selection digest, and segment/event identity.
- **FR-012**: Every Response event or segment MUST use a unique nonce under its invocation key; nonce reuse MUST be detected or rejected.
- **FR-013**: The system MUST preserve exact named-Data references, signer validation, Provider selection, one-time tokens, replay protection, and terminal-owner checks from the existing NDNSF flow.
- **FR-014**: A User other than the requesting User, even with the same service ABE authorization, MUST NOT decrypt the Response.
- **FR-015**: The Controller MUST publish immutable, signed status/revocation Data containing `controllerGenerationTimestamp`, non-zero `controllerEpoch`, validity interval, policy digest, and typed `RevocationTarget` entries that unambiguously represent identity-wide, certificate-only, or identity-plus-service-plus-attribute authorization removal. A service authorization target MUST carry exactly one canonical `authorizationAttribute`: `/PERMISSION/<service>` or `/SERVICE/<service>`.
- **FR-016**: Certificates used for signing/encryption MUST be checked against their validity periods and advertised digests; expired or revoked certificates MUST fail closed for new transactions.
- **FR-017**: Signed status MUST identify the exact ABE public-parameter generation by immutable Data name and digest, independently of ControllerVersion. Renewal MUST require current signed status. With the current NAC-ABE implementation, every authorization-reducing change MUST create a fresh ABE master-secret/public-parameter generation, publish the new public parameters, and reissue filtered DKEYs to all still-authorized identities; the withdrawn identity or attribute MUST be absent. Producers MUST encrypt all subsequent ABE-protected Data using the matching new public parameters. Old public parameters/DKEYs MUST remain generation-separated and MUST NOT authorize or decrypt new-generation traffic. Attribute-local rekey is a future optimization and MUST NOT be claimed unless a separate implementation and retained-old-DKEY test prove it. If the cryptographic rotation cannot complete, the Controller MUST still enforce the revocation through every subsequently published status (fail-closed), MUST record the rotation as pending, and MUST retry it before accepting any further revocation.
- **FR-018**: The runtime MUST require exact equality of both fields in the current non-zero `ControllerVersion` for the message's exact service on V2 protected paths; a process-wide maximum from another service MUST NOT authorize traffic or suppress that service's exact-status refresh, and zero MUST NOT act as a wildcard for either field.
- **FR-019**: On an accepted ControllerVersion change, User and Provider runtimes MUST atomically install the new authority and refresh-coordinator state. If the signed public-parameter name/digest changed, they MUST stage the matching new parameters and DKEY before replacing the global NAC-ABE generation and clearing obsolete MessageKey, Targeted-token, selection-binding, nonce, and incompatible incomplete-request caches. If the signed public-parameter identity is unchanged, unaffected DKEYs and ABE caches MUST remain usable; a grant-only target refresh MUST use a DKEY-only fence so an overlapping stale fetch cannot overwrite the active key and at most one coalesced follow-up is issued. A rejected or partially installed status MUST leave the prior authority and caches unchanged. Non-ABE request state and application availability MUST remain scoped so unrelated valid services can continue or resume.
- **FR-020**: Revocation MUST stop issuance and renewal for the affected identity, certificate, or exact `(identity, service, authorizationAttribute)` grant. Withdrawal of `/PERMISSION/<service>` MUST prevent the User from starting or progressing new invocations; withdrawal of `/SERVICE/<service>` MUST prevent the Provider from advertising, accepting Selection, executing, or publishing results under a revoked or stale ControllerVersion, without claiming historical ciphertext recall.
- **FR-021**: Status refresh MUST include a scheduled refresh before the accepted signed status expires, even when no peer advertises a newer version, plus the message-triggered hot-refresh path and bounded retry/backoff. Refresh MUST NOT block unrelated requests indefinitely; affected new transitions MUST fail closed after the signed validity/grace window expires if refresh has not succeeded.
- **FR-022**: The wire contract MUST support one invocation `K_response` for segmented/streaming results while requiring unique per-segment nonces and segment-bound AAD.
- **FR-023**: Unsupported algorithms, malformed envelopes, invalid tags, certificate mismatch, stale ControllerVersion, duplicate/conflicting Selection, and wrong recipient MUST fail closed with typed diagnostics.
- **FR-024**: The implementation MUST expose security telemetry for discovery authorization, key-envelope acceptance/rejection, ControllerVersion refresh, cache invalidation, nonce checks, and response decryption failures without logging key material or plaintext.
- **FR-025**: Existing service-level permission responses MUST remain controller-signed and recipient-encrypted, but MUST NOT be reused as request/result CEK transport.
- **FR-026**: The migration MUST support an explicit compatibility mode only for controlled rollback, with a visible counter and an owner-defined removal condition; the new request-scoped path MUST be the default for V2 protected invocations.
- **FR-027**: Request, ACK, Selection, Response, Targeted bootstrap/refill, and other authorization-bearing protected messages MUST carry the compact `ControllerVersion = (controllerGenerationTimestamp, controllerEpoch)`; streaming/segmented traffic MAY carry it once in the authenticated invocation binding and bind each event to that version digest.
- **FR-028**: Both ControllerVersion fields MUST be covered by the message signature or AEAD AAD. Receivers MUST compare the pair lexicographically only as a freshness signal and MUST NOT treat a peer-supplied value as authority.
- **FR-029**: After authenticating the sender, a message version newer than the accepted status for that exact service MUST enter a bounded refresh coordinator with at most one in-flight status fetch per service. Duplicate hints MUST coalesce, distinct higher hints MUST retain only bounded state for the highest candidate, and the node MUST fetch immutable Controller status Data named by the selected exact service and ControllerVersion. A newer status already installed for another service MUST NOT suppress this fetch. Processing may resume only after Controller signature, service scope, validity, generation timestamp, epoch, and atomic local installation validate; otherwise the message fails closed under bounded retry/backoff.
- **FR-030**: A message version older than local state MUST be rejected with a typed signed error or normal protocol response carrying the receiver's compact current ControllerVersion. The sender MUST fetch current Controller-signed status before retrying.
- **FR-031**: On each Controller start, `controllerGenerationTimestamp` MUST be atomically persisted as `max(currentUnixTimeMs, previousPersistedTimestamp + 1)` before protected status/key issuance; `controllerEpoch` starts non-zero and increments for every authorization-relevant change within that generation. The generation timestamp is an ordering token, not message time or proof of freshness, and receivers MUST NOT compare it with their local wall clock.
- **FR-032**: One Controller identity MUST have only one active status/key writer. The writer lease and generation state MUST use the same authoritative durable store with atomic acquisition and a fencing value, and the Controller MUST revalidate ownership immediately before each protected status/key publication. Failure to acquire or retain the lease, loss/corruption of durable generation state, or inability to establish a generation newer than the persisted value MUST stop protected issuance and produce an operator-visible error.
- **FR-033**: Nodes MAY retrieve Controller-signed PolicyStatus Data for an advertised ControllerVersion from the Controller, any Provider, or an NDN cache; acceptance MUST depend only on exact service scope and ControllerVersion, Controller signature/trust schema, Data validity, and content validation—not on which node returned the Data.
- **FR-034**: Revocation evaluation MUST distinguish identity-wide removal, individual certificate removal, User `/PERMISSION/<service>` authorization, and Provider `/SERVICE/<service>` authorization. `SERVICE_AUTHORIZATION` MUST match the exact `(targetIdentity, serviceName, authorizationAttribute)` tuple; applying one target MUST preserve the other attribute held by the same identity, unrelated identities, replacement certificates, services, and current-version requests.
- **FR-035**: After the node responsible for a protected transition accepts a revoking ControllerVersion, that node MUST reject the affected obsolete transition and converge to exactly one terminal outcome without duplicate execution. The Controller enforces attribute-specific issuance/renewal; the User runtime enforces its own `/PERMISSION/<service>` grant before Request and Selection publication and enforces the selected Provider's `/SERVICE/<service>` grant before ACK selection and Response/stream delivery; the Provider runtime enforces its own `/SERVICE/<service>` grant before ACK, Selection acceptance, execution, and Response/stream publication and enforces the requesting User's `/PERMISSION/<service>` grant before input access and execution. The system MUST record whether execution or key disclosure had already occurred rather than claiming rollback or remote key erasure. Post-Selection retry/reselection MUST remain disabled by default unless the application declares an idempotent or deduplicated operation.
- **FR-036**: Reauthorization after revocation MUST advance ControllerVersion and issue fresh authorization material; no old certificate, ABE key, request key, Targeted token, Selection envelope, nonce state, or replay record may authorize new conforming protocol traffic again. Previously disclosed symmetric keys may still decrypt their matching historical ciphertext and are not claimed to be cryptographically erased.
- **FR-037**: While the exact service's Controller status remains valid, failure to contact the Controller MUST NOT stop unrelated equal-version traffic. A newer observed ControllerVersion or expired local status MUST pause only affected transitions, perform bounded per-service refresh, and fail closed on refresh exhaustion without creating an unbounded retry or fetch storm.
- **FR-038**: A grant-only authorization change MUST advance ControllerVersion without rotating the ABE master secret or public parameters. Because the current KP-ABE implementation stores one monolithic policy/DKEY per identity, the Controller MUST replace only the target identity's complete policy; a target-only DKEY fetch (armed once an applied permission renewal records the grant and the target validates the newer signed status; explicit App-layer refetch is the trigger and fallback, as the runtime never polls permission records) MUST return one complete replacement DKEY containing all retained and newly granted attributes. The grant MUST become usable only after that DKEY is atomically installed. The target MUST advance a DKEY-only refresh fence, reject any overlapping stale fetch, and allow at most one coalesced follow-up while retaining the old key until replacement succeeds. Existing DKEYs for unaffected identities MUST remain usable under the unchanged public-parameter name/digest, with zero global DKEY fan-out or refresh. A target's old same-generation DKEY remains limited to its previous attributes while the replacement is pending. A grant that restores a previously revoked subject MUST use the current post-revocation generation and MUST NOT reactivate pre-revocation material. Any transaction containing a withdrawal follows FR-017's global-generation path instead.

### Key Entities

- **DiscoveryDescriptor**: ABE-protected service-level metadata used before selection; contains service, ControllerVersion, request binding, and authorization context but no plaintext application input.
- **RequestKeyBundle**: Fresh `K_input` and `K_response` for one User/request/attempt; never shared through a service-wide DKEY.
- **ProviderKeyAdvertisement**: ACK-bound Provider encryption-certificate name, digest, validity, algorithm, and revocation status.
- **SelectionKeyEnvelope**: Recipient-encrypted bundle carrying request keys and binding metadata for exactly one selected Provider.
- **ResponseCiphertext**: Signed NDN Data Content encrypted with `K_response`, nonce, segment/event identity, AAD digest, and ciphertext digest.
- **PolicyStatusData**: Controller-signed status/revocation record with non-zero ControllerVersion, validity, affected identities/policies, and the exact name plus digest of the active ABE public-parameter generation; several grant-only ControllerVersions may reference the same generation.
- **RevocationTarget**: Typed Controller decision whose kind is `IDENTITY`, `CERTIFICATE`, or `SERVICE_AUTHORIZATION`; a service target carries the exact identity, service, and canonical `/PERMISSION` or `/SERVICE` authorization attribute, while the other kinds carry only their required scope.
- **ControllerVersion**: Compact ordered pair of a persisted Controller generation timestamp and a non-zero epoch within that generation; echoed by protected messages but authoritative only when obtained from Controller-signed status Data.
- **AuthorizationCacheEntry**: User/Provider cached material records its issuance ControllerVersion, service, active ABE public-parameter name/digest, expiry, revocation state, and explicit invalidation reason. An unaffected DKEY may survive a grant-only ControllerVersion change when the signed status retains the same ABE generation.
- **ConfidentialityAuditEvent**: Redacted telemetry record for key-envelope, epoch, nonce, recipient, and response verification outcomes.

## Success Criteria

### Measurable Outcomes

- **SC-001**: In a controlled two-Provider/two-User test, 100% of unselected Providers and non-requesting Users fail to decrypt the named Input Data and Response Content.
- **SC-002**: 100% of valid selected-provider invocations fetch/decrypt the named input and decrypt the result using request-scoped keys, with no `/PERMISSION/<service>` response-key wrapping.
- **SC-003**: 100% of tampered AAD fields, ciphertexts, tags, nonces, certificate digests, Selection digests, and ControllerVersion fields are rejected before application delivery.
- **SC-004**: 100% of duplicate/conflicting Selection and consumed-key replays are rejected or idempotently handled according to the registered state machine, with no second application execution.
- **SC-005**: 100% of segmented/streaming response tests use unique nonces and successfully decrypt valid segments while rejecting a reused nonce.
- **SC-006**: 100% of certificate validity, Provider advertisement digest, and User certificate digest mismatches fail closed in unit and MiniNDN tests.
- **SC-007**: After a signed ControllerVersion change, 100% of obsolete-version new requests, cached token uses, stale key-envelope attempts, mixed-generation public-parameter/DKEY combinations, and retained-old-DKEY decryptions of new ciphertext fail; refreshed current-version requests succeed for non-revoked identities.
- **SC-008**: Revoked identities receive no new ABE material or certificate renewal after status propagation; all retained identities receive a filtered new-generation DKEY, and the evidence explicitly records both global rekey cost and the non-retractability of previously disclosed plaintext.
- **SC-009**: Online status refresh completes within a bounded configured deadline and does not invalidate unrelated current-version requests; refresh failures produce typed diagnostics and bounded retry counts.
- **SC-010**: No retained security telemetry contains private keys, plaintext application input, plaintext result, or decryptable key material.
- **SC-011**: Existing NDNSF authorization, selection, replay, and terminal-owner regressions pass with the new default path; controlled compatibility mode is never selected implicitly.
- **SC-012**: A complete trace maps every protected request and response to source revision, certificate, ControllerVersion, algorithm identifiers, digest values, and redacted failure evidence.
- **SC-013**: In Request, ACK, Selection, and Response tests, duplicate or many distinct higher ControllerVersion hints produce at most one in-flight fetch per service and bounded candidate/retry state; a newer version for service A cannot suppress an exact fetch for stale service B; valid Controller Data permits processing to resume, while forged/unverifiable values never change authority or create unbounded fetches.
- **SC-014**: Across at least three consecutive Controller restarts—including a simulated clock rollback—the persisted generation timestamp strictly increases, epoch reuse does not collide with an earlier generation, and old-generation protected messages are rejected.
- **SC-015**: A second concurrent Controller writer cannot publish protected status or keys for the same Controller identity, and nodes accept valid Controller-signed status identically whether returned by the Controller, another Provider, or an NDN cache.
- **SC-016**: Every normative unit and integration row in `validation-matrix.md` passes, and every security-critical revocation branch/state transition is linked to an executed test result; no fail-open branch remains justified only by aggregate line coverage.
- **SC-017**: Across all six lifecycle cut points—discovery, ACK collection, Selection, Provider execution, Response delivery, and active stream—once the responsible enforcement node accepts revocation, the affected next transition produces exactly one terminal result, zero later delivery through that conforming runtime, and no request executes more than once; evidence separately records transitions that occurred before enforcement.
- **SC-018**: In identity-wide, certificate-only, User-`/PERMISSION`, and Provider-`/SERVICE` revocation tests, 100% of affected new transitions fail while 100% of matched unaffected identity/service/attribute controls continue under valid current status. The service-attribute tests MUST include one dual-role identity and prove that withdrawing one attribute does not withdraw the other.
- **SC-019**: After explicit reauthorization, 100% of newly issued current-version material authorizes new traffic and 100% of retained pre-revocation keys, certificates, envelopes, and tokens remain rejected as authorization for new conforming traffic; historical ciphertext decryptability is reported separately.
- **SC-020**: During Controller unavailability, valid equal-version control traffic continues, while newer-version or expired-status transitions terminate within the configured refresh bound with one typed outcome and one coalesced fetch sequence per service/version.
- **SC-021**: Without receiving any newer-version peer message, every online conforming User and Provider performs scheduled refresh before status expiry and stops affected new transitions no later than the configured signed validity/grace bound after Controller revocation becomes available.
- **SC-022**: For a grant-only change, only the granted identity's policy changes and one target-only DKEY fetch (armed by an applied App-layer permission renewal — the runtime never polls permission records; explicit refetch is the trigger and fallback) returns one complete replacement DKEY containing its retained and newly granted attributes; the ABE public-parameter name/digest remains unchanged, all unaffected identities complete current traffic without DKEY reissuance, and a DKEY-only fence rejects an overlapping stale fetch with at most one coalesced follow-up. The old target DKEY cannot satisfy the new attribute before replacement installation. A reauthorized identity succeeds only with a DKEY from the current post-revocation generation. The single target-only refresh MUST occur whether the signed status install preceded or followed the permission renewal that armed it — no arrival order may silently consume the pending refresh without issuing it.

## Assumptions

- Existing NAC-ABE discovery, controller-signed permission Data, NDN certificate validation, Provider selection, one-time tokens, and replay stores remain the foundation.
- The initial implementation accepts global NAC-ABE rekey cost on every
  authorization-reducing change. Grant-only changes reuse the current master
  secret/public parameters, replace only the target identity's complete policy,
  and lazily generate one replacement DKEY when that identity fetches it;
  a DKEY-only refresh fence protects overlapping asynchronous fetches.
  Per-attribute or per-service revocation rekey requires a separate NAC-ABE
  extension and is not assumed by this feature.
- RSA encryption certificates remain the initial envelope recipient mechanism because the current NAC-ABE runtime already requires RSA encryption certificates; ECDSA remains the signing certificate type.
- AES-GCM is the initial AEAD subject, with a fixed nonce length and key length recorded in the contract; algorithm agility is explicit rather than implicit.
- A User can retain `K_response` for the lifetime of one invocation, including its bounded stream segments; key material is zeroized and discarded at terminal completion, timeout, cancellation, or ControllerVersion change.
- Existing stored old Data and ciphertext produced under an already disclosed request key may remain decryptable to that key holder; revocation claims are limited to future issuance and transitions enforced after authoritative status acceptance.
- The first implementation targets normal Request/ACK/Selection/Response and then extends the same binding to large and streaming results; Targeted fast paths must not bypass the epoch or recipient checks.
- MiniNDN is the default network/security validation environment; a local mock may test cryptographic state transitions but cannot be the sole acceptance gate.
- The first implementation assumes all processes using one Controller identity share one authoritative durable generation/lease store; active-active Controller replicas with independent stores require a separate consensus design.

## Out of Scope

- Replacing NAC-ABE, inventing a new trust schema, or making ABE itself a request-level result encryption mechanism.
- Revoking or erasing plaintext already disclosed before a ControllerVersion change.
- Remotely erasing a previously delivered request key or constraining a revoked party that bypasses the conforming runtime while its enforcing peer remains unrefreshed.
- GPU, performance optimization, or changing Provider selection strategy.
- Allowing multiple Users to decrypt one invocation result by sharing a service-wide response key.
- Silent migration, automatic fallback to the old `/PERMISSION/<service>` response-key path, or logging plaintext/key material.
- Active-active Controller high availability across independent authority stores.
- Revocation of same-process trusted `LocalServiceRegistry` calls, which do not use the NDNSF network authorization path.
- Emergency rotation of the Controller trust anchor/signing identity; this requires a separate trust-schema migration protocol.
- ~~Deferred runtime hardening (2026-09-04 release scope)~~ — **superseded by the 2026-09-05 Amendment (FR-039–FR-041) below**: persistent runtime-cache restoration across process restart, and production live User/Provider status installation under a configured file trust anchor are re-introduced in this revision with named failure modes, FRs, and RV rows. NAC-ABE internal cache renewal as such remains out of scope: the executed release already renews the Consumer/Producer cache through the patched API (`clearCache`/`refreshDecryptionKey`/`refreshPublicParameters`), and upstreaming that contract surface is FR-041. The 2026-09-04 release claim is unaffected; its executed trust-schema validation (`ConfiguredTrustSchemaControlsControllerStatusValidation`, `LargeResponseUsesConfiguredTrustAndRequestBoundAead`), runtime restart fail-closed behavior (`RuntimeRestartDropsControllerStatusAndFailsClosed`), and Controller restart restoration (`ServiceControllerRestoresEveryRevocationKindAfterRestart`, MiniNDN `controller-restart`) remain normative evidence.

## Amendment 2026-09-05 — runtime-hardening follow-ups (FR-039–FR-041)

This revision re-introduces the three recorded follow-ups by naming the
concrete failure mode each protects against and assigning an FR plus
RV-U/RV-I rows (see `tasks.md` Phase 7 and `validation-matrix.md`
"Amendment 2026-09-05 rows"). Each FR is satisfied only by its listed
executed evidence; none changes the 2026-09-04 release claim.

- **FR-039**: A User or Provider runtime MAY persist its accepted per-service
  Controller status in an explicitly enabled durable mode; when enabled, the
  runtime MUST atomically store each accepted `PolicyStatusData` wire (with
  its signature) plus the bound public-parameter name/digest and
  ControllerVersion, MUST fail closed on a missing, truncated, or
  unverifiable store (today's restart behavior stays the default), MUST
  restore only statuses that are unexpired and never superseded by a version
  the process itself observed, MUST immediately schedule a bounded online
  confirmation refresh and accept the Controller's signed answer as
  authority (higher version replaces; equal version is idempotent), and MUST
  keep deciding authorization from the restored status when the Controller is
  unreachable until expiry or a higher observed version. Restoring status
  restores authorization and refresh decisions only: decrypting protected
  request/response material still requires a fresh DKEY from the reachable
  AA, so an offline Controller with an offline AA cannot decrypt new
  traffic — this online-material limitation is unchanged from the release
  and is recorded, not hidden. Failure mode: today
  (`RuntimeRestartDropsControllerStatusAndFailsClosed`) a runtime restart
  discards still-valid authority even though FR-037 guarantees equal-version
  continuity only inside one process lifetime, so a restart during a
  Controller outage turns a bounded outage into an unbounded one and forces a
  full online bootstrap on every restart. Rows: RV-U23 (unit), RV-I32
  (component restart recovery). Design guidance and the opt-in
  configuration are in `plan.md` Amendment guidance.
- **FR-040**: Controller status installation MUST validate under a
  hierarchical configured trust anchor of arbitrary depth (anchor → one or
  more intermediate CAs → Controller certificate), not only a directly
  anchored Controller certificate; a signature from any certificate outside
  the anchored chain MUST fail closed before installation. Failure mode: the
  release executed trust-schema validation only with the Controller
  certificate directly anchored (or `trust-any`), so a production
  hierarchical PKI deployment has no executed evidence that live status
  installation works under its schema. Rows: RV-I33 (component hierarchical
  accept/reject). The framework's `ValidatorConfig`-based schema loading is
  reused; no trust-schema redesign is intended.
- **FR-041**: The Spec179 NAC-ABE dependency contract — DKEY segments with
  `FreshnessPeriod=0` (grant-only replacement cannot be hidden by a fresh
  Content Store copy), `Consumer::clearCache`/`refreshDecryptionKey`/
  `getPublicParams{Name,Digest}`, `CacheProducer::refreshPublicParameters`
  — MUST be accepted into the upstream NAC-ABE repository through
  maintainer review, and the NDNSF build prefix MUST then be rebuilt from the
  upstream commit with the RV-U20/RV-U21 gate re-run on that rebuild. Failure
  mode: the contract surface exists only on the local, unpushed NAC-ABE
  `Experimental` branch (`b1c9c4f`), so any clean environment fails the
  NDNSF build (compile-contract evidence `RV-U22`,
  `evidence/nac-abe-unpatched-contract-20260905.md`) and the dependency is
  one lost local branch away from unbuildable. Row: RV-U22 (compile
  contract, executed 2026-09-05) plus re-execution of RV-U20/RV-U21 on the
  upstream-pinned rebuild as the acceptance step.
