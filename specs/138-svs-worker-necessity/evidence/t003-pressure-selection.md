# T003 Pressure Selection Result

## Verdict

`WORKER_QUALIFICATION_FAILED`

The once-only control ladder and worker qualification ran under
`results/spec138-svs-worker-necessity/confirmation01-20260723`.

| Cell | Attempted pps | Rate error | Delivery ratio | Face CPU fraction | Heartbeat p99 | Admissible |
|---|---:|---:|---:|---:|---:|---|
| control 1000 | 988.800 | 1.1200% | 0.122371 | 22.52% | 23.418 ms | no |
| control 800 | 792.467 | 0.9417% | 1.000000 | 16.93% | 1.880 ms | yes |
| worker 800 | 783.667 | 2.0417% | 1.000000 | 6.98% | 3.555 ms | no |

The worker qualification missed the preregistered +/-2% offered-load gate by
0.0417 percentage points. The threshold is not widened and the cell is not
rerun. Spec 138 therefore creates no seal and no formal receipt.

This result identifies a new conservative successor boundary: the next
unobserved registered rate is 600 pps. It is not selected from a favorable
worker outcome; it is selected solely below the measured 800 pps offered-load
failure while retaining predicted Face pressure.
