# T031 local state-cleanup checkpoint (2026-08-27)

## Scope

This checkpoint records local correctness fixes in the Python conversation-state
bridge and the native Provider binding invalidation seam. It does **not** claim
that T031 is complete: the real multi-process Provider path, CUDA-resident
Qwen3.6 state bundle, and MiniNDN evidence remain open.

## Fixes

- A failed asynchronous host-to-device prefetch now restores the entry to an
  executable `HOST_RESIDENT`/`IDLE` state, releases non-authoritative copies,
  and permits a later retry.
- Cancelling a prefetch advances a generation fence, so a stale worker cannot
  publish a completed copy after a newer request has started.
- Provider-boot invalidation cancels pending work, clears request-local,
  conversation, staged, and prefetch views, and releases each state object at
  most once even when views alias the same object.
- Native staged promotion now rejects a non-empty binding checkpoint that does
  not match the authenticated aggregate checkpoint; the legacy overload cannot
  accept an unauthenticated non-empty checkpoint. Provider binding changes also
  clear the native staged-promotion side index.

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec175_conversation.py
29 passed
```

The focused native checkpoint-binding regression and the full native/unit
build are being rerun against the current source. These tests establish local
cleanup and binding invariants only; they do not replace the T022 real
MiniNDN M11--M14 matrix or the later SIF/Tiger gates.
