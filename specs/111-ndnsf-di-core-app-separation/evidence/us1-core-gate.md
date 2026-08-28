# US1 Core Gate

Date: 2026-07-14  
Verdict: **PASS**

Commands and outcomes:

- `./waf build -j4`: PASS, 310 build actions, 2m02.767s after the attempt-name correction.
- `./build/unit-tests --run_test=Spec111NativePlanSessionAndAttemptDefaultsAreCharacterized,ExecutionAttemptEpochScopesDependencyNamesAndMetadata --log_level=test_suite`: 2/2 PASS.
- Core contract/eligibility/execution/recovery/state tests: 15/15 PASS.
- Architecture and Core boundary import tests: 9/9 PASS.
- Lease/distributed consistency/recovery/fencing/orphan tests: 30/30 PASS.
- Runtime v1 compatibility tests after canonical Core telemetry migration: 26/26 PASS.
- Authenticated-transport transaction regression: 10/10 PASS, including fail-closed missing signer evidence.
- Positive MiniNDN authenticated lease/certificate/dependency run: PASS at `results/spec111-us1-core-4d695ce8b7ff-3a6bbdf23a87d112-base`.

The previous one-shot pre-separation canary remains the frozen characterization result and was not rerun. Failed or incomplete MiniNDN candidates remain preserved under their original result paths.
