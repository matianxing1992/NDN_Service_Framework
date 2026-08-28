# Spec175 implementation corrections (2026-08-27)

This checkpoint records three fail-closed corrections made after the current
implementation audit. It is local regression evidence only and does not alter
the qualification status of T014, T015, T030, T031, or T033.

## Stream identity and callback boundary

`AutomaticStreamingHandle` now validates optional `requestId` and
`generationId` fields on token events and terminal JSON against the bound
attempt/generation. Malformed events, lineage mismatches, and application
callback exceptions become terminal error records instead of escaping from a
delivery thread and leaving the handle waiting indefinitely. Existing opaque
generic completion payloads remain compatible.

## Multi-role activation boundary

`OnePlanGenerationLoop` now rejects duplicate/empty roles, requires a complete
role-to-Provider map when activation publication is enabled, and fails closed
when a selected adjacent role boundary has no activation payload. The prior
behavior silently skipped that edge and could make an incomplete multi-role
execution appear to complete.

## Conversation deadline boundary

Conversation receipt, state-ready, and Provider commit-ack waits are now capped
by the signed request deadline rather than an unconditional 30-second wait.
An already-expired deadline fails promotion before exposing a successor
checkpoint.

## Receipt and error-sink boundaries

`ConversationCoordinator.commit_turn()` now applies the same origin request,
generation, service, requester, and security-domain binding checks as
`prepare_checkpoint()`. Direct commit callers therefore cannot bypass the
receipt lineage contract. Test and probe receipt factories were updated to bind
the generation that actually produced each turn; the M13 malformed-turn case
remains intentionally rejected. Error callbacks are best effort and cannot
escape the delivery thread or replace the original terminal error.

## Verification

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  pytest -q tests/python/test_spec175_conversation.py \
    tests/python/test_spec175_streamed_generation.py \
    tests/python/test_streamed_invocation_api.py
71 passed
```

The full implementation queue and current-source G0--G7 qualification remain
open as recorded in `tasks.md` and `audit.md`.
