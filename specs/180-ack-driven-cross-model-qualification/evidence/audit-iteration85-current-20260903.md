# Spec180 audit iteration 85

Date: 2026-09-03  
Scope: current-source design/code reconciliation after the V3 artifact-boundary repair.

## Verified

- `codegraph status .`: index up to date (4,549 files; 139,044 nodes).
- `audit_speckit_structure.py ... --strict`: PASS; 25 functional requirements,
  9 success criteria, 4 user stories, and 20 traceable tasks.
- `scripts/spec180_contract_gate.py`: `status=PASS`,
  `contractReady=true`, `qualificationReady=false`, `issues=[]`.
- Spec180 runner/catalogue focused collection:
  `pytest -q tests/python/test_spec180_yolo_minindn.py
  tests/python/test_spec180_catalog_resolver.py` -> **47 passed**
  (43 runner tests and 4 resolver tests).
- Focused regression:
  `pytest -q tests/python/test_spec170_default_application_path.py
  tests/python/test_spec180_yolo_application.py
  tests/python/test_spec180_yolo_ack_planning.py` -> **20 passed**.
- Complete Spec180 Python collection: **169 passed, 22 existing warnings**.

## Repair

`AutomaticPlanningCoordinator._request_v3` now invokes the existing
`_prepare_artifacts` authority when the maintained caller supplies an explicit
catalog snapshot provider. A matching ACTIVE snapshot is checked for model,
graph, semantics, backend, precision, role-set, and rank coverage before the
sealed Selection is built. The regression uses a production-shaped explicit
snapshot and verifies exact role artifact resolution.

## Open blocking finding

The package candidate catalogue (`spec180-yolo-catalogue-v1`) and the runtime
active snapshot (`ndnsf-di-presplit-catalog-snapshot-v1`) are separate records.
The exporter currently emits candidate semantics but no runtime role/rank
`artifactDataNames`; the active snapshot publisher is not wired into
`run_minindn_case()`. The runner still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. T011 must publish or verify both
registered candidates' role/rank objects, publish the signed active snapshot
through `ServiceUser.publish_signed_app_data`, record its exact receipt, and
only then start User. Republish of package bytes, a legacy `/Stage` manifest,
or an offline snapshot is not sufficient.

## Verdict

**CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED.** No live
ACK/Selection/Provider/Response, MiniNDN, SIF, or Tiger result is promoted.
