# Optimized NDNSF Memphis-UCLA Admission Configuration

Date: 2026-05-22

This note records the optimized configuration used after fixing the current
AdaptiveAdmission default path regression.

## Configuration

- Topology: `Experiments/Topology/testbed(loss=0%).conf`
- Path: Memphis user/controller -> UCLA provider
- Providers: 1
- Strategy: FirstResponding
- Provider processing delay: 5 ms
- Workload: open-loop
- Per-rate duration used for quick validation: 30 s
- Request timeout: 5000 ms
- Drain time: 10 s
- Provider certificates served: enabled
- Performance mode: enabled
- Handler threads: `NDNSF_HANDLER_THREADS=2`
- ACK threads: `NDNSF_ACK_THREADS=2`
- AdaptiveAdmission: enabled with default parameters
- Recommended-rate hard pacing: disabled by default; opt in with
  `--enable-adaptive-recommended-rate`

## Admission-Control Behavior Fixed

- The benchmark sender no longer treats the framework recommended rate as
  mandatory pacing unless explicitly requested.
- The benchmark sender does not burst all accumulated open-loop credits after an
  admission pause; it admits at most one queued tick per dispatch.
- `ServiceUser` bounds its admission queue relative to the active admission
  window, preventing a small window from hiding a large queue and producing
  multi-second latency.
- `ServiceUser` allows a short startup probe before the latency baseline is
  established, avoiding cold-start self-throttling.
- Soft queue warnings only trigger multiplicative decrease when latency is not
  stable; stable queue pressure alone is no longer treated as congestion.

## Quick Validation Result

Results directory:
`results/fixed_ndnsf_admission_startup_ecnfix_10_50_70_30s_20260522_142309`

| Offered RPS | Actual RPS | Success | Avg latency | P50 latency | P95 latency | Timeout |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10 | 10.00 | 100% | 182.37 ms | 174.69 ms | 177.19 ms | 0 |
| 50 | 50.00 | 100% | 182.82 ms | 173.76 ms | 184.79 ms | 0 |
| 70 | 70.00 | 100% | 184.32 ms | 175.84 ms | 179.42 ms | 0 |

For comparison, the rebuilt no-admission sanity run returned to the same latency
class:
`results/fixed_ndnsf_no_admission_10_30_50_70_30s_20260522_140530`.
