# Formal Result

The complete one-shot six-rate campaign ran at:

`results/spec154-uav-stop-process-map-formal-20260726T081213Z`

It is frozen with `status=FAIL` and 2/6 accepted cells. No cell may be replaced,
supplemented, or rerun under Spec 154.

| fps | achieved fps | delivery | E2E p50 ms | E2E p99 ms | future hit | terminal gate |
|---:|---:|---:|---:|---:|---:|---|
| 10 | 9.997 | 99.923% | 99.504 | 830.322 | 98.942% | process exit failed |
| 20 | 20.003 | 99.907% | 49.540 | 667.216 | 98.307% | PASS |
| 30 | 29.999 | 99.914% | 33.613 | 589.793 | 97.746% | process exit failed |
| 40 | 39.999 | 99.908% | 26.707 | 558.838 | 96.265% | process exit failed |
| 50 | 49.970 | 99.876% | 21.409 | 548.609 | 94.789% | future hit failed |
| 60 | 60.019 | 99.903% | 17.581 | 529.290 | 96.679% | PASS |

The data plane itself met the configured-rate, delivery, latency, exact-wire,
zero-Mapping, and bounded-queue gates at every rate. Code inspection traced the
intermittent process-exit failure to a UAV APP lock/join cycle. The 50-fps cell
also recorded 1,707 retries and 1,734 timeouts in zero loss while filling the
entire 128-cursor predictive window. Spec 155 owns both repairs and a wholly new
six-rate confirmation.
