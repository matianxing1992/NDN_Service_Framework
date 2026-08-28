# Formal Result

Frozen root:
`results/spec156-predictive-half-window-formal-20260726T181835Z`

Status: **PASS — 6/6 accepted**

| fps | achieved | segment pps | delivery | mean ms | p50 ms | p95 ms | p99 ms | future hit | retry | timeout |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 9.997 | 240.372 | 99.923% | 99.128 | 98.002 | 114.758 | 128.985 | 99.748% | 0 | 0 |
| 20 | 19.997 | 278.955 | 99.894% | 49.695 | 49.054 | 66.864 | 81.467 | 99.823% | 0 | 0 |
| 30 | 30.003 | 288.974 | 99.906% | 35.529 | 31.300 | 50.290 | 90.696 | 99.705% | 46 | 46 |
| 40 | 39.999 | 295.241 | 99.916% | 27.603 | 23.738 | 40.599 | 79.212 | 99.703% | 40 | 41 |
| 50 | 50.005 | 300.357 | 99.885% | 22.215 | 18.951 | 33.889 | 55.723 | 99.753% | 31 | 36 |
| 60 | 59.998 | 306.196 | 99.903% | 18.949 | 16.186 | 28.858 | 59.556 | 99.746% | 33 | 36 |

All cells used a 64-cursor future horizon. Mapping Interests and Nacks were
zero; ready and terminal-gap queues ended at zero. Useless Interest ratio was
0–0.0814%, and every retry/timeout ratio was below 0.2%, versus the unchanged
2% gate.

The complete machine-readable metrics remain in `campaign-summary.json`; the
human-readable aggregate is `rate-comparison.md`.
