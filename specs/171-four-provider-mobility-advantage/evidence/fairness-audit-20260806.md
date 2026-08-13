# Fairness Audit and Corrected Mobility Pilots (2026-08-06)

## Decision

The first fair `block_network=true` + `gRPC-PAR-4` three-seed pilot did **not**
support an NDNSF advantage over parallel first-success gRPC. The corrected
serial pilot below also did **not** support an NDNSF success-rate advantage.
Consequently, no full repetition is authorized, and the paper must not claim a
mobility reliability win for NDNSF over either baseline. The defensible
boundary is a neutral/negative result for this single-active coverage-gated
workload, with NDNSF's multi-provider security and selection mechanisms kept as
architectural contributions rather than an empirically superior availability
claim.

The corrected pilot is retained at
[`results/four_provider_parallel_block_fair_pilot_20260806`](../../../results/four_provider_parallel_block_fair_pilot_20260806/)
with the compact aggregate in
[`aggregate.json`](../../../results/four_provider_parallel_block_fair_pilot_20260806/aggregate.json).

The corrected serial pilot is retained at
[`results/four_provider_serial_block_fair_pilot_20260806`](../../../results/four_provider_serial_block_fair_pilot_20260806/)
with its compact aggregate in
[`aggregate.json`](../../../results/four_provider_serial_block_fair_pilot_20260806/aggregate.json).

## Fairness defect found and fixed

The earlier `run_ndnsf()` path used a hard-coded two-second user delay while
gRPC/NSC used the registered four-second measurement barrier. That made the
earlier full parallel result unsuitable as final fair evidence. The harness
now starts all clients from the same absolute monotonic target, records actual
start/lateness and trace source, and rejects a cell with a mismatched trace or
measurement phase. The C++ user accepts the monotonic-millisecond target as a
64-bit value; the correct `App_WifiMobilityUser` target was rebuilt with the
current Boost 1.71/libdl toolchain.

## Corrected pilot protocol

- Three seeds: 20, 21, 22; conditions `moderate` and `stale-health`.
- Four providers, `single-active-handoff`, 300 logical requests, 60-second
  measured window, 5 RPS, 5 ms service delay.
- `block_network=true`; each cell retains gate counters and the same replay
  trace hash is used for NDNSF, gRPC-PAR-4, and NSC-4.
- NDNSF uses `first-responding`. gRPC-PAR-4 sends one concurrent RPC to all
  four providers and accepts the first successful response; its implementation
  disables health routing. NSC-4 remains sequential.
- All accepted measurement starts were within the 50 ms tolerance. NDNSF
  lateness was 0.28--2.44 ms (`moderate`) and 3.20--9.84 ms (`stale-health`).

## Results

| Condition | NDNSF | gRPC-PAR-4 | NSC-4 | NDNSF minus gRPC-PAR-4 |
|---|---:|---:|---:|---:|
| moderate (900 requests) | 177/900 (19.67%) | 588/900 (65.33%) | 535/900 (59.44%) | −45.67 pp mean; −53.33 to −38.33 pp paired range |
| stale-health (900 requests) | 91/900 (10.11%) | 499/900 (55.44%) | 301/900 (33.44%) | −45.33 pp mean; −47.00 to −43.67 pp bootstrap range |

The parallel baseline issued exactly four attempts per logical request. Its
median server duplicate-execution cost was 27 extra executions/request in
`moderate` and 14 in `stale-health`; median cancellations were 590 and 492.
Those costs remain an explicit limitation of this diagnostic, but they do not
reverse the measured success-rate result.

## Corrected serial pilot

The follow-up pilot used the same three seeds and conditions, but compared
NDNSF with strict sequential `gRPC-SEQ-4` (`--grpc-no-health-routing`) and
`NSC-4`. Every one of the 18 system cells passed validation: 300 logical
requests, 60-second measured window, per-campaign shared replay trace, signed
measurement-start lateness within 50 ms, and `block_network=true` gate
evidence. No setup retry or incomplete cell occurred.

