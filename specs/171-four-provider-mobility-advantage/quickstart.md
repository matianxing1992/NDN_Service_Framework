# Quickstart: Four-Provider Mobility Advantage

## Prerequisites

- MiniNDN-WiFi and Mininet-WiFi dependencies are installed.
- NDNSF example binaries and the gRPC baseline dependencies are built.
- Run commands from the repository root.

## Execution backend decision

Mobility evidence is local-first: MiniNDN-WiFi, smoke runs, seed expansion,
process repeats, and analysis scripts normally stay on the development host.
TigerCluster is optional and agent-selected; use it only when a registered
experiment genuinely needs unavailable GPU/node capacity or cannot finish
locally within a bounded budget. Do not use it for ordinary reruns or to
replace a failed local gate. Before a remote submission, freeze the exact
source/trace/seed/command identity, run local gates, create a fresh result
directory, and perform a bounded queue/GPU/disk preflight. Never auto-retry or
overwrite a failed campaign. The full rule is in
[`docs/tigercluster-execution-policy.md`](../../docs/tigercluster-execution-policy.md).

## Static gates

```bash
python3 -m py_compile Experiments/WifiRouterMobilityReliability.py \
  Experiments/gRPC/greeter_failover_client.py
python3 -m unittest discover -s tests/python -p 'test_*mobility*' -v
```

## Smoke gate

The implementation SHALL expose a command equivalent to:

```bash
sudo -E python3 Experiments/WifiRouterMobilityReliability.py \
  --profile four-provider-single-ap --smoke --include-ndnsf \
  --ap-layout single --speed-mps 2 \
  --lock-file /tmp/ndnsf-four-provider-mobility-smoke.lock \
  --output-dir results/wifi_router_mobility_four_provider_smoke_<timestamp>
```

The smoke is accepted only when all three systems start, each emits one
terminal summary, at least one baseline failover occurs, and the result is
marked `SMOKE_OK`. It is not performance evidence. `multi-ap` currently means
a deterministic nearest-AP coverage geometry replay over the existing MiniNDN
backhaul; it does not claim physical Wi-Fi association or handoff.
The forced smoke trace keeps UCLA out of coverage from 2.4--4.4 s so the
registered 1-second sequential attempt timeout must observe a real failover.

## Registered range/speed screen

For RandomWaypoint campaigns, apply the registered deterministic burn-in before
trace timestamp zero and retain it in the manifest:

```text
--mobility-warmup-s 300
```

This is simulated trace preparation, not an additional 300 seconds of
MiniNDN wall-clock runtime. Every compared system must replay the same resulting
trace hash.

Inspect the finite registration before starting any MiniNDN workload:

```bash
python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root /tmp/ndnsf-single-ap-range-speed-registration --dry-run
```

The registered pilot then runs the one-AP, four-Provider matrix (50/100 m,
2/15 m/s, seeds 40/41/42) with a 5-second logical deadline, 1-second gRPC/NSC
attempt timeout, and 1-second NDNSF ACK timeout. Its default gRPC condition is
`gRPC-SEQ-4` with no health oracle. The optional custom application health RPC
is enabled only by adding `--grpc-health-oracle enabled`, which creates a
separate sensitivity campaign.

For the strictly paired timeout sensitivity at the known 50 m / 2 m/s
boundary, keep the same trace, seeds, workload, and disabled admission policy
while changing only the timeout condition:

```bash
sudo -E env NDNSF_MOBILITY_NDN_LOG='ndn_service_framework.*=TRACE' \
  python3 Experiments/single_ap_range_speed_pilot.py \
  --output-root results/ndnsf-timeout-sensitivity-2s5s_<timestamp> \
  --ranges 50 --speeds 2 --seeds 40,41,42 \
  --attempt-timeout-ms 2000 --ack-timeout-ms 2000 \
  --global-deadline-ms 5000
```

This is labelled sensitivity evidence, not a replacement for the registered
1 s/5 s result. For `FirstResponding`, `ackTimeoutMs=2 s` does not create a
waiting window: the first successful ACK still triggers selection immediately.
The trace log is used to locate failures at ACK matching, provider selection,
or response observation when those lifecycle markers are enabled.

## Interpretation

Use the generated aggregate table, not raw request counts alone. Check
logical success, deadline failures, attempts/request, failovers/request,
recovery gap, latency percentiles, and Provider execution counts. Do not update
the paper or slides until the claim contract is satisfied. The current
screening numbers are diagnostic only: the four-Provider smoke was 25/25 for
all systems, while one stale-health 25-request run was 25/25 NDNSF, 24/25
gRPC-HC-4, and 22/25 NSC-4. Those counts have no confidence bound.
