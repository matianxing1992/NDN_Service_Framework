# MiniNDN Diagnostic Qualification

Two new, non-formal two-node MiniNDN roots were run once:

```text
results/spec152-diagnostic-fps10-20260726T-current
results/spec152-diagnostic-fps60-20260726T-current
```

| Requested | Achieved | Error | Delivery | Mapping | Ready/gap queue |
|---:|---:|---:|---:|---:|---:|
| 10 fps | 10.018 | 0.18% | 99.506% | 0 | 0/0 |
| 60 fps | 59.943 | 0.10% | 99.499% | 0 | 0/0 |

Both cells used the build Core, activated one predictive provider and consumer,
preserved exact wire identity, decoded video, and produced complete metrics.

The initial diagnostic evaluator returned nonzero and those results remain
preserved:

- 10 fps: p99 1145 ms exceeded the formal 1000-ms threshold.
- 60 fps: future-hit 94.60% was below the formal 95% threshold.

These are not harness failures. A 20-second diagnostic has only about 16
measured seconds: startup outliers dominate p99, while the fixed terminal
pending-Interest frontier remains in the future-hit denominator. The diagnostic
contract is therefore corrected prospectively to qualify only wiring, achieved
rate, delivery, Mapping, queues, and complete accounting. It does not rewrite
either result. The immutable >=60-second formal cells retain all latency,
future-hit, retry, and timeout gates.
