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
- Handler threads: 2 by default, override with `NDNSF_HANDLER_THREADS`
- ACK threads: 2 by default, override with `NDNSF_ACK_THREADS`
- AdaptiveAdmission: enabled by default, disable with
  `--disable-adaptive-admission-control` in `App_User`
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

These values are now the implementation defaults unless explicitly overridden.

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

## Paper Text

Concretely, NDNSF uses a window-based controller similar in spirit to
congestion control. The user maintains an admission window that bounds the
number of outstanding service requests. In the implementation used in our
experiments, AdaptiveAdmission is enabled by default. The window starts at 16
requests, with a minimum of 1 and a maximum of 512 outstanding requests, and the
controller is updated every 500 ms using recent completion, timeout,
queue-depth, and latency observations. When requests complete successfully and
the observed delay remains stable, the window is increased additively by 4
requests per control interval; during the initial probing phase, it may grow by
up to 16 requests per interval depending on recent successful completions. The
framework also maintains a bounded admission queue with a default soft threshold
of 32 requests and a hard threshold of 128 requests, while the effective queue
bound is further constrained by the current admission window to avoid hiding
excessive delay behind a small window. When latency shows persistent growth,
the window is multiplicatively reduced by a factor of 0.85. Under severe
congestion signals, such as timeouts or near-full queues, the window is reduced
more aggressively using a factor of 0.5. New requests may be delayed once the
soft threshold is reached and rejected once the hard threshold is reached,
preventing unbounded queue buildup and latency collapse.