| Condition | NDNSF | gRPC-SEQ-4 | NSC-4 | NDNSF minus gRPC-SEQ-4 |
|---|---:|---:|---:|---:|
| moderate (900 requests) | 158/900 (17.56%) | 567/900 (63.00%) | 540/900 (60.00%) | −45.44 pp mean; −57.67 to −37.33 pp paired range |
| stale-health (900 requests) | 92/900 (10.22%) | 343/900 (38.11%) | 399/900 (44.33%) | −27.89 pp mean; −33.33 to −22.67 pp paired range |

The pilot aggregate verdict is `NO_DEMONSTRATED_ADVANTAGE`. The driver’s
accounting gate passed in both conditions, but this is not a performance win:
NDNSF did not preserve terminal success under the single-active handoff trace,
while both sequential baselines completed substantially more requests through
their explicit retry/failover loops. This result is the reason to stop before
full repetition, not to reinterpret the pilot as evidence for the original
serial claim.

## Registered single-active deadline follow-up

To make the deadline mechanism explicit, a separate three-seed pilot (seeds
30--32) fixed one reachable Provider per epoch, `block_network=true`, a
1,500 ms global deadline, and 500 ms gRPC/NSC attempt timeouts. It compared
NDNSF `first-responding` with strict `gRPC-SEQ-4` and `NSC-4`; all nine cells
shared their seed trace and completed 300 requests. The result remained
negative: NDNSF completed 261/900 (29.00%), gRPC-SEQ-4 584/900 (64.89%), and
NSC-4 397/900 (44.11%). The paired NDNSF-minus-gRPC differences were −47.67,
−52.00, and −8.00 percentage points; versus NSC they were −28.00, −23.33,
and +6.00 points. The registered result is retained in
[`single-active-deadline-pilot-result-20260806.md`](single-active-deadline-pilot-result-20260806.md)
and machine-readable output under
[`results/single_active_deadline_advantage_pilot_20260806`](../../../results/single_active_deadline_advantage_pilot_20260806/).

This follow-up also records the client registration boundary: NDNSF has no
static Provider endpoint list and uses runtime NDN namespace forwarding plus
normal permission/token bootstrap; gRPC and NSC require four preconfigured
endpoint/prefix entries. The processes and service capacity were nevertheless
matched across cells. This distinction is an API/control-plane property, not
evidence of a mobility success-rate advantage.

## Bounded forced-outage smoke diagnostic

The corrected harness smoke was run separately with a five-second measured
window and an outage at 2.4--3.4 s. The baseline clients each completed 25/25
with one observed failover; NDNSF completed 23/25, so the strict all-success
smoke acceptance correctly failed. This diagnostic is retained at
[`results/four_provider_serial_smoke_forced_outage_20260806`](../../../results/four_provider_serial_smoke_forced_outage_20260806/)
but is excluded from both pilot aggregates and is not a claim-sized result.

## Supplementary redundant-coverage pilot

Because `single-active-handoff` deliberately leaves only one provider reachable
in each epoch, a separate pilot checked whether the same conclusion held when
the mobility trace actually provided redundant coverage. This is a
supplementary condition, not a replacement for the registered SC-005 claim.

- Seeds 20, 21, and 22; conditions `moderate` and `stale-health`.
- `random-waypoint`, four providers, multi-AP layout, `block_network=true`.
- 300 logical requests per system cell, 60-second measured window, 5 RPS.
- NDNSF used `first-responding`; gRPC used strict sequential `gRPC-SEQ-4`
  with health routing disabled; NSC-4 was the sequential baseline.
- All 18 cells passed. Each system used the same per-campaign replay trace,
  and the signed measurement-start gate remained within tolerance.

| Condition | NDNSF | gRPC-SEQ-4 | NSC-4 | NDNSF median attempts/request |
|---|---:|---:|---:|---:|
| moderate (900 requests) | 817/900 (90.78%) | 861/900 (95.67%) | 861/900 (95.67%) | 0.91 vs 1.86 baseline minimum |
| stale-health (900 requests) | 544/900 (60.44%) | 752/900 (83.56%) | 725/900 (80.56%) | 0.61 vs 1.89 baseline minimum |

