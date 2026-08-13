# Single-active deadline pilot result (2026-08-06)

The registered three-seed campaign is retained at
[`results/single_active_deadline_advantage_pilot_20260806`](../../../results/single_active_deadline_advantage_pilot_20260806/).
Each seed replays one trace with exactly one reachable Provider at every
epoch, `block_network=true`, four Providers, 5 requests/s for 60 seconds, and
a common 1,500 ms logical deadline. NDNSF uses `first-responding`; gRPC is
strict sequential `gRPC-SEQ-4`; NSC is `NSC-4`. The gRPC/NSC attempt timeout is
500 ms and NDNSF ACK collection is 500 ms. No client receives the availability
trace.

## Provider-registration boundary

All cells start four service processes to keep capacity matched. The client
configuration is deliberately different: NDNSF has no static Provider endpoint
list and publishes the generic request through the NDN namespace after its
normal controller permission/token bootstrap. gRPC receives four static
`--target name=host:port` entries, and NSC receives four static Provider
prefixes. “No NDNSF registration” means no client endpoint registration; it
does not bypass NDNSF identity, permission, or token checks. This contract is
recorded in `registration.json`, each `seed-*/seed-manifest.json`, and every
`seed-*/cells/*/cell-manifest.json`.

## Result

| System | Success | Success rate | Attempts / failovers |
|---|---:|---:|---:|
| NDNSF | 261/900 | 29.00% | 261 Provider executions |
| gRPC-SEQ-4 | 584/900 | 64.89% | 2,214 / 1,314 |
| NSC-4 | 397/900 | 44.11% | 2,275 / 1,375 |

Seed-level NDNSF-minus-baseline success differences were −47.67, −52.00, and
−8.00 percentage points against gRPC, and −28.00, −23.33, and +6.00
percentage points against NSC. The deterministic seed bootstrap intervals
were [−52.00, −8.00] pp for gRPC and [−28.00, +6.00] pp for NSC. The machine
verdict is `NO_DEMONSTRATED_ADVANTAGE`.

This result is deliberately narrow. It rejects a success-rate advantage claim
for the strict single-active, packet-blocked, 1.5-second-deadline condition;
it does not reject NDNSF's API or its separate multi-Provider work-efficiency
mechanism result under redundant coverage. NDNSF latency p95 is not emitted by
the current user summary and is not imputed here.

## Integrity checks

- 9/9 cells completed; every cell produced exactly 300 requests.
- All three cells for each seed share the seed trace SHA-256; all nine cell
  manifests match the registered Provider-registration contract.
- `registration.json` records the harness/wrapper hashes and the disk
  preflight. `aggregate.json` is the machine-readable verdict.
- No setup retry or automatic rerun was used.
