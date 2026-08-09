# Single-active handoff screening pilot

Date: 2026-08-06

This is a screening result, not the registered claim campaign. It uses two
complete 60-second stress cells (seeds 20 and 21) to decide whether a formal
multi-seed run is warranted. The third requested seed was stopped before its
first cell after the trace generator was corrected to make the handoff order
seed-dependent; it is not included below.

## Registered stress setup

- Four Providers: `ucla`, `wustl`, `uiuc`, `arizona`; three-AP geometry
  `multi-ap`; declared range 75 m; declared speed 15 m/s.
- `single-active-handoff`, 1.0 s rotation period, one reachable Provider per
  trace epoch; this is coverage-gated availability evidence, not an RF
  association claim.
- Shared workload: 60 s measured window, 5 requests/s, 300 logical requests,
  5 ms service delay, 300 ms global deadline, 100 ms per-attempt timeout,
  1,000 ms health interval, 4 s traffic barrier.
- Primary baselines: `gRPC-SEQ-4` (strict sequential, no proactive health
  routing) and `NSC-4`; NDNSF uses `first-responding`.
- Both completed seeds used the same trace hash
  `510606e3ba85a4ed1481a21cea4655b3897824518f052765ebb4599bef1afede`.
  Harness source hash in those manifests:
  `b2a4719056699832d6e7f30a7a3992ae839662d89b6d8428f206f50f659522aa`.

## Complete cells

| Seed | System | Success | Attempts | Success rate | Attempts/request |
|---:|---|---:|---:|---:|---:|
| 20 | NDNSF | 300/300 | 300 | 100% | 1.00 |
| 20 | gRPC-SEQ-4 | 240/300 | 600 | 80% | 2.00 |
| 20 | NSC-4 | 240/300 | 600 | 80% | 2.00 |
| 21 | NDNSF | 300/300 | 300 | 100% | 1.00 |
| 21 | gRPC-SEQ-4 | 240/300 | 600 | 80% | 2.00 |
| 21 | NSC-4 | 240/300 | 600 | 80% | 2.00 |

The paired point difference is +20 percentage points for NDNSF versus each
sequential baseline in both seeds. This is directionally consistent with the
10-second screen, but `n=2` and the two traces predate the seed-dependent
handoff-order correction, so no mobility advantage claim is admitted.

A separate 10-second `gRPC-HC-4` diagnostic on seed 20 completed 50/50 with
52 attempts, 40 health checks, 13 successful health checks, and 34
health-directed selections. This confirms the intended interpretation: a
health-assisted baseline can largely remove the sequential stale-endpoint
penalty, so it must remain separately labelled rather than silently treated as
the strict sequential baseline.

## Decision

The screening gate passes operationally: NDNSF remains at one attempt per
logical request while each sequential baseline spends two attempts and loses
20% of requests under the registered harsh condition. A formal campaign is
warranted, but it must regenerate all traces with the corrected seed-dependent
handoff order and run the pre-registered moderate and harsh conditions for at
least ten paired seeds. Page 32 and the paper remain unchanged until that gate
completes.

## Reproduction roots

- `results/four_provider_single_active_formal_stress_pilot_v2_20260806/stale-health-seed-20`
- `results/four_provider_single_active_formal_stress_pilot_v2_20260806/stale-health-seed-21`
