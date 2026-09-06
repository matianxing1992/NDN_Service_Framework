# T043 — finite deployment interface closure

**Date**: 2026-09-02  
**Status**: PASS (implementation boundary; no remote mutation performed)

T043 now has one repository-owned G3 matrix driver,
`scripts/run_spec175_g3_matrix.py`, and one Tiger submission entry point,
`packaging/ndnsf-di-container/jobs/spec175/submit.sh`. The driver fixes the
M01–M14 case order, workload/fault seeds, three fresh repetitions, frozen
topology, admission-disabled contract, fresh output root, and final manifest
driver. It stops on the first nonzero child and never pools an existing output
tree.

The current Tiger profile registers exactly five deployment gates: `control`,
`stage-readiness`, `multi-provider`, `conversation-residency`, and
`performance`. The former standalone `unary-functional` row was removed from
the current route; generic unary compatibility is covered by the local unit
and native integration gates, avoiding a duplicate expensive campaign.

## Verification

The following focused checks passed:

```text
python3 -m pytest -q tests/python/test_spec175_g3_matrix.py \
  tests/python/test_spec175_tiger_profile.py \
  tests/python/test_spec175_tiger_checklist.py \
  tests/python/test_spec175_gate_prerequisites.py
19 passed

python3 -m pytest -q tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_tiger_checklist.py
22 passed

run_spec175_g3_matrix.py --dry-run
DRY_RUN 42 M01 M14
```

The mutation test for an existing output root rejects before creating a case
directory or manifest and preserves a sentinel file. Profile and checklist
tests reject missing, stale, unknown, or drifted inputs before `sbatch`.
`git diff --check` also passes.

This evidence closes only the deployment-interface task. It does not claim a
G3, SIF, or Tiger result. T020 must still create a fresh source seal and pass
the design/code, G0–G2, and then G3 gates before any expensive action.
