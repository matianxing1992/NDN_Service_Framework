# T029/T030/T031 Focused Conversation-State Checkpoint (2026-08-26)

This checkpoint records the implementation seam for request-local versus
conversation-scoped state. It is development evidence only; it does not close
the real multi-process, CUDA, G0--G3, SIF, or Tiger acceptance gates.

## Implemented boundary

- `ProviderConversationStateManager` owns separate request-local and
  conversation-scoped stores. A later request can only perform an exact
  conversation receipt lookup; it never searches the request-local store.
- `ConversationStatePromotionTransaction` stages every selected role while
  retaining request-local ownership. It acquires participating manager locks in
  deterministic order, validates every candidate and quota, then commits all
  roles together.
- A stale parent, duplicate successor key, quota failure, or protected journal
  failure releases/rolls back the aggregate without exposing a partial
  conversation checkpoint. A failed candidate construction also cleans every
  request-local record it owns.
- The streamed Python conversation probe now passes this transaction to
  `ConversationCoordinator.commit_turn`; it no longer promotes each role
  independently before checkpoint commit.
- Completed prefetch handles are stable for the current GPU residency
  generation; a new host transition invalidates the old handle before another
  transfer begins.

## Focused verification

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec175_conversation.py \
  tests/python/test_spec175_real_minindn_gate.py -k 'conversation or prefetch'
27 passed, 23 deselected

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec175_*.py
144 passed
```

The focused tests cover all-role atomic commit, protected-journal rollback,
request-local cleanup, same-parent conflict, host prefetch cancellation, and
the M11--M14 conversation probes. They do not yet prove a real network
cross-request Provider state hit or the native Qwen/CUDA path; those remain
open under T030/T031 and the dependent qualification tasks.

The existing native conversation boundary was also rerun without source
changes:

```text
build/unit-tests --log_level=error \
  --run_test='ConversationStateStoreSeparatesCrossRequestState,\
NativeProviderRuntimePromotesDecodeStateAcrossRequests,\
NativeProviderRuntimeRestoresConversationStateForFreshRequest,\
NativeEpochCoordinatorRestoresConversationStateAndExtendsPrefix'
4 test cases; no errors detected
```
