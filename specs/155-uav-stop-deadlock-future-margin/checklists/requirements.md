# Requirements Checklist

- [x] Spec 154 failure is preserved rather than relabeled.
- [x] APP deadlock owner and exact lock/join cycle are identified.
- [x] Core change is a workload-neutral capacity rule.
- [x] Retry priority, API, wire, security, and FEC contracts remain unchanged.
- [x] Success requires a complete fresh six-rate matrix.
- [x] Rate, delivery, latency, future-hit, retry/timeout, and exit gates are
  explicit before implementation.
- [x] Rollback and frozen-evidence boundaries are explicit.
