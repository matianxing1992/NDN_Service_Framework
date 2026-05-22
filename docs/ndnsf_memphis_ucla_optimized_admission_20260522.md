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
- `ServiceUser` derives its admission queue limits from the current admission
  window, preventing a small window from hiding a large queue and producing
  multi-second latency.
- `ServiceUser` allows a short startup inflight probe before the latency
  baseline is established, but queue limits are still derived from the real
  admission window rather than the temporary probe limit.
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
number of outstanding service requests. In our implementation,
AdaptiveAdmission is enabled by default and starts with an initial window of 16
requests, with a minimum window of 1 and a maximum window of 512. The
controller is updated every 500 ms using recent completion, timeout,
queue-depth, and latency observations. When requests complete successfully and
the observed delay remains stable, the window is increased using an adaptive
additive step $\mathrm{clamp}(0.25W, 4, 16)$ per control interval, where $W$ is
the current admission window. This allows the controller to ramp up faster when
additional capacity is available, while bounding the probing rate to avoid
sudden queue buildup. When latency shows persistent growth, the window is
multiplicatively reduced by a factor of 0.85; under severe congestion signals,
such as timeouts or near-full queues, it is reduced more aggressively by a
factor of 0.5.

To avoid fixed queue thresholds that are insensitive to runtime capacity,
NDNSF derives its admission-queue limits from the current admission window. The
hard queue limit is computed as $\mathrm{clamp}(16 + 2W, 32, 256)$, where $W$
is the current admission window. The soft queue limit is set to 50\% of the
hard limit. Reaching the soft limit triggers early congestion feedback and
request pacing, while reaching the hard limit causes new requests to be
rejected or deferred to protect the service pipeline. During the first few
control intervals, the implementation may temporarily allow a small startup
inflight probe to avoid cold-start self-throttling, but queue limits continue
to use the real admission window. This design allows the buffering allowance to
grow only when the controller has already inferred higher service capacity,
while keeping queue buildup and tail latency bounded under overload.
