# Contract: Progress Admission and Dual Deadlines

## Progress observation

Progress uses the generic NDNSF operation-status path and binds:

```text
requestId, operationId, provider, role,
attempt, epoch, sequence, phase,
completedWork, totalWork?
```

The status must be authenticated and already bound to the admitted
collaboration before deadline evaluation.

## Renewal rule

An observation renews idle time only when:

1. all bindings match;
2. it is not post-terminal;
3. `(epoch, sequence)` is newer than the last admitted position;
4. work or an allowed phase transition advances monotonically.

Renewal computes:

```text
idleDeadline = min(observedAt + idleBudget, hardDeadline)
```

The hard deadline is set once at start and never changes.

## Non-renewing observations

Unauthenticated, forged, duplicated, reordered, wrong-bound, non-advancing,
invalid-total, or post-terminal observations are retained but do not renew.

## Terminal rules

- hard expiry: `HARD_TIMEOUT`;
- idle expiry before hard expiry: `STALLED`;
- explicit completion, failure, or cancellation: its corresponding terminal;
- if hard and idle boundaries coincide: `HARD_TIMEOUT`;
- once terminal: all later terminal or progress input is observational only.

## Testability

The state machine accepts a monotonic clock interface. Unit tests advance a
fake clock and must not sleep.
