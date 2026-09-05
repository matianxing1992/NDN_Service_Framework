# Spec179 Audit — Controller Revocation Test Coverage

## 2026-09-05 NAC dependency compatibility follow-up (current; dependent validation pending)

The broader dependency review found and repaired warm-cache authorization
reuse, CK callback reentry crashes, stale parameter installation and exact-name
generation relabeling. NAC `b3b43c8` passes the full42-case/3299-assertion CTest
gate and all three example builds. Public class layouts changed, so NDNSF must
be rebuilt before runtime acceptance. T020 remains open for that gate and
representative MiniNDN verification. The report is
`evidence/nac-abe-compatibility-review-20260905.md`; prior16/16 evidence below
remains valid for its recorded NAC `8b462d0`, not for the new dependency.

## 2026-09-05 online authorization follow-up (current; local scope PASS)

**PASS for T017–T019 local online grant/revoke repair.** The authoritative
report is `evidence/online-authorization-audit-20260905.md`; earlier verdicts
below are historical. Controller retry/grant fencing and immutable recovery
epochs, asynchronous unprovisioned startup, initial-DKEY admission, callback
reporting and truthful MiniNDN evidence are repaired. NAC `8b462d0` additionally
fences delayed content/CK callbacks and normalizes OpenABE errors; real negative
tests and GDB reproduce the old failure path.

Final gates: NDNSF unit182/182, integration72/72; dependency14/14; launcher15/15;
complete MiniNDN16/16 with driver exit0. Source, clean working diff and29 artifact
hashes agree across every final manifest. Both grant probes retain all target
and control outcomes with zero failures; rekey recovery has14/14 checks and all
five processes exit0. Earlier failed/incomplete campaigns remain negative.
No wire/authority format changed. T014 upstream dependency publication remains
external; this is not an upstream release or TigerCluster qualification claim.

## 2026-09-05 runtime grant/revoke audit closure (historical; superseded above)

**Verdict: PASS** — the three findings of the report-only audit
(evidence/runtime-grant-revoke-audit-20260905.md) are fixed and re-verified:
Finding A (reverse-order grant-only DKEY refresh loss) is closed by an
equal-version+pending install that now issues the pending DKEY-only refresh
(User and Provider mirrors), R1 (revoke rotation failure) now records the
rotation as pending and reconciles it before the next revoke entry, and
Finding B's spec wording now states the App-driven ownership of grant
discovery.

- **A (MEDIUM) → fixed.** `installControllerStatus`'s accepted branch now
  calls the shared `refreshNacDkeyForControllerStatus` helper both on
  `versionChanged` (via `invalidateControllerScopedCaches`,
  ServiceUser.cpp:4211 / ServiceProvider.cpp:12214) and on an
  equal-version install that consumes a pending grant-only entry
  (ServiceUser.cpp:3922 / ServiceProvider.cpp:11988); helper body keeps the
  DKEY-only wave fence, LocalMock defer, and NOT_REQUIRED semantics
  (ServiceUser.cpp:4276 / ServiceProvider.cpp:12352). RV-I34
  (`GrantOnlyRefreshSurvivesReverseOrderStatusFirstInstall`,
  controller-revocation-flow.t.cpp:3254) drives status-first → permission-later
  and asserts exactly one target DKEY fetch (pre-fix zero is mechanism
  analysis — the equal-version install consumed the pending entry without
  refreshing — not a separately executed negative).
- **R1 (MEDIUM) → fixed.** `revoke()` sets `m_abeRotationPending` when
  `rotateAbeGenerationAndReissuePolicies()` throws and returns false while the
  revocation stays enforced (memory + every published status); the next
  `revoke()` entry first calls `reconcilePendingAbeRotation()`
  (ServiceController.cpp:442-453, gate :525) which retries the rotation with
  an idempotency fence on `m_aa.getPublicParametersVersion()` vs the target
  `generation*1000+epoch` (:415 test-only `NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE`
  injection). RV-U24 (`ControllerRevokeRotationFailureRecordsPendingAndReconciles`,
  controller-revocation-flow.t.cpp:3417) asserts fail-closed retention +
  params unchanged + successful reconcile on the next entry. Spec FR-017
  (spec.md) now states the fail-closed + pending + retry contract.
- **B (LOW) → fixed in spec.md.** SC-022/FR-038 and the grant-only normative
  block now state that grant discovery is App-driven — the runtime never polls
  permission records; an applied App-layer renewal (or explicit
  `fetchPermissionsFromController` refetch) arms the single target-only
  refresh, regardless of whether the signed status install preceded or
  followed the renewal.
- Re-verified on build `build-clang-spec179-rv32` (2026-09-05): RV-I34 +
  RV-U24 green; full spec179 gate suites green — integration
  `ControllerRevocationFlow` 42/42 (was 40/40), `ControllerVersionRefresh`,
  `RequestScopedSelection`, `RequestScopedResponseConfidentiality`,
  `Spec175InvocationStream`; unit `RequestScopedConfidentiality`,
  `ControllerRevocationPolicy`, `ControllerRevocationState`, `GenericDynamicApi`,
  `RuntimeStatusStorePersistence` — "No errors detected" on every suite.
  MiniNDN fix-closure runs (new `results/spec179-minindn-fixclosure-20260905/`,
  frozen 2026-09-04 campaign untouched): grant-only-advance
  `grantedEpochGE2Fetches==1` forward path intact + revocation regression
  scenarios green (details in the evidence-file closure addendum).
- Matrix rows RV-I34 and RV-U24 executed (validation-matrix.md);
  `docs/architecture.md` grant/revoke lifecycle updated in the same commit.

## 2026-09-05 runtime grant/revoke audit (report only; no fixes applied; historical — superseded by the closure above)

**Historical verdict: CONDITIONAL PASS** — runtime service-permission
grant/revoke is safe (fail-closed at every protected transition; revocation is
independent of the grant-only mechanism and remains fully covered). Two MEDIUM
and one LOW non-security findings recorded with no code changes; full report:
`evidence/runtime-grant-revoke-audit-20260905.md`.

- **A (MEDIUM, functional/spec, not security)** — grant-only DKEY-only
  refresh's pending consumption is not guarded by the same condition as its
  execution: when a grant-only status installs through the independent status
  channel *before* the permission response (reverse order), the later
  equal-version install consumes the pending entry
  (ServiceUser.cpp:3899-3902 / ServiceProvider.cpp:11966-11968, pre-fix
  numbering) while `invalidate` — the only consumer of
  `grantOnlyDkeyRefresh` — runs only on `versionChanged`
  (ServiceUser.cpp:3906-3911 / ServiceProvider.cpp:11972-11976, pre-fix
  numbering). The refresh is silently lost until the next real version
  advance; affected content fails closed (no over-authorization). Spec
  SC-022's "normally initiated after signed status installation"
  (spec.md:598) is not met in this order. Untested (all grant-only cases
  drive the forward order).
- **R1 (MEDIUM, carried from 2026-09-03/04)** — `revoke()` ABE-rotation
  failure leaves memory revocation + durably advanced version with no crypto
  rotation/retry/reconcile (ServiceController.cpp:501-511); untested.
