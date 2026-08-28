# Spec 171 Provider-transition result

- Verdict: `PROVIDER_DISCOVERY_EVIDENCE_PASSED`
- Completed independent process replays: 3
- Scope: client-side discovery of an already authorized and routed Provider.
- gRPC/NSC can obtain equivalent behavior through an external resolver or configuration update.

## Replay 1 steady post-retirement window

| Cell | Configured Providers | Success | Provider-D successes | Mean successful latency | p95 successful latency |
|---|---:|---:|---:|---:|---:|
| ndnsf | 0 | 119/119 | 119 | 82.12 ms | 107.26 ms |
| grpc-static-3 | 3 | 0/119 | 0 | -- | -- |
| grpc-preregistered-4 | 4 | 119/119 | 119 | 609.87 ms | 3031.47 ms |
| nsc-static-3 | 3 | 0/119 | 0 | -- | -- |
| nsc-preregistered-4 | 4 | 119/119 | 119 | 1517.19 ms | 3047.00 ms |
