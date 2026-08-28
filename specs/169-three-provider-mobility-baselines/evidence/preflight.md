# Preflight Evidence

**Recorded**: 2026-08-04T02:28:30Z  
**Repository HEAD**: `f60385a23a6845b094233f34e32ad283664cbbab`  
**Repository tree**: `6e7ce5559418ee609ab5848fea7a760e435a9bb1`

## Scope and worktree

- The repository already contained extensive unrelated, uncommitted Spec 168
  and NDNSF-DI work. It is preserved and excluded from this feature.
- At preflight, the five mobility/gRPC/NSC source files listed below had no
  worktree modifications.
- `.specify/feature.json` remains owned by active Spec 168 and is not switched
  by this successor experiment.
- No active MiniNDN/mnexec/mobility baseline process was found.
- Noninteractive sudo is available; the workspace filesystem had 21 GiB free.
- grpcio version is 1.70.0; local NSC consumer and producer executables existed.

## Frozen pre-change hashes

```text
04d429f2c46b74a00de7315499b4a821e0275ee11dc13e8a93b45de65cc3cff8  Experiments/WifiRouterMobilityReliability.py
a0fc742243db61db5f358596007f1c6d03dd79a289def8b93df4d96ee0a4ae25  Experiments/gRPC/greeter_client.py
415a51790ce6d5b9d2139a49ef49ffdea69749850f767c381056ca681b5ccbd0  Experiments/gRPC/greeter_server.py
181661efd5d9f2ee15999119acf9cb65cfcb71d7b440a8b79f2c441e700954d4  Experiments/NDN_NSC/consumer.cpp
3b283ccc8fa031191079d37d49904c84892fb85a3c6ba4976cdf154a8de190ad  Experiments/NDN_NSC/producer.cpp
```

`greeter_client.py` is a frozen legacy input and is not an implementation
target; the new baseline uses an additive client.

## Registered formal schedule

Exactly these cells may run once after focused tests and smoke gates pass:

```text
grpc: 100, 150, 200 m
nsc:  100, 150, 200 m
```

Each cell uses 5 RPS, 60 seconds, 5 ms service delay, 5000 ms logical deadline,
200 ms attempt timeout, seed 20 and the same traffic-start offset. NDNSF is not
an admitted system and no formal cell may be automatically retried.

## Evidence limitation

No retained original `mobility_trace.csv`, mobility raw client logs, pcap, or
latency/message counter evidence was found for the frozen NDNSF table. The new
campaign is a retrospective three-Provider baseline remediation, not a paired
replay of the historical NDNSF cell.
