# Registered Single-Active Deadline Pilot (2026-08-06)

## Purpose

This pilot tests one bounded mechanism claim: with four candidate Providers,
at least one Provider reachable at every epoch but exactly one reachable at a
time, and a common request deadline that does not permit a sequential baseline
to complete a full four-Provider sweep, NDNSF `first-responding` may complete
more logical requests than sequential gRPC and NSC failover.

This is not a universal mobility, RF-handoff, latency, or GPU claim. The
topology remains coverage-gated; the client receives no availability trace or
health oracle.

## Frozen configuration

| Item | Registered value |
|---|---:|
| Providers | `ucla`, `wustl`, `uiuc`, `arizona` |
| Trace | `single-active-handoff`; exactly one `in_range=1` row per epoch |
| AP layout / coverage | synthetic `multi-ap` gate, 75 m range |
| Handoff period | 2,000 ms |
| Speed | 8 m/s (trace metadata; active schedule controls reachability) |
| Systems | NDNSF `first-responding`, strict `gRPC-SEQ-4`, `NSC-4` |
| Network gate | `block_network=true`; no client-side availability oracle |
| Service delay / workers | 5 ms / 4 workers per Provider |
| Offered load | 5 requests/s for 60 measured seconds (300/cell) |
| Common request deadline | 1,500 ms |
| NDNSF ACK collection | 500 ms |
| gRPC/NSC attempt timeout | 500 ms |
| gRPC routing | health routing disabled; one RPC at a time |
| Seeds | 30, 31, 32 |
| Measurement barrier | 4 s traffic start, 50 ms maximum lateness |

### Provider-registration contract

The four service processes are started in every cell for matched capacity, but
the client-side discovery contract is deliberately different. NDNSF does not
receive a Provider endpoint list: its user publishes the generic request into
the NDN namespace and relies on runtime forwarding plus the controller's
permission bootstrap. gRPC is given four pre-registered
`--target name=host:port` entries, and NSC is given four pre-registered
Provider prefixes in its consumer command. Thus “no NDNSF registration” means
no client endpoint registration; it does not waive NDNSF's normal identity,
permission, or token checks.

The 500 ms timeout and 1,500 ms deadline are frozen before execution. The
deadline intentionally leaves at most three sequential 500 ms attempts, while
NDNSF can solicit all four authorized Providers within one ACK collection
window. This is a deadline-constrained failover condition, not a claim that
serial baselines are bad in every SLA regime.

## Pairing and validity gates

For each seed, one byte-identical replay trace is used by all three systems.
The trace must have exactly one reachable Provider at every epoch and zero
all-unreachable epochs. Every cell must produce exactly 300 logical requests,
pass the 50 ms measurement-start gate, and finish without setup retry. Any
trace mismatch, missing system, extra reachable Provider, or all-unreachable
epoch invalidates the seed rather than being silently repaired.

The registration contract, trace hash, command, and request-count assertion
are persisted in `registration.json`, each `seed-manifest.json`, and every
cell's `cell-manifest.json`.

## Endpoints and claim gate

Record logical success rate, completion latency percentiles, attempts/failovers,
timeout/Nack counts, and Provider execution counts. The pilot supports a
mechanism claim only if NDNSF's paired success difference is positive against
both sequential baselines in all three seeds and the seed-level bootstrap
95% lower bound is above zero. Otherwise report `NO_DEMONSTRATED_ADVANTAGE`.
