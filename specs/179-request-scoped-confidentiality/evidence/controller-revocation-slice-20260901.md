# Spec179 Controller-Revocation Executed Slice

## Current-source addendum (2026-09-02)

The focused Controller-policy executable was rebuilt from the current source
objects with system Boost 1.71 and ndn-cxx linkage and passes 31/31; `ldd`
resolves all Boost dependencies to 1.71.0. The current
`build-clang-nodbg/integration-tests` executable passes 34/34
Controller-revocation cases, including configured file trust-anchor
acceptance/untrusted-signer rejection, the Provider-side Targeted
denial/reauthorization case, and the configured no-status refusal. The
current stream regression passes 19/19, including the revoked-User
Targeted-bootstrap refusal. These are bounded unit/component results; the
monolithic Waf unit target, production-runtime trust-schema path, full Targeted
token/refill and stream-Provider matrix, recovery cases, and MiniNDN network
evidence remain separate release gates. The older 32/32 and 18/18 counts
below describe the historical pre-extension run and are not current totals.

`ConfiguredTrustSchemaControlsControllerStatusValidation` intentionally uses
the configured trust-schema validator rather than the local-PIB signature
shortcut; the trusted Controller certificate is accepted and the untrusted
certificate is rejected. This closes the explicit validator component slice,
not production User/Provider status installation or MiniNDN propagation.

## 2026-09-02 current test additions

The current-source refresh tests were extended with a cache-scope regression.
Unit `RevocationStateInvalidatesOnlyAcceptedServiceFamilies` passes and checks
that only an accepted newer status invalidates all six authorization-material
families, while a status for another service is rejected without invalidating
the local state. Component `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic`
passes with the same assertions after a real Controller mutation, signed
status wire round-trip, and exact-version refresh. This is component evidence
for cache invalidation, not proof of persistent runtime-cache recovery or
cross-process propagation.

The refresh-state source now includes
`AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`. The current
Clang executable runs 10/10 `ControllerRevocationState` cases. This regression
keeps an authenticated message hint as a refresh trigger only: an invalid
exact status exhausts the bounded retry budget without installing authority,
and a later valid exact status is the first accepted authority.

`ServiceControllerPublishesStableTimestampedRevocationSnapshot` now also
invokes the real exact-name policy-status Interest handler, checks the signed
Data with the Controller certificate, decodes the version/validity/target, and
verifies the current validity window. The current Controller-revocation
component executable runs 34/34 cases. These additions close only the
coordinator no-status branch and the Controller publication route; production
User/Provider trust-schema installation, cross-process propagation, and the
full target × cut-point × mode × recovery matrix remain pending.

The current-source request-confidentiality focused executable
`/tmp/spec179-request-crypto-20260903` also passes 13/13. Its result is kept
separate from the Controller revocation evidence because it validates the
request/selection cryptographic primitives and does not prove live status
propagation.

The focused Targeted API executable `/tmp/spec179-targeted-revocation` passes
both `RevokedUserCannotConsumeTargetedTokenOrStartRefill` and
`RevokedProviderCannotConsumeTargetedTokenOrExecuteHandler`. After a newer
identity-revoking status is installed, the User returns no request ID, emits
no Request/Bootstrap, and leaves the cached token pool unchanged; the Provider
rejects a valid-token request without a second handler invocation. This closes
the User-side pre-consumption and Provider-side execution gates only;
token-batch response handling and cross-process refill propagation remain
open.

## 2026-09-02 current-source rerun

The newly added cache-scope assertions pass in the current build. The complete
Controller revocation suites now pass 11/11 refresh-state unit cases and 34/34
Controller-flow cases plus 1/1 refresh case (35/35 combined); the full unit target passes 680/680. The directly related
integration subset (Controller revocation/refresh, request-scoped response and
selection, and invocation-stream suites) passes 58/58. A full 121-case
integration run still reports one unrelated existing failure in
`NdnsfDataV1SvsFlow/ProductionProviderContextUsesSvsSegments` at
`tests/integration-tests/ndnsf-data-v1-svs-flow.t.cpp:434`; it is retained as a
separate integration gate and is not attributed to Spec179.

## 2026-09-02 test-plan extension