- **B (LOW)** — grant discovery depends on the one-shot `fetchPermissions-
  FromController` retry window (no production auto-driver; App_User.cpp:816
  calls once at startup); grants arriving after the window need an explicit
  App-level refetch. SC-022 "explicit fetch as fallback" ownership should be
  stated in spec.md.
- Re-verified on the closing binary: unit RV-U20/U21/U23 and integration
  RV-I32/I33 gates remain green; MiniNDN grant-only-advance
  `grantedEpochGE2Fetches==1` confirms exactly one target-only DKEY fetch on
  the forward path (`run_request_scoped_confidentiality.py:1251-1265`).

## 2026-09-05 independent re-audit ("审核并修复"; supersedes document state, not verdict)

**Verdict: PASS** — confirms the 2026-09-04 release audit and fixes the
cross-document inconsistencies it left behind. Full report:
`evidence/post-implementation-audit.md`.

- Independently re-ran the gates on the current post-removal binary
  (2026-09-05): unit `RequestScopedConfidentiality,ControllerRevocationPolicy,
  ControllerRevocationState,GenericDynamicApi` exit 0; integration
  `ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,
  RequestScopedResponseConfidentiality` exit 0; `Spec175InvocationStream`
  exit 0 — 241 cases, "No errors detected" ×3
  (`/tmp/spec179-reaudit-gate.log`).
- CodeGraph facts re-verified: provider fail-closed
  `ServiceProvider.cpp:5136-5157`, user fail-closed + trust-schema segment
  validation `ServiceUser.cpp:7043-7346`, refresh authority fence
  `PolicyRefreshCoordinator.cpp:33-208`, durable Controller generation
  `ServiceController.cpp:248-344`. Removed-symbol grep clean.
- MiniNDN evidence: all 14 scenario `result.json` files
  `gatePassed=true`/`networkEvidence=true`; the stale
  `results/spec179-minindn/campaign-summary.tsv` was regenerated from the
  final results.
- Fixes applied: `tasks.md` T005–T008 checked `[x]` with closing-evidence
  sentences (R179-A1); supersession banners on the frozen 2026-09-02
  validation-matrix sections (R179-A2); the three residual items recorded as
  deferred non-goals with owner and reintroduction criteria in `spec.md`
  `## Out of Scope` (R179-A3); `release-gate.md` scenario-name alignment
  (R179-A5).
- **R179-A6 (HIGH → RESOLVED 2026-09-05): the Spec179 changeset is now
  committed** — spec/plan/tasks/matrix/AUDIT and the FR-039–FR-041 code,
  tests, and evidence landed on `UAV-Experimental` as cc18c930 (spec
  amendment), 98388f7f/d6016773 (T014 evidence/records), 91fd25b0 (RV-I33),
  71bbe311 (FR-039 store + restore path), 2325781b (RV-I32), plus this
  close-out commit (tasks.md flip, traceability status, AUDIT closure). The
  only follow-up that still needs a maintainer action is T014's upstream
  package/split hand-off and push (user-authorized only).
- **R179-A8 (closing regression, 2026-09-05):** full unit and integration
  runs on `build-clang-spec179-rv32` reproduce only the two recorded
  pre-existing, out-of-scope DI failures — the deterministic codec SIGFPE
  (`NativeTensorBundleCodecRoundTripsPilotDtypesDynamicShapesAndKvOutputs`)
  and the schedule-dependent `Spec170*` integration cases that rotate per
  run (D2a, NativePostSelection, NativeDeviceMismatch, D2b/D2h) —
  `evidence/regression-red-green-20260904.md`. Every spec179 gate suite is
  green: `RuntimeStatusStorePersistence` 6/6 and `ControllerRevocationFlow`
  40/40 on the closing binary; MiniNDN coverage remains the 2026-09-04
  campaign 14/14 (the FR-039 additions are opt-in and touch no network
  path exercised there).

## 2026-09-04 release audit (T013; supersedes the 2026-09-03 BLOCK below)

### Verdict

**PASS.** All 13 tasks (T001–T013) are complete and the four evidence layers
are separated in `evidence/release-gate.md`. Every normative RV-U01–RV-U21 and
RV-I01–RV-I31 row and every security-critical negative branch maps to an
executed green case (`validation-matrix.md` `## Executed-case mapping
(2026-09-04, T010)`), and the measured cross-process half is the real MiniNDN
campaign of 2026-09-04: 14/14 scenarios with `gatePassed=true` and
`networkEvidence=true` (`evidence/minindn-campaign-20260904.md`). The
2026-09-03 BLOCK's missing cryptographic and cross-process evidence has been
produced: retained-old-DKEY exclusion against new-generation ciphertext
(RV-U20 mixed-generation matrix, `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`),
filtered new-generation DKEY recovery for retained identities, separate
`/PERMISSION/S` and `/SERVICE/S` withdrawal with unaffected and dual-role
controls at the network layer, in-flight revocation, Controller-unavailable
expiry, offline epoch skipping, restart, cache/source equivalence, and one
scenario per large-response/Targeted/stream cache path.

Migration closure (T012/R179-M4): the request-scoped path is the default and
sole V2 protected mode on a configured Controller; the
`NDNSF_REQUEST_SCOPED_COMPATIBILITY` switch, its counters/getters, the
mixed-mode rejection branch, and the old service-wide response-key carrier
were removed after the MiniNDN and streaming gates passed. Non-request-scoped
large responses fail closed with typed errors
(`ServiceProvider::finishRequestExecutionOnEventLoop`,
`ServiceUser::resolveLargeResponseReferencePayload`); a stale env value is
inert (unit regression `RequestScopedDefaultActivationWithConfiguredController`).
The unit → integration gates were re-run green on the post-removal binary
today (4 spec179 unit suites; integration `ControllerRevocationFlow` 38/38 +
`ControllerVersionRefresh` 1/1 + `RequestScopedSelection` 3/3 +
`RequestScopedResponseConfidentiality` 4/4 + `Spec175InvocationStream` 19/19).
Fail-open/leakage audit: no plaintext fallback or resurrected service-wide
carrier exists; failures carry typed redacted reasons; no private key or
plaintext in logs/telemetry/traces.

### Finding status update (2026-09-04)

| ID | Severity | Status | Evidence |
|---|---|---|---|
| R179-H0 | HIGH | RESOLVED (2026-09-03) | exact-attribute validation and dual-role regressions retained |
| R179-H0A | HIGH | RESOLVED 2026-09-04 | RV-U20 mixed-generation decrypt matrix + filtered new-DKEY recovery executed (`ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`, `t.cpp:855-935`); grant-only single-issuance wire timeline (`evidence/grant-only-single-issuance-20260904.md`); network half in MiniNDN `grant-only-advance` + `targeted-refill-invalidation`/`stream-invalidation` denial windows |
| R179-H0B | HIGH | RESOLVED 2026-09-04 | MiniNDN campaign 14/14: User/Provider identity revocation, `service-scoped-revocation-with-unaffected-control` (11/11, dual-role + unaffected), `large-response-invalidation` (14/14), `targeted-refill-invalidation` (14/14), `stream-invalidation` (6/6), `in-flight-revocation` |
| R179-H0C | HIGH | RESOLVED 2026-09-04 | post-Selection retry/reselection remains disabled by default (no automatic reselection exists in the source); duplicate/conflicting Selection and terminal replay are rejected before a second execution (`FirstRespondingAckAfterProviderSelectedIsIgnored`, `SelectedProviderReceivesExactEncryptedInputOnly`, `ConcurrentDuplicateCommitsOneRecordAndOneProjection`); execution/key-disclosure boundaries are recorded as exactly-one terminal outcomes (RV-I08 mapping) |
| R179-M1 | MEDIUM | RESOLVED 2026-09-04 | every RV-U/RV-I row now has an executed mapping and the network half ran 14/14 scenarios (2026-09-02 wording "remaining ... scenarios still require separate privileged runs" is superseded by the campaign) |
| R179-M2–M5 | MEDIUM | RESOLVED (2026-09-02/09-04) | see rows above / R179-M4 row |

