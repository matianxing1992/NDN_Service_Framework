# Spec 171 Provider-switch opportunity analysis

- Verdict: `CONDITIONAL_SWITCHING_COST_REDUCTION`
- `SWITCH_REQUIRED` requests: 1747
- NDNSF / gRPC successes in those requests: 1745 / 1745
- NDNSF metric: Request timestamp to selected Provider receiving Selection.
- gRPC metric: time spent in failed sequential attempts before success.
- NDNSF end-to-end per-request latency is unavailable in the frozen logs; Response publication is retained only as a labelled proxy.
- Unconditional success and user-latency results remain primary.

## Paired seed result

Mean seed-p95 reduction: 512.93 ms (95% paired bootstrap CI 140.50 to 886.27 ms; n=10 seeds).

The result supports only a conditional tail-cost claim. It does not show an unconditional success-rate advantage, and it does not claim that gRPC cannot use an external resolver or health system.
