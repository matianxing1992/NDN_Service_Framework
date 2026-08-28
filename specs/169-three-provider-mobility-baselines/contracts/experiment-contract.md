# Experiment Contract

## Formal Schedule

```text
systems: grpc,nsc
ranges_m: 100,150,200
rate_rps: 5
duration_s: 60
processing_delay_ms: 5
global_deadline_ms: 5000
attempt_timeout_ms: 200
health_interval_ms: 200
startup_offset_ms: 2000
seed: 20
repetitions: 1
automatic_retry: false
```

Exactly six cells are admitted. `ndnsf` is invalid in a formal Spec 169
schedule.

## gRPC Terminal Summary

The client emits one line beginning `GRPC_FAILOVER_RATE` with at least:

```text
sent success failures attempts failovers health_checks health_success
p50_ms p95_ms p99_ms actual_success_rps
```

## NSC Terminal Summary

The client emits one line beginning `NSC_FAILOVER_SUMMARY` with at least:

```text
count success terminal_failures attempts attempt_timeouts nacks failovers
late_callbacks p50_ms p95_ms p99_ms
```

## Cell Artifacts

- `cell-manifest.json`
- client/server/producer logs
- `mobility_trace.csv`
- structured terminal metrics
- terminal status even on failure

## Campaign Artifacts

- `campaign-summary.json`
- `campaign-runs.csv`
- `campaign-cells.csv`

Validation rejects missing, duplicate, unexpected or retried cells.
