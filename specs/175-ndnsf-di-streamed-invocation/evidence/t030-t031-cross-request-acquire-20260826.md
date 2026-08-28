# T030/T031 cross-request state-acquire checkpoint (2026-08-26)

This checkpoint records the implementation boundary for the distinction
between one-Request decode state and same-conversation state used by a later
Request. It is not a formal G2, MiniNDN, CUDA, SIF, or Tiger qualification.

## Implemented path

`ProviderConversationStateManager` keeps request-local entries and committed
`ConversationStateEntryV1` records in separate maps. A completed turn promotes
the complete adapter-owned state only through the all-role promotion
transaction. A later turn must call:

```text
acquire_for_request(APPEND_DELTA, freshRequestId, role, receipt)
release_for_request(APPEND_DELTA, freshRequestId, receipt)
```

The acquire operation rejects `FULL_CONTEXT`, an origin Request ID, a wrong
conversation/role, a wrong parent context epoch, or an unavailable/expired
receipt before model execution. It performs at most one existing single-flight
host-to-GPU prefetch, then pins the exact complete state. Release drops that
pin. It never searches the request-local map and increments the conversation
hit counter only after the exact entry is GPU-ready and pinned.

The M11 local conversation probe now executes this seam for all four roles
between the first turn's promotion and the second turn's successor promotion.
The acquired opaque state is used as the suffix-prefill readiness input; no
state tensor, pointer, path, or checkpoint bytes are emitted in evidence.

## Focused evidence

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q \
  tests/python/test_spec175_conversation.py \
  tests/python/test_spec175_real_minindn_gate.py
51 passed
```

The new direct-acquire regression also proves that the origin Request ID and a
wrong parent epoch fail closed, while a fresh Request receives one pinned
conversation hit and can release it without leaving a request-local entry.

## Remaining acceptance boundary

The native CUDA-resident Qwen3.6 path, real multi-Provider network M11--M14
execution, current-source G0--G3 manifests, final SIF, and Tiger G6C remain
open under T020/T022/T023/T034. This evidence therefore does not mark T030 or
T031 complete.
