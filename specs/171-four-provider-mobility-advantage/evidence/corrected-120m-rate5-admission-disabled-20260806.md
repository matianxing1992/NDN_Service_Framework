# Corrected 120 m NDNSF Rerun (2026-08-06)

## Purpose

The earlier 120 m NDNSF screen was run with the mobility user sending 5 RPS,
but `make_perf_args()` did not pass `rate_rps` to the SVS environment.  The
user therefore logged `expectedRps=0` and used the fallback publication-fetch
window.  This rerun fixes that configuration and verifies that the mobility
user has admission control disabled.

## Configuration

- Four Providers, one physical AP, `four-provider-single-ap`, 15 m/s.
- Range 120 m, random-waypoint, seed 40.
- `block_network=true`; 5 RPS; 60-second measured window; 300 requests.
- NDNSF ACK timeout and request deadline: 1 s and 5 s respectively.
- Canonical replay trace from the original 110/120 screen; trace hash is
  retained below.
- NDNSF `adaptiveAdmission=disabled`; no admission rejects or delays.
- SVS startup markers report `expectedRps=5`, `window=32` for user and
  Providers.

## Result

| System | Success | Success rate | Mean successful latency | P50 | P95 | Provider executions/request |
|---|---:|---:|---:|---:|---:|---:|
| NDNSF corrected | 282/300 | 94.000% | 87.36 ms | 52.84 ms | 121.98 ms | 0.940 |
| gRPC-SEQ-4 prior paired screen | 293/300 | 97.667% | 511.4 ms | N/A | N/A | 1.797 |
| NSC-4 prior paired screen | 293/300 | 97.667% | 745.7 ms | N/A | N/A | 1.783 |

NDNSF is **−3.667 percentage points** below both sequential baselines in
logical success, while using fewer Provider executions and substantially lower
successful-response latency.  This is a latency/control-cost trade-off, not a
mobility reliability advantage.

## Decision

Do **not** expand this 120 m condition to three seeds: it does not satisfy the
registered positive success-rate gate (SC-005a).  The corrected result replaces
the old `expectedRps=0` NDNSF number for interpretation; the old screen remains
historical diagnostic evidence only.

## Verification

- Python focused tests: `16 passed, 1 warning`.
- User readiness marker: `adaptiveAdmission=disabled`.
- User summary: `sent=300 accepted=300 completed=300`.
- User/Provider SVS marker: `expectedRps=5 window=32`.
- Canonical trace: `/tmp/ndnsf-coverage-screen-110-120-15ms-20260806-r2/seed-40/range-120-speed-15p0/trace.csv`.
- Corrected run: `/tmp/ndnsf-coverage-rerun-120-15ms-seed40-admission-off-replay`.
- `summary.json` SHA-256: `61f32e4167a5131829bd494e461f5cd80c0c83f10bc5272025019b3476cfdcd3`.
- `mobility_trace.csv` SHA-256: `ec9d2c2d051e934a4f349366ec4a3f327c1b99cffb839279f0e03dc49ba27221`.
- `runtime-commands.json` SHA-256: `8b0b8d5efc545fed78c6677e3e620c7f8ce22c3e025ad3e5583838925626a7b3`.
