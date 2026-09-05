# Spec179 Traceability (2026-09-04 release baseline)

This is the implementation baseline as of the T013 release audit (2026-09-04).
The per-row executed cases live in `validation-matrix.md`
(`Executed-case mapping (2026-09-04, T010)` and the T011 MiniNDN campaign
tables); `evidence/release-gate.md` records the layered
implemented/executed/measured claims and the release verdict. Evidence cells
below name the artifacts that actually executed the behavior; a checked task
box is never inferred from this table.

| Requirement range | Design/contract | Implementation task | Acceptance/evidence |
|---|---|---|---|
| FR-001–FR-004 | Discovery boundary, certificate advertisements | T003 | US1, SC-001/006; `CertificatePublisher` unit suite; encryption-certificate advertisement cases (RV-U13) |
| FR-005–FR-009, FR-013 | Request bundle, named Input Data, Selection envelope, replay binding | T004 | US1/2, SC-002/003/004; `RequestScopedConfidentiality`, `GenericDynamicApi` selection/replay cases (RV-U08); MiniNDN `selection-response-tamper-and-replay` (14/14) |
| FR-010–FR-014, FR-022–FR-023 | Response AEAD, signer/recipient checks, segment nonce contract | T005–T006 | US2, SC-002/003/005/006; `RequestScopedResponseConfidentiality` 4/4 and `Spec175InvocationStream` 19/19; per-segment AEAD reference cases (RV-U13); `request-scoped-response-confidentiality.t.cpp` + `evidence/response-runtime-green-20260904.md` |
| FR-015–FR-018, FR-038 | Signed PolicyStatus, ControllerVersion authority, authorization attributes, withdrawal-driven global ABE rotation, and grant-only target-policy replacement with one lazy DKEY fetch/install | T007 | US4, SC-006/007/008/014/015/022; Controller-authority completeness matrix; RV-U20–RV-U21, RV-I15–RV-I19, RV-I27, RV-I30–RV-I31; `evidence/grant-only-single-issuance-20260904.md`, `evidence/grant-only-dkey-20260903.md` |
| FR-019–FR-021 | Refresh, expiry, cache invalidation, restart behavior | T008 | US4, SC-007/008/009/013; `ControllerRevocationState`/`ControllerVersionRefresh` suites; RV-U10/U11/U18/U19, RV-I11/RV-I24; MiniNDN `controller-unavailable-expiry` (7/7) |
| FR-024–FR-025 | Redacted telemetry and separation from permission Data | T001, T005, T009 | SC-002/010/012; `RevocationStateReportsRedactedTypedReasons`, `RequestCryptoFailureNamesAreStable` (RV-U12); redacted MiniNDN trace hashes |
| FR-026 | Explicit compatibility mode and removal owner | T011–T012 | SC-011/012; executed migration: switch/counters/carrier removed once the MiniNDN and streaming gates passed; owner and threshold in `AUDIT.md` R179-M4 (RESOLVED); default pinned by `RequestScopedDefaultActivationWithConfiguredController`; see also `tasks.md` T012 and this audit's network campaign |
| FR-027, FR-028, FR-029, FR-030, FR-031, FR-032, FR-033 | Compact message version, bounded self-refresh, restart persistence, writer lease, source-independent signed Data retrieval | T001, T003–T009, T011 | SC-013/014/015; RV-U13/U17, RV-I26, RV-I28; `ServiceControllerRejectsSecondWriterAndPreservesAuthority`, `GenerationPersistsAcrossRestartAndClockRollback`, MiniNDN `controller-cache-provider-status-retrieval` (14/14) |
| FR-034, FR-035, FR-036, FR-037 | Typed revocation scope, in-flight enforcement ownership, reauthorization, Controller-unavailable behavior | T007–T011 | RV-U05–RV-U12; Controller-authority completeness matrix; RV-I01–RV-I14, RV-I23, RV-I25–RV-I27; SC-016–SC-020; `RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint` |
| FR-002, FR-003, FR-006, FR-007, FR-008, FR-011, FR-012, FR-016, FR-017, FR-020 | Request fields, Input Data binding, envelope/AAD validation, certificate validity, ControllerVersion-scoped ABE, forward-only revocation | T001–T008 | SC-001–SC-009; executed by the RV-U/RV-I rows listed under FR-005–FR-018 and by the security-critical negative branch table (invalid certificates, wrong recipient, digest mismatch, stale/forged version, tamper, nonce reuse, replay) |
| SC-001–SC-006 | Cross-user/provider, tamper, replay, nonce, certificate matrix | T009–T010 | executed unit/component mapping + MiniNDN `selection-response-tamper-and-replay`; `evidence/regression-red-green-20260904.md` |
| SC-007–SC-009 | ControllerVersion change, revocation, refresh deadline | T008–T010 | RV-U06/U20/U21, RV-I18/RV-I27/RV-I30/RV-I31; MiniNDN grant-only and withdrawal scenarios |
| SC-010–SC-012 | Telemetry scan, regression, release trace | T009–T012 | SC-010: redacted typed reasons + no private key/plaintext in telemetry; SC-011: all suites green on the new default path with stale switch inert; SC-012: this traceability table, `validation-matrix.md`, `AUDIT.md`, `evidence/release-gate.md` |
| SC-013–SC-015 | Cross-message refresh, repeated restart/clock rollback, writer exclusion, Provider/cache status retrieval | T007–T009, T011, T013 | RV-U17, RV-I05/RV-I12/RV-I26/RV-I28/RV-I29; MiniNDN `controller-cache-provider-status-retrieval`, `offline-rejoin-epoch-skip`, `controller-restart` |
| SC-016–SC-022 | Complete revocation decision/state matrix, affected/unaffected controls, reauthorization, grant-only target-only lazy DKEY fetch/install, bounded unavailable-Controller behavior, hintless scheduled refresh | T007–T011, T013 | `validation-matrix.md` executed mapping; MiniNDN campaign 2026-09-04 (14/14 scenarios, `gatePassed=true`) — `evidence/minindn-campaign-20260904.md` |

