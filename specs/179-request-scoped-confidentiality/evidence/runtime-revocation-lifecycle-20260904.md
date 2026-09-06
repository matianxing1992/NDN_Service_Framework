# T009 — Runtime revocation lifecycle execution record

Date: 2026-09-04 (CDT).  Binary: `build-clang-spec179-nac3`
(Clang 10; `LD_LIBRARY_PATH=/tmp/nac-abe-spec179-exact-prefix/lib`).
Current worktree HEAD `e2d793e8` plus today's grant-only refresh fixes.

## Method

Each CI family named by T009 was executed on the current source at
`--report_level=detailed` (per-case outcome + assertion count) and
`--log_level=message` (NDNSF_REVOCATION_*/REPLAY_* telemetry):

```text
integration-tests --run_test=<family> --log_level=message --report_level=detailed
```

All five families exit 0 on 2026-09-04 (one in-suite schedule-dependent
flake in `Spec175InvocationStream` is documented at the end; the family
re-ran 19/19 green).

| CI family (file) | executed | result |
|---|---|---|
| `ControllerRevocationFlow` ([controller-revocation-flow.t.cpp](tests/integration-tests/controller-revocation-flow.t.cpp)) | 38 cases | 38/38 passed |
| `ControllerVersionRefresh` ([controller-version-refresh.t.cpp](tests/integration-tests/controller-version-refresh.t.cpp)) | 1 case | 1/1 passed |
| `RequestScopedSelection` ([request-scoped-selection.t.cpp](tests/integration-tests/request-scoped-selection.t.cpp)) | 3 cases | 3/3 passed |
| `RequestScopedResponseConfidentiality` ([request-scoped-response-confidentiality.t.cpp](tests/integration-tests/request-scoped-response-confidentiality.t.cpp)) | 4 cases | 4/4 passed |
| `Spec175InvocationStream` ([invocation-stream-flow.t.cpp](tests/integration-tests/invocation-stream-flow.t.cpp)) | 19 cases | 19/19 passed (see flake note) |