### Completion answer (retained from 2026-09-03)

Spec179 claims **forward-looking withdrawal** of service use/provision in
conforming runtimes: affected parties cannot obtain new attribute keys, start
new protected transitions, or use stale tokens/keys after authoritative status
acceptance. Historical plaintext, already-disclosed keys, work completed
before the enforcement boundary, and a deliberately modified party while every
enforcing peer remains offline remain outside the claim — documented, not
defects.

## 2026-09-03 service-use/service-provision revocation audit

### Verdict

**BLOCK (evidence gate).** (Superseded 2026-09-04 by the release audit above;
retained as the historical evidence-gate record.) The revised specification and source now implement
the intended scope-aware revocation and grant-only key lifecycle, but the
release claim remains blocked by missing cryptographic and cross-process
evidence. Two code facts control this verdict:

1. Existing NAC-ABE routing already supplies the semantic discriminator:
   Providers receive `/SERVICE/<service>` attributes for Request/Selection,
   while Users receive `/PERMISSION/<service>` attributes for ACK/Response.
   Revocation should therefore identify and rotate the exact affected
   attribute rather than introduce a second, parallel role vocabulary.
2. `RevocationTarget` and `AuthorizationSubject` now carry and validate the
   exact `/PERMISSION/<service>` or `/SERVICE/<service>` attribute, and the
   Controller filters each role independently. Withdrawal advances the durable
   ControllerVersion, rotates the global NAC-ABE master/public-parameter pair,
   and rebuilds filtered monolithic DKEY policies. A grant-only change advances
   ControllerVersion but keeps that pair unchanged and replaces only the
   granted identity's complete DKEY policy. The target-only refresh now uses a
   DKEY-only fence, preserving the old key until a complete replacement is
   installed and rejecting an overlapping stale fetch. These paths have
   focused component and build evidence; retained-old-DKEY cryptographic and
   MiniNDN cross-process evidence is still required before release.

The spec, data model, wire contract, plan, tasks, and validation matrix have
been corrected to use an exact `authorizationAttribute`. T007's scope and
ABE-generation paths are implemented; T007/T008 remain open until
cryptographic exclusion, issuance counts, and cross-process refresh are
measured. Spec179 completion requires both policy enforcement and cryptographic
exclusion:

```text
withdraw User use of S:
  advance ControllerVersion
  create fresh global ABE master/public parameters
  reissue filtered new-generation DKEYs to every retained identity
  omit /PERMISSION/S from the revoked User
  deny the affected User at Request/Selection and Provider execution checks

withdraw Provider provision of S:
  advance ControllerVersion
  create fresh global ABE master/public parameters
  reissue filtered new-generation DKEYs to every retained identity
  omit /SERVICE/S from the revoked Provider
  deny the affected Provider at ACK/Selection/execution/publication checks
```

The compact ControllerVersion in Request/ACK/Selection/Response remains only a
notification hint. A receiver fetches exact Controller-signed status, new
public parameters, and its new DKEY; it never trusts the peer's version as
authority. The reviewed NAC-ABE implementation initializes one global
master-secret/public-parameter pair in the Attribute Authority constructor and
does not expose attribute-local rekey. Spec179 therefore accepts global rekey
and system-wide retained-identity DKEY refresh as the correct initial design.

Grant and withdrawal are explicitly different transactions. A grant-only
change advances ControllerVersion but preserves the exact active ABE public-
parameter name/digest and master secret; only the granted identity's complete
policy is replaced, and one target-only DKEY fetch (normally initiated after
signed status installation) returns a replacement containing its complete
current attribute set. A DKEY-only fence rejects an overlapping stale fetch
and permits at most one coalesced follow-up while retaining the old key.
Existing DKEYs for unaffected identities remain usable and are not refetched.
The new grant is not enabled until that replacement is validated and installed.
Any transaction containing a
withdrawal follows the global-rekey path above. Reauthorization after a prior
withdrawal uses the current post-revocation generation and cannot reactivate a
retained pre-revocation DKEY. This contract is specified by FR-038, RV-U21, and
RV-I31, but remains implementation evidence pending.

### New findings

| ID | Severity | Dimension | Code/evidence fact | Required action |
|---|---|---|---|---|
| R179-H0 | RESOLVED | Authorization scope | `RevocationTarget`/`AuthorizationSubject` now carry the exact canonical attribute, and Controller permission builders filter `/PERMISSION/<service>` and `/SERVICE/<service>` independently. | Preserve exact-attribute validation and dual-role regression coverage. |
| R179-H0A | HIGH | Cryptographic revocation | The Controller now rotates the global NAC-ABE generation on withdrawal and replaces filtered monolithic DKEY policies; grant-only changes preserve the pair and target only the granted identity. Retained-old-DKEY decryption denial and cross-process issuance evidence are still unmeasured. | Run RV-U20/RV-U21 and the MiniNDN release gate; do not claim cryptographic completion from policy-map assertions alone. |
| R179-H0B | HIGH | Runtime enforcement | Existing evidence closes one normal User-identity MiniNDN path, not separate User `/PERMISSION` and Provider `/SERVICE` withdrawal through normal, large, Targeted, and stream paths. | Execute the revised RV-U05/RV-U14 and RV-I01/RV-I02/RV-I04/RV-I15/RV-I17/RV-I18 plus representative MiniNDN scenarios with matched unaffected and dual-role controls. |
| R179-H0C | HIGH | In-flight semantics | A missing Response after Selection cannot prove whether execution occurred; automatic reselection can duplicate non-idempotent work. | Keep post-Selection retry/reselection disabled by default; permit it only with application-declared idempotency/deduplication and record execution/key-disclosure boundaries. |

### Completion answer

After all revised tasks pass, Spec179 can correctly claim **forward-looking
withdrawal** of service use/provision in conforming runtimes: affected parties
cannot obtain new attribute keys, start new protected transitions, or use stale
tokens/keys after authoritative status acceptance. It still cannot revoke
historical plaintext, erase an old key already disclosed, stop work completed
before the enforcement boundary, or constrain a deliberately modified party
while every enforcing peer remains offline and stale.

## 2026-09-02 progress and plan audit revision (historical; superseded above)

**Historical verdict: BLOCK.** At this checkpoint the Controller authority and
persistence foundation appeared substantially complete and T007 was closed.
The 2026-09-03 audit above reopens T007 because the service target did not
distinguish `/PERMISSION` from `/SERVICE` and did not rotate the global ABE
generation. The two earlier
runtime correctness findings (process-wide comparison suppressing a
service-scoped refresh, and sequential authority/coordinator mutation) have
since been fixed: protected message call sites use the exact service-scoped
ControllerVersion, and `installControllerStatus()` stages both state machines
before one commit. The remaining block is evidence and lifecycle completeness,
not those two transaction defects: production trust-schema installation,
NAC-ABE cache renewal, persistent runtime-cache/restart behavior, mode-specific
paths, and the representative MiniNDN matrix are still open.

