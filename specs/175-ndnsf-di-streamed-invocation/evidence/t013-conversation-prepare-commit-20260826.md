# T013/T030 Conversation checkpoint prepare/commit correction (2026-08-26)

## Problem

The User-side promotion path previously persisted the aggregate conversation
checkpoint before publishing Provider COMMIT controls.  A failed control
publication could therefore leave a durable User checkpoint while one or more
Provider role candidates were still staged or had not committed.

## Correction

`ConversationCoordinator.prepare_checkpoint()` now computes and signs the exact
next checkpoint without changing the visible checkpoint, transcript, journal,
or pending-turn ownership.  `AutomaticStreamingHandle._promote_conversation`
uses the prepared digest to publish role-bound COMMIT controls first.  Only
after every control publication succeeds does it call `commit_turn()` with the
same fixed `now_ms`; the resulting bytes must match the prepared checkpoint.
If a control or final commit fails, already-published controls receive a
best-effort authenticated ROLLBACK and the User turn remains unavailable.

Coordinators that do not expose `prepare_checkpoint()` are retained only as a
source-compatible test-double path and keep the previous ordering; production
`ConversationCoordinator` always provides the prepare/commit boundary.

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec175_conversation.py
25 passed
```

The new regression proves that the preview is not visible before commit and
that a commit at the same timestamp produces byte-identical checkpoint bytes.
The network acknowledgement and real MiniNDN M11--M14 evidence remain open
under T030--T033; this is a local transaction-boundary correction, not a
qualification result.
