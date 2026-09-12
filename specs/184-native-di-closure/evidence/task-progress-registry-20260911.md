# Spec184 Task Progress Registry Evidence

**Date**: 2026-09-12
**Status**: DOCUMENTATION_PASS_ONLY / product and qualification unchanged
**Scope**: shared `speckit-code-design` rule and Spec184 `tasks.md`

## Change

- `tasks.md` now has one `Execution Progress` row for every executable unit `T001`–`T008`.
- The existing batch table is explicitly named `Logical Batch Progress`; it records batch exits and
  cannot replace the unit registry.
- Unit states agree with the checked task list: `T001`–`T006` `DONE`, `T007` `IN_PROGRESS`, and
  `T008` `NOT_STARTED`.
- The repository skill and the installed personal copy carry the same registry rule.

## Checks

```text
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal
PASS: 11/11 local entrypoints; personal shared skill=present

python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/184-native-di-closure --strict
Structural verdict: PASS; tasks=8; tasks_complete=6

custom registry check
checkbox_ids == progress_ids == {T001..T008}
one_to_one=True

git diff --check
PASS
```

The later T007 C++ isolation-counterexample checkpoint is recorded in
`evidence/t007-process-qualification-20260911.md`; it does not change this registry's role as a
status index. No SIF/Tiger run or final qualification claim is made by this registry record.

## Current status overlay (2026-09-12)

The T007 unit remains `IN_PROGRESS`; its ordered remainder now records A0 current-candidate
receipt, A1 YOLO Y-A, and A2 YOLO Y-B/Y-N as `PASS_FOR_ROW`. A3 remains
`WAITING_EXTERNAL_INPUT` because this host can run only Qwen3-0.6B smoke, and A4 remains
`PARTIAL`. The r4 test-target lookup and the rejected old-test/r4-library mixed-ABI retry are
recorded in `evidence/t007-model-capability-20260912.md`; neither is a product or qualification
result. This overlay does not mark T007 or Spec184 complete and does not promote the local
candidate.

## Superseding source refresh (2026-09-12)

The framework/DI source review and same-tree rebuild changed the candidate
identity after the overlay above. A0 now refers to the refreshed receipt in
[A4 current candidate refresh](a4-current-candidate-refresh-20260912.md);
the former YOLO A1/A2 rows are historical and return to
`NOT_RUN_CURRENT_CANDIDATE` until root Y-A, Y-B, and Y-N are repeated. T007
remains `IN_PROGRESS`; the exact 27B row remains external and T008 remains
blocked.

## Current binding and Y-A overlay (2026-09-12)

The Python binding was rebuilt against the refreshed r4 candidate and the native identity receipt
was regenerated and verified. Root MiniNDN Y-A then passed on that same candidate; its C++ numerical
oracle matched and terminal cleanup completed. T007-A1 is therefore `PASS_FOR_ROW`. T007-A2
Y-B/Y-N remains `NOT_RUN_CURRENT_CANDIDATE`; A3 remains `WAITING_EXTERNAL_INPUT` because this
host can run only Qwen3-0.6B smoke, and A4 remains `PARTIAL`. T007 and Spec184 are not complete.

The protected multi-provider Y-B run `r59` also passed against the same refreshed candidate;
T007-A2 is now `PARTIAL` with Y-N still outstanding. This update does not promote T007 or
Spec184 to complete.

## Current Y-N closure overlay (2026-09-12)

The current-candidate Y-N run `r60` completed with aggregate `PASS`. Its matrix is
`.codex-tmp/spec184-yolo-Y-N-output-20260912-r60/y-n-matrix-result.json` (SHA-256
`009e07d68a766f1545312b87261884e789cbb207db4404432f1ac3729161215a`), and its launcher log
SHA-256 is `86cbd5a303b6cd5cf5db51ec5aa4568ec092aa18d09a4e762d32a306b7d694f1`.
`Y-N-O/C/P/R/I/E/L` all passed, including the declared native E rejection permutations and
controlled child cleanup. Together with Y-A `r58` and Y-B `r59`, this closes T007-A1/A2 for
the refreshed candidate. T007 remains `IN_PROGRESS`; A3 is `WAITING_EXTERNAL_INPUT` for the
exact 27B bundle, A4 remains `PARTIAL`, and T008 remains blocked. Local 0.6B evidence is not
used as a 27B qualification result.
