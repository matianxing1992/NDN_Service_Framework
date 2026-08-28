# Spec 142 Build And Test Gate

## Frozen canonical build

- Build directory:
  `build/spec142-svs-ndnsf-runtime-profile-r4`
- Binary SHA-256:
  `04e0e055afb5f548275e99d996598d8c6c2e6b99564de43157236f9a6006b8b1`
- Runtime NDN-SVS library SHA-256:
  `e977253ede5a7cdfe7a4b4be7d9213e1a468ee2632c2673216bdd493240983d4`
- Runtime linkage and source hashes:
  `build/spec142-svs-ndnsf-runtime-profile-r4/build-manifest.json`
- CPU affinity: logical CPUs 0--3.

## Executed tests

- NDN-SVS focused V3 extension handoff test: PASS (1/1).
- NDN-SVS full unit suite: PASS (75/75).
- Spec 142 Python contract/analyzer tests: PASS (11/11).
- Spec 140 runner regressions: PASS (7/7).
- Spec 141 runner regressions: PASS (5/5).
- r4 profile and 800 pps pacer preflight: PASS on four mode/peer probes.

## Campaign evidence

- Canonical campaign:
  `results/spec142-svs-ndnsf-runtime-profile/campaign-20260724T012559Z`
- Runtime profile manifest: PASS.
- 400 pps qualification verdict: FAIL.
- Both cells completed their requested offered load and emitted one terminal
  receipt, but both were `PROFILE_INVALID` because publication Fetch
  timeout/retry counters increased during the 60-second measurement window.
- The conditional 600/800 stage was not started.

## Preserved development failures

- r2 campaign `campaign-20260724T010804Z` exposed that V3 piggyback extensions
  were absent on receive despite send-side counters.
- r3 campaign `campaign-20260724T011721Z` verified the extension fix but also
  exposed that cumulative drain-end counters polluted the formal window gate.
- Neither campaign is pooled with the canonical r4 evidence.
