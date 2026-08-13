# Lower-Coverage Retry-Stress Pilot (2026-08-06)

## Purpose

This is a secondary stress experiment for the hypothesis that NDNSF should
benefit when the AP coverage radius is reduced and sequential baselines retry
more often. It does not replace the registered 50/100 m primary matrix or the
previous fair mobility audit.

## Configuration

- One physical AP, four Providers, `random-waypoint` coverage-gated trace.
- Coverage radius: 35 m; speed: 15 m/s; seeds: 40, 41, 42.
- `block_network=true`; 5 requests/s; 60 s measured window; 300 logical
  requests/system/seed; 5 s global deadline.
- NDNSF ACK timeout: 1 s; gRPC and NSC per-Provider attempt timeout: 1 s.
- gRPC health routing disabled; gRPC and NSC use strict sequential failover.
- All three systems replay the same seed-specific trace. This is coverage-gated
  availability evidence, not a physical Wi-Fi association measurement.

Campaign directory:

`/tmp/ndnsf-low-coverage-stress-35m-15ms-20260806-r2`

Artifact hashes:

- `aggregate.json`: `1f8339b4603351a7b556203f227701bd55830d54323469b6afc63fa93cbb77f7`
- `registration.json`: `85607d25ce4c8678c75e34f48c9642aaefed666c3593833993172640e537699e`
- harness: `3d576745fb915f09581a6eedd890f371b505f40dc59a6164e7481e06718870ca`
- pilot wrapper: `95c049fee2260b90f3fc477af6c6056269c3ea071521af4ea66d00064882090c`

## Trace severity

| Seed | At least one Provider in range | All Providers out of range | At least two in range |
|---:|---:|---:|---:|
| 40 | 31.36% | 68.64% | 0.98% |
| 41 | 16.32% | 83.68% | 7.17% |
| 42 | 26.72% | 73.28% | 7.17% |

## Results

The aggregate contains 9/9 complete cells and 2,700 logical requests per
system.

| System | Success | Success rate | Attempts/request | Failovers/request | Provider executions/request | Mean successful latency |
|---|---:|---:|---:|---:|---:|---:|
| NDNSF | 71/900 | 7.889% | N/A | 0 | 0.082 | 1,520.2 ms |
| gRPC-SEQ-4 | 136/900 | 15.111% | 3.757 | 2.757 | 3.757 | 216.5 ms |
| NSC-4 | 214/900 | 23.778% | 3.616 | 2.616 | 3.616 | 1,414.5 ms |

Paired success-rate differences (NDNSF minus baseline, three seeds) were:

- gRPC-SEQ-4: mean **−7.222 percentage points**, per-seed range −22.000 to
  +6.000 pp.
- NSC-4: mean **−15.889 percentage points**, per-seed range −23.333 to
  −6.333 pp.

## Interpretation

The premise is only partly observed: lower coverage raised sequential retry
cost, but it did not create a relative NDNSF advantage. At 35 m, most trace
epochs had **no reachable Provider at all**; NDNSF has no cross-Provider retry,
so its lower control cost did not translate into terminal success. The result
is therefore `NO_DEMONSTRATED_ADVANTAGE` for this random-waypoint stress
condition.

This negative result is useful: a lower coverage radius alone is not a valid
NDNSF-favorable stress design. A positive multi-Provider mechanism claim would
need a separately labelled condition with at least one reachable and responsive
Provider during the request (for example, a controlled handoff schedule), and
must still pass the existing paired success gate.

## Verification

- Pilot aggregate: 9/9 cells complete; no failed cell was rerun.
- `python3 -m py_compile` passed for the modified pilot and harness.
- Mobility profile tests: 5 passed, one existing Matplotlib deprecation warning.
- Context Mode project and active-Spec health checks passed; CodeGraph source
  exploration confirmed the provider-scope and metric paths.
