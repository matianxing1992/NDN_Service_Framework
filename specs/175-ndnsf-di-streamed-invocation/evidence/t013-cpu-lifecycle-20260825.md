# T013 CPU identity and lifecycle matrix - 2026-08-25

## Scope

This checkpoint extends the production `NativeProviderRuntime` path. It does
not claim CPU cache-effectiveness timing, CUDA residency, real Qwen3.6
qualification, or complete T013 acceptance.

## Added coverage

- Runtime constructor accepts explicit decode-state byte and entry limits;
  unchanged defaults are 512 MiB and 128 entries.
- A 27-variant production predecessor matrix mutates every complete identity
  field, component digest ordering, state/predecessor epoch, request, attempt,
  and generation. Every variant is rejected before runner execution, leaves
  the predecessor committed, and leaves no pin or candidate.
- Three concurrent chains cover different generations and attempts 1 and 2 of
  the same request. Six runner executions observe only their own state; the
  snapshot records three hits and six commits. Re-registering the runner with
  boot ID `boot-b` clears all three `boot-a` lineages and records three cleanups.
- A one-entry production runtime evicts the inactive LRU state. A later decode
  of the evicted request fails before runner execution.
- Event admission rejection and TOKEN_FEEDBACK publication failure roll back
  the staged candidate once. Activation publication failure occurs before a
  candidate is staged. All three finish with zero entries, pins, and
  candidates.
- Distinct prefill inputs are mandatory in the capacity test so the separate
  exact-forward output cache cannot mask KV runner invocation.

## Verification

```text
./build/unit-tests --run_test=KvStateStorePinsAndAtomicallyRollsBackCandidates,NativeProviderRuntimeAutomaticallyReusesCommittedDecodeState,NativeProviderRuntimeAppliesConfiguredDecodeStateCapacity,NativeProviderRuntimeIsolatesConcurrentGenerationsAndAttempts,NativeEpochCoordinatorRejectsCancellationBeforeRunner,NativeEpochCoordinatorRollsBackWhenDeadlineExpiresAfterRunner
PASS: 6 test cases

./build/unit-tests --run_test=NativeEpochCoordinatorRollsBackRejectedEventAdmission,NativeEpochCoordinatorRollsBackFailedFeedbackPublication,NativeEpochCoordinatorLeavesNoStateAfterActivationFailure
PASS: 3 test cases

./build/unit-tests --run_test=NativeProviderRuntimeRejectsEveryPredecessorIdentityMutation
PASS: 1 test case with 27 mutation contexts
```

## Remaining closure

The registered CPU identity/lifecycle cases are implemented and focused-tested.
The matched CPU cached-versus-full-prefix control must now record actual new
input extent, represented prefix extent, and positive prefix work avoided.
CUDA residency and Qwen3.6 remain later gates.
