# RandomWaypoint burn-in 10-seed registration

## Purpose

This bounded follow-up tests whether the one-seed mechanism result survives
independent mobility traces. It is a one-AP RandomWaypoint comparison under
SC-005a, not the `single-active-handoff` superiority claim in SC-005.

## Frozen matrix

- Seeds: `62,63,64,65,66,67,68,69,70,71`.
- AP and field: one AP at `(200,200)` in a `400 m x 400 m` field.
- Coverage radii: `100 m` partial-coverage primary and `150 m` higher-coverage
  control.
- Provider speed: `2 m/s`; deterministic simulated mobility burn-in: `300 s`.
- Systems: NDNSF FirstResponding with bounded Response-timeout reselection,
  `gRPC-SEQ-4`, and NSC-4. gRPC's custom application health RPC is disabled.
- Workload: `5 RPS`, `60 s`, `5 ms` service time, four workers per Provider,
  admission control disabled, and `block_network=true`.
- Timeouts: `1000 ms` NDNSF ACK / sequential attempt and `5000 ms` global
  deadline. Traffic begins at the common absolute `4 s` barrier.
- Pairing: one trace is generated per seed/radius and replayed byte-for-byte by
  all three systems. Clients do not receive the coverage trace as an oracle.
- Failure policy: every terminal cell is checkpointed. A failed cell stops the
  campaign and is retained; it is never retried automatically.

## Frozen implementation identity

- Harness SHA-256: `34511d610e78c5e8c57d7f1e3aaab2e920e65e7818eda4c7504fb82ac5b74d13`.
- Wrapper SHA-256: `367b5a7dba6178ff4732f991965529e215ef71583516dfba86e49a4b427d93b5`.
- NDNSF library SHA-256: `23c1018b55b70b070111ae25dbc819cd85092029f2fcbb4af0e4d8dd624b7c99`.
- NDN-SVS: `Experimental@6bb34545b4f89f1f6c265a68c18f1a40ade413eb`;
  runtime library SHA-256
  `b16760781518854e4bfe29987b06eda82952c67936f3be3efe23053d8a1f2990`.
- Runtime Boost libraries resolve exclusively to version `1.71`.

## Registered command

```bash
sudo -n env \
  NDNSF_MOBILITY_BUILD_DIR=/home/tianxing/NDN/ndn-service-framework/build-new-svs-20260808 \
  NDNSF_MOBILITY_RUNTIME_LIB_DIR=/tmp/ndn-svs-current-baseline-build-20260808 \
  python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/spec171-burnin300-100-150m-2ms-seeds62-71-20260808 \
  --ranges 100,150 --speeds 2 \
  --seeds 62,63,64,65,66,67,68,69,70,71 \
  --systems ndnsf,grpc,nsc \
  --mobility-warmup-s 300 --ndnsf-response-retry \
  --grpc-health-oracle disabled
```

## Analysis and claim gate

The mobility seed is the unit of inference; the 300 requests within one cell
are not treated as independent mobility repetitions. For each radius, report
the per-seed logical-success difference, a paired seed-level bootstrap 95%
interval against each sequential baseline, mean/p50/p95/p99 successful-response
latency, attempts/failovers or Provider executions, and the realized reachable
Provider distribution including the all-unreachable fraction.

An NDNSF advantage statement for this RandomWaypoint condition is allowed only
when the SC-005a run-level lower confidence bound is positive against both
sequential baselines. If that gate fails, retain the result and state
`NO_DEMONSTRATED_ADVANTAGE`. A lower p95 with a non-positive success bound may
support only a narrower tail-latency observation, not a success-rate claim.