Six additional revocation checks were added to the current source and compiled
successfully with the current C++17 toolchains:

- Unit `GlobalCertificateRevocationMatchesDigestAcrossIdentitiesAndServices`
  checks that a certificate-only target without an identity applies by digest
  across identities and service snapshots, while a replacement certificate
  remains allowed.
- Component
  `ServiceControllerRejectsStaleExactStatusAfterRevocation` checks that a
  Controller refuses a cached old exact status name and a forged future epoch
  after revocation, while publishing only the current signed status.
- Unit `BoundCertificateRevocationDoesNotOverRevokeAnotherIdentity` checks
  that an identity-bound certificate digest does not revoke another identity
  presenting the same digest, while replacement material remains allowed.
- Unit `RevocationStateRejectsAtStatusValidityEnd` checks the half-open
  `validFrom <= now < validUntil` rule for every protected transition.
- Component extensions to `LiveControllerStatusRefreshRejectsRevokedRenewal`
  and `RealRuntimeEnforcesCertificateAndServiceRevocation` reject replay of an
  older status and deny Provider identity execution until a newer status
  explicitly reauthorizes it.
- Component `ServiceControllerRestoresEveryRevocationKindAfterRestart` checks
  that identity-wide, global certificate-only, and service-scoped targets are
  all restored before the next Controller generation is published, without
  losing global or service scope.

The source counts are now 31 Controller-policy unit cases and 34
Controller-revocation component cases. Fresh current-tree runs now pass both
the focused policy and component assertions: the isolated policy executable
passes 31/31, and the Clang Waf integration executable passes 34/34. All other
coverage and limitations below
remain unchanged.

> Source revision note (2026-09-02): after the original focused run, the
> Controller status-name test, configured-runtime hint handling, and refresh
> terminal-boundary test and real Controller target-kind matrix were tightened.
> The current source state has isolated refresh-state results of 10/10 and
> Controller-policy results of 31/31, and the current integration executable
> passes the full Controller-revocation suite 34/34.

## 2026-09-02 reproducible rerun (historical isolated copy)

The following commands document an earlier current-tree rerun. The Waf targets
were rebuilt first; because `build-clang/*` is on a `noexec` mount, the linked
executables were copied to `/tmp` before execution. This is an execution-path
detail, not a source or test substitution.

```text
./waf -v build --targets=unit-tests -j2
./waf -v build --targets=integration-tests -j2

/tmp/spec179-unit-current-20260902 \
  --run_test=ControllerRevocationPolicy,ControllerRevocationState \
  --log_level=message
# Running 40 test cases ... No errors detected

/tmp/spec179-integration-current-20260902 \
  --run_test=ControllerRevocationFlow --log_level=message
# Running 31 test cases ... No errors detected

/tmp/spec179-integration-current-20260902 \
  --run_test=ControllerRevocationFlow,ControllerVersionRefresh \
  --log_level=message
# Running 32 test cases ... No errors detected
```

That isolated rerun established 31/31 Controller-policy cases, 9/9
refresh-state cases, 31/31 Controller-revocation component cases, and 1/1
Controller-version-refresh component case. It does **not** establish the
configured trust-schema, cross-process MiniNDN, Targeted, full segmented-stream,
offline/rejoin, persistent runtime-cache, or execution/key-disclosure gates
listed in the validation matrix.

## 2026-09-02 all-target restart regression

After the additional restart case was added, the current Clang integration
binary was rebuilt from the repository root and the complete Controller suite
was rerun (the root working directory is required because the component tests
load `examples/*.policies` and validator configuration by relative path):

```text
/tmp/spec179-integration-current-20260902-alltargets \
  --run_test=ControllerRevocationFlow --log_level=message
# Running 32 test cases ... No errors detected
```

`ServiceControllerRestoresEveryRevocationKindAfterRestart` now verifies that
identity-wide, global certificate-only, and service-scoped targets are all
restored before the new generation is published; the restored `/HELLO` status
contains three targets while an unrelated service contains only the two global
targets. The assertion also checks replacement-certificate and cross-service
controls. This closes the component recovery slice for typed-target
persistence, but it remains separate from configured trust-schema and
cross-process MiniNDN recovery evidence.

