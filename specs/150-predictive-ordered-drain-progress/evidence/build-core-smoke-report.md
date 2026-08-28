# Build-Core Impaired Smoke

Result root:

```text
/tmp/spec150-build-core-smoke.dA5IqJ
```

The MiniNDN processes explicitly loaded the current hashed-compatible
`build/libndn-service-framework.so.0.1.0` through:

```text
LD_LIBRARY_PATH=<repo>/build:<repo>/.local-boost171/lib
```

The 80-second 1% loss / 1% reorder UAV video run exited normally and decoded
video, but the Spec 150 analyzer rejected it:

| Metric | Value |
|---|---:|
| measured window | 76.039 s |
| pushed | 41,866 |
| delivered | 13,280 |
| delivery ratio | 31.720% |
| decoded frame samples | 1,357 |
| AoI mean / p50 / p95 / p99 | 1115.1 / 1090.6 / 1771.0 / 2030.6 ms |
| longest delivery gap | 1248.0 ms |
| Mapping Interests | 0 |
| Payload Interests | 15,142 |
| retry / timeout / Nack | 1,032 / 1,248 / 0 |
| recovery attempts / recoveries | 813 / 38 |
| recovery frontier / group Interests | 48 / 457 |
| useless Interest ratio | 5.325% |
| next deliver cursor | 13,390 |
| ready / terminal-gap queue depth | 0 / 0 |

The ordered-drain and direct-recovery corrections worked: no cursor remained
stuck, no ready queue accumulated, group Interests remained below recovery
attempts, and FEC recovered real source Data. The remaining failure is
throughput/catch-up: the consumer issued only 15,142 Payload Interests while
the provider pushed 41,866 source items.

This smoke is a development gate, not a formal cell.
