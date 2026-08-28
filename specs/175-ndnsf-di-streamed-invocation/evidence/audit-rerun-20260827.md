# Spec175 audit rerun evidence (2026-08-27)

This record captures the post-correction verification performed against the
current shared worktree. It is regression evidence only: the worktree is still
dirty, so no result below is a current G0--G4 qualification manifest.

## Checks

| Check | Result | Meaning |
|---|---:|---|
| Spec Kit structural audit | PASS; 73 requirements, 16 criteria, 34 tasks, 21 accepted | Documents are structurally consistent; 13 tasks remain open. |
| Focused Spec175 + Spec168 provider-generation Python regression | 195 passed | Current implementation regression, not a sealed qualification subject. |
| Broader Spec175/168/170 Python regression | 349 passed, 10 skipped, 1 warning | Shared regression evidence; expected skips and the existing warning remain visible. |
| Spec175 + streamed-API Python surface | 201 passed | Current wrapper/API compatibility regression. |
| Native Spec175/ConversationState selection | 33 passed | Focused native state/stream regression. |
| Native unit suite | 600 cases, no errors | Full current build unit result. |
| Spec175 streamed integration selection | 16 cases, no errors | Current native stream integration result. |
| Contract gate (`--expected-negative`) | `BLOCKED_EXPECTED` (`DIRTY_INPUT_TREE`, 255 paths) | Correctly refuses promotion until T020 seals the complete source subject. |
| Stream/continuation correction regression | 71 passed | Current fail-closed identity, callback, activation-edge, deadline-bound promotion, and receipt-lineage checks. |

## Interpretation

The timing-summary, stream-boundary, and conversation-deadline corrections and the document terminology corrections do not
close T014, T015, T020, T022--T023, T025--T028, T030--T031, T033, or T034.
Historical G0--G3 manifests, the retained M09 signal exit, and all existing
SIF candidates remain diagnostic inputs only. The next valid promotion path is
to finish the implementation queue, create one new source seal, and then run
the ordered G0--G7 sequence once.
