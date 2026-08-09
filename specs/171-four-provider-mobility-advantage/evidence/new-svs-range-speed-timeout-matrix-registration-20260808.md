# New-SVS Range/Speed/Timeout Matrix Registration

## Question

The three-seed `50 m / 15 m/s` result is mixed at the seed level.  This
registration tests whether that ordering is caused by trace-to-trace variation
and whether the NDNSF/gRPC/NSC comparison changes with coverage, speed, and
per-attempt timeout.  It is a descriptive follow-up to the registered
one-AP matrix, not a license to select a favorable condition after observing
the results.

## Frozen matrix

| Factor | Values |
|---|---|
| AP layout | one physical AP at `(200,200)` in a `400 m x 400 m` field |
| Providers | `ucla`, `wustl`, `uiuc`, `arizona` |
| Coverage radius | `50 m`, `100 m`, `150 m` |
| Mobility speed | `2 m/s`, `5 m/s`, `10 m/s` |
| Timeout condition | `500 ms` or `1,000 ms` attempt timeout and NDNSF ACK timeout |
| Global deadline | `5,000 ms` for both timeout conditions |
| Systems | NDNSF `FirstResponding`, `gRPC-SEQ-4`, `NSC-4` |
| Network gate | `block_network=true`; no client trace/oracle |
| Policy | admission control disabled; proactive health routing disabled |
| Workload | `5 RPS`, `60 s`, `300` logical requests per system cell |
| Trace | `random-waypoint`, generated once per `(range, speed, seed)` and replayed for both timeout conditions |

The timeout factor is deliberately not a global deadline.  At `500 ms`, gRPC
and NSC may advance through sequential attempts more quickly; at `1,000 ms`,
each attempt has more time, but both still share the same 5 s logical deadline.
NDNSF remains `FirstResponding`; changing `ackTimeoutMs` changes the ACK wait
bound only when no successful ACK arrives and does not turn the current path
into response-level retry/reselection.

## Seed policy and stages

The first registered screen uses independent new-SVS seeds `60,61,62`.  These
seeds are not reused from the earlier 40--59 campaigns.  The screen contains
`3 ranges x 3 speeds x 2 timeout conditions x 3 seeds x 3 systems = 162`
system cells, each with a 60-second measured window.  It is a bounded
descriptive matrix, not 162 independent mobility replicates: the seed is the
inference unit and the 300 requests within a trace are temporally correlated.

Before the formal screen, run an under-10-second smoke for one representative
cell at each timeout to verify startup, four-Provider failover, and manifest
provenance.  Smoke output must not be pooled with formal results.

If the screen shows a candidate condition, expand the pre-registered
intermediate condition `100 m / 5 m/s` to independent seeds `60--69` at both
timeouts.  This ten-seed confirmation is the only stage eligible for a
paper-level positive claim.  The full 18-condition screen remains descriptive
even if one condition looks favorable.

## Pairing and rejection rules

1. For each `(range, speed, seed)`, the trace hash must match between the
   500-ms and 1,000-ms roots and across all three systems.
2. All cells must use the same 4-second traffic barrier, 60-second measured
   window, source/build hashes, admission setting, and health policy.
3. A missing terminal summary, request count other than 300, offset mismatch,
   trace mismatch, or runtime provenance mismatch rejects that cell; do not
   silently substitute the old seed-40 300-second matrix.
4. The NDNSF manifest must identify the new-SVS installation and the
   `libndn-service-framework.so` hash.  The two timeout roots must otherwise
   differ only in the registered timeout values.

## Reported metrics

Report per `(range, speed, timeout)` and seed:

- logical success rate and deadline failures;
- mean, p50, and p95 successful-response latency;
- attempts/provider executions and failovers;
- measurement-window `at_least_one`, `at_least_two`, and all-unreachable
  fractions;
- failure-stage counts where lifecycle traces expose `ACK_MATCHED`,
  `PROVIDER_SELECTED`, and `RESPONSE_OBSERVED`.

Use paired NDNSF-minus-baseline differences by seed.  Do not pool all requests
as independent samples and do not report a universal NDNSF advantage.  If the
sign changes across seeds, report seed sensitivity and the coverage/timeout
boundary explicitly.

## Execution roots

Use separate fresh roots so timeout conditions cannot overwrite one another:

```text
results/ndnsf-new-svs-matrix-500ms-seeds60-62-20260808/
results/ndnsf-new-svs-matrix-1000ms-seeds60-62-20260808/
```

Both roots must be launched with the explicit new-SVS runtime variables and
MiniNDN-WiFi/root execution.  No automatic rerun is allowed; failed cells stay
in their root and are excluded from pooled summaries with the reason retained.

## Claim boundary

The 162-cell screen can establish where the result is stable, mixed, or
timeout-sensitive.  It cannot by itself prove a general mobility advantage.
Only the ten-seed `100 m / 5 m/s` confirmation may be evaluated against the
existing paired confidence-bound gate; otherwise retain
`DESCRIPTIVE_RANGE_SPEED_TIMEOUT_MATRIX_ONLY` or
`NO_DEMONSTRATED_ADVANTAGE`.
