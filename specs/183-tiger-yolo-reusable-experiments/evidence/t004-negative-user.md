# T004.s — bounded negative User observation

2026-09-07; follows native cutpoint checkpoint `c7aea386`.
Status: IN_PROGRESS, component evidence only. T004/T007/T015 remain open;
`NEGATIVE_RUNNER_NOT_WIRED` still prevents public negative submissions.

The generic YOLO User accepts a trusted Python terminal observer and bounded
dependency-progress timeout. Its existing `finally: client.shutdown()` remains
the owner boundary. Tiger's observer validates the nine-event Selection prefix,
binds request/attempt/placement/plan, waits within the request budget, then reads
the handle again after application shutdown. A response observed in either read
is retained; ordinary timeout cannot become negative-case PASS.

`negative-user.json` is explicitly OBSERVATION_ONLY. It includes response presence,
status, payload length/hash and elapsed budget, without response bytes or secrets.
The one-request application schedule omits independent numerical ORT reference
execution and retains public assignments. Shared two-rank operator dispatch and
independent native failure/cutpoint/cleanup collection are still pending.

Validation retained under
`Experiments/TigerCluster/results/spec183-negative-user-20260907/`:

- `focused.log`: 113 passed, 7.65s, across negative observer, lifecycle reader,
  application boundaries and bundle checks (16 new observer cases).
- `extra-first.log`: composition check passed; two schedule assertions failed
  because the fixture expected integer 0 at the existing string invocation-ID
  boundary. Production behavior was unchanged.
- `extra-fixed.log`: the two corrected schedule assertions passed. Together
  with composition this adds three new cases, for 19 new cases overall.

These checks use real lifecycle files/validators and explicit handle/process
boundary doubles. They do not establish native User shutdown behavior, actual
YOLO execution, distributed failure, SIF correctness or GPU qualification.
Previously recorded native cutpoint tests were not rerun; current changes are
Python-only. The frozen harness inventory now contains 28 files; no candidate E
or SIF was refreshed. Next: strict retained-record reader, two-rank negative
owner, producer/consumer evidence join, then revisit the design-code audit.
