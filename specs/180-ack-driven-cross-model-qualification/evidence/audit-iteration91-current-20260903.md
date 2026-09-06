# Spec180 Current Evidence — Audit Iteration 91

**Date**: 2026-09-03  
**Scope**: runtime-catalogue integrity checks and exact signed APP readback boundary  
**Verdict**: implementation evidence only; formal validation remains blocked

## Changes checked

- `build_runtime_catalogue_payload()` now requires exactly one snapshot for
  each candidate registered by the selected case.
- Unexpected candidate digests, missing/extra role keys, empty role artifact
  lists, duplicate artifact Data names, and non-absolute artifact names fail
  closed before publication.
- `publish_and_verify_runtime_catalogue()` now requires a non-empty signer
  certificate rooted at the configured controller signer, in addition to
  exact-name, signer-prefix, and byte-for-byte payload checks.
- The contract now identifies the controller-side orchestration as the
  publication owner and states that the resolver is not a publication
  substitute.

## Focused and bounded checks

Commands were run from the repository root with
`PYTHONPATH=NDNSF-DistributedInference` where required:

```text
python3 -m py_compile Experiments/NDNSF_DI_YoloAckDriven_Minindn.py tests/python/test_spec180_yolo_minindn.py
python3 -m pytest -q tests/python/test_spec180_yolo_minindn.py
47 passed in 0.84s
```

The four bounded Spec180 groups passed:

```text
group 1: 39 passed in 0.69s
group 2: 45 passed in 1.90s
group 3: 29 passed, 16 warnings in 25.79s
group 4: 60 passed, 4 warnings in 17.44s
total: 173 passed; 20 existing exporter/runtime warnings
```

Repository gates also passed:

```text
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/180-ack-driven-cross-model-qualification --strict
success_criteria=9, user_stories=4, tasks=20, tasks_complete=3, traced_requirements=25
python3 scripts/spec180_contract_gate.py
contractReady=true, qualificationReady=false, issues=[]
git diff --check
PASS
```

## Remaining blockers

`run_minindn_case()` still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. No real Controller/Repo/Provider/User
publication, ACK closure, Selection, Provider execution, Response, or
candidate-bound artifact publication has been executed. Therefore T011 is
partial and T014 convergence has not run; T015 local qualification and all
SIF/Tiger gates remain blocked. The next implementation boundary is the
controller-owned repo-backed artifact publisher plus the live ACK-driven
driver, followed by a fresh T014 audit before any expensive validation.
