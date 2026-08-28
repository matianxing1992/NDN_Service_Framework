# Implementation Plan: Three-Provider Mobility Baselines

## Technical Context

- **Runtime**: Python 3 / grpcio 1.70, C++17 / ndn-cxx, MiniNDN-WiFi
- **Primary Harness**: `Experiments/WifiRouterMobilityReliability.py`
- **gRPC Touchpoints**: `Experiments/gRPC/greeter_failover_client.py`,
  `Experiments/gRPC/greeter_server.py`
- **NSC Touchpoints**: `Experiments/NDN_NSC/consumer.cpp`
- **Validation**: focused local tests, two sub-10-second MiniNDN smokes, six
  unique formal cells
- **Output**: one unique `results/wifi_router_mobility_three_provider_*`
  directory with immutable per-cell logs/traces and campaign aggregates

## Constitution Check

- CodeGraph was used before source exploration and edits.
- Network/performance behavior is validated in MiniNDN, not host NFD.
- The measured window remains 60 seconds.
- Tasks are cohesive behavioral outcomes, not edit/test bookkeeping.
- Existing unrelated worktree modifications and active Spec 168 are preserved.

## Architecture

### gRPC baseline

The failover client owns three prewarmed channels and a timestamped health
table. Probe tasks update that table; each logical request rotates its initial
Provider, prefers fresh healthy Providers, and explicitly attempts each Provider
at most once under the global deadline. The final structured line is the sole
harness parsing contract.

### NSC baseline

The consumer owns only request-scoped attempt state. Notification, input and
result names retain one logical run/request identity and a Provider-specific
prefix. Timeout/Nack moves to the next Provider; attempt generation fences late
callbacks. There is no background availability table.

### Harness

The harness generates one deterministic schedule per range and applies it to
all three processes for each baseline. Formal execution is fail-fast at the
campaign level: a terminal failed cell is retained and no cell is automatically
repeated. Aggregates preserve success, latency, attempts, failovers and control
operations without inventing cross-protocol message equivalence.

## Data and State

See [data-model.md](data-model.md) and
[experiment-contract.md](contracts/experiment-contract.md).

## Rollback

The new gRPC client is additive. Legacy one-endpoint gRPC and NSC CLIs remain
available. Reverting the harness to its prior command path requires no protocol
migration and must not delete retained campaign evidence.

## Evidence Boundary

The formal output can repair endpoint-count and recovery-policy fairness for
NSC/gRPC. It cannot make the frozen NDNSF row trace-paired, supply missing
NDNSF latency/traffic evidence, or resolve the historical FirstResponding versus
AllSelected provenance conflict.
