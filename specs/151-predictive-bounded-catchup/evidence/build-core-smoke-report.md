# Correctly Linked Impaired Smoke

Verdict: **PASS**

Evidence root:

```text
/tmp/spec151-build-core-smoke.y4y1vi
```

The MiniNDN/UAV application ran for 80 seconds with 30 fps, 8 Mbps, one XOR
repair shard, 1% random loss, 5 ms delay with 2 ms jitter, and 1% reordering.
Both UAV executables used the repository `build/` Core through the frozen
`LD_LIBRARY_PATH` contract.

| Metric | Result |
|---|---:|
| Pushed | 36,493 |
| Delivered | 35,957 |
| Delivery | 98.531% |
| Repair attempts | 1,664 |
| FEC recoveries | 899 |
| Retry | 800 |
| Timeout | 1,829 |
| Nack | 0 |
| Future cursor horizon | 128 |
| Adaptive window / lookahead | 128 / 128 |
| Ready / terminal-gap queue at stop | 0 / 0 |
| Useless Interest ratio | 1.190% |

All 27 analyzer checks passed. The result shows that FEC was active and useful,
but did not replace retry or timeout handling. Closure came from the bounded
future horizon keeping the consumer able to catch up while the ordered drain
reached an empty terminal state.

The immutable cell summary SHA-256 is:

```text
7791309a5a9f6e03aa90a67e610ff3dbbbade40090011f74d17dcbe5a3c081da
```
