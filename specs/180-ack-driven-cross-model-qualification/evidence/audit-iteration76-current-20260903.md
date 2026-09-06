# Spec180 Audit Evidence — Iteration 76

Date: 2026-09-03

## Scope

Current-source audit of the Spec180 inventory, terminal result validator,
dispatcher/release boundary, and registered YOLO/Qwen entrypoints. This is an
implementation checkpoint, not formal qualification.

## Findings and corrections

- The local inventory previously duplicated seed `1750001` for both Q-C and
  Q-W. The frozen manifest is authoritative: Q-C uses source case M01/seed
  `175021`; Q-W uses M11/seed `175022`. `scripts/spec180_inventory.py` now
  derives both tuples from `qwen-reference-manifest-v1.json`.
- `scripts/validate_spec180_results.py` previously validated oracle shape and
  file digests but not the semantic fixed-gate assertions. It now requires two
  requests and terminal responses, `cuda-onnxruntime`, three unique device
  identities, `cpuFallback=false`, unique child IDs, zero secret findings, and
  complete child/output cleanup.
- `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` still fails closed at
  `run_minindn_case()` with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`.
- QWEN-F remains a separate external Qwen3.6-27B ONNX entrypoint and fails
  closed before dispatch until T019 creates that entrypoint.
- `RoleAssemblySpec` still has a source-compatible `plaintext-v1` default. That
  default is inadmissible for Spec180 qualification; T007/T010 must reject it
  and require a protected authorization epoch before the convergence gate.

## Verification

```text
audit_speckit_structure.py --strict: PASS
spec180_contract_gate.py: PASS (contractReady=true, qualificationReady=false)
tests/python/test_spec180_release_workflow.py: 9 passed
tests/python/test_spec180_inventory.py + test_spec180_local_gate.py: 14 passed
tests/python/test_spec180_*.py: 143 passed, 22 warnings
py_compile (changed validator/inventory/tests): PASS
```

## Evidence boundary

The focused tests prove fail-closed boundary behavior only. No live NFD/
NDN-SVS ACK-to-Selection-to-Provider-to-Response execution, MiniNDN
qualification, exact-SIF replay, or Tiger YOLO/Qwen result is present. T011,
T014, and all later qualification tasks remain open.
