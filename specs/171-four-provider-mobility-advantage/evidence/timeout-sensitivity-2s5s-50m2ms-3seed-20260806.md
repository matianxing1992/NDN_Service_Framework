# Timeout Sensitivity Pilot: 2 s Attempt / 5 s Global

**Status**: `COMPLETED_SENSITIVITY_ONLY`  
**Date**: 2026-08-06  
**Primary result affected**: No. The registered 1 s attempt / 5 s global
condition remains the main result.

## Question

Does the conditional 50 m / 2 m/s NDNSF signal persist when the sequential
gRPC/NSC baselines use a 2 s per-attempt timeout under the same 5 s logical
deadline?

## Configuration

- One physical AP, four Providers (`ucla`, `wustl`, `uiuc`, `arizona`).
- `block_network=true`, `random-waypoint`, coverage range 50 m, speed 2 m/s.
- Seeds 40, 41, and 42; one deterministic trace replayed by all systems per
  seed; 5 RPS; 60 s measured window; 300 logical requests per cell.
- Admission control explicitly disabled.
- NDNSF: `FirstResponding`, `ackTimeoutMs=2000`, request deadline 5000 ms.
- gRPC: strict `gRPC-SEQ-4`, no health routing, attempt timeout 2000 ms,
  global deadline 5000 ms.
- NSC: four static prefixes, attempt timeout 2000 ms, global deadline 5000 ms.
- The NDNSF lifecycle diagnostic used the scoped
  `NDNSF_MOBILITY_NDN_LOG=ndn_service_framework.*=TRACE` environment variable;
  it was not passed to gRPC or NSC.

Command:

```bash
sudo -n -E env NDNSF_MOBILITY_NDN_LOG='ndn_service_framework.*=TRACE' \
  python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-timeout-sensitivity-2s5s-50m2ms-20260806 \
  --ranges 50 --speeds 2 --seeds 40,41,42 \
  --attempt-timeout-ms 2000 --ack-timeout-ms 2000 \
  --global-deadline-ms 5000
```

The complete machine-readable output is in
`results/ndnsf-timeout-sensitivity-2s5s-50m2ms-20260806/` with nine terminal
cells, registration metadata, per-cell manifests, replayed traces, and
`aggregate.json`.

## Paired results

| Seed | NDNSF | gRPC-SEQ-4 | NSC-4 | NDNSF−gRPC | NDNSF−NSC |
|---:|---:|---:|---:|---:|---:|
| 40 | 197/300 (65.67%) | 197/300 (65.67%) | 140/300 (46.67%) | +0.00 pp | +19.00 pp |
| 41 | 229/300 (76.33%) | 229/300 (76.33%) | 221/300 (73.67%) | +0.00 pp | +2.67 pp |
| 42 | 222/300 (74.00%) | 218/300 (72.67%) | 208/300 (69.33%) | +1.33 pp | +4.67 pp |
| **Aggregate** | **648/900 (72.00%)** | **644/900 (71.56%)** | **569/900 (63.22%)** | **+0.44 pp** | **+8.78 pp** |

Retry/control cost over 900 requests:

- NDNSF: 648 Provider executions, 0.720 executions/request, no cross-Provider
  retry in the current FirstResponding path.
- gRPC: 2,218 attempts, 2.464 attempts/request, 1,318 failovers;
  1,384 `UNAVAILABLE` statuses and 190 deadline-exceeded statuses.
- NSC: 1,926 attempts, 2.140 attempts/request, 1,026 failovers, 1,357
  attempt timeouts, and 241 late callbacks.

### Attempt and latency interpretation

The 2 s value is a per-attempt deadline cap, not a mandatory 2 s sleep before
the next Provider. With a 5 s logical deadline, a request whose first three
Providers each consume the full 2 s can start only three attempts. However,
`UNAVAILABLE` failures can return much earlier, so the sequential loop may
reach the fourth configured Provider while global time remains. The retained
gRPC logs show four-attempt requests in 152, 71, and 84 cases for seeds 40,
41, and 42 respectively (307/900 total); no request exceeded the four-provider
limit. This is why the gRPC attempt totals are 903, 652, and 663 rather than a
hard 3-per-request cap.

The reported `mean_ms`, `p50_ms`, and `p95_ms` are successful-response
latencies, not an unconditional latency for every logical request. NDNSF's
per-seed p50/p95 values were 48.57/60.29 ms, 55.39/71.92 ms, and 51.94/63.75
ms. gRPC's p50 values were about 8.5 ms, but its p95 values reached
2,009.7/2,011.5/1,133.8 ms because successful requests sometimes followed
failed attempts. NSC's p95 values were 4,014.7/4,013.7/2,014.0 ms, matching
one or two 2 s attempt timeouts before a later successful Provider. Therefore
the similar NDNSF/gRPC success rates do not imply similar tail latency.

## Lifecycle-stage accounting

The NDNSF trace was grouped by request ID using
`ACK_MATCHED_PENDING_CALL` (the implementation's ACK_MATCHED trace marker),
`PROVIDER_SELECTED`, and `RESPONSE_OBSERVED`:

| Stage | Seed 40 | Seed 41 | Seed 42 | Aggregate |
|---|---:|---:|---:|---:|
| Logical requests | 300 | 300 | 300 | 900 |
| ACK matched | 197 | 229 | 222 | 648 |
| Provider selected | 197 | 229 | 222 | 648 |
| Response observed | 197 | 229 | 222 | 648 |
| `no_selection_published` timeout | 103 | 71 | 78 | 252 |

All observed successful requests traversed ACK → selection → response. The 252
failed requests stopped before ACK matching/selection; no selected-provider
response failure was observed. This is consistent with current
`FirstResponding`: the first successful ACK selects immediately, and a later
response failure would not trigger reselection. Passing `ackTimeoutMs=2 s`
therefore does not create a 2-second ACK collection window or change the
selection semantics.

## Interpretation and claim boundary

The 2 s sensitivity pilot does not strengthen a general NDNSF mobility claim.
At this boundary NDNSF is effectively tied with gRPC (0.44 percentage points
in aggregate; 0.00, 0.00, and 1.33 points by seed). The larger NSC gap is
compatible with a smaller sequential retry budget under a 2 s attempt timeout;
it is not evidence that NDNSF performs response-level mobility recovery.

The NDNSF trace logging was intentionally enabled to locate lifecycle failure
stages and is scoped away from the baselines. Because TRACE logging can add
runtime overhead, these measurements are diagnostic sensitivity evidence and
must not replace the registered 1 s/5 s primary table. The paper/slides remain
unchanged.

**Verdict**: `SENSITIVITY_COMPLETE; NO_CHANGE_TO_PRIMARY_CLAIM`.

To demonstrate recovery after a selected Provider becomes unreachable, the
next experiment must implement and evaluate response-level retry/reselection;
changing only attempt or ACK timeout is insufficient.
