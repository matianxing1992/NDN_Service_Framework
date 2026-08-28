# Spec170 post-Selection integration rerun (2026-08-19)

This rerun checks the production ingress path that was previously easy to
miss: encrypted Selection publication, assignment retrieval, native dispatch,
request/response completion, and a missing-backbone terminal failure.

## Command and subject

```text
command: build/integration-tests --run_test=Spec170NativePostSelection --log_level=test_suite
return code: 0
binary sha256: b1c4ee6d3f1175e1f6f6de4a06150a68ace68d89e22f857b708fd7ee43b11428
captured log: 3,102 bytes
log sha256: 287ce515a9c65bac724bc60436801ae0bb8c0d690d9f258c34a8d64f91da190e
```

## Result

All four cases passed with `*** No errors detected`:

1. `ProductionIngressRunsNativePostSelectionAssignmentFetch`
2. `ProductionIngressReportsNativeDeviceMismatch`
3. `ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse`
4. `MissingBackboneOutputFailsBeforeGlobalRequestTimeout`

The multi-role case reported `handler=1`, `assignmentFetch=1`,
`responsePublished=1`, `statusFailed=0`, `timedOut=0`, and `runnerRoles=4`.
The missing-backbone case terminated dependency waits with explicit
`CANCELLED` events instead of passing from a stale timing record or waiting for
the global request deadline.

## Scope boundary

This is current-build C++ integration evidence for the post-Selection path. It
strengthens protocol/lifecycle qualification but does not close T029, the full
T028/T037 negative matrix, exact-SIF parity for the current dirty source, or
T036 performance optimality.
