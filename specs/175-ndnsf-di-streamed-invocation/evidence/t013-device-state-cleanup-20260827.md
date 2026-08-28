# T013 CUDA streamed device-state cleanup checkpoint (2026-08-27)

This checkpoint records a local source correction only. It is not CUDA
qualification evidence and does not close T013 or T025.

## Correction

`OnnxRuntimeModelRunner::runStreamedImpl` now installs an RAII cleanup guard
for the session-keyed CUDA state map. The guard erases the request/session
entry on normal terminal export and on cancellation or runner exception. The
terminal state bundle is exported once before cleanup; intermediate streamed
epochs remain device-resident and are not copied to host. A lock protects the
final state export and erase against concurrent runner transitions.

This closes a lifecycle leak that could leave stale device allocations after a
completed or failed stream and make a later request appear to have a valid
predecessor. It does not prove real GPU execution, Qwen3.6 graph coverage, or
zero host round trips.

## Verification

```text
./waf build --target=unit-tests -j1
  exit 0

build/unit-tests --run_test='Spec175*' --log_level=message
  29 test cases; no errors detected
```

The remaining acceptance work is the real CUDA state-continuity and cleanup
trace in T025/G5 and the multi-Provider replay in T026/G6.
