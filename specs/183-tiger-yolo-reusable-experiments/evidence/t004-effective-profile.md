# T004 effective dispatch profile consistency

Date: 2026-09-07. Scope: renderer, profile validation, prepare and provisioning
components; no native/SIF/GPU qualification.

The renderer previously wrote effective-profile.json before refreshing profile
file rows. A changed harness or validation contract therefore left an old identity
in the snapshot even after the plane's file hashes were internally valid.
The checker verified the snapshot's hash but did not compare its behavior with
the actual profile. That could certify different declared execution settings.

The shared `runtime.yolo_profile.effective_profile_document` now supplies both
run-plan behavior and the dispatch snapshot. Renderer seals the harness, updates
non-release file rows, writes the snapshot and plane, then updates excluded
release rows. Dispatch checking requires complete semantic snapshot equality
after byte/harness validation; stale fields, extra fields and changed settings
raise `EFFECTIVE_PROFILE_BINDING`. Physical file paths and new gate references
remain excluded from behavior identity as required for cross-host reuse.

Verification:

```bash
python3 -m pytest Experiments/TigerCluster/tests/test_yolo_effective_profile.py \
  Experiments/TigerCluster/tests/test_yolo_submit.py \
  Experiments/TigerCluster/tests/test_yolo_provision_profile.py \
  Experiments/TigerCluster/tests/test_yolo_operator_profile.py -q --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-effective-profile-20260907/focused-r2.xml
```

**79 passed in 19.82s.** The renderer test uses real small content planes, hashes,
frozen production scripts and validators with source paths redirected to fixtures.
It checks that the first render validates, and an unchanged second render produces
identical dispatch/profile bytes with no row updates. No model executes.
Initial invocation stopped during collection because the new test imported the
runtime package before initializing the fixture's Tiger path; corrected the
import order. The initial JUnit remains `focused.xml`; it is not a passing run.

This closes the snapshot-order finding recorded in t004-gate-reuse.md. T004/T007
remain incomplete pending remote staging/run and full production audit. Component
passes do not qualify the installed native libraries or the actual GPU case.

Actual profile was rendered once after final source/contract edits. Public CLI
`check --stage dispatch --profile Experiments/TigerCluster/profiles/yolo-two-node.json`
then exited 78 as designed: integrity VERIFIED, status INCOMPLETE, qualification
NOT_EVALUATED. Full result is retained at
`Experiments/TigerCluster/results/spec183-effective-profile-20260907/profile-check.json`.
E is `sha256:0520ccc23b6bbc6f2c6af3770f3c9eec4dc3672d5413336bbee860ec74eb7e20`;
profile digest is `sha256:6715d9ef9319a72a6cf41f31f2e6def1206a108496a39be073a69b9f4a789166`.
Inputs and runtime IDs were unchanged. No image rebuild was performed.
