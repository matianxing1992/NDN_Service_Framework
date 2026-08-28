# Formal Three-Provider Mobility Baseline Evidence

## Frozen run

- Campaign: `mobility-ab0aa1b8a49f4a7b9e4fa42c566704af`
- Output: `results/wifi_router_mobility_three_provider_20260804T035100Z`
- Created: `2026-08-04T03:51:44.737569Z`
- Git HEAD/tree: `f60385a23a6845b094233f34e32ad283664cbbab` / `6e7ce5559418ee609ab5848fea7a760e435a9bb1`
- Systems: `grpc,nsc`; Providers: `ucla,wustl,uiuc`; ranges: `100,150,200 m`
- Per cell: `60 s`, `5 RPS`, `5 ms` service delay, `5 s` global deadline, `200 ms` attempt timeout, seed `20`, traffic at gate `+2 s`.
- Exactly one execution per cell, zero driver retries, and no NDNSF cell.

## Results

| System | Range | Success | Attempts | Failovers | p50 ms | p95 ms | p99 ms | Defined messages |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| gRPC-HC-3 | 100 m | 270/300 (90.00%) | 361 | 61 | 7.339 | 8.061 | 10.301 | 2,506 |
| NSC-3 | 100 m | 268/300 (89.33%) | 501 | 201 | 13.138 | 415.522 | 420.102 | 1,841 |
| gRPC-HC-3 | 150 m | 300/300 (100.00%) | 302 | 2 | 7.709 | 9.005 | 13.023 | 2,382 |
| NSC-3 | 150 m | 300/300 (100.00%) | 334 | 34 | 11.491 | 213.861 | 414.913 | 1,834 |
| gRPC-HC-3 | 200 m | 300/300 (100.00%) | 300 | 0 | 7.491 | 8.500 | 10.618 | 2,382 |
| NSC-3 | 200 m | 300/300 (100.00%) | 300 | 0 | 11.454 | 14.505 | 19.496 | 1,800 |

Latency percentiles describe successful logical requests. Message totals use the declared system-specific definitions: gRPC counts each measured service/health RPC request plus its terminal event; NSC counts accepted or sent application-stage Interest/Data events and excludes late events and wire retransmissions. The totals expose overhead but are not byte-equivalent packet counts.

## Closure checks

- Two-cell MiniNDN smoke passed with real failover in both systems at `results/wifi_router_mobility_three_provider_smoke_20260804T034830Z`.
- Formal audit passed: six manifests, six terminal receipts, six run rows, six cell rows, 300 logical requests per cell, shared trace hash per range, unchanged source/binary hashes, no NDNSF cell, and zero retry.
- All cell evidence manifests and gRPC server-owned Stats snapshots passed semantic and hash validation.
- Static gates passed: 18 gRPC tests, 3 NSC tests, 8 mobility-harness contract tests, NSC build/self-test, Python compilation, strict preflight, and `git diff --check`.

This is a one-shot descriptive campaign. It supports the proposal comparison but does not provide variance estimates or statistical significance.
