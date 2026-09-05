# Spec179 Release Gate — 2026-09-04

Binary: `build-clang-spec179-nac3` (Clang 10, exact Spec179 NAC-ABE prefix
`/tmp/nac-abe-spec179-exact-prefix`, debug, `--with-tests --with-examples`).
The four layers below follow the audit principles: **proposed** (spec/plan),
**implemented** (source + build), **executed** (tests ran), **measured**
(reproducible network campaign).

## Gate re-run after the T012 carrier removal (unit → integration order)

The T012 removal changed no gated behavior (see the removal argument in
`tasks.md` T012), and the gates were re-run on the post-removal binary in the
required order before this gate was written:

| Layer | Run on 2026-09-04 (post-removal binary) | Result |
|---|---|---|
| Unit | `unit-tests --run_test='RequestScopedConfidentiality,ControllerRevocationPolicy,ControllerRevocationState,GenericDynamicApi'` | exit 0, no errors |
| Unit (focused) | `RequestScopedDefaultActivationWithConfiguredController`, `RequestScopedLargeResponseKeepsSmallPayloadInline`, `RequestScopedLargeResponseUsesPerSegmentAeadReference`, `ProviderResolveLargeDataReferenceLeavesInlinePayload`, `LargeDataOptimization*` (5 cases) | 5/5 green |
| Integration | `integration-tests --run_test='ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection'` | 42/42 cases, no errors |
| Integration | `RequestScopedResponseConfidentiality` | 4/4 cases, no errors |
| Integration (stream regression) | `Spec175InvocationStream` | 19/19 cases, no errors |

Logs: `/tmp/spec179-t013-unit-gate.log`, `/tmp/spec179-t013-integration-gate.log`,
`/tmp/spec179-t013-stream-gate.log`.

## Implemented (source, build exit 0)

All 38 FRs and 22 SCs of `spec.md` are traced in `traceability.md`. The
request-scoped confidentiality path is the default and sole V2 protected mode
on a configured Controller: per-invocation `K_input`/`K_response` AEAD with
request-bound AAD, one `K_response` per segmented/streaming invocation with
unique per-segment nonces, ControllerVersion-signed authority with durable
fencing, withdrawal-driven global ABE rotation and grant-only DKEY-only
replacement, bounded refresh coordination, and fail-closed revocation
enforcement at every protected transition. The old service-wide response-key
carrier, the `NDNSF_REQUEST_SCOPED_COMPATIBILITY` switch, its counters and
mixed-mode rejection are removed; non-request-scoped large responses fail
closed with typed errors (T012). No workload-specific or UAV branches exist in
the Core files changed by this spec.

## Executed (tests ran green, evidence retained)

- Unit suites `RequestScopedConfidentiality`, `ControllerRevocationPolicy`,
  `ControllerRevocationState`, `GenericDynamicApi` — spec179 gate suites green
  after removal; the 2026-09-04 full unit run (686 cases, one recorded
  pre-existing out-of-scope DI codec SIGFPE) is in
  `evidence/regression-red-green-20260904.md`.
- Integration families `ControllerRevocationFlow` 38/38,
  `ControllerVersionRefresh` 1/1, `RequestScopedSelection` 3/3,
  `RequestScopedResponseConfidentiality` 4/4, `Spec175InvocationStream` 19/19 —
  green after removal (logs above) and on 2026-09-04 detailed runs recorded in
  `validation-matrix.md` (RV-U/RV-I executed-case mapping).
- Every RV-U01–RV-U21 and RV-I01–RV-I31 row, and every security-critical
  negative branch of the T010 wording, maps to at least one executed green
  case (see `validation-matrix.md` `## Executed-case mapping (2026-09-04,
  T010)`). No normative row of this matrix is left without an executed
  mapping.

## Measured (reproducible MiniNDN campaign)

`tests/minindn/run_request_scoped_confidentiality.py` +
`spec179_scenario_checks.py`, real NFD/MiniNDN with role binaries, shared-PIB
real signing/encryption, redacted evidence, run 2026-09-04: **14/14
scenarios, every scenario `gatePassed=true` with `networkEvidence=true`** —
`user-identity-revocation`, `provider-identity-revocation`,
`service-scoped-revocation-with-unaffected-control` (11/11),
`in-flight-revocation`, `offline-rejoin-epoch-skip` (8/8),
`controller-cache-provider-status-retrieval` (14/14),
`controller-unavailable-expiry` (7/7), `hintless-scheduled-refresh` (6/6),
`controller-restart` (15/15), `large-response-invalidation` (14/14),
`targeted-refill-invalidation` (14/14), `stream-invalidation` (6/6),
`selection-response-tamper-and-replay` (14/14), `grant-only-advance`
(`grantOnlyGateOk=true`, 41 target-only refresh attempts). Per-scenario
checks, redacted trace hashes, execution counts, invalidated-cache counts,
and terminal owners are in `results/spec179-minindn/<scenario>/` and
`evidence/minindn-campaign-20260904.md`. The campaign exposed and the reruns
fixed two real admission defects (S9 versionless Targeted requests, S10
discarded stream admission result), both validated by rebuild + genuine rerun.

## Unrun / out-of-scope claims (recorded, not gated)

- The monolithic all-project unit target on this host shows one pre-existing
  out-of-scope DI codec SIGFPE (spec179 code is not implicated; recorded in
  `regression-red-green-20260904.md`). The spec179 gate suites run green.
- NAC-ABE internal cache renewal, persistent runtime-cache restoration across
  process restart, and production live User/Provider status installation under
  a configured file trust anchor remain runtime extensions beyond the executed
  rows; they are recorded as deferred non-goals with owner (NDNSF maintainer)
  and reintroduction criteria in `spec.md` `## Out of Scope` (2026-09-04), not
  missing normative matrix rows. Every normative RV-U/RV-I row has an executed
  mapping.
- Deterministic malformed/tamper branches are executed at unit/component
  layer; the network layer executes representative scenarios only, per the
  matrix design.

## Fail-open and leakage audit (T013)

- Provider large-response handling without request-scoped invocation state
  fails closed with a typed error (ServiceProvider.cpp
  `finishRequestExecutionOnEventLoop`); no plaintext fallback and no
  resurrected service-wide ABE carrier exists.
- User large-response reference resolution outside the authenticated
  request-scoped key scope fails closed (ServiceUser.cpp
  `resolveLargeResponseReferencePayload`); no service-wide wrapped-key
  consumption remains.
- No residual reference to the removed `makeResponseWithLargeDataOptimization`
  or to the removed compatibility getters/counters exists in source or tests
  (build and `rg` clean).
- Request-scoped inline and segmented responses are AEAD-authenticated before
  application delivery; revocation/refresh/decryption failures carry typed
  redacted reasons (`RevocationStateReportsRedactedTypedReasons`,
  `RequestCryptoFailureNamesAreStable`); no private key or plaintext appears
  in logs, telemetry, or the redacted MiniNDN traces.

## Verdict

**PASS.** Confidentiality (cross-user request-scoped) and ControllerVersion
revocation are not marked complete before their normative matrix rows
executed: every RV-U and RV-I row now has an executed mapping, the 14-scenario
MiniNDN campaign ran green, and the T012 migration removal re-ran the unit →
integration gates green on the post-removal binary. No CRITICAL/HIGH finding
is open against this spec. Residual items are the documented non-goals and
the out-of-scope DI SIGFPE listed above.
