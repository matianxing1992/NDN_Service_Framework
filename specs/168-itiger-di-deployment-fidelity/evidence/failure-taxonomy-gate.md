# Spec 168 failure-taxonomy gate

**Status: PASS (local contract gate).** This gate validates the failure
classification and progress-deadline boundary before any new remote
submission. It does not relabel the retained TigerCluster failures.

## Contract coverage

The canonical `FailureRecordV1` maps every admitted primary code family to one
boundary: `ENVIRONMENT`, `BOOTSTRAP`, `ROUTING`, `ACK`, `PLAN`,
`REPO_PUBLISH`, `REPO_FETCH`, `PREP`, `DEPENDENCY`, `EXEC`, `TOKEN`,
`RESPONSE`, `CLEANUP`, or `ANALYZER`. `UNRESOLVED_EVIDENCE_GAP` is the only
explicit unresolved class. Generic `TIMEOUT` and mismatched code/boundary
pairs fail closed.

Each record binds the request and attempt, component, optional Provider/role,
operation or artifact range, terminal reason, last authenticated checkpoint,
and retained evidence paths. `RequestFailureStatus` exposes the same primary
boundary to operators without allowing a non-terminal request state.

`DeadlineMonitor` now retains the last accepted checkpoint in every decision
and in `terminal_evidence()`. Duplicate, reordered, unauthenticated, wrong
binding, and non-advancing observations do not renew the idle deadline. A
no-progress deadline produces `STALLED`; the absolute deadline produces
`HARD_TIMEOUT`; first terminal state wins.

## Verification

```text
PYTHONPATH=.:pythonWrapper:NDNSF-DistributedInference \
  python3 tests/python/test_spec168_failure_taxonomy.py       4 passed
PYTHONPATH=.:pythonWrapper:NDNSF-DistributedInference \
  python3 tests/python/test_spec165_progress_deadline.py      6 passed
```

This evidence is local logic coverage. A later remote failure remains
environmental, deployment, repository, or model evidence according to its
retained earliest boundary; the analyzer must not infer a later component that
never executed.
