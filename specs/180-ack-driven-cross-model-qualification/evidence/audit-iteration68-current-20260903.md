# Spec180 iteration-68 audit reconciliation

Date: 2026-09-03

This checkpoint corrects the boundary between startup authorization and
request-time placement. It does not claim a live MiniNDN, SIF, or Tiger result.

## Finding resolved

The candidate config's `service.providers[*].roles` entries are authorized
capability advertisements. They are not Provider assignments. A role may be
advertised by several Providers and one Provider may advertise several roles;
the final one-to-one role assignment remains owned by the authenticated closed
ACK snapshot and sealed plan. The runner now validates role coverage and node
mapping only and reports `roleCapabilities` in its preflight descriptor.

## Verification

```text
PYTHONPATH='NDNSF-DistributedInference:pythonWrapper' \
  python3 -m pytest -q \
  tests/python/test_spec180_yolo_minindn.py \
  tests/python/test_spec180_local_gate.py \
  tests/python/test_spec180_inventory.py \
  tests/python/test_spec180_contract_gate.py
37 passed in 1.56s

python3 -m py_compile Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
runner_py_compile=PASS

python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/180-ack-driven-cross-model-qualification --strict
Structural verdict: PASS
25 functional requirements; 9 success criteria; 20 tasks; 25 traced requirements

python3 scripts/spec180_contract_gate.py \
  --feature-dir specs/180-ack-driven-cross-model-qualification
status=PASS contractReady=true qualificationReady=false

spec180_tests=$(rg --files tests/python | rg 'test_spec180_.*\\.py$' | sort | tr '\\n' ' ')
PYTHONPATH='NDNSF-DistributedInference:pythonWrapper' \
  python3 -m pytest -q $spec180_tests
122 passed, 19 warnings in 31.14s
```

The warnings are existing exporter/runtime diagnostics and are not live
NFD/NDN-SVS evidence. `run_minindn_case` remains fail-closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; T011 and the mandatory T014 convergence
audit remain open.