The paired NDNSF-minus-gRPC success differences were −4.89 percentage points
in `moderate` (bootstrap interval −9.00 to −0.33 pp) and −23.11 pp in
`stale-health` (−35.00 to −15.67 pp). The driver therefore recorded
`claim_verdict=NO_DEMONSTRATED_ADVANTAGE` and
`supplementary_verdict=NO_DEMONSTRATED_REDUNDANT_COVERAGE_ADVANTAGE`.
NDNSF did use fewer attempts/provider executions, but that control-cost result
did not produce a terminal-success advantage and should not be presented as a
reliability win.

The traces did contain real redundant coverage. The fraction of epochs with at
least two providers in range was 88.47%, 76.09%, and 77.78% for moderate seeds
20--22, and 69.43%, 30.72%, and 65.06% for stale-health seeds 20--22. This
supports retaining a separately labelled redundant-coverage diagnostic in the
paper, while keeping the formal harsh-condition conclusion negative/neutral.
The compact evidence is retained at
[`results/four_provider_randomwaypoint_redundant_pilot_20260806`](../../../results/four_provider_randomwaypoint_redundant_pilot_20260806/)
with the aggregate in
[`aggregate.json`](../../../results/four_provider_randomwaypoint_redundant_pilot_20260806/aggregate.json)
and the protocol/interpretation in its `README.md`.

## Capacity-matched multiprovider work-efficiency pilot

The negative mobility-success result does not test whether NDNSF avoids
duplicating provider work when several providers are simultaneously reachable.
A bounded three-seed pilot was therefore run as a separate mechanism-level
test, without changing or replacing any prior result. It used `random-waypoint`
multi-AP coverage, `block_network=true`, 200 m range, 8 m/s, 250 ms service
time, 5 RPS, 300 logical requests/cell, and the same four service workers for
NDNSF and gRPC. The three systems replayed the same trace in each seed; all
four-provider coverage was 1.0000, 0.9911, and 0.9808, with no all-unreachable
epoch. The signed measurement-start lateness was 2.5, 8.8, and 0.5 ms for
NDNSF, below the 50 ms gate.

| Seed | NDNSF success / provider executions | gRPC-PAR-4 success / server executions | gRPC extra executions | NSC-4 success / attempts |
|---:|---:|---:|---:|---:|
| 20 | 300/300; 300 | 300/300; 1,200 | 900 | 300/300; 300 |
| 21 | 299/300; 300 | 300/300; 1,199 | 899 | 300/300; 301 |
| 22 | 299/300; 300 | 300/300; 1,194 | 894 | 300/300; 302 |
| **pooled** | **898/900; 900** | **900/900; 3,593** | **2,693** | **900/900; 903** |

The NDNSF-minus-gRPC success differences were 0, −0.33, and −0.33 percentage
points, while the NDNSF/gRPC server-execution ratios were 0.2500, 0.2502, and
0.2513. Every gRPC logical request had multiple server executions; NDNSF had
one provider execution per logical request. The predeclared gate therefore
passes as `NDNSF_MULTIPROVIDER_WORK_EFFICIENCY_ADVANTAGE`. This is deliberately
worded as a work-efficiency/mechanism result: the NDNSF user summary does not
provide a directly comparable p95, and this pilot does not overturn the
formal `single-active-handoff` success-rate verdict.

The compact retained evidence is at
[`results/four_provider_work_efficiency_pilot_20260806`](../../../results/four_provider_work_efficiency_pilot_20260806/),
with the exact controls and acceptance rule in
[`capacity-matched-multiprovider-pilot-plan-20260806.md`](capacity-matched-multiprovider-pilot-plan-20260806.md).
The separate 20 RPS run was a deliberately unclaimed saturation screen: both
NDNSF and gRPC-PAR-4 hit deadline/control-path pressure, so it is not included
in this claim-sized aggregate.

## Independent confirmatory work-efficiency repeat

Because the pilot's work-efficiency endpoint passed, seeds 23--25 were
pre-registered as an independent repeat with exactly the same configuration.
This repeat was kept in a separate campaign root and was not pooled until all
per-seed manifests, trace hashes, capacity settings, and gate results had been
validated. All 9 cells passed. The all-four coverage fractions were 1.0000,
0.9586, and 1.0000; no epoch was all-unreachable, and every measurement-start
lateness was below 6.1 ms.

