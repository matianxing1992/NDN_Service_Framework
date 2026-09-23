# Spec179 Handoff — Codex/OpenAI provider restore

**Date**: 2026-09-04 (CDT)

## Active goal

`/goal wan chen spec 179 de suo you ren wu` — complete all tasks of
`specs/179-request-scoped-confidentiality` (request-scoped confidentiality and
epoch revocation). Goal is active; do not create a new goal.

## Task status (from `specs/179-request-scoped-confidentiality/tasks.md`)

- T001–T004 complete (wire/crypto foundation, discovery metadata, selected
  Provider input/key delivery).
- T005 partial: normal/large request-scoped Response paths have component
  coverage; full normal/large User runtime + old carrier removal remain.
- T006 partial: stream binding + selected LocalMock revocation boundaries
  execute; Targeted refill/fast path, event-key confidentiality, restart, and
  configured stream trust remain.
- T007 partial: exact `/PERMISSION` vs `/SERVICE` targets, durable
  ControllerVersion, withdrawal-driven global ABE rekey, grant-only
  target-only replacement, and DKEY-only fence are implemented; retained-old-DKEY
  cryptographic exclusion + complete issuance evidence remain.
- T008 partial: state-ledger, Targeted pools, in-flight cleanup, service-scoped
  non-ABE eviction, exact service-scoped comparison, atomic install, and
  generation-fenced NAC-ABE cache-clear are implemented; cross-process
  source-equivalence, persistent restart, and complete public-parameter/DKEY
  staging evidence remain.
- T009–T013 pending: distinct runtime paths, negative/telemetry gate,
  privileged MiniNDN evidence, migration removal, and final release audit.

## Authoritative reference

- Audit verdict: `specs/179-request-scoped-confidentiality/AUDIT.md` is BLOCK
  (evidence gate). 8 active high findings (R179-H0A/H0B/H0C/H7), 2 active
  medium (R179-M4 compatibility counter missing).
- Normative matrix: `specs/179-request-scoped-confidentiality/validation-matrix.md`.
- Evidence: `specs/179-request-scoped-confidentiality/evidence/` (most recent
  `regression-20260903.md`).

## Build/test baseline

- Canonical build dir: `build-spec179-exact4` (Clang/Boost 1.71, NAC-ABE prefix
  `/tmp/nac-abe-spec179-exact-prefix`). Config command is recorded in
  `evidence/regression-20260903.md`.
- Integration target built at `build-spec179-exact4/integration-tests`
  (2026-09-03 22:51). Full unit target was mid-build when the session was
  interrupted.
- Known non-Spec179 blockers (must be repaired/quarantined before a green full
  gate): `NdnsfDataV1SvsFlow/ProductionProviderContextUsesSvsSegments` (SVS
  publish-before-subscribe timing) and
  `Spec175InvocationStream/NormalStreamCancellationFencesLaterCallbacks`
  (schedule-sensitive), plus the unrelated unit FEC case
  `Stream/LiveStreamGf256RepairRecoversAnyTwoOpaqueSources`.

## Next steps (in dependency order)

1. Re-run the exact4 unit and integration targets and record a green/red
   baseline.
2. T007/T008: execute cryptographic retained-old-DKEY exclusion (fresh global
   ABE generation on withdrawal) and grant-only one-fetch/zero-fan-out issuance
   evidence.
3. T009: execute runtime revocation across normal/large/Targeted/stream paths
   with matched unaffected and dual-role controls.
4. T010: map every security-critical branch to an executed unit/component case.
5. T011: privileged MiniNDN campaign (large/Targeted/stream, grant-only,
   offline-rejoin, Controller-restart) with redacted traces in
   `tests/minindn/` and `evidence/`.
6. T012: migration (default path already on), add compatibility counter
   (R179-M4), update README/README_ch, remove old service-wide response-key
   carrier.
7. T013: final audit + `traceability.md` + `AUDIT.md` +
   `evidence/release-gate.md`.

## Method

Use the five-tool gate (Context Mode → CodeGraph → Spec Kit → GSD → ARS). In
this session Context Mode guard reported `Codex configuration is missing:
hooks=true`; after this restore it should pass again. CodeGraph index is up to date.

Suggested skills: `speckit-implement`, `speckit-audit`,
`codegraph-first`, `gsd-resume-work`.
