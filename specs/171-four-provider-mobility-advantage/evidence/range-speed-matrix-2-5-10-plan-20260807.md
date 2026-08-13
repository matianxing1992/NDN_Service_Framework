# Minimal Range/Speed Matrix Plan: 50/75/100 m × 2/5/10 m/s

## Decision

Build one trace-paired, one-AP/four-Provider matrix at three coverage radii
and three fixed speeds. Keep the existing 50 m / 2 m/s evidence as an anchor
when its manifest and source hashes match the final campaign contract. Do not
pool cells with different timeout, traffic-barrier, admission, health-routing,
or trace-generation settings.

The bounded mobility field is 400 m × 400 m with the AP at (200, 200). The
coverage radius is the packet gate, not the field size. A radius of 50, 75, or
100 m therefore tests different outage/redundancy regimes; it does not mean
that the drone is confined to a circle.

## Frozen common configuration

| Dimension | Value |
|---|---|
| AP layout | one physical AP at the centre of the 400 m × 400 m field |
| Providers | four: `ucla`, `wustl`, `uiuc`, `arizona` |
| Coverage radii | 50, 75, 100 m |
| Fixed speeds | 2, 5, 10 m/s |
| Trace | `random-waypoint`, generated once per `(range, speed, seed)` and replayed byte-for-byte |
| Systems | NDNSF FirstResponding, gRPC-SEQ-4, NSC-4 |
| Network gate | `block_network=true`; no client reachability oracle |
| Workload | 5 RPS, 60 s, 300 logical requests/cell |
| Deadlines | 1 s NDNSF ACK / baseline attempt timeout; 5 s global |
| Policy | admission control disabled; proactive health routing disabled |
| Minimum seed set | 50, 51, 52 for every new cell |

The existing 50 m / 2 m/s follow-up already contains these anchor seeds and
can be reused only after exact manifest/source/trace-contract verification.
The existing 75 m result at 15 m/s and the older 50/100 m × 2/15 m/s matrix
are not substitutes for this matrix.

## Minimum new workload

Run the three multi-Provider systems for the eight conditions not covered by
the 50 m / 2 m/s anchor:

```text
75/2, 100/2,
50/5, 75/5, 100/5,
50/10, 75/10, 100/10
```

This is `8 conditions × 3 systems × 3 seeds = 72 new cells`. Each cell has a
60-second measured window, so the measured workload is 72 minutes before
MiniNDN startup, settle, and cleanup overhead.

Add fixed single-Provider controls only at 75 and 100 m at 2 m/s:
`2 ranges × 2 controls × 3 seeds = 12 cells`. The existing 50 m / 2 m/s
gRPC-1 and NSC-1 controls cover the anchor. Do not repeat fixed controls at
5/10 m/s unless the multi-Provider result is ambiguous; they do not exercise
failover and would add 36 cells to a full five-system matrix.

Thus the minimum extension is **84 new cells**. An operationally simpler,
self-contained campaign may rerun the nine 50 m / 2 m/s anchor cells and the
six anchor controls, becoming 99 cells; this costs about 15 additional
measured minutes but avoids cross-campaign aggregation.

## Execution order

1. Generate and inspect all 27 trace combinations (3 ranges × 3 speeds × 3
   seeds) before starting requests. Retain `at_least_one`, `all-unreachable`,
   and `at_least-two` coverage fractions for every trace.
2. Run the 72 new multi-Provider cells in three bounded campaigns so each
   campaign has one speed setting. Use fresh output roots and no automatic
   reruns.
3. Run the 12 low-speed single-Provider controls separately.
4. Aggregate by `(range, speed)` and compare NDNSF with each sequential
   baseline using the seed as the inference unit. Never treat the 300 requests
   inside one trace as 300 independent mobility replicates.

Example commands after the dry-run registration:

```bash
sudo -E python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-ranges-75-100-speed2-20260807 \
  --ranges 75,100 --speeds 2 --seeds 50,51,52 \
  --systems ndnsf,grpc,nsc

sudo -E python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-ranges-50-75-100-speed5-20260807 \
  --ranges 50,75,100 --speeds 5 --seeds 50,51,52 \
  --systems ndnsf,grpc,nsc

sudo -E python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-ranges-50-75-100-speed10-20260807 \
  --ranges 50,75,100 --speeds 10 --seeds 50,51,52 \
  --systems ndnsf,grpc,nsc

sudo -E python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-single-controls-speed2-20260807 \
  --ranges 75,100 --speeds 2 --seeds 50,51,52 \
  --systems grpc-single,nsc-single
```

## Claim and confirmation gate

The 3-seed matrix is a descriptive range/speed map. Report, per cell,
logical success, paired NDNSF-minus-baseline differences, p50/p95/p99 or mean
successful-response latency, attempts/failovers, Provider executions, and
coverage fractions. A macro-average over the nine `(range, speed)` cells is
secondary and must not replace the per-cell table.

Before running, pre-register one confirmatory cell (recommended: 75 m / 5
m/s as the intermediate coverage/speed condition) and expand that cell to at
least 10 independent mobility seeds if the goal is a paper-level positive
claim. The cell must be chosen before inspecting outcomes; a positive screen
does not justify selecting a different cell after the fact. Apply the existing
SC-005a paired lower-bound and latency requirements. Otherwise label the matrix
`DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY` or `NO_DEMONSTRATED_ADVANTAGE`.

## Evidence boundary

This plan does not reuse the 75 m / 15 m/s single-active or multi-AP pilots as
matched evidence. It also does not claim that geometric coverage area equals
temporal reachability: the trace-derived coverage fractions remain the
qualification metrics.
