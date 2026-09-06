# Spec180 Audit Evidence — Iteration 84

**Date:** 2026-09-03  
**Subject:** current-source design/code reconciliation  
**Verdict:** conditional pass for implementation; formal validation blocked

## Checks

- `audit_speckit_structure.py ... --strict`: PASS (25 functional requirements,
  9 success criteria, 20 tasks, complete traceability).
- `scripts/spec180_contract_gate.py`: PASS for the document/trust-root
  contract; `qualificationReady=false`.
- Focused runner tests: **42 passed**.
- Complete `tests/python/test_spec180_*.py` collection: **167 passed, 22
  existing warnings**.
- Python syntax compilation for the runner and focused test module: PASS.

## Corrected discrepancies

1. The catalogue APP Data name now follows the native ServiceUser publisher
   rule: `/example/controller/NDNSF/DI/catalogue/v1`, signed by
   `/example/controller`. Free-standing `/spec180/catalogue/v1` names are
   rejected before MiniNDN startup.
2. The case adapter now binds the declared identity map before keychain
   initialization; the undefined identity lookup was removed.
3. Missing `runtime.identities.providerPrefix` now returns the documented
   fail-closed `RunnerError` instead of leaking `KeyError`.
4. Startup-phase launch is now failure-atomic: a child-launch error rolls back
   only the children added by that phase through the bounded process-group
   cleanup helper.

## Remaining gate

`run_minindn_case()` still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. No live catalogue publication,
ACK/Selection/Provider/Response exchange, MiniNDN qualification, exact-SIF
replay, or Tiger result is claimed. T011 must wire the staged runtime and T014
must produce a fresh design-code convergence `PASS` before T015–T020.
