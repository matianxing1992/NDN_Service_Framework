# Boundary-safe opportunity holdout registration

This registration is frozen before execution. It confirms or rejects the
conditional end-to-end switching-latency claim; it does not replace the
unconditional 50/100/150 m results.

## Matrix

- One AP at `(200,200)` in the 400 m x 400 m field.
- Coverage 100 m, Provider speed 2 m/s, RandomWaypoint burn-in 300 s.
- New mobility seeds 72--81; systems NDNSF, sequential gRPC-4, and NSC-4.
- 60 seconds, 5 RPS, 300 requests/cell, 30 cells total.
- 1-second attempt/ACK timeout, 5-second global deadline, admission disabled.
- gRPC health routing disabled; NDNSF FirstResponding plus bounded Response
  reselection.
- Shared request phase 4.05 s. With 100 ms trace updates and 200 ms requests,
  this places publication midway between gate epochs instead of on them.
- No automatic retry. Any failed terminal cell stops the campaign and remains
  evidence.

## Actual-state pairing rule

Each client records the actual monotonic publication time for every request.
The analyzer reconstructs the applied four-Provider gate state from that cell's
`mobility_trace.csv`. A request is included in the conditional comparison only
when all three paired cells have complete gate state and agree that the request
is `SWITCH_REQUIRED`. Non-atomic gate observations and cross-cell disagreements
are retained but excluded.

The mobility seed is the inference unit. For each seed the analyzer computes
successful end-to-end p95 latency within the paired `SWITCH_REQUIRED` rows. The
claim passes only when fixed-seed, 20,000-replicate paired bootstrap 95%
intervals for NDNSF minus gRPC and NDNSF minus NSC both have upper bounds below
zero. A failed gate is retained as a negative result.

## Registered command

```bash
sudo -n env \
  NDNSF_MOBILITY_BUILD_DIR=/home/tianxing/NDN/ndn-service-framework/build-new-svs-20260808 \
  NDNSF_MOBILITY_RUNTIME_LIB_DIR=/tmp/ndn-svs-current-baseline-build-20260808 \
  python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/spec171-opportunity-holdout-100m-2ms-seeds72-81-20260809 \
  --ranges 100 --speeds 2 \
  --seeds 72,73,74,75,76,77,78,79,80,81 \
  --systems ndnsf,grpc,nsc \
  --mobility-warmup-s 300 --ndnsf-response-retry \
  --grpc-health-oracle disabled --traffic-start-delay-s 4.05
```

## Frozen identities

| Artifact | SHA-256 |
|---|---|
| Mobility harness | `34511d610e78c5e8c57d7f1e3aaab2e920e65e7818eda4c7504fb82ac5b74d13` |
| Campaign wrapper | `eacf227f179df4e880c420948ad7d0c457d5f56a31df751d9e40ff703c6de0e4` |
| gRPC client | `5bc4be3eb260ee3a4d683a8b1507b7c93de3853b7f286a3d886c029f00d72e8a` |
| NSC source | `4200f117557193d7db5ca9fb26b6eaee4362a8f79b01453f8d88c01fb1f21d8b` |
| NSC binary | `91330db259fa8b19acf965089b006b7ae71d740b6f420f8aac4dedd17a42776d` |
| NDNSF user source | `5aaf086d459f7bdde03a15f7bd869736533780d30ce3299cdaf832e557342f6a` |
| NDNSF user binary | `426817d86e1840f5a59a6bab796e9b04234d3291ce634375c7a05e7f41b321e2` |
| NDNSF library | `23c1018b55b70b070111ae25dbc819cd85092029f2fcbb4af0e4d8dd624b7c99` |
| NDN-SVS Experimental runtime | `b16760781518854e4bfe29987b06eda82952c67936f3be3efe23053d8a1f2990` |
| Registered analyzer | `6a3404a21926aff8dc32b9e1118aa8c512b1d653215666f7c2741dee9c6ab7b9` |

Registration preflight recorded 26.51 GiB free. Source files are frozen by
content hash because the current integration worktree is intentionally not
rewritten or committed as part of experiment registration.
