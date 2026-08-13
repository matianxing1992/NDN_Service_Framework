# Spec170 foundation signing failure — 2026-08-13

This is a retained negative build record. It is not an installable or frozen
Spec170 candidate.

- Workflow run: `31689367215`
- Source revision: `757606306018e0caf6a422c8263e8cfc12411648`
- Image reference: `ghcr.io/matianxing1992/ndnsf-di-spec170-foundation`
- Built image digest: `sha256:d17035529d447ed05d2f19d19679879915237a8fa9e0bd195d9eeec9942ae8b6`
- Build result: native foundation compilation and GHCR push succeeded
- Failure step: `Sign and verify the foundation digest`
- Failure: runner cosign returned `unknown flag: --bundle` for `cosign sign`
- Candidate status: `INVALID_CANDIDATE` (no signature verification or immutable
  manifest was produced)

The retry uses a new source identity and pins the cosign installer to the
known v2.4.3 release. The unsigned digest above remains retained only to
explain why it cannot be used for SIF materialization or TigerCluster gates.
