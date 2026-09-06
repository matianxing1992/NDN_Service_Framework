# Spec180 audit iteration 75 — QWEN-F dispatch correction

Date: 2026-09-03

Verdict: CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED

## Finding and correction

The prior dispatcher contract routed `qwen-functional` to the Spec175 M11
streaming wrapper while labeling the gate `QWEN-F`. That would allow the tiny
CPU/MiniNDN fixture to be mistaken for the external Qwen3.6-27B ONNX Tiger
workload. It also duplicated model-manifest/root path bindings in the
workload environment even though `run-functional.sh` owns fixed `/inputs` and
`/models` mounts.

The current contract now reserves
`Experiments/NDNSF_DI_QwenAckDriven_Minindn.py` with the fixed argument
`--case QWEN-F`. The entrypoint is intentionally absent until T019 wires the
real external ONNX runner, so both the in-image dispatcher and host release
validator fail closed before child or scheduler startup. QWEN-F carries only
four non-path identity fields; `SPEC180_MODEL_MANIFEST` and
`SPEC180_MODEL_ROOT` are the sole model path bindings.
The model-identity and prompt-identity fields are required to use the
content-digest form `sha256:<64 lowercase hex characters>`.

## Verification

- `tests/python/test_spec180_dispatcher.py`: 15 passed.
- `tests/python/test_spec180_release_workflow.py`: 8 passed.
- Full `tests/python/test_spec180_*.py`: 142 passed, 22 warnings.
- `python3 -m py_compile scripts/run_spec180_case.py scripts/spec180_release.py`:
  passed.
- Spec Kit structure audit: PASS (25 FR, 9 SC, 20 tasks).
- Spec180 contract gate: PASS (`contractReady=true`,
  `qualificationReady=false`, `issues=[]`).

## Remaining qualification blockers

The real NFD/NDN-SVS ACK-to-Response YOLO driver remains fail-closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; T014 convergence, T015 local
qualification, T016 exact-SIF replay, and T017--T020 remote/Tiger work remain
open. No MiniNDN, SIF, CUDA, Tiger, or Qwen3.6-27B result is promoted by this
checkpoint.
