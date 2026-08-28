# T013 cancellation and deadline checkpoint - 2026-08-25

## Scope

This checkpoint covers the production coordinator stop boundary and the real
I08 cancellation path. It is not the complete T013 or G1 acceptance matrix.

## Implementation

- `CollaborationContext::streamRemainingDeadline()` derives bounded remaining
  time from the authenticated streamed request's absolute deadline.
- `NativeProviderHandler` maps the shared request lifecycle to
  `NativeEpochStopReason::{Cancelled,Deadline}`.
- `NativeEpochCoordinator` checks the stop reason before new epoch work and
  around every post-run observation, event publication, feedback publication,
  and state commit boundary.
- A stop after runner return enters the existing candidate rollback path;
  terminal release removes any committed state for the request/role.

## Verification

```text
./waf build --targets=unit-tests,integration-tests -j2
PASS

./build/unit-tests \
  --run_test=NativeEpochCoordinatorRejectsCancellationBeforeRunner,NativeEpochCoordinatorRollsBackWhenDeadlineExpiresAfterRunner
PASS: 2 test cases

./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI08CancelsAfterThirdEvent
PASS: 1 test case
```

The unit cases prove zero runner calls for cancellation observed before work,
and one runner call followed by candidate rollback, zero event publication,
and zero store residue when the deadline expires after runner completion. I08
proves the real Request/ACK/plan/Selection/native path emits exactly three
events and leaves both selected Providers with zero entries, zero pins, zero
candidates, and one terminal cleanup.

The first expired-deadline integration run found that the upstream role still
committed once. `NativeProviderHandler` had installed `stopCheck` only when
`CollaborationContext::isStreamed()` was true, but that predicate means the
Provider owns the external stream publisher; it is false for upstream roles.
The corrected handler gates deadline observation on authenticated
`StreamRequestOptions` for every selected role and gates cancellation lookup
separately on stream-lifecycle ownership. The two-Provider regression now
passes after ACK and plan commit with two `REQUEST_DEADLINE` Provider failures,
zero events, zero commits, and zero state residue. I08 still passes afterward.

```text
./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState
PASS: 1 test case

./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI08CancelsAfterThirdEvent
PASS: 1 test case
```

## Remaining closure

The registered CPU identity/lifecycle cases are implemented and focused-tested.
T013 still requires the matched CPU cache-effectiveness observation/control.
CUDA residency and the real Qwen3.6 qualification remain later gates.