### Progress by task outcome

| Tasks | Status | Audit conclusion |
|---|---|---|
| T001–T004 | complete | wire/crypto foundation, discovery metadata, and selected-Provider input/key delivery have focused executed evidence |
| T005 | partial | normal and large request-scoped Response paths have strong component coverage; full normal/large User runtime and migration removal remain open |
| T006 | partial | stream binding and selected LocalMock revocation boundaries execute; Targeted refill/fast path, event-key confidentiality, restart, and configured stream trust remain open |
| T007 | partial | exact `/PERMISSION` versus `/SERVICE` targets, durable ControllerVersion changes, withdrawal-driven global ABE generation rotation, filtered monolithic DKEY policies, grant-only target-only replacement, and DKEY-only stale-fetch fencing are implemented and covered by focused component/build checks; retained-old-DKEY cryptographic exclusion and complete issuance evidence remain open |
| T008 | partial | state-ledger, Targeted pools, in-flight unary/stream cleanup, service-scoped non-ABE cache eviction, exact service-scoped message comparison, atomic combined installation, and framework calls to a generation-fenced NAC-ABE cache-clear API are implemented; cross-process source-equivalence, persistent restart behavior, and complete public-parameter/DKEY staging evidence remain open |
| T009–T013 | pending | distinct runtime paths, negative/telemetry gate, privileged MiniNDN evidence, migration removal, and final release audit remain |

### New findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| R179-H5 | RESOLVED | Security / distributed correctness | `ServiceUser.cpp:4097–4130`; `ServiceProvider.cpp:12168–12202` | Protected Request/ACK/Selection/Response paths now call the service-scoped overload; the process-wide accessor remains only as an explicit legacy/controller-free compatibility view. Restart and service-isolation tests prevent a stale service from borrowing another service's authority. | Keep RV-I29 as a regression and retain a two-service runtime assertion before removing this historical finding. |
| R179-H6 | RESOLVED | Security / atomicity | `ServiceUser.cpp:3735–3795`; `ServiceProvider.cpp:11839–11895` | `installControllerStatus()` validates private copies of `RevocationState` and `PolicyRefreshCoordinator`, then commits both maps together; rejected installs leave both views and caches unchanged. | Preserve RV-U19 regression coverage and retain the atomic staging pattern. |
| R179-H7 | HIGH | Revocation / cache ownership | `HybridMessageCrypto.*`; NAC-ABE Consumer/Producer caches; T008 | Hybrid send/receive/wrapped-key state has service-scoped invalidation, and User/Provider now clear NAC-ABE DKEY/CK caches through a generation-fenced `Consumer::clearCache()` plus `CacheProducer::clearCache()`. The patched NAC-ABE prefix builds and the focused 41-case runtime subset passes. The dependency patch is not yet an upstream/release pin, and cross-process old-key denial remains unmeasured. | Land or pin the dependency patch, require the explicit prefix/closure gate, then run a cross-process old-epoch decrypt/renewal denial with an unaffected service control. |
| R179-M3 | RESOLVED | Validation design | `spec.md`, `plan.md`, `tasks.md`, `validation-matrix.md` | The previous target × cut-point × mode × recovery Cartesian requirement repeated identical code paths and made the network gate impractically large without improving branch confidence. | The revised plan uses exhaustive unit decisions, risk/pairwise component coverage of every distinct owner/cache/terminal path, and representative MiniNDN cross-process scenarios. Any omitted combination must name an executed equivalent path. |
| R179-M4 | RESOLVED | Migration / observability | `ServiceUser.cpp`; FR-026/T012 | Compatibility counters were added and covered by unit tests, but the finding stayed open until the first-release removal owner/threshold was recorded and executed. Owner: NDNSF maintainer. Threshold: cross-user confidentiality and ControllerVersion-revocation gates pass in the first release using the request-scoped path — met 2026-09-04 when all 14 MiniNDN campaign scenarios (including the streaming gate) passed with `gatePassed=true`. T012 then removed the `NDNSF_REQUEST_SCOPED_COMPATIBILITY` switch, its counters/getters, the mixed-mode rejection branch, and the old service-wide response-key carrier (Provider `makeResponseWithLargeDataOptimization` and the User legacy large-response decrypt tail); non-request-scoped large responses now fail closed with a typed error, and `RequestScopedDefaultActivationWithConfiguredController` pins the post-migration default while asserting a stale `NDNSF_REQUEST_SCOPED_COMPATIBILITY=1` is inert. | Keep the default-activation regression and the fail-closed large-response error path; do not reintroduce a silent service-wide fallback. |
| R179-M5 | RESOLVED | Evidence integrity | current rebuilt unit executable | The service-scoped Hybrid cache change and its new unit test were initially outside the previously recorded executable counts. | The current unit target rebuilt successfully; `GenericDynamicApi/CryptoAndAuthorization` passes 16/16, including `HybridMessageCryptoInvalidatesOnlyAffectedService`. |

## 2026-09-02 runtime restart regression update

`ControllerRevocationFlow/RuntimeRestartDropsControllerStatusAndFailsClosed`
now covers the process boundary explicitly. A first User instance installs a
valid Controller status, then a newly constructed User instance fetches only a
stale permission snapshot; without a newly authenticated status it has no
service ControllerVersion or policy epoch and refuses to publish a protected
Request. The rebuilt integration executable passes this case with 7/7
assertions, including the redacted `status_lost=fail_closed` telemetry marker.
This closes the local restart-loss-of-authority regression, but not durable
User/Provider cache restoration, offline rejoin, or cross-process status-source
equivalence; those remain T008/T009/T011 release gates.

`ControllerRevocationFlow/NewerServiceStatusDoesNotAuthorizeOtherService`
adds the two-service runtime isolation check: service A advances to
`ControllerVersion{9400,2}` while service B remains at `{9400,1}`; a B request
carrying A's newer version is rejected without publication, while a B request
carrying B's exact version is still accepted. The current executable passes
11/11 assertions. This confirms the protected User path no longer borrows the
process-wide maximum as service-B authority; cross-process exact-fetch and
source-equivalence evidence remain open.

The revised test strategy is complete by independent behavior rather than by
raw combination count: unit tests exhaust deterministic authority branches;
component tests execute every production enforcement owner, cache family,
terminal owner, and recovery implementation with target-scope and unaffected
controls distributed across them; MiniNDN validates only cross-process
propagation, cache-source equivalence, loss/reordering, offline/rejoin, and
restart. This is sufficient only after every row names an executed test or an
explicit equivalent implementation path.

## 2026-09-02 configured trust-schema validation update

`ConfiguredTrustSchemaControlsControllerStatusValidation` now exercises the
production `MessageValidator` configured-schema path with a temporary file trust
anchor: a trusted Controller certificate is accepted and an otherwise valid
status signed by an untrusted certificate is rejected. The test bypasses the
local-PIB shortcut intentionally, so this is evidence for configured trust
validation rather than only a self-signature check. The current Clang/Boost
1.71 integration executable passes 34/34 Controller-revocation cases,
including this test. This closes the explicit configured-validator unit/
component slice; production User/Provider status installation under the same
schema, cross-process propagation, and MiniNDN execution remain open.

