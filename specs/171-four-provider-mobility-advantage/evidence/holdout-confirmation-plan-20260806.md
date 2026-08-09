# Confirmatory gRPC comparison holdout plan (2026-08-06)

## Purpose

The exploratory corrected NDNSF summaries for the 50 m / 2 m/s condition were
launched with a 2 s trace-relative traffic start, while the retained paired
gRPC/NSC cells used 4 s. They share trace files but not the same measurement
window and therefore cannot be used as final paired publication evidence.

This holdout is registered before inspecting its traces or results. It uses
the current `single_ap_range_speed_pilot.py` wrapper, which passes the same
4 s trace-relative measurement start to every system and rejects a cell whose
reported traffic phase is outside the 50 ms tolerance.

## Frozen configuration

| Field | Value |
|---|---|
| topology | one physical AP, four Providers |
| coverage / speed | 50 m / 2 m/s |
| trace profile | `random-waypoint` |
| seeds | 43, 44, 45, 46, 47 (new holdout seeds) |
| workload | 5 RPS, 300 logical requests, 60 s measured window |
| trace-relative start | 4 s |
| attempt / ACK timeout | 1 s / 1 s |
| global deadline | 5 s |
| admission control | explicitly disabled |
| gRPC health routing | disabled |
| systems | NDNSF `FirstResponding`, `gRPC-SEQ-4`, diagnostic fixed-endpoint `gRPC-1` |
| network gate | `block_network=true` |

The exact command is:

```bash
python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-grpc-holdout-50m2ms-20260806 \
  --ranges 50 --speeds 2 --seeds 43,44,45,46,47 \
  --systems ndnsf,grpc,grpc-single \
  --attempt-timeout-ms 1000 --ack-timeout-ms 1000 \
  --global-deadline-ms 5000
```

No failed cell is automatically rerun. A setup failure is retained with its
terminal reason and does not count as a successful repetition.

## Claim gate

The publication analysis will report, per seed and pooled:

- logical success rate and deadline misses;
- successful-response mean, p50, p95, and p99 latency;
- sequential attempts/failovers or NDNSF Provider executions;
- all-unreachable, at-least-one, and at-least-two coverage fractions during
  the actual 60 s measurement window;
- paired run-level bootstrap intervals and source/trace hashes.

The conditional claim is admitted only if all three conditions hold:

1. NDNSF minus `gRPC-1` success has a positive paired 95% lower bound;
2. NDNSF minus `gRPC-SEQ-4` success has a paired 95% lower bound of at least
   -5 percentage points (success non-inferiority);
3. The paired 95% upper bounds for NDNSF / `gRPC-SEQ-4` successful-response
   mean-latency and p95-latency ratios are both below 1.0.

If any condition fails, the result remains a descriptive figure and the paper
must say that this holdout did not confirm the conditional claim.

## Figure contract

The generator will emit a CSV, JSON summary, and SVG/PDF/PNG figure. The main
figure will show the fixed-endpoint control separately from the fair sequential
baseline, draw all five seed points, and annotate that latency is measured on
successful responses. No old 2 s/4 s mixed-phase summaries or the timing-audit
hold aggregate may be pooled into the confirmatory figure.
