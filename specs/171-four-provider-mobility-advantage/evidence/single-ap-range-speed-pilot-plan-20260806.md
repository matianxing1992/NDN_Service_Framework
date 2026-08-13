# Registered Pilot: One AP, Four Providers, Range and Speed

## Decision

The primary mobility screen uses the original one-AP topology with four mobile
Providers. It varies declared coverage radius and deterministic drone speed;
it does not add APs or give the clients a coverage/oracle file. The initial
registered matrix is:

| Dimension | Values |
|---|---|
| AP layout | one physical AP at the harness `single` position |
| Providers | `ucla`, `wustl`, `uiuc`, `arizona` |
| Coverage radius | 50 m, 100 m |
| Speed | 2 m/s, 15 m/s |
| Seeds | 40, 41, 42 |
| Systems | NDNSF, gRPC-SEQ-4, NSC-4 |
| Workload | 5 RPS, 60 s measured window, 300 logical requests |

This is 36 finite cells (2 ranges × 2 speeds × 3 seeds × 3 systems). A
three-AP layout and the 8 m/s middle speed are extensions, not replacements
for this primary screen.

## Baselines and health policy

- NDNSF uses `first-responding` multi-Provider selection and its normal
  permission/token path; the client receives no endpoint list or trace.
- gRPC-SEQ-4 has four statically configured targets, tries one target at a
  time, and uses a 1 s attempt timeout. The primary condition passes
  `--disable-health-routing`.
- NSC-4 has four statically configured Provider prefixes and uses a 1 s
  sequential attempt timeout.
- The wrapper's optional `--grpc-health-oracle enabled` condition uses the
  repository's custom `NDNSFBaseline/Health` application RPC. It is not the
  standard `grpc.health.v1` protocol and is never enabled implicitly.

The gRPC health protocol is an official standard, but a server must register
and serve it and a client must be configured to use health checking. Therefore
the fair default is no proactive oracle because NDNSF and NSC do not receive an
equivalent oracle. If a health-aware condition is run, probe count, health
success, health-directed selections, and probe traffic are reported separately.

## Timing and trace controls

- Common logical deadline: 5 s.
- NDNSF ACK timeout: 1 s.
- gRPC and NSC per-attempt timeout: 1 s.
- gRPC health interval/timeout, when explicitly enabled: 1 s.
- `random-waypoint` traces are generated once per `(seed, range, speed)` and
  replayed byte-for-byte by all three systems.
- `block_network=true` gates packets according to the trace. The trace files
  are retained for analysis only and are not exposed to any client.

The trace report records per-Provider reachability, the fraction of epochs with
at least one reachable Provider, all-unreachable epochs, and epochs with at
least two reachable Providers. These quantities qualify whether a condition
can test failover or only measures outage.

## Measurements and claim boundary

Every cell records logical success/deadline misses, completion p50/p95/p99,
attempts/request, failovers/request, provider handler executions, and terminal
status classes. gRPC additionally records health probes and health-directed
selections when the opt-in oracle is enabled.

The pilot is a registered range/speed sensitivity matrix. It can support an
NDNSF advantage statement only if a paired run-level lower confidence bound is
positive against the corresponding sequential baseline, while also reporting
the all-unreachable fraction, retry/control cost, and latency. A negative or
inconclusive result remains valid evidence and must be reported as such; no
post-hoc range, speed, seed, or health setting may be selected for a positive
claim.

## Execution contract

The wrapper is `Experiments/single_ap_range_speed_pilot.py`. It performs a
20-GiB free-space preflight, writes a registration manifest and source hashes,
refuses a non-empty output root, and stops on the first failed cell without
automatic rerun. Use `--dry-run` to inspect the complete registration before a
formal campaign. Use `--grpc-health-oracle enabled` only for a separately
labelled sensitivity run. The separate smoke profile uses a 2-second forced
UCLA outage only to prove that the 1-second sequential timeout actually
executes a failover; that outage is not part of the formal range/speed traces.
