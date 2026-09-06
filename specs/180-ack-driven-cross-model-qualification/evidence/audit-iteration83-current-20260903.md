# Spec180 Audit Iteration 83 Evidence

**Date**: 2026-09-03  
**Subject**: staged MiniNDN process startup and readiness barrier  
**Candidate status**: focused source evidence only; no qualification result

## Change under audit

`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` now labels each
`CaseProcessSpec` with one of three startup phases:

```text
control -> readiness -> providers -> readiness -> catalogue publication -> user
```

`MiniNdnCaseRuntime.start_processes()` accepts only one explicit phase, rejects
unknown, repeated, or out-of-order phases, resolves every phase node before
launching its first child, and retains child handles/log paths. The verified
catalogue publication receipt must be recorded before the User phase.
`wait_for_ready()` requires each declared marker and fails on early child exit,
unreadable logs, or timeout.
Non-package inputs in the process vector are now required to be readable files;
the canonical package remains a readable directory.

## Verification

| Check | Result |
|---|---|
| `pytest -q tests/python/test_spec180_yolo_minindn.py` | **38 passed** |
| `pytest -q tests/python/test_spec180_*.py` | **163 passed, 22 existing warnings** |
| `python3 -m py_compile Experiments/NDNSF_DI_YoloAckDriven_Minindn.py tests/python/test_spec180_yolo_minindn.py` | **PASS** |
| `git diff --check` | **PASS** |
| `python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/180-ack-driven-cross-model-qualification --strict` | **PASS** |
| `python3 scripts/spec180_contract_gate.py` | **PASS; qualificationReady=false** |

## Boundary

The production entrypoint still returns
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. No child process, NFD/SVS exchange,
catalogue publication, encrypted input publication, ACK/Selection/Response
trace, MiniNDN case, SIF replay, or Tiger job is claimed. T011 remains partial
and T014 convergence remains mandatory before T015 or any expensive validation.
