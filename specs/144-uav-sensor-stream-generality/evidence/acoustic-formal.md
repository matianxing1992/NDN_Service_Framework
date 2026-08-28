# T008 Acoustic/Audio Formal Evidence

**Campaign**: `results/spec144-uav-sensor-stream-20260724T165132Z`  
**Execution**: 16 declared cells, 16 terminal cells, one invocation each,
zero automatic retries  
**Verdict**: **MEASURED NEGATIVE**

## Treatment Verdicts

| Profile | Accepted / attempted | Required | Exact 95% interval | Verdict |
|---|---:|---:|---:|---|
| zero-loss | 1/1 | 1 | [0.025000, 1.000000] | PASS |
| 1% loss | 0/5 | 4 | [0.000000, 0.521824] | FAIL |
| reorder | 0/5 | 4 | [0.000000, 0.521824] | FAIL |
| combined | 0/5 | 4 | [0.000000, 0.521824] | FAIL |

The acoustic workload does not pass as a family. All impaired cells remain in
their declared denominators and were not rerun.

## Per-Cell Results

Latency is source-ready to complete-block admission on the shared host.
`Future=NA` means no provider-confirmed future-Interest denominator was
available and the gate failed. `Nonprod` excludes protection-only repair Data.

| Cell | Pass | Blocks | Mean ms | p50 | p95 | p99 | Gap ms | Future | Novelty | Map I | Payload I | Retry | Timeout | Nack | Nonprod | Recovered sources | Failed gates |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| zero-loss-r01 | yes | 1500/1500 | 14.331 | 9.815 | 31.518 | 47.826 | 85.796 | 100% | 100% | 1632 | 8124 | 2 | 3 | 0 | 0.000% | 0 | — |
| loss-r01 | no | 1500/1500 | 2846.876 | 2730.843 | 3504.918 | 3606.738 | 319.949 | NA | 100% | 1670 | 8320 | 237 | 237 | 0 | 2.356% | 0 | future hit, latency |
| loss-r02 | no | 1500/1500 | 275.029 | 171.015 | 803.798 | 910.457 | 337.071 | 100% | 100% | 1664 | 8248 | 159 | 237 | 0 | 2.437% | 71 | conservation, latency, longest gap |
| loss-r03 | no | 1500/1500 | 1196.239 | 1467.380 | 1979.619 | 2120.625 | 284.744 | 100% | 100% | 1676 | 8308 | 231 | 251 | 0 | 2.443% | 13 | latency |
| loss-r04 | no | 1500/1500 | 79.294 | 26.965 | 308.984 | 415.131 | 311.567 | 100% | 100% | 1657 | 8202 | 106 | 230 | 0 | 2.463% | 118 | latency |
| loss-r05 | no | 1500/1500 | 117.519 | 67.830 | 360.913 | 455.768 | 261.274 | 100% | 100% | 1662 | 8214 | 126 | 227 | 0 | 2.325% | 92 | conservation, latency |
| reorder-r01 | no | 1500/1500 | 87.033 | 66.539 | 208.122 | 255.302 | 200.296 | 100% | 100% | 1638 | 8132 | 16 | 18 | 0 | 18.569% | 1423 | Interest utility |
| reorder-r02 | no | 1500/1500 | 2275.078 | 2188.879 | 4141.728 | 4181.043 | 208.358 | 100% | 100% | 1641 | 8124 | 10 | 12 | 0 | 4.911% | 317 | latency |
| reorder-r03 | no | 1500/1500 | 935.226 | 361.651 | 3054.615 | 3280.358 | 248.403 | 100% | 100% | 1632 | 8146 | 27 | 27 | 0 | 12.104% | 875 | conservation, Interest utility, latency |
| reorder-r04 | no | 1500/1500 | 748.540 | 233.586 | 2986.397 | 3077.137 | 250.376 | 100% | 100% | 1651 | 8144 | 39 | 43 | 0 | 13.593% | 1004 | Interest utility, latency |
| reorder-r05 | no | 1492/1500 | 2039.676 | 1700.227 | 4957.059 | 5232.901 | 187.208 | 100% | 100% | 2089 | 8136 | 351 | 173 | 344 | 5.383% | 303 | delivery, latency |
| combined-r01 | no | 927/1500 | 4004.429 | 3955.203 | 7895.801 | 8133.893 | 295.319 | NA | 100% | 1805 | 8799 | 2769 | 3703 | 341 | 40.721% | 21 | delivery, future hit, Interest utility, latency |
| combined-r02 | no | 856/1500 | 5091.497 | 5212.822 | 7733.778 | 8044.888 | 324.552 | NA | 100% | 1777 | 8868 | 3064 | 4140 | 357 | 44.632% | 0 | delivery, future hit, Interest utility, latency, longest gap |
| combined-r03 | no | 831/1500 | 4347.104 | 4155.301 | 7635.070 | 7968.096 | 357.472 | NA | 100% | 1744 | 8898 | 3137 | 4273 | 325 | 45.864% | 0 | delivery, future hit, Interest utility, latency, longest gap |
| combined-r04 | no | 662/1500 | 4789.808 | 4758.895 | 7907.720 | 8063.553 | 288.956 | NA | 100% | 1705 | 9221 | 3957 | 5439 | 402 | 57.152% | 0 | delivery, future hit, Interest utility, latency |
| combined-r05 | no | 1215/1500 | 3702.538 | 3413.487 | 7894.182 | 8313.860 | 7788.610 | 100% | 100% | 2004 | 8389 | 1586 | 1848 | 407 | 22.327% | 146 | delivery, Interest utility, latency, longest gap |

## Interest and Recovery Interpretation

- The zero-loss cell delivered all 1,500 blocks with p99 47.826 ms. It fetched
  4,874 source and 3,250 repair items; the unused repairs were correctly
  classified as protection-only rather than application-useful.
- Loss cells delivered all blocks, but none met every latency/future/
  conservation gate. Reorder cells show severe latency variability and up to
  18.569% nonproductive Payload Interests. Combined impairment caused both
  multi-second latency and incomplete delivery.
- Raw `recoveryAttempts`, `recoveryExhaustions`, `recoveredBlocks`,
  `recoveredSources`, and `coreRecoveredSources` remain preserved in each cell.
  The derived `recovery.successRate` is **not valid evidence**: its numerator
  counts recovered source items while its denominator counts recovery
  attempts, and one attempt may recover two sources. For example,
  acoustic-reorder-r01 reports 1,502/1,297 (115.8%). It is therefore marked
  unavailable for the final claim and must be corrected only in a new Spec.
- `interestConservation=false` in loss-r02, loss-r05, and reorder-r03 is a
  genuine fail-closed result: each retained one unresolved terminal Payload
  Interest. No failed gate was waived.

## Integrity

The campaign hashes are identical to the telemetry evidence:

- summary:
  `01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738`;
- manifest:
  `5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517`;
- CSV:
  `c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96`.

The result is frozen and `rerunAllowed=false`.