Date: 2026-09-01
Branch: `UAV-Experimental`

## Follow-up test update

The historical commands and counts below document the earlier 27-case policy
and 29-case component runs. After adding the explicit message-version negative
and timestamped immutable-snapshot cases, the current source was rebuilt and
rerun in isolated executables:

```text
/tmp/spec179-controller-policy-gcc \
  --run_test=ControllerRevocationPolicy --log_level=test_suite
# Running 28 test cases ... No errors detected
/tmp/spec179-controller-flow-20260903b \
  --run_test=ControllerRevocationFlow --log_level=test_suite
# Running 30 test cases ... No errors detected
/tmp/spec179-request-selection-20260903b \
  --run_test=RequestScopedSelection --log_level=test_suite
# Running 3 test cases ... No errors detected
```

The new unit case rejects zero and duplicate `ControllerVersion` TLVs in
Request, ACK, Selection, and Response messages. The new component case proves
that a real Controller publishes a newer, currently valid revocation status
whose repeated reads at one version are byte-identical. These remain
component/focused results; configured trust-schema, cross-process propagation,
Targeted/stream recovery, and MiniNDN gates are still pending.

## Commands

The new files were compiled with the repository's configured `/usr/bin`
GCC-9 C++17 toolchain and installed `libndn-cxx` dependencies. The focused
unit and component suites were linked as isolated Boost.Test executables because the
monolithic unit target includes large DI translation units that have
previously triggered GCC-9 assembler/relocation failures. The integration
target was rebuilt with the current tree; the current-tree focused commands
are:

```text
/tmp/spec179-current-controller-policy-new2 --run_test=ControllerRevocationPolicy --log_level=test_suite
# Latest current-source focused unit executable:
/tmp/spec179-controller-policy-new-current --run_test=ControllerRevocationPolicy --log_level=message
/tmp/spec179-controller-state-unit-current \
  --run_test=ControllerRevocationState --log_level=message
/tmp/spec179-current-controller-flow --run_test=ControllerRevocationFlow --log_level=test_suite
/tmp/spec179-current-request-crypto --run_test=RequestScopedConfidentiality --log_level=test_suite
./waf build --target=integration-tests -j2
./build-clang/integration-tests --run_test=ControllerRevocationFlow \
  --log_level=message
./build-clang/integration-tests \
  --run_test=ControllerVersionRefresh --log_level=message
timeout 120s ./build-clang/integration-tests \
  --run_test=Spec175InvocationStream --log_level=message

# Current-source focused stream unit regression (monolithic unit target remains
# unavailable on this host):
clang++ -O2 -g -std=c++17 ... -c \
  tests/unit-tests/invocation-stream-message.t.cpp \
  -o /tmp/spec179-invocation-stream-message.o
/tmp/spec179-invocation-stream-unit-current \
  --run_test=Spec175InvocationStreamMessage --log_level=message

# Current-source isolated state test (Clang, Boost 1.71 system libraries):
/tmp/spec179-controller-state-unit-current \
  --run_test=ControllerRevocationState --log_level=message
```

## Results

### Incremental Controller test rerun

After adding `GenerationStoreRejectsInvalidWriterAndStartInputs` and the
post-revocation status-validity assertions, the current source was compiled
with Clang and relinked against the existing Boost 1.71 framework objects:

```text
/home/tianxing/NDN/ndn-service-framework/build-clang/unit-tests \
  --run_test=ControllerRevocationPolicy --log_level=message
# Running 31 test cases ... No errors detected
/home/tianxing/NDN/ndn-service-framework/build-clang/integration-tests \
  --run_test=ControllerRevocationFlow --log_level=message
# Running 32 test cases ... No errors detected
```

The new unit case covers missing/empty writers, zero generation timestamps,
pre-start epoch advancement, and post-release advancement. The existing real
Controller mutation case now also checks that an accepted withdrawal advances
the version and refreshes the validity window, while a duplicate target leaves
both values unchanged. These are focused component results; they do not close
configured trust-schema or MiniNDN lifecycle gates.

