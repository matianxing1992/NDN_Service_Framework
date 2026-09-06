# Spec180 audit iteration 88 — publication/readback seam

Date: 2026-09-03

## Correction

`MiniNdnCaseRuntime.publish_and_verify_runtime_catalogue()` now owns the
publication barrier. It composes the candidate-bound
`ndnsf-di-presplit-catalog-snapshot-v1` payload, calls a
`ServiceUser`-compatible publisher, reads back the exact Data name with the
expected signer, compares the complete payload, and then records the digest via
`mark_catalogue_published()`. A publisher call without readback is rejected.

## Evidence

- CodeGraph index: up to date before the source edit.
- Structural Spec Kit audit: PASS.
- Spec180 contract gate: PASS (`contractReady=true`,
  `qualificationReady=false`, `issues=[]`).
- Focused runner regression: `tests/python/test_spec180_yolo_minindn.py -k
  runtime_catalogue_publisher` — 1 passed.
- Full focused collection: `tests/python/test_spec180_*.py` — 170 passed, 22
  existing warnings in 37.67 seconds.
- `git diff --check`: PASS.

## Remaining boundary

`run_minindn_case()` still fails closed before MiniNDN startup. The production
driver must instantiate the controller-node `ServiceUser`, publish every
candidate-bound role/rank object, invoke this seam, then start User and run the
real ACK→Selection→Provider→Response lifecycle. T011/T014 remain open and no
MiniNDN, SIF, or Tiger qualification result is promoted.

## Verdict

`CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED`.
