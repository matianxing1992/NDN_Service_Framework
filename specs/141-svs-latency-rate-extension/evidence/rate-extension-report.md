# Spec 141 600/800 pps Rate-Extension Report

## Closure Verdict

`NEGATIVE_RESULT_RETAINED`

All four cells ran exactly once with the frozen Spec 140 binary and
configuration. The 600 pps cells are valid `LOAD_UNSUSTAINED` measurements.
The 800 pps cells are `HARNESS_INVALID` because the unchanged preregistered
signer-utilization gate was exceeded.

## Configuration Identity

```text
binary SHA-256:
a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a

topology:
two MiniNDN nodes, 100 Mbps, 10 ms one-way, no configured loss

traffic:
both nodes publish and subscribe; 256-byte payload; RSA-2048

timing:
10 s warmup, 60 s measurement, 10 s drain

matrix:
600-inline, 600-worker, 800-inline, 800-worker

automatic retries:
0
```

Only the offered rate changed from Spec 140.

## Cross-Rate Results

The 400 pps rows are frozen Spec 140 evidence. The 600/800 rows are the fresh
Spec 141 campaign.

| Rate | Mode | Terminal | Attempted pps/peer | Delivered pps/peer | Delivery | Samples | Mean ms | p50 ms | p95 ms | p99 ms |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 400 | Face-inline | COMPLETE | 400.00 | 400.00 | 100.00% | 48,000 | 76.985 | 77.219 | 105.722 | 116.785 |
| 400 | One-worker | COMPLETE | 400.00 | 400.00 | 100.00% | 48,000 | 58.236 | 56.290 | 80.949 | 97.219 |
| 600 | Face-inline | LOAD_UNSUSTAINED | 599.98 | 0.00 | 0.00% | 0 | N/A | N/A | N/A | N/A |
| 600 | One-worker | LOAD_UNSUSTAINED | 599.98 | 65.97 | 11.00% | 7,917 | 93.870 | 66.299 | 259.679 | 367.900 |
| 800 | Face-inline | HARNESS_INVALID | 800.00 | 0.00 | 0.00% | 0 | N/A | N/A | N/A | N/A |
| 800 | One-worker | HARNESS_INVALID | 800.00 | 0.00 | 0.00% | 0 | N/A | N/A | N/A | N/A |

The 600-worker percentiles describe only the 7,917 delivered survivors. They
are not comparable to the full-delivery 400 pps population as a capacity or
quality success.

## 800 pps Admission Failure

The unchanged maximum estimated signer-utilization gate is 90%.

| Mode | peer-a | peer-b |
|---|---:|---:|
| Face-inline | 94.89% | 95.14% |
| One-worker | 104.52% | 102.31% |

Both 800 pps cells therefore remain inadmissible even though their APP pacers
reached the requested attempted rate.

## Interpretation

- At 400 pps, both modes sustain full delivery and the worker lowers
  mean/p50/p95/p99.
- At 600 pps, moving production off Face prevents a complete zero-delivery
  collapse in this run, but 11% delivery is still a failed capacity outcome.
- The worker removes RSA production work from Face; it does not remove the
  shared Sync/Fetch/network retrieval ceiling that appears after publication.
- At 800 pps, the same configuration fails its preregistered signer-load
  admissibility condition, so no valid inline-versus-worker latency conclusion
  is allowed.

## Evidence Integrity

- Campaign:
  `results/spec141-svs-latency-rate-extension/extension-20260724T001016Z`
- Analyzer status: `FAIL`, caused only by the two retained
  `HARNESS_INVALID` 800 pps receipts.
- Spec 141 result tree SHA-256:
  `d3a49cfc6ffaca7124e945b6b3f68c84a9f825139f53552df1479f9c0531f929`.
- Frozen Spec 140 result tree remains:
  `03950caba86fcdc60c6ca66dc8713b4f1e1fd42625198f8bb6ff0f93ac72154c`.

No cell was retried, replaced, tuned, or omitted.