## 2026-09-02 Controller-withdrawal test coverage extension

The unit/component inventory was extended to cover two previously implicit
Controller branches. `AuthenticatedHintWithoutStatusRemainsFailClosedUntilExactFetch`
now proves that an authenticated ControllerVersion carried by a protected
message can start a bounded exact-name fetch but cannot install authority or
authorize a transition; an invalid exact result exhausts the configured retry
budget, while a later valid exact result is accepted. The refresh-state suite
therefore passes 11/11 current-source cases. The existing
`ServiceControllerPublishesStableTimestampedRevocationSnapshot` case now also
invokes the real exact-name policy-status Interest handler, verifies the
Controller signature, decodes the advertised version/validity/target, and
checks the current timestamp window. The current Controller-revocation
component suite passes 34/34.

These additions close the coordinator no-status branch and the Controller's
signed exact-status publication route. They do not prove that production
User/Provider validators install that status, nor do they cover every distinct
enforcement/cache/terminal/recovery implementation path; those remain explicit
release gates.

## 2026-09-02 cache-invalidation test extension

The unit case `RevocationStateInvalidatesOnlyAcceptedServiceFamilies` now
asserts that an accepted newer service status invalidates exactly the six
authorization-material families (`abe`, `message-key`, `targeted-token`,
`selection-binding`, `nonce`, and `replay`). It also feeds that status to a
`RevocationState` for a different service and verifies that the wrong-scope
status is rejected without invalidating that state's caches. The component
`ControllerStatusRefreshCoordinatesRevocationAndUnrelatedTraffic` case now
performs the same assertions after a real `ServiceController` revocation and
exact-version refresh. These tests strengthen the Controller withdrawal
boundary, but they do not claim persistent cache recovery or cross-process
propagation.

The unit case also verifies that receiving the identical immutable status again
is idempotent: it does not re-invalidate the six cache families. This guards
scheduled refresh and duplicate status Data from causing unnecessary key/token
eviction.

## 2026-09-02 runtime cache behavior extension

The refresh implementation now performs service-scoped runtime eviction after
an accepted newer ControllerVersion. `ServiceUser` removes the affected
Targeted token/control/stream-offer entries and clears request-scoped key,
binding, nonce, selection-assignment, and collaboration material. It keeps an
ordinary pending request alive until its existing deadline so a rejected
response still produces the public timeout terminal outcome. A streamed request
reports `Unauthorized` immediately before its consumer and pending state are
cleaned up. `ServiceProvider` removes affected Targeted token entries and
pending request/stream/collaboration state while leaving replay tombstones
bounded rather than treating them as reusable credentials.

The 27-case `GenericDynamicApi/TargetedInvocation` executable covers the
affected User/Provider pool controls, and the focused integration subset covers
the unary response-timeout and active-stream Unauthorized outcomes. These are
component/runtime-cache assertions. Service-scoped `HybridMessageCrypto`
send/receive/wrapped-key eviction is now implemented, and the rebuilt
`GenericDynamicApi/CryptoAndAuthorization` suite passes 16/16 including its
affected/unaffected-service cache case. NAC-ABE internal cache renewal, completed nonce/replay
lifecycle, persistent restart recovery, configured trust-schema refresh, and
cross-process MiniNDN propagation remain explicit release gates.

## 2026-09-02 test-plan extension

The Controller revocation inventory now includes seven additional executable
checks: the unit test `GlobalCertificateRevocationMatchesDigestAcrossIdentitiesAndServices`
for identity-independent certificate-digest scope, and the component test
`ServiceControllerRejectsStaleExactStatusAfterRevocation` for refusing cached
old or forged future exact status names after a policy change. The unit
additions also cover identity-bound certificate non-over-revocation and the
exact `validUntil` fail-closed boundary; the component additions cover
stale-status replay, Provider identity withdrawal/reauthorization, and an old
message-carried ControllerVersion rejected before a second Provider execution.
The component test `ServiceControllerRestoresEveryRevocationKindAfterRestart`
also verifies that identity-wide, global certificate-only, and service-scoped
targets survive a new Controller generation without losing their scope.
All were relinked and executed against the current tree on 2026-09-02; the focused
policy suite passes 31/31 and the Controller component suite passes 34/34.
The audit verdict remains blocked for production-runtime trust-schema,
cross-process, and network gates. The MiniNDN launcher contract and seven-node topology
are now checked in and pass three non-privileged contract tests. The C++ role
examples now expose deterministic scheduled Controller withdrawal, bounded
lifetime, and identity controls. The launcher now performs bounded NFD/route
setup, shared-PIB role startup, controlled revocation/restart, and redacted
log/CSV evidence collection. No network evidence is claimed because this host
could not satisfy the root prerequisite at the time of that historical note.
One privileged normal-mode run has since produced network evidence; the full
scenario matrix remains open.

**Date**: 2026-09-02
**Scope**: ControllerVersion, signed PolicyStatus/revocation state, durable
Controller generation, message version propagation, and the proposed unit /
integration / MiniNDN validation gates.

## Follow-up execution update

The earlier counts in this audit predate five explicit revocation checks. The current
source now passes 31/31 Controller-policy unit cases (including the
identity-independent certificate digest scope) and 34/34 Controller-revocation
component cases (including stale/forged exact-status refusal, all-target-kind
restart restoration, and a stable
timestamped revocation snapshot). The 3/3 request-scoped selection component
cases also pass with the wrong-Provider signer rejection. These additions do
not change the audit verdict: production-runtime trust-schema installation,
cross-process recovery, and the representative MiniNDN network gate remain open.

## 2026-09-02 MiniNDN evidence and harness correction

The first privileged run exposed two launcher evidence defects rather than an
NDNSF protocol failure: Provider was given `--provider-lifecycle-csv` without
`--benchmark`, and the release-gate predicate referenced a local `selected`
variable outside `_collect_runtime_evidence`. Both are fixed in
`tests/minindn/run_request_scoped_confidentiality.py`; its four-case contract
suite now covers the lifecycle-state parser and the normal dry-run contracts.

After the fix, the same seven-node `user-identity-revocation` normal-mode run
completed with real cross-process evidence:

```text
gatePassed=true, networkEvidence=true
validated Request=156, Selection=76, Response=31
Provider executions=16 (unique provider/request EXECUTION_DONE rows)
Controller revocation applied=true, generation=1788384620539, epoch=2
terminal owner=/example/hello/provider/B, terminal reason=response_callback
```

The redacted result and trace hash are recorded in
`evidence/minindn-user-identity-revocation-20260902.md`. This changes the
MiniNDN status from “not executed” to “one representative normal path
measured”, but it does not close T011: certificate-only/service-scoped
unaffected controls, restart/offline recovery, large-response, Targeted,
streaming, and source-equivalence scenarios remain unrun. The gate therefore
remains **BLOCK** for Spec179 completion.

## 2026-09-02 historical verdict (superseded by 2026-09-03)