Unit-side owners named by T006/T009 executed inside today's full unit
run (686 cases, single pre-existing out-of-scope SIGFPE — see
[regression-red-green-20260904.md](regression-red-green-20260904.md)):
`RevokedUserCannotConsumeTargetedTokenOrStartRefill`
([generic-dynamic-api-targeted.t.cpp:165](tests/unit-tests/generic-dynamic-api-targeted.t.cpp#L165))
passed; the `RevocationState` component sweep
(`RevokedUserAndProviderHavePairedUnaffectedControls`,
`ServiceRevocationCoversEveryCutPointWithoutCrossServiceDenial`, …)
passed inside the green `ControllerRevocationState`/`ControllerRevocationPolicy`
unit suites.

## Enforcement matrix (executed rows)

Legend: path — normal (Request/ACK/Selection/execution/Response), large,
Targeted, stream.  Target — user-identity, provider-identity,
certificate-only, User-`/PERMISSION`, Provider-`/SERVICE`.

| # | Case | Path | Target / scope | Boundary evidence (telemetry / assertion) |
|---|---|---|---|---|
| 1 | `RevokedUserAndProviderHavePairedUnaffectedControls` (component, t.cpp:395) | normal | user-identity + provider-identity | every transition denied after identity revocation; exactly-once terminal (`ledger.terminal.count==1` on double deny); unaffected paired control (bob) stays allowed |
| 2 | `ServiceRevocationCoversEveryCutPointWithoutCrossServiceDenial` (component, t.cpp:515) | normal | Provider-`/SERVICE` | all 6 protected transitions denied (`revoked_before_*`); same provider on `/OtherService` allowed (no cross-service denial) |
| 3 | `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` (t.cpp:3061) | normal | identity / certificate / service | `NDNSF_REVOCATION_CONTROLLER_TARGET_MATRIX identity=6/6 certificate=6/6 service=6/6 cross_service=allow` — real Controller-issued typed status accepted and applied at every protected cut point |
| 4 | `RealControllerStatusDrivesUserAndProviderRevocation` (t.cpp:2549) | normal | user-identity + provider-identity | `NDNSF_REVOCATION_CONTROLLER_RUNTIME controller_status=accepted provider_revoke=execution_denied user_revoke=publication_denied` |
| 5 | `RealUserAndProviderEnforceControllerRevocationAtRuntime` (t.cpp:3168) | normal | user-identity + provider-identity | production LocalMock dispatch boundary: `NDNSF_REVOCATION_RUNTIME user_prepublication=deny provider_execution=deny unaffected_before=allow` |
| 6 | `RealRuntimeEnforcesCertificateAndServiceRevocation` (t.cpp:3436) | normal + Targeted discovery | certificate-only, User-`/PERMISSION`, Provider-`/SERVICE` | `NDNSF_REVOCATION_RUNTIME provider_certificate_only=deny provider_service_scope=deny user_certificate_only=deny provider_identity_only=deny user_service_scope=deny targeted_discovery=deny provider_identity_reauthorized=allow user_reauthorized=allow` — scope rules are not identity-wide; reauthorized identities live again |
| 7 | `LiveControllerStatusRefreshRejectsRevokedRenewal` (t.cpp:2699) | normal (live Controller→runtime fetch) | user-identity + provider-identity, signed renewal | `NDNSF_REVOCATION_LIVE_REFRESH permission_fetch=accepted user_revoke=renewal_denied provider_revoke=execution_denied` — signed permission renewal denied after revocation |
| 8 | `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut` (t.cpp:2903) | normal | grant-only | see [grant-only-single-issuance-20260904.md](grant-only-single-issuance-20260904.md): one target-only DKEY fetch, zero unaffected fan-out, idempotent repeat |
| 9 | `RealTargetedProviderRejectsRevokedExecution` (t.cpp:3618) | Targeted | provider-identity (execution transition) | `NDNSF_REVOCATION_TARGETED provider_execution=deny reauthorized_after_new_version=allow` — Targeted skips ACK/Selection only after token bootstrap; Provider execution stays Controller-protected |
| 10 | `RevokedUserCannotConsumeTargetedTokenOrStartRefill` (unit, generic-dynamic-api-targeted.t.cpp:165) | Targeted | user-identity (token/refill gate) | revoked user cannot consume a Targeted token or start refill; passed in full unit run |
| 11 | `TargetedStreamRevocationStopsBeforeBootstrap` (invocation-stream-flow.t.cpp:547) | stream + Targeted | user-identity | revocation stops the streamed-Targeted bootstrap before it starts |
| 12 | `StreamEventAfterUserRevocationIsRejectedBeforeDelivery` (invocation-stream-flow.t.cpp:449) | stream | user-identity | stream event rejected before User delivery |
| 13 | `StreamEventAfterProviderRevocationIsRejectedAtPublication` (invocation-stream-flow.t.cpp:601) | stream | provider-identity | Provider Face commit/publication boundary rejects after revocation |
| 14 | `RevokedUserCannotReceiveResponseAfterProviderExecution` (request-scoped-selection.t.cpp:447) | normal (large path variant) | user-identity | response-time revocation after Provider execution → User delivery denied (key-disclosure-before-enforcement control: provider already executed) |
| 15 | `RevokedUserCannotReceiveLargeResponseAfterProviderExecution` (request-scoped-response-confidentiality.t.cpp:473) | large | user-identity | segmented large-response delivery denied after revocation; sibling `NormalResponseCompletesThroughProductionOnResponse` / `LargeResponseCompletesThroughProductionOnResponse` / `LargeResponseUsesConfiguredTrustAndRequestBoundAead` prove the unaffected modes still complete |
| 16 | `RuntimeRestartDropsControllerStatusAndFailsClosed` (t.cpp:3282) | normal | participant restart | `NDNSF_REVOCATION_RESTART status_lost=fail_closed stale_permission=denied` — process-local authority; restart never resurrects obsolete status/binding/credential |
| 17 | `ServiceControllerRestoresRevocationsAfterRestart` / `RestoresEveryRevocationKindAfterRestart` / `StatusVersionIsMonotonicAcrossRestarts` (t.cpp) | normal | controller restart | `NDNSF_REVOCATION_RESTART target_kinds=identity,certificate,service hello_targets=3 other_service_targets=2 restored_epoch=1` — durable state restores every kind; version monotonic across restarts |
| 18 | `RestartClockRollbackAndControllerUnavailableAreBounded` (t.cpp) | normal | clock rollback, Controller unavailable | bounded fail-closed behavior without wall-clock trust |
| 19 | `NewerServiceStatusDoesNotAuthorizeOtherService` (t.cpp:3352) | normal | cross-service isolation | `NDNSF_REVOCATION_SERVICE_ISOLATION service_a=newer service_b=exact` — a newer status of service A never authorizes service B |
| 20 | `ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic` (controller-version-refresh.t.cpp) | normal | concurrent affected/unaffected | revocation refresh completes while unrelated traffic proceeds (1/1 passed) |
| 21 | `ConfiguredControllerFailsClosedBeforeStatusInstallation` (t.cpp) | normal | no-status | `NDNSF_REVOCATION_RUNTIME no_status=fail_closed user_publish=0 provider_execute=0` — configured runtime fails closed before any signed status |
| 22 | `ServiceControllerFailsClosedWhenGenerationStateIsCorrupt` / `RevocationRollsBackAfterWriterLoss` / `RollsBackWhenStateCannotBeRead` / `RejectsSecondWriterAndPreservesAuthority` / `RejectsStaleExactStatusAfterRevocation` (t.cpp) | normal | durability, rollback, single-writer fencing, stale status | Controller-side component rows, all 38-case family green |
| 23 | `ControllerProviderAndCacheCopiesHaveIdenticalAuthority` (t.cpp) | normal | source equivalence | Controller/Provider/cache authority copies identical |

Dual-role `/PERMISSION/S` vs `/SERVICE/S` independence (both
directions, one identity holding both attributes for the same service)
is proven inside
`ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`
([t.cpp:948-1000](tests/integration-tests/controller-revocation-flow.t.cpp#L948-L1000)):
withdrawing `SERVICE_AUTHORIZATION(/PERMISSION/S)` leaves `/SERVICE/S`
intact and vice versa, at both the `isRevoked` and the ABE-policy level
(`dual-role-use`, `dual-role-provision`).  Identity reauthorization after
withdrawal against the current generation is proven in the same case
(t.cpp:1002-1010, `identityReauthorized`).

## Feature-coverage claims

- **Signed permission-renewal denial** — #7 (`renewal_denied`).
- **Reauthorization** — #6 (`provider_identity_reauthorized=allow`,
  `user_reauthorized=allow`), #9 (`reauthorized_after_new_version=allow`),
  component reauth in `ServiceControllerGrantOnly...`.
- **Controller unavailability** — #18; bounded, fail-closed.
- **Participant/Controller restart** — #16, #17.
- **Offline epoch skipping** — #16 (restart with a stale permission
  snapshot present still fails closed until a fresh signed status) plus
  Controller-side stale-status refusal (#22).
- **Concurrent affected/unaffected traffic** — #20; unaffected controls
  in #1/#3/#5 (`cross_service=allow`, `unaffected_before=allow`).
- **Execution/key-disclosure-before-enforcement** — #14/#15 (Provider
  executes under a valid grant; User revocation lands before delivery).
- **Exactly one terminal result** — component ledger `rejectExactlyOnce`
  (#1) and Request/terminal telemetry
  (`NDNSF_REQUEST_TERMINAL ... RESET`) across the selection and
  response-confidentiality cases.
- **Default-disabled post-Selection retry/reselection** — no automatic
  reselection/retry mechanism exists in the core runtime
  (`rg "reselect" ndn-service-framework/ServiceUser.cpp ServiceProvider.cpp`
  is empty): denial is terminal unless an explicit idempotency/dedup
  contract re-issues (replay tombstones are the only re-entry path, and
  they are rejected — `NDNSF_PROVIDER_REPLAY_REJECTED
  reason=duplicate-request-and-token` in the stream family).

## In-suite schedule-dependent flake (not a defect)

In one `Spec175InvocationStream` detailed run,
`NormalStreamCancellationFencesLaterCallbacks` aborted once
(`events.size() == 1` check, [2 != 1] — a late event racing the
cancellation fence).  Five consecutive isolated runs of the case passed
(exit 0) and the full family re-ran 19/19 green immediately after.
Classification: same pre-existing Spec175 stream timing sensitivity
recorded in earlier sessions; not a spec179 semantic change (the case
and stream machinery are untouched by the spec179 diff).

## Status per T009 wording

Every enforcement owner named by T009 executes an executed row above
(normal #1-#8/#14-#23, large #14-#15, Targeted #9-#11, stream
#11-#13), each with a same-version unaffected control where the target
scope is exercised (#1,#3,#5,#6,#19).  Component evidence does not
close the MiniNDN cross-process gate — that gate is T011.
