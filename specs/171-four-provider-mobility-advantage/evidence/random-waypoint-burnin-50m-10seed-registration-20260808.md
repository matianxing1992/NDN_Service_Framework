# RandomWaypoint burn-in 50 m 10-seed registration

## Purpose

This bounded extension adds the missing low-coverage condition without
rerunning or modifying the accepted 100/150 m campaign. It tests seeds 62--71
under the finalized reconnect repair and deterministic burn-in. Earlier 50 m
results used different pre-repair or no-burn-in conditions and are diagnostic
only; they will not be pooled with this extension.

## Frozen matrix

- Seeds: `62,63,64,65,66,67,68,69,70,71`.
- AP and field: one AP at `(200,200)` in a `400 m x 400 m` field.
- Coverage radius: `50 m`; Provider speed: `2 m/s`.
- Deterministic simulated mobility burn-in: `300 s`.
- Systems: NDNSF FirstResponding with bounded Response-timeout reselection,
  `gRPC-SEQ-4`, and NSC-4.
- Workload: `5 RPS`, `60 s`, 300 logical requests/cell, `5 ms` service time,
  four workers/Provider, `block_network=true`, admission control disabled.
- Timeouts: `1000 ms` ACK/sequential attempt and `5000 ms` global deadline.
- Health routing: disabled; clients receive no coverage oracle.
- Pairing: the three systems replay one byte-identical trace per seed.
- Failure policy: checkpoint every terminal cell, stop on the first failed
  cell, retain its evidence, and never retry automatically.

## Frozen implementation identity

- Campaign wrapper SHA-256:
  `367b5a7dba6178ff4732f991965529e215ef71583516dfba86e49a4b427d93b5`.
- MiniNDN harness SHA-256:
  `34511d610e78c5e8c57d7f1e3aaab2e920e65e7818eda4c7504fb82ac5b74d13`.
- NDNSF library SHA-256:
  `23c1018b55b70b070111ae25dbc819cd85092029f2fcbb4af0e4d8dd624b7c99`.
- NDN-SVS runtime library SHA-256:
  `b16760781518854e4bfe29987b06eda82952c67936f3be3efe23053d8a1f2990`.
- NDN-SVS source identity: `Experimental@6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.
- Runtime Boost libraries: version `1.71` only.

## Registered command

```bash
sudo -n env \
  NDNSF_MOBILITY_BUILD_DIR=/home/tianxing/NDN/ndn-service-framework/build-new-svs-20260808 \
  NDNSF_MOBILITY_RUNTIME_LIB_DIR=/tmp/ndn-svs-current-baseline-build-20260808 \
  python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/spec171-burnin300-50m-2ms-seeds62-71-20260808 \
  --ranges 50 --speeds 2 \
  --seeds 62,63,64,65,66,67,68,69,70,71 \
  --systems ndnsf,grpc,nsc \
  --mobility-warmup-s 300 --ndnsf-response-retry \
  --grpc-health-oracle disabled
```

## Analysis contract

The mobility seed is the unit of inference. For every system, report all ten
seed-level logical-success rates, their mean, median, sample standard deviation,
and a deterministic seed-bootstrap 95% interval. Report paired seed-level
NDNSF-minus-baseline success intervals, seed-level p95 latency distributions,
retry/execution cost, and the realized zero/one/two-or-more Provider coverage
fractions. A pooled request rate may appear only as a descriptive total beside
the seed distribution. Any positive claim must survive the registered paired
seed-level gate; otherwise retain the negative result.