## Current evidence status (2026-09-04)

All 13 tasks T001–T013 are complete. The deterministic Controller authority,
policy, cache, refresh, revocation-state, reauthorization, and fail-closed
behavior is executed by the unit suites `RequestScopedConfidentiality`,
`ControllerRevocationPolicy`, `ControllerRevocationState`, and
`GenericDynamicApi` (spec179 gate suites re-run green after the T012 removal
on 2026-09-04), and the runtime behavior by the integration suites
`ControllerRevocationFlow` (38/38), `ControllerVersionRefresh` (1/1),
`RequestScopedSelection` (3/3), `RequestScopedResponseConfidentiality` (4/4),
and `Spec175InvocationStream` (19/19). The cross-process half is executed by
the real MiniNDN campaign of 2026-09-04: 14 scenarios, every scenario
`gatePassed=true` with `networkEvidence=true` — User/Provider identity
revocation, `/PERMISSION/S` and `/SERVICE/S` service withdrawal with
unaffected and dual-role controls, retained-old-DKEY failure and filtered
new-DKEY recovery, in-flight revocation, offline epoch skipping,
Controller/Provider/cache status-source equivalence, Controller-unavailable
expiry, hintless scheduled revocation discovery, Controller restart, and one
scenario per large-response, Targeted/refill, and stream cache path, plus
wire-level tamper/replay and the grant-only advance. Per-scenario checks,
redacted trace hashes, and execution counts are retained in
`results/spec179-minindn/<scenario>/` and
`evidence/minindn-campaign-20260904.md`.

The migration half of the spec is closed: the request-scoped path is the
default and sole V2 protected mode for configured Controllers, and the old
service-wide response-key carrier, its compatibility switch, counters, and
mixed-mode rejection branch were removed (T012) after the MiniNDN and
streaming gates passed; non-request-scoped large responses fail closed with
typed errors. `AUDIT.md` R179-M4 records the owner (NDNSF maintainer) and the
met removal threshold.

Two documented non-goals remain outside the normative completion rule: (1)
the full monolithic unit target on this host shows one pre-existing,
out-of-scope DI codec SIGFPE (recorded, not spec179 code —
`evidence/regression-red-green-20260904.md`); (2) NAC-ABE internal cache
renewal, persistent runtime-cache restoration, and production live
User/Provider status installation under a file trust anchor remain runtime
extensions of the recorded evidence, not missing normative rows of this
matrix — every RV-U and RV-I row has an executed mapping.

## Evidence rules

- Unit tests establish deterministic serialization, cryptographic failure codes,
  canonical AAD, nonce behavior, Controller authority, typed revocation policy,
  cache invalidation, and every security-critical state transition.
- The integration suites execute real ServiceController/ServiceUser/
  ServiceProvider handlers and the LocalMock runtime boundary; they are not a
  network or trust-schema gate by themselves.
- MiniNDN establishes two-User/two-Provider cross-process behavior; a
  deterministic branch is closed only by its named unit/component case, and a
  network row is closed only by its named campaign scenario.
- A result is `implemented` only after code, `wired` only after an invocation
  uses it, and `measured` only after the named test/evidence artifact exists.
- `validation-matrix.md` is normative: aggregate line coverage cannot replace a
  missing RV-U/RV-I row or a missing fail-closed transition.