**BLOCKED for Spec179 completion; the deterministic foundation is sound but the
runtime revocation feature is only partially wired.** The current tests prove
the state-machine/persistence slice and directly exercise Controller mutation,
status filtering, signed status publication, and recipient-bound permission
issuance. The component slice also pairs identity-wide and certificate-only
withdrawal across all six modeled cut points, with replacement and unrelated
controls. The permission-handler case now proves certificate-only withdrawal
returns an empty snapshot for the affected Provider while a same-service
unaffected Provider still receives its grant. User/Provider source paths now
consult the installed revocation state
at selected Request/ACK/Selection/Response boundaries. New component tests
also drive real LocalMock User/Provider objects through normal Request
publication and Provider dispatch before and after identity withdrawal. The
live relay case now proves signed permission/PolicyStatus installation,
exact-version refresh after empty revoked renewals, User publication denial,
and Provider execution denial; production-runtime trust-schema installation,
full runtime
certificate-only/unaffected enforcement, and the complete Targeted/stream
lifecycle remain open.

## 2026-09-02 historical findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| R179-H1 | HIGH | Code reality / validation | `ServiceUser.*`, `ServiceProvider.*`; `validation-matrix.md:RV-I01–RV-I14, RV-I23` | The Controller permission handlers have direct component coverage for immutable version-addressable, recipient-bound encrypted snapshots. The live relay case drives signed permission and PolicyStatus Data through the runtime Face path, proves exact-version refresh after empty User/Provider renewals, and proves User publication plus Provider execution denial. The permission-handler case also proves certificate-only empty renewal and a same-service unaffected Provider grant. The separate configured-schema test now rejects an untrusted status signer, but the live relay still uses the test validator and does not cover full runtime certificate-only/unaffected enforcement. | Add production-schema validation to the live User/Provider path and retain the MiniNDN gate. |
| R179-H2 | HIGH | Security / distributed correctness | `ServiceUser.*`, `ServiceProvider.*`, `RevocationState.*`; `validation-matrix.md:31–40` | User/Provider now consult `RevocationState` at selected normal Request/ACK/Selection/Response boundaries. Request-scoped large responses now use per-segment AEAD and the configured trust-schema validator, but authenticated status installation and the complete Targeted, segmented-stream, and offline/rejoin paths are not executed. | Wire the state and authenticated refresh into every enforcement owner/cut point, then execute RV-I01–RV-I14 through real component handlers and MiniNDN before claiming revocation convergence. |
| R179-H3 | RESOLVED | Persistence / recovery | `ControllerGenerationStore.*`, `ServiceController::m_revocations`; `GenerationPersistsRevocationsAcrossRestart`, `ServiceControllerRestoresRevocationsAfterRestart` | Revocation targets are serialized in the same atomically replaced generation record and restored before a new Controller generation is published. | Keep restart/rejoin tests as release regressions; live multi-process recovery remains part of the MiniNDN gate. |
| R179-H4 | HIGH | Security / code reality | `ServiceUser.cpp`; `ServiceProvider.cpp`; `controller-revocation-flow.t.cpp`; `controller-revocation-state.t.cpp` | The configured-Controller no-status bypass is now closed: User and Provider fail closed before any authenticated non-zero service status is installed, and configured runtimes no longer adopt permission/manifest version hints as authority. The remaining issue is that exact signed-status retrieval and validation still need execution across every protected message type. A hint therefore cannot authorize traffic by itself, but the runtime refresh path is not yet complete. | Keep the no-status regression as a release gate. Finish/coalesce bounded exact-name status retrieval, validate the Controller-signed result before authority adoption, and add Request/ACK/Selection/Response plus Targeted/stream cases; never let a hint alone authorize traffic. |
| R179-M1 | MEDIUM | Evidence integrity | `validation-matrix.md`, `quickstart.md` | The current focused suites close deterministic authority logic and selected component paths, including Controller target/cut-point decisions, malformed-target rejection, persistence/restart, exact signed status publication, cache-ledger invalidation, Targeted/stream boundary checks, and configured trust-anchor acceptance. One normal-mode MiniNDN scenario is now measured, but the remaining distinct production paths and cross-process matrix are still open. | Keep incomplete RV-I rows pending, map each open row to a distinct implementation path, and finish production-schema User/Provider installation, certificate/unaffected enforcement, mode-specific caches, and representative MiniNDN scenarios. |
| R179-M2 | RESOLVED | Authentication | `MessageValidator::validateWithConfiguredTrustSchema`; `ConfiguredTrustSchemaControlsControllerStatusValidation` | A trusted Controller status is accepted through a configured file trust anchor and an otherwise valid status signed by an untrusted certificate is rejected; the test intentionally bypasses the local-PIB shortcut. | Keep this as a regression and add the same configured validator to live User/Provider status-installation coverage. |

## 2026-09-02 current test rerun

After adding the cache-scope assertions, the current Clang/Boost-1.71 build
passes 11/11 `ControllerRevocationState` cases, 34/34
`ControllerRevocationFlow` cases, and 1/1 `ControllerVersionRefresh` case
(35/35 when the two component suites are combined).
The complete unit target currently executes 682 cases; one unrelated existing
FEC regression, `Stream/LiveStreamGf256RepairRecoversAnyTwoOpaqueSources` at
`tests/unit-tests/stream.t.cpp:1925`, fails consistently in this environment.
The Controller/Targeted focused unit cases still pass, so this failure is not
evidence against the revocation changes, but it prevents claiming a green full
unit target. The Spec179-focused integration
subset (`ControllerRevocationFlow`, `ControllerVersionRefresh`,
`RequestScopedSelection`, `RequestScopedResponseConfidentiality`, and
`Spec175InvocationStream`) passes 58/58. The complete 121-case integration
binary still has one unrelated pre-existing failure in
`NdnsfDataV1SvsFlow/ProductionProviderContextUsesSvsSegments` at
`tests/integration-tests/ndnsf-data-v1-svs-flow.t.cpp:434`; this does not
invalidate the passing Controller/response/stream subset, but it prevents a
claim that the entire integration target is green.

The added unit and component cases are therefore useful and necessary, but not
sufficient for the full feature. They cover the Controller's local mutation,
version, target-shape, status-scope, fail-closed predicate logic, configured
trust-anchor acceptance/rejection, configured no-status runtime rejection, the
coordinator rule that a higher hint does
not change local authority before exact status validation, one live
signed-status/empty-renewal denial path, and the Controller permission-handler
certificate-only/unaffected renewal controls. They do not establish configured
production-runtime trust-schema status retrieval, full runtime certificate-only or
unaffected enforcement, network propagation, persisted runtime-cache recovery,
or enforcement at every lifecycle cut point.

## Executed evidence

- `tests/unit-tests/controller-revocation-policy.t.cpp`: the current-source
  isolated C++17/ndn-cxx executable, relinked with matching framework objects,
  passes 31/31 cases using the repository's Boost 1.71 toolchain, including
  `PolicyStatusWirePreservesValidityTimestamps`,
  `BoundCertificateRevocationDoesNotOverRevokeAnotherIdentity`, and
  `RevocationStateRejectsAtStatusValidityEnd`. This includes the explicit target-kind × cut-point matrix
  with unaffected and replacement controls.
