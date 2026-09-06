# T020 — current-candidate local qualification

**Date**: 2026-09-02  
**Status**: PASS for G0–G2; G3 remains the next gate

The current candidate is bound to source seal
`results/spec175/current-20260902/g0/source-seal.json` and the current
Experimental NDN-SVS checkout. The production Data-V1 fetch path and the
linked SVS surface agree on `subscribeToProducerWithCatchUp`; the stale test
that rejected this current API was corrected and now requires the same symbol.
The configure probe in `wscript` and the focused production-path regression
therefore use one header/library ABI pair.

## Fresh gate results

| Gate | Result | Evidence |
|---|---|---|
| design/code and contract G0 | PASS, 0 blockers | `g0/qualification-manifest.json` |
| Python contract G1 | PASS, 374 passed, 1 registered skip, 0 failed | `g1/qualification-manifest.json` |
| native integration G2 | PASS, 38 registered results, no missing cases | `g2/qualification-manifest.json` |
| native unit suite | PASS, 605 cases, `*** No errors detected` | command output; no qualification manifest required |

The Python skip is an explicitly optional environment-dependent check reported
by the gate; it is not a missing required test or a model-compute fallback.
All required Spec175 contract, profile, MiniNDN-layout, native-oracle, state,
security, and production-wiring checks ran. The full unit suite included the
previously long `PredictiveConsumerValidatesAndDeliversReorderedData` path and
completed successfully.

No SIF was built, uploaded, staged, or submitted during T020. The next allowed
action is the unchanged `scripts/run_spec175_g3_matrix.py` command from a new
output root. A G3 failure must invalidate this candidate and restart at its
owning local layer; it must not be bypassed by a SIF or Tiger smoke.