- Controller unit: the current-source focused executable, relinked with the
  matching current `NDNSFMessages.cpp` object, passes all 31 cases, including
  `PolicyStatusWirePreservesValidityTimestamps`. The source contains 31 cases,
  including complete target-shape validation, canonical field ordering,
  same-identity multi-role withdrawal, invalid-status preservation, future
  validity, global certificate replacement, conflicting equal-version
  fail-closed branches, repeated starts under clock rollback, stale-writer
  fencing after lease replacement, and the explicit
  `RevocationTargetKindsCoverEveryProtectedTransition` identity/certificate/
  service matrix across all six modeled cut points with unaffected and
  replacement controls. The current executable is the build-clang artifact;
  older 27-case temporary binaries are historical evidence only.
- Refresh-coordinator unit: the historical executable passed 6/6 cases;
  the current-source isolated Clang executable passes 10/10, adding exact
  status-name/version addressing, authenticated-hint/no-status exact-fetch
  gating, and no-fetch, equal-version, expiry, retry, and terminal-boundary
  assertions. The suite covers authenticated-hint gating, highest-candidate
  coalescing, hintless scheduling, bounded retry, equal-version conflict, and
  service-scope/missing-status fail-closed behavior.
- Component integration source: the established executable passed the complete
  `ControllerRevocationFlow` suite, including the same-identity Controller
  writer-conflict case. The suite includes a real
  `ServiceController` authority mutation case, identity-wide and
  certificate-only withdrawal across all six modeled cut points with matched
  replacement/unaffected controls, a Controller status wire
  round-trip with all target scopes, restart restoration of revocations,
  repeated restart/status publication with old-generation exact-name refusal,
  revoked User/Provider permission-snapshot filtering (including identity-wide
  User withdrawal), direct policy-status handler refusal for malformed and
  unavailable state, immutable version-addressable recipient-bound User/Provider permission
  issuance with wrong-recipient rejection and identity-wide/certificate-only
  zero-grant denial snapshots, wrong-signer/content-tamper status negatives,
  ParametersSha256Digest normalization, same-identity Controller writer-
  conflict refusal, the real LocalMock User/Provider
  normal Request publication/execution revocation boundary (RV-I23), and
  certificate-only/service-scoped runtime withdrawal with newer-version
  reauthorization.
  The current source additionally includes
  `ServiceControllerRevocationRollsBackAfterWriterLoss`,
  `ServiceControllerRejectsInvalidRevocationTargetsWithoutEpochAdvance`,
  `ServiceControllerRevocationRollsBackWhenStateCannotBeRead`,
  `RealControllerStatusDrivesUserAndProviderRevocation`, and
  `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`, plus
  `LiveControllerStatusRefreshRejectsRevokedRenewal`; these cases are present
  in the current source/object and are included in the current focused
  31/31 executable run. The latter two prove that real
  Controller-generated status applies identity, certificate, and service
  targets at all six modeled cut points while preserving replacement,
  unrelated, and cross-service controls, and that signed permission and
  PolicyStatus Data refresh the exact ControllerVersion after empty revoked
  renewals before User publication and Provider execution are denied.
- Refresh component integration: 1/1 case passed. It composes a real
  `ServiceController`-produced status with `PolicyRefreshCoordinator` and
  `RevocationState`, covering hintless pre-expiry scheduling, single-flight
  duplicate-hint coalescing, status wire round-trip, higher-version
  revocation installation, and a matched unaffected authorization control.
- Request crypto: 13/13 cases passed in the current-tree focused executable,
  including certificate-advertisement and protected-message-container
  round-trips.
- Request-scoped large-response coverage includes
  `RequestScopedLargeResponseUsesPerSegmentAeadReference` and
  `RequestScopedSelection/SelectedProviderReceivesExactEncryptedInputOnly`,
  which passes with a 9,000-byte response through the compact reference,
  Provider IMS, configured SegmentFetcher, ordered per-segment AEAD checks, and
  User-side reconstruction. The production fix keeps each signed segment in a
  shared owner before IMS insertion; the previous stack-owned Data caused
  `bad_weak_ptr` and an error Response. This closes the normal component
  large-response path, but not large-response revocation, Targeted/stream
  revocation, or the MiniNDN network gate.