- `tests/unit-tests/controller-revocation-state.t.cpp`: 11/11 current-source
  focused cases passed, covering unauthenticated-hint rejection,
  authenticated-hint/no-status exact-fetch gating, highest-candidate
  coalescing, hintless pre-expiry scheduling, bounded invalid-status retry,
  stale/conflicting equal-version rejection, and service/missing-status
  fail-closed behavior.
- `tests/unit-tests/request-scoped-confidentiality.t.cpp`: 13/13 focused crypto
  cases passed in a rebuilt current-tree executable, including the protected-
  message-container and certificate-advertisement round-trips. The executed set includes golden wire
  digests, RSA-OAEP recipient binding, AES-GCM/AAD and nonce checks,
  malformed/tampered inputs, expiry, and zeroization.
- `tests/integration-tests/controller-revocation-flow.t.cpp`: the current-source
integration executable, relinked by the current 62-task Clang Waf build,
passes 34/34 cases. This includes the repeated exact-status publication
  and stale-message execution assertions added in this revision.
  The suite exercises
  paired affected/unaffected state decisions, identity-wide and certificate-only
  withdrawal across all six cut points, replacement certificates, restart/expiry
  behavior, status-source equivalence, a real
  `ServiceController` mutation/state check, a real Controller status
  wire round-trip, direct policy-status handler refusal for missing service and
  corrupt generation, immutable version-addressable recipient-bound encrypted permission
  issuance with wrong-recipient rejection, identity-wide/certificate-only
  zero-grant denial snapshots, wrong-signer/content-tamper status negatives,
  configured file trust-anchor acceptance and untrusted-signer rejection,
  ParametersSha256Digest normalization, restart/status monotonicity with
  old-generation exact-name refusal, corrupt-generation fail-closed
  behavior, and the real LocalMock User/Provider normal Request
  publication/execution revocation boundary plus certificate-only/service-scoped
  withdrawal and newer-version reauthorization, plus rejection of an old
  message-carried ControllerVersion without a second Provider execution.
  The added Controller-boundary cases
  (`ServiceControllerRevocationRollsBackAfterWriterLoss`,
  `ServiceControllerRevocationRollsBackWhenStateCannotBeRead`,
  `RealControllerStatusDrivesUserAndProviderRevocation`, and
  `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint`, plus
  `LiveControllerStatusRefreshRejectsRevokedRenewal`) are included in the
  current 34/34 run, including the repeated exact-status publication and
  stale-message execution assertions.
- `tests/integration-tests/controller-version-refresh.t.cpp`: 1/1 case passed
  in the same configured integration executable. It composes a real
  Controller-produced PolicyStatus with `PolicyRefreshCoordinator` and
  `RevocationState`, covering hintless pre-expiry scheduling, single-flight
  coalescing, wire round-trip, higher-version revocation installation, and a
  matched unaffected authorization control.
- `tests/integration-tests/request-scoped-response-confidentiality.t.cpp`: 1/1
  dedicated response gate passed after the current integration relink. It
  fetched a retained request-scoped Response segment through the Provider IMS,
  validated the Provider signature using the configured trust schema, checked
  the exact version/AAD binding and unique nonce, and rejected ciphertext,
  Provider-certificate, and ControllerVersion mutations. This complements the
  full reconstruction flow; it does not close production-runtime trust
  installation or the MiniNDN matrix.
- `audit_speckit_structure.py ... --strict`: PASS (37 functional requirements,
  21 success criteria, 13 tasks, all 37 requirements traced).
- `git diff --check`: PASS.
- A prior `./waf build --target=integration-tests -j2` completed in the
  Clang/Boost-1.71 build directory. The bounded relink after the latest source
  change re-entered 62 tasks and did not produce a newer executable. Earlier
  GCC-9 compiler/assembler failures in unrelated UAV/DI translation units
  remain a separate full-target toolchain limitation.
- `./build-clang-nodbg/integration-tests --run_test=ControllerRevocationFlow --log_level=message`:
  34/34 PASS in the current executable, including
  `ConfiguredControllerFailsClosedBeforeStatusInstallation` and the real
  Controller target-kind × six-cut-point matrix.
- `/lib64/ld-linux-x86-64.so.2 ./build/integration-tests --run_test=ControllerVersionRefresh --log_level=message`: 1/1 PASS.
- `/tmp/spec179-controller-policy-current-20260902 --run_test=ControllerRevocationPolicy`:
  31/31 PASS in a current-tree focused unit executable, including the
  identity-independent certificate-digest scope, invalid writer/start-input,
  redacted typed-reason, and validity-timestamp regressions.
- `./build-clang-nodbg/unit-tests --run_test=ControllerRevocationState`:
  11/11 PASS in the current-tree focused unit executable, including accepted
  status cache-family invalidation, duplicate-status idempotence, and
  cross-service scope rejection.
- `/tmp/spec179-current-request-crypto --run_test=RequestScopedConfidentiality`:
  13/13 PASS in a current-tree focused crypto executable.

The full unit target remains a separate gate because unrelated large DI
translation units have triggered GCC-9 compiler/assembler and final-link
relocation failures. The current-tree attempt compiled all 117 units but
failed while linking `Stream.cpp.1.o` with `bad reloc symbol index`; this is a
toolchain/build-artifact failure, not a Controller revocation assertion. The
focused integration result is not treated as proof that the complete
revocation matrix or the full Waf integration target passes.

The Controller-policy source was also rebuilt as a bounded focused executable
from the current tree (the policy test plus its Controller/message objects,
with system Boost 1.71 and ndn-cxx libraries). It passes 31/31, and `ldd`
resolves its Boost dependencies to 1.71.0. This is current-source unit
evidence, but it is intentionally not presented as a successful monolithic
`unit-tests` Waf build.

## Coverage conclusion

Covered by the current slice:

- non-zero ControllerVersion ordering and canonical encoding;
- every pre-attribute RevocationTarget shape and malformed/unknown target rejection (the revised `/PERMISSION` versus `/SERVICE` shapes remain pending);
- fail-closed behavior for missing status, incomplete subjects, wrong service,
  and expired signed status;
- real Controller authority mutation: epoch advancement, duplicate/invalid
  target rejection, identity/certificate/service scope, per-service status
  filtering, and revoked User/Provider permission-snapshot filtering;
- canonical Controller-produced PolicyStatus wire round-trip with all target
  kinds and service-scope filtering;
- typed identity, certificate-only, and service-scoped revocation decisions;
- all six protected transition cut points as modeled decisions for
  identity-wide, certificate-only, and service-scoped withdrawal;
- affected/unaffected controls, replacement certificate, cache-family
  invalidation, and exactly-one terminal ledger outcome;
- durable generation monotonicity across restart/clock rollback, corruption
  fail-closed behavior, repeated starts, and single-writer/lost-lease fencing;
- atomic persistence and restoration of Controller revocation targets across
  Controller restart;
- immutable version-addressable recipient-bound encrypted User/Provider permission snapshots,
  wrong-recipient decryption rejection, identity-wide/certificate-only
  zero-grant renewal-denial snapshots, and signed-status wrong-signer/content-
  tamper negatives at the real Controller handlers;
- optional ControllerVersion fields on Request/ACK/Selection/Response and the
  protected-message-container round-trip in the current focused unit suite;
  signature/AAD tamper detection and live message enforcement remain pending.
