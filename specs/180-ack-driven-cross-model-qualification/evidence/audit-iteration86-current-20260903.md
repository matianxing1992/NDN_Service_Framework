# Spec180 audit iteration 86 evidence

Date: 2026-09-03

## Parser-boundary correction

`NetworkCatalogSnapshotResolver` now rejects duplicate `candidateDigest`
records in the signed `ndnsf-di-presplit-catalog-snapshot-v1` envelope. The
runtime contract permits at most one ACTIVE snapshot for each registered
candidate; aliases and manifest digests are not sufficient uniqueness because
placement resolves by candidate digest.

The regression constructs two records with different aliases and manifest
digests but the same candidate digest and verifies fail-closed parsing.

## Verification

- `codegraph status .`: PASS; index up to date.
- `python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/180-ack-driven-cross-model-qualification --strict`: PASS.
- `python3 scripts/spec180_contract_gate.py`: PASS; `qualificationReady=false`, `issues=[]`.
- `pytest -q tests/python/test_spec180_catalog_resolver.py`: 4 passed.
- `pytest -q tests/python/test_spec180_*.py`: 169 passed, 22 warnings in 36.85s.

This is a parser and focused-regression correction only. No live catalogue
publication, ACK/Selection/Provider/Response execution, T014 convergence PASS,
MiniNDN qualification, SIF replay, or Tiger result is claimed.

## Verdict

`CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED` remains the
accurate gate. A180-133 is resolved; A180-132 and the live T011/T014 boundary
remain controlling.
