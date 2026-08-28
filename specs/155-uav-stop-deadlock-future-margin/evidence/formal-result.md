# Formal Result

Frozen root:
`results/spec155-uav-stop-deadlock-future-margin-formal-20260726T174654Z`

Status: **FAIL — 2/6 accepted**

| fps | achieved | delivery | p50 ms | p99 ms | future hit | retry % | timeout % | result |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 10 | 9.992 | 99.923% | 99.034 | 880.146 | 98.942% | 0.659% | 0.659% | PASS |
| 20 | 20.003 | 99.925% | 49.640 | 684.498 | 98.140% | 1.814% | 1.814% | PASS |
| 30 | 29.999 | 99.885% | 32.241 | 586.922 | 98.072% | 2.283% | 2.287% | FAIL |
| 40 | 40.007 | 99.904% | 25.325 | 567.107 | 97.261% | 3.220% | 3.236% | FAIL |
| 50 | 50.001 | 99.914% | 20.273 | 531.168 | 97.119% | 3.599% | 3.650% | FAIL |
| 60 | 60.002 | 99.915% | 17.102 | 518.564 | 96.491% | 4.076% | 4.255% | FAIL |

The stop deadlock is closed: every cell emitted the terminal UAV marker and
exited normally. The generic 25% reserve improved the former 50-fps future-hit
and timeout result, but it did not meet the predeclared zero-loss efficiency
limit at 30–60 fps. No threshold was changed after freeze.