- Invocation stream unit passes 16/16 and the current-source integration
  regression passes 17/17 after adding the optional ControllerVersion stream
  wrapper and a final-delivery authorization callback. The focused unit run
  covers version-wrapper round-trip, legacy versionless decoding,
  invalid-version rejection, and binding-digest sensitivity; the integration
  run additionally lets the first event be delivered, installs a newer
  identity-revoking status, and rejects an already-buffered later event before
  its application callback. This remains LocalMock evidence rather than proof
  of configured trust-schema, restart/rejoin, Targeted, or MiniNDN propagation.
  One initial full-suite rerun exposed the pre-existing cancellation test's timing-sensitive
  `events.size()==2` result; two subsequent full-suite reruns and three focused
  cancellation reruns passed, so the flake is retained as a separate stream
  stability item rather than attributed to ControllerVersion binding.
- Focused current-source integration run:
  `ControllerRevocationFlow` passed 31/31 cases. This includes the configured
  no-status fail-closed regression (`user_publish=0`, `provider_execute=0`),
  higher-hint non-adoption assertions, status publication checks, the
  request-scoped exact-input selection flow, and the real Controller
  target-kind × six-cut-point matrix and the live signed-status/empty-renewal
  denial path.
- `./waf build --target=integration-tests -j2` completed in the current
  Clang/Boost-1.71 build directory and produced the executable used for the
  31/31 focused run above. Earlier GCC-9 compiler/assembler failures in
  unrelated UAV/DI translation units remain a separate full-target toolchain
  limitation.
- `./waf build --target=unit-tests` reached GCC-9
  internal compiler error while compiling
  `generic-dynamic-api-tokens-replay.t.cpp` in an earlier attempt. The latest
  current-tree attempt timed out after 300 seconds while compiling the
  monolithic target and did not produce `build/unit-tests`. The new
  `controller-revocation-state.t.cpp` object is present and contains
  `HigherHintNeverBecomesAuthorityBeforeExactStatusFetch`; the focused
  controller executable remains the deterministic unit result.

## Added controller-revocation cases in this revision

The test additions are intentionally split by responsibility:

| Layer | New case | Evidence intended |
|---|---|---|
| Unit | `GenerationStoreRejectsInvalidWriterAndStartInputs` | Missing writer, empty owner, zero generation timestamp, pre-start epoch advance, and post-release publication all fail closed without creating authority. |
| Unit | `RevocationTargetKindsCoverEveryProtectedTransition` | Every canonical target shape (identity, certificate, service) denies the named subject at Discovery, ACK collection, Selection, Provider execution, Response delivery, and Stream event while a replacement/unaffected subject remains authorized. |
| Component | `ServiceControllerRevocationRollsBackAfterWriterLoss` | A fenced-out real Controller cannot advance its epoch or retain a newly appended target after persistence/lease failure. |
| Component | `RealControllerStatusDrivesUserAndProviderRevocation` | A status generated by the real Controller is installed into real LocalMock User/Provider objects; Provider service revocation denies execution while User revocation prevents the next Request publication. |
| Component | `ServiceControllerRevocationRollsBackWhenStateCannotBeRead` | A real Controller with unreadable durable state refuses to publish authority and leaves the prior writer's state unchanged. |
| Component | `LiveControllerStatusRefreshRejectsRevokedRenewal` | Controller-signed permission and PolicyStatus Data traverse the runtime Face relay; empty revoked renewals advance the exact version, then User publication and Provider execution are denied. |
| Component | `ServiceControllerPermissionHandlersEncryptCurrentRevocation` | Certificate-only withdrawal returns an empty current-version snapshot for the affected Provider, while a same-service unaffected Provider still receives `/HELLO`; this closes the paired issuance-control gap without claiming full runtime or configured trust-schema coverage. |
| Component | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` | A real Controller-generated status is accepted by a component RevocationState for identity, certificate, and service targets at Discovery, ACK collection, Selection, Provider execution, Response delivery, and Stream event; replacement, unrelated, and cross-service controls remain allowed. |
| Component | `RealRuntimeEnforcesCertificateAndServiceRevocation` | LocalMock User and Provider runtimes exercise certificate-only and service-scoped withdrawal independently: affected User publication or Provider execution is denied, a newer status without the target reauthorizes the same certificate, and execution remains at-most-once. |

## 2026-09-02 stale-message regression

The Controller revocation integration source was extended so that, after the
Provider installs a newer non-revoking status, it receives the previously
captured request carrying the older ControllerVersion. The Provider rejects it
before invoking the handler, and the execution counter is unchanged. The
current Clang/Boost-1.71 executable was rebuilt in `build-clang`, copied to
`/tmp` because the build mount is `noexec`, and ran:

```text
/tmp/spec179-integration-current-20260902 \
  --run_test=ControllerRevocationFlow --log_level=message
  Running 31 test cases ... No errors detected

