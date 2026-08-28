# Spec 148 Closure Report

**Date**: 2026-07-25  
**Status**: **NOT CLOSED — MIGRATION IMPLEMENTED, ACCEPTANCE BLOCKED**

The replacement API and repository migration are implemented. The public
high-level provider surface is now `start()` / `push()` / `flush()` /
`status()` / `stop()`, the old facade and `start_predictive()` are removed,
Python is symmetric with C++, and UAV Drone/Ground Station execute the real
predictive Core path.

## Verification completed

- full native build: PASS;
- rebuilt Python extension loads the local NDNSF library and Boost 1.71;
- full native suite: PASS, 394 tests;
- focused Python facade suite: PASS, 6 tests;
- targeted whitespace/source diff check: PASS;
- fresh immutable two-cell MiniNDN campaign: EXECUTED ONCE, no automatic retry,
  with source and binary hashes unchanged.

Canonical campaign:

```text
results/spec148-predictive-uav-formal-20260726T034348Z/
```

## Formal cell results

| Cell | Result | Delivery | Future hit | Mapping Interests | Retry | Timeout | Recovery |
|---|---|---:|---:|---:|---:|---:|---:|
| zero loss | PASS | 99.842% | 100.000% | 0 | 0 | 0 | not needed |
| 1% loss + 1% reorder | FAIL | 1.424% | 88.029% | 39,488 | 5,087 | 6,821 | 513 / 2,318 attempts |

The impaired cell proves that FEC is active but insufficient to stabilize the
current recovery path. Every newly missing cursor independently fetches the
frontier and linearly scans retained group commits in reverse to find its FEC
group. Concurrent gaps therefore amplify a small packet-loss event into
Mapping/control traffic, retry, timeout, and queue pressure. One-XOR repair
successfully recovered 513 sources, but the resulting control-path expansion
caused the acceptance failure.

The failed cell is preserved. Its profile, workload, window, FEC, retry,
timeout, and SVS timing were not tuned after observation, and it will not be
rerun as Spec 148.

## Other regression evidence

The complete Python discovery suite executed 1,091 tests and did not pass:
2 failures, 10 errors, and 5 skips. The recorded failures are in historical
candidate hashes, frozen Spec 127 source hashes, old Spec 130/132 manifests,
and protected NDN-SVS subject state. They are preserved in
`evidence/python-full-test.log`; they are not represented as Spec 148 passes.

## Closure blockers

- **SC-004** is not satisfied because the full Python suite is not green.
- **SC-005/SC-009** are not satisfied for the light-loss/reorder boundary.
- **T021** remains unchecked because the post-implementation audit is BLOCK.

Fixing recovery lookup/coalescing changes the recovery algorithm and is outside
Spec 148. The next feature should define that boundary explicitly and use a new
formal campaign rather than rewriting or rerunning this evidence.
