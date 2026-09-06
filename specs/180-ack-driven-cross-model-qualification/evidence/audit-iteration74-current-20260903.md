# Spec180 audit iteration 74 — release boundary checkpoint

Date: 2026-09-03

Verdict: CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED

## Focused evidence

- `tests/python/test_spec180_dispatcher.py`: 13 dispatcher tests pass.
- `tests/python/test_spec180_release_workflow.py`: release/submit tests pass,
  including malformed dispatch rejection before scheduler mutation.
- `tests/python/test_prepare_local_sif_source.py` and
  `tests/python/test_spec175_sif_preflight.py`: source/SIF contract tests pass.
- Combined focused source/SIF/release command reports `58 passed`.
- Complete `tests/python/test_spec180_*.py` command reports `140 passed, 22
  warnings in 34.15s`.
- Structural Spec Kit audit: PASS (25 FR, 9 SC, 20 tasks).
- Spec180 contract gate: `status=PASS`, `contractReady=true`,
  `qualificationReady=false`, `issues=[]`, trust root configured.

## Current corrections

1. The host release validator imports and applies the single T011 dispatcher
   contract before scheduler submission; malformed schema, gate/case, fixed
   arguments, or environment cannot reach `sbatch`.
2. `run-functional.sh` validates the mounted model manifest and workload
   digest formats and contents before it creates the writable evidence root.
   A post-submit file replacement therefore fails closed without creating
   evidence or starting the image dispatcher.
3. The local SIF source archive includes both
   `scripts/run_spec180_case.py` and
   `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`.

4. The dispatcher no longer routes `qwen-functional` to the Spec175 M11
   wrapper. QWEN-F now names a separate Qwen3.6-27B ONNX entrypoint and fails
   closed with `ENTRYPOINT_MISSING` until T019 wires it. This prevents the
   tiny local fixture from being mislabeled as a Tiger Qwen qualification and
   removes duplicate model-manifest/root path bindings from the workload
   environment.

## Qualification boundary

These checks prove only source/release boundary behavior. The candidate SIF
has not been rebuilt with the new files, the real NFD/NDN-SVS ACK-to-Response
driver remains fail-closed with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, and no
MiniNDN, CUDA, SIF replay, or Tiger result exists. T014 design-code
convergence remains the next gate; T015--T020 cannot be promoted from focused
checks to qualification until it passes.
