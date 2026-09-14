# Spec186 direct-target audit — 2026-09-13

## Verdict

The specification now states MiniNDN + TigerCluster YOLO as the direct experiment
objective. The executable path and the validation matrix agree with that objective:
candidate closure and exact runtime composition are prerequisites, real MiniNDN YOLO is
first, the same composition then runs on TigerCluster single-node GPU and two-node
normal/negative cases, and an independent two-node reuse run closes the primary path.

## Audit findings and repairs

The implementation already routed `submit.py` through `check/prepare/local/submit/collect`
and enforced terminal protocol, numerical, role/GPU, exit and cleanup evidence. The
ambiguity was documentary: static `PASS`, native closure, preflight and Qwen rows could
be read as alternate completion paths. The following documents now make the boundary
explicit:

- `spec.md` has a direct objective and a four-step primary path; static evidence is
  explicitly a prerequisite and Qwen is conditional.
- `plan.md` identifies the only primary chain as
  `T005 → T006 → T007 → T009 → T010 → T012`.
- `tasks.md` labels the same chain as the direct qualification path and limits T013 to
  evidence reconciliation.
- `validation-matrix.md` marks V06/V07/V09/V10/V11/V13 as direct runtime rows.
- `quickstart.md` starts with the real MiniNDN-to-Tiger execution path and states that
  checks and probes cannot substitute for runtime receipts.

## Code and evidence boundary

CodeGraph confirms the maintained entrypoints are the intended path: the MiniNDN runners
own the local process boundary, `submit.py` owns candidate-gated local/Tiger lifecycle,
and `collect` calls the terminal validator. No document change promotes the existing
tiny-ONNX collaboration receipt, static pre-dispatch receipts, or historical Spec183
evidence to YOLO qualification.

The direct target is therefore **corrected and unambiguous in the documents**, but it is
not yet achieved. Current direct rows remain `WAITING_EXTERNAL_INPUT` (MiniNDN YOLO) or
`NOT_RUN` (Tiger single/two-node and reuse) until the exact source-sealed base SIF,
canonical YOLO package/config/key-map/model inputs, and retained runtime allocations are
available.
