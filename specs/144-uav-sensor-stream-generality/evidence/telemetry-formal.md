# T007 Telemetry Formal Evidence

**Campaign**: `results/spec144-uav-sensor-stream-20260724T165132Z`  
**Execution**: 16 declared cells, 16 terminal cells, one invocation each,
zero automatic retries  
**Verdict**: **MEASURED NEGATIVE**

## Treatment Verdicts

| Profile | Accepted / attempted | Required | Exact 95% interval | Verdict |
|---|---:|---:|---:|---|
| zero-loss | 1/1 | 1 | [0.025000, 1.000000] | PASS |
| 1% loss | 5/5 | 4 | [0.478176, 1.000000] | PASS |
| reorder | 4/5 | 4 | [0.283582, 0.994949] | PASS |
| combined | 2/5 | 4 | [0.052745, 0.853367] | FAIL |

The telemetry workload does not pass as a family because the combined
loss-and-reorder treatment accepted only 2/5 repetitions. The failed cells
remain in the denominator and were not rerun.

## Per-Cell Results

All latency columns are AoI in the shared-host steady-clock domain. `Future`
and `Novelty` are provider-confirmed future-hit and Mapping-novelty ratios.
`Nonprod` is the nonproductive Payload Interest ratio.

| Cell | Pass | Delivered | Mean ms | p50 | p95 | p99 | Gap ms | Future | Novelty | Map I | Payload I | Retry | Timeout | Nack | Nonprod | Failed gates |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| zero-loss-r01 | yes | 1200/1200 | 8.053 | 4.904 | 16.504 | 18.449 | 77.086 | 100% | 100% | 1307 | 1300 | 2 | 4 | 0 | 0.000% | — |
| loss-r01 | yes | 1200/1200 | 16.590 | 11.277 | 60.690 | 151.354 | 150.061 | 100% | 100% | 1336 | 1332 | 63 | 65 | 0 | 2.402% | — |
| loss-r02 | yes | 1200/1200 | 12.154 | 5.358 | 29.890 | 125.384 | 129.134 | 100% | 100% | 1344 | 1327 | 65 | 68 | 0 | 2.035% | — |
| loss-r03 | yes | 1200/1200 | 11.216 | 4.283 | 29.248 | 128.710 | 126.531 | 100% | 100% | 1340 | 1320 | 55 | 56 | 0 | 1.515% | — |
| loss-r04 | yes | 1200/1200 | 11.267 | 4.563 | 28.785 | 111.682 | 109.452 | 100% | 100% | 1332 | 1326 | 53 | 54 | 0 | 1.961% | — |
| loss-r05 | yes | 1200/1200 | 15.220 | 12.099 | 41.694 | 137.965 | 139.538 | 100% | 100% | 1338 | 1330 | 62 | 64 | 0 | 2.256% | — |
| reorder-r01 | yes | 1200/1200 | 32.090 | 30.887 | 59.329 | 73.596 | 124.008 | 100% | 100% | 1313 | 1300 | 9 | 9 | 0 | 0.000% | — |
| reorder-r02 | yes | 1200/1200 | 31.934 | 31.002 | 59.580 | 73.343 | 122.380 | 100% | 100% | 1320 | 1300 | 12 | 16 | 0 | 0.000% | — |
| reorder-r03 | yes | 1200/1200 | 32.552 | 31.172 | 60.475 | 74.678 | 121.738 | 100% | 100% | 1304 | 1300 | 0 | 0 | 0 | 0.000% | — |
| reorder-r04 | yes | 1200/1200 | 30.260 | 29.881 | 59.972 | 72.769 | 134.252 | 100% | 100% | 1317 | 1300 | 13 | 14 | 0 | 0.000% | — |
| reorder-r05 | no | 1200/1200 | 92.829 | 32.417 | 580.527 | 833.306 | 300.871 | 100% | 100% | 1313 | 1302 | 10 | 11 | 0 | 0.154% | latency, longest gap |
| combined-r01 | yes | 1200/1200 | 40.650 | 27.941 | 130.250 | 226.347 | 246.557 | 100% | 100% | 1377 | 1324 | 81 | 97 | 0 | 1.813% | — |
| combined-r02 | no | 1199/1200 | 53.905 | 32.046 | 200.573 | 344.118 | 246.757 | 100% | 100% | 1943 | 1338 | 474 | 373 | 306 | 2.915% | latency |
| combined-r03 | no | 1193/1200 | 52.320 | 32.498 | 170.564 | 310.924 | 646.801 | 100% | 100% | 1969 | 1363 | 520 | 429 | 307 | 5.136% | delivery, latency, longest gap |
| combined-r04 | yes | 1200/1200 | 44.777 | 32.311 | 138.083 | 231.135 | 245.747 | 100% | 100% | 1344 | 1323 | 55 | 63 | 0 | 1.738% | — |
| combined-r05 | no | 1200/1200 | 59.832 | 32.720 | 228.382 | 390.078 | 279.160 | 100% | 100% | 1370 | 1330 | 81 | 96 | 0 | 2.256% | latency, longest gap |

## Interpretation

- The 20 Hz, one-item, no-FEC boundary is viable under zero loss, 1% iid
  loss, and four of five reorder repetitions.
- Mapping novelty and provider-confirmed future hit were 100% in every cell.
  The failure is therefore not evidence that the prefetcher issued mostly
  useless future Interests.
- The combined profile is unstable: two cells exceeded latency/gap gates and
  one also lost seven samples. Large Mapping/Retry/Nack counts in r02/r03
  identify control/fetch churn under the joint impairment.
- This evidence supports only the passing treatments. It does not establish a
  positive telemetry-family or cross-application generality claim.

## Integrity

- campaign summary SHA-256:
  `01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738`;
- campaign manifest SHA-256:
  `5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517`;
- campaign CSV SHA-256:
  `c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96`.

The formal result is frozen and `rerunAllowed=false`.