/tmp/spec179-integration-current-20260902 \
  --run_test=ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,Spec175InvocationStream \
  --log_level=message
  Running 52 test cases ... No errors detected

/tmp/spec179-unit-current-20260902 \
  --run_test=ControllerRevocationPolicy,ControllerRevocationState,RequestScopedConfidentiality,GenericDynamicApiTargeted \
  --log_level=message
  Running 53 test cases ... No errors detected
```

This closes the LocalMock normal-request stale-message enforcement assertion.
It does not close configured production trust-schema validation, persistent
cache/restart recovery, Targeted or stream propagation through a real network,
or the MiniNDN Cartesian matrix.

These cases close deterministic Controller mutation and normal unary runtime
gaps. They do not substitute for configured trust-schema verification,
cross-process status/permission renewal, large/Targeted/stream traffic,
offline/rejoin, or the MiniNDN Cartesian lifecycle matrix; those remain
explicitly pending below.

## Behaviors exercised

The executed slice covers canonical ControllerVersion ordering and wire
round-trip, strict typed identity/certificate/service revocation targets,
every invalid target shape, service-scope checks, status validity and signature
gate, missing/incomplete/expired authorization subjects, durable generation
monotonicity across clock rollback, corrupt-state fail-closed behavior,
single-writer fencing, all six protected lifecycle cut points, cache-family
invalidation, exactly-one terminal recording, certificate replacement,
same-identity multi-role withdrawal, canonical status field order,
invalid-status replacement preservation, forward-only reauthorization,
Controller-produced status wire round-trip,
direct permission-snapshot filtering, and equal-version operation until signed
status expiry.

## Boundary

This is component evidence, not a release claim. The direct Controller
authority mutation, policy-snapshot filtering, and recipient-bound
permission-handler outputs are covered, but complete `ServiceController`
issuance through live User/Provider validators,
  live permission/status retrieval through `ServiceUser`/`ServiceProvider`
  validators, status retrieval from
  Controller/Provider/cache, live refresh coalescing and expiry enforcement,
  large/Targeted/stream traffic,
and the MiniNDN network rows remain unrun and therefore pending in the
normative matrix.

## 2026-09-02 response-confidentiality component gate

The current integration build also includes the dedicated
`RequestScopedResponseConfidentiality/LargeResponseUsesConfiguredTrustAndRequestBoundAead`
case. It passed 1/1 after fetching a retained request-scoped response segment
through the Provider IMS, validating the Provider signature with the configured
trust schema, checking the exact ControllerVersion/segment AAD and unique
nonce, and rejecting ciphertext, Provider-certificate, and ControllerVersion
mutations. The full three-case `RequestScopedSelection` suite, the 34-case
`ControllerRevocationFlow` suite, the one-case refresh suite, and the 680-case
unit suite also passed at that earlier evidence checkpoint.

This gate strengthens the response evidence but remains component-level: it
does not claim production trust-schema installation, full segmented retry,
Targeted refill, stream restart/rejoin, or privileged MiniNDN propagation.

## 2026-09-02 revocation-cache regression update

The current source adds service-scoped User/Provider Targeted-cache eviction
and preserves public terminal behavior for in-flight requests. The focused
Targeted executable passes 27/27 cases, and the Spec179 integration subset
passes 58/58 after the cache and replay-tombstone fixes. The full unit target
now executes 682 cases but has one unrelated failure in
`Stream/LiveStreamGf256RepairRecoversAnyTwoOpaqueSources`
(`tests/unit-tests/stream.t.cpp:1925`); this is recorded as an independent
release blocker, not as Controller-revocation evidence.