- deterministic state-model rejection for missing status is covered, but the
  configured-Controller User/Provider no-status rejection is now exercised by
  `ConfiguredControllerFailsClosedBeforeStatusInstallation`; the remaining
  message-hint-only gap is exact signed-status retrieval and enforcement in the
  live User/Provider paths.

Still required before claiming complete revocation coverage:

- production-runtime trust-schema verification and certificate-only/unaffected
  renewal denial controls;
- full runtime enforcement in Request/ACK/Selection/Response handlers,
  including authenticated status installation and issuance denial beyond the
  executed normal User/Provider path;
- cryptographic ABE, RSA recipient wrapping, AES-GCM AAD/nonce, replay, and
  key-disclosure tests;
- persistent User/Provider cache and restart recovery;
- Targeted refill, segmented stream, and telemetry paths; the normal
  request-scoped large-response implementation now has a relinked component
  execution result, but revocation enforcement for that mode remains open;
- source-independent status retrieval, offline/rejoin, bounded/coalesced
  refresh, scheduled pre-expiry refresh, and Controller-unavailable behavior;
- exact-version hint refresh before enforcement across every message type (the
  configured no-status rejection and one live exact-fetch path are covered),
  repeated exact status publication across Controller restart, and the
  certificate-only/unaffected live renewal controls (RV-U16–RV-U17 and
  RV-I25–RV-I28);
- MiniNDN two-User/two-Provider network scenarios with matched unaffected
  controls and redacted trace evidence. One normal user-identity-revocation
  scenario is now measured; certificate-only/service-scoped controls,
  restart/offline recovery, large-response, Targeted, streaming, and source
  equivalence scenarios remain unrun.

## 2026-09-02 post-fix focused rerun

After the service-scoped comparison, atomic-install, and two-service isolation
changes, the rebuilt Clang/Boost-1.71 integration executable passes the four
Spec179 suites
(`ControllerRevocationFlow`, `ControllerVersionRefresh`,
`RequestScopedSelection`, and `RequestScopedResponseConfidentiality`) with
41/41 cases and 840/840 assertions. The new
`RuntimeRestartDropsControllerStatusAndFailsClosed` case contributes 7/7
assertions: a fresh User process does not treat a stale permission snapshot as
Controller authority and refuses to publish a protected Request. This is a
local/component regression result; it does not close persistent cache
restoration, production trust-schema installation, or the MiniNDN matrix.

The repository Context Mode installation also passes `context-mode doctor`,
CodeGraph is enabled, and GSD health is healthy. A privileged normal-mode
MiniNDN rerun is recorded in
`evidence/minindn-user-identity-revocation-20260902-priv-rerun.md` with
`gatePassed=true`, validated Request/Selection/Response publications, 16
Provider executions, and all five role processes returning zero. This closes
only the normal User-identity network row; the mode/recovery matrix remains
open. The only host-level Context Mode blocker is the active Codex app-server
holding a corrupt `logs_2.sqlite`; the guarded recovery helper refuses to move
it until the full VS Code/Codex client is closed. No database was deleted or
modified during this audit.

## Readiness scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Controller withdrawal is explicitly separated from receiver-side enforcement. |
| Architecture and ownership | Partial | Authority state is centralized and selected normal-path checks are wired; full lifecycle ownership is not yet closed. |
| Security/correctness | Partial | Deterministic fail-closed predicates pass; live signature, issuance, and message checks remain. |
| Task executability | Yes | New test entry points and RV-U/RV-I rows identify behavior and acceptance. |
| Task cohesion/granularity | Yes | Controller unit/component authority tests are grouped by independently reviewable behavior. |
| Validation/evidence | No | The current Clang/Boost-1.71 full unit target passes 683/683 cases and 62118/62118 assertions. The four Spec179-focused integration suites pass 41/41 cases and 840/840 assertions, including service-scoped version isolation and restart fail-closed behavior; the focused stream unit and `Spec175InvocationStream` regression remain 16/16 and 19/19. Direct recipient-bound issuance, signature negatives, configured file trust-anchor acceptance, ParametersSha256Digest normalization, scheduled/coalesced refresh bookkeeping, authenticated-hint/no-status exact-fetch gating, no-status runtime rejection, hint non-adoption, exact current-version status-name/refusal checks, terminal/expiry boundaries, identity-bound certificate scope, Provider identity denial/reauthorization, normal LocalMock runtime enforcement, malformed-target rejection without epoch mutation, historical permission-snapshot preservation, authenticated nonce reservation after tamper, atomic multi-segment nonce reservation, redacted typed revocation reasons, the real Controller target-kind × six-cut-point matrix, durable-read rollback, timestamped snapshot stability, exact-name signed status publication, all-target-kind restart restoration, restart/status monotonicity, global certificate-only digest scope, one live signed-status/empty-renewal denial path, stale-status replay rejection, cache invalidation/idempotence and cross-service rejection, inline-response ciphertext rejection before delivery, cross-user Response-name rejection, wrong-Provider transport-evidence rejection, stale Response-version rejection, response-time revocation after Provider execution, Provider-side Targeted execution denial/reauthorization, and Targeted stream discovery denial are covered. Full integration remains non-green because two pre-existing SVS/Spec170 timing cases fail independently; production-runtime trust-schema installation, certificate/unaffected live renewal, full lifecycle, stream restart/rejoin/Targeted provider enforcement, and the representative MiniNDN matrix remain pending. |
| Migration/rollback | No | Revocation persistence and mixed-version recovery still require an explicit implementation gate. |
| Code reality | Partial | Current code matches the deterministic slice and selected normal-path gates; it does not yet match the full runtime claims. |

## Metrics

Historical 2026-09-02 snapshot; the 2026-09-04 release audit at the head of
this file supersedes the completion and finding counts below.

- User stories: 4
- Functional requirements: 38 (37 listed in the 2026-09-02 snapshot; FR-038 added by the grant-only audit)
- Success criteria: 22 (21 in the 2026-09-02 snapshot; SC-022 added by the grant-only audit)
- Tasks: 13 (13/13 complete as of the 2026-09-04 release audit; T007 was reopened by the 2026-09-03 authorization-attribute and global-ABE-rekey audit and closed by its executed evidence)
- Requirement coverage: 38/38 structurally traced (see `traceability.md`)
- Executed Controller revocation tests: 31 Controller-policy unit + 11
  refresh-coordinator unit + 34 revocation component + 1 refresh component;
  stream wire/lifecycle regressions add 16 focused unit and 19 integration cases
  (2026-09-02 snapshot); the 2026-09-04 full unit run is 686 cases with one
  recorded pre-existing out-of-scope DI codec SIGFPE, and the spec179 gate
  suites were re-run green after the T012 removal.
- Normative revocation rows: RV-U01–RV-U21 and RV-I01–RV-I31 — every row has
  an executed mapping (2026-09-04), including the MiniNDN network halves.
- Findings (2026-09-04): 0 critical, 0 active high, 0 active medium, 0 low;
  R179-H0/H0A/H0B/H0C and R179-M1–M5 RESOLVED (the active counts in the
  2026-09-02 snapshot are historical).

The normative completion rule remains: every RV-U and RV-I row must execute at
its declared layer, and every affected transition must have a corresponding
unaffected control. Line coverage alone is insufficient.
