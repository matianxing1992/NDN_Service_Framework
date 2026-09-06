# Spec180 audit iteration 87 — runtime snapshot status boundary

Date: 2026-09-03

## Correction

`NetworkCatalogSnapshotResolver` now rejects any `PreSplitCatalogSnapshot`
whose status is not `ACTIVE`. `RETIRED` and `REVOKED` remain valid catalog
history states, but the signed `ndnsf-di-presplit-catalog-snapshot-v1` APP
envelope is the publication used for new request-scoped placement and must
contain only active snapshots.

## Evidence

- CodeGraph index: up to date before the source edit.
- Structural Spec Kit audit: PASS.
- Spec180 contract gate: PASS (`contractReady=true`,
  `qualificationReady=false`, `issues=[]`).
- Focused regression: `tests/python/test_spec180_catalog_resolver.py` — 4
  passed.
- Full focused collection: `tests/python/test_spec180_*.py` — 169 passed, 22
  existing warnings.
- `git diff --check`: PASS.

## Scope boundary

This is a fail-closed parser correction only. It does not publish candidate
role/rank artifacts, connect the live ACK→Selection→Provider→Response driver,
or provide MiniNDN, exact-SIF, or Tiger qualification evidence. T011 and the
mandatory T014 design-code convergence audit remain open; formal validation is
blocked by A180-132 until the real runtime publication and driver exist.

## Verdict

`CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED`.
