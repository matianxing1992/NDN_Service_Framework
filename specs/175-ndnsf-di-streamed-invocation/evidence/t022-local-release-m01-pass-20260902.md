# T022 host/CPU M01 production-path pass (2026-09-02)

## Scope

This is a post-rebuild engineering run of the single representative M01
MiniNDN path. It is not yet a T020-sealed qualification record because the
Python native extension was rebuilt after the prior source seal.

## Result

- The same four-Provider topology started and tore down cleanly.
- The repository STATUS route completed ACK, Selection, Response, and response
  decryption.
- The Artifact/v2/STORE collaboration completed after the rebuilt
  `commit_collaboration_plan` binding was loaded.
- The streamed generation route completed 8 ordered tokens across 4 stages and
  returned one terminal Response with `stopReason=EOS`.
- The run exited with `M01_NORMAL_RC=0` and
  `NDNSF_DI_SPEC175_CASE_PASS`.
- Measured diagnostic runtime was `tiny-onnx`, `distributed_ms=6442.80`.

## Evidence

Run root:
`results/spec175/focused-20260902-normal-r1/`

Relevant records:

- `run.log`: clean process exit and M01 pass markers;
- `spec175-case-result.json`: case result and ordered token evidence;
- `llm-pipeline-user-measured.csv`: measured response timing;
- `spec175-request-lifecycle.jsonl`: control/data lifecycle events.

## Qualification boundary

This pass clears the previously observed Artifact STORE/`commit_plan`
segmentation-fault symptom for the rebuilt local candidate. It does not close
T020 or authorize SIF/Tiger promotion: the source/dependency/native-extension
seal must be regenerated, the design-to-code convergence audit must pass, and
the exact local SIF smoke must still pass under T022.
