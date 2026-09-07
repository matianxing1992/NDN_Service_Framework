# T004 Structural Profile Checkpoint

Date: 2026-09-06
Status: PARTIAL / runtime qualification NOT_RUN

Implemented `schemas/tiger-yolo-v1.schema.json` and
`runtime/yolo_profile.py::load_operator_profile`. The loader is read-only and
uses the existing local JSON Schema library, imported lazily. Content-plane
checking and node workers do not acquire a JSON Schema runtime dependency.
`requirements-operator.txt` records the observed eight operator packages and
Python-version conditional dependencies; no packages were installed and no SIF
library/wheel lock was changed.

One missing-loader tracer test failed with AttributeError, then passed after
implementation. The final 25 profile cases cover unknown fields, types and
integer-only values, fixed ACK/roles/env, insufficient walltime, invalid progress
budget, relative-cwd independence, invalid namespaces, symlinks, duplicate JSON
keys, remote path traversal, missing stage manifests and fingerprint changes.
The structural-only test forbids subprocess launch, directory creation and
content-chain verification. It explicitly accepts an as-yet absent referenced
artifact but returns NOT_EVALUATED, not a release or gate PASS.

```text
python3 -m pytest -q Experiments/TigerCluster/tests --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-t004-profile-r2/junit.xml
```

Exit 0: **200 passed in 6.61s**. This includes 25 new profile tests, the prior 171
role/integrity/lifecycle tests and four existing runtime-template tests. The
JUnit file is ignored evidence, not a committed generated artifact. The earlier
r1 (199 passed in 6.38s) predates the added profile-filename control-character
regression; it is retained separately, not overwritten.

This closes only the structural slice: T004 stays unchecked. No actual enabled
profile, frozen bundle, five-command CLI, shared submission journal, genuine gate
receipt verification, or production command-boundary rejection test is complete.
No build, SIF creation, upload, Slurm allocation, or model execution occurred.
Next: wire the unique entrypoint and frozen effective-run plan, then the
allocate-once/reconciliation boundary and T005/T006 application/verdict consumers.
