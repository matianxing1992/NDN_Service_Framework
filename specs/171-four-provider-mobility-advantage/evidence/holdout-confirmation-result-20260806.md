# Confirmatory gRPC comparison holdout result (2026-08-07)

## Outcome

The frozen five-seed holdout completed all 15 cells. Every cell used the same
trace-relative 4 s measurement start and passed the 50 ms phase gate. The
registered SC-008 conditional claim is **not confirmed**:

- NDNSF vs `gRPC-1` success difference: mean `+29.40` percentage points,
  paired 95% interval `[-5.33, +71.40]` percentage points; the lower bound is
  not positive.
- NDNSF vs `gRPC-SEQ-4` success difference: mean `-2.47` percentage points,
  paired 95% interval `[-11.80, +4.40]`; this fails the registered `-5 pp`
  non-inferiority bound.
- NDNSF / `gRPC-SEQ-4` successful-response mean-latency ratio: mean `0.8785`,
  95% interval `[0.4705, 1.2864]`.
- NDNSF / `gRPC-SEQ-4` successful-response p95 ratio: mean `1.7121`, 95%
  interval `[0.0688, 4.1111]`.

Therefore the publication wording must remain descriptive: this condition
does not establish a general NDNSF mobility advantage or a latency advantage
over sequential gRPC retry. The chart is retained as a negative/diagnostic
outcome, not replaced by a favorable rerun.

## Frozen configuration and command

One AP, four Providers, 50 m coverage, 2 m/s, `random-waypoint`, seeds
43--47, 5 RPS, 300 requests per seed, 60 s measured window, trace-relative
start 4 s, 1 s attempt/ACK timeout, 5 s global deadline, `block_network=true`,
admission control disabled, gRPC health routing disabled. NDNSF used
`FirstResponding`; `gRPC-SEQ-4` used sequential one-at-a-time failover;
`gRPC-1` was a fixed-endpoint diagnostic control.

```bash
sudo -n -E python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-grpc-holdout-50m2ms-20260806 \
  --ranges 50 --speeds 2 --seeds 43,44,45,46,47 \
  --systems ndnsf,grpc,grpc-single \
  --attempt-timeout-ms 1000 --ack-timeout-ms 1000 \
  --global-deadline-ms 5000
```

## Pooled descriptive metrics

| System | Success | Mean successful latency | Mean per-seed p95 | Attempts / request |
|---|---:|---:|---:|---:|
| gRPC-1 (diagnostic) | 47.87% | 8.16 ms | 9.27 ms | 1.000 |
| gRPC-SEQ-4 | 79.73% | 140.12 ms | 881.72 ms | 2.113 |
| NDNSF | 77.27% | 112.50 ms | 500.05 ms | 0.773 Provider executions |

The fixed endpoint fails completely for seeds 43 and 44, then succeeds for
72.33--90.67% of requests on seeds 45--47. This supports only the narrow
availability-cost diagnostic interpretation. The sequential baseline has
higher retry/tail cost in several seeds, but the paired confidence bounds do
not support a uniform latency claim.

## Coverage and provenance

Across the actual 60 s measurement windows, the pooled all-unreachable
fraction was `20.83%`, at-least-one-provider coverage `79.17%`, and
at-least-two-provider coverage `58.20%`. Per-seed coverage is preserved in
`holdout-comparison-20260806/analysis.json` and `per-seed.csv`.

| Artifact | SHA-256 |
|---|---|
| `results/.../aggregate.json` | `d9f40684ab2ed31d321cbcdb17b25e41a20f617fb2b0a21b63d5024195b31d2d` |
| `results/.../registration.json` | `f47c98443312730103d11facb0e7f6db8dc4ea470d43f490095d7d748513b0c9` |
| `Experiments/single_ap_range_speed_pilot.py` | `0120f23240f832b2ae85166a3edeee58053e4fb1a48d05d0d5a67ee6bf3b1900` |
| `Experiments/WifiRouterMobilityReliability.py` | `b16945a19596f2b4027df5c483a3517d4b1a1784a43399a6e82158e6aa33251b` |

Generated outputs:

- `holdout-comparison-20260806/analysis.json`
- `holdout-comparison-20260806/per-seed.csv`
- `holdout-comparison-20260806/mobility-comparison.{png,svg,pdf}`
- `holdout-comparison-20260806/README.md`

The old mixed 2 s/4 s exploratory cells remain diagnostic only and are not
pooled into this figure.