| Seed | NDNSF success / provider executions | gRPC-PAR-4 success / server executions | gRPC extra executions | NSC-4 success / attempts |
|---:|---:|---:|---:|---:|
| 23 | 300/300; 300 | 300/300; 1,200 | 900 | 300/300; 300 |
| 24 | 300/300; 300 | 300/300; 1,187 | 887 | 300/300; 304 |
| 25 | 300/300; 300 | 300/300; 1,200 | 900 | 300/300; 300 |
| **pooled** | **900/900; 900** | **900/900; 3,587** | **2,687** | **900/900; 904** |

The independent repeat's seed-level success difference is 0 pp for every
seed. NDNSF uses 0.2500--0.2527 of the gRPC server execution work per logical
request, and every gRPC logical request has multiple server executions. The
pre-registered gate therefore passes as
`NDNSF_MULTIPROVIDER_WORK_EFFICIENCY_ADVANTAGE_CONFIRMED`. Across all six
seeds, the pooled result is NDNSF 1,798/1,800 versus gRPC 1,800/1,800, with
1,800 versus 7,180 provider/server executions (ratio 0.2507); the paired
success difference remains within −0.33 percentage points for every seed.

The compact repeat evidence is retained at
[`results/four_provider_work_efficiency_confirmatory_20260806`](../../../results/four_provider_work_efficiency_confirmatory_20260806/)
with the six-seed pooled machine-readable result in
[`combined-six-seed-aggregate.json`](../../../results/four_provider_work_efficiency_confirmatory_20260806/combined-six-seed-aggregate.json)
and the registered protocol in
[`capacity-matched-multiprovider-pilot-plan-20260806.md`](capacity-matched-multiprovider-pilot-plan-20260806.md).
This strengthens the multi-provider work-efficiency mechanism claim only; it
does not upgrade the formal SC-005 mobility-success verdict.

## Setup anomaly and retry provenance

The original stale-health/seed20 directory is retained as a failed setup
record: provider B could not connect to `/run/nfd/wustl.sock` before traffic
started. It was not silently retried or overwritten. A manual, independent
retry directory completed all three cells and is the retained seed20
replacement; `retry-provenance.txt` records that relationship. The retry's
stale-health success rates were NDNSF 32/300, gRPC-PAR-4 168/300, and NSC-4
79/300.

## Historical-result boundary

`parallel-block-full-20260806.md`, the earlier pilot, and the original formal
serial comparison remain historical diagnostic artifacts from the pre-fix
timing regime. They must not be cited as fair final evidence. The corrected
serial pilot supersedes the old serial verdict for this workload and does not
justify a full repetition.

## Validation

- Focused mobility/failover Python suite: 44 passed, 1 existing Matplotlib
  warning (including the signed-lateness and random-waypoint verdict tests).
- `App_WifiMobilityUser` rebuilt successfully; `git diff --check` and Python
  byte-compilation passed.
- Corrected retained pilot: 18/18 system cells passed after one separately
  documented setup retry; no experiment processes remained at close.
- Corrected serial pilot: 18/18 system cells passed with no setup retry; no
  experiment processes remained at close.
- Supplementary random-waypoint pilot: 18/18 system cells passed with no setup
  retry; its aggregate and compact campaign evidence are retained separately
  from the formal SC-005 result.
- Capacity-matched work-efficiency pilot: 9/9 system cells passed across
  seeds 20--22; all three per-seed trace hashes matched across systems, the
  coverage preflight and signed-lateness gates passed, and the retained
  aggregate records the work-efficiency verdict without a latency claim.
- Final focused mobility/failover suite after the worker-capacity harness
  change: 44 passed, 1 existing Matplotlib deprecation warning; no experiment
  processes remained and the root filesystem retained approximately 35 GB.
- Independent confirmatory work-efficiency repeat: 9/9 cells passed across
  seeds 23--25; the pre-registration, trace coverage, capacity-match, and
  paired work-efficiency evidence are retained separately from the exploratory
  pilot, including compact per-cell network-gate counter logs.
