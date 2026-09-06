# T031 adapter-owned conversation-state transfer

**Date**: 2026-09-01  
**Subject**: current Experimental source tree, native C++ unit target  
**Scope**: production `NativeProviderRuntime`/`ConversationStateStore` seam and
the ONNX Runtime adapter's CUDA conversation-state ownership path.

## Repair

The previous implementation treated a serialized `TensorBundle` plus residency
counters as if it were a cache transfer.  That was not sufficient: a later
request could appear to restore state even though no adapter buffer had moved.

The repaired path introduces `NativeConversationStateHandleV1`.  The native
store now keeps only the exact binding, an opaque adapter handle, the logical
byte count, residency/lifecycle metadata, and the runner reference.  The
adapter owns the actual state:

```text
request session -> promoteSessionStateToConversation()
               -> Provider store stages opaque handle
GPU -> host     -> pauseConversationStateToHost() (D2H + host buffer)
host -> GPU     -> prefetchConversationStateToGpu() (cudaMalloc + H2D)
new request     -> restoreConversationState() (device state binding)
terminal/evict  -> releaseConversationState() (zero/free/erase)
```

The CUDA adapter dynamically resolves the CUDA runtime so CPU builds do not
gain a toolkit link dependency.  ORT-managed state remains Provider-local;
raw device allocations created for a host-tier prefetch are tracked and freed
after the ORT wrappers are released.  The CPU path continues to use the
explicit serialized-state control path and is not presented as CUDA evidence.

## Focused acceptance

- `./waf build -j1 --targets=unit-tests`: **PASS**, 104/104 compile/link.
- `./build/unit-tests --run_test=NativeProviderRuntimeUsesAdapterOwnedConversationTransfers`:
  **PASS**.
- `./build/unit-tests --run_test='*ConversationState*'`:
  **PASS**, 6/6 cases.

The new transfer-runner regression observes promotion, restore, pause,
prefetch, and release callbacks; verifies the store reports GPU/host bytes;
and verifies that a resumed request reaches the runner without a
`__ndnsf_provider_decode_state` TensorBundle.  Existing serialized-state
conversation tests still pass, preserving the explicit CPU fallback.

## Boundary

This closes T031's implementation boundary only.  It does not claim that a
Qwen3.6-27B CUDA graph, a final SIF, or TigerCluster has been qualified.
Physical CUDA copy measurements and the final candidate remain T034/G6C work;
the fresh design-to-code verdict is T042.  No formal G0--G7 or Tiger result is
reopened by this focused repair.
